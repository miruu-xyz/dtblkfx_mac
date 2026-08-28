#pragma once

#include "DtBlkFxProcessor.h"
#include "DesignChrome.h"
#include "SpectrogramComponent.h"
#include <juce_gui_basics/juce_gui_basics.h>

class DtBlkFxAudioProcessor;

#include "RetroLookAndFeel.h"

//==============================================================================
/** The global header row (Figma node 6:464): the MixBack gauge with its
    FILT/POWR words, then Delay, Ovrlp and BlkLen. Every control in it paints
    itself -- see src/DesignChrome.h. */
class HeaderComponent : public juce::Component {
public:
  HeaderComponent(DtBlkFxAudioProcessor& p, design::LockHoverState& lockHover);

  void resized() override;

  /** Lock state for randomisation, indexed the way startRandomization() groups
      the globals: 0 mixback/power, 1 delay, 2 blklen, 3 overlap/sync. The
      design gives the gauge no lock, so 0 is never locked. */
  bool isLocked(int index) const;

  /** The headings read straight from their parameters when they paint, so a
      refresh is just a repaint. */
  void refreshTexts() { repaint(); }

private:
  design::MixBackKnob knob;
  design::GlobalHeading delayHeading, overlapHeading, blkLenHeading;
};

//==============================================================================
class ParameterRowComponent : public juce::Component {
public:
  ParameterRowComponent(DtBlkFxAudioProcessor& p, int index, design::LockHoverState& lockHover);
  ~ParameterRowComponent() override;

  void paint(juce::Graphics& g) override;
  void resized() override;

  // Reports the row's lock to the shared hover signal, so hovering it outlines
  // RANDOM like the header locks do. 6.3 replaces this button with the design's
  // glyph, which will report directly.
  void mouseEnter(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;

  bool isLocked() const { return lockButton.getToggleState(); }

  // Re-read every slider's text from its parameter.
  void refreshTexts();

  juce::ToggleButton lockButton;
  juce::ToggleButton onOffButton;
  int lastActiveTypeId = 1; // Default to 1 (first effect) if not Off

private:
  DtBlkFxAudioProcessor& processor;
  design::LockHoverState& lockHover;
  int rowIndex;
  juce::Slider freqASlider, freqBSlider, ampSlider, valSlider, valFineSlider;
  juce::ComboBox typeBox;

  juce::Label freqALabel, freqBLabel, ampLabel, valLabel, valFineLabel;

  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> freqAAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> freqBAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> ampAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> valAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttachment;

  RetroLookAndFeel retroLnF;
};

//==============================================================================
class DtBlkFxEditor
    : public juce::AudioProcessorEditor
    , public juce::Timer {
public:
  DtBlkFxEditor(DtBlkFxAudioProcessor&);
  ~DtBlkFxEditor() override;

  void paint(juce::Graphics&) override;
  void resized() override;

  void timerCallback() override;

  // Randomization & Presets
  void startRandomization();
  void updateInterpolation();
  void savePreset();
  void loadPreset();
  void loadFactoryPreset(int index);

  struct FooterComponent : public juce::Component {
    FooterComponent(DtBlkFxEditor& editor);
    ~FooterComponent() override = default;

    void resized() override;
    void paint(juce::Graphics& g) override;

    DtBlkFxEditor& owner;
    design::RandomButton randomizeButton;
    juce::Slider smoothSlider;
    juce::Label smoothLabel;
    juce::ComboBox presetBox;
  };

  struct LimiterComponent : public juce::Component {
    LimiterComponent(DtBlkFxAudioProcessor& p);
    ~LimiterComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    DtBlkFxAudioProcessor& processor;
    juce::SharedResourcePointer<design::FontStore> fonts;
    juce::Slider ceilingSlider, gainSlider, releaseSlider;
    juce::ToggleButton enableButton;
    juce::Label ceilingLabel, gainLabel, releaseLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> ceilingAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> releaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachment;
  };

  // Figma node 6-669. Fixed until 6.7, then revisited -- the design is built
  // on auto-layout and is meant to become resizable.
  static constexpr int windowWidth = 652;
  static constexpr int windowHeight = 912;

private:
  DtBlkFxAudioProcessor& audioProcessor;

  // Declared before `header` and `footer`: both take a reference to it.
  design::LockHoverState lockHover;

  // Owns the in-flight chooser so it survives past savePreset()/loadPreset()
  // returning -- launchAsync's callback fires later, on the message thread.
  std::unique_ptr<juce::FileChooser> fileChooser;

  design::TitleBar titleBar;
  HeaderComponent header;
  FooterComponent footer;
  LimiterComponent limiter;

  SpectrogramComponent inputSpectrogram;
  SpectrogramComponent outputSpectrogram;

  // Interpolation State
  bool isInterpolating = false;
  int textRefreshTick = 0;
  double interpolationTime = 0.0;
  double interpolationDuration = 0.0;
  std::map<juce::String, float> startValues;
  std::map<juce::String, float> targetValues;

  juce::ComboBox inputChannelSelector;
  juce::ComboBox outputChannelSelector;

  std::vector<std::unique_ptr<ParameterRowComponent>> paramRows;

  RetroLookAndFeel retroLnF;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DtBlkFxEditor)
};
