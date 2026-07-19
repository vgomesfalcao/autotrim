#include "PluginEditor.h"

#include "Dsp.h"
#include "Measurement.h"
#include "Presets.h"

namespace autotrim
{
namespace
{
    constexpr int kChannelWidth = 440;
    constexpr int kPanelWidth = 860;
    constexpr int kPanelHeight = 580;
    constexpr int kRowHeight = 46;
    constexpr int kColName = 160;
    constexpr int kColPreset = 120;
    constexpr int kColTrim = 90;
    constexpr int kColAuto = 70;
    constexpr int kColStatus = 130;

    juce::String formatDb(float db)
    {
        return (db >= 0.0f ? "+" : "") + juce::String(db, 1) + " dB";
    }

    void styleCaption(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(13.0f)));
        label.setColour(juce::Label::textColourId, colours::subtext);
    }

    void styleTitle(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
        label.setColour(juce::Label::textColourId, colours::text);
    }

    void styleHSlider(juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    }

    // Rotary knob following the conventions users already know from FabFilter
    // and friends: calm drag (400 px for the full range), Cmd/Ctrl-drag for
    // fine adjustment (JUCE velocity mode), double-click resets to default,
    // big value readout underneath.
    void styleKnob(juce::Slider& slider, const juce::String& suffix, double defaultValue)
    {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 104, 26);
        slider.setMouseDragSensitivity(400);
        slider.setDoubleClickReturnValue(true, defaultValue);
        slider.setTextValueSuffix(suffix);
    }

    // Compact draggable value bar for set-once parameters: quiet by design,
    // still draggable/typeable when needed.
    void styleCompactBar(juce::Slider& slider, const juce::String& suffix, double defaultValue)
    {
        slider.setSliderStyle(juce::Slider::LinearBar);
        slider.setMouseDragSensitivity(300);
        slider.setDoubleClickReturnValue(true, defaultValue);
        slider.setTextValueSuffix(suffix);
        slider.setColour(juce::Slider::trackColourId, colours::accent.withAlpha(0.25f));
        slider.setColour(juce::Slider::textBoxTextColourId, colours::text);
    }

    // Small uppercase section header with letter spacing.
    void styleSection(juce::Label& label, const juce::String& text)
    {
        label.setText(text.toUpperCase(), juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold))
                          .withExtraKerningFactor(0.12f));
        label.setColour(juce::Label::textColourId, colours::subtext);
    }

    void writeBoolParam(juce::RangedAudioParameter* param, bool value)
    {
        if (param == nullptr)
            return;
        param->beginChangeGesture();
        param->setValueNotifyingHost(value ? 1.0f : 0.0f);
        param->endChangeGesture();
    }
} // namespace

//==============================================================================
namespace
{
    // Nonlinear meter scale anchored on the target.
    constexpr float kMeterFloorDb = -60.0f;
    constexpr float kMeterKneeDb = 12.0f;  // fine-zoom span below the target
    constexpr float kMeterKneeFrac = 0.25f;
    constexpr float kMeterTargetFrac = 0.70f;
} // namespace

void MeterBar::setLevelLin(float newLevelLin)
{
    const float newDb = dsp::gainToDb(newLevelLin);
    if (std::abs(newDb - levelDb) > 0.05f)
    {
        levelDb = newDb;
        repaint();
    }
}

void MeterBar::setScaleAnchorDb(float newAnchorDb)
{
    if (std::abs(newAnchorDb - anchorDb) > 0.05f)
    {
        anchorDb = newAnchorDb;
        repaint();
    }
}

void MeterBar::setTickDb(float newTickDb)
{
    if (std::abs(newTickDb - tickDb) > 0.05f)
    {
        tickDb = newTickDb;
        repaint();
    }
}

float MeterBar::mapDbToFrac(float db) const
{
    const float t = juce::jlimit(kMeterFloorDb + 4.0f, -1.0f, anchorDb);
    const float knee = juce::jmax(t - kMeterKneeDb, kMeterFloorDb + 2.0f);
    if (db <= kMeterFloorDb)
        return 0.0f;
    if (db <= knee)
        return kMeterKneeFrac * (db - kMeterFloorDb) / (knee - kMeterFloorDb);
    if (db <= t)
        return kMeterKneeFrac + (kMeterTargetFrac - kMeterKneeFrac) * (db - knee) / (t - knee);
    if (db >= 0.0f)
        return 1.0f;
    return kMeterTargetFrac + (1.0f - kMeterTargetFrac) * (db - t) / (0.0f - t);
}

