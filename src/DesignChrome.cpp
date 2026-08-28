/*
 * This file is part of DtBlkFx.  See LICENSE.md for copyright and licensing.
 *
 * Phase 6.2 -- the window furniture. See docs/PHASE6.md.
 */

#include "DesignChrome.h"
#include "DtBlkFxProcessor.h"
#include "RetroLookAndFeel.h"
#include "core/BlkFxParam.h"
#include "core/rfftw_float.h"

namespace design {

namespace {

// Full travel in roughly 200px of drag, either axis. The original's sliders
// were about a window-width wide, so this is the same order of sensitivity.
constexpr float dragRange = 200.0f;

// A menu value below this is a command rather than a value to set.
constexpr float commandValue = -1.0f;

juce::Colour glyphColour(bool on, bool hovered)
{
  if (hovered)
    return colour::accentBright;
  return on ? colour::accent : colour::text.withAlpha(0.35f);
}

} // namespace

//==============================================================================
DraggableValue::DraggableValue(juce::RangedAudioParameter& p, juce::MouseCursor cursor)
    : param(p)
{
  setMouseCursor(cursor);
}

DraggableValue::~DraggableValue() = default;

void DraggableValue::mouseEnter(const juce::MouseEvent&)
{
  hovered = true;
  repaint();
}

void DraggableValue::mouseExit(const juce::MouseEvent&)
{
  hovered = false;
  repaint();
}

void DraggableValue::mouseDown(const juce::MouseEvent& e)
{
  if (e.mods.isPopupMenu()) {
    showMenu();
    return;
  }

  valueAtDragStart = param.getValue();
  dragging = true;
  param.beginChangeGesture();
}

void DraggableValue::mouseDrag(const juce::MouseEvent& e)
{
  if (!dragging)
    return;

  // Either axis, so the control does not care which way the user reaches for
  // it. Up and right both increase.
  const auto delta = (float)(e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY());
  param.setValueNotifyingHost(
      juce::jlimit(0.0f, 1.0f, valueAtDragStart + delta / dragRange));
  repaint();
}

void DraggableValue::mouseUp(const juce::MouseEvent&)
{
  if (!dragging)
    return;

  dragging = false;
  param.endChangeGesture();
}

void DraggableValue::mouseDoubleClick(const juce::MouseEvent&)
{
  showEditor();
}

void DraggableValue::showMenu()
{
  const auto entries = menuEntries();
  if (entries.empty())
    return;

  juce::PopupMenu menu;
  for (size_t i = 0; i < entries.size(); ++i)
    menu.addItem((int)i + 1, entries[i].second);

  menu.showMenuAsync(
      juce::PopupMenu::Options().withTargetComponent(this),
      [safe = juce::Component::SafePointer<DraggableValue>(this), entries](int result) {
        // The host can close the editor while the menu is still up, and the
        // callback fires regardless.
        auto* self = safe.getComponent();
        if (self == nullptr || result <= 0 || result > (int)entries.size())
          return;

        const auto& [value, label] = entries[(size_t)result - 1];
        juce::ignoreUnused(label);

        if (value <= commandValue) {
          self->handleMenuCommand(result - 1);
          self->repaint();
          return;
        }

        self->param.beginChangeGesture();
        self->param.setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
        self->param.endChangeGesture();
        self->repaint();
      });
}

void DraggableValue::showEditor()
{
  // Typed entry goes through the parameter, so the GUI and the host cannot
  // disagree about what a string means -- Phase 5 wired every inverse.
  editor = std::make_unique<juce::TextEditor>();
  addAndMakeVisible(*editor);
  editor->setBounds(getLocalBounds().removeFromBottom(juce::jmin(18, getHeight())));
  editor->setText(param.getCurrentValueAsText(), juce::dontSendNotification);
  editor->selectAll();
  editor->grabKeyboardFocus();

  // Never delete the editor from inside one of its own callbacks -- it is
  // still on the stack. Unwind first.
  auto dismiss = [safe = juce::Component::SafePointer<DraggableValue>(this)] {
    juce::MessageManager::callAsync([safe] {
      if (auto* self = safe.getComponent()) {
        self->editor.reset();
        self->repaint();
      }
    });
  };

  editor->onEscapeKey = dismiss;
  editor->onFocusLost = dismiss;
  editor->onReturnKey = [this, dismiss] {
    param.beginChangeGesture();
    param.setValueNotifyingHost(
        juce::jlimit(0.0f, 1.0f, param.getValueForText(editor->getText())));
    param.endChangeGesture();
    dismiss();
  };
}

//==============================================================================
MixBackKnob::MixBackKnob(DtBlkFxAudioProcessor& p)
    : DraggableValue(*p.apvts.getParameter(DtBlkFxAudioProcessor::mixBackId),
                     juce::MouseCursor::UpDownLeftRightResizeCursor)
    , processor(p)
{
}

juce::Rectangle<int> MixBackKnob::filtBounds() const
{
  return {8, 0, 10, getHeight()};
}

juce::Rectangle<int> MixBackKnob::powrBounds() const
{
  return {getWidth() - 10, 0, 10, getHeight()};
}

void MixBackKnob::mouseMove(const juce::MouseEvent& e)
{
  const int was = wordHover;
  wordHover = filtBounds().contains(e.getPosition()) ? 0
              : powrBounds().contains(e.getPosition()) ? 1
                                                       : -1;
  if (was != wordHover)
    repaint();
}

void MixBackKnob::mouseExit(const juce::MouseEvent& e)
{
  wordHover = -1;
  DraggableValue::mouseExit(e);
}

void MixBackKnob::mouseDown(const juce::MouseEvent& e)
{
  // The two words are the `power` boolean, which Phase 5 split out of the
  // packed MIX_BACK parameter. Clicking one selects that mode outright rather
  // than toggling, so a click always does what the word says.
  if (!e.mods.isPopupMenu()) {
    const bool onFilt = filtBounds().contains(e.getPosition());
    const bool onPowr = powrBounds().contains(e.getPosition());

    if (onFilt || onPowr) {
      if (auto* power = processor.apvts.getParameter(DtBlkFxAudioProcessor::powerId)) {
        power->beginChangeGesture();
        power->setValueNotifyingHost(onPowr ? 1.0f : 0.0f);
        power->endChangeGesture();
      }
      repaint();
      return;
    }
  }

  DraggableValue::mouseDown(e);
}

std::vector<std::pair<float, juce::String>> MixBackKnob::menuEntries()
{
  std::vector<std::pair<float, juce::String>> entries;
  for (int i = 0; i <= 100; i += 25)
    entries.emplace_back((float)i * 0.01f, juce::String(i) + "%");
  return entries;
}

void MixBackKnob::paint(juce::Graphics& g)
{
  const bool powerOn =
      processor.apvts.getRawParameterValue(DtBlkFxAudioProcessor::powerId)->load() >= 0.5f;

  auto word = [&](const juce::String& text, juce::Rectangle<int> area, bool lit, bool hovered) {
    // FILT reads bottom-to-top and POWR top-to-bottom, mirroring each other
    // around the knob.
    juce::Graphics::ScopedSaveState save(g);
    g.setFont(fonts->pixel(10.0f));
    g.setColour(hovered ? colour::accentBright : (lit ? colour::accent : colour::textFaint));

    const auto centre = area.getCentre().toFloat();
    g.addTransform(juce::AffineTransform::rotation(
        text == "FILT" ? -juce::MathConstants<float>::halfPi : juce::MathConstants<float>::halfPi,
        centre.x,
        centre.y));

    g.drawText(text,
               juce::Rectangle<int>(area.getHeight(), area.getWidth()).withCentre(area.getCentre()),
               juce::Justification::centred,
               false);
  };

  word("FILT", filtBounds(), !powerOn, wordHover == 0);
  word("POWR", powrBounds(), powerOn, wordHover == 1);

  // The gauge. Not a rotary -- the design fills the circle from the bottom,
  // which is why there is no pointer anywhere in the Figma (node 6:468).
  const auto side = 38.0f;
  juce::Rectangle<float> dial(side, side);
  dial.setCentre(getLocalBounds().getCentre().toFloat());

  g.setColour(colour::bevelLight);
  g.drawEllipse(dial.expanded(1.5f), 2.0f);

  g.setColour(colour::bevelLight);
  g.fillEllipse(dial);

  const auto value = param.getValue();
  {
    auto inner = dial.reduced(3.0f);
    juce::Path clip;
    clip.addEllipse(inner);

    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(clip);
    g.setColour(colour::accentBright.withAlpha(0.4f));
    g.fillRect(inner.withTop(inner.getBottom() - inner.getHeight() * value));
  }

  g.setColour(juce::Colour(0xff8845ba));
  g.drawEllipse(dial.reduced(1.0f), 2.0f);

  // The design prints the percentage bare, without the sign.
  g.setColour(colour::text);
  g.setFont(fonts->pixel(10.0f));
  g.drawText(param.getCurrentValueAsText().removeCharacters("%"),
             dial.toNearestInt(),
             juce::Justification::centred,
             false);
}

//==============================================================================
namespace {

juce::String headingTitle(GlobalHeading::Which which)
{
  switch (which) {
    case GlobalHeading::Which::delay:
      return "Delay";
    case GlobalHeading::Which::overlap:
      return "Ovrlp";
    case GlobalHeading::Which::blkLen:
      return "BlkLen";
  }
  return {};
}

juce::String headingParamId(GlobalHeading::Which which)
{
  switch (which) {
    case GlobalHeading::Which::delay:
      return DtBlkFxAudioProcessor::paramId(BlkFxParam::DELAY);
    case GlobalHeading::Which::overlap:
      return DtBlkFxAudioProcessor::overlapId;
    case GlobalHeading::Which::blkLen:
      return DtBlkFxAudioProcessor::paramId(BlkFxParam::FFT_LEN);
  }
  return {};
}

} // namespace

GlobalHeading::GlobalHeading(DtBlkFxAudioProcessor& p, Which w)
    : DraggableValue(*p.apvts.getParameter(headingParamId(w)),
                     juce::MouseCursor::UpDownResizeCursor)
    , processor(p)
    , which(w)
    , title(headingTitle(w))
{
}

int GlobalHeading::titleWidth() const
{
  return fonts->heading(28.0f).getStringWidth(title);
}

juce::Rectangle<int> GlobalHeading::lockBounds() const
{
  return {titleWidth() + 4, 0, 9, 11};
}

juce::Rectangle<int> GlobalHeading::syncBounds() const
{
  return {titleWidth() + 3, 16, 11, 11};
}

bool GlobalHeading::modeIsOn() const
{
  if (which == Which::overlap)
    return processor.apvts.getRawParameterValue(DtBlkFxAudioProcessor::syncId)->load() >= 0.5f;

  // Delay: "on" means beats rather than milliseconds.
  BlkFxParam::Delay d(param.getValue());
  return d.getUnits() == BlkFxParam::Delay::BEATS;
}

void GlobalHeading::toggleMode()
{
  if (which == Which::overlap) {
    if (auto* sync = processor.apvts.getParameter(DtBlkFxAudioProcessor::syncId)) {
      sync->beginChangeGesture();
      sync->setValueNotifyingHost(modeIsOn() ? 0.0f : 1.0f);
      sync->endChangeGesture();
    }
    return;
  }

  if (which != Which::delay || processor.core == nullptr)
    return;

  // Switch units while keeping the delay the same *length*. This is what the
  // original's first delay-menu entry did (GlobalCtrl::updateMenus); flipping
  // the flag alone would jump the delay time, because the fractional part means
  // something different either side of the flag.
  BlkFxParam::Delay current(param.getValue());
  const auto sampleRate = (float)juce::jmax(1.0, processor.getSampleRate());
  const auto sampsPerBeat = juce::jmax(1.0f, processor.core->getSampsPerBeat());
  const auto samps =
      (float)(processor.core->getDelaySamps(current) - processor.core->_initial_delay);

  auto replacement = current.getUnits() == BlkFxParam::Delay::MSEC
                         ? BlkFxParam::Delay::beats(samps / sampsPerBeat)
                         : BlkFxParam::Delay::msec(samps / sampleRate * 1000.0f);

  param.beginChangeGesture();
  param.setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, (float)replacement));
  param.endChangeGesture();
}

