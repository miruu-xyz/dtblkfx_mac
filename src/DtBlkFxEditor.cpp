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
ParameterRowComponent::ParameterRowComponent(DtBlkFxAudioProcessor& p,
                                             int index,
                                             design::LockHoverState& lh)
    : processor(p)
    , lockHover(lh)
    , rowIndex(index)
{
  setLookAndFeel(&retroLnF);
  auto& apvts = p.apvts;

  int baseIndex = BlkFxParam::NUM_GLOBAL_PARAMS + index * BlkFxParam::NUM_FX_PARAMS;

  auto setupSlider = [&](juce::Slider& s, const juce::String& paramId, bool isKnob) {
    addAndMakeVisible(s);
    if (isKnob)
      s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    else
      s.setSliderStyle(juce::Slider::LinearHorizontal);

    s.setTextBoxStyle(
        juce::Slider::TextBoxBelow, false, 50, 14); // Wider and slightly taller text box
  };

  setupSlider(freqASlider, "param_" + juce::String(baseIndex + 0), false); // Slider
  setupSlider(freqBSlider, "param_" + juce::String(baseIndex + 1), false); // Slider
  setupSlider(ampSlider, "param_" + juce::String(baseIndex + 2), true);    // Knob
  setupSlider(valSlider, "param_" + juce::String(baseIndex + 4), true);    // Knob

  // Fine Slider (Not attached to parameter directly)
  addAndMakeVisible(valFineSlider);
  valFineSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
  valFineSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 40, 14);
  valFineSlider.setRange(-0.1, 0.1, 0.001);
  valFineSlider.setValue(0.0);
  valFineSlider.setTextValueSuffix("");
  valFineSlider.setDoubleClickReturnValue(true, 0.0);

  valFineSlider.onValueChange = [this] {
    // Adjust main slider based on fine slider movement
    // This is a bit tricky because we want "Fine" to be an offset, but we don't want to accumulate
    // drift. Let's make it simple: Fine slider adds to the current value of the main slider when
    // moved. Actually, a better UX for "Fine" knob is: It resets to 0 on release? No. It just acts
    // as a +/- 0.1 modifier. Let's try this: We can't easily modify the main slider while dragging
    // the fine slider without feedback loops if we aren't careful. But since fine slider isn't
    // attached, it's fine.

    // Alternative: Fine slider is just a visual representation of a fine offset?
    // No, let's make it apply the delta.
    static double lastFineValue = 0.0;
    double currentFine = valFineSlider.getValue();
    double delta = currentFine - lastFineValue;
    lastFineValue = currentFine;

    if (std::abs(delta) > 0.00001) {
      valSlider.setValue(valSlider.getValue() + delta);
    }
  };

  valFineSlider.onDragStart = [this] {
    // Reset delta tracking
    // We might want to reset the knob to 0 on drag start?
    // Or just keep it relative.
    // Let's reset it to 0 on drag start to act as a "nudge" tool.
    valFineSlider.setValue(0.0, juce::dontSendNotification);
  };

  addAndMakeVisible(typeBox);
  // Populate effects dynamically
  for (int i = 0; i < g_num_fx_1_0; ++i) {
    if (auto* fx = GetFxRun1_0(i)) {
      typeBox.addItem(fx->name(), i + 1); // IDs are 1-based
    }
  }

  // Labels (Only on first row)
  if (rowIndex == 0) {
    auto setupLabel = [&](juce::Label& l, const juce::String& text) {
      addAndMakeVisible(l);
      l.setText(text, juce::dontSendNotification);
      l.setFont(retroLnF.fonts->pixel(9.0f));
      l.setJustificationType(juce::Justification::centred);
    };

    setupLabel(freqALabel, "Freq A");
    setupLabel(freqBLabel, "Freq B");
    setupLabel(ampLabel, "Amp");
    setupLabel(valLabel, "Val");
    setupLabel(valFineLabel, "Fine");
  }

  try {
    freqAAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "param_" + juce::String(baseIndex + 0), freqASlider);
    freqBAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "param_" + juce::String(baseIndex + 1), freqBSlider);
    ampAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "param_" + juce::String(baseIndex + 2), ampSlider);
    valAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "param_" + juce::String(baseIndex + 4), valSlider);
    typeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "param_" + juce::String(baseIndex + 3), typeBox);
  }
  catch (...) {
  }

  // Readouts, straight from the parameters. Amp switches between dB and a mix
  // percentage, frequencies snap to the real FFT bin, and a control the
  // current effect does not use reads "-" -- all decided by the engine, so
  // hand-rolling any of it here just makes the GUI disagree with the host.
  useParamText(freqASlider, apvts, "param_" + juce::String(baseIndex + BlkFxParam::FX_FREQ_A));
  useParamText(freqBSlider, apvts, "param_" + juce::String(baseIndex + BlkFxParam::FX_FREQ_B));
  useParamText(ampSlider, apvts, "param_" + juce::String(baseIndex + BlkFxParam::FX_AMP));
  useParamText(valSlider, apvts, "param_" + juce::String(baseIndex + BlkFxParam::FX_VAL));
  // Lock Button
  addAndMakeVisible(lockButton);
  lockButton.setButtonText("Lock");
  lockButton.addMouseListener(this, false);

  // On/Off Button
  addAndMakeVisible(onOffButton);
  onOffButton.setButtonText("On");
  onOffButton.setToggleState(true, juce::dontSendNotification); // Default On

  // Handle On/Off logic
  onOffButton.onClick = [this] {
    if (onOffButton.getToggleState()) {
      // Turned ON: Restore last active type
      typeBox.setSelectedId(lastActiveTypeId, juce::sendNotification);
      onOffButton.setButtonText("On");
    }
    else {
      // Turned OFF: Store current type and set to "Off"
      int currentId = typeBox.getSelectedId();
      // Find "Off" ID
      int offId = -1;
      for (int i = 0; i < typeBox.getNumItems(); ++i) {
        if (typeBox.getItemText(i) == "Off") {
          offId = typeBox.getItemId(i);
          break;
        }
      }

      if (offId != -1 && currentId != offId) {
        lastActiveTypeId = currentId;
        typeBox.setSelectedId(offId, juce::sendNotification);
      }
      onOffButton.setButtonText("Off");
    }
  };

  // Update On/Off state when Type changes externally
  typeBox.onChange = [this] {
    if (typeBox.getText() == "Off") {
      onOffButton.setToggleState(false, juce::dontSendNotification);
      onOffButton.setButtonText("Off");
    }
    else {
      onOffButton.setToggleState(true, juce::dontSendNotification);
      onOffButton.setButtonText("On");
      lastActiveTypeId = typeBox.getSelectedId();
    }
  };
}

