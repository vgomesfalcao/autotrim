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
constexpr float kDefaultTargetDb = -10.0f;
constexpr float kDefaultMaxTrimDb = 36.0f;
constexpr float kDefaultMeasDurationS = 5.0f;
// Hard limit of the trim host parameter; the panel's editable clamp acts
// within this range. Weak sources can need +40 dB or more of make-up.
constexpr float kTrimParamRangeDb = 60.0f;

// Deadband around the target: within this window the rider holds still. The
// peak envelope reads slightly under the true peak the measurement used, so
// without a tolerance the rider would always "top up" a correct trim.
constexpr float kRiderToleranceDb = 1.0f;
// Peak envelope attack for the rider detector (release is per profile).
constexpr float kEnvAttackS = 0.005f;

// Rider profiles per source type. Targets and measurement are peak-based, so
// the continuous detector is a sliding-window peak hold: it reads "the recent
// peak" — the same quantity the measurement captured — and stays put between
// syllables and short pauses, so the rider only moves on real level changes
// (a release-envelope detector sawtooths on dynamic material and pumps in
// gaps). Drums are hit-based: the level is the average of the last few hit
// peaks (Drum Leveler-style).
struct RiderProfile
{
    float holdS;         // sliding peak-hold window (continuous profiles)
    float upDbPerS;      // ride-up slew
    float downDbPerS;    // ride-down slew
    float rideRangeDb;   // offset confined to ± this around the measured trim
    float sensitivityDb; // default activity threshold (per-channel parameter)
    float returnDbPerS;  // glide back to the measured trim when idle
    bool hitBased;
};

// Order must match the "profile" AudioParameterChoice: Voz, Instrumento, Bateria.
// Ride-up defaults follow the pro references: phrase-level for vocals (Vocal
// Rider "Slow" ≈ 1.5-3 dB/s), note-by-note for instruments (Bass Rider holds
// gain within notes), and fast per-hit convergence for drums (Drum Leveler
// jumps to the new gain each hit).
inline constexpr RiderProfile kProfiles[3] = {
    { 2.5f, 2.5f, 6.0f, 6.0f, -45.0f, 2.0f, false }, // Voz
    { 3.0f, 1.5f, 6.0f, 4.0f, -50.0f, 1.0f, false }, // Instrumento
    { 0.0f, 4.0f, 8.0f, 4.0f, -40.0f, 0.5f, true },  // Bateria
};
constexpr int kNumProfiles = 3;
inline constexpr const char* kProfileNames[3] = { "Voz", "Instrumento", "Bateria" };

// Editable rider speed (the ride-up rate; ride-down keeps the profile's
// down/up ratio). The range spans syllable-fast vocal riding up to Drum
// Leveler-style per-hit snaps.
constexpr float kSpeedMinDbPerS = 0.5f;
constexpr float kSpeedMaxDbPerS = 15.0f;

// Sliding peak-hold implementation: coarse 100 ms slots, enough for the
// longest profile window.
constexpr float kHoldSlotS = 0.1f;
constexpr int kMaxHoldSlots = 40;

inline const RiderProfile& profileFor(int index)
{
    return kProfiles[std::clamp(index, 0, kNumProfiles - 1)];
}

// Clip Guard: if output peaks exceed 0 dBFS (real digital clipping — not a
// margin over the target) this many times inside the window, the trim is cut
// automatically (even with the rider off) to protect the downstream chain.
// Shown in red; can be disabled per channel (on by default).
constexpr float kClipThresholdDb = 0.0f;
constexpr int kProtectHitCount = 5;
constexpr float kProtectWindowS = 3.0f;
constexpr float kProtectMaxCutDb = 12.0f;
// A continuously-over passage counts one event per rearm period.
constexpr float kProtectRearmS = 0.3f;
// The cut is temporary: after this long with no overs, and with enough
// headroom, it glides back to zero at the release rate.
constexpr float kProtectHoldS = 5.0f;
constexpr float kProtectReleaseDbPerS = 0.5f;