void MeterBar::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(colours::cardOutline);
    g.fillRoundedRectangle(r, 4.0f);

    const float frac = juce::jlimit(0.0f, 1.0f, mapDbToFrac(levelDb));
    if (frac > 0.001f)
    {
        // Teal up to the target mark, then amber into red past it.
        juce::ColourGradient gradient(colours::meterLow, r.getX(), 0.0f,
                                      colours::meterHigh, r.getRight(), 0.0f, false);
        gradient.addColour(kMeterTargetFrac, colours::meterLow);
        gradient.addColour(juce::jmin(kMeterTargetFrac + 0.12, 0.99), colours::warning);
        g.setGradientFill(gradient);
        g.fillRoundedRectangle(r.withWidth(r.getWidth() * frac), 4.0f);
    }

    // Target mark
    if (tickVisible)
    {
        const float tickFrac = juce::jlimit(0.01f, 0.99f, mapDbToFrac(tickDb));
        const float tickX = r.getX() + r.getWidth() * tickFrac;
        g.setColour(colours::text.withAlpha(0.85f));
        g.fillRect(tickX - 1.0f, r.getY(), 2.0f, r.getHeight());
    }

    g.setColour(colours::text);
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    const auto text = levelDb <= -90.0f ? juce::String("-inf dB") : formatDb(levelDb);
    g.drawText(text, getLocalBounds().reduced(6, 0), juce::Justification::centredRight);
}

//==============================================================================
void StatusStrip::update(float trimTotalDb, float riderOffsetDb, float rideRangeDb,
                         bool riderEnabled, State newState, float protectDb)
{
    const bool changed = std::abs(trimTotalDb - trimTotal) > 0.05f
                         || std::abs(riderOffsetDb - offset) > 0.05f
                         || std::abs(rideRangeDb - range) > 0.05f || riderEnabled != riderOn
                         || newState != state || std::abs(protectDb - protect) > 0.05f;
    if (! changed)
        return;
    trimTotal = trimTotalDb;
    offset = riderOffsetDb;
    range = juce::jmax(0.1f, rideRangeDb);
    riderOn = riderEnabled;
    state = newState;
    protect = protectDb;
    repaint();
}

void StatusStrip::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();

    // GANHO chip: the total gain currently applied, always readable at a glance.
    auto chip = r.removeFromLeft(162.0f);
    g.setColour(colours::card);
    g.fillRoundedRectangle(chip, 8.0f);
    g.setColour(colours::cardOutline);
    g.drawRoundedRectangle(chip.reduced(0.5f), 8.0f, 1.0f);
    auto chipInner = chip.reduced(12.0f, 4.0f);
    g.setColour(colours::subtext);
    g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold))
                  .withExtraKerningFactor(0.12f));
    g.drawText("GANHO", chipInner.removeFromLeft(52.0f), juce::Justification::centredLeft);
    g.setColour(colours::accent);
    g.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
    g.drawText(formatDb(trimTotal), chipInner, juce::Justification::centredRight);

    r.removeFromLeft(14.0f);

    // Overload protection engaged: red badge, impossible to miss.
    if (protect < -0.05f)
    {
        auto badge = r.removeFromRight(118.0f);
        g.setColour(colours::meterHigh.withAlpha(0.18f));
        g.fillRoundedRectangle(badge, 8.0f);
        g.setColour(colours::meterHigh);
        g.drawRoundedRectangle(badge.reduced(0.5f), 8.0f, 1.0f);
        g.setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
        g.drawText(utf8("PROT ") + formatDb(protect), badge.reduced(8.0f, 0.0f),
                   juce::Justification::centred);
        r.removeFromRight(10.0f);
    }

    if (state == measuring)
    {
        g.setColour(colours::info);
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText(utf8("medindo…"), r, juce::Justification::centredLeft);
        return;
    }
    if (state == noSignal)
    {
        g.setColour(colours::warning);
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText(utf8("sem sinal na última medição"), r, juce::Justification::centredLeft);
        return;
    }

    // Rider: bipolar center-zero bar within the profile's ride range.
    const float alpha = riderOn ? 1.0f : 0.35f;
    g.setColour(colours::subtext.withMultipliedAlpha(riderOn ? 1.0f : 0.6f));
    g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold))
                  .withExtraKerningFactor(0.12f));
    g.drawText("RIDER", r.removeFromLeft(46.0f), juce::Justification::centredLeft);

    auto valueArea = r.removeFromRight(56.0f);
    auto barArea = r.withSizeKeepingCentre(r.getWidth() - 8.0f, 10.0f);
    g.setColour(colours::cardOutline.withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(barArea, 5.0f);

    const float cx = barArea.getCentreX();
    if (riderOn)
    {
        const float frac = juce::jlimit(-1.0f, 1.0f, offset / range);
        const float w = std::abs(frac) * (barArea.getWidth() / 2.0f - 2.0f);
        if (w > 0.5f)
        {
            const auto fill = frac >= 0.0f
                                  ? juce::Rectangle<float>(cx, barArea.getY() + 2.0f, w, 6.0f)
                                  : juce::Rectangle<float>(cx - w, barArea.getY() + 2.0f, w, 6.0f);
            g.setColour(colours::accent);
            g.fillRoundedRectangle(fill, 3.0f);
        }
    }
    // Center (zero) notch
    g.setColour(colours::text.withAlpha(0.5f * alpha));
    g.fillRect(cx - 0.75f, barArea.getY() - 2.0f, 1.5f, barArea.getHeight() + 4.0f);

    g.setColour(riderOn ? colours::text : colours::subtext);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText(riderOn ? formatDb(offset) : juce::String("off"), valueArea,
               juce::Justification::centredRight);
}

