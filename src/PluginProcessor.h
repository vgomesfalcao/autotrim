#pragma once

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

    juce::AudioProcessorValueTreeState apvts;
    std::shared_ptr<ChannelShared> shared;

private:
    float gainLin = 1.0f;
    float gainCoef = 0.001f;
    float envLin = 0.0f;
    float envAttack = 0.5f;
    // Detector release depends on the rider profile (index-matched).
    float envReleaseCoefs[3] = { 0.001f, 0.001f, 0.001f };
    // Local measurement accumulator; reset when measEpoch changes so the
    // panel's reset can never race the audio thread.
    float measPeak = 0.0f;
    uint32_t measEpoch = 0;
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

    void resetHitState();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoTrimProcessor)
};
} // namespace autotrim
