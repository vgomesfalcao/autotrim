// Dark, flat look shared by both views.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace autotrim
{
namespace colours
{
    const juce::Colour background { 0xff131720 };
    const juce::Colour card { 0xff1b212c };
    const juce::Colour cardOutline { 0xff2a3140 };
    const juce::Colour accent { 0xff43c6ac };
    const juce::Colour text { 0xffe6eaf0 };
    const juce::Colour subtext { 0xff8a93a5 };
    const juce::Colour warning { 0xffe5b84b };
    const juce::Colour info { 0xff5aa7e8 };
    const juce::Colour meterLow { 0xff43c6ac };
    const juce::Colour meterHigh { 0xffe0564f };
} // namespace colours

class AutoTrimLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AutoTrimLookAndFeel()
    {
        auto scheme = getDarkColourScheme();
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::windowBackground, colours::background);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::widgetBackground, colours::card);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::defaultText, colours::text);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::highlightedFill, colours::accent);
        setColourScheme(scheme);

        setColour(juce::Slider::thumbColourId, colours::accent);
        setColour(juce::Slider::rotarySliderFillColourId, colours::accent);
        setColour(juce::Slider::rotarySliderOutlineColourId, colours::cardOutline);
        setColour(juce::Slider::trackColourId, colours::accent.withAlpha(0.6f));
        setColour(juce::Slider::backgroundColourId, colours::cardOutline);
        setColour(juce::Slider::textBoxTextColourId, colours::text);
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxHighlightColourId, colours::accent.withAlpha(0.3f));
        setColour(juce::Label::textColourId, colours::text);
        setColour(juce::ToggleButton::textColourId, colours::text);
        setColour(juce::ToggleButton::tickColourId, colours::accent);
        setColour(juce::ToggleButton::tickDisabledColourId, colours::subtext);
        setColour(juce::TextButton::buttonColourId, colours::accent);
        setColour(juce::TextButton::textColourOffId, juce::Colour(0xff0e1218));
        setColour(juce::TextEditor::backgroundColourId, colours::card);
        setColour(juce::TextEditor::textColourId, colours::text);
        setColour(juce::TextEditor::outlineColourId, colours::cardOutline);
        setColour(juce::TextEditor::focusedOutlineColourId, colours::accent);
        setColour(juce::ComboBox::backgroundColourId, colours::card);
        setColour(juce::ComboBox::textColourId, colours::text);
        setColour(juce::ComboBox::outlineColourId, colours::cardOutline);
        setColour(juce::ComboBox::arrowColourId, colours::accent);
        setColour(juce::PopupMenu::backgroundColourId, colours::card);
        setColour(juce::PopupMenu::textColourId, colours::text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, colours::accent.withAlpha(0.25f));
        setColour(juce::PopupMenu::highlightedTextColourId, colours::text);
        setColour(juce::ProgressBar::backgroundColourId, colours::cardOutline);
        setColour(juce::ProgressBar::foregroundColourId, colours::info);
        setColour(juce::ScrollBar::thumbColourId, colours::cardOutline.brighter(0.4f));
    }

    juce::Font getTextButtonFont(juce::TextButton&, int) override
    {
        return juce::Font(juce::FontOptions(15.0f, juce::Font::bold));
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override
    {
        return juce::Font(juce::FontOptions(16.0f));
    }

    juce::Font getPopupMenuFont() override
    {
        return juce::Font(juce::FontOptions(16.0f));
    }

    // Rotary sliders get a big bold readout — the value must be legible at a
    // glance; linear sliders keep the default compact textbox.
    juce::Label* createSliderTextBox(juce::Slider& slider) override
    {
        auto* label = LookAndFeel_V4::createSliderTextBox(slider);
        if (slider.getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag)
        {
            // The "hero" knob (Ganho) gets a display-size readout: it is the
            // one number this plugin exists to show. Panel-row knobs stay
            // compact.
            const float size = slider.getName() == "hero"  ? 36.0f
                               : slider.getName() == "row" ? 13.0f
                                                           : 20.0f;
            label->setFont(juce::Font(juce::FontOptions(size, juce::Font::bold)));
            label->setColour(juce::Label::textColourId, colours::accent);
            label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            label->setJustificationType(juce::Justification::centred);
        }
        return label;
    }

    // Clean thin-arc rotary instead of JUCE's default filled pie wedge (which
    // reads as an ugly two-tone blob at small sizes): a full background ring,
    // an accent value arc, and a short pointer. Works at any diameter.
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider& slider) override
    {
        const auto bounds =
            juce::Rectangle<int>(x, y, width, height).toFloat().reduced(2.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f;
        const float cx = bounds.getCentreX(), cy = bounds.getCentreY();
        const float thickness = juce::jmax(2.5f, radius * 0.22f);
        const float angle = startAngle + sliderPos * (endAngle - startAngle);
        const auto fill = slider.findColour(juce::Slider::rotarySliderFillColourId);

        juce::Path bg;
        bg.addCentredArc(cx, cy, radius, radius, 0.0f, startAngle, endAngle, true);
        g.setColour(colours::cardOutline);
        g.strokePath(bg, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));

        if (sliderPos > 0.0f)
        {
            juce::Path arc;
            arc.addCentredArc(cx, cy, radius, radius, 0.0f, startAngle, angle, true);
            g.setColour(fill);
            g.strokePath(arc, juce::PathStrokeType(thickness, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }

        // Pointer from just inside the ring to the centre hub.
        const float inner = radius - thickness;
        juce::Point<float> tip(cx + std::sin(angle) * inner, cy - std::cos(angle) * inner);
        juce::Point<float> hub(cx + std::sin(angle) * (inner * 0.35f),
                               cy - std::cos(angle) * (inner * 0.35f));
        g.setColour(colours::text);
        g.drawLine({ hub, tip }, juce::jmax(1.5f, thickness * 0.5f));
    }

    // The hero knob's readout is drawn as a chip: rounded card, accent bold
    // value — the single gain display of the plugin.
    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        auto* slider = dynamic_cast<juce::Slider*>(label.getParentComponent());
        if (slider != nullptr && slider->getName() == "hero" && ! label.isBeingEdited())
        {
            auto r = label.getLocalBounds().toFloat();
            g.setColour(colours::card);
            g.fillRoundedRectangle(r, 9.0f);
            g.setColour(colours::cardOutline);
            g.drawRoundedRectangle(r.reduced(0.5f), 9.0f, 1.0f);
            // Follows the knob's fill colour: teal normally, yellow while an
            // AGC correction is acting.
            g.setColour(slider->findColour(juce::Slider::rotarySliderFillColourId));
            // Explicit display size: the label's own font can lag behind
            // look-and-feel changes, so never trust it here.
            g.setFont(juce::Font(juce::FontOptions(36.0f, juce::Font::bold)));
            g.drawText(label.getText(), label.getLocalBounds().reduced(8, 0),
                       juce::Justification::centred);
            return;
        }
        LookAndFeel_V4::drawLabel(g, label);
    }
};
} // namespace autotrim
