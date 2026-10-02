#include "DtBlkFxEditor.h"
#include "BinaryData.h" // Generated header
#include "DtBlkFxProcessor.h"
#include "core/BlkFxParam.h"
#include "core/DtBlkFx.hpp" // For GetFxRun1_0

namespace {

// Every readout in the editor comes from the parameter itself, so the GUI and
// the host can never disagree about what a value means. The two lambdas map
// between the slider's own range and the parameter's 0..1.
void useParamText(juce::Slider& s,
                  juce::AudioProcessorValueTreeState& apvts,
                  const juce::String& paramId,
                  std::function<double(double)> sliderToParam = [](double v) { return v; },
                  std::function<double(double)> paramToSlider = [](double v) { return v; })
{
  auto* param = apvts.getParameter(paramId);
  if (param == nullptr)
    return;

  s.textFromValueFunction = [param, sliderToParam](double v) {
    return param->getText((float)sliderToParam(v), 0);
  };
  s.valueFromTextFunction = [param, paramToSlider](const juce::String& t) {
    return paramToSlider((double)param->getValueForText(t));
  };
  s.updateText();
}

} // namespace

//==============================================================================
HeaderComponent::HeaderComponent(DtBlkFxAudioProcessor& p, design::LockHoverState& lockHover)
    : knob(p)
    , delayHeading(p, design::GlobalHeading::Which::delay, lockHover)
    , overlapHeading(p, design::GlobalHeading::Which::overlap, lockHover)
    , blkLenHeading(p, design::GlobalHeading::Which::blkLen, lockHover)
{
  for (auto* c : {(juce::Component*)&knob,
                  (juce::Component*)&delayHeading,
                  (juce::Component*)&overlapHeading,
                  (juce::Component*)&blkLenHeading})
    addAndMakeVisible(c);
}

bool HeaderComponent::isLocked(int index) const
{
  switch (index) {
    case 1:
      return delayHeading.isLocked();
    case 2:
      return blkLenHeading.isLocked();
    case 3:
      return overlapHeading.isLocked();
    default:
      return false;
  }
}

void HeaderComponent::resized()
{
  // Figma node 6:464: the gauge block is 86 wide on the left, then the three
  // headings, spread. BlkLen is wider than the other two because its readout
  // is the longest string in the row.
  // Widths are the design's; the row is space-between, so whatever is left
  // over after them is split evenly into the three gaps.
  auto area = getLocalBounds();
  const int widths[]{86, 124, 124, 145};
  const int gap = juce::jmax(0, (area.getWidth() - (widths[0] + widths[1] + widths[2] + widths[3])) / 3);

  knob.setBounds(area.removeFromLeft(widths[0]));

  juce::Component* headings[]{&delayHeading, &overlapHeading, &blkLenHeading};
  for (int i = 0; i < 3; ++i) {
    area.removeFromLeft(gap);
    headings[i]->setBounds(area.removeFromLeft(widths[i + 1]));
  }
}

