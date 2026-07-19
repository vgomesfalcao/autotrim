mod dsp;
mod editor;
mod measurement;
mod registry;

use nih_plug::prelude::*;
use nih_plug_egui::EguiState;
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;

use registry::{ChannelShared, SharedSettings, GLOBALS};

pub struct AutoTrim {
    params: Arc<AutoTrimParams>,
    shared: Arc<ChannelShared>,
    sample_rate: f32,
    /// Smoothed applied gain (linear), one-pole toward the trim value.
    gain_lin: f32,
    gain_coef: f32,
    /// Peak envelope (linear) driving the rider.
    env_lin: f32,
    env_attack: f32,
    env_release: f32,
    /// Local measurement accumulator; reset when `meas_epoch` changes so the
    /// panel's reset can never race the audio thread.
    meas_peak: f32,
    meas_epoch: u32,
    meter_decay: f32,
}

#[derive(Params)]
pub struct AutoTrimParams {
    #[persist = "editor-state"]
    pub(crate) editor_state: Arc<EguiState>,
    #[persist = "settings"]
    pub(crate) settings: SharedSettings,
}

impl Default for AutoTrim {
    fn default() -> Self {
        let shared = registry::register();
        Self {
            params: Arc::new(AutoTrimParams {
                editor_state: EguiState::from_size(560, 420),
                settings: SharedSettings(shared.clone()),
            }),
            shared,
            sample_rate: 48000.0,
            gain_lin: 1.0,
            gain_coef: 0.001,
            env_lin: 0.0,
            env_attack: 0.5,
            env_release: 0.001,
            meas_peak: 0.0,
            meas_epoch: 0,
            meter_decay: 1.0,
        }
    }
}

impl Drop for AutoTrim {
    fn drop(&mut self) {
        registry::unregister(&self.shared);
    }
}

impl Plugin for AutoTrim {
    const NAME: &'static str = "AutoTrim";
    const VENDOR: &'static str = "vgomesfalcao";
    const URL: &'static str = "https://github.com/vgomesfalcao";
    const EMAIL: &'static str = "vgomesfalcao@gmail.com";
    const VERSION: &'static str = env!("CARGO_PKG_VERSION");

