/*
  ==============================================================================

    DesignRow.h

    Phase 6.3b: one FX row from the Figma redesign (component 5:297, four
    variants -- Default, Hover, Selected, Variant3).

    The row *is* the control. There is no slider, no combo box and no text box
    in it: four values you drag or type into, a grip between the two
    frequencies that moves both together, a bevelled effect picker, and the
    lock and power glyphs. Everything paints itself.

    Phase 6.6b adds what sits behind and above the cells: the amp wedge, the
    frequency window with its dimming overlays, and the two triangle handles,
    all laid out through DesignAxis.h so they line up with the spectrograms.

  ==============================================================================
*/

#pragma once

#include "DesignChrome.h"
#include "DesignPalette.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class DtBlkFxAudioProcessor;
class FxRun1_0;

namespace design {

//==============================================================================
/** One of a row's four value cells.

    All four were `DtPopupHSlider`s in the original -- the same widget as the
    global headings -- so they get the same behaviour from `DraggableValue`:
    drag either axis, right-click for a menu of named values, double-click to
    type one in.

    Only amp and value carry a menu. `FxCtrl` populated those two
    (`updateAmpMenu`, `updateFxValMenu`) and never populated one for the
    frequencies, and both are rebuilt whenever the row's effect changes,
    because what they offer depends on it.
*/
class RowValue : public DraggableValue {
public:
  enum class Which { freqA, freqB, amp, value };

  RowValue(DtBlkFxAudioProcessor& p, int set, Which which);

  void paint(juce::Graphics& g) override;

  /** Repaint only if what this cell reads has actually changed. */
  void refresh();

  /** Show the hover box regardless of the pointer -- the `↔` grip uses it on
      both frequency readouts, since it moves both. */
  void setLinked(bool shouldBe);

protected:
  std::vector<std::pair<float, juce::String>> menuEntries() override;
  void valueChanged() override;
  float dragPixels() const override;
  juce::Rectangle<int> editorBounds() const override;

private:
  DtBlkFxAudioProcessor& processor;
  juce::SharedResourcePointer<FontStore> fonts;
  int set;
  Which which;
  juce::String shown;
  bool linked = false;
};

//==============================================================================
/** The grip between the two frequency readouts (Figma `14:533`).

    Drags both frequencies at once by the same delta *in parameter space*, so
    the interval between them holds rather than the distance in Hz. This is the
    original's "drag both" gesture, which `FxCtrl::mouseInterceptBefore` bound
    to the right button or ctrl+left anywhere over the frequency area; giving it
    a visible grip of its own frees the right button for the value menus.
*/
class FreqLink : public juce::Component {
public:
  FreqLink(DtBlkFxAudioProcessor& p, int set);

  void paint(juce::Graphics& g) override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  void mouseEnter(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;

private:
  void showLinked(bool on);

  juce::RangedAudioParameter* freq[2]{};
  float valueAtDragStart[2]{};
  bool dragging = false, hovered = false;
};

//==============================================================================
/** The FX-type menu, Figma "Dropdown Menu" (3:130).

    Two columns: "Off" and the NORMAL effects on the left; MASK FX and STEREO FX
    on the right, dropped one row so the two column headers sit level. Engine
    order within each group, engine names throughout -- the manual uses them,
    and so does the host.

    `valueForResult[id - 1]` is the FX_TYPE value behind each result id. Free
    rather than a member so the test binary can render the real menu.
*/
juce::PopupMenu buildFxTypeMenu(int currentEffect, std::vector<float>& valueForResult);

/** Whether `fx`, running in the row below a mask, is shaped by that mask.

    Not an engine flag -- there is none. Every effect that processes through
    `MaskedRun` / `AutoHarmMaskRun` in FxRun1_0.cpp consumes the mask above it,
    and that is all of them except "Off", the masks themselves, Vocode and the
    two HarmMatch stereo effects. Those are named, from a brace-matched read of
    each effect class's body. */
bool consumesMask(FxRun1_0* fx);

//==============================================================================
/** The effect picker: a 128 x 26 bevelled cell that pops a menu.

    Deliberately not a `juce::ComboBox`. A ComboBox's frame can be restyled
    through the LookAndFeel but the menu it builds cannot, and 6.4 needs two
    columns with NORMAL / MASK FX / STEREO FX headers -- neither of which a
    ComboBox can express. This writes `FX_TYPE` itself, the way the original's
    `_fxtype_ctrl` did.
*/
class FxTypeCell : public juce::Component {
public:
  FxTypeCell(DtBlkFxAudioProcessor& p, int set);

  void paint(juce::Graphics& g) override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseEnter(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;

  /** Repaint only if the effect or the bypass flag has actually changed. */
  void refresh();

private:
  bool isOn() const;

  DtBlkFxAudioProcessor& processor;
  juce::RangedAudioParameter& param;
  juce::SharedResourcePointer<FontStore> fonts;
  int set;
  juce::String shown;
  bool shownOn = true, hovered = false, menuOpen = false;
};

//==============================================================================
/** A whole FX row.

    Paints, back to front: the amp wedge, the frequency window's dimming, the
    dashed border and the two glyphs. The cells sit on top of that, and the
    two frequency handles on top of the cells.
*/
class FxRow : public juce::Component {
public:
  FxRow(DtBlkFxAudioProcessor& p, int set, LockHoverState& lockHover);
  ~FxRow() override;