ParameterRowComponent::~ParameterRowComponent()
{
  lockHover.set(this, false);
}

void ParameterRowComponent::mouseEnter(const juce::MouseEvent& e)
{
  if (e.eventComponent == &lockButton)
    lockHover.set(this, true);
}

void ParameterRowComponent::mouseExit(const juce::MouseEvent& e)
{
  if (e.eventComponent == &lockButton)
    lockHover.set(this, false);
}

void ParameterRowComponent::refreshTexts()
{
  // The effect type decides what the other four read, and the frequencies also
  // follow the block length, which lives on another component entirely.
  for (auto* s : {&freqASlider, &freqBSlider, &ampSlider, &valSlider})
    s->updateText();
}

void ParameterRowComponent::paint(juce::Graphics& g)
{
  // Rows alternate against the window grey. The design's dashed outline and
  // amp/frequency graphics replace this wholesale in Phase 6.6.
  g.fillAll(rowIndex % 2 == 0 ? design::colour::bevelLight.withAlpha(0.45f)
                              : design::colour::windowBg);
  RetroLookAndFeel::drawBevel(g, getLocalBounds(), false);
}

void ParameterRowComponent::resized()
{
  auto area = getLocalBounds().reduced(2);
  int w = area.getWidth() / 6; // 6 columns

  // Column 0: Buttons (On/Lock)
  auto btnArea = area.removeFromLeft(w).reduced(2);

  // Center vertically
  int btnHeight = 24;
  int btnY = btnArea.getY() + (btnArea.getHeight() - btnHeight) / 2;

  // If row 0, adjust for label alignment (sliders have labels at top)
  // Sliders take full height, labels are top 12px.
  // Let's just center them in the available space.

  auto rowRect = btnArea.withY(btnY).withHeight(btnHeight);
  onOffButton.setBounds(rowRect.removeFromLeft(40));
  lockButton.setBounds(rowRect.removeFromRight(50));

  auto layoutItem = [&](juce::Component& ctrl, juce::Label* label) {
    auto r = area.removeFromLeft(w).reduced(2);

    // Always reserve space for label/spacer to keep alignment consistent across rows
    auto labelArea = r.removeFromTop(12);
    if (rowIndex == 0 && label) {
      label->setBounds(labelArea);
    }

    if (auto* slider = dynamic_cast<juce::Slider*>(&ctrl)) {
      if (slider->getSliderStyle() == juce::Slider::LinearHorizontal) {
        // Slider: use full remaining space so TextBoxBelow aligns with knobs
        slider->setBounds(r);
      }
      else {
        // Knob: keep square aspect ratio, but ensure it sits at the bottom or fills height
        // to align text boxes with sliders.
        int knobSize = juce::jmin(r.getWidth(), r.getHeight());
        // Center horizontally, but keep vertical fill to align text
        slider->setBounds(r.withWidth(knobSize).withX(r.getX() + (r.getWidth() - knobSize) / 2));
      }
    }
    else {
      // ComboBox (Type)
      // Visual height is approx r.getHeight() - 14.
      int dropdownHeight = 24;
      int visualHeight = r.getHeight() - 14;
      int y = r.getY() + (visualHeight - dropdownHeight) / 2;
      ctrl.setBounds(r.getX(), y, r.getWidth(), dropdownHeight);
    }
  };

  layoutItem(freqASlider, &freqALabel);
  layoutItem(freqBSlider, &freqBLabel);
  layoutItem(ampSlider, &ampLabel);
  layoutItem(typeBox, nullptr);

  // Split last column for Val and Fine
  auto valArea = area.removeFromLeft(w).reduced(2);

  // Split valArea into Coarse (70%) and Fine (30%)
  int fineWidth = valArea.getWidth() * 0.35f;
  int coarseWidth = valArea.getWidth() - fineWidth;

  auto coarseRect = valArea.removeFromLeft(coarseWidth);
  auto fineRect = valArea;

  // Layout Coarse
  auto coarseLabelArea = coarseRect.removeFromTop(12);
  if (rowIndex == 0)
    valLabel.setBounds(coarseLabelArea);

  int coarseKnobSize = juce::jmin(coarseRect.getWidth(), coarseRect.getHeight());
  valSlider.setBounds(coarseRect.withWidth(coarseKnobSize)
                          .withX(coarseRect.getX() + (coarseRect.getWidth() - coarseKnobSize) / 2));

  // Layout Fine
  auto fineLabelArea = fineRect.removeFromTop(12);
  if (rowIndex == 0)
    valFineLabel.setBounds(fineLabelArea);

  int fineKnobSize = juce::jmin(fineRect.getWidth(), fineRect.getHeight());
  valFineSlider.setBounds(fineRect.withWidth(fineKnobSize)
                              .withX(fineRect.getX() + (fineRect.getWidth() - fineKnobSize) / 2));
}

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
      owner.loadFactoryPreset(0); // Init
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

