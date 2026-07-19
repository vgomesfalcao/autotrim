#pragma once

#include "LookAndFeel.h"
#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace autotrim
{
// Horizontal peak meter on a -60..0 dBFS scale with a dB readout.
class MeterBar : public juce::Component
{
public:
    void setLevelLin(float newLevelLin);
    void paint(juce::Graphics& g) override;

private:
    float levelDb = -200.0f;
};

// Per-channel view: name, meter, target, trim readout and toggles.
class ChannelView : public juce::Component
{
public:
    explicit ChannelView(AutoTrimProcessor& processor);
    void resized() override;
    void paint(juce::Graphics& g) override;
    void refresh();

private:
    AutoTrimProcessor& proc;

    juce::Label title, nameCaption, presetCaption, profileCaption, sensCaption;
    juce::TextEditor nameEditor;
    juce::ComboBox presetBox, profileBox;
    MeterBar meter, outMeter;
    juce::Label meterCaption, outMeterCaption, targetCaption, trimCaption, trimValue, statusLabel;
    juce::Slider targetSlider, trimSlider, sensSlider;
    juce::ToggleButton automationToggle { utf8("Automação ligada (aplica o trim)") },
        riderToggle { utf8("Modo contínuo (rider): segue o target ao vivo") },
        panelToggle { utf8("Usar esta instância como painel de controle") };

    juce::AudioProcessorValueTreeState::SliderAttachment targetAttachment, trimAttachment,
        sensAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment automationAttachment, riderAttachment;
    // Created after the box is populated (attachment applies the stored value).
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> profileAttachment;
};

// One row of the panel's channel list.
class PanelRow : public juce::Component
{
public:
    explicit PanelRow(std::shared_ptr<ChannelShared> channel);
    void resized() override;
    void paint(juce::Graphics& g) override;
    void refresh();

    std::shared_ptr<ChannelShared> shared;

private:
    juce::Label nameLabel, trimLabel, statusLabel;
    MeterBar meter, outMeter;
    juce::ComboBox presetBox;
    juce::ToggleButton automationToggle;
};

// Panel view: global settings, mass measurement, live channel list.
class PanelView : public juce::Component
{
public:
    explicit PanelView(AutoTrimProcessor& processor);
    void resized() override;
    void paint(juce::Graphics& g) override;
    void refresh();

private:
    AutoTrimProcessor& proc;

    juce::Label title, durationCaption, maxTrimCaption, listHeader, emptyLabel;
    juce::Slider durationSlider, maxTrimSlider;
    juce::TextButton measureButton { "Medir e regular todos os canais" };
    juce::TextButton cancelButton { "Cancelar" };
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<PanelRow>> rows;
    juce::ToggleButton panelToggle { "Modo painel (desmarque para voltar ao modo canal)" };

    void rebuildRowsIfNeeded();
    void layoutRows();
};

class AutoTrimEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AutoTrimEditor(AutoTrimProcessor& processor);
    ~AutoTrimEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void rebuildView();

    AutoTrimProcessor& proc;
    AutoTrimLookAndFeel lookAndFeel;
    std::unique_ptr<juce::Component> view;
    bool viewIsPanel = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoTrimEditor)
};
} // namespace autotrim
