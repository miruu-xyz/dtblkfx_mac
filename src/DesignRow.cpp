/*
 * Phase 6.3b: the FX row. See DesignRow.h.
 *
 * See LICENSE.md for copyright and licensing information.
 */

#include "DesignRow.h"
#include "DtBlkFxProcessor.h"
#include "RetroLookAndFeel.h"

namespace design {

namespace {

using namespace juce;

// The row's values are set at the design's 16px, the same cut and tracking as
// the frequency and dB readouts it was drawn with.
constexpr float valueSize = 16.0f;

// A menu value below this is a command rather than a value to set. Matches
// DesignChrome.cpp; the row has no commands yet, but getValue() must never be
// allowed to return one by accident.
constexpr float commandValue = -1.0f;

// What a bypassed row fades to. The design greys the values and the border to
// 30% and leaves the picker and the glyphs alone (Figma "Property 1=Variant3").
constexpr float bypassedAlpha = 0.3f;

/** The paired triangles JUCE's fonts cannot be trusted to draw.

    Neither embedded typeface is guaranteed to carry U+2195 or U+2194, and a
    tofu box in the middle of a row would be loud, so both arrows are drawn --
    the same decision `RetroLookAndFeel::drawComboBox` already made.
*/
void drawArrows(Graphics& g, Rectangle<int> box, bool vertical, Colour c)
{
  const auto centre = box.getCentre().toFloat();
  const float across = 3.0f, along = 4.0f, reach = 6.0f;

  // Heads plus a stem between them -- two heads on their own read as a pair of
  // blobs at this size rather than as one double-headed arrow.
  //
  // The stem is filled separately rather than added to the same Path. A
  // triangle and a rectangle that overlap can wind against each other, and the
  // overlap then fills as a hole -- which is the white line that appeared
  // through the middle of both arrows when they shared one path.
  Path heads;
  Rectangle<float> stem;

  if (vertical) {
    heads.addTriangle(centre.x - across, centre.y - reach + along, centre.x + across,
                      centre.y - reach + along, centre.x, centre.y - reach);
    heads.addTriangle(centre.x - across, centre.y + reach - along, centre.x + across,
                      centre.y + reach - along, centre.x, centre.y + reach);
    stem = {centre.x - 0.5f, centre.y - reach, 1.0f, reach * 2.0f};
  }
  else {
    heads.addTriangle(centre.x - reach + along, centre.y - across, centre.x - reach + along,
                      centre.y + across, centre.x - reach, centre.y);
    heads.addTriangle(centre.x + reach - along, centre.y - across, centre.x + reach - along,
                      centre.y + across, centre.x + reach, centre.y);
    stem = {centre.x - reach, centre.y - 0.5f, reach * 2.0f, 1.0f};
  }

  g.setColour(c);
  g.fillPath(heads);
  g.fillRect(stem);
}

/** The effect a row is set to, read from its parameter rather than from the
    engine. The engine runs "Off" while the row is bypassed, and the menus have
    to keep offering what is parked in the row. */
FxRun1_0* parkedEffect(DtBlkFxAudioProcessor& p, int set)
{
  const auto id =
      DtBlkFxAudioProcessor::paramId(BlkFxParam::paramOffs(set) + BlkFxParam::FX_TYPE);
  return GetFxRun1_0((int)BlkFxParam::getEffectType(p.apvts.getRawParameterValue(id)->load()));
}

} // namespace

//==============================================================================
RowValue::RowValue(DtBlkFxAudioProcessor& p, int s, Which w)
    : DraggableValue(*p.apvts.getParameter(DtBlkFxAudioProcessor::paramId(
                         BlkFxParam::paramOffs(s) +
                         (w == Which::freqA  ? BlkFxParam::FX_FREQ_A
                          : w == Which::freqB ? BlkFxParam::FX_FREQ_B
                          : w == Which::amp   ? BlkFxParam::FX_AMP
                                              : BlkFxParam::FX_VAL))),
                     // Left-right, because the row reads that way and the cells
                     // sit side by side. The drag itself still takes either
                     // axis -- DraggableValue sums both -- so the cursor is a
                     // hint about the natural direction, not a restriction.
                     MouseCursor::LeftRightResizeCursor)
    , processor(p)
    , set(s)
    , which(w)
{
}

void RowValue::paint(Graphics& g)
{
  // Plain black, centred. Unlike the global headings these carry no outline or
  // shadow -- the design sets them flat, and the pointer is the affordance.
  shown = displayText();

  g.setFont(fonts->value(valueSize));
  g.setColour(colour::text);
  g.drawText(shown, getLocalBounds(), Justification::centred, false);
}

void RowValue::refresh()
{
  // The editor polls ten times a second, because these readouts follow each
  // other around. Repainting regardless is what made a drag feel sluggish:
  // eight rows of unconditional repaints compete on the message thread with the
  // cell that is actually being dragged, and the paints coalesce.
  if (shown != displayText())
    repaint();
}

std::vector<std::pair<float, String>> RowValue::menuEntries()
{
  auto* fx = parkedEffect(processor, set);
  std::vector<std::pair<float, String>> entries;

  // Ported from FxCtrl::updateAmpMenu. Effects that mix rather than scale get a
  // percentage list up to the 0 dB point and then carry on in dB; the rest
  // start at -inf and run from -40.
  if (which == Which::amp) {
    float dB = -40.0f;

    if (fx != nullptr && fx->ampMixMode()) {
      for (int i = 0; i <= 100; i += 25)
        entries.emplace_back(BlkFxParam::getAmpParam0dB() * 0.01f * (float)i,
                             String(i) + "%");
      dB = 10.0f;
    }
    else {
      entries.emplace_back(0.0f, "-inf");
    }

    for (; dB <= 40.0f; dB += 10.0f)
      entries.emplace_back(BlkFxParam::getAmpParam(dB), String((int)dB) + " dB");

    return entries;
  }

  // Ported from FxCtrl::updateFxValMenu. Every effect names its own values, and
  // maps a chosen name back to a parameter value against the current one -- so
  // this has to be rebuilt on each open, not just when the effect changes.
  if (which == Which::value && fx != nullptr) {
    const float current = param.getValue();

    for (int i = 0;; ++i) {
      const char* name = fx->getValueName(i);
      if (name == nullptr)
        break;

      const float v = fx->getValue(i, current);
      if (v > commandValue)
        entries.emplace_back(v, name);
    }
  }

  // The frequencies deliberately get nothing: the original never populated a
  // menu for them either.
  return entries;
}

//==============================================================================
FreqLink::FreqLink(DtBlkFxAudioProcessor& p, int set)
{
  for (int i = 0; i < 2; ++i)
    freq[i] = p.apvts.getParameter(
        DtBlkFxAudioProcessor::paramId(BlkFxParam::paramOffs(set) + BlkFxParam::FX_FREQ_A + i));

  setMouseCursor(MouseCursor::LeftRightResizeCursor);
}

void FreqLink::paint(Graphics& g)
{
  drawArrows(g, getLocalBounds(), false, hovered ? colour::accentBright : colour::text);
}

void FreqLink::mouseEnter(const MouseEvent&)
{
  hovered = true;
  repaint();
}

void FreqLink::mouseExit(const MouseEvent&)
{
  hovered = false;
  repaint();
}

void FreqLink::mouseDown(const MouseEvent&)
{
  dragging = true;
  for (int i = 0; i < 2; ++i)
    if (freq[i] != nullptr) {
      valueAtDragStart[i] = freq[i]->getValue();
      freq[i]->beginChangeGesture();
    }
}

void FreqLink::mouseDrag(const MouseEvent& e)
{
  if (!dragging)
    return;

  // The same delta into both, in parameter space. The frequency parameter is a
  // note offset, so an equal step moves both by the same interval and the range
  // keeps its width in musical terms rather than in Hz.
  const float delta = (float)e.getDistanceFromDragStartX() / DraggableValue::dragRange;

  for (int i = 0; i < 2; ++i)
    if (freq[i] != nullptr)
      freq[i]->setValueNotifyingHost(jlimit(0.0f, 1.0f, valueAtDragStart[i] + delta));
}

void FreqLink::mouseUp(const MouseEvent&)
{
  if (!dragging)
    return;

  dragging = false;
  for (int i = 0; i < 2; ++i)
    if (freq[i] != nullptr)
      freq[i]->endChangeGesture();
}

//==============================================================================
FxTypeCell::FxTypeCell(DtBlkFxAudioProcessor& p, int s)
    : processor(p)
    , param(*p.apvts.getParameter(
          DtBlkFxAudioProcessor::paramId(BlkFxParam::paramOffs(s) + BlkFxParam::FX_TYPE)))
    , set(s)
{
  setMouseCursor(MouseCursor::PointingHandCursor);
}

void FxTypeCell::mouseEnter(const MouseEvent&)
{
  hovered = true;
  repaint();
}

void FxTypeCell::mouseExit(const MouseEvent&)
{
  hovered = false;
  repaint();
}

void FxTypeCell::refresh()
{
  const bool on = isOn();
  if (shown == param.getCurrentValueAsText() && shownOn == on)
    return;

  shown = param.getCurrentValueAsText();
  shownOn = on;
  repaint();
}

bool FxTypeCell::isOn() const
{
  return processor.apvts.getRawParameterValue(DtBlkFxAudioProcessor::fxOnId(set))->load() >= 0.5f;
}

void FxTypeCell::paint(Graphics& g)
{
  auto bounds = getLocalBounds();

  // A bypassed row reads "---", which is how the design draws it. The effect is
  // still parked in the parameter; it is only not being shown.
  const bool on = isOn();
  shown = param.getCurrentValueAsText();
  shownOn = on;

  // The design gives the picker its full-strength fill and bevel in three of
  // its four variants -- Hover, Selected and the bypassed one. It brightens on
  // its own hover rather than the row's, though, which is a deliberate
  // departure: the row's hover cue belongs to the frequency window (6.6), and
  // having the picker light up from anywhere in the row makes it look clickable
  // when the pointer is nowhere near it.
  const bool strong = hovered || menuOpen || !on;

  g.setColour(colour::bevelLight.withAlpha(strong ? 0.5f : 0.2f));
  g.fillRect(bounds);

  // A single inset step, not RetroLookAndFeel's two-step bevel: the design
  // gives this one 1px of #808080 at the top left and 1px of white at the
  // bottom right, and the heavier edge reads wrong at 128 x 26.
  const float edge = strong ? 1.0f : 0.5f;
  g.setColour(colour::bevelDarkSoft.withAlpha(edge));
  g.fillRect(bounds.getX(), bounds.getY(), bounds.getWidth(), 1);
  g.fillRect(bounds.getX(), bounds.getY(), 1, bounds.getHeight());
  g.setColour(colour::bevelLight.withAlpha(edge));
  g.fillRect(bounds.getX(), bounds.getBottom() - 1, bounds.getWidth(), 1);
  g.fillRect(bounds.getRight() - 1, bounds.getY(), 1, bounds.getHeight());

  if (menuOpen) {
    g.setColour(colour::accentBright);
    g.drawRect(bounds, 2);
  }

  g.setFont(fonts->value(valueSize));
  g.setColour(on ? colour::text : colour::textFaint);
  g.drawText(on ? param.getCurrentValueAsText() : "---",
             bounds.withTrimmedLeft(16).withTrimmedRight(16),
             Justification::centred,
             false);

  drawArrows(g, bounds.removeFromRight(16), true, colour::text);
}

void FxTypeCell::mouseDown(const MouseEvent&)
{
  PopupMenu menu;
  menu.setLookAndFeel(&getLookAndFeel());

  // Flat, in engine order, for now. 6.4 splits it into two columns under
  // NORMAL / MASK FX / STEREO FX headers, which is the whole of that sub-phase.
  std::vector<float> values;
  const int current = (int)BlkFxParam::getEffectType(param.getValue());
  String previous;

  for (int i = 0; i < g_num_fx_1_0; ++i) {
    auto* fx = GetFxRun1_0(i);
    if (fx == nullptr)
      continue;

    const String name(fx->name());

    // The original skipped "DoNotUse"; the two adjacent no-op slots both report
    // "Off", so the second would list a duplicate nobody can tell apart.
    if (name == "DoNotUse" || name == previous)
      continue;
    previous = name;

    values.push_back(BlkFxParam::getEffectTypeInv(i));
    menu.addItem((int)values.size(), name, true, i == current);
  }

  menuOpen = true;
  repaint();

  menu.showMenuAsync(
      PopupMenu::Options().withTargetComponent(this).withMinimumWidth(getWidth()),
      [safe = Component::SafePointer<FxTypeCell>(this), values](int result) {
        // The host can close the editor while the menu is still up.
        auto* self = safe.getComponent();
        if (self == nullptr)
          return;

        self->menuOpen = false;
        self->repaint();

        if (result <= 0 || result > (int)values.size())
          return;

        self->param.beginChangeGesture();
        self->param.setValueNotifyingHost(values[(size_t)result - 1]);
        self->param.endChangeGesture();
      });
}

//==============================================================================
FxRow::FxRow(DtBlkFxAudioProcessor& p, int s, LockHoverState& lh)
    : processor(p)
    , lockHover(lh)
    , set(s)
    , freqA(p, s, RowValue::Which::freqA)
    , freqB(p, s, RowValue::Which::freqB)
    , amp(p, s, RowValue::Which::amp)
    , value(p, s, RowValue::Which::value)
    , link(p, s)
    , type(p, s)
{
  for (auto* c : std::initializer_list<Component*>{&freqA, &link, &freqB, &amp, &type, &value}) {
    addAndMakeVisible(c);
    c->addMouseListener(&hoverRelay, false);
  }

  refresh();
}

FxRow::~FxRow()
{
  lockHover.set(this, false);
}

bool FxRow::isOn() const
{
  return processor.apvts.getRawParameterValue(DtBlkFxAudioProcessor::fxOnId(set))->load() >= 0.5f;
}

void FxRow::refresh()
{
  // A bypassed row fades rather than being redrawn differently: the values keep
  // their layout, so nothing shifts as it goes off and back on.
  const bool on = isOn();
  const float wanted = on ? 1.0f : bypassedAlpha;
  for (auto* c : std::initializer_list<Component*>{&freqA, &link, &freqB, &amp, &value})
    if (c->getAlpha() != wanted)
      c->setAlpha(wanted);

  // Only the border and the glyphs are ours, and both follow the bypass flag.
  // Everything else asks its own child, which repaints only if its text moved.
  if (on != shownOn) {
    shownOn = on;
    repaint();
  }

  for (auto* c : std::initializer_list<RowValue*>{&freqA, &freqB, &amp, &value})
    c->refresh();

  type.refresh();
}

void FxRow::resized()
{
  auto area = getLocalBounds();
  area.removeFromRight(rightPad);
  area.removeFromRight(glyphStrip);

  const int cellWidth = (area.getWidth() - linkWidth - dropdownWidth) / 4;

  freqA.setBounds(area.removeFromLeft(cellWidth));
  link.setBounds(area.removeFromLeft(linkWidth));
  freqB.setBounds(area.removeFromLeft(cellWidth));
  amp.setBounds(area.removeFromLeft(cellWidth));

  auto picker = area.removeFromLeft(dropdownWidth);
  type.setBounds(picker.withSizeKeepingCentre(dropdownWidth, dropdownHeight));

  value.setBounds(area);
}

juce::Rectangle<int> FxRow::lockBounds() const
{
  // Anchored to the right edge, 7 x 9 and 8 x 9 with the design's 10px between
  // them, centred in the strip so the hover padding stays inside the row.
  const int right = getWidth() - rightPad - (glyphStrip - 25) / 2;
  return {right - 25, (getHeight() - 9) / 2, 7, 9};
}

juce::Rectangle<int> FxRow::powerBounds() const
{
  return lockBounds().withWidth(8).withX(lockBounds().getRight() + 10);
}

void FxRow::paint(Graphics& g)
{
  const bool on = isOn();

  // The dashed border, #999DA6, dimmed with everything else when bypassed.
  const float dashes[]{3.0f, 3.0f};
  auto b = getLocalBounds().toFloat().reduced(0.5f);
  g.setColour(colour::rowOutline.withAlpha(on ? 1.0f : bypassedAlpha));
  for (auto line : {Line<float>(b.getTopLeft(), b.getTopRight()),
                    Line<float>(b.getBottomLeft(), b.getBottomRight()),
                    Line<float>(b.getTopLeft(), b.getBottomLeft()),
                    Line<float>(b.getTopRight(), b.getBottomRight())})
    g.drawDashedLine(line, dashes, 2, 1.0f);

  // The two glyphs. Same treatment as the header's: a hovered one brightens the
  // ground behind it, because at 9px the glyph alone is too small to read as a
  // hover.
  //
  // Clipped out early when the repaint is for something else. The cells are not
  // opaque, so every repaint of a value being dragged also calls this -- and
  // drawRaised builds a DropShadow image per glyph, which is far too expensive
  // to run on every drag event.
  const auto clip = g.getClipBounds();

  auto glyphAt = [&](Rectangle<int> bounds, const Path& path, bool lit, bool hot) {
    if (!clip.intersects(glyphHitArea(bounds)))
      return;

    if (hot) {
      g.setColour(colour::bevelLight.withAlpha(0.55f));
      g.fillRect(glyphHitArea(bounds));
    }

    drawRaised(g, Glyphs::scaled(path, bounds.toFloat()), glyphColour(lit, hot), glyphOutline);
  };

  // Locked and unlocked are the same weight -- the padlock shape carries the
  // state -- while power lights up, because "this row is running" is the thing
  // worth seeing from across the window.
  glyphAt(lockBounds(), glyphs->lock(locked, true), false, glyphHover == 0);
  glyphAt(powerBounds(), glyphs->power, on, glyphHover == 1);
}

void FxRow::updateHover()
{
  const bool now = getLocalBounds().contains(getMouseXYRelative());
  if (now == hovered)
    return;

  hovered = now;
  repaint();
}

void FxRow::mouseEnter(const MouseEvent&)
{
  updateHover();
}

void FxRow::mouseExit(const MouseEvent&)
{
  updateHover();
  glyphHover = -1;
  lockHover.set(this, false);
  repaint();
}

void FxRow::mouseMove(const MouseEvent& e)
{
  const int was = glyphHover;
  glyphHover = glyphHitArea(lockBounds()).contains(e.getPosition())    ? 0
               : glyphHitArea(powerBounds()).contains(e.getPosition()) ? 1
                                                                       : -1;

  if (was == glyphHover)
    return;

  // The glyphs are clicks inside a row that is otherwise all drag targets.
  setMouseCursor(glyphHover >= 0 ? MouseCursor::PointingHandCursor
                                 : MouseCursor::NormalCursor);

  // Hovering any lock in the window outlines RANDOM, which is the only thing
  // that says what a lock is for.
  lockHover.set(this, glyphHover == 0);
  repaint();
}

void FxRow::mouseDown(const MouseEvent& e)
{
  if (glyphHitArea(lockBounds()).contains(e.getPosition())) {
    locked = !locked;
    repaint();
    return;
  }

  if (glyphHitArea(powerBounds()).contains(e.getPosition())) {
    if (auto* on = processor.apvts.getParameter(DtBlkFxAudioProcessor::fxOnId(set))) {
      on->beginChangeGesture();
      on->setValueNotifyingHost(isOn() ? 0.0f : 1.0f);
      on->endChangeGesture();
    }
    refresh();
  }
}

} // namespace design