//==============================================================================
ChannelView::ChannelView(AutoTrimProcessor& processor)
    : proc(processor),
      targetAttachment(proc.apvts, "target", targetSlider),
      trimAttachment(proc.apvts, "trim", trimSlider),
      sensAttachment(proc.apvts, "sens", sensSlider),
      automationAttachment(proc.apvts, "automation", automationToggle),
      riderAttachment(proc.apvts, "rider", riderToggle)
{
    styleTitle(title, "AutoTrim");
    styleCaption(nameCaption, "Nome do canal");
    styleCaption(presetCaption, "Preset");
    styleCaption(profileCaption, "Perfil do rider");
    styleCaption(sensCaption, "Sensibilidade");
    styleCaption(meterCaption, "Entrada");
    styleCaption(outMeterCaption, utf8("Saída"));
    styleCaption(targetCaption, "Target");
    styleCaption(trimCaption, "Ganho");
    styleCaption(sensCaption, "Sensibilidade");
    styleSection(sectionLabel, utf8("Configuração inicial"));
    // Ganho is the day-to-day control: bigger, brighter caption on the knob.
    trimCaption.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
    trimCaption.setColour(juce::Label::textColourId, colours::text);
    trimCaption.setJustificationType(juce::Justification::centred);

    meter.setTickVisible(false); // input level needs no moving mark

    nameEditor.setFont(juce::Font(juce::FontOptions(15.0f)));
    {
        const juce::ScopedLock lock(proc.shared->nameLock);
        nameEditor.setText(proc.shared->name, juce::dontSendNotification);
    }
    nameEditor.onTextChange = [this]
    {
        const juce::ScopedLock lock(proc.shared->nameLock);
        proc.shared->name = nameEditor.getText();
    };

    presets::fillComboBox(presetBox);
    presetBox.onChange = [this]
    {
        const int index = presetBox.getSelectedId() - 1;
        if (index < 0)
            return;
        presets::apply(*proc.shared, presets::all()[(size_t) index]);
        presetBox.setSelectedId(0, juce::dontSendNotification);
    };

    profileBox.addItem("Voz", 1);
    profileBox.addItem("Instrumento", 2);
    profileBox.addItem("Bateria", 3);
    profileAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        proc.apvts, "profile", profileBox);
    // Manually switching the profile also resets the sensitivity to that
    // profile's default (still adjustable afterwards).
    profileBox.onChange = [this]
    {
        const int index = profileBox.getSelectedItemIndex();
        if (index >= 0)
            presets::writeParam(proc.shared->sensParam,
                                dsp::profileFor(index).sensitivityDb);
    };

    trimSlider.setName("hero");
    styleKnob(trimSlider, " dB", 0.0);
    trimSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 130, 32);
    styleCompactBar(targetSlider, " dBFS", (double) dsp::kDefaultTargetDb);
    styleCompactBar(sensSlider, " dBFS", (double) dsp::kProfiles[1].sensitivityDb);

    advancedButton.setButtonText(utf8("▸  Avançado"));
    advancedButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    advancedButton.setColour(juce::TextButton::textColourOffId, colours::subtext);
    advancedButton.onClick = [this]
    {
        advancedOpen = ! advancedOpen;
        advancedButton.setButtonText(advancedOpen ? utf8("▾  Avançado") : utf8("▸  Avançado"));
        for (auto* c : { (juce::Component*) &targetCaption, (juce::Component*) &targetSlider,
                         (juce::Component*) &sensCaption, (juce::Component*) &sensSlider })
            c->setVisible(advancedOpen);
        resized();
        repaint();
        // The editor's timer follows desiredHeight() and resizes the window.
    };

    panelToggle.onClick = [this]
    {
        if (panelToggle.getToggleState())
            proc.shared->panelMode.store(true);
    };

    for (auto* c : std::initializer_list<juce::Component*> {
             &title, &nameCaption, &nameEditor, &presetCaption, &presetBox, &profileCaption,
             &profileBox, &sensCaption, &sensSlider, &meter, &outMeter, &meterCaption,
             &outMeterCaption, &targetCaption, &trimCaption, &statusStrip, &sectionLabel,
             &targetSlider, &trimSlider, &automationToggle, &riderToggle, &panelToggle,
             &advancedButton })
        addAndMakeVisible(c);

    // "Avançado" starts collapsed.
    for (auto* c : { (juce::Component*) &targetCaption, (juce::Component*) &targetSlider,
                     (juce::Component*) &sensCaption, (juce::Component*) &sensSlider })
        c->setVisible(false);
}

