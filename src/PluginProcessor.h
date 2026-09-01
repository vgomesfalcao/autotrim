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
    dsp::PeakHoldWindow riderHold;
    // Local measurement state; reset when measEpoch changes so the panel's
    // reset can never race the audio thread. The window is armed: counting
    // starts only when signal first crosses the gate. The captured level is
    // the average of peaks (slot peaks, or gated hit peaks for drums).
    dsp::SlotAverager measAverager;
    dsp::MeasBudget measBudget;
    float measPeakMaxLin = 0.0f; // highest peak seen (for the "peak" algorithm)
    int measCount = 0; // peaks captured: drum hits, or continuous slots
    float measPeaksDb[512] = {}; // all captured peaks (for the sporadic check)
    uint32_t measEpoch = 0;
    bool measStartedLocal = false;
    bool measBudgetArmed = false;
    double currentSampleRate = 48000.0;
    // Drum measurement: wall-clock time spent armed, closes the measurement
    // at registry::measDrumWindowS (or sooner via "Concluir agora").
    float measArmedS = 0.0f;
    // Channel's own floor (bleed + noise), tracked continuously — not reset
    // per measurement — so it's already converged by the time one starts.
    // Feeds the drum measurement's arm threshold (floor + margin), replacing
    // the fixed-dBFS Sensibilidade for that one purpose.
    dsp::FloorTracker measFloor;

    // Hit detection (drum profile): shared by the rider and the measurement.
    dsp::HitDetector hitDetector;
    int hitWindowSamples = 2400;
    int hitRetriggerSamples = 4800;
    // Drum measurement's hit-detector front-end: high-passed so a hit's real
    // separation from broadband bleed on the same mic survives detection.
    // Reset per measurement (epoch change); never touches the actual audio.
    dsp::Biquad drumDetectHpf[2];
    float hitHistoryDb[8] = {};
    int hitHistoryCount = 0;
    int hitHistoryPos = 0;
    float sinceHitS = 1000.0f;
    float sinceCorrectionS = 1000.0f;

    // AGC observation (window + persistence evidence) and solo bail-out,
    // both pure structs so the scenario tests can drive them directly.
    dsp::AgcObserver agcObserver;
    dsp::AgcBailDetector agcBail;

    // Clip Guard state machine (output peaks above 0 dBFS)
    dsp::ClipGuard clipGuard;

    // LUFS analysis (panel instance only, master passthrough)
    dsp::Biquad lufsShelf[2], lufsHighpass[2];
    double lufsSlotSumSq[30] = {};
    int lufsSlotSamples[30] = {};
    int lufsSlotIndex = 0;
    float lufsSlotElapsed = 0.0f;

    void resetHitState();
    void resetAgcState();
    void analyzeLoudness(const juce::AudioBuffer<float>& buffer);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoTrimProcessor)
};
} // namespace autotrim
