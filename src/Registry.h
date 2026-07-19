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

    juce::CriticalSection nameLock;
    juce::String name; // guarded by nameLock, message/state threads only

    std::atomic<float> peakPreTrim { 0.0f };   // linear, decaying meter value
    std::atomic<float> peakPostTrim { 0.0f };  // linear, decaying, after trim
    std::atomic<float> measuredPeak { 0.0f };  // linear, max during measurement
    std::atomic<float> riderOffsetDb { 0.0f }; // continuous-mode correction
    std::atomic<bool> panelMode { false };
    std::atomic<bool> measuring { false };
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
    std::atomic<float>* profile = nullptr;     // index into dsp::kProfiles
    std::atomic<float>* sensitivityDb = nullptr;

    // Parameter objects for writes from the message thread (panel/measurement).
    juce::RangedAudioParameter* targetParam = nullptr;
    juce::RangedAudioParameter* trimParam = nullptr;
    juce::RangedAudioParameter* automationParam = nullptr;
    juce::RangedAudioParameter* profileParam = nullptr;
    juce::RangedAudioParameter* sensParam = nullptr;

    juce::String displayName() const;
    bool isAutomationOn() const { return automationOn != nullptr && automationOn->load() > 0.5f; }
    float effectiveTrimDb() const
    {
        return (trimDb != nullptr ? trimDb->load() : 0.0f) + riderOffsetDb.load();
    }
};

namespace registry
{
    std::shared_ptr<ChannelShared> registerInstance();
    void unregisterInstance(const std::shared_ptr<ChannelShared>& shared);
    // Alive, non-panel channels in registration order.
    std::vector<std::shared_ptr<ChannelShared>> channels();

    // Session-wide settings owned by the panel instance.
    extern std::atomic<float> maxTrimDb;
    extern std::atomic<float> measDurationS;
} // namespace registry
} // namespace autotrim
