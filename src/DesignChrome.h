/*
  ==============================================================================

    DesignChrome.h

    Phase 6.2: the window furniture from the Figma redesign (node 6-669) --
    title bar, the global header row, and the footer. The FX rows and the
    spectrograms are not here; they are 6.3 and 6.7.

    Everything in this file paints itself. None of it uses a stock JUCE
    control, because the design's controls have no visible track, thumb or
    frame -- the text *is* the control.

  ==============================================================================
*/

#pragma once

#include "DesignPalette.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class DtBlkFxAudioProcessor;

namespace design {

//==============================================================================
/** Shared drag/menu/type behaviour for the header controls.

    All four global controls were one widget in the original -- a
    `DtPopupHSlider`, which dragged *and* offered a menu of named values (see
    `dtblkfx_src/GlobalCtrl.cpp`). This keeps that: drag either axis to change
    the value, right-click for the menu, double-click to type one in.
*/
class DraggableValue : public juce::Component {
public:
  DraggableValue(juce::RangedAudioParameter& p, juce::MouseCursor cursor);
  ~DraggableValue() override;

  void mouseEnter(const juce::MouseEvent&) override;
  void mouseExit(const juce::MouseEvent&) override;
  void mouseDown(const juce::MouseEvent&) override;
  void mouseDrag(const juce::MouseEvent&) override;
  void mouseUp(const juce::MouseEvent&) override;
  void mouseDoubleClick(const juce::MouseEvent&) override;

  bool isHovered() const { return hovered; }

protected:
  /** Entries for the right-click menu, as {value, label}. Rebuilt on each
      open, because the tempo-dependent ones go stale. */
  virtual std::vector<std::pair<float, juce::String>> menuEntries() { return {}; }

  /** Called when the menu's first entry is a command rather than a value.
      Returns true if handled. */
  virtual bool handleMenuCommand(int /*index*/) { return false; }

  juce::RangedAudioParameter& param;

private:
  void showMenu();
  void showEditor();

  bool hovered = false, dragging = false;
  float valueAtDragStart = 0.0f;
  std::unique_ptr<juce::TextEditor> editor;
};

//==============================================================================
/** The MixBack control (Figma node 6:470).

    Not a rotary: a circle with a purple level rising from the bottom, the
    percentage printed in the middle, and FILT / POWR set vertically either
    side. The lit word is the `power` boolean -- click either to switch.
*/
class MixBackKnob : public DraggableValue {
public:
  explicit MixBackKnob(DtBlkFxAudioProcessor& p);

  void paint(juce::Graphics& g) override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;

protected:
  std::vector<std::pair<float, juce::String>> menuEntries() override;

private:
  juce::Rectangle<int> filtBounds() const;
  juce::Rectangle<int> powrBounds() const;

  DtBlkFxAudioProcessor& processor;
  juce::SharedResourcePointer<FontStore> fonts;
  int wordHover = -1; // 0 = FILT, 1 = POWR
};

//==============================================================================
/** One of Delay / Ovrlp / BlkLen (Figma "Top Options", node 6:375).

    A 28px heading over a 10px readout, with a lock glyph top-right and -- for
    the two that carry a mode flag -- a beat-sync glyph beneath it.
*/
class GlobalHeading : public DraggableValue {
public:
  enum class Which { delay, overlap, blkLen };

  GlobalHeading(DtBlkFxAudioProcessor& p, Which which);

  void paint(juce::Graphics& g) override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;

  bool isLocked() const { return locked; }

protected:
  std::vector<std::pair<float, juce::String>> menuEntries() override;
  bool handleMenuCommand(int index) override;

private:
  int titleWidth() const;
  juce::Rectangle<int> lockBounds() const;
  juce::Rectangle<int> syncBounds() const;
  bool hasSyncGlyph() const { return which != Which::blkLen; }

  /** Flip beats/msec or the sync flag. The delay case deliberately recomputes
      the parameter so the delay keeps the same *length* -- that is what the
      original's menu entry 0 did, and a naive flag flip would jump the time. */
  void toggleMode();
  bool modeIsOn() const;

  DtBlkFxAudioProcessor& processor;
  Which which;
  juce::String title;
  juce::SharedResourcePointer<FontStore> fonts;
  juce::SharedResourcePointer<Glyphs> glyphs;

  bool locked = false;
  int glyphHover = -1; // 0 = lock, 1 = beat sync
};

//==============================================================================
/** The Win95 title bar: gradient, name, and a `?` that opens the manual. */
class TitleBar : public juce::Component {
public:
  TitleBar();

  void paint(juce::Graphics& g) override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;

private:
  juce::Rectangle<int> helpBounds() const;

  juce::SharedResourcePointer<FontStore> fonts;
  juce::SharedResourcePointer<Glyphs> glyphs;
  bool helpHover = false, helpDown = false;
};

//==============================================================================
/** The RANDOM button. Its label is rotated 180 degrees, which is deliberate. */
class RandomButton : public juce::Button {
public:
  RandomButton();
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
  juce::SharedResourcePointer<FontStore> fonts;
};

} // namespace design