//==============================================================================
//==============================================================================
DtBlkFxEditor::LimiterComponent::LimiterComponent(DtBlkFxAudioProcessor& p)
    : processor(p)
{
  auto& apvts = p.apvts;

  auto setupSlider = [&](juce::Slider& s,
                         const juce::String& paramId,
                         const juce::String& name,
                         const juce::String& suffix) {
    addAndMakeVisible(s);
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 14);
    s.setTextValueSuffix(suffix);
  };

  setupSlider(ceilingSlider, DtBlkFxAudioProcessor::limiterCeilingId, "Ceiling", " dB");
  setupSlider(gainSlider, DtBlkFxAudioProcessor::limiterGainId, "Gain", " dB");
  setupSlider(releaseSlider, DtBlkFxAudioProcessor::limiterReleaseId, "Release", " ms");

  addAndMakeVisible(enableButton);
  enableButton.setButtonText("Limiter");

  // Labels
  auto setupLabel = [&](juce::Label& l, const juce::String& text, juce::Component& target) {
    addAndMakeVisible(l);
    l.setText(text, juce::dontSendNotification);
    l.setFont(fonts->pixel(10.0f));
    l.setJustificationType(juce::Justification::centred);
    l.attachToComponent(&target, false);
  };

  setupLabel(ceilingLabel, "Ceiling", ceilingSlider);
  setupLabel(gainLabel, "Gain", gainSlider);
  setupLabel(releaseLabel, "Release", releaseSlider);

  // Attachments
  ceilingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
      apvts, DtBlkFxAudioProcessor::limiterCeilingId, ceilingSlider);
  gainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
      apvts, DtBlkFxAudioProcessor::limiterGainId, gainSlider);
  releaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
      apvts, DtBlkFxAudioProcessor::limiterReleaseId, releaseSlider);
  enableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
      apvts, DtBlkFxAudioProcessor::limiterEnabledId, enableButton);
}

void DtBlkFxEditor::LimiterComponent::paint(juce::Graphics& g)
{
  g.fillAll(design::colour::windowBg);
  RetroLookAndFeel::drawBevel(g, getLocalBounds(), true);
}

void DtBlkFxEditor::LimiterComponent::resized()
{
  auto area = getLocalBounds().reduced(10);

  // Layout: [Enable] [Gain] [Ceiling] [Release]
  int w = area.getWidth() / 4;

  enableButton.setBounds(area.removeFromLeft(w).reduced(10, 20));
  gainSlider.setBounds(area.removeFromLeft(w).reduced(5));
  ceilingSlider.setBounds(area.removeFromLeft(w).reduced(5));
  releaseSlider.setBounds(area.removeFromLeft(w).reduced(5));
}

//==============================================================================
DtBlkFxEditor::FooterComponent::FooterComponent(DtBlkFxEditor& e)
    : owner(e)
    , randomizeButton(e.lockHover)
{
  addAndMakeVisible(randomizeButton);
  randomizeButton.onClick = [&] { owner.startRandomization(); };

  // The smooth slider and the limiter are not in the design (docs/PHASE6.md,
  // 6.2). They stay constructed and wired so they can be re-added later --
  // they are simply not made visible or given bounds.
  smoothSlider.setSliderStyle(juce::Slider::LinearHorizontal);
  smoothSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 15);
  smoothSlider.setRange(0.0, 10.0, 0.1);
  smoothSlider.setValue(1.0); // Default 1s
  smoothSlider.setTooltip("Interpolation Time (s)");

  smoothLabel.setText("Smooth (s):", juce::dontSendNotification);

  addAndMakeVisible(presetBox);
  // The two "Factory" entries were placeholders implying a bank we do not have
  // yet; the original's 43 presets arrive in Phase 8.
  presetBox.addItem("Init", 1);
  presetBox.addItem("Random", 2);
  presetBox.addSeparator();
  presetBox.addItem("Save Preset...", 100);
  presetBox.addItem("Load Preset...", 101);
  presetBox.setText("Presets");

  presetBox.onChange = [&] {
    int id = presetBox.getSelectedId();
    if (id == 1)
      owner.loadInitPreset();
    else if (id == 2)
      owner.startRandomization(); // Random
    else if (id == 100)
      owner.savePreset();
    else if (id == 101)
      owner.loadPreset();

    presetBox.setText("Presets"); // Reset text
  };
}

void DtBlkFxEditor::FooterComponent::paint(juce::Graphics& g)
{
  g.fillAll(design::colour::windowBg);
}

void DtBlkFxEditor::FooterComponent::resized()
{
  // Figma node 6:673: presets 128x26 inset 9 from the left, RANDOM 97x23 inset
  // 9 from the right, both centred vertically in the 44px strip.
  presetBox.setBounds(9, (getHeight() - 26) / 2, 128, 26);
  randomizeButton.setBounds(getWidth() - 9 - 97, (getHeight() - 23) / 2, 97, 23);
}