// AGC (automatic gain control): re-trim on *permanent* program-level changes
// (a new song, a timbre change mid-song). It watches a sliding recent-peak
// window all the time, ignores anything below the channel sensitivity
// (background noise / bleed), and only when the deviation from the target
// persists for the full hold period it corrects the previously measured trim
// in one step — like a re-measurement, not a rider. Off by default (works
// well for some sources, not all).
constexpr float kAgcSlotS = 0.5f;
constexpr int kAgcWinSlots = 6;      // 3 s recent-peak window (syllable gaps)
constexpr float kAgcToleranceDb = 3.0f;
// Default persistence before a re-trim fires; editable per channel. The AGC
// correction is a *separate* offset (shown in yellow), never written into the
// measured trim: 3 s of silence (the observation window emptying below the
// sensitivity) resets it, returning the channel to the measured gain.
constexpr float kAgcHoldS = 12.0f;
constexpr float kAgcHoldMinS = 3.0f;
constexpr float kAgcHoldMaxS = 20.0f;
// Default max correction per re-trim; editable per channel.
constexpr float kAgcRangeDb = 10.0f;
constexpr float kAgcRangeMinDb = 1.0f;
constexpr float kAgcRangeMaxDb = 20.0f;
// Like the rider's pause margin: a level *this* far below where the program
// should sit is a pause or bleed, never "the song got quieter".
constexpr float kAgcPauseMarginDb = 3.0f;

// Sudden-loud bail-out: any meaningful AGC boost with the output sustained
// past target + margin (a surprise guitar solo after a quiet passage) proves
// the measured calibration was right — the boost is dropped right away
// instead of waiting for the slow evidence. "Sustained" is accumulated
// over-time tolerating the gaps between the note peaks of a phrase, so a
// picked solo fires within a second while a lone spike (mic bump, pick
// scrape) never accumulates enough and the boost survives it.
constexpr float kAgcBailMinBoostDb = 1.0f;
constexpr float kAgcBailMarginDb = 5.0f;
constexpr float kAgcBailOverS = 0.2f; // accumulated over-time that fires
constexpr float kAgcBailGapS = 0.3f;  // silence between overs before restart

struct AgcBailDetector
{
    float overAccumS = 0.0f;
    float gapS = 0.0f;

    void reset() { overAccumS = gapS = 0.0f; }

    // Feed once per block; true = drop the boost now.
    bool step(bool over, float dt)
    {
        if (over)
        {
            gapS = 0.0f;
            overAccumS += dt;
            if (overAccumS >= kAgcBailOverS)
            {
                reset();
                return true;
            }
        }
        else
        {
            gapS += dt;
            if (gapS > kAgcBailGapS)
                overAccumS = 0.0f;
        }
        return false;
    }
};

// The correction one AGC re-trim applies, from the peak observed over the
// evidence period (the same formula a re-measurement would use). otherGainDb
// is the *base* gain only (trim + pending AGC) — never the rider, whose
// temporary correction would mask exactly the permanent shifts the AGC is
// there to fix, and never the Clip Guard cut, which is intentional
// attenuation. Empty while the level is close enough to the target, or when
// the deviation looks like a pause/bleed rather than a program change.
inline std::optional<float> agcCorrectionDb(float evidencePeakDb, float otherGainDb,
                                            float targetDb, float rangeDb)
{
    const float error = targetDb - (evidencePeakDb + otherGainDb);
    if (std::abs(error) <= kAgcToleranceDb)
        return std::nullopt;
    if (error > rangeDb + kAgcPauseMarginDb)
        return std::nullopt;
    return std::clamp(error, -rangeDb, rangeDb);
}

// Armed measurement: the window only starts counting when the channel first
// sees signal above the gate (isolated sources like toms play at arbitrary
// moments). Channels that never receive signal time out untouched.
constexpr float kMeasArmTimeoutS = 90.0f;
// The measured level is the *average* of peaks over the window, not the
// single loudest moment: continuous profiles average 0.5 s slot peaks above
// the arm threshold (pauses never drag the average down); drum profiles
// average the peaks of every hit captured during the window.
constexpr float kMeasSlotS = 0.5f;

