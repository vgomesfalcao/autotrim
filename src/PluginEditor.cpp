#include "PluginEditor.h"

#include "Dsp.h"
#include "Measurement.h"
#include "Presets.h"

namespace autotrim
{
namespace
{
    constexpr int kChannelWidth = 460;
    constexpr int kPanelWidth = 860;
    constexpr int kPanelHeight = 616;
    // Tall enough for a usable per-row gain knob (it was nearly invisible).
    constexpr int kRowHeight = 64;
    constexpr int kColName = 160;
    constexpr int kColPreset = 120;
    constexpr int kColTrim = 148;
    constexpr int kColAuto = 70;
    constexpr int kColStatus = 130;

    juce::String formatDb(float db)
    {
        return (db >= 0.0f ? "+" : "") + juce::String(db, 1) + " dB";
    }

    void styleCaption(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(14.0f)));
        label.setColour(juce::Label::textColourId, colours::subtext);
    }

    void styleTitle(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
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

    // Small uppercase section header with letter spacing. Pass the text
    // already uppercased: juce::String::toUpperCase() leaves accented
    // characters (ç, ã) lowercase.
    void styleSection(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold))
                          .withExtraKerningFactor(0.12f));
        label.setColour(juce::Label::textColourId, colours::subtext);
    }

    // Small per-row "regulate this channel" button (panel + compact rows).
    void styleRegButton(juce::TextButton& button, const std::shared_ptr<ChannelShared>& shared)
    {
        button.setButtonText("Reg");
        button.setColour(juce::TextButton::buttonColourId, colours::cardOutline);
        button.setColour(juce::TextButton::textColourOffId, colours::text);
        button.onClick = [shared]
        {
            if (shared->measuring.load())
                measurement::cancel();
            else if (! measurement::isRunning())
                measurement::startChannel(shared, registry::measDurationS.load());
        };
    }

    void refreshRegButton(juce::TextButton& button, const ChannelShared& shared)
    {
        const bool mine = shared.measuring.load();
        button.setButtonText(mine ? "X" : "Reg");
        button.setEnabled(! measurement::isRunning() || mine);
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
    // Nonlinear meter scale, fixed and identical for every instance: the
    // "hot" reference point (0 dBFS for peak meters, the LUFS target for the
    // loudness meter) always sits at kMeterHotFrac, with the kMeterZoomSpanDb
    // below it getting most of the width — that's the region operators
    // actually watch (near clipping, or near the calibrated target).
    // Marks (the per-channel Target) move freely within this fixed scale.
    constexpr float kMeterFloorDb = -60.0f;
    constexpr float kMeterZoomSpanDb = 24.0f;
    constexpr float kMeterZoomStartFrac = 0.22f;
    constexpr float kMeterHotFrac = 0.88f;
    constexpr float kMeterOverheadDb = 6.0f; // headroom shown past the hot point

    // Peak-hold marker: freezes at the recent peak, then falls slowly enough
    // to read as a calm reference instead of a blur that vanishes into the
    // live level.
    constexpr float kPeakHoldTimeS = 2.0f;
    constexpr float kPeakHoldFallDbPerS = 6.0f;
} // namespace

void MeterBar::setLevelLin(float newLevelLin)
{
    const float newDb = dsp::gainToDb(newLevelLin);
    const double now = juce::Time::getMillisecondCounterHiRes();
    bool dirty = std::abs(newDb - levelDb) > 0.05f;
    levelDb = newDb;

    // Peak hold: grabs new maxima instantly, holds kPeakHoldTimeS, then falls
    // at kPeakHoldFallDbPerS — slow and visually distinct enough to read as a
    // calm reference instead of a blur that chases the live level.
    if (newDb >= holdDb)
    {
        if (std::abs(newDb - holdDb) > 0.05f)
            dirty = true;
        holdDb = newDb;
        holdSetMs = now;
        holdLastStepMs = now;
    }
    else if (now - holdSetMs > kPeakHoldTimeS * 1000.0)
    {
        const double dtS = juce::jmax(0.0, now - holdLastStepMs) / 1000.0;
        holdLastStepMs = now;
        const float fallen = juce::jmax(newDb, holdDb - (float) (kPeakHoldFallDbPerS * dtS));
        if (std::abs(fallen - holdDb) > 0.005f)
            dirty = true;
        holdDb = fallen;
    }

    // Numeric readout refreshes twice a second so it can actually be read.
    if (now - textSetMs > 500.0)
    {
        if (std::abs(textDb - levelDb) > 0.05f)
            dirty = true;
        textDb = levelDb;
        textSetMs = now;
    }

    if (dirty)
        repaint();
}