//==============================================================================
void DtBlkFxEditor::startRandomization()
{
  float duration = (float)footer.smoothSlider.getValue();

  startValues.clear();
  targetValues.clear();

  auto& params = audioProcessor.getParameters();
  juce::Random rng;

  for (auto* p : params) {
    if (auto* param = dynamic_cast<juce::AudioProcessorParameterWithID*>(p)) {
      // Skip Limiter parameters
      if (param->paramID.startsWith("limiter")) {
        continue;
      }

      // Locks are per global control / per FX row. The globals no longer all
      // have a "param_<n>" id, so the unpacked halves map back by name.
      int paramIndex = -1;
      if (param->paramID == DtBlkFxAudioProcessor::mixBackId ||
          param->paramID == DtBlkFxAudioProcessor::powerId)
        paramIndex = 0;
      else if (param->paramID == DtBlkFxAudioProcessor::overlapId ||
               param->paramID == DtBlkFxAudioProcessor::syncId)
        paramIndex = 3;
      else if (param->paramID.startsWith("param_"))
        paramIndex = param->paramID.fromFirstOccurrenceOf("param_", false, false).getIntValue();
      else
        continue;

      if (paramIndex < 4 && header.isLocked(paramIndex)) {
        continue; // Skip locked global params
      }

      // Check locks for FX rows
      if (paramIndex >= 4) {
        int fxIndex = (paramIndex - 4) / 5; // 5 params per row
        if (fxIndex >= 0 && fxIndex < paramRows.size() && paramRows[fxIndex]->isLocked()) {
          continue; // Skip locked FX row
        }
      }

      startValues[param->paramID] = param->getValue();
      targetValues[param->paramID] = rng.nextFloat();
    }
  }

  if (duration > 0.0f) {
    isInterpolating = true;
    interpolationTime = 0.0;
    interpolationDuration = duration;
  }
  else {
    // Instant
    for (auto const& [id, val] : targetValues) {
      if (auto* param = audioProcessor.apvts.getParameter(id)) {
        param->setValueNotifyingHost(val);
      }
    }
  }
}

void DtBlkFxEditor::updateInterpolation()
{
  if (!isInterpolating)
    return;

  interpolationTime += 1.0 / 60.0;
  float progress = (float)(interpolationTime / interpolationDuration);

  if (progress >= 1.0f) {
    progress = 1.0f;
    isInterpolating = false;
  }

  for (auto const& [id, target] : targetValues) {
    if (auto* param = audioProcessor.apvts.getParameter(id)) {
      float start = startValues[id];
      float current = start + (target - start) * progress;
      param->setValueNotifyingHost(current);
    }
  }
}

void DtBlkFxEditor::savePreset()
{
  auto file = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                  .getChildFile("DtBlkFx_Presets")
                  .getNonexistentChildFile("Preset", ".xml");

  fileChooser = std::make_unique<juce::FileChooser>("Save Preset", file, "*.xml");
  fileChooser->launchAsync(
      juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
      [this](const juce::FileChooser& fc) {
        auto result = fc.getResult();
        if (result != juce::File{}) {
          auto xml = audioProcessor.apvts.copyState().createXml();
          xml->writeTo(result);
        }
      });
}

void DtBlkFxEditor::loadPreset()
{
  fileChooser = std::make_unique<juce::FileChooser>(
      "Load Preset",
      juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
          .getChildFile("DtBlkFx_Presets"),
      "*.xml");

  fileChooser->launchAsync(juce::FileBrowserComponent::openMode, [this](const juce::FileChooser& fc) {
    auto result = fc.getResult();
    if (result != juce::File{}) {
      auto xml = juce::XmlDocument::parse(result);
      if (xml)
        audioProcessor.apvts.replaceState(juce::ValueTree::fromXml(*xml));
    }
  });
}