// Drum measurement: hits more than this far below the reference are bleed or
// ghost notes, not direct hits — real strong hits of one drum cluster within
// ~5-6 dB, while ghost notes sit 10-20 dB under the accents.
constexpr float kMeasHitGateDb = 6.0f;
constexpr int kMeasMaxHits = 512; // ring capacity (~30 s of 16th-note hi-hat)

// Robust average of drum-hit peaks, BS.1770-style relative gating adapted to
// hits: the reference is the P90 hit (dense sources — one freak rimshot can't
// exclude everything, and majority bleed can't drag it), or the second
// loudest for sparse sources like toms (the max when only 1-2 hits exist).
// Hits below reference − kMeasHitGateDb are dropped; the result is the mean
// dB of the survivors.
inline float gatedHitAverageDb(const float* hitsDb, int count)
{
    if (count <= 0)
        return -200.0f;
    if (count == 1)
        return hitsDb[0];

    float sorted[kMeasMaxHits];
    const int n = std::min(count, kMeasMaxHits);
    std::copy(hitsDb, hitsDb + n, sorted);
    std::sort(sorted, sorted + n, [](float a, float b) { return a > b; });

    // P90 for dense sources, but never the loudest hit itself (index >= 1):
    // with few hits the reference degrades to the second loudest, and a
    // single freak rimshot can never become the reference at any count.
    const int refIdx = std::min(std::max(1, n / 10), n - 1);
    const float refDb = sorted[refIdx];
    const float gateDb = refDb - kMeasHitGateDb;

    float sum = 0.0f;
    int kept = 0;
    for (int i = 0; i < n && sorted[i] >= gateDb; ++i)
    {
        sum += sorted[i];
        ++kept;
    }
    return kept > 0 ? sum / (float) kept : refDb;
}

// Master loudness meter (panel instance): short-term LUFS per ITU-R BS.1770
// (K-weighting + 3 s window), no gating (gating only applies to integrated).
constexpr float kLufsTargetDb = -12.0f;
constexpr float kLufsWindowS = 3.0f;
constexpr float kLufsSlotS = 0.1f;
constexpr int kLufsSlots = 30;
constexpr float kLufsFloorDb = -70.0f;

// Transposed direct-form II biquad.
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    float process(float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() { z1 = z2 = 0.0f; }
};

// K-weighting stage 1: high shelf (+~4 dB), BS.1770 reference parameters,
// redesigned for the running sample rate via the RBJ cookbook.
inline Biquad makeKShelf(float fs)
{
    const float G = 3.999843853973347f, f0 = 1681.974450955533f, Q = 0.7071752369554196f;
    const float A = std::pow(10.0f, G / 40.0f);
    const float w0 = 2.0f * 3.14159265358979f * f0 / fs;
    const float alpha = std::sin(w0) / (2.0f * Q);
    const float c = std::cos(w0), sqA = std::sqrt(A);
    const float a0 = (A + 1.0f) - (A - 1.0f) * c + 2.0f * sqA * alpha;
    Biquad b;
    b.b0 = (A * ((A + 1.0f) + (A - 1.0f) * c + 2.0f * sqA * alpha)) / a0;
    b.b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * c)) / a0;
    b.b2 = (A * ((A + 1.0f) + (A - 1.0f) * c - 2.0f * sqA * alpha)) / a0;
    b.a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * c)) / a0;
    b.a2 = ((A + 1.0f) - (A - 1.0f) * c - 2.0f * sqA * alpha) / a0;
    return b;
}