void GlobalHeading::mouseMove(const juce::MouseEvent& e)
{
  const int was = glyphHover;
  glyphHover = lockBounds().contains(e.getPosition()) ? 0
               : (hasSyncGlyph() && syncBounds().contains(e.getPosition())) ? 1
                                                                            : -1;
  if (was != glyphHover)
    repaint();
}

void GlobalHeading::mouseExit(const juce::MouseEvent& e)
{
  glyphHover = -1;
  DraggableValue::mouseExit(e);
}

void GlobalHeading::mouseDown(const juce::MouseEvent& e)
{
  if (!e.mods.isPopupMenu()) {
    if (lockBounds().contains(e.getPosition())) {
      locked = !locked;
      repaint();
      return;
    }

    if (hasSyncGlyph() && syncBounds().contains(e.getPosition())) {
      toggleMode();
      repaint();
      return;
    }
  }

  DraggableValue::mouseDown(e);
}

std::vector<std::pair<float, juce::String>> GlobalHeading::menuEntries()
{
  std::vector<std::pair<float, juce::String>> entries;

  const auto sampleRate = (float)juce::jmax(1.0, processor.getSampleRate());
  const auto sampsPerBeat =
      processor.core != nullptr ? juce::jmax(1.0f, processor.core->getSampsPerBeat()) : 1.0f;

  auto describe = [&](float samps) {
    return juce::String(samps / sampleRate, 3) + " sec, " + juce::String((int)samps) +
           " samps, (" + juce::String(samps / sampsPerBeat, 2) + " beats)";
  };

  switch (which) {
    case Which::delay: {
      // Entries 0 and 1 are the original's two reserved slots: swap units
      // without changing the delay time, and "as long as one block".
      entries.emplace_back(commandValue,
                           modeIsOn() ? "Switch to milliseconds" : "Switch to beats");

      if (processor.core != nullptr) {
        const auto blockSamps = (float)g_fft_sz[BlkFxParam::getPlan(
            processor.apvts.getRawParameterValue(DtBlkFxAudioProcessor::paramId(BlkFxParam::FFT_LEN))
                ->load())];
        entries.emplace_back(
            (float)BlkFxParam::Delay::msec(blockSamps / sampleRate * 1000.0f),
            "One block  -  " + describe(blockSamps));
      }

      // The original's beat presets, rebuilt on every open because the labels
      // are tempo-dependent.
      for (float beats : {0.25f,
                          1.0f / 3.0f,
                          0.5f,
                          2.0f / 3.0f,
                          0.75f,
                          1.0f,
                          1.5f,
                          2.0f,
                          3.0f,
                          4.0f,
                          5.0f,
                          6.0f,
                          7.0f,
                          8.0f}) {
        const auto samps = beats * sampsPerBeat;
        entries.emplace_back((float)BlkFxParam::Delay::beats(beats),
                             juce::String(beats, 2) + " beats  -  " + describe(samps));
      }
      break;
    }

    case Which::overlap: {
      // The parameter's 0..1 spans 0..85%, which is why the ends are named
      // rather than numbered.
      entries.emplace_back(0.0f, "minimum");
      for (int i = 10; i <= 80; i += 10)
        entries.emplace_back((float)i / 85.0f, juce::String(i) + "%");
      entries.emplace_back(1.0f, "maximum");
      break;
    }

    case Which::blkLen: {
      // Snapped to the real FFT sizes. Anything else would be a lie about what
      // the engine can actually do -- guessFFTLen only ever picks one of these.
      for (int i = 0; i < NUM_FFT_SZ; ++i)
        entries.emplace_back(BlkFxParam::getFFTLenParam(i), describe((float)g_fft_sz[i]));
      break;
    }
  }

  return entries;
}

