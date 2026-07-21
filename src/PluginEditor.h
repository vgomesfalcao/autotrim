#pragma once

#include "LookAndFeel.h"
#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace autotrim
{
// Horizontal peak meter with a target-anchored nonlinear scale: the target
// mark sits at ~70% of the width, the 12 dB below it get the most pixels
// (that's where the action is), everything far below is compressed into the
// first quarter, and whatever exceeds the target runs to the right edge.
class MeterBar : public juce::Component
{
public:
    void setLevelLin(float newLevelLin);
    // The scale's fixed reference point — 0 dBFS for peak meters, the LUFS
    // target for the loudness meter — never varies per channel/instance, so
    // every meter shares the same geometry and marks are genuinely
    // comparable and move with their real value.
    void setHotDb(float newHotDb);
    // Where to draw the mark (e.g. the per-channel Target, or target − trim
    // for the input meter); moves freely within the fixed scale.
    void setTickDb(float newTickDb);
    void setTickVisible(bool shouldShow) { tickVisible = shouldShow; }
    void setUnit(const juce::String& newUnit) { unit = newUnit; }
    // Yellow fill while an AGC correction is acting on this channel.
    void setAgcTint(bool shouldTint);
    void paint(juce::Graphics& g) override;

private:
    float mapDbToFrac(float db) const;

    float levelDb = -200.0f;
    float hotDb = 0.0f;
    float tickDb = -10.0f;
    bool tickVisible = true;
    juce::String unit { " dB" };
    // Console-style readability: peak-hold marker (compare against the target
    // calmly) and a slow-refresh numeric readout.
    bool agcTint = false;
    float holdDb = -200.0f;
    double holdSetMs = 0.0;
    double holdLastStepMs = 0.0; // dt anchor for the fall, independent of holdSetMs
    float textDb = -200.0f;
    double textSetMs = 0.0;
};

// Designed status strip: TRIM chip with the total applied gain, plus a
// bipolar center-zero bar showing the rider's live correction within its
// ride range. Replaces the old hard-to-read text line.
class StatusStrip : public juce::Component
{
public:
    enum State { normal = 0, armed, measuring, noSignal };

    void update(float riderOffsetDb, float rideRangeDb, bool riderEnabled, State newState,
                float protectDb, float agcDb);
    void paint(juce::Graphics& g) override;

private:
    float offset = 0.0f, range = 6.0f, protect = 0.0f, agc = 0.0f;
    bool riderOn = false;
    State state = normal;
};

// Per-channel view: name, meter, target, trim readout and toggles.
class ChannelView : public juce::Component
{
public:
    explicit ChannelView(AutoTrimProcessor& processor);
    void resized() override;
    void paint(juce::Graphics& g) override;
    void refresh();
    int desiredHeight() const;

private:
    AutoTrimProcessor& proc;
    juce::String lastPlaceholder;
    juce::Rectangle<int> configCard;
    bool advancedOpen = false; // "Avançado" disclosure, closed by default
    juce::TextButton advancedButton;

    juce::Label title, nameCaption, presetCaption, profileCaption, sensCaption, speedCaption,
        agcTimeCaption, agcRangeCaption, sectionLabel;
    juce::TextEditor nameEditor;
    juce::ComboBox presetBox, profileBox;
    MeterBar meter, outMeter;
    StatusStrip statusStrip;
    juce::Label meterCaption, outMeterCaption, targetCaption, trimCaption;
    juce::Slider targetSlider, trimSlider, sensSlider, speedSlider, agcTimeSlider,
        agcRangeSlider;
    juce::TextButton measureButton { "Regular ganho" };
    juce::ToggleButton automationToggle { utf8("Automação") },
        riderToggle { utf8("Rider (modo contínuo)") },
        clipGuardToggle { utf8("Clip Guard (corte anti-clip)") },
        agcToggle { utf8("AGC (reajuste do ganho)") },
        panelToggle { utf8("Usar esta instância como painel de controle") };

    juce::AudioProcessorValueTreeState::SliderAttachment targetAttachment, trimAttachment,
        sensAttachment, speedAttachment, agcTimeAttachment, agcRangeAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment automationAttachment, riderAttachment,
        clipGuardAttachment, agcAttachment;
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
    juce::Label nameLabel, statusLabel;
    MeterBar meter, outMeter;
    juce::TextButton regButton;
    juce::Slider trimKnob;
    juce::ComboBox presetBox;
    juce::ToggleButton automationToggle;
};

// Compact-panel row: name + post-trim meter with target mark, nothing else.
class MiniPanelRow : public juce::Component
{
public:
    explicit MiniPanelRow(std::shared_ptr<ChannelShared> channel);
    void resized() override;
    void refresh();

    std::shared_ptr<ChannelShared> shared;

private:
    juce::Label nameLabel;
    MeterBar outMeter;
    juce::TextButton regButton;
};

// Minimal panel for piloting the live show: measure button + one meter per
// channel in a small always-glanceable window.
class MiniPanelView : public juce::Component
{
public:
    explicit MiniPanelView(AutoTrimProcessor& processor);
    void resized() override;
    void refresh();
    int desiredHeight() const;

private:
    void rebuildRowsIfNeeded();
    void layoutRows();

    AutoTrimProcessor& proc;
    juce::TextButton measureButton { "Regular ganhos" }, expandButton;
    juce::Label lufsCaption;
    MeterBar lufsMeter;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<MiniPanelRow>> rows;
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

    juce::Label title, durationCaption, maxTrimCaption, listHeader, emptyLabel, lufsCaption;
    MeterBar lufsMeter;
    juce::Slider durationSlider, maxTrimSlider;
    juce::TextButton measureButton { "Regular ganhos de todos os canais" };
    juce::TextButton cancelButton { "Cancelar" };
    juce::TextButton compactButton { "Modo compacto" };
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
    enum class ViewMode { channel, panel, mini };

    void timerCallback() override;
    void rebuildView();
    ViewMode currentMode() const;

    AutoTrimProcessor& proc;
    AutoTrimLookAndFeel lookAndFeel;
    std::unique_ptr<juce::Component> view;
    ViewMode viewMode = ViewMode::channel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoTrimEditor)
};
} // namespace autotrim