    const AUDIO_IO_LAYOUTS: &'static [AudioIOLayout] = &[
        AudioIOLayout {
            main_input_channels: NonZeroU32::new(2),
            main_output_channels: NonZeroU32::new(2),
            ..AudioIOLayout::const_default()
        },
        AudioIOLayout {
            main_input_channels: NonZeroU32::new(1),
            main_output_channels: NonZeroU32::new(1),
            ..AudioIOLayout::const_default()
        },
    ];

    const MIDI_INPUT: MidiConfig = MidiConfig::None;
    const SAMPLE_ACCURATE_AUTOMATION: bool = false;

    type SysExMessage = ();
    type BackgroundTask = ();

    fn params(&self) -> Arc<dyn Params> {
        self.params.clone()
    }

    fn editor(&mut self, _async_executor: AsyncExecutor<Self>) -> Option<Box<dyn Editor>> {
        editor::create(self.params.clone(), self.shared.clone())
    }

    fn initialize(
        &mut self,
        _audio_io_layout: &AudioIOLayout,
        buffer_config: &BufferConfig,
        _context: &mut impl InitContext<Self>,
    ) -> bool {
        self.sample_rate = buffer_config.sample_rate;
        self.gain_coef = dsp::onepole_coef(dsp::GAIN_SMOOTH_S, self.sample_rate);
        self.env_attack = dsp::onepole_coef(dsp::ENV_ATTACK_S, self.sample_rate);
        self.env_release = dsp::onepole_coef(dsp::ENV_RELEASE_S, self.sample_rate);
        true
    }

    fn reset(&mut self) {
        self.gain_lin = dsp::db_to_gain(self.shared.trim_db.load(Relaxed));
        self.env_lin = 0.0;
    }

    fn process(
        &mut self,
        buffer: &mut Buffer,
        _aux: &mut AuxiliaryBuffers,
        _context: &mut impl ProcessContext<Self>,
    ) -> ProcessStatus {
        let shared = &self.shared;
        let num_samples = buffer.samples();
        if num_samples == 0 {
            return ProcessStatus::Normal;
        }

        let max_trim_db = GLOBALS.max_trim_db.load(Relaxed);
        let automation_on = shared.automation_on.load(Relaxed);
        let rider_on = shared.rider_on.load(Relaxed) && automation_on;
        let mut trim_db = shared
            .trim_db
            .load(Relaxed)
            .clamp(-max_trim_db, max_trim_db);
        // Bypassed automation means unity gain, but keep metering alive.
        let target_gain = if automation_on {
            dsp::db_to_gain(trim_db)
        } else {
            1.0
        };

        let mut block_peak = 0.0f32;
        for mut frame in buffer.iter_samples() {
            let mut frame_peak = 0.0f32;
            for sample in frame.iter_mut() {
                frame_peak = frame_peak.max(sample.abs());
            }
            block_peak = block_peak.max(frame_peak);
            self.env_lin =
                dsp::envelope_step(self.env_lin, frame_peak, self.env_attack, self.env_release);

            self.gain_lin += self.gain_coef * (target_gain - self.gain_lin);
            for sample in frame.iter_mut() {
                *sample *= self.gain_lin;
            }
        }

        // Metering: max-hold with decay.
        let dt = num_samples as f32 / self.sample_rate;
        let decayed = shared.peak_pre_trim.load(Relaxed) * dsp::db_to_gain(-dsp::METER_DECAY_DB_PER_S * dt);
        shared
            .peak_pre_trim
            .store(decayed.max(block_peak), Relaxed);

        // Measurement window: single writer (this thread); the epoch bump
        // tells us the panel started a new run.
        let epoch = shared.meas_epoch.load(Relaxed);
        if epoch != self.meas_epoch {
            self.meas_epoch = epoch;
            self.meas_peak = 0.0;
        }
        if shared.measuring.load(Relaxed) {
            self.meas_peak = self.meas_peak.max(block_peak);
            shared.measured_peak.store(self.meas_peak, Relaxed);
        }

        // Continuous rider: block-rate trim adjustment with slew limiting.
        if rider_on && !shared.measuring.load(Relaxed) {
            let env_db = dsp::gain_to_db(self.env_lin);
            let new_trim = dsp::rider_step(
                trim_db,
                env_db,
                shared.target_db.load(Relaxed),
                max_trim_db,
                dt,
            );
            if new_trim != trim_db {
                trim_db = new_trim;
                shared.trim_db.store(trim_db, Relaxed);
            }
        }

        ProcessStatus::Normal
    }
}

impl ClapPlugin for AutoTrim {
    const CLAP_ID: &'static str = "com.vgomesfalcao.autotrim";
    const CLAP_DESCRIPTION: Option<&'static str> =
        Some("Auto gain trim with a central control panel");
    const CLAP_MANUAL_URL: Option<&'static str> = None;
    const CLAP_SUPPORT_URL: Option<&'static str> = None;
    const CLAP_FEATURES: &'static [ClapFeature] = &[
        ClapFeature::AudioEffect,
        ClapFeature::Utility,
        ClapFeature::Mixing,
        ClapFeature::Stereo,
        ClapFeature::Mono,
    ];
}

impl Vst3Plugin for AutoTrim {
    const VST3_CLASS_ID: [u8; 16] = *b"AutoTrimVGF00001";
    const VST3_SUBCATEGORIES: &'static [Vst3SubCategory] =
        &[Vst3SubCategory::Fx, Vst3SubCategory::Tools];
}

nih_export_clap!(AutoTrim);
nih_export_vst3!(AutoTrim);