bool GlobalHeading::handleMenuCommand(int index)
{
  if (which == Which::delay && index == 0) {
    toggleMode();
    return true;
  }
  return false;
}

void GlobalHeading::paint(juce::Graphics& g)
{
  // Figma "Top Options": a 28px heading row with the 10px readout directly
  // beneath it, both left-aligned, in a 38px block.
  auto area = getLocalBounds();
  auto headingArea = area.removeFromTop(28);

  // The design puts a soft purple glow behind the heading. Drawn as a halo of
  // the same text rather than a shadow pass, which at 28px is indistinguishable
  // and far cheaper.
  const auto headingFont = fonts->heading(28.0f);

  g.setFont(headingFont);
  g.setColour(colour::accentGlow);
  for (auto offset : {juce::Point<int>(-1, 0),
                      juce::Point<int>(1, 0),
                      juce::Point<int>(0, -1),
                      juce::Point<int>(0, 1)})
    g.drawText(title, headingArea + offset, juce::Justification::centredLeft, false);

  g.setColour(colour::textDim);
  g.drawText(title, headingArea, juce::Justification::centredLeft, false);

  // Readout. BlkLen appends a red asterisk when the delay is capping the block
  // size, which Phase 5 established as a user-facing signal.
  auto readout = param.getCurrentValueAsText();
  const bool capped = readout.endsWith("*");
  if (capped)
    readout = readout.dropLastCharacters(1).trimEnd();

  g.setFont(fonts->pixel(10.0f));
  g.setColour(colour::text);
  g.drawText(readout, area, juce::Justification::centredLeft, false);

  if (capped) {
    g.setColour(colour::warning);
    g.drawText("*",
               area.withX(area.getX() + g.getCurrentFont().getStringWidth(readout) + 1),
               juce::Justification::centredLeft,
               false);
  }

  Glyphs::draw(g,
               glyphs->lock(locked, true),
               lockBounds().toFloat(),
               glyphHover == 0 ? colour::accentBright
                               : colour::text.withAlpha(locked ? 0.75f : 0.35f));

  if (hasSyncGlyph()) {
    const bool on = modeIsOn();
    Glyphs::draw(g,
                 on ? glyphs->beatSyncOn : glyphs->beatSyncOff,
                 syncBounds().toFloat(),
                 glyphColour(on, glyphHover == 1));
  }
}

