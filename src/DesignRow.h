/*
  ==============================================================================

    DesignRow.h

    Phase 6.3b: one FX row from the Figma redesign (component 5:297, four
    variants -- Default, Hover, Selected, Variant3).

    The row *is* the control. There is no slider, no combo box and no text box
    in it: four values you drag or type into, a grip between the two
    frequencies that moves both together, a bevelled effect picker, and the
    lock and power glyphs. Everything paints itself.

    Not here yet: the amp wedge and the frequency window with its dimming
    overlays and triangle handles. Both need the shared Hz-to-pixel mapping
    from 6.5 and land in 6.6.

  ==============================================================================
*/

#pragma once

#include "DesignChrome.h"
#include "DesignPalette.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class DtBlkFxAudioProcessor;

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

protected:
  std::vector<std::pair<float, juce::String>> menuEntries() override;

private:
  DtBlkFxAudioProcessor& processor;
  juce::SharedResourcePointer<FontStore> fonts;
  int set;
  Which which;
  juce::String shown;
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

    Paints the dashed border and the two glyphs; everything else is a child.
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

  /** Re-read everything from the parameters. The effect type decides what the
      other four cells mean, and the frequencies also follow the block length,
      which lives on another component entirely. */
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

    FxRow& row;
  };

  void updateHover();
  bool isOn() const;
  juce::Rectangle<int> lockBounds() const;
  juce::Rectangle<int> powerBounds() const;

  DtBlkFxAudioProcessor& processor;
  LockHoverState& lockHover;
  int set;

  juce::SharedResourcePointer<FontStore> fonts;
  juce::SharedResourcePointer<Glyphs> glyphs;

  RowValue freqA, freqB, amp, value;
  FreqLink link;
  FxTypeCell type;
  HoverRelay hoverRelay{*this};

  // `hovered` drives nothing yet. The design's hover variant is mostly the
  // frequency window's two dimming overlays coming up to full strength, and
  // those arrive in 6.6; the picker deliberately does not follow it.
  bool hovered = false, locked = false, shownOn = true;
  int glyphHover = -1; // 0 = lock, 1 = power
};

} // namespace design
