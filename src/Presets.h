// Channel-type presets ("AudioLive Ganhos" table): picking one sets the peak
// target, rider profile and sensitivity default — never the channel name.
#pragma once

#include "Dsp.h"
#include "Registry.h"

#include <vector>

namespace autotrim::presets
{
// Rider profile indices (must match the "profile" AudioParameterChoice)
constexpr int kProfileVoice = 0;
constexpr int kProfileInstrument = 1;
constexpr int kProfileDrums = 2;

struct Preset
{
    const char* name; // UTF-8
    float targetDb;
    int profile; // pre-selected rider profile; the user can change it freely
};

inline const std::vector<Preset>& all()
{
    static const std::vector<Preset> list = {
        { "Bumbo", -6.0f, kProfileDrums },
        { "Snare", -6.0f, kProfileDrums },
        { "Esteira", -10.0f, kProfileDrums },
        { "Chimbal", -10.0f, kProfileDrums },
        { "Tons", -10.0f, kProfileDrums },
        { "SPD", -10.0f, kProfileDrums },
        { "Surdo", -10.0f, kProfileDrums },
        { "Overs", -10.0f, kProfileDrums },
        { "Vozes", -12.0f, kProfileVoice },
        { "Guitar", -12.0f, kProfileInstrument },
        { "Violão", -14.0f, kProfileInstrument },
        { "Keys", -10.0f, kProfileInstrument },
        { "Bass", -10.0f, kProfileInstrument },
        { "Sample", -10.0f, kProfileInstrument },
        { "LED", -12.0f, kProfileInstrument },
        { "AMB", -10.0f, kProfileInstrument },
    };
    return list;
}

inline void writeParam(juce::RangedAudioParameter* param, float value)
{
    if (param == nullptr)
        return;
    param->beginChangeGesture();
    param->setValueNotifyingHost(param->convertTo0to1(value));
    param->endChangeGesture();
}

inline void apply(ChannelShared& ch, const Preset& preset)
{
    writeParam(ch.targetParam, preset.targetDb);
    writeParam(ch.profileParam, (float) preset.profile);
    writeParam(ch.sensParam, dsp::profileFor(preset.profile).sensitivityDb);
    writeParam(ch.speedParam, dsp::profileFor(preset.profile).upDbPerS);
}

// Fills the box as an action menu: no persistent selection, just a trigger.
inline void fillComboBox(juce::ComboBox& box)
{
    box.clear(juce::dontSendNotification);
    box.setTextWhenNothingSelected(utf8("Preset…"));
    int id = 1;
    for (auto& preset : all())
        box.addItem(utf8(preset.name) + "  (" + juce::String((int) preset.targetDb) + " dB)",
                    id++);
}
} // namespace autotrim::presets