//==============================================================================
TitleBar::TitleBar()
{
  setInterceptsMouseClicks(true, false);
}

juce::Rectangle<int> TitleBar::helpBounds() const
{
  return {getWidth() - 26, (getHeight() - 20) / 2, 22, 20};
}

void TitleBar::paint(juce::Graphics& g)
{
  g.setGradientFill(juce::ColourGradient(colour::titleBarLeft,
                                         0.0f,
                                         0.0f,
                                         colour::titleBarRight,
                                         (float)getWidth(),
                                         0.0f,
                                         false));
  g.fillAll();

  g.setFont(fonts->pixel(12.0f));
  g.setColour(colour::titleBarText);
  g.drawText("DtBlkFx Revived",
             getLocalBounds().withTrimmedLeft(6).withTrimmedRight(32),
             juce::Justification::centredLeft,
             false);

  const auto help = helpBounds();
  g.setColour(colour::baseGrey);
  g.fillRect(help);
  RetroLookAndFeel::drawBevel(g, help, !helpDown);

  Glyphs::draw(g,
               glyphs->help,
               help.reduced(6).toFloat().translated(helpDown ? 1.0f : 0.0f, helpDown ? 1.0f : 0.0f),
               helpHover ? colour::accentBright : colour::text);
}

void TitleBar::mouseMove(const juce::MouseEvent& e)
{
  const bool was = helpHover;
  helpHover = helpBounds().contains(e.getPosition());
  if (was != helpHover)
    repaint();
}