void ChannelView::resized()
{
    auto r = getLocalBounds().reduced(20);
    title.setBounds(r.removeFromTop(32));
    r.removeFromTop(10);

    // Identity
    auto nameRow = r.removeFromTop(48);
    auto presetCol = nameRow.removeFromRight(150);
    nameRow.removeFromRight(12);
    nameCaption.setBounds(nameRow.removeFromTop(18));
    nameEditor.setBounds(nameRow.removeFromTop(30));
    presetCaption.setBounds(presetCol.removeFromTop(18));
    presetBox.setBounds(presetCol.removeFromTop(30));
    r.removeFromTop(14);

    // Input meter first: what is arriving on the channel
    auto inRow = r.removeFromTop(26);
    meterCaption.setBounds(inRow.removeFromLeft(64));
    meter.setBounds(inRow.reduced(0, 2));
    r.removeFromTop(12);

    // Set-once configuration card ("Avançado" adds a collapsed row)
    configCard = r.removeFromTop(advancedOpen ? 252 : 220);
    auto card = configCard.reduced(14, 12);
    sectionLabel.setBounds(card.removeFromTop(16));
    card.removeFromTop(10);

    auto knobRow = card.removeFromTop(142);
    auto gainCol = knobRow.removeFromLeft(knobRow.getWidth() * 11 / 20);
    trimCaption.setBounds(gainCol.removeFromTop(22));
    trimSlider.setBounds(gainCol);
    auto rightCol = knobRow.withTrimmedLeft(10).withTrimmedBottom(30);
    profileCaption.setBounds(rightCol.removeFromTop(16));
    profileBox.setBounds(rightCol.removeFromTop(28));
    rightCol.removeFromTop(8);
    automationToggle.setBounds(rightCol.removeFromTop(26));
    riderToggle.setBounds(rightCol.removeFromTop(26));
    card.removeFromTop(8);

    advancedButton.setBounds(card.removeFromTop(20).removeFromLeft(120));
    if (advancedOpen)
    {
        card.removeFromTop(8);
        auto compactRow = card.removeFromTop(24);
        targetCaption.setBounds(compactRow.removeFromLeft(52));
        targetSlider.setBounds(compactRow.removeFromLeft(96));
        compactRow.removeFromLeft(20);
        sensCaption.setBounds(compactRow.removeFromLeft(92));
        sensSlider.setBounds(compactRow.removeFromLeft(96));
    }
    r.removeFromTop(12);

    // Output meter last: the corrected result, next to its status strip
    auto outRow = r.removeFromTop(26);
    outMeterCaption.setBounds(outRow.removeFromLeft(64));
    outMeter.setBounds(outRow.reduced(0, 2));
    r.removeFromTop(8);
    statusStrip.setBounds(r.removeFromTop(36));

    panelToggle.setBounds(r.removeFromBottom(28));
}

int ChannelView::desiredHeight() const
{
    // Fixed sections (title, name row, meters, status strip, gaps, panel
    // toggle, margins) plus the config card, which follows the disclosure.
    return 304 + (advancedOpen ? 252 : 220);
}