void MeterBar::setAgcTint(bool shouldTint)
{
    if (shouldTint != agcTint)
    {
        agcTint = shouldTint;
        repaint();
    }
}

void MeterBar::setHotDb(float newHotDb)
{
    if (std::abs(newHotDb - hotDb) > 0.05f)
    {
        hotDb = newHotDb;
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
    const float zoomStart = hotDb - kMeterZoomSpanDb;
    if (db <= kMeterFloorDb)
        return 0.0f;
    if (db <= zoomStart)
        return kMeterZoomStartFrac * (db - kMeterFloorDb) / (zoomStart - kMeterFloorDb);
    if (db <= hotDb)
        return kMeterZoomStartFrac
               + (kMeterHotFrac - kMeterZoomStartFrac) * (db - zoomStart) / kMeterZoomSpanDb;
    // Past the hot point: a fixed headroom span stays visible instead of
    // pinning instantly to the edge, so an over is legible while it happens.
    const float over = juce::jmin(db - hotDb, kMeterOverheadDb);
    return kMeterHotFrac + (1.0f - kMeterHotFrac) * over / kMeterOverheadDb;
}

void MeterBar::paint(juce::Graphics& g)
{
    auto full = getLocalBounds().toFloat();

    // Fixed dark readout gutter to the RIGHT of the coloured bar — the
    // FabFilter / iZotope Insight convention. Keeping the number off the fill
    // gives it a constant dark background, so contrast never depends on the
    // gradient colour underneath (a translucent chip over the fill read as
    // amateur). The bar's scale uses only the remaining width.
    constexpr float kGutterW = 58.0f;
    auto gutter = full.removeFromRight(kGutterW);
    auto r = full;
    r.removeFromRight(6.0f); // gap between bar and gutter

    g.setColour(colours::cardOutline);
    g.fillRoundedRectangle(r, 4.0f);

    const float frac = juce::jlimit(0.0f, 1.0f, mapDbToFrac(levelDb));
    if (frac > 0.001f)
    {
        if (agcTint)
        {
            // Yellow: the level shown includes an active AGC correction.
            g.setColour(colours::warning);
            g.fillRoundedRectangle(r.withWidth(r.getWidth() * frac), 4.0f);
        }
        else
        {
            // Teal up to the hot point (0 dBFS / the LUFS target), then amber
            // into red past it — headroom at a glance, independent of where
            // this channel's own Target mark happens to sit.
            juce::ColourGradient gradient(colours::meterLow, r.getX(), 0.0f,
                                          colours::meterHigh, r.getRight(), 0.0f, false);
            gradient.addColour(kMeterHotFrac, colours::meterLow);
            gradient.addColour(juce::jmin(kMeterHotFrac + 0.10, 0.99), colours::warning);
            g.setGradientFill(gradient);
            g.fillRoundedRectangle(r.withWidth(r.getWidth() * frac), 4.0f);
        }
    }

    // Peak-hold marker: protrudes past the bar and uses a colour that never
    // blends with the fill (bright neutral, or red above target) — it needs
    // to read as a distinct flag even when it lands right at the fill edge.
    if (holdDb > -90.0f)
    {
        const float holdFrac = juce::jlimit(0.0f, 1.0f, mapDbToFrac(holdDb));
        if (holdFrac > 0.01f)
        {
            const float holdX = r.getX() + r.getWidth() * holdFrac;
            g.setColour(holdDb > tickDb + 0.5f ? colours::meterHigh : colours::text);
            g.fillRect(holdX - 1.5f, r.getY() - 3.0f, 3.0f, r.getHeight() + 6.0f);
            g.fillRect(holdX - 3.5f, r.getY() - 3.0f, 7.0f, 2.5f); // flag head
        }
    }

    // Target mark
    if (tickVisible)
    {
        const float tickFrac = juce::jlimit(0.01f, 0.99f, mapDbToFrac(tickDb));
        const float tickX = r.getX() + r.getWidth() * tickFrac;
        g.setColour(colours::text.withAlpha(0.85f));
        g.fillRect(tickX - 1.0f, r.getY(), 2.0f, r.getHeight());
    }

    // Numeric readout in its own gutter: constant dark background, so the
    // value is legible at silence, at target and clipping alike. Turns amber
    // once the level is past the hot point (approaching clip) as a quiet cue.
    const auto text = textDb <= -90.0f
                          ? "-inf" + unit
                          : (textDb >= 0.0f ? "+" : "") + juce::String(textDb, 1) + unit;
    g.setColour(colours::background);
    g.fillRoundedRectangle(gutter, 4.0f);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.setColour(textDb >= hotDb - 0.05f ? colours::warning : colours::text);
    g.drawText(text, gutter.reduced(6.0f, 0.0f), juce::Justification::centredRight);
}

//==============================================================================
void StatusStrip::update(float riderOffsetDb, float rideRangeDb, bool riderEnabled,
                         State newState, float protectDb, float agcDb)
{
    const bool changed = std::abs(riderOffsetDb - offset) > 0.05f
                         || std::abs(rideRangeDb - range) > 0.05f || riderEnabled != riderOn
                         || newState != state || std::abs(protectDb - protect) > 0.05f
                         || std::abs(agcDb - agc) > 0.05f;
    if (! changed)
        return;
    offset = riderOffsetDb;
    range = juce::jmax(0.1f, rideRangeDb);
    riderOn = riderEnabled;
    state = newState;
    protect = protectDb;
    agc = agcDb;
    repaint();
}

void StatusStrip::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();

    // Clip Guard engaged: red badge, impossible to miss.
    if (protect < -0.05f)
    {
        auto badge = r.removeFromRight(118.0f);
        g.setColour(colours::meterHigh.withAlpha(0.18f));
        g.fillRoundedRectangle(badge, 8.0f);
        g.setColour(colours::meterHigh);
        g.drawRoundedRectangle(badge.reduced(0.5f), 8.0f, 1.0f);
        g.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
        g.drawText(utf8("CLIP ") + formatDb(protect), badge.reduced(8.0f, 0.0f),
                   juce::Justification::centred);
        r.removeFromRight(10.0f);
    }

    // Active AGC correction: yellow badge with the temporary offset.
    if (std::abs(agc) > 0.05f)
    {
        auto badge = r.removeFromRight(112.0f);
        g.setColour(colours::warning.withAlpha(0.16f));
        g.fillRoundedRectangle(badge, 8.0f);
        g.setColour(colours::warning);
        g.drawRoundedRectangle(badge.reduced(0.5f), 8.0f, 1.0f);
        g.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
        g.drawText("AGC " + formatDb(agc), badge.reduced(8.0f, 0.0f),
                   juce::Justification::centred);
        r.removeFromRight(10.0f);
    }

    if (state == armed || state == measuring)
    {
        g.setColour(colours::info);
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText(state == armed ? utf8("aguardando sinal…") : utf8("medindo…"), r,
                   juce::Justification::centredLeft);
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
    g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold))
                  .withExtraKerningFactor(0.12f));
    g.drawText("RIDER", r.removeFromLeft(52.0f), juce::Justification::centredLeft);

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
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText(riderOn ? formatDb(offset) : juce::String("off"), valueArea,
               juce::Justification::centredRight);
}

