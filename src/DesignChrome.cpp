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

// White outline width for the header glyphs. Much thinner than the headings'
// 1.2px: the stroke is a fixed width, so on a 9px padlock it closes the
// counters and washes the whole glyph out toward the background.
constexpr float glyphOutline = 0.45f;

juce::Colour glyphColour(bool on, bool hovered)
{
  if (hovered)
    return colour::accentBright;

  return on ? colour::accent : colour::textDim;
}

} // namespace

//==============================================================================
DraggableValue::DraggableValue(juce::RangedAudioParameter& p, juce::MouseCursor cursor)
    : param(p)
{
  setMouseCursor(cursor);

  // Without this the inline editor cannot be dismissed by clicking away. JUCE
  // only fires focusLost when focus actually moves to something else, and it
  // finds that something by walking up from whatever was clicked -- so if
  // nothing in the chain wants focus, the editor silently keeps it and goes on
  // swallowing every key and click. The editor's top-level component sets the
  // same flag, to catch clicks that land on the window background.
  setWantsKeyboardFocus(true);
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
  // A click anywhere while typing dismisses the editor rather than also
  // starting a drag on whatever was underneath it.
  if (editor != nullptr)
    return;

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
      juce::jlimit(0.0f, 1.0f, valueAtDragStart + dragSign() * delta / dragRange));
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
  if (editor == nullptr)
    showEditor();
}

void DraggableValue::showMenu()
{
  const auto entries = menuEntries();
  if (entries.empty())
    return;

  juce::PopupMenu menu;

  // PopupMenu resolves its LookAndFeel from the default, not from the component
  // it is attached to, so it has to be told -- otherwise these come up in the
  // stock JUCE styling while the preset dropdown next to them does not.
  menu.setLookAndFeel(&getLookAndFeel());

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
  editor->setText(displayText(), juce::dontSendNotification);
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
        juce::jlimit(0.0f, 1.0f, valueForDisplayText(editor->getText())));
    param.endChangeGesture();
    dismiss();
  };
}

//==============================================================================
MixBackKnob::MixBackKnob(DtBlkFxAudioProcessor& p)
    : DraggableValue(*p.apvts.getParameter(DtBlkFxAudioProcessor::mixBackId),
                     juce::MouseCursor::UpDownResizeCursor)
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

juce::Rectangle<float> MixBackKnob::dialBounds() const
{
  // Centred in the gap between the two words, not in the component -- the
  // words are not symmetrical about the component's middle.
  juce::Rectangle<float> dial((float)dialSize, (float)dialSize);
  dial.setCentre((float)(filtBounds().getRight() + powrBounds().getX()) * 0.5f,
                 (float)getHeight() * 0.5f);
  return dial;
}

// The gauge presents dry/wet, which is what people expect of a knob in this
// position now. MixBack is the opposite sense, and it is a plain linear percent
// (see docs/PARAMETERS.md), so the inversion is exact. This is the one place
// the GUI deliberately reads differently from the host parameter, which still
// automates as "MixBack %".
juce::String MixBackKnob::displayText()
{
  return juce::String(juce::roundToInt((1.0f - param.getValue()) * 100.0f)) + "%";
}

float MixBackKnob::valueForDisplayText(const juce::String& t)
{
  return 1.0f - juce::jlimit(0.0f, 1.0f, t.getFloatValue() * 0.01f);
}

