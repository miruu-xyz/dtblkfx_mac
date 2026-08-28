/*
  ==============================================================================

    RetroLookAndFeel.h

    The Windows 95 look from the Figma redesign (node 6-669). Colours and
    typefaces all come from DesignPalette.h -- nothing here should name a
    colour or a font size of its own.

    Phase 6.1 restyles the controls that exist today. The FX row's own
    graphics (the amp wedge, the frequency window, the drag handles) are not
    LookAndFeel work and land in 6.6.

  ==============================================================================
*/

#pragma once

#include "DesignPalette.h"
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

class RetroLookAndFeel : public juce::LookAndFeel_V4 {
public:
  RetroLookAndFeel()
  {
    using namespace design::colour;

    setColour(juce::ResizableWindow::backgroundColourId, baseGrey);

    setColour(juce::Slider::thumbColourId, accent);
    setColour(juce::Slider::trackColourId, accentBright);
    setColour(juce::Slider::backgroundColourId, fieldFill);
    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);

    setColour(juce::ComboBox::backgroundColourId, fieldFill);
    setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::arrowColourId, text);
    setColour(juce::ComboBox::textColourId, text);

    setColour(juce::TextButton::buttonColourId, baseGrey);
    setColour(juce::TextButton::buttonOnColourId, accent);
    setColour(juce::TextButton::textColourOffId, text);
    setColour(juce::TextButton::textColourOnId, bevelLight);

    setColour(juce::Label::textColourId, text);
    setColour(juce::GroupComponent::textColourId, text);
    setColour(juce::GroupComponent::outlineColourId, bevelDarkSoft);

    setColour(juce::PopupMenu::backgroundColourId, bevelLight);
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accentBright);
    setColour(juce::PopupMenu::highlightedTextColourId, bevelLight);
    setColour(juce::PopupMenu::headerTextColourId, textFaint);

    setColour(juce::ToggleButton::textColourId, textDim);
    setColour(juce::ToggleButton::tickColourId, accent);
    setColour(juce::ToggleButton::tickDisabledColourId, bevelDarkSoft);

    setColour(juce::TextEditor::backgroundColourId, bevelLight);
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::highlightColourId, accentGlow);
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
  }

  /** The design's three cuts. Public so components can pick one explicitly --
      a heading is not a label and the LookAndFeel cannot guess which is which. */
  juce::SharedResourcePointer<design::FontStore> fonts;

  //============================================================================
  // Fonts. Overriding these is what carries the typeface to every stock
  // control without touching each call site; a Slider's text box is a Label,
  // so getLabelFont covers those too.

  juce::Font getLabelFont(juce::Label& label) override
  {
    return fonts->pixel(juce::jmax(8.0f, label.getHeight() * 0.7f));
  }

  juce::Font getComboBoxFont(juce::ComboBox& box) override
  {
    return fonts->value(juce::jmin(16.0f, box.getHeight() * 0.62f));
  }

  juce::Font getPopupMenuFont() override { return fonts->value(15.0f); }

  juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override
  {
    return fonts->pixel(juce::jmin(11.0f, buttonHeight * 0.5f));
  }

  //============================================================================
  /** The Win95 two-step bevel. `raised` gives a button, `!raised` a sunken
      field -- highlight and shadow simply swap. */
  static void drawBevel(juce::Graphics& g, juce::Rectangle<int> b, bool raised)
  {
    using namespace design::colour;
    const auto outerHi = raised ? bevelLight : bevelDarkSoft;
    const auto outerLo = raised ? bevelDark : bevelLightSoft;
    const auto innerHi = raised ? bevelLightSoft : bevelDark;
    const auto innerLo = raised ? bevelDarkSoft : bevelLight;

    auto edge = [&g](juce::Rectangle<int> r, juce::Colour hi, juce::Colour lo) {
      g.setColour(hi);
      g.fillRect(r.getX(), r.getY(), r.getWidth(), 1);
      g.fillRect(r.getX(), r.getY(), 1, r.getHeight());
      g.setColour(lo);
      g.fillRect(r.getX(), r.getBottom() - 1, r.getWidth(), 1);
      g.fillRect(r.getRight() - 1, r.getY(), 1, r.getHeight());
    };

    edge(b, outerHi, outerLo);
    edge(b.reduced(1), innerHi, innerLo);
  }

  //============================================================================
  void drawLinearSlider(juce::Graphics& g,
                        int x,
                        int y,
                        int width,
                        int height,
                        float sliderPos,
                        float /*minSliderPos*/,
                        float /*maxSliderPos*/,
                        const juce::Slider::SliderStyle /*style*/,
                        juce::Slider& /*slider*/) override
  {
    auto track = juce::Rectangle<int>(x, y + height / 2 - 4, width, 8);
    g.setColour(design::colour::fieldFill);
    g.fillRect(track);
    drawBevel(g, track, false);

    auto filled = track.reduced(2).withWidth(juce::jmax(0, (int)sliderPos - x - 2));
    g.setColour(design::colour::accentBright.withAlpha(0.55f));
    g.fillRect(filled);

    // Handle: the design uses a small solid triangle rather than a thumb.
    juce::Path handle;
    const float cx = sliderPos, top = (float)track.getBottom();
    handle.addTriangle(cx - 4.5f, top + 7.0f, cx + 4.5f, top + 7.0f, cx, top);
    g.setColour(design::colour::text);
    g.fillPath(handle);
  }

  void drawRotarySlider(juce::Graphics& g,
                        int x,
                        int y,
                        int width,
                        int height,
                        float sliderPos,
                        const float rotaryStartAngle,
                        const float rotaryEndAngle,
                        juce::Slider& /*slider*/) override
  {
    // The design's global knob: a plain circle with a purple arc for the
    // amount and the value printed in the middle (Figma node 6:468).
    auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height)
                      .withSizeKeepingCentre((float)juce::jmin(width, height),
                                             (float)juce::jmin(width, height))
                      .reduced(1.0f);

    g.setColour(design::colour::bevelLight);
    g.fillEllipse(bounds);
    g.setColour(design::colour::bevelDarkSoft);
    g.drawEllipse(bounds, 1.0f);

    const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    juce::Path arc;
    arc.addCentredArc(bounds.getCentreX(),
                      bounds.getCentreY(),
                      bounds.getWidth() * 0.5f - 2.0f,
                      bounds.getHeight() * 0.5f - 2.0f,
                      0.0f,
                      rotaryStartAngle,
                      angle,
                      true);
    g.setColour(design::colour::accent);
    g.strokePath(arc, juce::PathStrokeType(3.0f));
  }

  void drawComboBox(juce::Graphics& g,
                    int width,
                    int height,
                    bool /*isButtonDown*/,
                    int /*buttonX*/,
                    int /*buttonY*/,
                    int /*buttonW*/,
                    int /*buttonH*/,
                    juce::ComboBox& box) override
  {
    auto bounds = juce::Rectangle<int>(0, 0, width, height);
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRect(bounds);
    drawBevel(g, bounds, false);

    // The "up/down" glyph, drawn rather than typed -- neither embedded font is
    // guaranteed to carry U+2195, and a tofu box in the FX row would be loud.
    const float cx = (float)width - 12.0f, cy = (float)height * 0.5f;
    juce::Path arrows;
    arrows.addTriangle(cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx, cy - 6.0f);
    arrows.addTriangle(cx - 3.5f, cy + 1.5f, cx + 3.5f, cy + 1.5f, cx, cy + 6.0f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.fillPath(arrows);
  }

  void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
  {
    label.setBounds(4, 1, box.getWidth() - 22, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setJustificationType(juce::Justification::centred);
  }

  void drawButtonBackground(juce::Graphics& g,
                            juce::Button& button,
                            const juce::Colour& backgroundColour,
                            bool shouldDrawButtonAsHighlighted,
                            bool shouldDrawButtonAsDown) override
  {
    auto bounds = button.getLocalBounds();
    g.setColour(shouldDrawButtonAsHighlighted ? backgroundColour.brighter(0.08f)
                                              : backgroundColour);
    g.fillRect(bounds);
    drawBevel(g, bounds, !shouldDrawButtonAsDown);
  }

  void drawToggleButton(juce::Graphics& g,
                        juce::ToggleButton& button,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override
  {
    const auto tick = juce::jmin(11.0f, (float)button.getHeight() * 0.7f);

    drawTickBox(g,
                button,
                0.0f,
                ((float)button.getHeight() - tick) * 0.5f,
                tick,
                tick,
                button.getToggleState(),
                button.isEnabled(),
                shouldDrawButtonAsHighlighted,
                shouldDrawButtonAsDown);

    g.setColour(button.findColour(juce::ToggleButton::textColourId));
    g.setFont(fonts->pixel(9.0f));

    if (!button.isEnabled())
      g.setOpacity(0.5f);

    g.drawFittedText(button.getButtonText(),
                     button.getLocalBounds().withTrimmedLeft(juce::roundToInt(tick) + 5),
                     juce::Justification::centredLeft,
                     1);
  }

  void drawTickBox(juce::Graphics& g,
                   juce::Component& component,
                   float x,
                   float y,
                   float w,
                   float h,
                   bool ticked,
                   bool /*isEnabled*/,
                   bool /*shouldDrawButtonAsHighlighted*/,
                   bool /*shouldDrawButtonAsDown*/) override
  {
    juce::Rectangle<int> box((int)x, (int)y, (int)w, (int)h);

    g.setColour(design::colour::bevelLight);
    g.fillRect(box);
    drawBevel(g, box, false);

    if (ticked) {
      g.setColour(component.findColour(juce::ToggleButton::tickColourId));
      g.fillRect(box.reduced(3));
    }
  }
};
