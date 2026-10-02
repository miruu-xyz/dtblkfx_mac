/*
 * Phases 6.3b and 6.6b: the FX row. See DesignRow.h.
 *
 * See LICENSE.md for copyright and licensing information.
 */

#include "DesignRow.h"
#include "DesignAxis.h"
#include "DtBlkFxProcessor.h"
#include "RetroLookAndFeel.h"

namespace design {

namespace {

using namespace juce;

// The row's values, and the picker's effect name, at the design's 15px (Figma
// 26:612). The drawn arrows keep their size so they stay legible.
constexpr float valueSize = 15.0f;

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

/** Pixels of drag for a row value's full range: halfway between the
    headings' fixed 200px and the distance its indicator travels on the row.
    Exactly the indicator's distance felt slow; 200px was far too fast. */
float rowDragPixels(float indicatorTravel)
{
  return (DraggableValue::dragRange + indicatorTravel) * 0.5f;
}

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

juce::Rectangle<int> RowValue::editorBounds() const
{
  // Figma 26:612: inset 4px either side, 22px tall, centred. The two frequency
  // boxes are inset further on the side facing the grip between them -- at 4px
  // their edges ran right up against its arrowheads.
  constexpr int inset = 4, nearGrip = 10;
  auto box = getLocalBounds().withSizeKeepingCentre(getWidth(), 22);
  box = box.withTrimmedLeft(which == Which::freqB ? nearGrip : inset)
            .withTrimmedRight(which == Which::freqA ? nearGrip : inset);
  return box;
}

void RowValue::setLinked(bool shouldBe)
{
  if (linked == shouldBe)
    return;

  linked = shouldBe;
  repaint();
}

float RowValue::dragPixels() const
{
  // Scaled to how far the value's indicator travels on the row -- a frequency
  // across the frequency axis, amp across the wedge, which spans the whole row
  // -- so the rate follows the row's size. Value has no indicator; it takes the
  // row's width so every cell in a row drags at the same rate.
  if (auto* row = findParentComponentOfClass<FxRow>())
    return rowDragPixels(which == Which::freqA || which == Which::freqB ? row->freqAxisLength()
                                                                        : (float)row->getWidth());
  return DraggableValue::dragPixels();
}

void RowValue::paint(Graphics& g)
{
  // Plain black, centred. Unlike the global headings these carry no outline or
  // shadow -- the design sets them flat.
  shown = displayText();

  // The value under the pointer, or being dragged or typed into, sits in a
  // pale box with a dashed edge (Figma 26:612), so it is clear which one a
  // click will change.
  if (isHovered() || isEditing() || linked) {
    const auto box = editorBounds().toFloat();
    g.setColour(colour::bevelLight.withAlpha(0.5f));
    g.fillRect(box);
    drawDashedRect(g, box, colour::rowOutline);
  }

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

void RowValue::valueChanged()
{
  // Amp moves the wedge and the frequencies move their handles, anywhere along
  // the row -- well outside this cell -- so the row has to hear about it now.
  if (auto* row = findParentComponentOfClass<FxRow>())
    row->refresh();
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
  showLinked(true);
  repaint();
}

void FreqLink::mouseExit(const MouseEvent&)
{
  // During a drag JUCE holds off the exit until the button is released, so by
  // the time this arrives the drag is over.
  hovered = false;
  showLinked(false);
  repaint();
}

void FreqLink::showLinked(bool on)
{
  // The grip changes both frequencies, so both readouts take the hover box.
  if (auto* row = findParentComponentOfClass<FxRow>())
    row->setFreqsLinked(on);
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
  // Scaled like the frequency readouts, so the grip and the readouts it moves
  // drag at the same rate.
  auto* row = findParentComponentOfClass<FxRow>();
  const float pixels = row != nullptr ? rowDragPixels(row->freqAxisLength()) : DraggableValue::dragRange;
  const float delta = (float)e.getDistanceFromDragStartX() / pixels;

  for (int i = 0; i < 2; ++i)
    if (freq[i] != nullptr)
      freq[i]->setValueNotifyingHost(jlimit(0.0f, 1.0f, valueAtDragStart[i] + delta));

  if (auto* row = findParentComponentOfClass<FxRow>())
    row->refresh();
}

void FreqLink::mouseUp(const MouseEvent&)
{
  if (!dragging)
    return;

  dragging = false;
  for (int i = 0; i < 2; ++i)
    if (freq[i] != nullptr)
      freq[i]->endChangeGesture();

  showLinked(isMouseOver());
}

//==============================================================================
PopupMenu buildFxTypeMenu(int currentEffect, std::vector<float>& valueForResult)
{
  // The engine knows which effects are masks. It has no equivalent for the
  // stereo ones -- they are just the tail of the table, inside #ifdef STEREO --
  // so those are named. An effect added later lands in NORMAL until it is
  // listed here, which is the safe direction to be wrong in.
  const StringArray stereo{"Vocode", "HarmMatchLR", "HarmMatchRL", "CrossMix", "WarpMix"};

  int off = -1;
  std::vector<int> normal, mask, stereoFx;
  StringArray seen;

  for (int i = 0; i < g_num_fx_1_0; ++i) {
    auto* fx = GetFxRun1_0(i);
    if (fx == nullptr)
      continue;

    // The original skipped "DoNotUse". The two adjacent no-op slots both report
    // "Off", and a second one would be a duplicate nobody can tell apart.
    const String name(fx->name());
    if (name == "DoNotUse" || seen.contains(name))
      continue;
    seen.add(name);

    if (name == "Off")
      off = i;
    else
      (fx->isMask() ? mask : stereo.contains(name) ? stereoFx : normal).push_back(i);
  }

  // Ticked by name, so a row on the second "Off" slot still shows as Off.
  auto* current = GetFxRun1_0(currentEffect);
  const String currentName = current != nullptr ? current->name() : "";

  PopupMenu menu;
  valueForResult.clear();

  auto add = [&](int i) {
    valueForResult.push_back(BlkFxParam::getEffectTypeInv(i));
    const String name(GetFxRun1_0(i)->name());
    menu.addItem((int)valueForResult.size(), name, true, name == currentName);
  };

  if (off >= 0)
    add(off);
  menu.addSectionHeader("NORMAL");
  for (int i : normal)
    add(i);

  menu.addColumnBreak();

  // One empty row, so MASK FX sits level with NORMAL rather than beside "Off".
  // Disabled, so it never highlights and can never be returned.
  menu.addItem(PopupMenu::Item().setID(std::numeric_limits<int>::max()).setEnabled(false));

  menu.addSectionHeader("MASK FX");
  for (int i : mask)
    add(i);

  menu.addSectionHeader("STEREO FX");
  for (int i : stereoFx)
    add(i);

  return menu;
}

//==============================================================================
bool consumesMask(FxRun1_0* fx)
{
  if (fx == nullptr || fx->isMask())
    return false;

  const StringArray ignoresMask{"Off", "Vocode", "HarmMatchLR", "HarmMatchRL"};
  return !ignoresMask.contains(fx->name());
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
    g.setColour(colour::selection);
    g.drawRect(bounds, 2);
  }

  // The design's 96px text box holds "HarmMask" but not the longest names --
  // AutoHarmMask, HarmRepitch, HarmMatchLR -- at 16px. Those squash
  // horizontally to fit, down to 75%, rather than being clipped.
  g.setFont(fonts->value(valueSize));
  g.setColour(on ? colour::text : colour::textFaint);
  g.drawFittedText(on ? param.getCurrentValueAsText() : "---",
                   bounds.withTrimmedLeft(16).withTrimmedRight(16),
                   Justification::centred,
                   1,
                   0.75f);

  drawArrows(g, bounds.removeFromRight(16), true, colour::text);
}

void FxTypeCell::mouseDown(const MouseEvent&)
{
  std::vector<float> values;
  auto menu = buildFxTypeMenu((int)BlkFxParam::getEffectType(param.getValue()), values);
  menu.setLookAndFeel(&getLookAndFeel());

  menuOpen = true;
  repaint();

  menu.showMenuAsync(
      PopupMenu::Options().withTargetComponent(this),
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

        // The effect decides which of the wedge and handles are shown at all.
        if (auto* row = self->findParentComponentOfClass<FxRow>())
          row->refresh();
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
  // The handles go last, so they sit above the cells.
  for (auto* c : std::initializer_list<Component*>{
           &freqA, &link, &freqB, &amp, &type, &value, &handles}) {
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

void FxRow::setFreqsLinked(bool on)
{
  freqA.setLinked(on);
  freqB.setLinked(on);
}

bool FxRow::rangeHighlight(float& a, float& b) const
{
  // Evaluated live rather than tracked through events: during a press JUCE
  // keeps the pressed component as the one "under" the mouse, and the button
  // state is already up to date when the editor polls.
  auto mouse = Desktop::getInstance().getMainMouseSource();
  auto* pressed = mouse.isDragging() ? mouse.getComponentUnderMouse() : nullptr;
  const bool pressing = pressed != nullptr && (pressed == &freqA || pressed == &freqB ||
                                               pressed == &link || pressed == &handles);

  if (!pressing || !shown.usesA || !shown.usesB)
    return false;

  a = shown.a;
  b = shown.b;
  return true;
}

FxRun1_0* FxRow::effect() const
{
  return parkedEffect(processor, set);
}

juce::RangedAudioParameter* FxRow::fxParam(int fxParamIndex) const
{
  return processor.apvts.getParameter(
      DtBlkFxAudioProcessor::paramId(BlkFxParam::paramOffs(set) + fxParamIndex));
}

FxRow::Shown FxRow::current() const
{
  Shown now;
  now.a = fxParam(BlkFxParam::FX_FREQ_A)->getValue();
  now.b = fxParam(BlkFxParam::FX_FREQ_B)->getValue();
  now.amp = fxParam(BlkFxParam::FX_AMP)->getValue();
  now.on = isOn();

  // What the parked effect uses, not the engine's "Off" -- a bypassed row
  // still shows its wedge and window, faded, so it can be set up while off.
  if (auto* fx = effect()) {
    now.usesA = fx->paramUsed(BlkFxParam::FX_FREQ_A);
    now.usesB = fx->paramUsed(BlkFxParam::FX_FREQ_B);
    now.usesAmp = fx->paramUsed(BlkFxParam::FX_AMP);
  }
  return now;
}

void FxRow::refresh()
{
  // A bypassed row fades rather than being redrawn differently: the values keep
  // their layout, so nothing shifts as it goes off and back on.
  const float wanted = isOn() ? 1.0f : bypassedAlpha;
  for (auto* c : std::initializer_list<Component*>{&freqA, &link, &freqB, &amp, &value})
    if (c->getAlpha() != wanted)
      c->setAlpha(wanted);

  visualsChanged();

  for (auto* c : std::initializer_list<RowValue*>{&freqA, &freqB, &amp, &value})
    c->refresh();

  type.refresh();
}

void FxRow::visualsChanged()
{
  const auto now = current();
  const auto was = shown;
  shown = now;

  // Anything that changes what is drawn at all, rather than where: repaint the
  // lot. Crossing the handles is one of these -- the dimming flips from the
  // outside of the range to the inside, which changes the whole span between
  // them, not just the strip a handle moved across.
  if (now.on != was.on || now.usesA != was.usesA || now.usesB != was.usesB ||
      now.usesAmp != was.usesAmp || (now.a <= now.b) != (was.a <= was.b)) {
    repaint();
    return;
  }

  // Otherwise only the strip each value moved across. A full-row repaint would
  // also redraw the two glyphs, whose drop shadows are far too expensive to
  // build on every drag event -- that was most of 6.3b's sluggish drag.
  const auto span = freqSpan();
  const float h = (float)getHeight(), w = (float)getWidth();
  const float pad = handleBounds(0).getWidth() * 0.5f + 1.0f;

  auto strip = [&](float x0, float x1, float margin) {
    repaint(juce::Rectangle<float>(juce::jmin(x0, x1) - margin, 0.0f,
                                   std::abs(x1 - x0) + margin * 2.0f, h)
                .getSmallestIntegerContainer());
  };

  if (now.a != was.a)
    strip(axis::paramToX(was.a, span), axis::paramToX(now.a, span), pad);
  if (now.b != was.b)
    strip(axis::paramToX(was.b, span), axis::paramToX(now.b, span), pad);
  if (now.amp != was.amp)
    strip(axis::paramToX(was.amp, {0.0f, w}), axis::paramToX(now.amp, {0.0f, w}), 1.0f);
}

void FxRow::resized()
{
  auto area = getLocalBounds();
  handles.setBounds(area);

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

//------------------------------------------------------------------------------
// Geometry. The frequency axis is the one the spectrograms use (DesignAxis.h),
// inset 5px each side so a handle at either extreme is fully visible. The amp
// wedge spans the whole row, as the Figma draws it: its ramp starts at the
// left edge and reaches full height at x = 370.1 of 618, which is 0.599 -- the
// engine's 0 dB point to within a pixel.

juce::Range<float> FxRow::freqSpan() const
{
  return axis::span(getLocalBounds().toFloat());
}

float FxRow::freqX(int handle) const
{
  return axis::paramToX(handle == 0 ? shown.a : shown.b, freqSpan());
}

juce::Rectangle<float> FxRow::handleBounds(int handle) const
{
  // Figma 5:278: 9 x 7, centred on its frequency. Min sits on the bottom edge
  // pointing up, max on the top edge pointing down.
  const float x = freqX(handle) - 4.5f;
  return {x, handle == 0 ? (float)getHeight() - 7.0f : 0.0f, 9.0f, 7.0f};
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

//------------------------------------------------------------------------------
void FxRow::paint(Graphics& g)
{
  const auto bounds = getLocalBounds().toFloat();
  const float w = bounds.getWidth(), h = bounds.getHeight();

  // 1. The amp wedge (Figma 6:1659): a ramp from nothing at the left edge to
  //    full height at 0 dB, full height beyond it, filled from the left edge to
  //    the current amp and nothing past it. The kink *is* the split: below it
  //    the ramp is wet % for a mix-mode effect and attenuation for the rest,
  //    and above it is gain either way.
  if (shown.usesAmp) {
    const float unity = axis::paramToX(axis::ampUnityParam(), {0.0f, w});
    const float level = axis::paramToX(shown.amp, {0.0f, w});

    Path wedge;
    wedge.startNewSubPath(0.0f, h);
    wedge.lineTo(unity, 0.0f);
    wedge.lineTo(w, 0.0f);
    wedge.lineTo(w, h);
    wedge.closeSubPath();

    Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(juce::Rectangle<float>(0.0f, 0.0f, level, h).getSmallestIntegerContainer());
    g.setColour(shown.on ? colour::wedge : colour::wedgeBypassed);
    g.fillPath(wedge);
  }

  // 2. The frequency window (Figma 5:285): white over everything the effect
  //    does not touch. Full strength only while the range is being worked --
  //    pointer on a frequency readout, the grip or a handle. With the
  //    handles crossed the engine processes the *outside* of the range
  //    (SplitMaskProcess: "freqA > freqB : process outside region"), so the
  //    dimming flips to the band between them. Only drawn when the effect uses
  //    both frequencies: HarmMask's single frequency is a fundamental, not a
  //    range edge.
  if (shown.usesA && shown.usesB) {
    auto dim = rangeHot ? colour::rangeDimHover : colour::rangeDim;
    if (!shown.on)
      dim = colour::rangeDimHover.withMultipliedAlpha(0.2f); // Variant3: 0.6 x 20%

    const float xA = freqX(0), xB = freqX(1);
    g.setColour(dim);

    if (shown.a <= shown.b) {
      g.fillRect(juce::Rectangle<float>(0.0f, 0.0f, xA, h));
      g.fillRect(juce::Rectangle<float>(xB, 0.0f, w - xB, h));
    }
    else {
      g.fillRect(juce::Rectangle<float>(xB, 0.0f, xA - xB, h));
    }
  }

  // 3. The dashed border, #999DA6, dimmed with everything else when bypassed.
  drawDashedRect(g, bounds, colour::rowOutline.withAlpha(shown.on ? 1.0f : bypassedAlpha));

  // 4. The two glyphs. Same treatment as the header's: a hovered one brightens
  //    the ground behind it, because at 9px the glyph alone is too small to
  //    read as a hover.
  //
  //    Clipped out early when the repaint is for something else. The cells are
  //    not opaque, so every repaint of a value being dragged also calls this --
  //    and drawRaised builds a DropShadow image per glyph, which is far too
  //    expensive to run on every drag event.
  const auto clip = g.getClipBounds();

  auto glyphAt = [&](juce::Rectangle<int> r, const Path& path, bool lit, bool hot) {
    if (!clip.intersects(glyphHitArea(r)))
      return;

    if (hot) {
      g.setColour(colour::bevelLight.withAlpha(0.55f));
      g.fillRect(glyphHitArea(r));
    }

    drawRaised(g, Glyphs::scaled(path, r.toFloat()), glyphColour(lit, hot), glyphOutline);
  };

  // Locked and unlocked are the same weight -- the padlock shape carries the
  // state -- while power lights up, because "this row is running" is the thing
  // worth seeing from across the window.
  glyphAt(lockBounds(), glyphs->lock(locked, true), false, glyphHover == 0);
  glyphAt(powerBounds(), glyphs->power, shown.on, glyphHover == 1);
}

//------------------------------------------------------------------------------
void FxRow::updateHover()
{
  // Over one of the range controls, or still dragging one even if the pointer
  // has left it -- during a drag JUCE keeps the pressed component as the one
  // "under" the mouse.
  auto mouse = Desktop::getInstance().getMainMouseSource();
  auto* pressed = mouse.isDragging() ? mouse.getComponentUnderMouse() : nullptr;
  const auto p = getMouseXYRelative();

  const bool now = (pressed != nullptr && (pressed == &freqA || pressed == &freqB ||
                                           pressed == &link || pressed == &handles)) ||
                   freqA.getBounds().contains(p) || freqB.getBounds().contains(p) ||
                   link.getBounds().contains(p) || handles.handleAt(p) >= 0;
  if (now == rangeHot)
    return;

  rangeHot = now;

  // It only changes the dimming, and only where there is any.
  if (shown.usesA && shown.usesB)
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
  repaint(lockBounds().getUnion(powerBounds()).expanded(4));
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
  repaint(lockBounds().getUnion(powerBounds()).expanded(4));
}

void FxRow::mouseDown(const MouseEvent& e)
{
  if (glyphHitArea(lockBounds()).contains(e.getPosition())) {
    locked = !locked;
    repaint(lockBounds().expanded(4));
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

//==============================================================================
FxRow::HandleLayer::HandleLayer(FxRow& r)
    : row(r)
{
  setMouseCursor(MouseCursor::LeftRightResizeCursor);
}

int FxRow::HandleLayer::handleAt(juce::Point<int> p) const
{
  // The 9 x 7 triangle padded to roughly 15 x 12, so it can actually be caught.
  // Min and max sit on opposite edges, so their areas can never overlap.
  for (int h : {0, 1}) {
    if (!(h == 0 ? row.shown.usesA : row.shown.usesB))
      continue;

    auto area = row.handleBounds(h).withSizeKeepingCentre(15.0f, 12.0f);
    area = h == 0 ? area.withBottomY((float)getHeight()) : area.withY(0.0f);
    if (area.contains(p.toFloat()))
      return h;
  }
  return -1;
}

bool FxRow::HandleLayer::hitTest(int x, int y)
{
  return dragging >= 0 || handleAt({x, y}) >= 0;
}

void FxRow::HandleLayer::paint(Graphics& g)
{
  for (int h : {0, 1}) {
    if (!(h == 0 ? row.shown.usesA : row.shown.usesB))
      continue;

    const auto r = row.handleBounds(h);
    auto path = Glyphs::scaled(row.glyphs->freqHandle, r);
    if (h == 1)
      path.applyTransform(AffineTransform::verticalFlip(r.getHeight()).translated(0.0f, r.getY() * 2.0f));

    // #999DA6 at rest, so they do not compete with the values; accent while
    // hovered or dragged. Variant3 fades the whole frequency visual to 20%,
    // handles included.
    const bool lit = hot == h || dragging == h;
    g.setColour((lit ? colour::accentBright : colour::rowOutline)
                    .withMultipliedAlpha(row.shown.on ? 1.0f : 0.2f));
    g.fillPath(path);
  }
}

void FxRow::HandleLayer::mouseMove(const MouseEvent& e)
{
  const int was = hot;
  hot = handleAt(e.getPosition());
  if (hot != was) {
    repaint();
    row.updateHover();
  }
}

void FxRow::HandleLayer::mouseExit(const MouseEvent&)
{
  if (dragging >= 0 || hot < 0)
    return;

  hot = -1;
  repaint();
}

void FxRow::HandleLayer::mouseDown(const MouseEvent& e)
{
  const int h = handleAt(e.getPosition());
  if (h < 0 || e.mods.isPopupMenu())
    return;

  auto* param = row.fxParam(h == 0 ? BlkFxParam::FX_FREQ_A : BlkFxParam::FX_FREQ_B);
  if (param == nullptr)
    return;

  // Option-click resets, as on every other value: min to 0 Hz, max to 25.8 kHz,
  // so it opens the window rather than collapsing it.
  if (e.mods.isAltDown()) {
    param->beginChangeGesture();
    param->setValueNotifyingHost(param->getDefaultValue());
    param->endChangeGesture();
    row.refresh();
    return;
  }

  // Absolute, keeping the offset it was grabbed at, so it never jumps under the
  // pointer on mouse-down.
  dragging = h;
  grabOffset = (float)e.x - row.freqX(h);
  param->beginChangeGesture();
  repaint();
}

void FxRow::HandleLayer::mouseDrag(const MouseEvent& e)
{
  if (dragging < 0)
    return;

  if (auto* param = row.fxParam(dragging == 0 ? BlkFxParam::FX_FREQ_A : BlkFxParam::FX_FREQ_B)) {
    param->setValueNotifyingHost(axis::xToParam((float)e.x - grabOffset, row.freqSpan()));
    row.refresh();
  }
}

void FxRow::HandleLayer::mouseUp(const MouseEvent& e)
{
  if (dragging < 0)
    return;

  if (auto* param = row.fxParam(dragging == 0 ? BlkFxParam::FX_FREQ_A : BlkFxParam::FX_FREQ_B))
    param->endChangeGesture();

  dragging = -1;
  hot = handleAt(e.getPosition());
  row.updateHover();
  repaint();
}

void FxRow::HandleLayer::mouseDoubleClick(const MouseEvent& e)
{
  // The same inline editor as the handle's readout. An option-double-click is
  // two resets, as it is on the readouts.
  const int h = handleAt(e.getPosition());
  if (h < 0 || e.mods.isAltDown())
    return;

  (h == 0 ? row.freqA : row.freqB).showEditor();
}

} // namespace design
