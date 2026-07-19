//! Panel-driven mass measurement: start a timed window on every automated
//! channel, then compute and apply each trim when the window closes.
//!
//! All functions here run on GUI threads; the audio thread only reads the
//! `measuring`/`meas_epoch` flags and accumulates `measured_peak`.

use crate::dsp::{compute_trim_db, gain_to_db};
use crate::registry::{self, GLOBALS};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Mutex;
use std::time::{Duration, Instant};

struct Run {
    started: Instant,
    deadline: Instant,
}

static RUN: Mutex<Option<Run>> = Mutex::new(None);

pub fn is_running() -> bool {
    RUN.lock().unwrap().is_some()
}

/// Progress of the current run in `0.0..=1.0`, or `None` when idle.
pub fn progress() -> Option<f32> {
    let run = RUN.lock().unwrap();
    run.as_ref().map(|r| {
        let total = (r.deadline - r.started).as_secs_f32();
        let elapsed = r.started.elapsed().as_secs_f32();
        (elapsed / total).clamp(0.0, 1.0)
    })
}

pub fn start(duration_s: f32) {
    let mut run = RUN.lock().unwrap();
    if run.is_some() {
        return;
    }
    for ch in registry::channels() {
        if !ch.automation_on.load(Relaxed) {
            continue;
        }
        ch.meas_epoch.fetch_add(1, Relaxed);
        ch.measured_peak.store(0.0, Relaxed);
        ch.no_signal.store(false, Relaxed);
        ch.measuring.store(true, Relaxed);
    }
    let now = Instant::now();
    *run = Some(Run {
        started: now,
        deadline: now + Duration::from_secs_f32(duration_s.max(0.5)),
    });
}

pub fn cancel() {
    let mut run = RUN.lock().unwrap();
    if run.take().is_some() {
        for ch in registry::channels() {
            ch.measuring.store(false, Relaxed);
        }
    }
}

/// Call every GUI frame while measuring. When the window has elapsed, stops
/// the run and applies trims; channels under the gate are flagged `no_signal`
/// and left untouched.
pub fn poll() {
    let mut run = RUN.lock().unwrap();
    let Some(r) = run.as_ref() else { return };
    if Instant::now() < r.deadline {
        return;
    }
    *run = None;
    drop(run);

    let max_trim_db = GLOBALS.max_trim_db.load(Relaxed);
    for ch in registry::channels() {
        if !ch.measuring.swap(false, Relaxed) {
            continue;
        }
        let peak_db = gain_to_db(ch.measured_peak.load(Relaxed));
        match compute_trim_db(peak_db, ch.target_db.load(Relaxed), max_trim_db) {
            Some(trim) => ch.trim_db.store(trim, Relaxed),
            None => ch.no_signal.store(true, Relaxed),
        }
    }
}