void ChannelView::paint(juce::Graphics& g)
{
    // Card behind the set-once configuration section.
    g.setColour(colours::card);
    g.fillRoundedRectangle(configCard.toFloat(), 10.0f);
    g.setColour(colours::cardOutline);
    g.drawRoundedRectangle(configCard.toFloat().reduced(0.5f), 10.0f, 1.0f);
}

void ChannelView::refresh()
{
    // The big readout mirrors the fader; the rider's live correction is shown
    // separately so the two never disagree.
    const float trim = proc.shared->trimDb->load();
    const float riderOffset = proc.shared->riderOffsetDb.load();
    const float target = proc.shared->targetDb->load();

    // When the host provides a track name (VST3), surface it as the
    // placeholder so an unnamed channel still shows something meaningful.
    juce::String host;
    {
        const juce::ScopedLock lock(proc.shared->nameLock);
        host = proc.shared->hostName;
    }
    const auto placeholder =
        host.trim().isEmpty() ? utf8("ex.: Vozes") : host + utf8(" (nome da track)");
    if (placeholder != lastPlaceholder)
    {
        lastPlaceholder = placeholder;
        nameEditor.setTextToShowWhenEmpty(placeholder, colours::subtext);
        nameEditor.repaint();
    }

    // Both meters use a stable target-anchored scale; on the input meter only
    // the mark moves (to target − trim, where the input should sit).
    meter.setLevelLin(proc.shared->peakPreTrim.load());
    meter.setScaleAnchorDb(target);
    meter.setTickDb(target - (trim + riderOffset));
    outMeter.setLevelLin(proc.shared->peakPostTrim.load());
    outMeter.setScaleAnchorDb(target);
    outMeter.setTickDb(target);

    const auto state = proc.shared->measuring.load() ? StatusStrip::measuring
                       : proc.shared->noSignal.load() ? StatusStrip::noSignal
                                                      : StatusStrip::normal;
    const bool riderEnabled =
        proc.shared->isAutomationOn() && proc.shared->riderOn->load() > 0.5f;
    const auto& profile = dsp::profileFor((int) proc.shared->profile->load());
    const float protect = proc.shared->protectOffsetDb.load();
    statusStrip.update(trim + riderOffset + protect, riderOffset, profile.rideRangeDb,
                       riderEnabled, state, protect);
}

//==============================================================================
PanelRow::PanelRow(std::shared_ptr<ChannelShared> channel) : shared(std::move(channel))
{
    meter.setTickVisible(false); // input level needs no moving mark
    nameLabel.setFont(juce::Font(juce::FontOptions(15.0f)));
    trimLabel.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    trimLabel.setColour(juce::Label::textColourId, colours::accent);
    statusLabel.setFont(juce::Font(juce::FontOptions(13.0f)));

    automationToggle.onClick = [this]
    { writeBoolParam(shared->automationParam, automationToggle.getToggleState()); };

    presets::fillComboBox(presetBox);
    presetBox.onChange = [this]
    {
        const int index = presetBox.getSelectedId() - 1;
        if (index < 0)
            return;
        presets::apply(*shared, presets::all()[(size_t) index]);
        presetBox.setSelectedId(0, juce::dontSendNotification);
    };

    for (auto* c : std::initializer_list<juce::Component*> {
             &nameLabel, &meter, &outMeter, &presetBox, &trimLabel, &automationToggle,
             &statusLabel })
        addAndMakeVisible(c);
}

void PanelRow::resized()
{
    auto r = getLocalBounds().reduced(4);
    nameLabel.setBounds(r.removeFromLeft(kColName));
    statusLabel.setBounds(r.removeFromRight(kColStatus));
    automationToggle.setBounds(r.removeFromRight(kColAuto).withSizeKeepingCentre(24, 24));
    trimLabel.setBounds(r.removeFromRight(kColTrim));
    presetBox.setBounds(r.removeFromRight(kColPreset).reduced(0, 8));
    auto meterArea = r.reduced(8, 5);
    meter.setBounds(meterArea.removeFromTop((meterArea.getHeight() - 2) / 2));
    meterArea.removeFromTop(2);
    outMeter.setBounds(meterArea);
}

void PanelRow::paint(juce::Graphics& g)
{
    g.setColour(colours::cardOutline.withAlpha(0.5f));
    g.fillRect(getLocalBounds().removeFromBottom(1));
}