//==============================================================================
ChannelView::ChannelView(AutoTrimProcessor& processor)
    : proc(processor),
      targetAttachment(proc.apvts, "target", targetSlider),
      trimAttachment(proc.apvts, "trim", trimSlider),
      sensAttachment(proc.apvts, "sens", sensSlider),
      speedAttachment(proc.apvts, "speed", speedSlider),
      agcTimeAttachment(proc.apvts, "agctime", agcTimeSlider),
      agcRangeAttachment(proc.apvts, "agcrange", agcRangeSlider),
      automationAttachment(proc.apvts, "automation", automationToggle),
      riderAttachment(proc.apvts, "rider", riderToggle),
      clipGuardAttachment(proc.apvts, "clipguard", clipGuardToggle),
      agcAttachment(proc.apvts, "agc", agcToggle)
{
    styleTitle(title, "AutoTrim");
    styleCaption(nameCaption, "Nome do canal");
    styleCaption(presetCaption, "Preset");
    styleCaption(profileCaption, "Perfil do rider");
    styleCaption(sensCaption, "Sensibilidade");
    styleCaption(speedCaption, "Velocidade do rider");
    styleCaption(agcTimeCaption, "Tempo do AGC");
    styleCaption(agcRangeCaption, utf8("Máx. do AGC"));
    styleCaption(meterCaption, "Entrada");
    styleCaption(outMeterCaption, utf8("Saída"));
    styleCaption(targetCaption, "Target");
    styleCaption(trimCaption, "Ganho");
    styleCaption(sensCaption, "Sensibilidade");
    styleSection(sectionLabel, utf8("CONFIGURAÇÃO INICIAL"));
    // Ganho is the day-to-day control: bigger, brighter caption on the knob.
    trimCaption.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    trimCaption.setColour(juce::Label::textColourId, colours::text);
    trimCaption.setJustificationType(juce::Justification::centred);

    meter.setTickVisible(false); // input level needs no moving mark

    nameEditor.setFont(juce::Font(juce::FontOptions(17.0f)));
    nameEditor.setJustification(juce::Justification::centredLeft);
    {
        const juce::ScopedLock lock(proc.shared->nameLock);
        nameEditor.setText(proc.shared->name, juce::dontSendNotification);
        nameEditor.applyFontToAllText(juce::Font(juce::FontOptions(17.0f)));
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
        {
            presets::writeParam(proc.shared->sensParam,
                                dsp::profileFor(index).sensitivityDb);
            presets::writeParam(proc.shared->speedParam, dsp::profileFor(index).upDbPerS);
        }
    };

    trimSlider.setName("hero");
    styleKnob(trimSlider, " dB", 0.0);
    trimSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 170, 48);
    styleCompactBar(targetSlider, " dBFS", (double) dsp::kDefaultTargetDb);
    styleCompactBar(sensSlider, " dBFS", (double) dsp::kProfiles[1].sensitivityDb);
    styleCompactBar(speedSlider, " dB/s", (double) dsp::kProfiles[1].upDbPerS);
    styleCompactBar(agcTimeSlider, " s", (double) dsp::kAgcHoldS);
    styleCompactBar(agcRangeSlider, " dB", (double) dsp::kAgcRangeDb);

    measureButton.onClick = [this]
    {
        if (proc.shared->measuring.load())
            measurement::cancel();
        else if (! measurement::isRunning())
            measurement::startChannel(proc.shared, registry::measDurationS.load());
    };

    advancedButton.setButtonText(utf8("▸  Avançado"));
    advancedButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    advancedButton.setColour(juce::TextButton::textColourOffId, colours::subtext);
    advancedButton.onClick = [this]
    {
        advancedOpen = ! advancedOpen;
        advancedButton.setButtonText(advancedOpen ? utf8("▾  Avançado") : utf8("▸  Avançado"));
        for (auto* c : { (juce::Component*) &targetCaption, (juce::Component*) &targetSlider,
                         (juce::Component*) &sensCaption, (juce::Component*) &sensSlider,
                         (juce::Component*) &speedCaption, (juce::Component*) &speedSlider,
                         (juce::Component*) &agcTimeCaption, (juce::Component*) &agcTimeSlider,
                         (juce::Component*) &agcRangeCaption, (juce::Component*) &agcRangeSlider,
                         (juce::Component*) &clipGuardToggle, (juce::Component*) &agcToggle })
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
             &profileBox, &sensCaption, &sensSlider, &speedCaption, &speedSlider,
             &agcTimeCaption, &agcTimeSlider, &agcRangeCaption, &agcRangeSlider, &meter,
             &outMeter, &meterCaption,
             &outMeterCaption, &targetCaption, &trimCaption, &statusStrip, &sectionLabel,
             &targetSlider, &trimSlider, &automationToggle, &riderToggle, &clipGuardToggle,
             &agcToggle, &panelToggle, &advancedButton, &measureButton })
        addAndMakeVisible(c);

    // "Avançado" starts collapsed.
    for (auto* c : { (juce::Component*) &targetCaption, (juce::Component*) &targetSlider,
                     (juce::Component*) &sensCaption, (juce::Component*) &sensSlider,
                     (juce::Component*) &speedCaption, (juce::Component*) &speedSlider,
                     (juce::Component*) &agcTimeCaption, (juce::Component*) &agcTimeSlider,
                     (juce::Component*) &agcRangeCaption, (juce::Component*) &agcRangeSlider,
                     (juce::Component*) &clipGuardToggle, (juce::Component*) &agcToggle })
        c->setVisible(false);
}

