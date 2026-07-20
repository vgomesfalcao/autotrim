#pragma once

#include "Dsp.h"
#include "Registry.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace autotrim
{
class AutoTrimProcessor : public juce::AudioProcessor
{
public:
    AutoTrimProcessor();
    ~AutoTrimProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AutoTrim"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Track name pushed by the host (VST3 hosts only; Logic/AU has no such API).
    void updateTrackProperties(const TrackProperties& properties) override;

    juce::AudioProcessorValueTreeState apvts;
    std::shared_ptr<ChannelShared> shared;

private:
    float gainLin = 1.0f;
    float gainCoef = 0.001f;
    // Sliding peak-hold detector for the continuous rider profiles.
    float holdSlots[40] = {};
    int holdSlotIndex = 0;
    float holdSlotElapsed = 0.0f;
    // Local measurement accumulators; reset when measEpoch changes so the
    // panel's reset can never race the audio thread. The window is armed:
    // counting starts only when signal first crosses the gate. The captured
    // level is the average of peaks (slot peaks, or hit peaks for drums).
    float measSumDb = 0.0f;
    int measCount = 0;
    float measSlotPeak = 0.0f;
    float measSlotElapsed = 0.0f;
    // Drum hits captured during the window (ring; the gated average drops
    // bleed/ghost notes).
    float measHitsDb[512] = {};
    uint32_t measEpoch = 0;
    bool measStartedLocal = false;
    int measSamplesLeft = 0;
    double currentSampleRate = 48000.0;

    // Hit detection state (drum profile)
    bool inHit = false;
    float hitPeak = 0.0f;
    int hitWindowSamplesLeft = 0;
    int hitCooldownSamples = 0;
    int hitWindowSamples = 2400;
    int hitRetriggerSamples = 4800;
    float hitHistoryDb[8] = {};
    int hitHistoryCount = 0;
    int hitHistoryPos = 0;
    float sinceHitS = 1000.0f;
    float sinceCorrectionS = 1000.0f;

    // AGC observation (window + persistence evidence) and solo bail-out,
    // both pure structs so the scenario tests can drive them directly.
    dsp::AgcObserver agcObserver;
    dsp::AgcBailDetector agcBail;

    // Clip Guard state (output peaks above 0 dBFS)
    float protClockS = 0.0f;
    float protEventTimes[8] = {};
    int protEventIndex = 0;
    int protEventCount = 0;
    bool protOverActive = false;
    float protOverElapsedS = 0.0f;
    float protOffenderMaxLin = 0.0f;
    float protSinceOverS = 1000.0f;

    // LUFS analysis (panel instance only, master passthrough)
    dsp::Biquad lufsShelf[2], lufsHighpass[2];
    double lufsSlotSumSq[30] = {};
    int lufsSlotSamples[30] = {};
    int lufsSlotIndex = 0;
    float lufsSlotElapsed = 0.0f;

    void resetHitState();
    void resetProtectionWindow();
    void resetAgcState();
    void analyzeLoudness(const juce::AudioBuffer<float>& buffer);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoTrimProcessor)
};
} // namespace autotrim
