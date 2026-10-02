/*
 * Phase 5 check: the host-facing parameter text.
 *
 * The audio harness (tools/check_audio.sh) cannot see any of this — it drives
 * the engine directly and never builds a JUCE parameter. This is the runnable
 * counterpart: it instantiates the real AudioProcessor and exercises every
 * parameter's stringFromValue / valueFromString pair.
 *
 * The invariant is text stability, not value stability: displays round to
 * whole percent, to 0.1 dB and to an FFT bin, so typing back what the plugin
 * printed must land on a value that prints the same string again.
 *
 * See LICENSE.md for copyright and licensing information.
 */

#include "DesignAxis.h"
#include "DesignPalette.h"
#include "DesignRow.h"
#include "DtBlkFxEditor.h"
#include "DtBlkFxProcessor.h"
#include "RetroLookAndFeel.h"
#include "SpectrogramComponent.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const juce::String& what)
{
  if (!ok) {
    std::printf("FAIL  %s\n", what.toRawUTF8());
    ++failures;
  }
}

juce::AudioProcessorParameter* get(DtBlkFxAudioProcessor& p, const juce::String& id)
{
  auto* param = p.apvts.getParameter(id);
  check(param != nullptr, "missing parameter " + id);
  return param;
}

// For the two render modes below.
int writePng(const juce::Image& image, const char* path)
{
  const juce::File out(juce::File::getCurrentWorkingDirectory().getChildFile(path));
  out.deleteFile();

  juce::FileOutputStream stream(out);
  juce::PNGImageFormat png;
  if (!stream.openedOk() || !png.writeImageToStream(image, stream)) {
    std::printf("could not write %s\n", out.getFullPathName().toRawUTF8());
    return 1;
  }

  std::printf("wrote %s (%dx%d)\n", out.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
  return 0;
}

// Every row state at once: a live mask pair, crossed handles, an inert mask
// pair, a bypassed row, an Off row, and a mask on the last row with nothing
// below it. Used by `--demo-shot` and by the mask-outline check.
void setUpDemo(DtBlkFxAudioProcessor& processor)
{
  using namespace BlkFxParam;
  auto set = [&](int row, int fxParam, float v) {
    processor.apvts.getParameter(DtBlkFxAudioProcessor::paramId(paramOffs(row) + fxParam))
        ->setValueNotifyingHost(v);
  };
  auto fx = [&](int row, const char* name) {
    auto* p = processor.apvts.getParameter(DtBlkFxAudioProcessor::paramId(paramOffs(row) + FX_TYPE));
    p->setValueNotifyingHost(p->getValueForText(name));
  };
  auto row = [&](int r, const char* name, float a, float b, float amp) {
    fx(r, name);
    set(r, FX_FREQ_A, a);
    set(r, FX_FREQ_B, b);
    set(r, FX_AMP, amp);
  };

  row(0, "Contrast", 0.30f, 0.70f, 0.75f);   // a plain range, +15 dB
  row(1, "HarmMask", 0.45f, 1.00f, 0.60f);   // live mask -> outlines 1-2
  row(2, "Clip", 0.80f, 0.40f, 0.35f);       // crossed: a notch
  row(3, "ThreshMask", 0.00f, 1.00f, 0.60f); // inert mask: Vocode ignores it
  row(4, "Vocode", 0.20f, 0.90f, 0.60f);
  row(5, "Filter", 0.10f, 0.50f, 0.30f);     // bypassed, below
  fx(6, "Off");
  row(7, "AutoHarmMask", 0.25f, 0.60f, 0.60f); // last row: nothing below

  // Every row on except the bypassed one -- only row 2 is on by default.
  for (int r = 0; r < NUM_FX_SETS; ++r)
    processor.apvts.getParameter(DtBlkFxAudioProcessor::fxOnId(r))->setValueNotifyingHost(r == 5 ? 0.0f : 1.0f);
}

// Type text back in and it must print the same thing.
void checkTextRoundTrip(DtBlkFxAudioProcessor& p, const juce::String& id)
{
  auto* param = get(p, id);
  if (param == nullptr)
    return;

  for (int i = 0; i <= 20; ++i) {
    const float v = (float)i / 20.0f;
    const auto text = param->getText(v, 0);
    check(text.isNotEmpty(), id + " printed nothing at " + juce::String(v));
    if (text == "-")
      continue; // param not used by the current effect; nothing to type back
    if (text.endsWith(" *"))
      continue; // BlkLen reduced by the delay: what it prints is the delay,
                // which is not one of the available FFT lengths

    const auto again = param->getText(param->getValueForText(text), 0);
    check(again == text, id + ": \"" + text + "\" -> \"" + again + "\"");
  }
}

void dump(DtBlkFxAudioProcessor& p, const juce::String& id)
{
  auto* param = get(p, id);
  if (param == nullptr)
    return;
  std::printf("  %-10s %-12s", id.toRawUTF8(), param->getName(32).toRawUTF8());
  for (float v : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
    std::printf(" |%10s", param->getText(v, 0).toRawUTF8());
  std::printf("\n");
}

// A host that reports a steady 120 BPM. The VST2 stub used to hand the engine
// an uninitialised VstTimeInfo, so everything tempo-driven -- delay in beats,
// block sync, the parameter interpolation window -- ran on garbage.
struct FixedTempoPlayHead : juce::AudioPlayHead {
  juce::Optional<PositionInfo> getPosition() const override
  {
    PositionInfo info;
    info.setBpm(120.0);
    info.setPpqPosition(0.0);
    info.setTimeInSamples(0);
    info.setIsPlaying(true);
    return info;
  }
};

} // namespace

int main(int argc, char** argv)
{
  // `--shot <file.png>` renders the editor offscreen to a PNG instead of running
  // the checks. The GUI phases need a way to see the window without launching
  // the Standalone, which opens the default audio input *and* output and can
  // feed back through monitors (see CLAUDE.md).
  // `--demo-shot <file.png>` is `--shot` with setUpDemo's rows.
  const bool demo = argc == 3 && juce::String(argv[1]) == "--demo-shot";

  if (argc == 3 && (juce::String(argv[1]) == "--shot" || demo)) {
    juce::ScopedJuceInitialiser_GUI gui;

    DtBlkFxAudioProcessor processor;
    processor.prepareToPlay(44100.0, 512);

    if (demo)
      setUpDemo(processor);

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    if (editor == nullptr) {
      std::printf("no editor\n");
      return 1;
    }

    return writePng(editor->createComponentSnapshot(editor->getLocalBounds(), true), argv[2]);
  }

  // `--sgram-shot <file.png>` renders one spectrogram fed a synthetic signal --
  // pink-ish noise, a rising tone, a broadband click every 40 lines -- with a
  // range inverted and the hover on, since a live one is black without audio.
  if (argc == 3 && juce::String(argv[1]) == "--sgram-shot") {
    juce::ScopedJuceInitialiser_GUI gui;
    SpectrogramComponent sgram("Output");
    sgram.setSize(640, 177);

    constexpr int fftLen = 2048, bins = fftLen / 2 + 1;
    std::vector<float> block(bins);
    for (int line = 0; line < 177; ++line) {
      const int tone = juce::roundToInt(std::pow(2.0, 3.0 + 6.5 * line / 176.0)); // ~8..720
      for (int b = 0; b < bins; ++b) {
        block[(size_t)b] = 2e-9f / (1.0f + 0.01f * (float)b); // below -80 dB: black
        if (std::abs(b - tone) <= 1)
          block[(size_t)b] = 0.02f;
        if (line % 40 == 20)
          block[(size_t)b] = std::max(block[(size_t)b], 1e-3f);
      }
      sgram.pushBlock(block.data(), bins, 44100.0);
      sgram.writeLine();
    }

    sgram.setHighlight(true, 0.35f, 0.6f);
    sgram.setHover(0.5f, true);
    if (std::getenv("SGRAM_PAUSED") != nullptr) // review aid: the paused look
      sgram.setPaused(true);
    return writePng(sgram.createComponentSnapshot(sgram.getLocalBounds(), true), argv[2]);
  }

  // `--menu-shot <file.png>` renders the FX-type menu. A PopupMenu is its own
  // desktop window, so `--shot` never sees one -- but showMenuAsync builds and
  // lays that window out synchronously, so it can be snapshotted before any
  // message loop runs. The window does briefly exist on screen.
  if (argc == 3 && juce::String(argv[1]) == "--menu-shot") {
    juce::ScopedJuceInitialiser_GUI gui;
    RetroLookAndFeel lnf;

    std::vector<float> values;
    auto menu = design::buildFxTypeMenu(4 /* Clip, so the current-item colour shows */, values);
    menu.setLookAndFeel(&lnf);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea({200, 200, 128, 26}));

    auto& desktop = juce::Desktop::getInstance();
    auto* window = desktop.getNumComponents() > 0
                       ? desktop.getComponent(desktop.getNumComponents() - 1)
                       : nullptr;
    if (window == nullptr) {
      std::printf("no menu window\n");
      return 1;
    }

    const int rc = writePng(window->createComponentSnapshot(window->getLocalBounds(), true), argv[2]);
    juce::PopupMenu::dismissAllActiveMenus();
    return rc;
  }

  using namespace BlkFxParam;
  juce::ScopedJuceInitialiser_GUI juceInit;

  DtBlkFxAudioProcessor p;
  p.prepareToPlay(44100.0, 512);

  const auto set0 = [](int fxParam) { return DtBlkFxAudioProcessor::paramId(paramOffs(0) + fxParam); };

  // Set 1 starts bypassed (6.6c), and a bypassed set prints "-" for everything
  // -- which the round trips below skip. Without this they pass by checking
  // nothing.
  get(p, DtBlkFxAudioProcessor::fxOnId(0))->setValueNotifyingHost(1.0f);

  // Give the engine room: FFT_LEN and OVERLAP both display a block length,
  // which the delay caps, so at zero delay they are pinned to the minimum.
  get(p, DtBlkFxAudioProcessor::paramId(DELAY))->setValueNotifyingHost((float)BlkFxParam::Delay::msec(1000.0f));
  // Contrast uses all five of its set's params, so none of them print "-".
  get(p, set0(FX_TYPE))->setValueNotifyingHost(getEffectTypeInv(1));

  std::printf("globals\n");
  for (auto* id : {DtBlkFxAudioProcessor::mixBackId,
                   DtBlkFxAudioProcessor::powerId,
                   DtBlkFxAudioProcessor::overlapId,
                   DtBlkFxAudioProcessor::syncId})
    dump(p, id);
  dump(p, DtBlkFxAudioProcessor::paramId(DELAY));
  dump(p, DtBlkFxAudioProcessor::paramId(FFT_LEN));

  std::printf("fx set 1 (Contrast)\n");
  for (int fxParam = 0; fxParam < NUM_FX_PARAMS; ++fxParam)
    dump(p, set0(fxParam));

  // --- text round trips ------------------------------------------------------
  for (auto* id : {DtBlkFxAudioProcessor::mixBackId,
                   DtBlkFxAudioProcessor::powerId,
                   DtBlkFxAudioProcessor::overlapId,
                   DtBlkFxAudioProcessor::syncId})
    checkTextRoundTrip(p, id);
  checkTextRoundTrip(p, DtBlkFxAudioProcessor::paramId(FFT_LEN));
  checkTextRoundTrip(p, set0(FX_FREQ_A));
  checkTextRoundTrip(p, set0(FX_FREQ_B));
  checkTextRoundTrip(p, set0(FX_AMP));
  checkTextRoundTrip(p, set0(FX_TYPE));

  // Delay is deliberately not round-tripped: what it prints is the delay the
  // engine actually applies (getDelaySamps minus the reported latency), not the
  // amount the parameter asks for, so the two do not have to agree. Check that
  // typed units are honoured instead.
  {
    auto* delay = get(p, DtBlkFxAudioProcessor::paramId(DELAY));
    BlkFxParam::Delay beats(delay->getValueForText("2.00 beats"));
    check(beats.getUnits() == BlkFxParam::Delay::BEATS, "\"2.00 beats\" did not select beats");
    check(std::abs(beats.getAmount() - 2.0f) < 0.05f,
          "\"2.00 beats\" -> " + juce::String(beats.getAmount()) + " beats");

    BlkFxParam::Delay ms(delay->getValueForText("500msec"));
    check(ms.getUnits() == BlkFxParam::Delay::MSEC, "\"500msec\" did not select msec");
    check(std::abs(ms.getAmount() - 500.0f) < 1.0f,
          "\"500msec\" -> " + juce::String(ms.getAmount()) + " msec");

    BlkFxParam::Delay sec(delay->getValueForText("1.20sec"));
    check(sec.getUnits() == BlkFxParam::Delay::MSEC, "\"1.20sec\" did not select msec");
    check(std::abs(sec.getAmount() - 1200.0f) < 1.0f,
          "\"1.20sec\" -> " + juce::String(sec.getAmount()) + " msec");
  }

  // Delay in beats has to follow the host tempo. At 120 BPM a two-beat delay
  // is one second, and that is what the parameter must read back.
  {
    FixedTempoPlayHead playHead;
    p.setPlayHead(&playHead);

    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    for (int i = 0; i < 8; ++i) {
      block.clear();
      p.processBlock(block, midi);
    }

    // The beats readout divides by the same samples-per-beat it multiplied by,
    // so it round-trips at any tempo and proves nothing on its own. What broke
    // was the figure underneath it: check that instead.
    check(std::abs(p.core->getSampsPerBeat() - 22050.0f) < 1.0f,
          "120 BPM at 44.1kHz should be 22050 samples per beat, got " +
              juce::String(p.core->getSampsPerBeat()));

    // With that at zero -- which is what an uninitialised VstTimeInfo produced
    // -- a beats delay collapsed to getDelaySamps' 100-sample floor, so no
    // delay was applied and BlkLen was permanently capped.
    const long samps = p.core->getDelaySamps(BlkFxParam::Delay::beats(2.0f));
    check(std::abs(samps - 44100L) < 2L,
          "two beats at 120 BPM should be 44100 samples, got " + juce::String((int)samps));

    p.setPlayHead(nullptr);
  }

  // Note-name entry on the frequency params. The original never wired this up,
  // so there is no upstream behaviour to match — only NoteFreq's own notation.
  {
    auto* freq = get(p, set0(FX_FREQ_A));
    const auto a4 = getHz(freq->getValueForText("a4"));
    check(std::abs(a4 - 440.0f) < 1.0f, "\"a4\" -> " + juce::String(a4) + " Hz");

    const auto c4 = getHz(freq->getValueForText("c-4:+00"));
    check(std::abs(c4 - 261.63f) < 1.0f, "\"c-4:+00\" -> " + juce::String(c4) + " Hz");

    // ...including cents, which is the half NoteToHz never got right.
    const auto sharp = getHz(freq->getValueForText("a4:+50"));
    check(sharp > a4 * 1.02f && sharp < a4 * 1.035f,
          "\"a4:+50\" -> " + juce::String(sharp) + " Hz (expected ~453)");

    const auto khz = getHz(freq->getValueForText("1.20kHz"));
    check(std::abs(khz - 1200.0f) < 20.0f, "\"1.20kHz\" -> " + juce::String(khz) + " Hz");
  }

  // Every effect must be selectable by the name the plugin prints for it.
  {
    auto* type = get(p, set0(FX_TYPE));
    for (int i = 0; i < g_num_fx_1_0; ++i) {
      const juce::String name(GetFxRun1_0(i)->name());
      const long got = getEffectType(type->getValueForText(name));
      // "Off" occupies two slots; either is the right answer for that name.
      check(juce::String(GetFxRun1_0((int)got)->name()) == name,
            "effect \"" + name + "\" -> slot " + juce::String((int)got));
    }
  }

  // The two split globals only work if packing is lossless: the engine has to
  // see the same float it would have seen from one packed parameter.
  for (int i = 0; i <= 100; ++i) {
    const float f = (float)i / 100.0f;
    for (bool flag : {false, true}) {
      const float packed = getMixbackParam(f, flag);
      check(std::abs(getMixBackFrac(packed) - f) < 1.0e-3f &&
                (getPwrMatch(packed) > 0.5f) == flag,
            "mixback pack/unpack lost " + juce::String(f) + "/" + juce::String((int)flag));

      // getOverlapParam clamps to 0.499 / 0.501 so the two halves of the packed
      // param cannot collide, which costs a little under 0.2% of the request.
      // This is the original's own arithmetic -- dtblkfx_src/GlobalCtrl.cpp:466
      // packs its overlap slider and sync toggle exactly this way -- and the
      // check below shows it does not move the displayed overlap at all.
      const float ov = getOverlapParam(f, flag);
      check(std::abs(getOverlapPart(ov) - f) < 3.0e-3f && getBlkSync(ov) == flag,
            "overlap pack/unpack lost " + juce::String(f) + "/" + juce::String((int)flag));
    }
  }

  // The split has to reach the same extremes the packed parameter did: what the
  // user sees at 0 and 1 must survive a trip through getOverlapParam.
  {
    auto* overlap = get(p, DtBlkFxAudioProcessor::overlapId);
    for (float f : {0.0f, 1.0f})
      for (bool flag : {false, true}) {
        const auto direct = overlap->getText(f, 0);
        const auto packed = overlap->getText(getOverlapPart(getOverlapParam(f, flag)), 0);
        check(direct == packed,
              "overlap " + juce::String(f) + " reads \"" + direct + "\" but \"" + packed +
                  "\" after packing");
      }
    check(overlap->getText(0.0f, 0) == "0%", "overlap 0 reads " + overlap->getText(0.0f, 0));
    check(overlap->getText(1.0f, 0) == "85%", "overlap 1 reads " + overlap->getText(1.0f, 0));
  }

  // --- Phase 6.3a: per-row bypass -------------------------------------------
  // The engine has no bypass of its own, so fxOn_<n> masks FX_TYPE on the way
  // to it. What has to hold: a bypassed row's engine runs "Off"; the row's own
  // FX_TYPE parameter is left alone, so the parked effect comes back
  // untouched; and -- since the Phase 6 review -- every text the host or the
  // GUI reads for that row, its effect included, is the parked effect's, not
  // "Off"'s dashes.
  {
    auto* type = get(p, set0(FX_TYPE));
    auto* amp = get(p, set0(FX_AMP));
    auto* on = get(p, DtBlkFxAudioProcessor::fxOnId(0));

    if (type != nullptr && amp != nullptr && on != nullptr) {
      auto engineEffect = [&] { return juce::String(p.core->_fx1_0[0].getFxRun()->name()); };

      type->setValueNotifyingHost(getEffectTypeInv(1)); // Contrast
      const float parked = type->getValue();
      const auto running = amp->getText(0.5f, 0);
      check(engineEffect() == "Contrast", "row 1 is running " + engineEffect() + ", not Contrast");

      on->setValueNotifyingHost(0.0f);
      check(engineEffect() == "Off", "a bypassed row's engine runs " + engineEffect() + ", not Off");
      check(type->getValue() == parked, "bypass moved the row's FX_TYPE parameter");
      check(amp->getText(0.5f, 0) == running,
            "bypassed amp reads \"" + amp->getText(0.5f, 0) + "\", not its effect's \"" + running + "\"");
      check(type->getText(type->getValue(), 0) == "Contrast",
            "a bypassed row's effect lane reads \"" + type->getText(type->getValue(), 0) + "\"");

      on->setValueNotifyingHost(1.0f);
      check(engineEffect() == "Contrast", "un-bypassing left the engine on " + engineEffect());
      check(amp->getText(0.5f, 0) == running, "un-bypassing changed the amp text");
    }

    checkTextRoundTrip(p, DtBlkFxAudioProcessor::fxOnId(0));
  }

  // --- Phase 6.6a: defaults ---------------------------------------------------
  // A fresh row is fully open and a fresh instance has one beat of delay.
  // Option-click and the host's own reset both go to these.
  {
    if (auto* freqB = get(p, set0(FX_FREQ_B)))
      check(freqB->getDefaultValue() == 1.0f,
            "FreqB defaults to " + juce::String(freqB->getDefaultValue()) + ", not 1 (25.8 kHz)");

    // Only row 2 starts on.
    for (int set = 0; set < NUM_FX_SETS; ++set)
      if (auto* on = get(p, DtBlkFxAudioProcessor::fxOnId(set)))
        check((on->getDefaultValue() >= 0.5f) == (set == 1),
              "row " + juce::String(set + 1) + " defaults to " +
                  (on->getDefaultValue() >= 0.5f ? "on" : "bypassed"));

    if (auto* delay = get(p, DtBlkFxAudioProcessor::paramId(DELAY))) {
      const BlkFxParam::Delay d(delay->getDefaultValue());
      check(d.getUnits() == BlkFxParam::Delay::BEATS && std::abs(d.getAmount() - 1.0f) < 0.01f,
            "Delay does not default to 1 beat");
    }
  }

  // --- Phase 6.6b: when a mask outline is live -------------------------------
  // A mask only shapes the row below when both are running and the effect below
  // goes through MaskedRun. Anything else gets the faint outline, so a mask
  // that is silently doing nothing shows it.
  {
    juce::ScopedJuceInitialiser_GUI gui;
    DtBlkFxAudioProcessor demo;
    demo.prepareToPlay(44100.0, 512);
    setUpDemo(demo);

    std::unique_ptr<juce::AudioProcessorEditor> editor(demo.createEditor());
    if (auto* e = dynamic_cast<DtBlkFxEditor*>(editor.get())) {
      const std::vector<int> expected{0, 2, 0, 1, 0, 0, 0, 1};
      const auto got = e->maskOutlines();
      juce::String text;
      for (int v : got)
        text << v << " ";
      check(got == expected, "mask outlines read " + text.trim() + ", not 0 2 0 1 0 0 0 1");

      // Bypassing the row below a live mask makes it inert.
      demo.apvts.getParameter(DtBlkFxAudioProcessor::fxOnId(2))->setValueNotifyingHost(0.0f);
      check(e->maskOutlines()[1] == 1, "a mask over a bypassed row still reads as live");
    }
    else {
      check(false, "createEditor() did not return a DtBlkFxEditor");
    }
  }

  // --- Phase 6.5: the shared coordinate model --------------------------------
  // The rows' handles and the spectrogram columns must agree by construction,
  // so the model is checked here rather than by eye.
  {
    namespace ax = design::axis;
    std::printf("\ncoordinate model\n");

    const auto s = ax::span({0.0f, 0.0f, 640.0f, 40.0f});
    check(s == juce::Range<float>(5.0f, 635.0f), "axis span is not inset 5px each side");

    for (float v : {0.0f, 0.1f, 0.5f, 0.97f, 1.0f})
      check(std::abs(ax::xToParam(ax::paramToX(v, s), s) - v) < 1.0e-5f,
            "param -> x -> param lost " + juce::String(v));

    // Known points on the frequency axis: 0 is 0 Hz, the middle is 649.6 Hz,
    // and the right edge is the Figma's 25.8 kHz.
    check(ax::paramToHz(0.0f) == 0.0f, "param 0 is not 0 Hz");
    check(std::abs(ax::paramToHz(0.5f) - 649.6f) < 0.5f,
          "param 0.5 is " + juce::String(ax::paramToHz(0.5f)) + " Hz, not 649.6");
    check(std::abs(ax::paramToHz(1.0f) - 25826.0f) < 20.0f,
          "param 1 is " + juce::String(ax::paramToHz(1.0f)) + " Hz, not 25.8k");
    for (float v : {0.01f, 0.3f, 0.5f, 0.9f, 1.0f})
      check(std::abs(ax::hzToParam(ax::paramToHz(v)) - v) < 1.0e-4f,
            "param -> Hz -> param lost " + juce::String(v));

    // The wedge's 0 dB split has to be where the host says 0 dB is, not where
    // the Figma drew it. Set 1 is on Contrast, which reads amp in dB.
    if (auto* amp = get(p, set0(FX_AMP)))
      check(amp->getText(ax::ampUnityParam(), 0) == "0.0 dB",
            "the amp split reads \"" + amp->getText(ax::ampUnityParam(), 0) + "\", not 0.0 dB");

    // Spectrogram columns across the 630px strip, for the smallest and largest
    // FFTs, and a sample rate whose Nyquist is past the axis altogether.
    const int columns = (int)s.getLength();
    for (auto [fftLen, rate] : {std::pair{256, 44100.0}, std::pair{1024, 44100.0},
                                std::pair{80640, 48000.0}, std::pair{1024, 96000.0}}) {
      const auto edges = ax::binEdges(columns, fftLen, rate);
      const int end = fftLen / 2 + 1;
      const auto what = juce::String(fftLen) + " @ " + juce::String((int)rate);

      // The last edge is one past Nyquist, or the 25.8 kHz bin if that comes
      // first: nothing above the axis is folded into its last column.
      const int top = std::min(end, juce::roundToInt(ax::paramToHz(1.0f) * fftLen / rate));
      check((int)edges.size() == columns + 1 && edges.front() == 0 && edges.back() == top,
            what + ": edges do not run 0.." + juce::String(top));
      check(std::is_sorted(edges.begin(), edges.end()), what + ": edges go backwards");

      // Empty columns are exactly the ones past Nyquist, and form one run at
      // the right.
      const float nyquistParam = ax::hzToParam((float)rate * 0.5f);
      const int expectedEmpty = nyquistParam >= 1.0f ? 0 : (int)std::ceil((1.0f - nyquistParam) * columns);
      int empty = 0;
      bool tail = true;
      for (int c = columns - 1; c >= 0; --c) {
        const auto bins = ax::columnBins(edges, c);
        if (bins.isEmpty()) {
          ++empty;
          check(tail, what + ": empty column " + juce::String(c) + " below a non-empty one");
        }
        else {
          tail = false;
          check(bins.getStart() >= 0 && bins.getEnd() <= end,
                what + ": column " + juce::String(c) + " reads outside the FFT");
        }
      }
      check(std::abs(empty - expectedEmpty) <= 1,
            what + ": " + juce::String(empty) + " empty columns, expected about " +
                juce::String(expectedEmpty));

      // The Nyquist bin is shown somewhere when it is on the axis, and not at
      // all when it is past it.
      bool nyquistShown = false;
      for (int c = 0; c < columns && !nyquistShown; ++c)
        nyquistShown = ax::columnBins(edges, c).contains(end - 1);
      check(nyquistShown == (top == end),
            what + (nyquistShown ? ": the Nyquist bin is shown past the axis"
                                 : ": the Nyquist bin is in no column"));

      std::printf("  fft %5d @ %6.0f Hz  -> %d columns, %d empty above Nyquist, %d showing only bins 0-1\n",
                  fftLen, rate, columns, empty,
                  (int)std::count_if(edges.begin(), edges.end() - 1, [&](int e) { return e <= 1; }));
    }
  }

  // --- Phase 6.7: the original's spectrogram colours -----------------------
  // Black at or below -80 dB (and for NaN), red at or above -14 dB, and the
  // original's stops in between -- halfway is between cyan and green.
  {
    using design::spectrogramColour;
    check(spectrogramColour(0.0f) == juce::Colours::black, "silence is not black");
    check(spectrogramColour(std::nanf("")) == juce::Colours::black, "NaN is not black");
    check(spectrogramColour(1e-8f) == juce::Colours::black, "-80 dB is not black");
    check(spectrogramColour(0.04f) == juce::Colour(255, 0, 0), "-14 dB is not red");
    check(spectrogramColour(1.0f) == juce::Colour(255, 0, 0), "0 dB is not red");

    const auto mid = spectrogramColour(std::sqrt(1e-8f * 0.04f)); // halfway in log
    check(mid.getRed() == 0 && mid.getGreen() > 200 && mid.getBlue() > 100 && mid.getBlue() < 160,
          "halfway reads " + mid.toDisplayString(false) + ", not between cyan and green");
  }

  // Phase 6.1: the embedded fonts. createSystemTypefaceFor returns null on a
  // file it cannot parse, and juce::Font then falls back to a system face
  // without complaining -- so a broken BinaryData wiring looks like a slightly
  // wrong-looking GUI rather than an error. Catch it here instead.
  {
    juce::ScopedJuceInitialiser_GUI gui;
    design::FontStore fonts;

    std::printf("\nembedded fonts\n");
    const std::pair<const char*, juce::Typeface::Ptr> loaded[]{
        {"Monaspace Xenon", fonts.xenon},
        {"Monaspace Xenon Wide", fonts.xenonWide},
        {"Monaspace Xenon Wide Italic", fonts.xenonWideItalic},
        {"Player Sans Mono", fonts.playerSans}};

    for (const auto& [expected, face] : loaded) {
      check(face != nullptr, juce::String(expected) + " failed to load from BinaryData");
      if (face == nullptr)
        continue;

      std::printf("  %-24s -> %s\n", expected, face->getName().toRawUTF8());
      check(face->getName().containsIgnoreCase(juce::String(expected).upToFirstOccurrenceOf(
                " ", false, false)),
            juce::String("expected ") + expected + ", loaded \"" + face->getName() + "\"");
      check(juce::Font(face).withPointHeight(16.0f).getStringWidth("MMMM") > 0,
            juce::String(expected) + " measures to zero width");
    }

    // Both Monaspace cuts carry the family name "Monaspace Xenon", so the name
    // alone cannot tell them apart -- if the Wide Light file were missing or
    // mis-wired, the headings would silently render in the regular cut. The
    // width is what actually distinguishes them.
    if (fonts.xenon != nullptr && fonts.xenonWide != nullptr) {
      const auto regular = juce::Font(fonts.xenon).withPointHeight(28.0f).getStringWidth("BlkLen");
      const auto wide = juce::Font(fonts.xenonWide).withPointHeight(28.0f).getStringWidth("BlkLen");
      std::printf("  %-24s -> regular %dpx, wide %dpx\n", "\"BlkLen\" at 28pt", regular, wide);
      check(wide > regular, "the wide cut is not wider than the regular one");
    }
  }

  std::printf(failures == 0 ? "\nparam text: all checks passed\n"
                            : "\nparam text: %d check(s) failed\n",
              failures);
  return failures == 0 ? 0 : 1;
}