void PanelRow::refresh()
{
    nameLabel.setText(shared->displayName(), juce::dontSendNotification);
    const float trim = shared->trimDb != nullptr ? shared->trimDb->load() : 0.0f;
    const float target =
        shared->targetDb != nullptr ? shared->targetDb->load() : dsp::kDefaultTargetDb;
    meter.setLevelLin(shared->peakPreTrim.load());
    meter.setScaleAnchorDb(target);
    meter.setTickDb(target - (trim + shared->riderOffsetDb.load()));
    outMeter.setLevelLin(shared->peakPostTrim.load());
    outMeter.setScaleAnchorDb(target);
    outMeter.setTickDb(target);
    trimLabel.setText(formatDb(trim), juce::dontSendNotification);

    const bool automationOn = shared->isAutomationOn();
    automationToggle.setToggleState(automationOn, juce::dontSendNotification);

    if (shared->measuring.load())
    {
        statusLabel.setText(utf8("medindo…"), juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, colours::info);
    }
    else if (shared->protectionActive.load())
    {
        statusLabel.setText(utf8("PROT ") + formatDb(shared->protectOffsetDb.load()),
                            juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, colours::meterHigh);
    }
    else if (shared->noSignal.load())
    {
        statusLabel.setText("sem sinal", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, colours::warning);
    }
    else if (! automationOn)
    {
        statusLabel.setText("desativado", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, colours::subtext);
    }
    else if (shared->riderOn != nullptr && shared->riderOn->load() > 0.5f)
    {
        statusLabel.setText("rider " + formatDb(shared->riderOffsetDb.load()),
                            juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, colours::accent);
    }
    else
    {
        statusLabel.setText("ok", juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, colours::subtext);
    }
}

//==============================================================================
namespace
{
    constexpr int kMiniWidth = 320;
    constexpr int kMiniRowHeight = 26;
    // Margins (10+10) + button bar (30) + gap (8): everything except the rows.
    constexpr int kMiniChromeHeight = 58;
} // namespace

MiniPanelRow::MiniPanelRow(std::shared_ptr<ChannelShared> channel) : shared(std::move(channel))
{
    nameLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    nameLabel.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(nameLabel);
    addAndMakeVisible(outMeter);
}

void MiniPanelRow::resized()
{
    auto r = getLocalBounds().reduced(2);
    nameLabel.setBounds(r.removeFromLeft(88));
    outMeter.setBounds(r.reduced(0, 4));
}

void MiniPanelRow::refresh()
{
    nameLabel.setText(shared->displayName(), juce::dontSendNotification);
    // Red name = overload protection engaged on this channel.
    nameLabel.setColour(juce::Label::textColourId,
                        shared->protectionActive.load() ? colours::meterHigh : colours::text);
    outMeter.setLevelLin(shared->peakPostTrim.load());
    const float target =
        shared->targetDb != nullptr ? shared->targetDb->load() : dsp::kDefaultTargetDb;
    outMeter.setScaleAnchorDb(target);
    outMeter.setTickDb(target);
}

MiniPanelView::MiniPanelView(AutoTrimProcessor& processor) : proc(processor)
{
    measureButton.onClick = [] { measurement::start(registry::measDurationS.load()); };

    expandButton.setButtonText(utf8("⤢"));
    expandButton.setColour(juce::TextButton::buttonColourId, colours::cardOutline);
    expandButton.setColour(juce::TextButton::textColourOffId, colours::text);
    expandButton.onClick = [this] { proc.shared->panelCompact.store(false); };

    viewport.setViewedComponent(&rowContainer, false);
    viewport.setScrollBarsShown(true, false);

    for (auto* c : std::initializer_list<juce::Component*> {
             &measureButton, &expandButton, &progressBar, &viewport })
        addAndMakeVisible(c);
    progressBar.setVisible(false);
}

void MiniPanelView::resized()
{
    auto r = getLocalBounds().reduced(10);
    auto top = r.removeFromTop(30);
    expandButton.setBounds(top.removeFromRight(30));
    top.removeFromRight(6);
    measureButton.setBounds(top);
    progressBar.setBounds(top);
    r.removeFromTop(8);
    viewport.setBounds(r);
    layoutRows();
}

void MiniPanelView::refresh()
{
    measurement::poll();
    const bool running = measurement::isRunning();
    progressValue = running ? (double) measurement::progress() : 0.0;
    measureButton.setVisible(! running);
    progressBar.setVisible(running);

    rebuildRowsIfNeeded();
    for (auto& row : rows)
        row->refresh();
}

