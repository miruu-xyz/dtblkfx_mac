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

#include "DtBlkFxProcessor.h"
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

} // namespace

int main()
{
  using namespace BlkFxParam;
  juce::ScopedJuceInitialiser_GUI juceInit;

  DtBlkFxAudioProcessor p;
  p.prepareToPlay(44100.0, 512);

  const auto set0 = [](int fxParam) { return DtBlkFxAudioProcessor::paramId(paramOffs(0) + fxParam); };

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

  std::printf(failures == 0 ? "\nparam text: all checks passed\n"
                            : "\nparam text: %d check(s) failed\n",
              failures);
  return failures == 0 ? 0 : 1;
}
