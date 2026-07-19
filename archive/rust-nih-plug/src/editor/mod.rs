mod channel;
mod panel;

use crate::registry::ChannelShared;
use crate::AutoTrimParams;
use nih_plug::prelude::Editor;
use nih_plug_egui::{create_egui_editor, egui};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;
use std::time::Duration;

/// Per-editor scratch state (text buffers etc.).
#[derive(Default)]
pub struct UiState {
    pub name_buf: String,
    pub name_synced: bool,
}

pub fn create(
    params: Arc<AutoTrimParams>,
    shared: Arc<ChannelShared>,
) -> Option<Box<dyn Editor>> {
    create_egui_editor(
        params.editor_state.clone(),
        UiState::default(),
        |_ctx, _state| {},
        move |ctx, _setter, state| {
            // Keep meters moving even without host-driven repaints.
            ctx.request_repaint_after(Duration::from_millis(33));

            egui::CentralPanel::default().show(ctx, |ui| {
                if shared.panel_mode.load(Relaxed) {
                    panel::ui(ui, &shared);
                } else {
                    channel::ui(ui, &shared, state);
                }
            });
        },
    )
}

/// Shared meter widget: linear peak drawn on a -60..0 dBFS scale.
pub fn peak_meter(ui: &mut egui::Ui, peak_lin: f32, width: f32) {
    let db = crate::dsp::gain_to_db(peak_lin);
    let frac = ((db + 60.0) / 60.0).clamp(0.0, 1.0);
    let text = if db <= -90.0 {
        "-inf dB".to_string()
    } else {
        format!("{db:+.1} dB")
    };
    ui.add_sized(
        [width, 16.0],
        egui::ProgressBar::new(frac).text(egui::RichText::new(text).size(10.0)),
    );
}

pub fn format_db(db: f32) -> String {
    format!("{db:+.1} dB")
}