void ChannelView::resized()
{
    auto r = getLocalBounds().reduced(20);
    title.setBounds(r.removeFromTop(30));
    r.removeFromTop(12);

    // Identity
    auto nameRow = r.removeFromTop(56);
    auto presetCol = nameRow.removeFromRight(160);
    nameRow.removeFromRight(12);
    nameCaption.setBounds(nameRow.removeFromTop(20));
    nameEditor.setBounds(nameRow.removeFromTop(34));
    presetCaption.setBounds(presetCol.removeFromTop(20));
    presetBox.setBounds(presetCol.removeFromTop(34));
    r.removeFromTop(16);

    // Input meter first: what is arriving on the channel
    auto inRow = r.removeFromTop(30);
    meterCaption.setBounds(inRow.removeFromLeft(72));
    meter.setBounds(inRow.reduced(0, 3));
    r.removeFromTop(12);

    measureButton.setBounds(r.removeFromTop(34));
    r.removeFromTop(14);

    // Set-once configuration card ("Avançado" adds four collapsed rows)
    configCard = r.removeFromTop(advancedOpen ? 422 : 282);
    auto card = configCard.reduced(16, 14);
    sectionLabel.setBounds(card.removeFromTop(18));
    card.removeFromTop(12);

    auto knobRow = card.removeFromTop(190);
    auto gainCol = knobRow.removeFromLeft(knobRow.getWidth() * 11 / 20);
    trimCaption.setBounds(gainCol.removeFromTop(24));
    trimSlider.setBounds(gainCol);
    auto rightCol = knobRow.withTrimmedLeft(10);
    profileCaption.setBounds(rightCol.removeFromTop(18));
    profileBox.setBounds(rightCol.removeFromTop(32));
    rightCol.removeFromTop(8);
    automationToggle.setBounds(rightCol.removeFromTop(28));
    riderToggle.setBounds(rightCol.removeFromTop(28));
    card.removeFromTop(12);

    advancedButton.setBounds(card.removeFromTop(22).removeFromLeft(130));
    if (advancedOpen)
    {
        card.removeFromTop(10);
        auto compactRow = card.removeFromTop(26);
        targetCaption.setBounds(compactRow.removeFromLeft(56));
        targetSlider.setBounds(compactRow.removeFromLeft(100));
        compactRow.removeFromLeft(20);
        sensCaption.setBounds(compactRow.removeFromLeft(96));
        sensSlider.setBounds(compactRow.removeFromLeft(100));
        card.removeFromTop(8);
        auto speedRow = card.removeFromTop(26);
        speedCaption.setBounds(speedRow.removeFromLeft(132));
        speedSlider.setBounds(speedRow.removeFromLeft(100));
        card.removeFromTop(8);
        auto agcTimeRow = card.removeFromTop(26);
        agcTimeCaption.setBounds(agcTimeRow.removeFromLeft(96));
        agcTimeSlider.setBounds(agcTimeRow.removeFromLeft(92));
        agcTimeRow.removeFromLeft(14);
        agcRangeCaption.setBounds(agcTimeRow.removeFromLeft(88));
        agcRangeSlider.setBounds(agcTimeRow.removeFromLeft(92));
        card.removeFromTop(8);
        auto toggleRow = card.removeFromTop(28);
        clipGuardToggle.setBounds(toggleRow.removeFromLeft(toggleRow.getWidth() / 2));
        agcToggle.setBounds(toggleRow);
    }
    r.removeFromTop(14);

    // Output meter last: the corrected result, next to its status strip
    auto outRow = r.removeFromTop(30);
    outMeterCaption.setBounds(outRow.removeFromLeft(72));
    outMeter.setBounds(outRow.reduced(0, 3));
    r.removeFromTop(10);
    statusStrip.setBounds(r.removeFromTop(40));

    panelToggle.setBounds(r.removeFromBottom(30));
}