void DtBlkFxEditor::loadFactoryPreset(int index)
{
  startValues.clear();
  targetValues.clear();

  auto setById = [&](const juce::String& paramID, float val) {
    targetValues[paramID] = val;
    if (auto* p = audioProcessor.apvts.getParameter(paramID))
      startValues[paramID] = p->getValue();
  };
  auto setParam = [&](int id, float val) {
    const auto paramID = DtBlkFxAudioProcessor::paramId(id);
    if (paramID.isNotEmpty())
      setById(paramID, val);
  };

  // Reset all first
  for (int i = 0; i < BlkFxParam::TOTAL_NUM; ++i) {
    setParam(i, 0.0f); // Default 0
  }
  setById(DtBlkFxAudioProcessor::mixBackId, 0.0f);
  setById(DtBlkFxAudioProcessor::powerId, 1.0f);
  setById(DtBlkFxAudioProcessor::overlapId, 0.0f);
  setById(DtBlkFxAudioProcessor::syncId, 0.0f);

  // Apply specific settings
  if (index == 0) { // Init
    setById(DtBlkFxAudioProcessor::mixBackId, 0.0f); // Mix Dry
    setParam(2, 0.5f);                               // BlkLen
    setById(DtBlkFxAudioProcessor::overlapId, 1.0f); // Overlap
  }
  else if (index == 1) {                             // Vocoder-ish
    setById(DtBlkFxAudioProcessor::mixBackId, 1.0f); // Wet
    setParam(2, 0.7f);                               // BlkLen
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
    , inputSpectrogram("Input Left")
    , outputSpectrogram("Output Left")
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
  inputSpectrogram.setLabel("Input L+R");
  outputSpectrogram.setLabel("Output L+R");

  for (int i = 0; i < 8; ++i) {
    auto row = std::make_unique<ParameterRowComponent>(p, i, lockHover);
    addAndMakeVisible(*row);
    paramRows.push_back(std::move(row));
  }

  setSize(windowWidth, windowHeight);
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
  if (audioProcessor.newInputSpectrogramDataAvailable) {
    juce::ScopedLock lock(audioProcessor.inputSpectrogramLock);
    inputSpectrogram.processPendingData(audioProcessor.inputSpectrogramData.data(),
                                        (int)audioProcessor.inputSpectrogramData.size());
    audioProcessor.newInputSpectrogramDataAvailable = false;
  }

  if (audioProcessor.newOutputSpectrogramDataAvailable) {
    juce::ScopedLock lock(audioProcessor.outputSpectrogramLock);
    outputSpectrogram.processPendingData(audioProcessor.outputSpectrogramData.data(),
                                         (int)audioProcessor.outputSpectrogramData.size());
    audioProcessor.newOutputSpectrogramDataAvailable = false;
  }

  updateInterpolation();

  // Readouts follow each other around -- the effect type changes what its row
  // reads, the delay changes what BlkLen and Overlap read -- so rather than
  // wiring every dependency by hand, refresh the lot a few times a second.
  // updateText() is a no-op when the string has not changed.
  if (++textRefreshTick >= 6) {
    textRefreshTick = 0;
    header.refreshTexts();
    for (auto& row : paramRows)
      row->refreshTexts();
  }
}

void DtBlkFxEditor::paint(juce::Graphics& g)
{
  g.fillAll(design::colour::windowBg);
  RetroLookAndFeel::drawBevel(g, getLocalBounds(), true);
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
