// Pure gain/rider/envelope math, JUCE-free so the unit tests build alone.
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace autotrim::dsp
{
// Peaks below this level mean "no signal": measurement skips the channel and
// the rider freezes.
constexpr float kGateDb = -60.0f;
constexpr float kDefaultTargetDb = -18.0f;
constexpr float kDefaultMaxTrimDb = 24.0f;
constexpr float kDefaultMeasDurationS = 10.0f;
// Hard limit of the trim host parameter; the panel's editable clamp acts
// within this range.
constexpr float kTrimParamRangeDb = 48.0f;

// Deadband around the target: within this window the rider holds still. The
// peak envelope reads slightly under the true peak the measurement used, so
// without a tolerance the rider would always "top up" a correct trim.
constexpr float kRiderToleranceDb = 1.0f;
// Peak envelope attack for the rider detector (release is per profile).
constexpr float kEnvAttackS = 0.005f;

// Rider profiles per source type. Targets and measurement are peak-based, so
// the continuous detector is a slow-release peak envelope (sits near the
// recent peak level, consistent with the target) rather than plain RMS.
// Drums are hit-based: continuous riding pumps between hits, so the level is
// the average of the last few hit peaks (Drum Leveler-style).
struct RiderProfile
{
    float envReleaseS;   // detector release (continuous profiles)
    float upDbPerS;      // ride-up slew
    float downDbPerS;    // ride-down slew
    float rideRangeDb;   // offset confined to ± this around the measured trim
    float sensitivityDb; // default activity threshold (per-channel parameter)
    float returnDbPerS;  // glide back to the measured trim when idle
    bool hitBased;
};

// Order must match the "profile" AudioParameterChoice: Voz, Instrumento, Bateria.
inline constexpr RiderProfile kProfiles[3] = {
    { 1.5f, 2.0f, 6.0f, 6.0f, -45.0f, 2.0f, false }, // Voz
    { 2.0f, 1.5f, 6.0f, 4.0f, -50.0f, 1.0f, false }, // Instrumento
    { 0.0f, 1.5f, 4.0f, 4.0f, -40.0f, 0.5f, true },  // Bateria
};
constexpr int kNumProfiles = 3;

inline const RiderProfile& profileFor(int index)
{
    return kProfiles[std::clamp(index, 0, kNumProfiles - 1)];
}

// Hit detection (drum profile)
constexpr float kHitWindowS = 0.050f;    // peak capture window per hit
constexpr float kHitRetriggerS = 0.100f; // minimum spacing between hits
constexpr int kHitHistory = 8;           // hits averaged for the level estimate
constexpr float kDrumIdleS = 3.0f;       // no hits for this long -> idle glide
constexpr float kHitCorrectionMaxDtS = 1.0f;
// Smoothing time for the applied gain (click-free trim changes).
constexpr float kGainSmoothS = 0.050f;
// Meter fall speed, dB per second.
constexpr float kMeterDecayDbPerS = 20.0f;

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

inline float gainToDb(float gain)
{
    return gain <= 1e-10f ? -200.0f : 20.0f * std::log10(gain);
}

// One-pole coefficient for a time constant at the given sample rate.
inline float onepoleCoef(float timeS, float sampleRate)
{
    return timeS <= 0.0f ? 1.0f : 1.0f - std::exp(-1.0f / (timeS * sampleRate));
}

// Trim needed to bring measuredPeakDb to targetDb, clamped to the panel's
// limit. Empty when the peak is below the no-signal gate.
inline std::optional<float> computeTrimDb(float measuredPeakDb, float targetDb, float maxTrimDb)
{
    if (measuredPeakDb < kGateDb)
        return std::nullopt;
    return std::clamp(targetDb - measuredPeakDb, -maxTrimDb, maxTrimDb);
}

// One rider step over the *offset* around the measured trim: closes the error
// between the output level (detector + base trim + offset) and the target,
// slew-limited and confined to the profile's ride range. The loop converges
// because raising the offset raises the output, shrinking the error.
inline float riderOffsetStep(float offsetDb, float levelDb, float baseTrimDb, float targetDb,
                             const RiderProfile& profile, float dt)
{
    const float error = targetDb - (levelDb + baseTrimDb + offsetDb);
    if (std::abs(error) <= kRiderToleranceDb)
        return offsetDb;
    // Correct only up to the edge of the deadband so the loop settles there
    // instead of oscillating around the exact target.
    const float effective = error > 0.0f ? error - kRiderToleranceDb : error + kRiderToleranceDb;
    const float step = std::clamp(effective, -profile.downDbPerS * dt, profile.upDbPerS * dt);
    return std::clamp(offsetDb + step, -profile.rideRangeDb, profile.rideRangeDb);
}

// Below the sensitivity threshold the rider glides the offset back toward the
// measured trim instead of chasing noise or bleed.
inline float riderIdleStep(float offsetDb, const RiderProfile& profile, float dt)
{
    const float step =
        std::clamp(-offsetDb, -profile.returnDbPerS * dt, profile.returnDbPerS * dt);
    return offsetDb + step;
}

// Peak envelope follower over one sample.
inline float envelopeStep(float env, float sampleAbs, float attackCoef, float releaseCoef)
{
    const float coef = sampleAbs > env ? attackCoef : releaseCoef;
    return env + coef * (sampleAbs - env);
}
} // namespace autotrim::dsp