int ChannelView::desiredHeight() const
{
    // Fixed sections (title, name row, meters, status strip, gaps, panel
    // toggle, margins) plus the config card, which follows the disclosure.
    return 382 + (advancedOpen ? 422 : 282);
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
    // Per-channel measurements are polled here too, so they finish even when
    // no panel window is open.
    measurement::poll();
    juce::String measureText = "Regular ganho";
    if (proc.shared->measuring.load())
    {
        measureText = "Cancelar";
        if (proc.shared->measStarted.load()
            && dsp::profileFor((int) proc.shared->profile->load()).hitBased)
            measureText += " (" + juce::String((int) proc.shared->measHitCount.load()) + "/"
                           + juce::String(registry::measHits.load()) + " hits)";
    }
    measureButton.setButtonText(measureText);
    measureButton.setEnabled(! measurement::isRunning() || proc.shared->measuring.load());

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

    // Both meters share the same fixed 0 dBFS-anchored scale; the Target
    // mark moves freely within it (on the input meter it sits at
    // target − trim, where the input should land).
    meter.setLevelLin(proc.shared->peakPreTrim.load());
    meter.setHotDb(0.0f);
    meter.setTickDb(target - (trim + riderOffset));
    outMeter.setLevelLin(proc.shared->peakPostTrim.load());
    outMeter.setHotDb(0.0f);
    outMeter.setTickDb(target);

    // Yellow = an AGC correction is acting: output meter, knob and chip.
    const float agc = proc.shared->agcOffsetDb.load();
    const bool agcActive = std::abs(agc) > 0.05f;
    outMeter.setAgcTint(agcActive);
    const auto knobFill = agcActive ? colours::warning : colours::accent;
    if (trimSlider.findColour(juce::Slider::rotarySliderFillColourId) != knobFill)
    {
        trimSlider.setColour(juce::Slider::rotarySliderFillColourId, knobFill);
        trimSlider.repaint();
    }

    const auto state = proc.shared->measuring.load()
                           ? (proc.shared->measStarted.load() ? StatusStrip::measuring
                                                              : StatusStrip::armed)
                       : proc.shared->noSignal.load() ? StatusStrip::noSignal
                                                      : StatusStrip::normal;
    const bool riderEnabled =
        proc.shared->isAutomationOn() && proc.shared->riderOn->load() > 0.5f;
    const auto& profile = dsp::profileFor((int) proc.shared->profile->load());
    const float protect = proc.shared->protectOffsetDb.load();
    statusStrip.update(riderOffset, profile.rideRangeDb, riderEnabled, state, protect, agc);
}