void TitleBar::mouseExit(const juce::MouseEvent&)
{
  helpHover = helpDown = false;
  repaint();
}

void TitleBar::mouseDown(const juce::MouseEvent& e)
{
  helpDown = helpBounds().contains(e.getPosition());
  repaint();
}

void TitleBar::mouseUp(const juce::MouseEvent& e)
{
  const bool clicked = helpDown && helpBounds().contains(e.getPosition());
  helpDown = false;
  repaint();

  if (!clicked)
    return;

  // The manual travels with the source. Fall back to the project page if this
  // is an installed build with no repository beside it.
  const auto manual = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                          .getParentDirectory()
                          .getChildFile("docs/MANUAL.md");

  if (manual.existsAsFile())
    manual.startAsProcess();
  else
    juce::URL("https://github.com/miruu-xyz/dtblkfx_mac/blob/main/docs/MANUAL.md")
        .launchInDefaultBrowser();
}

//==============================================================================
RandomButton::RandomButton()
    : juce::Button("Randomize")
{
}

void RandomButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
  auto bounds = getLocalBounds();

  g.setColour(highlighted ? colour::baseGrey.brighter(0.08f) : colour::baseGrey);
  g.fillRect(bounds);
  RetroLookAndFeel::drawBevel(g, bounds, !down);

  // Rotated 180 degrees, which is deliberate -- it is how the design draws it.
  juce::Graphics::ScopedSaveState save(g);
  const auto centre = bounds.getCentre().toFloat();
  g.addTransform(
      juce::AffineTransform::rotation(juce::MathConstants<float>::pi, centre.x, centre.y));

  g.setFont(fonts->pixel(11.0f));
  g.setColour(colour::text);
  g.drawText("RANDOM",
             down ? bounds.translated(1, 1) : bounds,
             juce::Justification::centred,
             false);
}

} // namespace design