  void paint(juce::Graphics& g) override;
  void resized() override;

  void mouseEnter(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override;
  void mouseDown(const juce::MouseEvent& e) override;

  bool isLocked() const { return locked; }
  bool isOn() const;

  /** Pixels across the row's frequency axis -- what a frequency drag is
      scaled to, so it moves about as far as its handle does. */
  float freqAxisLength() const { return freqSpan().getLength(); }

  /** Both frequency readouts show their hover box -- for the `↔` grip. */
  void setFreqsLinked(bool on);

  /** True while the mouse is down on one of this row's range controls -- a
      frequency readout, the grip or a handle -- and the effect uses both
      frequencies. The spectrograms invert that range then, as the original's
      FxCtrl::doHiliteSgrams did. Hovering alone does not: it only dims. */
  bool rangeHighlight(float& a, float& b) const;

  /** The effect parked in the row -- what its parameter says, not what the
      engine is running, which is "Off" while the row is bypassed. */
  FxRun1_0* effect() const;

  /** Re-read everything from the parameters and repaint only what moved. Every
      change made from inside the row calls this at once; the editor's 10Hz poll
      calls it to pick up automation and changes from the host. */
  void refresh();

  // Figma 5:276. The four value cells share what is left after these.
  static constexpr int rightPad = 16;
  static constexpr int glyphStrip = 31; // 7 + 10 + 8, plus room for the hover pad
  static constexpr int dropdownWidth = 128;
  static constexpr int dropdownHeight = 26;
  static constexpr int linkWidth = 10;

private:
  /** Hover is row-wide, but a pointer moving onto a child sends the row a
      mouseExit -- so the children report too, and the row recomputes from the
      pointer position rather than trusting the direction of travel. */
  struct HoverRelay : public juce::MouseListener {
    explicit HoverRelay(FxRow& r)
        : row(r)
    {
    }

    void mouseEnter(const juce::MouseEvent&) override { row.updateHover(); }
    void mouseExit(const juce::MouseEvent&) override { row.updateHover(); }

    // Re-checked on the next message, not inside mouseUp: the drag state is
    // still settling while the release is being delivered, and if the pointer
    // was let go outside the row there is no later enter or exit to catch it.
    void mouseUp(const juce::MouseEvent&) override
    {
      juce::MessageManager::callAsync([safe = juce::Component::SafePointer<FxRow>(&row)] {
        if (safe != nullptr)
          safe->updateHover();
      });
    }

    FxRow& row;
  };

  /** The two frequency handles. A transparent layer over the whole row that
      only claims the pointer over a handle -- `hitTest` is false everywhere
      else, so clicks fall through to the cells and the row beneath. That is
      what lets a handle win where it overlaps a cell without the cells having
      to know about it. */
  struct HandleLayer : public juce::Component {
    explicit HandleLayer(FxRow& r);

    void paint(juce::Graphics& g) override;
    bool hitTest(int x, int y) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    /** 0 = min (A, bottom), 1 = max (B, top), -1 = neither. */
    int handleAt(juce::Point<int> p) const;

    FxRow& row;
    int hot = -1, dragging = -1;
    float grabOffset = 0.0f;
  };

  /** What is on screen, so a change repaints only the strip it moved across.
      `paint` draws from this rather than from the parameters, so a partial
      repaint can never show half of an old window and half of a new one. */
  struct Shown {
    float a = 0.0f, b = 1.0f, amp = 0.0f;
    bool usesA = false, usesB = false, usesAmp = false, on = true;
  };

  Shown current() const;
  void visualsChanged();
  void updateHover();

  juce::Range<float> freqSpan() const;
  float freqX(int handle) const;
  juce::Rectangle<float> handleBounds(int handle) const;
  juce::Rectangle<int> lockBounds() const;
  juce::Rectangle<int> powerBounds() const;
  juce::RangedAudioParameter* fxParam(int fxParamIndex) const;

  DtBlkFxAudioProcessor& processor;
  LockHoverState& lockHover;
  int set;

  juce::SharedResourcePointer<FontStore> fonts;
  juce::SharedResourcePointer<Glyphs> glyphs;

  RowValue freqA, freqB, amp, value;
  FreqLink link;
  FxTypeCell type;
  HandleLayer handles{*this};
  HoverRelay hoverRelay{*this};

  Shown shown;

  // True while the pointer is on -- or dragging -- something that moves the
  // frequency range: a frequency readout, the grip, or a handle. Only that
  // brings the dimming up to full strength; the rest of the row does not.
  bool rangeHot = false;
  bool locked = false;
  int glyphHover = -1; // 0 = lock, 1 = power
};

} // namespace design