//==============================================================================
PanelRow::PanelRow(std::shared_ptr<ChannelShared> channel) : shared(std::move(channel))
{
    meter.setTickVisible(false); // input level needs no moving mark
    nameLabel.setFont(juce::Font(juce::FontOptions(16.0f)));
    statusLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    profileLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
    profileLabel.setColour(juce::Label::textColourId, colours::subtext);

    for (auto* b : { &moveUpButton, &moveDownButton })
    {
        b->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        b->setColour(juce::TextButton::textColourOffId, colours::subtext);
        b->setColour(juce::TextButton::textColourOnId, colours::subtext);
    }
    moveUpButton.onClick = [this] { if (onMoveUp) onMoveUp(); };
    moveDownButton.onClick = [this] { if (onMoveDown) onMoveDown(); };

    // Manual gain knob for this channel, writing straight to its parameter.
    trimKnob.setName("row");
    trimKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    trimKnob.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 18);
    trimKnob.setRange(-dsp::kTrimParamRangeDb, dsp::kTrimParamRangeDb, 0.1);
    trimKnob.setTextValueSuffix(" dB");
    trimKnob.setMouseDragSensitivity(400);
    trimKnob.setDoubleClickReturnValue(true, 0.0);
    trimKnob.onValueChange = [this]
    { presets::writeParam(shared->trimParam, (float) trimKnob.getValue()); };

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

    styleRegButton(regButton, shared);

    for (auto* c : std::initializer_list<juce::Component*> {
             &nameLabel, &profileLabel, &meter, &outMeter, &regButton, &presetBox, &trimKnob,
             &automationToggle, &statusLabel, &moveUpButton, &moveDownButton })
        addAndMakeVisible(c);
}