int MiniPanelView::desiredHeight() const
{
    const int numRows = juce::jmax(1, (int) registry::channels().size());
    return juce::jmin(640, kMiniChromeHeight + numRows * kMiniRowHeight);
}

void MiniPanelView::rebuildRowsIfNeeded()
{
    auto channels = registry::channels();
    const bool changed = channels.size() != rows.size()
                         || ! std::equal(channels.begin(), channels.end(), rows.begin(),
                                         [](const auto& ch, const auto& row)
                                         { return ch == row->shared; });
    if (! changed)
        return;

    rows.clear();
    for (auto& ch : channels)
    {
        rows.push_back(std::make_unique<MiniPanelRow>(ch));
        rowContainer.addAndMakeVisible(*rows.back());
    }
    layoutRows();
}

void MiniPanelView::layoutRows()
{
    const int width = juce::jmax(0, viewport.getWidth() - viewport.getScrollBarThickness());
    rowContainer.setSize(width, (int) rows.size() * kMiniRowHeight);
    int y = 0;
    for (auto& row : rows)
    {
        row->setBounds(0, y, width, kMiniRowHeight);
        y += kMiniRowHeight;
    }
}

//==============================================================================
PanelView::PanelView(AutoTrimProcessor& processor) : proc(processor)
{
    styleTitle(title, utf8("AutoTrim — Painel de Controle"));
    styleCaption(durationCaption, utf8("Duração da medição"));
    styleCaption(maxTrimCaption, utf8("Limite de trim (±)"));
    styleCaption(listHeader,
                 utf8("Canal                                     Entrada (cima) / Saída (baixo)"));
    styleCaption(emptyLabel, utf8("Nenhum canal registrado. Insira o AutoTrim nos canais da sessão."));

    styleHSlider(durationSlider);
    durationSlider.setRange(1.0, 60.0, 0.5);
    durationSlider.setTextValueSuffix(" s");
    durationSlider.setValue(registry::measDurationS.load(), juce::dontSendNotification);
    durationSlider.onValueChange = [this]
    { registry::measDurationS.store((float) durationSlider.getValue()); };

    styleHSlider(maxTrimSlider);
    maxTrimSlider.setRange(1.0, (double) dsp::kTrimParamRangeDb, 0.5);
    maxTrimSlider.setTextValueSuffix(" dB");
    maxTrimSlider.setValue(registry::maxTrimDb.load(), juce::dontSendNotification);
    maxTrimSlider.onValueChange = [this]
    { registry::maxTrimDb.store((float) maxTrimSlider.getValue()); };

    measureButton.onClick = [] { measurement::start(registry::measDurationS.load()); };
    cancelButton.onClick = [] { measurement::cancel(); };
    cancelButton.setColour(juce::TextButton::buttonColourId, colours::cardOutline);
    cancelButton.setColour(juce::TextButton::textColourOffId, colours::text);

    compactButton.setColour(juce::TextButton::buttonColourId, colours::cardOutline);
    compactButton.setColour(juce::TextButton::textColourOffId, colours::text);
    compactButton.onClick = [this] { proc.shared->panelCompact.store(true); };

    panelToggle.setToggleState(true, juce::dontSendNotification);
    panelToggle.onClick = [this]
    {
        if (! panelToggle.getToggleState())
            proc.shared->panelMode.store(false);
    };

    viewport.setViewedComponent(&rowContainer, false);
    viewport.setScrollBarsShown(true, false);

    for (auto* c : std::initializer_list<juce::Component*> {
             &title, &durationCaption, &maxTrimCaption, &durationSlider, &maxTrimSlider,
             &measureButton, &cancelButton, &progressBar, &viewport, &emptyLabel, &panelToggle,
             &compactButton })
        addAndMakeVisible(c);
    progressBar.setVisible(false);
    cancelButton.setVisible(false);
}

