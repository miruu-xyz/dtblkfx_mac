/*
 * See LICENSE.md for copyright and licensing information.
 *
 * This file is part of DtBlkFx.
 *
 * DtBlkFx is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DtBlkFx is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DtBlkFx.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "DtBlkFxProcessor.h"
#include "DtBlkFxEditor.h"
#include "FxRun1_0.h"
#include "FxState1_0.h"
#include "NoteFreq.h"
#include "rfftw_float.h"
#include <limits>

DtBlkFxAudioProcessor::DtBlkFxAudioProcessor()
    : AudioProcessor(BusesProperties()
#if !JucePlugin_IsMidiEffect
#  if !JucePlugin_IsSynth
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
#  endif
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
                         )
    , apvts(*this, nullptr, "Parameters", createParameterLayout())
{
  static bool initialized = false;
  if (!initialized) {
    CreateFFTWfPlans();
    initialized = true;
  }

  core = new DtBlkFx(nullptr);
  core->setSampleRate(getSampleRate());
  core->setBlockSize(getBlockSize());

  core->inputSpectrogramCallback = [this](const float* data, int numBins) {
    pushInputSpectrogramData(data, numBins);
  };
  core->outputSpectrogramCallback = [this](const float* data, int numBins) {
    pushOutputSpectrogramData(data, numBins);
  };

  // Add listeners and sync the engine to the initial parameter values.
  for (int i = 0; i < BlkFxParam::TOTAL_NUM; ++i) {
    const auto id = paramId(i);
    if (id.isEmpty())
      continue; // replaced by a pair, handled below
    apvts.addParameterListener(id, this);
    core->setParameter(i, apvts.getRawParameterValue(id)->load());
  }

  for (auto* id : {mixBackId, powerId, overlapId, syncId})
    apvts.addParameterListener(id, this);

  // One call each is enough: parameterChanged reads the partner itself.
  parameterChanged(mixBackId, apvts.getRawParameterValue(mixBackId)->load());
  parameterChanged(overlapId, apvts.getRawParameterValue(overlapId)->load());
}

// --- text helpers -------------------------------------------------------------
//
// The engine prints its own parameter text (see DtBlkFx::getParamDisplayGlobal
// and FxState1_0::getParamDisplay) and that is what the host shows, so the
// readouts match the original plugin. These helpers only cover the inverse
// direction -- turning typed text back into a 0..1 parameter -- plus the two
// packed globals, which no longer reach the engine display code as one value.

namespace {

// "1.20sec" / "227msec" / "5.80usec" / a bare number of seconds.
double parseSeconds(const juce::String& text)
{
  const auto s = text.trim().toLowerCase();
  const double v = s.getDoubleValue();
  if (s.contains("usec") || s.contains("us"))
    return v * 1.0e-6;
  if (s.contains("msec") || s.contains("ms"))
    return v * 1.0e-3;
  return v;
}

// "440Hz" / "1.20kHz" / a bare number of Hz.
double parseHz(const juce::String& text)
{
  const auto s = text.trim().toLowerCase();
  const double v = s.getDoubleValue();
  return s.contains("khz") ? v * 1.0e3 : v;
}

// "50%" / "50" / "0.5" -> 0..1
float parseFraction(const juce::String& text)
{
  const auto s = text.trim();
  const double v = s.getDoubleValue();
  return juce::jlimit(0.0f, 1.0f, (float)(s.contains("%") || v > 1.0 ? v * 0.01 : v));
}

// A note name as NoteToTxt prints it ("c-4:+00", "c#4:-45"), or -1 if the text
// is not a note. NoteToTxt writes naturals as "c-4" but the lookup table is
// keyed "c4", so the filler dash comes back out first.
float noteTextToHz(const juce::String& text)
{
  auto s = text.trim().toLowerCase().removeCharacters(" ");
  if (s.isEmpty() || s[0] < 'a' || s[0] > 'g')
    return -1.0f;

  const int colon = s.indexOfChar(':');
  auto head = colon >= 0 ? s.substring(0, colon) : s;
  const auto tail = colon >= 0 ? s.substring(colon) : juce::String();
  if (head.length() >= 2 && head[1] == '-')
    head = head.substring(0, 1) + head.substring(2);

  return NoteToHz((head + tail).toStdString());
}

// Nearest available FFT plan to a block length in seconds.
float fftLenParamForSeconds(double seconds, double sampleRate)
{
  const double want = seconds * (sampleRate > 0.0 ? sampleRate : 44100.0);
  int best = 0;
  double bestErr = std::numeric_limits<double>::max();
  for (int i = 0; i < NUM_FFT_SZ; ++i) {
    const double err = std::abs((double)g_fft_sz[i] - want);
    if (err < bestErr) {
      bestErr = err;
      best = i;
    }
  }
  return BlkFxParam::getFFTLenParam(best);
}

// The effect whose name matches, or -1.
int effectIndexForName(const juce::String& text)
{
  const auto wanted = text.trim();
  for (int i = 0; i < g_num_fx_1_0; ++i)
    if (wanted.equalsIgnoreCase(GetFxRun1_0(i)->name()))
      return i;
  return -1;
}

} // namespace

juce::String DtBlkFxAudioProcessor::paramId(int index)
{
  // MIX_BACK and OVERLAP have no single host parameter -- see the header.
  if (index == BlkFxParam::MIX_BACK || index == BlkFxParam::OVERLAP)
    return {};
  return "param_" + juce::String(index);
}

juce::String DtBlkFxAudioProcessor::coreParamText(int index, float v)
{
  if (core == nullptr)
    return {};

  BlkFxParam::SplitParamNum p(index);
  CharArray<64> buf;
  std::memset(buf.data, 0, sizeof(buf.data));

  // FxState1_0::getParamDisplay prints the name of the effect the set is
  // *currently* on rather than the effect the value asks for -- fine for a
  // VST2 host that only ever asks about the current value, useless in an
  // automation lane. Same name, from the same table, but honouring v.
  if (p.fx_param == BlkFxParam::FX_TYPE)
    return GetFxRun1_0((int)BlkFxParam::getEffectType(v))->name();

  // Both of these take the value as an argument and only read engine state, so
  // asking for text never disturbs the audio. DtBlkFx::getParameterDisplay,
  // which the VST2 build used, does the opposite -- it ignores the value and
  // reads _params.getInput(index) -- so it is deliberately not used here.
  if (core->getParamDisplayGlobal(p, v, buf) ||
      (p.fx_set >= 0 && core->_fx1_0[p.fx_set].getParamDisplay(p, v, buf)))
    return juce::String(buf.data);

  // Param is in morph mode and not attached here; the engine falls back to a
  // percentage, so do the same.
  return juce::String(juce::roundToInt(v * 100.0f)) + "%";
}

long DtBlkFxAudioProcessor::guessBlkLen(float fftLenParam, bool& capped)
{
  using namespace BlkFxParam;

  // This is DtBlkFx::guessFFTLen's arithmetic, driven by the value passed in
  // rather than by the engine's own fft-len param. The engine's version ignores
  // any value handed to it, which makes every point of an automation lane print
  // the same length.
  //
  // The block shoulder is fixed at 0 (DtBlkFx::configParams1_0 calls
  // setModeFixed), so the time-domain length equals the FFT length and the
  // delay is the only thing that can cut it down.
  const long plan = getPlan(fftLenParam);
  long len = g_fft_sz[plan];
  capped = false;

  if (core != nullptr) {
    const long delayN =
        core->getDelaySamps(BlkFxParam::Delay(apvts.getRawParameterValue(paramId(DELAY))->load()));
    if (len > delayN) {
      len = g_fft_sz[DtBlkFx::reducePlan(plan, delayN)];
      if (len > delayN)
        len = delayN;
      capped = true;
    }
  }
  return len;
}

juce::String DtBlkFxAudioProcessor::blkLenText(float v)
{
  bool capped = false;
  const long len = guessBlkLen(v, capped);

  const float sr = (float)(getSampleRate() > 0.0 ? getSampleRate() : 44100.0);
  CharArray<32> buf;
  std::memset(buf.data, 0, sizeof(buf.data));
  // sprnum + "sec" is exactly how the engine prints a block length.
  CharRng(buf.data, (int)sizeof(buf.data)) << sprnum((float)len / sr) << "sec";

  // docs/MANUAL.md: "If the specified Delay is less than the BlkLen specified
  // then a smaller block length will be used and displayed with an asterisk".
  return juce::String(buf.data) + (capped ? " *" : "");
}

// How far the engine steps forward per block, and therefore how much of each
// block overlaps the last. The parameter asks for 0..100% of the available
// range; what comes out tops at about 85%, because that is where
// getBlkShiftFwd's interpolation ends. Both the engine
// (DtBlkFx::getParamDisplayGlobal) and the original GUI (GlobalCtrl.cpp:228)
// display the achieved figure, not the request, so this does too.
juce::String DtBlkFxAudioProcessor::overlapText(float part)
{
  bool capped = false;
  const long len = guessBlkLen(apvts.getRawParameterValue(paramId(BlkFxParam::FFT_LEN))->load(),
                               capped);
  if (len <= 0)
    return "0%";

  const long fwd = BlkFxParam::getBlkShiftFwd(part, (int)len);
  return juce::String(juce::roundToInt((1.0f - (float)fwd / (float)len) * 100.0f)) + "%";
}

// Inverse of the above: what request lands on the overlap the user typed.
float DtBlkFxAudioProcessor::overlapValueForText(const juce::String& text)
{
  bool capped = false;
  const long len = guessBlkLen(apvts.getRawParameterValue(paramId(BlkFxParam::FFT_LEN))->load(),
                               capped);
  if (len <= 0)
    return 0.0f;

  const auto s = text.trim();
  const double pct = s.getDoubleValue();
  const float wanted = (float)((s.contains("%") || pct > 1.0 ? pct * 0.01 : pct));

  // getBlkShiftFwd interpolates the step from (len - 16) down to len * 0.15.
  const float lo = (float)len - 16.0f;
  const float hi = (float)len * 0.15f;
  const float fwd = (1.0f - wanted) * (float)len;
  return juce::jlimit(0.0f, 1.0f, (fwd - lo) / (hi - lo));
}

juce::AudioProcessorValueTreeState::ParameterLayout DtBlkFxAudioProcessor::createParameterLayout()
{
  using namespace BlkFxParam;
  juce::AudioProcessorValueTreeState::ParameterLayout layout;

  const juce::NormalisableRange<float> unit(0.0f, 1.0f);

  auto addFloat = [&layout, &unit](const juce::String& id,
                                   const juce::String& name,
                                   float defaultValue,
                                   std::function<juce::String(float, int)> toText,
                                   std::function<float(const juce::String&)> fromText) {
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        id,
        name,
        unit,
        defaultValue,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction(std::move(toText))
            .withValueFromStringFunction(std::move(fromText))));
  };

  auto addBool = [&layout](const juce::String& id,
                           const juce::String& name,
                           bool defaultValue,
                           const char* trueText,
                           const char* falseText) {
    layout.add(std::make_unique<juce::AudioParameterBool>(
        id,
        name,
        defaultValue,
        juce::AudioParameterBoolAttributes()
            .withStringFromValueFunction(
                [trueText, falseText](bool v, int) { return juce::String(v ? trueText : falseText); })
            .withValueFromStringFunction([trueText](const juce::String& t) {
              const auto s = t.trim();
              return s.equalsIgnoreCase(trueText) || s.equalsIgnoreCase("on") ||
                     s.equalsIgnoreCase("yes") || s.equalsIgnoreCase("true") ||
                     s.getFloatValue() >= 0.5f;
            })));
  };

  // --- globals ---------------------------------------------------------------
  // Names follow docs/MANUAL.md so the host list reads like the manual.

  // MIX_BACK, unpacked. The engine's own text prints the mode and the amount
  // together ("match  33%"); split across two host parameters each half prints
  // its own part, which is what the original GUI's two controls did.
  addFloat(
      mixBackId,
      "MixBack",
      getMixBackFrac(0.0f),
      [](float v, int) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; },
      [](const juce::String& t) { return parseFraction(t); });

  addBool(powerId, "Power", getPwrMatch(0.0f) > 0.5f, "match", "filter");

  addFloat(
      paramId(DELAY),
      "Delay",
      0.0f,
      [this](float v, int) { return coreParamText(DELAY, v); },
      [](const juce::String& t) {
        const auto s = t.trim().toLowerCase();
        if (s.contains("beat"))
          return (float)Delay::beats((float)s.getDoubleValue());
        return (float)Delay::msec((float)(parseSeconds(s) * 1000.0));
      });

  addFloat(
      paramId(FFT_LEN),
      "BlkLen",
      0.5f,
      [this](float v, int) { return blkLenText(v); },
      [this](const juce::String& t) {
        return fftLenParamForSeconds(parseSeconds(t), getSampleRate());
      });

  // OVERLAP, unpacked, same reasoning as MIX_BACK. The percentage the engine
  // prints is the achieved overlap (it depends on the block length), while the
  // parameter is the requested amount, so this one prints the request.
  addFloat(
      overlapId,
      "Overlap",
      getOverlapPart(0.5f),
      [this](float v, int) { return overlapText(v); },
      [this](const juce::String& t) { return overlapValueForText(t); });

  addBool(syncId, "Sync", getBlkSync(0.5f), "on", "off");

  // --- the eight effect sets -------------------------------------------------
  for (int set = 0; set < NUM_FX_SETS; ++set) {
    const juce::String prefix(set + 1);

    auto addFx = [&](int fxParam,
                     const juce::String& suffix,
                     float defaultValue,
                     std::function<float(const juce::String&)> fromText) {
      const int index = paramOffs(set) + fxParam;
      addFloat(
          paramId(index),
          prefix + ": " + suffix,
          defaultValue,
          [this, index](float v, int) { return coreParamText(index, v); },
          std::move(fromText));
    };

    // Frequencies accept Hz ("440", "1.2kHz") or a note name ("c#4:-45").
    // The original never wired note entry up -- NoteToHz has no call sites in
    // dtblkfx_src -- but the notation is the plugin's own, so it is honoured.
    auto parseFreq = [](const juce::String& t) {
      float hz = noteTextToHz(t);
      if (hz < 0.0f)
        hz = (float)parseHz(t);
      if (hz <= 0.0f)
        return 0.0f;
      return juce::jlimit(0.0f, 1.0f, HzToNoteOffs(hz) / noteSpan());
    };

    addFx(FX_FREQ_A, "FreqA", 0.0f, parseFreq);
    addFx(FX_FREQ_B, "FreqB", 0.0f, parseFreq);

    addFx(FX_AMP, "Amp", getAmpParam0dB(), [](const juce::String& t) {
      const auto s = t.trim().toLowerCase();
      if (s.contains("inf"))
        return 0.0f;
      // Effects with a mix-mode amp print a percentage below 0 dB, where the
      // param is linear up to the 0 dB point.
      if (s.contains("%"))
        return juce::jlimit(0.0f, 1.0f, (float)(s.getDoubleValue() * 0.01) * getAmpParam0dB());
      return getAmpParam((float)s.getDoubleValue());
    });

    // Left as a plain float, not an AudioParameterChoice: the effect is
    // selected by (long)(param * 255) / 8, and a choice parameter would
    // renormalise, so old automation and ported presets would pick a
    // different effect.
    addFx(FX_TYPE, "Type", 0.0f, [](const juce::String& t) {
      const int idx = effectIndexForName(t);
      return idx >= 0 ? getEffectTypeInv(idx) : juce::jlimit(0.0f, 1.0f, t.getFloatValue());
    });

    // Every effect gives FX_VAL its own meaning and its own text ("40% odd",
    // "+12.00 notes", "above 25%", ...). Reading those back needs 31 bespoke
    // parsers; until then the raw 0..1 value is accepted.
    // See docs/FUTURE-ROADMAP.md.
    addFx(FX_VAL, "Value", 0.0f, [](const juce::String& t) {
      return juce::jlimit(0.0f, 1.0f, t.trim().getFloatValue());
    });
  }

  // Limiter Parameters
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      limiterCeilingId,
      "Limiter Ceiling",
      juce::NormalisableRange<float>(-24.0f, 0.0f, 0.1f),
      -0.1f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      limiterGainId, "Limiter Gain", juce::NormalisableRange<float>(0.0f, 24.0f, 0.1f), 0.0f));
  layout.add(std::make_unique<juce::AudioParameterFloat>(
      limiterReleaseId,
      "Limiter Release",
      juce::NormalisableRange<float>(0.1f, 1000.0f, 0.1f, 0.5f),
      50.0f));
  // Default OFF. The limiter is a post-port addition, not part of the original
  // plugin, and it has not been reviewed or tuned yet — off is the behaviour
  // that matches DtBlkFx as people know it. See docs/ROADMAP.md, Phase 9.
  layout.add(
      std::make_unique<juce::AudioParameterBool>(limiterEnabledId, "Limiter Enabled", false));

  return layout;
}

void DtBlkFxAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
  if (core == nullptr)
    return;

  if (parameterID.startsWith("param_")) {
    const int index = parameterID.substring(6).getIntValue();
    core->setParameter(index, newValue);
    // BlkLen and Overlap both print something derived from the block length,
    // which DELAY caps and FFT_LEN requests.
    if (index == BlkFxParam::DELAY || index == BlkFxParam::FFT_LEN)
      displayRefresher.triggerAsyncUpdate();
    // FX_TYPE decides what the other four params in its set mean, including
    // whether they print "-" at all.
    else if (index >= BlkFxParam::NUM_GLOBAL_PARAMS &&
             (index - BlkFxParam::NUM_GLOBAL_PARAMS) % BlkFxParam::NUM_FX_PARAMS ==
                 BlkFxParam::FX_TYPE)
      displayRefresher.triggerAsyncUpdate();
    return;
  }

  // The two packed globals: recombine the pair into the single float the
  // engine expects. Only ever one writer per engine value.
  if (parameterID == mixBackId || parameterID == powerId) {
    const float frac =
        parameterID == mixBackId ? newValue : apvts.getRawParameterValue(mixBackId)->load();
    const bool match = parameterID == powerId ? newValue >= 0.5f
                                              : apvts.getRawParameterValue(powerId)->load() >= 0.5f;
    core->setParameter(BlkFxParam::MIX_BACK, BlkFxParam::getMixbackParam(frac, match));
  }
  else if (parameterID == overlapId || parameterID == syncId) {
    const float part =
        parameterID == overlapId ? newValue : apvts.getRawParameterValue(overlapId)->load();
    const bool sync = parameterID == syncId ? newValue >= 0.5f
                                            : apvts.getRawParameterValue(syncId)->load() >= 0.5f;
    core->setParameter(BlkFxParam::OVERLAP, BlkFxParam::getOverlapParam(part, sync));
  }
}

DtBlkFxAudioProcessor::~DtBlkFxAudioProcessor()
{
  displayRefresher.cancelPendingUpdate();
  if (core) {
    delete core;
    core = nullptr;
  }
}

const juce::String DtBlkFxAudioProcessor::getName() const
{
  return JucePlugin_Name;
}

bool DtBlkFxAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
  return true;
#else
  return false;
#endif
}

bool DtBlkFxAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
  return true;
#else
  return false;
#endif
}

bool DtBlkFxAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
  return true;
#else
  return false;
#endif
}

double DtBlkFxAudioProcessor::getTailLengthSeconds() const
{
  return 0.0;
}

int DtBlkFxAudioProcessor::getNumPrograms()
{
  return 1; // NB: some hosts don't cope very well if you tell them there are 0 programs,
            // so this should be at least 1, even if you're not really implementing programs.
}

int DtBlkFxAudioProcessor::getCurrentProgram()
{
  return 0;
}

void DtBlkFxAudioProcessor::setCurrentProgram(int index)
{
  juce::ignoreUnused(index);
}

const juce::String DtBlkFxAudioProcessor::getProgramName(int index)
{
  juce::ignoreUnused(index);
  return {};
}

void DtBlkFxAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
  juce::ignoreUnused(index, newName);
}

void DtBlkFxAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
  if (core) {
    core->setSampleRate(sampleRate);
    core->setBlockSize(samplesPerBlock);
    core->resume();
  }

  juce::dsp::ProcessSpec spec;
  spec.sampleRate = sampleRate;
  spec.maximumBlockSize = (juce::uint32)samplesPerBlock;
  spec.numChannels = (juce::uint32)getTotalNumOutputChannels();

  limiter.prepare(spec);
  limiter.reset();
}

void DtBlkFxAudioProcessor::updateTimeInfo()
{
  // The engine asks the "host" for tempo through the VST2 stub, which hands
  // back one struct that nobody was ever filling in. Everything tempo-related
  // -- delay in beats, block sync, the parameter interpolation window -- reads
  // from here, so it all sat on whatever the default was.
  auto& ti = core->timeInfo;
  ti.sampleRate = getSampleRate();
  ti.flags = 0;

  auto* playHead = getPlayHead();
  if (playHead == nullptr)
    return;

  const auto pos = playHead->getPosition();
  if (!pos.hasValue())
    return;

  if (const auto bpm = pos->getBpm()) {
    ti.tempo = *bpm;
    ti.flags |= kVstTempoValid;
  }
  if (const auto ppq = pos->getPpqPosition()) {
    ti.ppqPos = *ppq;
    ti.flags |= kVstPpqPosValid;
  }
  if (const auto sig = pos->getTimeSignature()) {
    ti.timeSigNumerator = sig->numerator;
    ti.timeSigDenominator = sig->denominator;
    ti.flags |= kVstTimeSigValid;
  }
  if (const auto samples = pos->getTimeInSamples())
    ti.samplePos = (double)*samples;
  if (pos->getIsPlaying())
    ti.flags |= kVstTransportPlaying;
}

void DtBlkFxAudioProcessor::releaseResources()
{
  if (core) {
    core->suspend();
  }
}

bool DtBlkFxAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
  juce::ignoreUnused(layouts);
  return true;
#else
  // This is the place where you check if the layout is supported.
  // In this template code we only support mono or stereo.
  // Some plugin hosts, such as certain GarageBand versions, will only
  // load plugins that support stereo bus layouts.
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;

    // This checks if the input layout matches the output layout
#  if !JucePlugin_IsSynth
  if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
    return false;
#  endif

  return true;
#endif
}

void DtBlkFxAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& midiMessages)
{
  juce::ignoreUnused(midiMessages);

  juce::ScopedNoDenormals noDenormals;
  auto totalNumInputChannels = getTotalNumInputChannels();
  auto totalNumOutputChannels = getTotalNumOutputChannels();

  // In case we have more outputs than inputs, this code clears any output
  // channels that didn't contain input data, (because these aren't
  // guaranteed to be empty - they may contain garbage).
  // This is here to avoid people getting screaming feedback
  // when they first compile a plugin, but obviously you don't need to keep
  // this code if your algorithm always overwrites all the output channels.
  for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());

  if (core) {
    updateTimeInfo();

    // getArrayOfWritePointers() returns float* const* as of JUCE 7 (the
    // pointed-to samples are still mutable, only the array of pointers is
    // const); processReplacing predates that and wants float**.
    auto* writePointers = const_cast<float**>(buffer.getArrayOfWritePointers());
    core->processReplacing(writePointers, writePointers, buffer.getNumSamples());
  }

  // Output Limiter
  bool limiterEnabled = *apvts.getRawParameterValue(limiterEnabledId) > 0.5f;
  if (limiterEnabled) {
    float ceiling = *apvts.getRawParameterValue(limiterCeilingId);
    float gain = *apvts.getRawParameterValue(limiterGainId);
    float release = *apvts.getRawParameterValue(limiterReleaseId);

    // Apply Input Gain
    if (gain > 0.0f) {
      buffer.applyGain(juce::Decibels::decibelsToGain(gain));
    }

    limiter.setThreshold(ceiling);
    limiter.setRelease(release);

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    limiter.process(context);
  }
}
//==============================================================================
bool DtBlkFxAudioProcessor::hasEditor() const
{
  return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* DtBlkFxAudioProcessor::createEditor()
{
  return new DtBlkFxEditor(*this);
}

//==============================================================================
void DtBlkFxAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
  // You should use this method to store your parameters in the memory block.
  // You could do that either as raw data, or use the XML or ValueTree classes
  // as intermediaries
  auto state = apvts.copyState();
  std::unique_ptr<juce::XmlElement> xml(state.createXml());
  copyXmlToBinary(*xml, destData);
}

void DtBlkFxAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
  // You should use this method to restore your parameters from this memory block,
  // whose contents will have been created by the getStateInformation() call.
  std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

  if (xmlState.get() != nullptr)
    if (xmlState->hasTagName(apvts.state.getType()))
      apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

void DtBlkFxAudioProcessor::pushInputSpectrogramData(const float* data, int numBins)
{
  juce::ScopedLock lock(inputSpectrogramLock);
  if (inputSpectrogramData.size() != numBins)
    inputSpectrogramData.resize(numBins);

  std::memcpy(inputSpectrogramData.data(), data, numBins * sizeof(float));
  newInputSpectrogramDataAvailable = true;
}

void DtBlkFxAudioProcessor::pushOutputSpectrogramData(const float* data, int numBins)
{
  juce::ScopedLock lock(outputSpectrogramLock);
  if (outputSpectrogramData.size() != numBins)
    outputSpectrogramData.resize(numBins);

  std::memcpy(outputSpectrogramData.data(), data, numBins * sizeof(float));
  newOutputSpectrogramDataAvailable = true;
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
  return new DtBlkFxAudioProcessor();
}