void DtBlkFxEditor::loadInitPreset()
{
  // Init is a fresh instance: every parameter to its own default. It used to be
  // a hand-copied list of values, which drifted the moment a default changed.
  // The limiter stays out of it, as it does for RANDOM -- it is not part of the
  // original plugin and is not in the layout yet.
  startValues.clear();
  targetValues.clear();

  for (auto* p : audioProcessor.getParameters())
    if (auto* param = dynamic_cast<juce::RangedAudioParameter*>(p))
      if (!param->paramID.startsWith("limiter")) {
        startValues[param->paramID] = param->getValue();
        targetValues[param->paramID] = param->getDefaultValue();
      }

  // Trigger interpolation (short)
  isInterpolating = true;
  interpolationTime = 0.0;
  interpolationDuration = 0.5; // 0.5s transition for presets
}

//==============================================================================
DtBlkFxEditor::DtBlkFxEditor(DtBlkFxAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , audioProcessor(p)
    , header(p, lockHover)
    , inputSpectrogram("Input")
    , outputSpectrogram("Output")
    , footer(*this)
    , limiter(p)
{
  setLookAndFeel(&retroLnF);

  // Gives clicks on the window background somewhere to move focus to, which is
  // what dismisses an open inline editor. See DraggableValue's constructor.
  setWantsKeyboardFocus(true);

  addAndMakeVisible(titleBar);
  addAndMakeVisible(header);
  addAndMakeVisible(inputSpectrogram);
  addAndMakeVisible(outputSpectrogram);
  addAndMakeVisible(footer);
  // `limiter` stays constructed and attached to its parameters, but the design
  // has no panel for it -- see docs/PHASE6.md, 6.2.

  // Channel Selectors Removed
  // The engine feeds both from the left channel only, so they do not claim
  // "L+R" as the design does.
  inputSpectrogram.setLinked(&outputSpectrogram);
  outputSpectrogram.setLinked(&inputSpectrogram);

  for (int i = 0; i < 8; ++i) {
    auto row = std::make_unique<design::FxRow>(p, i, lockHover);
    addAndMakeVisible(*row);
    paramRows.push_back(std::move(row));
  }

  setSize(windowWidth, windowHeight);
  shownMaskOutlines = maskOutlines();
  startTimerHz(60);
}

DtBlkFxEditor::~DtBlkFxEditor()
{
  stopTimer();
  paramRows.clear();
  setLookAndFeel(nullptr);
}

void DtBlkFxEditor::timerCallback()
{
  const double rate = audioProcessor.getSampleRate();

  if (audioProcessor.newInputSpectrogramDataAvailable) {
    juce::ScopedLock lock(audioProcessor.inputSpectrogramLock);
    inputSpectrogram.pushBlock(audioProcessor.inputSpectrogramData.data(),
                               (int)audioProcessor.inputSpectrogramData.size(),
                               rate);
    audioProcessor.newInputSpectrogramDataAvailable = false;
  }

  if (audioProcessor.newOutputSpectrogramDataAvailable) {
    juce::ScopedLock lock(audioProcessor.outputSpectrogramLock);
    outputSpectrogram.pushBlock(audioProcessor.outputSpectrogramData.data(),
                                (int)audioProcessor.outputSpectrogramData.size(),
                                rate);
    audioProcessor.newOutputSpectrogramDataAvailable = false;
  }

  // The row whose range is being dragged, if any, shows that range inverted on
  // both spectrograms. Polled at the timer's 60Hz, which is quick enough to
  // follow a drag.
  float a = 0.0f, b = 0.0f;
  bool any = false;
  for (auto& row : paramRows)
    if ((any = row->rangeHighlight(a, b)))
      break;
  inputSpectrogram.setHighlight(any, a, b);
  outputSpectrogram.setHighlight(any, a, b);

  updateInterpolation();

  // Readouts follow each other around -- the effect type changes what its row
  // reads, the delay changes what BlkLen and Overlap read -- so rather than
  // wiring every dependency by hand, refresh the lot a few times a second.
  // updateText() is a no-op when the string has not changed.
  if (++textRefreshTick >= 6) {
    textRefreshTick = 0;
    header.refreshTexts();
    for (auto& row : paramRows)
      row->refresh();

    // Repaint the outlines only when one actually changed -- repainting the
    // rows' area redraws every row's glyphs.
    if (auto outlines = maskOutlines(); outlines != shownMaskOutlines) {
      shownMaskOutlines = std::move(outlines);
      if (!paramRows.empty())
        repaint(paramRows.front()->getBounds()
                    .getUnion(paramRows.back()->getBounds())
                    .expanded(1));
    }
  }
}

void DtBlkFxEditor::paint(juce::Graphics& g)
{
  g.fillAll(design::colour::windowBg);
  RetroLookAndFeel::drawBevel(g, getLocalBounds(), true);
}

std::vector<int> DtBlkFxEditor::maskOutlines() const
{
  // Figma 6:662: a mask row and the row below it get a solid purple outline.
  // Solid when the mask is actually shaping something; faint when the pairing
  // exists but does nothing -- the mask bypassed, the row below bypassed or on
  // an effect that ignores masks, or no row below at all. That second case is
  // the roadmap's "masks look broken": they were silently doing nothing.
  std::vector<int> state(paramRows.size(), 0);

  for (size_t i = 0; i < paramRows.size(); ++i) {
    auto* fx = paramRows[i]->effect();
    if (fx == nullptr || !fx->isMask())
      continue;

    const bool live = paramRows[i]->isOn() && i + 1 < paramRows.size() &&
                      paramRows[i + 1]->isOn() &&
                      design::consumesMask(paramRows[i + 1]->effect());
    state[i] = live ? 2 : 1;
  }
  return state;
}

void DtBlkFxEditor::paintOverChildren(juce::Graphics& g)
{
  for (size_t i = 0; i < shownMaskOutlines.size(); ++i) {
    if (shownMaskOutlines[i] == 0)
      continue;

    auto area = paramRows[i]->getBounds();
    if (i + 1 < paramRows.size())
      area = area.getUnion(paramRows[i + 1]->getBounds());

    g.setColour(design::colour::selection.withAlpha(shownMaskOutlines[i] == 2 ? 1.0f : 0.3f));
    g.drawRect(area, 1);
  }
}

void DtBlkFxEditor::resized()
{
  // Geometry straight off Figma node 6-669. Laid out by removing bands from the
  // top rather than by absolute coordinates, so that making the window
  // resizable after 6.7 is a matter of changing the band sizes, not rewriting
  // this.
  auto area = getLocalBounds();

  titleBar.setBounds(area.removeFromTop(36).reduced(4));

  // The design leaves 26px under the title bar; that reads as a gap rather than
  // as padding at this size, so it is tightened here.
  area.removeFromTop(10);
  auto content = area.withTrimmedLeft(6).withTrimmedRight(6).withTrimmedBottom(8);

  // The footer is anchored to the bottom rather than flowed to it, so whatever
  // rounding is left over collects above it instead of below.
  footer.setBounds(content.removeFromBottom(44));

  // 46 rather than the design's 40: the readout needs more air under the
  // heading than the design allows, and the lock and sync glyphs need their
  // hover highlights to fit.
  header.setBounds(content.removeFromTop(46));
  content.removeFromTop(26);

  inputSpectrogram.setBounds(content.removeFromTop(177));
  content.removeFromTop(3);
  outputSpectrogram.setBounds(content.removeFromTop(177));
  content.removeFromTop(8);

  for (auto& row : paramRows) {
    row->setBounds(content.removeFromTop(40));
    content.removeFromTop(3);
  }
}