void MixBackKnob::mouseMove(const juce::MouseEvent& e)
{
  const int was = wordHover;
  wordHover = filtBounds().contains(e.getPosition()) ? 0
              : powrBounds().contains(e.getPosition()) ? 1
                                                       : -1;

  // The words are click targets inside a drag target, so the cursor has to say
  // which one the pointer is actually over.
  setMouseCursor(wordHover >= 0 ? juce::MouseCursor::PointingHandCursor
                                : juce::MouseCursor::UpDownResizeCursor);

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
  // The original's 0/25/50/75/100, but labelled as wet to match the readout.
  std::vector<std::pair<float, juce::String>> entries;
  for (int i = 0; i <= 100; i += 25)
    entries.emplace_back(1.0f - (float)i * 0.01f, juce::String(i) + "% wet");
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
  const auto dial = dialBounds();

  g.setColour(colour::bevelLight);
  g.fillEllipse(dial);

  // Inverted, so the fill rises with wetness rather than with MixBack.
  const auto value = 1.0f - param.getValue();
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
  g.drawText(displayText().removeCharacters("%"),
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

GlobalHeading::GlobalHeading(DtBlkFxAudioProcessor& p, Which w, LockHoverState& lh)
    : DraggableValue(*p.apvts.getParameter(headingParamId(w)),
                     juce::MouseCursor::UpDownResizeCursor)
    , processor(p)
    , lockHover(lh)
    , which(w)
    , title(headingTitle(w))
{
}

GlobalHeading::~GlobalHeading()
{
  lockHover.set(this, false);
}

int GlobalHeading::titleWidth() const
{
  return fonts->heading(28.0f).getStringWidth(title);
}

juce::Rectangle<int> GlobalHeading::glyphHitArea(juce::Rectangle<int> glyph)
{
  // Padded, both so a 9px target is actually clickable and so the hover
  // highlight has somewhere to sit.
  return glyph.expanded(3);
}

// Both sit far enough from the top that glyphHitArea's 3px padding -- and so
// the hover highlight -- stays inside the component instead of being clipped.
juce::Rectangle<int> GlobalHeading::lockBounds() const
{
  return {titleWidth() + 4, 5, 9, 10};
}

juce::Rectangle<int> GlobalHeading::syncBounds() const
{
  return {titleWidth() + 3, 22, 11, 11};
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
  glyphHover =
      glyphHitArea(lockBounds()).contains(e.getPosition())                       ? 0
      : (hasSyncGlyph() && glyphHitArea(syncBounds()).contains(e.getPosition())) ? 1
                                                                                 : -1;

  // Two click targets sitting inside a drag target.
  setMouseCursor(glyphHover >= 0 ? juce::MouseCursor::PointingHandCursor
                                 : juce::MouseCursor::UpDownResizeCursor);

  // Hovering any lock outlines RANDOM, which is the only thing a lock affects.
  lockHover.set(this, glyphHover == 0);

  if (was != glyphHover)
    repaint();
}

void GlobalHeading::mouseExit(const juce::MouseEvent& e)
{
  glyphHover = -1;
  lockHover.set(this, false);
  setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
  DraggableValue::mouseExit(e);
}

void GlobalHeading::mouseDown(const juce::MouseEvent& e)
{
  if (!e.mods.isPopupMenu()) {
    if (glyphHitArea(lockBounds()).contains(e.getPosition())) {
      locked = !locked;
      repaint();
      return;
    }

    if (hasSyncGlyph() && glyphHitArea(syncBounds()).contains(e.getPosition())) {
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
  // Figma "Top Options": a 28px italic heading with the 10px readout directly
  // beneath it. Both are drawn as paths on an explicit baseline rather than
  // with drawText, which clips to its box -- that is what was cropping the
  // readouts -- and cannot outline or shadow anything.
  const auto headingFont = fonts->heading(28.0f);
  drawRaised(g, textAsPath(headingFont, title, 0.0f, headingBaseline), colour::textDim);

  // The readout. BlkLen appends an asterisk when the delay is capping the block
  // size, which Phase 5 established as a user-facing signal; it is the one part
  // that is a different colour from the rest of the string.
  auto readout = displayText();
  const bool capped = readout.endsWith("*");
  if (capped)
    readout = readout.dropLastCharacters(1).trimEnd();

  const auto readoutFont = fonts->pixel(10.0f);
  g.setColour(colour::text);
  g.fillPath(textAsPath(readoutFont, readout, 0.0f, readoutBaseline));

  if (capped) {
    g.setColour(colour::warning);
    g.fillPath(textAsPath(
        readoutFont, "*", (float)readoutFont.getStringWidth(readout) + 1.0f, readoutBaseline));
  }

  // A hovered glyph brightens the ground behind it. At 9px the glyph alone is
  // too small to read as a hover, and there is no other affordance.
  auto glyphAt = [&](juce::Rectangle<int> bounds, const juce::Path& path, juce::Colour fill, bool hot) {
    if (hot) {
      g.setColour(colour::bevelLight.withAlpha(0.55f));
      g.fillRect(glyphHitArea(bounds));
    }

    drawRaised(g, Glyphs::scaled(path, bounds.toFloat()), fill, glyphOutline);
  };

  // Locked and unlocked are the same 60% black as the heading beside them --
  // the open and closed padlock shapes carry the state, not the weight.
  glyphAt(lockBounds(),
          glyphs->lock(locked, true),
          glyphColour(false, glyphHover == 0),
          glyphHover == 0);

  if (hasSyncGlyph()) {
    const bool on = modeIsOn();
    const auto bounds = syncBounds();
    const auto fill = glyphColour(on, glyphHover == 1);
    const juce::Rectangle<float> grid(Glyphs::beatSyncGrid, Glyphs::beatSyncGrid);

    if (glyphHover == 1) {
      g.setColour(colour::bevelLight.withAlpha(0.55f));
      g.fillRect(glyphHitArea(bounds));
    }

    // Both states are placed on the same grid, so the arrows do not jump or
    // change size as the flag is toggled.
    drawRaised(g,
               Glyphs::scaledFrom(on ? glyphs->beatSyncOn : glyphs->beatSyncOff,
                                  grid,
                                  bounds.toFloat()),
               fill,
               glyphOutline);

    if (!on)
      drawRaised(g, Glyphs::scaledFrom(glyphs->beatSyncCross, grid, bounds.toFloat()), fill, glyphOutline);
  }
}

juce::String GlobalHeading::displayText()
{
  auto text = param.getCurrentValueAsText();

  // Turning sync on otherwise changes nothing visible -- the percentage stays
  // put -- so say what happened.
  if (which == Which::overlap && modeIsOn())
    text += " sync";

  return text;
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
RandomButton::RandomButton(LockHoverState& lh)
    : juce::Button("Randomize")
    , lockHover(lh)
{
  lockHover.addChangeListener(this);
}

RandomButton::~RandomButton()
{
  lockHover.removeChangeListener(this);
}

void RandomButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
  auto bounds = getLocalBounds();

  // Hovering a lock anywhere outlines *and* lifts this button: randomisation is
  // the only thing a lock affects, and nothing else on screen says so.
  const bool lockCue = lockHover.isHovered();

  g.setColour(lockCue         ? colour::baseGrey.brighter(0.35f)
              : highlighted   ? colour::baseGrey.brighter(0.08f)
                              : colour::baseGrey);
  g.fillRect(bounds);
  RetroLookAndFeel::drawBevel(g, bounds, !down);

  if (lockCue) {
    g.setColour(colour::accentBright);
    g.drawRect(bounds, 2);
  }

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