// K-weighting stage 2: high-pass, BS.1770 reference parameters.
inline Biquad makeKHighpass(float fs)
{
    const float f0 = 38.13547087602444f, Q = 0.5003270373238773f;
    const float w0 = 2.0f * 3.14159265358979f * f0 / fs;
    const float alpha = std::sin(w0) / (2.0f * Q);
    const float c = std::cos(w0);
    const float a0 = 1.0f + alpha;
    Biquad b;
    b.b0 = ((1.0f + c) / 2.0f) / a0;
    b.b1 = (-(1.0f + c)) / a0;
    b.b2 = ((1.0f + c) / 2.0f) / a0;
    b.a1 = (-2.0f * c) / a0;
    b.a2 = (1.0f - alpha) / a0;
    return b;
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
                             const RiderProfile& profile, float dt, float upDbPerS,
                             float downDbPerS)
{
    const float error = targetDb - (levelDb + baseTrimDb + offsetDb);
    if (std::abs(error) <= kRiderToleranceDb)
        return offsetDb;
    // Correct only up to the edge of the deadband so the loop settles there
    // instead of oscillating around the exact target.
    const float effective = error > 0.0f ? error - kRiderToleranceDb : error + kRiderToleranceDb;
    const float step = std::clamp(effective, -downDbPerS * dt, upDbPerS * dt);
    return std::clamp(offsetDb + step, -profile.rideRangeDb, profile.rideRangeDb);
}

// Profile-default speeds (the editable "Velocidade" overrides them).
inline float riderOffsetStep(float offsetDb, float levelDb, float baseTrimDb, float targetDb,
                             const RiderProfile& profile, float dt)
{
    return riderOffsetStep(offsetDb, levelDb, baseTrimDb, targetDb, profile, dt,
                           profile.upDbPerS, profile.downDbPerS);
}

// Program-presence window: the rider may only ride *up* while the level is
// near where the program should sit (target − trim − offset). Anything
// further below is a pause or stage bleed — riding it up would boost noise to
// the range limit (absolute sensitivity alone cannot catch bleed above it).
constexpr float kRiderPauseMarginDb = 3.0f;

