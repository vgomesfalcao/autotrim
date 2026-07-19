//! Gain math, metering and the continuous rider. Pure functions where
//! possible so they can be unit-tested without a host.

use crate::registry::GATE_DB;

/// Rider slew rates, in dB per second.
pub const RIDER_UP_DB_PER_S: f32 = 3.0;
pub const RIDER_DOWN_DB_PER_S: f32 = 12.0;
/// Peak envelope time constants for the rider.
pub const ENV_ATTACK_S: f32 = 0.005;
pub const ENV_RELEASE_S: f32 = 0.300;
/// Smoothing time for the applied trim gain (click-free changes).
pub const GAIN_SMOOTH_S: f32 = 0.050;
/// Meter decay: how fast the displayed peak falls, in dB per second.
pub const METER_DECAY_DB_PER_S: f32 = 20.0;

pub fn db_to_gain(db: f32) -> f32 {
    10f32.powf(db / 20.0)
}

pub fn gain_to_db(gain: f32) -> f32 {
    if gain <= 1e-10 {
        -200.0
    } else {
        20.0 * gain.log10()
    }
}

/// One-pole coefficient for a given time constant at `sample_rate`.
pub fn onepole_coef(time_s: f32, sample_rate: f32) -> f32 {
    if time_s <= 0.0 {
        1.0
    } else {
        1.0 - (-1.0 / (time_s * sample_rate)).exp()
    }
}

/// Trim needed to bring `measured_peak_db` to `target_db`, clamped to the
/// panel's limit. Returns `None` when the peak is below the no-signal gate.
pub fn compute_trim_db(measured_peak_db: f32, target_db: f32, max_trim_db: f32) -> Option<f32> {
    if measured_peak_db < GATE_DB {
        return None;
    }
    Some((target_db - measured_peak_db).clamp(-max_trim_db, max_trim_db))
}

/// One rider step: move `trim_db` toward closing the error between the peak
/// envelope and the target, limited by the up/down slew rates over `dt`
/// seconds. Frozen while the envelope is under the gate.
pub fn rider_step(
    trim_db: f32,
    env_db: f32,
    target_db: f32,
    max_trim_db: f32,
    dt: f32,
) -> f32 {
    if env_db < GATE_DB {
        return trim_db;
    }
    let error = target_db - env_db;
    let step = error.clamp(-RIDER_DOWN_DB_PER_S * dt, RIDER_UP_DB_PER_S * dt);
    (trim_db + step).clamp(-max_trim_db, max_trim_db)
}

/// Peak envelope follower over one sample.
pub fn envelope_step(env: f32, sample_abs: f32, attack_coef: f32, release_coef: f32) -> f32 {
    let coef = if sample_abs > env {
        attack_coef
    } else {
        release_coef
    };
    env + coef * (sample_abs - env)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn approx(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-3
    }

    #[test]
    fn db_gain_roundtrip() {
        for db in [-24.0, -6.0, 0.0, 6.0, 24.0] {
            assert!(approx(gain_to_db(db_to_gain(db)), db));
        }
        assert!(approx(db_to_gain(0.0), 1.0));
        assert!(approx(db_to_gain(-6.0206), 0.5));
    }

    #[test]
    fn trim_reaches_target() {
        // Peak at -30 dBFS, target -18 => +12 dB trim.
        assert!(approx(compute_trim_db(-30.0, -18.0, 24.0).unwrap(), 12.0));
        // Peak above target => negative trim.
        assert!(approx(compute_trim_db(-6.0, -18.0, 24.0).unwrap(), -12.0));
    }

    #[test]
    fn trim_clamps_to_configurable_limit() {
        assert!(approx(compute_trim_db(-59.0, -6.0, 24.0).unwrap(), 24.0));
        assert!(approx(compute_trim_db(-59.0, -6.0, 12.0).unwrap(), 12.0));
        assert!(approx(compute_trim_db(0.0, -40.0, 6.0).unwrap(), -6.0));
    }

    #[test]
    fn no_signal_below_gate() {
        assert!(compute_trim_db(-60.1, -18.0, 24.0).is_none());
        assert!(compute_trim_db(-90.0, -18.0, 24.0).is_none());
        // Exactly at the gate still counts as signal.
        assert!(compute_trim_db(-60.0, -18.0, 24.0).is_some());
    }

    #[test]
    fn rider_freezes_below_gate() {
        let trim = rider_step(3.0, -80.0, -18.0, 24.0, 0.1);
        assert!(approx(trim, 3.0));
    }

    #[test]
    fn rider_slew_is_asymmetric_and_clamped() {
        // Big positive error: limited to +3 dB/s.
        let up = rider_step(0.0, -40.0, -18.0, 24.0, 1.0);
        assert!(approx(up, RIDER_UP_DB_PER_S));
        // Big negative error: limited to -12 dB/s.
        let down = rider_step(0.0, -6.0, -30.0, 24.0, 1.0);
        assert!(approx(down, -RIDER_DOWN_DB_PER_S));
        // Clamp honors the panel's limit.
        let clamped = rider_step(23.9, -40.0, -18.0, 24.0, 1.0);
        assert!(approx(clamped, 24.0));
    }

    #[test]
    fn envelope_attacks_faster_than_it_releases() {
        let (a, r) = (onepole_coef(ENV_ATTACK_S, 48000.0), onepole_coef(ENV_RELEASE_S, 48000.0));
        let mut env = 0.0;
        env = envelope_step(env, 1.0, a, r);
        let attacked = env;
        env = envelope_step(env, 0.0, a, r);
        assert!(attacked > 0.0);
        assert!(env < attacked);
        assert!(attacked - env < attacked); // release is gradual, not a reset
    }
}