void PanelView::resized()
{
    auto r = getLocalBounds().reduced(20);
    title.setBounds(r.removeFromTop(32));
    r.removeFromTop(10);

    auto controls = r.removeFromTop(52);
    auto left = controls.removeFromLeft(controls.getWidth() / 2).withTrimmedRight(12);
    auto right = controls.withTrimmedLeft(12);
    durationCaption.setBounds(left.removeFromTop(18));
    durationSlider.setBounds(left);
    maxTrimCaption.setBounds(right.removeFromTop(18));
    maxTrimSlider.setBounds(right);
    r.removeFromTop(12);

    auto actionRow = r.removeFromTop(36);
    measureButton.setBounds(actionRow);
    cancelButton.setBounds(actionRow.removeFromRight(110));
    progressBar.setBounds(actionRow.withTrimmedRight(8));
    r.removeFromTop(14);

    auto bottomRow = r.removeFromBottom(28);
    compactButton.setBounds(bottomRow.removeFromRight(140));
    panelToggle.setBounds(bottomRow);
    r.removeFromBottom(8);

    viewport.setBounds(r);
    emptyLabel.setBounds(r.removeFromTop(40));
    layoutRows();
}

void PanelView::paint(juce::Graphics&) {}

void PanelView::refresh()
{
    measurement::poll();

    const bool running = measurement::isRunning();
    progressValue = running ? (double) measurement::progress() : 0.0;
    measureButton.setVisible(! running);
    progressBar.setVisible(running);
    cancelButton.setVisible(running);

    rebuildRowsIfNeeded();
    for (auto& row : rows)
        row->refresh();
    emptyLabel.setVisible(rows.empty());
}

void PanelView::rebuildRowsIfNeeded()
{
    auto channels = registry::channels();
    const bool changed = channels.size() != rows.size()
                         || ! std::equal(channels.begin(), channels.end(), rows.begin(),
                                         [](const auto& ch, const auto& row)
                                         { return ch == row->shared; });
    if (! changed)
        return;

    rows.clear();
    for (auto& ch : channels)
    {
        rows.push_back(std::make_unique<PanelRow>(ch));
        rowContainer.addAndMakeVisible(*rows.back());
    }
    layoutRows();
}

void PanelView::layoutRows()
{
    const int width = juce::jmax(0, viewport.getWidth() - viewport.getScrollBarThickness());
    rowContainer.setSize(width, (int) rows.size() * kRowHeight);
    int y = 0;
    for (auto& row : rows)
    {
        row->setBounds(0, y, width, kRowHeight);
        y += kRowHeight;
    }
}

//==============================================================================
AutoTrimEditor::AutoTrimEditor(AutoTrimProcessor& processor)
    : AudioProcessorEditor(processor), proc(processor)
{
    setLookAndFeel(&lookAndFeel);
    rebuildView();
    startTimerHz(30);
}

AutoTrimEditor::~AutoTrimEditor()
{
    setLookAndFeel(nullptr);
}

void AutoTrimEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void AutoTrimEditor::resized()
{
    if (view != nullptr)
        view->setBounds(getLocalBounds());
}

AutoTrimEditor::ViewMode AutoTrimEditor::currentMode() const
{
    if (! proc.shared->panelMode.load())
        return ViewMode::channel;
    return proc.shared->panelCompact.load() ? ViewMode::mini : ViewMode::panel;
}

void AutoTrimEditor::timerCallback()
{
    if (currentMode() != viewMode)
        rebuildView();

    switch (viewMode)
    {
        case ViewMode::panel:
            static_cast<PanelView*>(view.get())->refresh();
            break;
        case ViewMode::mini:
        {
            auto* mini = static_cast<MiniPanelView*>(view.get());
            mini->refresh();
            // Follow the channel count so the window stays exactly as tall as
            // the list needs.
            if (getHeight() != mini->desiredHeight())
                setSize(kMiniWidth, mini->desiredHeight());
            break;
        }
        case ViewMode::channel:
        {
            auto* channel = static_cast<ChannelView*>(view.get());
            channel->refresh();
            if (getHeight() != channel->desiredHeight())
                setSize(kChannelWidth, channel->desiredHeight());
            break;
        }
    }
}

void AutoTrimEditor::rebuildView()
{
    viewMode = currentMode();
    switch (viewMode)
    {
        case ViewMode::panel:
            view = std::make_unique<PanelView>(proc);
            setSize(kPanelWidth, kPanelHeight);
            break;
        case ViewMode::mini:
        {
            auto mini = std::make_unique<MiniPanelView>(proc);
            const int h = mini->desiredHeight();
            view = std::move(mini);
            setSize(kMiniWidth, h);
            break;
        }
        case ViewMode::channel:
        {
            auto channel = std::make_unique<ChannelView>(proc);
            const int h = channel->desiredHeight();
            view = std::move(channel);
            setSize(kChannelWidth, h);
            break;
        }
    }
    addAndMakeVisible(*view);
    resized();
}
} // namespace autotrim
