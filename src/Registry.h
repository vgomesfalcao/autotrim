// Global in-process registry shared by every plugin instance. The audio
// thread only touches the atomics inside ChannelShared; the registry lock is
// taken on the message/state threads only.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace autotrim
{
// JUCE's String(const char*) treats literals as Latin-1; every user-facing
// string with accents must go through this.
inline juce::String utf8(const char* text)
{
    return juce::String(juce::CharPointer_UTF8(text));
}

// Lock-free state for one plugin instance, plus pointers to its host
// parameters so the panel can write them from the message thread. The
// pointers stay valid while `alive` is true: the owning processor clears
// `alive` and unregisters before its parameters are destroyed.
struct ChannelShared
{
    uint64_t id = 0;
    // Manual panel order (defaults to registration order = id, which usually
    // matches track order on a fresh session load, but isn't guaranteed —
    // hosts don't expose a reliable track index to plugins). Editable via the
    // panel's move up/down buttons; persisted per instance so it survives a
    // session reload. Ties (equal order) fall back to id.
    std::atomic<int> order { 0 };

    juce::CriticalSection nameLock;
    juce::String name;     // user-set; guarded by nameLock, message/state threads only
    juce::String hostName; // provided by the host (VST3 only; AU has no such API)

    std::atomic<float> peakPreTrim { 0.0f };   // linear, decaying meter value
    std::atomic<float> peakPostTrim { 0.0f };  // linear, decaying, after trim
    // Wall-clock ms of the last processBlock; the UI meters use it to tell a
    // live source from a stopped one (a frozen peak must not stick).
    std::atomic<double> lastProcessMs { 0.0 };
    std::atomic<float> lufsShort { -100.0f };  // panel instance only (master mix)
    std::atomic<float> measuredPeak { 0.0f };  // linear, max during measurement
    std::atomic<float> riderOffsetDb { 0.0f };   // continuous-mode correction
    std::atomic<float> agcOffsetDb { 0.0f };     // slow AGC re-trim offset
    std::atomic<float> protectOffsetDb { 0.0f }; // Clip Guard cut (<= 0)
    std::atomic<bool> protectionActive { false };
    std::atomic<bool> panelMode { false };
    std::atomic<bool> panelCompact { false };
    std::atomic<bool> measuring { false };
    // Arm timeout for THIS channel's own measurement (independent of every
    // other channel — one channel's window never blocks another's).
    std::atomic<double> measArmDeadlineMs { 0.0 };
    // Armed measurement handshake: audio thread flips measStarted when signal
    // arrives and measDone when the window completes.
    std::atomic<bool> measStarted { false };
    std::atomic<bool> measDone { false };
    std::atomic<uint32_t> measHitCount { 0 }; // drum-profile measurement progress
    std::atomic<bool> noSignal { false };
    std::atomic<bool> alive { true };
    // Bumped when a new measurement starts so the audio thread resets its
    // local accumulator without racing the panel's reset.
    std::atomic<uint32_t> measEpoch { 0 };

    // Raw parameter values (owned by the processor's APVTS) for lock-free reads.
    std::atomic<float>* targetDb = nullptr;
    std::atomic<float>* trimDb = nullptr;
    std::atomic<float>* automationOn = nullptr;
    std::atomic<float>* riderOn = nullptr;
    std::atomic<float>* agcOn = nullptr;       // slow re-trim mode (off by default)
    std::atomic<float>* agcHoldS = nullptr;    // persistence before a re-trim
    std::atomic<float>* agcRangeDb = nullptr;  // max correction per re-trim
    std::atomic<float>* clipGuardOn = nullptr; // anti-clip protection (on by default)
    std::atomic<float>* profile = nullptr;     // index into dsp::kProfiles
    std::atomic<float>* sensitivityDb = nullptr;
    std::atomic<float>* speedDbPerS = nullptr; // rider ride-up rate
    std::atomic<float>* measPeakMode = nullptr; // measurement: 0 = average, 1 = highest peak

    // Parameter objects for writes from the message thread (panel/measurement).
    juce::RangedAudioParameter* targetParam = nullptr;
    juce::RangedAudioParameter* trimParam = nullptr;
    juce::RangedAudioParameter* automationParam = nullptr;
    juce::RangedAudioParameter* profileParam = nullptr;
    juce::RangedAudioParameter* sensParam = nullptr;
    juce::RangedAudioParameter* speedParam = nullptr;

    juce::String displayName() const;
    bool isAutomationOn() const { return automationOn != nullptr && automationOn->load() > 0.5f; }
    float effectiveTrimDb() const
    {
        return (trimDb != nullptr ? trimDb->load() : 0.0f) + riderOffsetDb.load()
               + agcOffsetDb.load() + protectOffsetDb.load();
    }
};

namespace registry
{
    std::shared_ptr<ChannelShared> registerInstance();
    void unregisterInstance(const std::shared_ptr<ChannelShared>& shared);
    // Alive, non-panel channels sorted by their manual panel order (ties
    // broken by registration order).
    std::vector<std::shared_ptr<ChannelShared>> channels();

    // Session-wide settings owned by the panel instance.
    extern std::atomic<float> maxTrimDb;
    extern std::atomic<float> measDurationS; // sustained-instrument window
    extern std::atomic<int> measHits;        // percussive (drum) hit count
    extern std::atomic<int> peakFrequentPct; // % of peaks that counts as "frequent"
} // namespace registry
} // namespace autotrim