inline bool riderSeesProgram(float levelDb, float baseTrimDb, float offsetDb, float targetDb,
                             const RiderProfile& profile)
{
    const float error = targetDb - (levelDb + baseTrimDb + offsetDb);
    return error <= profile.rideRangeDb + kRiderPauseMarginDb;
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

// The Clip Guard state machine, extracted pure for scenario tests: counts
// clipping events (a sustained over re-arms one event per rearm period) and
// fires a protective cut when enough land inside the window; after a quiet
// hold with headroom the cut glides back to zero.
struct ClipGuard
{
    float clockS = 0.0f;
    float eventTimes[kProtectHitCount] = {};
    int eventIndex = 0;
    int eventCount = 0;
    bool overActive = false;
    float overElapsedS = 0.0f;
    float offenderMaxLin = 0.0f;
    float sinceOverS = 1000.0f;

    void resetWindow()
    {
        eventIndex = 0;
        eventCount = 0;
        overActive = false;
        overElapsedS = 0.0f;
        offenderMaxLin = 0.0f;
    }

    // Feed once per block; cutDb is the currently applied cut (<= 0).
    // Returns the new cut when it changes: deeper on a confirmed overload,
    // shallower (toward 0) while releasing.
    std::optional<float> step(float peakPostLin, float recentOutLin, float targetDb,
                              float cutDb, float dt)
    {
        std::optional<float> newCut;
        clockS += dt;
        if (peakPostLin > dbToGain(kClipThresholdDb))
        {
            sinceOverS = 0.0f;
            offenderMaxLin = std::max(offenderMaxLin, peakPostLin);
            bool newEvent = false;
            if (! overActive)
            {
                overActive = true;
                overElapsedS = 0.0f;
                newEvent = true;
            }
            else
            {
                overElapsedS += dt;
                if (overElapsedS >= kProtectRearmS)
                {
                    overElapsedS = 0.0f;
                    newEvent = true;
                }
            }
            if (newEvent)
            {
                eventTimes[eventIndex] = clockS;
                eventIndex = (eventIndex + 1) % kProtectHitCount;
                eventCount = std::min(eventCount + 1, kProtectHitCount);
                // After the increment, the next slot holds the oldest event.
                const float oldest = eventTimes[eventIndex];
                if (eventCount >= kProtectHitCount && clockS - oldest <= kProtectWindowS)
                {
                    const float cut = targetDb - gainToDb(offenderMaxLin);
                    newCut = std::clamp(cutDb + cut, -kProtectMaxCutDb, 0.0f);
                    resetWindow();
                }
            }
        }
        else
        {
            overActive = false;
            sinceOverS += dt;
        }

        // Temporary by design: after the hold with no overs, and with
        // headroom in the recent output, glide back to zero.
        if (! newCut && cutDb < 0.0f && sinceOverS > kProtectHoldS)
        {
            if (gainToDb(recentOutLin) + 1.0f <= kClipThresholdDb)
            {
                const float released = std::min(0.0f, cutDb + kProtectReleaseDbPerS * dt);
                newCut = released >= -0.05f ? 0.0f : released;
            }
        }
        return newCut;
    }
};

// Per-sample transient detector shared by the drum rider and the drum
// measurement: captures each hit's peak over a short window, with a
// retrigger cooldown so one drum stroke never counts twice.
struct HitDetector
{
    bool inHit = false;
    float peak = 0.0f;
    int windowSamplesLeft = 0;
    int cooldownSamples = 0;

    void reset()
    {
        inHit = false;
        peak = 0.0f;
        windowSamplesLeft = 0;
        cooldownSamples = 0;
    }

    // Returns the hit's peak (linear) when its capture window completes.
    std::optional<float> step(float framePeak, float thresholdLin, int windowSamples,
                              int retriggerSamples)
    {
        if (inHit)
        {
            peak = std::max(peak, framePeak);
            if (--windowSamplesLeft <= 0)
            {
                inHit = false;
                cooldownSamples = retriggerSamples - windowSamples;
                return peak;
            }
        }
        else if (cooldownSamples > 0)
        {
            --cooldownSamples;
        }
        else if (framePeak > thresholdLin)
        {
            inHit = true;
            peak = framePeak;
            windowSamplesLeft = windowSamples;
        }
        return std::nullopt;
    }
};

// Measurement window budget: elapsed time only advances while the signal is
// above the arm threshold, so a paused source (a breath, a gap between
// phrases) never burns through the window — this is what lets the default
// duration be short (5 s) even for sources with pauses. Applies to every
// profile, including drums: a rarely-hit tom's window only advances during
// its hits, so it can take much longer than 5 s of wall clock to fill.
struct MeasBudget
{
    int samplesLeft = 0;

    void arm(float durationS, double sampleRate)
    {
        samplesLeft = (int) (durationS * (float) sampleRate);
    }

    // Feed once per block; true = the window is exhausted (measurement done).
    bool step(int numSamples, bool aboveThreshold)
    {
        if (aboveThreshold)
            samplesLeft -= numSamples;
        return samplesLeft <= 0;
    }
};

// Continuous-profile measurement: running average (dB) of 0.5 s slot peaks
// above the arm threshold — pauses never enter the average.
struct SlotAverager
{
    float sumDb = 0.0f;
    int count = 0;
    float slotPeak = 0.0f;
    float slotElapsed = 0.0f;

    void reset()
    {
        sumDb = 0.0f;
        count = 0;
        slotPeak = 0.0f;
        slotElapsed = 0.0f;
    }

    // Feed once per block; returns the updated average (dB) at the slot
    // boundaries that contained program.
    std::optional<float> step(float blockPeakLin, float dt, float armLin)
    {
        std::optional<float> avgDb;
        slotPeak = std::max(slotPeak, blockPeakLin);
        slotElapsed += dt;
        if (slotElapsed >= kMeasSlotS)
        {
            slotElapsed = 0.0f;
            if (slotPeak > armLin)
            {
                sumDb += gainToDb(slotPeak);
                ++count;
                avgDb = sumDb / (float) count;
            }
            slotPeak = 0.0f;
        }
        return avgDb;
    }
};

// Sliding peak-hold window for the continuous rider detector: reads "the
// recent peak" so the rider stays put between syllables and short pauses.
struct PeakHoldWindow
{
    float slots[kMaxHoldSlots] = {};
    int index = 0;
    float elapsed = 0.0f;

    void reset()
    {
        for (auto& slot : slots)
            slot = 0.0f;
        index = 0;
        elapsed = 0.0f;
    }

    float step(float blockPeakLin, float dt, int numSlots)
    {
        slots[index % kMaxHoldSlots] =
            std::max(slots[index % kMaxHoldSlots], blockPeakLin);
        elapsed += dt;
        while (elapsed >= kHoldSlotS)
        {
            elapsed -= kHoldSlotS;
            index = (index + 1) % numSlots;
            slots[index] = 0.0f;
        }
        float holdMax = blockPeakLin;
        for (int i = 0; i < numSlots; ++i)
            holdMax = std::max(holdMax, slots[i]);
        return holdMax;
    }
};

// The AGC's whole observation state machine — 3 s recent-peak window,
// persistence evidence, silence detection — extracted pure so the scenario
// tests can drive it slot by slot: the state machines are where the real
// bugs live, not the formulas. Short pauses are bridged by the peak window;
// only a *full* window below the sensitivity reads as silence.
struct AgcObserver
{
    struct Action
    {
        bool silenceReset = false;         // 3 s below sensitivity: back to base
        std::optional<float> correctionDb; // deviation persisted: add to offset
    };

    float win[kAgcWinSlots] = {};
    int winIndex = 0;
    float slotPeak = 0.0f;
    float slotElapsed = 0.0f;
    float deviationS = 0.0f;
    int devSign = 0;
    float evidencePeakLin = 0.0f;

    void resetEvidence()
    {
        deviationS = 0.0f;
        devSign = 0;
        evidencePeakLin = 0.0f;
    }

    void reset()
    {
        for (auto& slot : win)
            slot = 0.0f;
        winIndex = 0;
        slotPeak = 0.0f;
        slotElapsed = 0.0f;
        resetEvidence();
    }

    // Feed once per block with the pre-trim block peak; appliedDb is the base
    // gain only (trim + pending AGC — never the rider or the Clip Guard cut).
    Action step(float blockPeakLin, float dt, float sensDb, float appliedDb, float targetDb,
                float holdS, float rangeDb)
    {
        Action action;
        slotPeak = std::max(slotPeak, blockPeakLin);
        slotElapsed += dt;
        while (slotElapsed >= kAgcSlotS)
        {
            slotElapsed -= kAgcSlotS;
            winIndex = (winIndex + 1) % kAgcWinSlots;
            win[winIndex] = slotPeak;
            slotPeak = 0.0f;

            float winMax = 0.0f;
            for (float slot : win)
                winMax = std::max(winMax, slot);
            const float winMaxDb = gainToDb(winMax);
            if (winMaxDb < sensDb)
            {
                action.silenceReset = true;
                resetEvidence();
                continue;
            }

            const auto slotCorr = agcCorrectionDb(winMaxDb, appliedDb, targetDb, rangeDb);
            if (! slotCorr)
            {
                // Back within tolerance restarts the evidence; a pause-like
                // drop (too far below the program) just holds it.
                if (std::abs(targetDb - (winMaxDb + appliedDb)) <= kAgcToleranceDb)
                    resetEvidence();
                continue;
            }

            const int sign = *slotCorr > 0.0f ? 1 : -1;
            if (sign != devSign)
            {
                resetEvidence();
                devSign = sign;
            }
            deviationS += kAgcSlotS;
            evidencePeakLin = std::max(evidencePeakLin, winMax);
            if (deviationS >= holdS)
            {
                action.correctionDb =
                    agcCorrectionDb(gainToDb(evidencePeakLin), appliedDb, targetDb, rangeDb);
                resetEvidence();
            }
        }
        return action;
    }
};
} // namespace autotrim::dsp