void PanelRow::resized()
{
    auto r = getLocalBounds().reduced(4);
    auto moveCol = r.removeFromLeft(18);
    moveUpButton.setBounds(moveCol.removeFromTop(moveCol.getHeight() / 2));
    moveDownButton.setBounds(moveCol);
    auto nameCol = r.removeFromLeft(kColName);
    nameLabel.setBounds(nameCol.removeFromTop(nameCol.getHeight() * 3 / 5));
    profileLabel.setBounds(nameCol);
    statusLabel.setBounds(r.removeFromRight(kColStatus));
    automationToggle.setBounds(r.removeFromRight(kColAuto).withSizeKeepingCentre(24, 24));
    trimKnob.setBounds(r.removeFromRight(kColTrim).reduced(0, 2));
    presetBox.setBounds(r.removeFromRight(kColPreset).reduced(0, 8));
    auto meterArea = r.reduced(8, 5);
    regButton.setBounds(meterArea.removeFromRight(46).reduced(2, 5));
    meterArea.removeFromRight(6);
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
    const int profileIndex = shared->profile != nullptr ? (int) shared->profile->load() : 1;
    profileLabel.setText(dsp::kProfileNames[juce::jlimit(0, dsp::kNumProfiles - 1, profileIndex)],
                         juce::dontSendNotification);
    moveUpButton.setEnabled(onMoveUp != nullptr);
    moveDownButton.setEnabled(onMoveDown != nullptr);
    const float trim = shared->trimDb != nullptr ? shared->trimDb->load() : 0.0f;
    const float target =
        shared->targetDb != nullptr ? shared->targetDb->load() : dsp::kDefaultTargetDb;
    meter.setLevelLin(shared->peakPreTrim.load());
    meter.setHotDb(0.0f);
    meter.setTickDb(target - (trim + shared->riderOffsetDb.load()));
    outMeter.setLevelLin(shared->peakPostTrim.load());
    outMeter.setHotDb(0.0f);
    outMeter.setTickDb(target);
    // Don't fight the user's drag; otherwise mirror the parameter.
    if (! trimKnob.isMouseButtonDown())
        trimKnob.setValue(trim, juce::dontSendNotification);

    // Yellow = AGC correction acting on this channel.
    const float agcDb = shared->agcOffsetDb.load();
    const bool agcActive = std::abs(agcDb) > 0.05f;
    outMeter.setAgcTint(agcActive);
    const auto knobFill = agcActive ? colours::warning : colours::accent;
    if (trimKnob.findColour(juce::Slider::rotarySliderFillColourId) != knobFill)
    {
        trimKnob.setColour(juce::Slider::rotarySliderFillColourId, knobFill);
        trimKnob.repaint();
    }

    const bool automationOn = shared->isAutomationOn();
    automationToggle.setToggleState(automationOn, juce::dontSendNotification);
    refreshRegButton(regButton, *shared);

    if (shared->measuring.load())
    {
        juce::String text = utf8("aguardando sinal…");
        if (shared->measStarted.load())
        {
            text = utf8("medindo…");
            if (dsp::profileFor((int) shared->profile->load()).hitBased)
                text += " " + juce::String((int) shared->measHitCount.load()) + "/"
                        + juce::String(registry::measHits.load()) + " hits";
        }
        statusLabel.setText(text, juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, colours::info);
    }
    else if (shared->protectionActive.load())
    {
        statusLabel.setText(utf8("CLIP ") + formatDb(shared->protectOffsetDb.load()),
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
    else if (agcActive)
    {
        statusLabel.setText("AGC " + formatDb(agcDb), juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, colours::warning);
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
    // Margins (10+10) + button bar (30) + LUFS row (6+20+6): all but the rows.
    constexpr int kMiniChromeHeight = 82;
} // namespace

MiniPanelRow::MiniPanelRow(std::shared_ptr<ChannelShared> channel) : shared(std::move(channel))
{
    nameLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    nameLabel.setColour(juce::Label::textColourId, colours::text);
    // Read-only Ganho readout — piloting the show is glance-only here, no
    // knob to fight with (that lives in the full panel or the channel view).
    gainLabel.setFont(juce::Font(juce::FontOptions(12.5f, juce::Font::bold)));
    gainLabel.setJustificationType(juce::Justification::centredRight);
    styleRegButton(regButton, shared);
    addAndMakeVisible(nameLabel);
    addAndMakeVisible(gainLabel);
    addAndMakeVisible(outMeter);
    addAndMakeVisible(regButton);
}

void MiniPanelRow::resized()
{
    auto r = getLocalBounds().reduced(2);
    nameLabel.setBounds(r.removeFromLeft(72));
    regButton.setBounds(r.removeFromRight(42).reduced(0, 2));
    r.removeFromRight(6);
    gainLabel.setBounds(r.removeFromLeft(52));
    r.removeFromLeft(4);
    outMeter.setBounds(r.reduced(0, 4));
}

void MiniPanelRow::refresh()
{
    nameLabel.setText(shared->displayName(), juce::dontSendNotification);
    refreshRegButton(regButton, *shared);
    // Red name = overload protection engaged on this channel.
    nameLabel.setColour(juce::Label::textColourId,
                        shared->protectionActive.load() ? colours::meterHigh : colours::text);
    const bool agcActive = std::abs(shared->agcOffsetDb.load()) > 0.05f;
    gainLabel.setText(
        formatDb(shared->trimDb != nullptr ? shared->trimDb->load() : 0.0f),
        juce::dontSendNotification);
    gainLabel.setColour(juce::Label::textColourId, agcActive ? colours::warning : colours::text);
    outMeter.setLevelLin(shared->peakPostTrim.load());
    outMeter.setAgcTint(agcActive);
    const float target =
        shared->targetDb != nullptr ? shared->targetDb->load() : dsp::kDefaultTargetDb;
    outMeter.setHotDb(0.0f);
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
    progressBar.setTextToDisplay(utf8("medindo…"));

    styleCaption(lufsCaption, "LUFS");
    lufsMeter.setUnit(" LUFS");
    lufsMeter.setHotDb(dsp::kLufsTargetDb);
    lufsMeter.setTickDb(dsp::kLufsTargetDb);

    for (auto* c : std::initializer_list<juce::Component*> {
             &measureButton, &expandButton, &progressBar, &viewport, &lufsCaption, &lufsMeter })
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
    r.removeFromTop(6);
    auto lufsRow = r.removeFromTop(20);
    lufsCaption.setBounds(lufsRow.removeFromLeft(44));
    lufsMeter.setBounds(lufsRow);
    r.removeFromTop(6);
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

    lufsMeter.setLevelLin(dsp::dbToGain(proc.shared->lufsShort.load()));

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
    styleCaption(durationCaption, utf8("Medição — instrumentos (s)"));
    styleCaption(hitsCaption, utf8("Medição — bateria (batidas)"));
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

    styleHSlider(hitsSlider);
    hitsSlider.setRange((double) dsp::kMeasHitsMin, (double) dsp::kMeasHitsMax, 1.0);
    hitsSlider.setValue(registry::measHits.load(), juce::dontSendNotification);
    hitsSlider.onValueChange = [this]
    { registry::measHits.store((int) hitsSlider.getValue()); };

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

    progressBar.setTextToDisplay(utf8("medindo… (canais prontos)"));

    styleCaption(lufsCaption, "LUFS (master)");
    lufsMeter.setUnit(" LUFS");
    lufsMeter.setHotDb(dsp::kLufsTargetDb);
    lufsMeter.setTickDb(dsp::kLufsTargetDb);
    addAndMakeVisible(lufsCaption);
    addAndMakeVisible(lufsMeter);

    panelToggle.setToggleState(true, juce::dontSendNotification);
    panelToggle.onClick = [this]
    {
        if (! panelToggle.getToggleState())
            proc.shared->panelMode.store(false);
    };

    viewport.setViewedComponent(&rowContainer, false);
    viewport.setScrollBarsShown(true, false);

    for (auto* c : std::initializer_list<juce::Component*> {
             &title, &durationCaption, &hitsCaption, &maxTrimCaption, &durationSlider,
             &hitsSlider, &maxTrimSlider, &measureButton, &cancelButton, &progressBar, &viewport,
             &emptyLabel, &panelToggle, &compactButton })
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
    const int colW = controls.getWidth() / 3;
    auto colA = controls.removeFromLeft(colW).withTrimmedRight(10);
    auto colB = controls.removeFromLeft(colW).withTrimmedRight(10).withTrimmedLeft(2);
    auto colC = controls.withTrimmedLeft(10);
    durationCaption.setBounds(colA.removeFromTop(18));
    durationSlider.setBounds(colA);
    hitsCaption.setBounds(colB.removeFromTop(18));
    hitsSlider.setBounds(colB);
    maxTrimCaption.setBounds(colC.removeFromTop(18));
    maxTrimSlider.setBounds(colC);
    r.removeFromTop(12);

    auto actionRow = r.removeFromTop(36);
    measureButton.setBounds(actionRow);
    cancelButton.setBounds(actionRow.removeFromRight(110));
    progressBar.setBounds(actionRow.withTrimmedRight(8));
    r.removeFromTop(10);

    auto lufsRow = r.removeFromTop(24);
    lufsCaption.setBounds(lufsRow.removeFromLeft(110));
    lufsMeter.setBounds(lufsRow.reduced(0, 1));
    r.removeFromTop(12);

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
    lufsMeter.setLevelLin(dsp::dbToGain(proc.shared->lufsShort.load()));

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
    for (size_t i = 0; i < channels.size(); ++i)
    {
        auto row = std::make_unique<PanelRow>(channels[i]);
        if (i > 0)
            row->onMoveUp = [this, i] { swapOrder(i, i - 1); };
        if (i + 1 < channels.size())
            row->onMoveDown = [this, i] { swapOrder(i, i + 1); };
        rowContainer.addAndMakeVisible(*row);
        rows.push_back(std::move(row));
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

void PanelView::swapOrder(size_t a, size_t b)
{
    auto channels = registry::channels();
    if (a >= channels.size() || b >= channels.size())
        return;
    const int orderA = channels[a]->order.load();
    const int orderB = channels[b]->order.load();
    channels[a]->order.store(orderB);
    channels[b]->order.store(orderA);
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
    // The view is created after setLookAndFeel, so slider textboxes were
    // built while the default LookAndFeel was in effect; force a re-sync so
    // the fonts and justification from our LookAndFeel actually apply.
    view->sendLookAndFeelChange();
    resized();
}
} // namespace autotrim
