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
    // Anchors the nonlinear scale (fixed per channel — the bar must never
    // move because of a trim change).
    void setScaleAnchorDb(float newAnchorDb);
    // Where to draw the mark (may differ from the anchor, e.g. the input
    // meter marks target − trim on a target-anchored scale).
    void setTickDb(float newTickDb);
    void setTickVisible(bool shouldShow) { tickVisible = shouldShow; }
    void paint(juce::Graphics& g) override;

private:
    float mapDbToFrac(float db) const;

    float levelDb = -200.0f;
    float anchorDb = -10.0f;
    float tickDb = -10.0f;
    bool tickVisible = true;
};

// Designed status strip: TRIM chip with the total applied gain, plus a
// bipolar center-zero bar showing the rider's live correction within its
// ride range. Replaces the old hard-to-read text line.
class StatusStrip : public juce::Component
{
public:
    enum State { normal = 0, measuring, noSignal };

    void update(float trimTotalDb, float riderOffsetDb, float rideRangeDb, bool riderEnabled,
                State newState, float protectDb);
    void paint(juce::Graphics& g) override;

private:
    float trimTotal = 0.0f, offset = 0.0f, range = 6.0f, protect = 0.0f;
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

    juce::Label title, nameCaption, presetCaption, profileCaption, sensCaption, sectionLabel;
    juce::TextEditor nameEditor;
    juce::ComboBox presetBox, profileBox;
    MeterBar meter, outMeter;
    StatusStrip statusStrip;
    juce::Label meterCaption, outMeterCaption, targetCaption, trimCaption;
    juce::Slider targetSlider, trimSlider, sensSlider;
    juce::ToggleButton automationToggle { utf8("Automação") },
        riderToggle { utf8("Rider (modo contínuo)") },
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
    juce::TextButton measureButton { "Medir" }, expandButton;
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

    juce::Label title, durationCaption, maxTrimCaption, listHeader, emptyLabel;
    juce::Slider durationSlider, maxTrimSlider;
    juce::TextButton measureButton { "Medir e regular todos os canais" };
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
