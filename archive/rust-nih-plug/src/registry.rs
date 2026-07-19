//! Global in-process registry shared by every plugin instance.
//!
//! The audio thread only ever touches the atomics inside [`ChannelShared`];
//! the registry mutex is locked from GUI/main threads only.

use atomic_float::AtomicF32;
use serde::{Deserialize, Serialize};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering::Relaxed};
use std::sync::{Arc, LazyLock, Mutex};

/// Peaks below this level mean "no signal": measurement skips the channel and
/// the rider freezes.
pub const GATE_DB: f32 = -60.0;
pub const DEFAULT_TARGET_DB: f32 = -18.0;
pub const DEFAULT_MAX_TRIM_DB: f32 = 24.0;
pub const DEFAULT_MEAS_DURATION_S: f32 = 10.0;

/// Lock-free state for one plugin instance.
pub struct ChannelShared {
    pub id: u64,
    /// GUI threads only.
    pub name: Mutex<String>,
    pub target_db: AtomicF32,
    pub trim_db: AtomicF32,
    pub automation_on: AtomicBool,
    pub rider_on: AtomicBool,
    pub panel_mode: AtomicBool,
    /// Pre-trim peak (linear) with decay, for metering.
    pub peak_pre_trim: AtomicF32,
    /// Max pre-trim peak (linear) accumulated during the current measurement.
    pub measured_peak: AtomicF32,
    pub measuring: AtomicBool,
    /// Bumped by the panel when a new measurement starts so the audio thread
    /// resets its local accumulator without racing the panel's reset.
    pub meas_epoch: AtomicU32,
    /// Result of the last measurement: channel had no usable signal.
    pub no_signal: AtomicBool,
    pub alive: AtomicBool,
}

impl ChannelShared {
    fn new(id: u64) -> Self {
        Self {
            id,
            name: Mutex::new(String::new()),
            target_db: AtomicF32::new(DEFAULT_TARGET_DB),
            trim_db: AtomicF32::new(0.0),
            automation_on: AtomicBool::new(true),
            rider_on: AtomicBool::new(false),
            panel_mode: AtomicBool::new(false),
            peak_pre_trim: AtomicF32::new(0.0),
            measured_peak: AtomicF32::new(0.0),
            measuring: AtomicBool::new(false),
            meas_epoch: AtomicU32::new(0),
            no_signal: AtomicBool::new(false),
            alive: AtomicBool::new(true),
        }
    }

    pub fn display_name(&self) -> String {
        let name = self.name.lock().unwrap();
        if name.trim().is_empty() {
            format!("Canal {}", self.id)
        } else {
            name.clone()
        }
    }

    pub fn snapshot(&self) -> Settings {
        Settings {
            name: self.name.lock().unwrap().clone(),
            target_db: self.target_db.load(Relaxed),
            trim_db: self.trim_db.load(Relaxed),
            automation_on: self.automation_on.load(Relaxed),
            rider_on: self.rider_on.load(Relaxed),
            panel_mode: self.panel_mode.load(Relaxed),
            max_trim_db: GLOBALS.max_trim_db.load(Relaxed),
            meas_duration_s: GLOBALS.meas_duration_s.load(Relaxed),
        }
    }

    pub fn apply_settings(&self, s: &Settings) {
        *self.name.lock().unwrap() = s.name.clone();
        self.target_db.store(s.target_db, Relaxed);
        self.trim_db.store(s.trim_db, Relaxed);
        self.automation_on.store(s.automation_on, Relaxed);
        self.rider_on.store(s.rider_on, Relaxed);
        self.panel_mode.store(s.panel_mode, Relaxed);
        // Global panel settings are saved with every instance, but only the
        // panel instance restores them, so a channel's stale copy never wins.
        if s.panel_mode {
            GLOBALS.max_trim_db.store(s.max_trim_db, Relaxed);
            GLOBALS.meas_duration_s.store(s.meas_duration_s, Relaxed);
        }
    }
}

/// Session-wide settings owned by the panel.
pub struct Globals {
    pub max_trim_db: AtomicF32,
    pub meas_duration_s: AtomicF32,
}

pub static GLOBALS: Globals = Globals {
    max_trim_db: AtomicF32::new(DEFAULT_MAX_TRIM_DB),
    meas_duration_s: AtomicF32::new(DEFAULT_MEAS_DURATION_S),
};

static REGISTRY: LazyLock<Mutex<Vec<Arc<ChannelShared>>>> =
    LazyLock::new(|| Mutex::new(Vec::new()));
static NEXT_ID: AtomicU64 = AtomicU64::new(1);

pub fn register() -> Arc<ChannelShared> {
    let shared = Arc::new(ChannelShared::new(NEXT_ID.fetch_add(1, Relaxed)));
    REGISTRY.lock().unwrap().push(shared.clone());
    shared
}

pub fn unregister(shared: &Arc<ChannelShared>) {
    shared.alive.store(false, Relaxed);
    REGISTRY
        .lock()
        .unwrap()
        .retain(|ch| !Arc::ptr_eq(ch, shared));
}

/// Alive, non-panel channels in registration order.
pub fn channels() -> Vec<Arc<ChannelShared>> {
    REGISTRY
        .lock()
        .unwrap()
        .iter()
        .filter(|ch| ch.alive.load(Relaxed) && !ch.panel_mode.load(Relaxed))
        .cloned()
        .collect()
}

/// Serialized per-instance state (plus the panel's global settings).
#[derive(Serialize, Deserialize, Clone)]
#[serde(default)]
pub struct Settings {
    pub name: String,
    pub target_db: f32,
    pub trim_db: f32,
    pub automation_on: bool,
    pub rider_on: bool,
    pub panel_mode: bool,
    pub max_trim_db: f32,
    pub meas_duration_s: f32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            name: String::new(),
            target_db: DEFAULT_TARGET_DB,
            trim_db: 0.0,
            automation_on: true,
            rider_on: false,
            panel_mode: false,
            max_trim_db: DEFAULT_MAX_TRIM_DB,
            meas_duration_s: DEFAULT_MEAS_DURATION_S,
        }
    }
}

/// Bridges the shared atomics into nih-plug's `#[persist]` state system: the
/// atomics are the single source of truth, (de)serialized on demand.
pub struct SharedSettings(pub Arc<ChannelShared>);

impl<'a> nih_plug::params::persist::PersistentField<'a, Settings> for SharedSettings {
    fn set(&self, new_value: Settings) {
        self.0.apply_settings(&new_value);
    }

    fn map<F, R>(&self, f: F) -> R
    where
        F: Fn(&Settings) -> R,
    {
        f(&self.0.snapshot())
    }
}
