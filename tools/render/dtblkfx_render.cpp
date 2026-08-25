/*
 * dtblkfx_render — offline render + regression fingerprint for the DtBlkFx core.
 *
 * See LICENSE.md for copyright and licensing information. This file is part of
 * DtBlkFx and is distributed under the GNU General Public License v3 or later.
 *
 * Why this exists
 * ---------------
 * The DSP engine is twenty-year-old code that nobody alive fully remembers, and
 * the work queued up against it (limiter, true-stereo sidechain, parameter
 * display, GUI) all runs the risk of silently changing what it *sounds* like.
 * This drives the engine directly — no JUCE, no plugin host — pushes a fixed
 * signal through every effect type, and reduces each render to a short
 * fingerprint that can be diffed against a checked-in baseline.
 *
 * It is a change detector, not a correctness oracle. A FAIL means "the audio
 * moved"; whether that is a fix or a regression is still a human call.
 *
 * Each case is rendered in its own process by default. That used to be load-
 * bearing rather than just cheap insurance: DtBlkFx read uninitialised
 * fftwf_malloc memory on an instance's first FFT window, so a whole-sweep run
 * in one process picked up whatever a previous instance left on the heap and
 * gave different numbers every time, occasionally including bursts of tens of
 * thousands of NaN samples. Fixed in Phase 3 (see docs/ROADMAP.md) by clearing
 * those buffers once in the constructor. --in-process now agrees with
 * isolated rendering to the bit (--repeat proves it via the fingerprint hash,
 * below); it is kept because it is the only mode that models what a DAW
 * actually hands an instance — recycled heap, not fresh pages.
 *
 * Usage:
 *   dtblkfx_render --list
 *   dtblkfx_render --write tests/baseline/core.fingerprint
 *   dtblkfx_render --check tests/baseline/core.fingerprint
 *   dtblkfx_render --wav out/ [--case Vocode]
 */

#include "BlkFxParam.h"
#include "DtBlkFx.hpp"
#include "FxRun1_0.h"
#include "rfftw_float.h"

#include <fftw3.h>

#include <mach-o/dyld.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Smear (fx02) randomises phase from a single process-global PRBS
// (FxRun1_0.cpp) shared and never reseeded by design -- see docs/ROADMAP.md,
// Phase 3. It is the one thing in the engine meant to be non-deterministic, so
// rather than exempt it from the reproducibility check, pin it from the test
// side: reset it before every render so repeated in-process renders of the
// same case see the same seed. This does not touch engine behaviour.
extern long g_rand_i;

namespace {

// ── Fixed render conditions ──────────────────────────────────────────────────
// Everything here is a constant on purpose. Changing any of them invalidates
// every baseline, so treat them as part of the file format.
constexpr double kSampleRate = 44100.0;

// Host block size. Baselines are always taken at 512; --block only exists so
// the engine's sensitivity to the host buffer can be probed by hand, which is
// a real variable (the FFT length the engine actually manages to use is capped
// by how much input it has buffered).
constexpr int kBaselineBlockSize = 512;
int gBlockSize = kBaselineBlockSize;
constexpr int kNumSamples = 65536; // power of two: analysed with one FFT
constexpr double kTempo = 120.0;

// Phase 4 metrics scope (see docs/ROADMAP.md): band energies are now per
// segment instead of over the whole render, at roughly 1/3-octave instead of
// 1.25-octave width. 8 segments of 8192 samples lines up exactly with the
// input signal's impulse period, so each segment also localises one
// transient.
constexpr int kNumSegments = 8;
constexpr int kSegmentLen = kNumSamples / kNumSegments;
constexpr int kNumBands = 24;

// ── Deterministic test signal ────────────────────────────────────────────────
// The two channels are deliberately *different*. A collapse to mono shows up in
// the correlation metric, which is one of the things we are watching for.
//
// Left  : log sweep 50 Hz -> 8 kHz, plus periodic impulses (transient response)
// Right : 110 Hz with eight harmonics (feeds the harmonic/vocode effects)
// Both  : a low noise floor from a fixed LCG, so nothing sits at exact zero
struct Lcg {
  uint32_t s = 0x1234567u;
  float next()
  {
    s = s * 1664525u + 1013904223u;
    return (float)((int32_t)(s >> 8) - 0x800000) / (float)0x800000;
  }
};

void makeInput(std::vector<float>& left, std::vector<float>& right)
{
  left.resize(kNumSamples);
  right.resize(kNumSamples);

  Lcg rng;
  const double f0 = 50.0, f1 = 8000.0;
  const double k = std::log(f1 / f0) / (double)kNumSamples;
  double phase = 0.0;

  for (int i = 0; i < kNumSamples; ++i) {
    const double t = (double)i / kSampleRate;

    // log sweep
    const double f = f0 * std::exp(k * (double)i);
    phase += 2.0 * M_PI * f / kSampleRate;
    double l = 0.5 * std::sin(phase);

    // one impulse every 8192 samples
    if ((i % 8192) == 0)
      l += 0.9;

    // harmonic stack
    double r = 0.0;
    for (int h = 1; h <= 8; ++h)
      r += std::sin(2.0 * M_PI * 110.0 * (double)h * t) / (double)h;
    r *= 0.3;

    left[i] = (float)l + 0.0005f * rng.next();
    right[i] = (float)r + 0.0005f * rng.next();
  }
}

// ── Cases ────────────────────────────────────────────────────────────────────
struct Case {
  std::string name;
  int fxType;    // index into g_fft_fx_table, or -1 for "engine only"
  float fxVal;   // FX_VAL for the effect under test
  float fxAmp;   // FX_AMP; 0.6 is 0 dB, see BlkFxParam::getEffectAmp
  float mixBack; // MIX_BACK, as the raw param — see mixbackParam() below
  float delayMs; // DELAY in milliseconds; also caps the FFT length
  float fftLen;  // FFT_LEN
  float overlap; // OVERLAP
};

// MIX_BACK packs a mode flag and an amount into one 0..1 control: the lower
// half is power-match mode, the upper half filter mode, and in power-match the
// amount runs 0..0.5. So 0.5 is *fully dry*, not half. See getMixbackParam.
float mixbackParam(float dryFraction)
{
  return BlkFxParam::getMixbackParam(dryFraction, /*pwr_match*/ true);
}

// DELAY packs units (beats vs msec) into the integer half of the param, which
// makes the raw value non-monotonic — 0.4 is 6.4 beats, 0.6 is 1.2 seconds.
// Always go through Delay::msec rather than writing a raw float.
float delayParam(float ms)
{
  if (ms <= 0.0f)
    return 0.0f;
  BlkFxParam::Delay d = BlkFxParam::Delay::msec(ms);
  return (float)d;
}

// The engine reduces the requested FFT length to fit the delay buffer
// (DtBlkFx::guessFFTLen), so FFT_LEN only bites once DELAY gives it room. The
// engine cases below vary the two together for that reason — at DELAY 0 every
// FFT_LEN above ~0.3 collapses onto the same actual block size.
std::vector<Case> buildCases()
{
  std::vector<Case> cases;

  // Engine-only passes: no effect active, so these isolate the FFT
  // analysis/resynthesis path from the effects themselves. If one of these
  // moves, the overlap-add core changed.
  // One variable moves per case, so a FAIL points at a control rather than a
  // combination.
  cases.push_back({"engine.default", -1, 0.0f, 0.6f, 0.0f, 0.0f, 0.50f, 0.50f});
  cases.push_back({"engine.fft_small", -1, 0.0f, 0.6f, 0.0f, 0.0f, 0.10f, 0.50f});
  cases.push_back({"engine.fft_large", -1, 0.0f, 0.6f, 0.0f, 0.0f, 0.90f, 0.50f});
  cases.push_back({"engine.overlap_low", -1, 0.0f, 0.6f, 0.0f, 0.0f, 0.50f, 0.15f});
  cases.push_back({"engine.overlap_high", -1, 0.0f, 0.6f, 0.0f, 0.0f, 0.50f, 0.40f});
  // OVERLAP's upper half is meant to be the same overlap amount with beat-sync
  // switched on, but today it renders bit-identical to the lower half. Kept as
  // a case on purpose: when sync starts working this will FAIL, which is the
  // signal we want. See docs/ROADMAP.md.
  cases.push_back({"engine.overlap_sync", -1, 0.0f, 0.6f, 0.0f, 0.0f, 0.50f, 0.85f});
  cases.push_back({"engine.delay_100ms", -1, 0.0f, 0.6f, 0.0f, 100.0f, 0.50f, 0.50f});
  cases.push_back({"engine.delay_500ms", -1, 0.0f, 0.6f, 0.0f, 500.0f, 0.50f, 0.50f});

  // Mix-back is an identity when nothing is processing, so it needs an active
  // effect to mean anything.
  cases.push_back({"mix.filter_dry50", 0, 0.25f, 0.20f, mixbackParam(0.5f), 0.0f, 0.35f, 0.50f});

  // One case per effect type. `.lo` cuts, `.hi` boosts — most of these effects
  // behave differently in each direction, and a unity-gain pass would leave
  // several of them indistinguishable from the dry signal.
  for (int t = 0; t < g_num_fx_1_0; ++t) {
    const char* n = GetFxRun1_0(t)->name();
    char buf[128];
    std::snprintf(buf, sizeof buf, "fx%02d.%s.lo", t, n);
    cases.push_back({buf, t, 0.25f, 0.20f, 0.0f, 0.0f, 0.35f, 0.50f});
    std::snprintf(buf, sizeof buf, "fx%02d.%s.hi", t, n);
    cases.push_back({buf, t, 0.75f, 0.85f, 0.0f, 0.0f, 0.35f, 0.50f});
  }

  return cases;
}

// ── Fingerprint ──────────────────────────────────────────────────────────────
struct Fingerprint {
  double peak[2] = {0, 0}; // dBFS
  double rms[2] = {0, 0};  // dBFS
  double correlation = 0;  // L/R Pearson correlation; ~1.0 means collapsed
  int nonFinite = 0;       // NaN or Inf samples, must always be 0
  // Per-segment, per-band energy, dB, left+right summed. Segment-major,
  // band-minor. See docs/ROADMAP.md, Phase 4.
  double segBand[kNumSegments][kNumBands] = {{0}};
  // Lag (samples) and normalized peak of the circular cross-correlation
  // between the dry mono input and the wet mono output: a stand-in for phase
  // that moves when the overlap-add alignment moves. See docs/ROADMAP.md.
  int lagSamples = 0;
  double lagCorr = 0;
  uint64_t hash = 0;    // FNV-1a over the raw L/R sample bytes; see below
  bool hasHash = false; // false for baseline lines predating the hash column
};

// FNV-1a over the raw output samples. This is bit-exact by construction, which
// is the point: the tolerant metrics above are sized to absorb legitimate
// cross-machine differences (slice, FFTW planner noise) and so cannot see a
// same-machine state leak that biases output by less than 0.05 dB. The hash
// can. It is only meaningful comparing renders from the same binary on the
// same machine -- see docs/ROADMAP.md, Phase 2.1.
uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull)
{
  const unsigned char* p = (const unsigned char*)data;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

uint64_t hashSamples(const std::vector<float>& l, const std::vector<float>& r)
{
  uint64_t h = fnv1a(l.data(), l.size() * sizeof(float));
  return fnv1a(r.data(), r.size() * sizeof(float), h);
}

double toDb(double lin)
{
  return lin > 1e-12 ? 20.0 * std::log10(lin) : -240.0;
}

// Band energies over l[offset..offset+len), log-spaced 40 Hz..Nyquist. Used
// once per segment, so a change localises to where in time it happened as
// well as where in frequency.
void computeBandEnergies(const float* l, const float* r, int offset, int len, double* bands)
{
  std::vector<float> in((size_t)len);
  for (int i = 0; i < len; ++i) {
    const double a = std::isfinite(l[offset + i]) ? l[offset + i] : 0.0;
    const double b = std::isfinite(r[offset + i]) ? r[offset + i] : 0.0;
    // Hann window, so band edges do not smear from the segment boundary
    const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)len);
    in[i] = (float)((a + b) * 0.5 * w);
  }

  const int nbins = len / 2 + 1;
  fftwf_complex* out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * (size_t)nbins);
  fftwf_plan plan = fftwf_plan_dft_r2c_1d(len, in.data(), out, FFTW_ESTIMATE);
  fftwf_execute(plan);

  const double lo = 40.0, hi = kSampleRate * 0.5;
  const double step = std::log(hi / lo) / (double)kNumBands;
  for (int b = 0; b < kNumBands; ++b) {
    const double fLo = lo * std::exp(step * b);
    const double fHi = lo * std::exp(step * (b + 1));
    const int kLo = (int)(fLo * len / kSampleRate);
    const int kHi = std::min(nbins - 1, (int)(fHi * len / kSampleRate));
    double e = 0.0;
    for (int k = kLo; k <= kHi; ++k)
      e += (double)out[k][0] * out[k][0] + (double)out[k][1] * out[k][1];
    bands[b] = toDb(std::sqrt(e) / (double)len);
  }

  fftwf_destroy_plan(plan);
  fftwf_free(out);
}

void analyseSegBands(const std::vector<float>& l, const std::vector<float>& r,
                      double segBand[kNumSegments][kNumBands])
{
  for (int s = 0; s < kNumSegments; ++s)
    computeBandEnergies(l.data(), r.data(), s * kSegmentLen, kSegmentLen, segBand[s]);
}

// Circular cross-correlation (via FFT) between the dry mono input and the wet
// mono output. The input signal is a fixed function of nothing (see
// makeInput), so it is cheap to regenerate here rather than plumb it through
// render(). Peak lag is a stand-in for phase/group delay: it moves if the
// overlap-add windowing shifts, independent of level, which the band energies
// cannot see (see docs/ROADMAP.md, Phase 4).
void analyseLag(const std::vector<float>& l, const std::vector<float>& r, int& lagSamples,
                 double& lagCorr)
{
  std::vector<float> dryL, dryR;
  makeInput(dryL, dryR);

  std::vector<float> dry((size_t)kNumSamples), wet((size_t)kNumSamples);
  double sumSqDry = 0.0, sumSqWet = 0.0;
  for (int i = 0; i < kNumSamples; ++i) {
    dry[i] = (dryL[i] + dryR[i]) * 0.5f;
    const float a = std::isfinite(l[i]) ? l[i] : 0.0f;
    const float b = std::isfinite(r[i]) ? r[i] : 0.0f;
    wet[i] = (a + b) * 0.5f;
    sumSqDry += (double)dry[i] * dry[i];
    sumSqWet += (double)wet[i] * wet[i];
  }

  const int nbins = kNumSamples / 2 + 1;
  fftwf_complex* fftDry = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * (size_t)nbins);
  fftwf_complex* fftWet = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * (size_t)nbins);
  fftwf_plan pDry = fftwf_plan_dft_r2c_1d(kNumSamples, dry.data(), fftDry, FFTW_ESTIMATE);
  fftwf_plan pWet = fftwf_plan_dft_r2c_1d(kNumSamples, wet.data(), fftWet, FFTW_ESTIMATE);
  fftwf_execute(pDry);
  fftwf_execute(pWet);

  fftwf_complex* cross = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * (size_t)nbins);
  for (int k = 0; k < nbins; ++k) {
    // wet * conj(dry): peak at lag n means wet is dry delayed by n samples.
    const double re = (double)fftWet[k][0] * fftDry[k][0] + (double)fftWet[k][1] * fftDry[k][1];
    const double im = (double)fftWet[k][1] * fftDry[k][0] - (double)fftWet[k][0] * fftDry[k][1];
    cross[k][0] = (float)re;
    cross[k][1] = (float)im;
  }

  std::vector<float> corr((size_t)kNumSamples);
  fftwf_plan pInv = fftwf_plan_dft_c2r_1d(kNumSamples, cross, corr.data(), FFTW_ESTIMATE);
  fftwf_execute(pInv);

  // FFTW's inverse is unnormalized (scales by N); fold that into the
  // normalizer along with the two signal energies.
  const double norm =
      (double)kNumSamples * std::sqrt(std::max(sumSqDry, 1e-20) * std::max(sumSqWet, 1e-20));

  // Search non-negative lags only -- the engine only ever adds latency, never
  // looks ahead. Half the render covers the largest DELAY case (500 ms) with
  // headroom.
  int bestLag = 0;
  double bestVal = -1.0;
  const int maxLag = kNumSamples / 2;
  for (int lag = 0; lag < maxLag; ++lag) {
    const double v = std::fabs((double)corr[(size_t)lag] / norm);
    if (v > bestVal) {
      bestVal = v;
      bestLag = lag;
    }
  }
  lagSamples = bestLag;
  lagCorr = bestVal;

  fftwf_destroy_plan(pDry);
  fftwf_destroy_plan(pWet);
  fftwf_destroy_plan(pInv);
  fftwf_free(fftDry);
  fftwf_free(fftWet);
  fftwf_free(cross);
}

Fingerprint analyse(const std::vector<float>& l, const std::vector<float>& r)
{
  Fingerprint fp;

  double sum[2] = {0, 0}, sumSq[2] = {0, 0}, sumLR = 0;
  for (int i = 0; i < kNumSamples; ++i) {
    const float ch[2] = {l[i], r[i]};
    for (int c = 0; c < 2; ++c) {
      if (!std::isfinite(ch[c])) {
        ++fp.nonFinite;
        continue;
      }
      fp.peak[c] = std::max(fp.peak[c], (double)std::fabs(ch[c]));
      sum[c] += ch[c];
      sumSq[c] += (double)ch[c] * ch[c];
    }
    if (std::isfinite(ch[0]) && std::isfinite(ch[1]))
      sumLR += (double)ch[0] * ch[1];
  }

  const double n = kNumSamples;
  for (int c = 0; c < 2; ++c)
    fp.rms[c] = toDb(std::sqrt(sumSq[c] / n));

  // Pearson correlation of the two channels.
  const double meanL = sum[0] / n, meanR = sum[1] / n;
  const double covar = sumLR / n - meanL * meanR;
  const double varL = sumSq[0] / n - meanL * meanL;
  const double varR = sumSq[1] / n - meanR * meanR;
  fp.correlation = (varL > 1e-20 && varR > 1e-20) ? covar / std::sqrt(varL * varR) : 0.0;

  for (int c = 0; c < 2; ++c)
    fp.peak[c] = toDb(fp.peak[c]);

  analyseSegBands(l, r, fp.segBand);
  analyseLag(l, r, fp.lagSamples, fp.lagCorr);
  fp.hash = hashSamples(l, r);
  fp.hasHash = true;
  return fp;
}

// ── Render ───────────────────────────────────────────────────────────────────
void render(const Case& c, std::vector<float>& outL, std::vector<float>& outR)
{
  using namespace BlkFxParam;

  g_rand_i = 1;

  std::vector<float> inL, inR;
  makeInput(inL, inR);

  DtBlkFx core(nullptr);
  core.setSampleRate((float)kSampleRate);
  core.setBlockSize(gBlockSize);

  // The VST2 stub leaves `timeInfo` uninitialised and hands it back on every
  // getTimeInfo() call, so the engine's tempo tracking reads whatever was on
  // the stack. Pin it here: without this the renders are not reproducible, and
  // pinning it also gives us a controlled way to revisit the parked
  // "garbage tempo" hypothesis later. See docs/ROADMAP.md.
  core.timeInfo = VstTimeInfo{};
  core.timeInfo.sampleRate = kSampleRate;
  core.timeInfo.tempo = kTempo;
  core.timeInfo.timeSigNumerator = 4;
  core.timeInfo.timeSigDenominator = 4;
  core.timeInfo.flags = kVstTempoValid | kVstPpqPosValid | kVstTransportPlaying;

  // Defaults matching the plugin's parameter layout.
  for (int i = 0; i < TOTAL_NUM; ++i) {
    SplitParamNum p(i);
    float v = 0.0f;
    if (p.fx_param == FX_AMP)
      v = 0.6f; // 0 dB
    core.setParameter(i, v);
  }
  core.setParameter(MIX_BACK, c.mixBack);
  core.setParameter(DELAY, delayParam(c.delayMs));
  core.setParameter(FFT_LEN, c.fftLen);
  core.setParameter(OVERLAP, c.overlap);

  // Every fx set is switched off, then set 0 gets the effect under test.
  const float offType = getEffectTypeInv(9); // g_no_fx
  for (int s = 0; s < NUM_FX_SETS; ++s)
    core.setParameter(paramOffs(s) + FX_TYPE, offType);

  if (c.fxType >= 0) {
    const int base = paramOffs(0);
    core.setParameter(base + FX_TYPE, getEffectTypeInv(c.fxType));
    core.setParameter(base + FX_FREQ_A, 0.15f);
    core.setParameter(base + FX_FREQ_B, 0.55f);
    core.setParameter(base + FX_AMP, c.fxAmp);
    core.setParameter(base + FX_VAL, c.fxVal);
  }

  core.resume();

  outL.assign((size_t)kNumSamples, 0.0f);
  outR.assign((size_t)kNumSamples, 0.0f);

  for (int pos = 0; pos < kNumSamples; pos += gBlockSize) {
    const int n = std::min(gBlockSize, kNumSamples - pos);

    // processReplacing works in place, which is how the plugin calls it.
    std::vector<float> bufL(inL.begin() + pos, inL.begin() + pos + n);
    std::vector<float> bufR(inR.begin() + pos, inR.begin() + pos + n);
    float* io[2] = {bufL.data(), bufR.data()};

    core.timeInfo.samplePos = (double)pos;
    core.timeInfo.ppqPos = (double)pos / kSampleRate * (kTempo / 60.0);

    core.processReplacing(io, io, n);

    std::copy(bufL.begin(), bufL.end(), outL.begin() + pos);
    std::copy(bufR.begin(), bufR.end(), outR.begin() + pos);
  }

  core.suspend();
}

// ── WAV out (32-bit float, for listening) ────────────────────────────────────
void writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r)
{
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return;
  }

  const uint32_t dataBytes = (uint32_t)(l.size() * 2 * sizeof(float));
  auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
  auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };

  f.write("RIFF", 4);
  u32(36 + dataBytes);
  f.write("WAVE", 4);
  f.write("fmt ", 4);
  u32(16);
  u16(3); // IEEE float
  u16(2);
  u32((uint32_t)kSampleRate);
  u32((uint32_t)kSampleRate * 2 * sizeof(float));
  u16(2 * sizeof(float));
  u16(32);
  f.write("data", 4);
  u32(dataBytes);

  for (size_t i = 0; i < l.size(); ++i) {
    const float s[2] = {l[i], r[i]};
    f.write((const char*)s, sizeof s);
  }
}

// ── Fingerprint file I/O ─────────────────────────────────────────────────────
std::string formatLine(const std::string& name, const Fingerprint& fp)
{
  char buf[4096];
  int n = std::snprintf(buf,
                        sizeof buf,
                        "%-28s peak %8.3f %8.3f  rms %8.3f %8.3f  corr %7.4f  nan %d"
                        "  lag %6d %7.4f  segbands",
                        name.c_str(),
                        fp.peak[0],
                        fp.peak[1],
                        fp.rms[0],
                        fp.rms[1],
                        fp.correlation,
                        fp.nonFinite,
                        fp.lagSamples,
                        fp.lagCorr);
  for (int s = 0; s < kNumSegments; ++s)
    for (int b = 0; b < kNumBands; ++b)
      n += std::snprintf(buf + n, sizeof buf - (size_t)n, " %8.3f", fp.segBand[s][b]);
  n += std::snprintf(
      buf + n, sizeof buf - (size_t)n, "  hash %016llx", (unsigned long long)fp.hash);
  return buf;
}

bool parseLine(const std::string& line, std::string& name, Fingerprint& fp)
{
  char nameBuf[128], w1[16], w2[16], w3[16], w4[16], w5[16], w6[16];
  int consumed = 0;
  const int got = std::sscanf(line.c_str(),
                              "%127s %15s %lf %lf %15s %lf %lf %15s %lf %15s %d"
                              " %15s %d %lf %15s%n",
                              nameBuf,
                              w1,
                              &fp.peak[0],
                              &fp.peak[1],
                              w2,
                              &fp.rms[0],
                              &fp.rms[1],
                              w3,
                              &fp.correlation,
                              w4,
                              &fp.nonFinite,
                              w5,
                              &fp.lagSamples,
                              &fp.lagCorr,
                              w6,
                              &consumed);
  if (got < 15)
    return false;

  const char* p = line.c_str() + consumed;
  for (int s = 0; s < kNumSegments; ++s) {
    for (int b = 0; b < kNumBands; ++b) {
      char* end = nullptr;
      fp.segBand[s][b] = std::strtod(p, &end);
      if (end == p)
        return false;
      p = end;
    }
  }

  // Optional trailing "hash <hex>", absent from baselines predating Phase 2.1.
  char hashWord[16];
  char hashHex[32];
  if (std::sscanf(p, "%15s %31s", hashWord, hashHex) == 2 && std::strcmp(hashWord, "hash") == 0) {
    fp.hash = std::strtoull(hashHex, nullptr, 16);
    fp.hasHash = true;
  }

  name = nameBuf;
  return true;
}

// Tolerances. Generous enough to absorb the arm64/x86_64 slice difference and
// FFTW planner noise, tight enough that any audible change trips them.
constexpr double kTolDb = 0.05;
constexpr double kTolCorr = 0.002;

bool compare(const Fingerprint& a, const Fingerprint& b, std::string& why)
{
  auto dbOk = [&](const char* what, double x, double y) {
    if (std::fabs(x - y) <= kTolDb)
      return true;
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s %.3f -> %.3f dB; ", what, x, y);
    why += buf;
    return false;
  };

  bool ok = true;
  ok &= dbOk("peakL", a.peak[0], b.peak[0]);
  ok &= dbOk("peakR", a.peak[1], b.peak[1]);
  ok &= dbOk("rmsL", a.rms[0], b.rms[0]);
  ok &= dbOk("rmsR", a.rms[1], b.rms[1]);
  for (int s = 0; s < kNumSegments; ++s) {
    for (int i = 0; i < kNumBands; ++i) {
      char what[24];
      std::snprintf(what, sizeof what, "seg%d.band%d", s, i);
      ok &= dbOk(what, a.segBand[s][i], b.segBand[s][i]);
    }
  }
  if (std::fabs(a.correlation - b.correlation) > kTolCorr) {
    char buf[128];
    std::snprintf(
        buf, sizeof buf, "corr %.4f -> %.4f; ", a.correlation, b.correlation);
    why += buf;
    ok = false;
  }
  // Lag tolerance of 1 sample absorbs a near-tie argmax shifted by FFTW
  // planner/cross-machine FP noise; a real alignment change moves it by a
  // hop or more. See docs/ROADMAP.md, Phase 4.
  if (std::abs(a.lagSamples - b.lagSamples) > 1 || std::fabs(a.lagCorr - b.lagCorr) > kTolCorr) {
    char buf[128];
    std::snprintf(buf,
                  sizeof buf,
                  "lag %d(%.4f) -> %d(%.4f); ",
                  a.lagSamples,
                  a.lagCorr,
                  b.lagSamples,
                  b.lagCorr);
    why += buf;
    ok = false;
  }
  if (a.nonFinite != b.nonFinite) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "nan %d -> %d; ", a.nonFinite, b.nonFinite);
    why += buf;
    ok = false;
  }
  return ok;
}

// ── Per-case isolation ───────────────────────────────────────────────────────
std::string selfPath()
{
  char buf[4096];
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0)
    return "dtblkfx_render";
  return buf;
}

// Render one case in a fresh child process and read back its fingerprint line.
bool renderIsolated(const Case& c, Fingerprint& fp)
{
  char cmd[8192];
  std::snprintf(cmd,
                sizeof cmd,
                "'%s' --print --case '%s' --block %d",
                selfPath().c_str(),
                c.name.c_str(),
                gBlockSize);

  FILE* pipe = popen(cmd, "r");
  if (!pipe)
    return false;

  char line[4096];
  const bool got = std::fgets(line, sizeof line, pipe) != nullptr;
  const int rc = pclose(pipe);

  if (!got || rc != 0)
    return false;

  std::string name;
  return parseLine(line, name, fp) && name == c.name;
}

// ── Parameter scope dump (Phase 5) ───────────────────────────────────────────
// Walks every parameter and every entry in g_fft_fx_table and prints what the
// engine already knows how to display. Pure documentation: it renders no audio
// and touches no baseline. See docs/ROADMAP.md, Phase 5.
//
// dispVal() takes an FxState1_0* but no override in FxRun1_0.cpp actually names
// it, so FX_VAL text is a pure function of (effect, value) and NULL is safe --
// the engine itself calls it that way when it builds the preset menus.

std::string globalDisp(DtBlkFx& core, int index, float v)
{
  core.setParameter(index, v);
  char buf[64] = {0};
  core.getParameterDisplay(index, buf);
  return buf;
}

std::string fxValDisp(FxRun1_0* fx, float v)
{
  CharArray<128> buf;
  fx->dispVal(nullptr, buf, v);
  return std::string(buf.data);
}

void dumpParams()
{
  using namespace BlkFxParam;

  DtBlkFx core(nullptr);
  core.setSampleRate((float)kSampleRate);
  core.setBlockSize(gBlockSize);
  core.timeInfo = VstTimeInfo{};
  core.timeInfo.sampleRate = kSampleRate;
  core.timeInfo.tempo = kTempo;
  core.timeInfo.timeSigNumerator = 4;
  core.timeInfo.timeSigDenominator = 4;
  core.timeInfo.flags = kVstTempoValid | kVstPpqPosValid | kVstTransportPlaying;

  for (int i = 0; i < TOTAL_NUM; ++i)
    core.setParameter(i, i % NUM_FX_PARAMS == FX_AMP && i >= NUM_GLOBAL_PARAMS ? 0.6f : 0.0f);

  // Context for every table below. DELAY matters: guessFFTLen caps the block by
  // how much input is buffered, so at DELAY 0 the whole FFT_LEN range collapses
  // onto one block size and both the FFT_LEN and OVERLAP displays freeze. One
  // second of delay gives the range room to move. Whenever a sweep column is the
  // parameter under test, the parameter is put back to these values afterwards.
  const float ctxDelay = delayParam(1000.0f);
  const float ctxFftLen = 0.5f;
  core.setParameter(DELAY, ctxDelay);
  core.setParameter(FFT_LEN, ctxFftLen);

  std::printf("<!-- generated by `dtblkfx_render --params`; do not edit by hand -->\n");
  std::printf("# DtBlkFx parameter scope\n\n");
  std::printf("Sample rate %.0f Hz, tempo %.0f BPM, host block %d, `DELAY` = 1 sec "
              "(param %.4f),\n`FFT_LEN` = 0.5 unless that is the parameter being swept.\n"
              "Display strings are exactly what the engine's own `getParameterDisplay` /\n"
              "`dispVal` produce -- nothing here is invented.\n\n",
              kSampleRate,
              kTempo,
              gBlockSize,
              delayParam(1000.0f));

  // ── Global params ────────────────────────────────────────────────────────
  std::printf("## Global parameters\n\n");
  std::printf("%d parameters, indices 0..%d.\n\n", NUM_GLOBAL_PARAMS, NUM_GLOBAL_PARAMS - 1);

  const char* globalIds[] = {"MIX_BACK", "DELAY", "FFT_LEN", "OVERLAP"};
  static const float sweep[] = {
      0.0f, 0.1f, 0.2f, 0.25f, 0.3f, 0.4f, 0.499f, 0.5f, 0.501f, 0.6f, 0.7f, 0.75f, 0.8f, 0.9f, 1.0f};

  for (int g = 0; g < NUM_GLOBAL_PARAMS; ++g) {
    char nameBuf[64] = {0};
    core.getParameterName(g, nameBuf);
    std::printf("### %d. `%s` (short name `%s`)\n\n", g, globalIds[g], nameBuf);
    std::printf("| param | display |\n|---|---|\n");
    for (float v : sweep)
      std::printf("| %.3f | `%s` |\n", v, globalDisp(core, g, v).c_str());
    std::printf("\n");
    core.setParameter(g, g == DELAY ? ctxDelay : g == FFT_LEN ? ctxFftLen : 0.0f);
  }

  // FFT_LEN deserves its own table: the param is quantised to a plan index.
  std::printf("### FFT_LEN plan quantisation\n\n");
  std::printf("`getPlan` maps the param to one of %d FFT sizes; `getFFTLenParam` inverts it.\n"
              "The length actually used is capped by DELAY (`guessFFTLen`), which is why the\n"
              "display above stops moving.\n\n",
              NUM_FFT_SZ);
  std::printf("| plan | param | fft samples | seconds @ %.0f Hz |\n|---|---|---|---|\n", kSampleRate);
  for (int i = 0; i < NUM_FFT_SZ; ++i)
    std::printf("| %d | %.4f | %d | %.4f |\n",
                i,
                getFFTLenParam(i),
                g_fft_sz[i],
                g_fft_sz[i] / kSampleRate);
  std::printf("\n");

  // ── FX set params ────────────────────────────────────────────────────────
  std::printf("## FX set parameters\n\n");
  std::printf("%d sets of %d, indices %d..%d. Short names come from "
              "`DtBlkFx::getParameterName`.\n\n",
              NUM_FX_SETS,
              NUM_FX_PARAMS,
              NUM_GLOBAL_PARAMS,
              TOTAL_NUM - 1);
  {
    const char* ids[] = {"FX_FREQ_A", "FX_FREQ_B", "FX_AMP", "FX_TYPE", "FX_VAL"};
    std::printf("| fx param | id | set 0 index | set 0 short name |\n|---|---|---|---|\n");
    for (int i = 0; i < NUM_FX_PARAMS; ++i) {
      char nameBuf[64] = {0};
      core.getParameterName(paramOffs(0) + i, nameBuf);
      std::printf("| %d | `%s` | %d | `%s` |\n", i, ids[i], paramOffs(0) + i, nameBuf);
    }
    std::printf("\n");
  }

  // FX_FREQ: the display rounds to the FFT bin, so it is engine-state dependent.
  std::printf("### FX_FREQ_A / FX_FREQ_B\n\n");
  std::printf("Exponential: the param *is* a note offset from c0, %.0f notes "
              "(%.2f octaves) full scale.\n"
              "`getParameterDisplay` rounds to the nearest real FFT bin via `guessRoundHz`, so\n"
              "the same param reads differently at a different FFT length -- both columns below.\n\n",
              noteSpan(),
              octaveSpan());
  std::printf("| param | raw Hz | note | displayed (bin-rounded) |\n|---|---|---|---|\n");
  static const float fsweep[] = {0.0f,  0.05f, 0.1f,  0.15f, 0.2f, 0.3f,
                                 0.4f,  0.5f,  0.6f,  0.7f,  0.8f, 0.9f, 1.0f};
  for (float v : fsweep) {
    CharArray<64> note;
    HzToNote(note, getHz(v));
    std::printf("| %.3f | %.2f | `%s` | `%s` |\n",
                v,
                getHz(v),
                getHz(v) <= 0.0f ? "-" : note.data,
                globalDisp(core, paramOffs(0) + FX_FREQ_A, v).c_str());
  }
  core.setParameter(paramOffs(0) + FX_FREQ_A, 0.0f);
  std::printf("\n");

  // FX_AMP.
  std::printf("### FX_AMP\n\n");
  std::printf("Linear %.0f..%+.0f dB, 0 dB at param %.3f. Effects with `ampMixMode` true read\n"
              "the sub-0 dB half as a wet percentage instead.\n\n",
              getEffectAmp(0.0f),
              getEffectAmp(1.0f),
              getAmpParam0dB());
  std::printf("| param | dB | dB-mode display | mix-mode display |\n|---|---|---|---|\n");
  static const float asweep[] = {0.0f, 0.1f, 0.2f, 0.3f, 0.45f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
  for (float v : asweep) {
    // set 0 is a dB-mode effect (Filter), set 1 is switched to a mix-mode one.
    core.setParameter(paramOffs(0) + FX_TYPE, getEffectTypeInv(0));  // Filter
    std::string dbTxt = globalDisp(core, paramOffs(0) + FX_AMP, v);
    core.setParameter(paramOffs(1) + FX_TYPE, getEffectTypeInv(7));  // Shift
    std::string mixTxt = globalDisp(core, paramOffs(1) + FX_AMP, v);
    std::printf("| %.3f | %+.1f | `%s` | `%s` |\n", v, getEffectAmp(v), dbTxt.c_str(), mixTxt.c_str());
  }
  std::printf("\n");

  // ── Effect table ─────────────────────────────────────────────────────────
  std::printf("## Effects\n\n");
  std::printf("%d entries in `g_fft_fx_table`. `FX_TYPE` quantises to "
              "`(long)(param * 255.0) / 8`;\n"
              "the param column is the centre value `getEffectTypeInv` returns.\n\n",
              g_num_fx_1_0);
  std::printf("| idx | FX_TYPE param | name | mask | amp mode | FrqA | FrqB | Amp | Type | Val | "
              "presets |\n|---|---|---|---|---|---|---|---|---|---|---|\n");
  for (int i = 0; i < g_num_fx_1_0; ++i) {
    FxRun1_0* fx = GetFxRun1_0(i);
    int presets = 0;
    while (fx->getValueName(presets) != nullptr)
      presets++;
    std::printf("| %02d | %.4f | %s | %s | %s |", i, getEffectTypeInv(i), fx->name(),
                fx->isMask() ? "yes" : "-", fx->ampMixMode() ? "mix/dB" : "dB");
    for (int q = 0; q < NUM_FX_PARAMS; ++q)
      std::printf(" %s |", fx->paramUsed(q) ? "y" : "-");
    std::printf(" %d |\n", presets);
  }
  std::printf("\n");

  // ── Per-effect FX_VAL ────────────────────────────────────────────────────
  std::printf("## FX_VAL, per effect\n\n");
  std::printf("`dispVal` sweep plus the value-preset menu (`getValueName` / `getValue`).\n"
              "An effect whose FX_VAL is unused shows `-` for every value; the engine prints that\n"
              "from `paramUsed`, not from `dispVal`.\n\n");
  static const float vsweep[] = {0.0f,  0.125f, 0.25f, 0.375f, 0.499f,
                                 0.5f,  0.625f, 0.75f, 0.875f, 1.0f};
  for (int i = 0; i < g_num_fx_1_0; ++i) {
    FxRun1_0* fx = GetFxRun1_0(i);
    std::printf("### %02d %s%s\n\n", i, fx->name(), fx->paramUsed(FX_VAL) ? "" : "  (FX_VAL unused)");
    std::printf("| value | dispVal |\n|---|---|\n");
    for (float v : vsweep)
      std::printf("| %.3f | `%s` |\n", v, fxValDisp(fx, v).c_str());
    if (fx->getValueName(0) != nullptr) {
      // getValue() takes the current value: Shift, HarmShift, HarmRepitch and
      // Resample have relative "Change by +N notes" entries that offset it
      // rather than jump to a fixed point. Probing from two different current
      // values is what tells the two kinds apart.
      std::printf("\nPresets (`from 0.25` / `from 0.75` = `getValue(k, curr)`; "
                  "differing values mean the entry is relative):\n\n"
                  "| # | from 0.25 | from 0.75 | kind | name |\n|---|---|---|---|---|\n");
      for (int k = 0; fx->getValueName(k) != nullptr; ++k) {
        const float a = fx->getValue(k, 0.25f);
        const float b = fx->getValue(k, 0.75f);
        std::printf("| %d | %.6f | %.6f | %s | `%s` |\n",
                    k,
                    a,
                    b,
                    std::fabs(a - b) < 1e-6f ? "absolute" : "relative",
                    fx->getValueName(k));
      }
    }
    std::printf("\n");
  }
}

int usage()
{
  std::fprintf(stderr,
               "usage: dtblkfx_render <mode>\n"
               "  --list                 list case names\n"
               "  --params               dump the parameter/effect scope table as markdown\n"
               "  --write <file>         render everything, write fingerprints\n"
               "  --check <file>         render everything, diff against fingerprints\n"
               "  --wav <dir>            render to 32-bit float WAVs\n"
               "  --print                render one case, print its fingerprint\n"
               "  --case <name>          restrict to one case\n"
               "  --block <n>            host block size (default 512; baselines use 512)\n"
               "  --repeat <n>           render each case n times; hash-compares the repeats\n"
               "                         to catch same-machine instability (see Phase 2.1)\n"
               "  --in-process           skip per-case process isolation; models a DAW's\n"
               "                         recycled heap instead of a fresh process's zeroed one\n");
  return 2;
}

} // namespace

int main(int argc, char** argv)
{
  std::string mode, arg, only;
  int repeat = 1;
  bool inProcess = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--case" && i + 1 < argc) {
      only = argv[++i];
    }
    else if (a == "--repeat" && i + 1 < argc) {
      repeat = std::atoi(argv[++i]);
      if (repeat < 1)
        return usage();
    }
    else if (a == "--block" && i + 1 < argc) {
      gBlockSize = std::atoi(argv[++i]);
      if (gBlockSize < 1)
        return usage();
    }
    else if ((a == "--write" || a == "--check" || a == "--wav") && i + 1 < argc) {
      mode = a;
      arg = argv[++i];
    }
    else if (a == "--list" || a == "--print" || a == "--params") {
      mode = a;
    }
    else if (a == "--in-process") {
      inProcess = true;
    }
    else {
      return usage();
    }
  }
  if (mode.empty())
    return usage();

  const std::vector<Case> cases = buildCases();

  if (mode == "--params") {
    CreateFFTWfPlans();
    dumpParams();
    return 0;
  }

  if (mode == "--list") {
    for (const auto& c : cases)
      std::printf("%s\n", c.name.c_str());
    return 0;
  }

  if (mode == "--print" && only.empty()) {
    std::fprintf(stderr, "--print needs --case <name>\n");
    return 2;
  }

  // The parent process in an isolated run never renders, so it does not need
  // the plan table; building it anyway costs nothing and keeps --print,
  // --wav and --in-process on one path.
  CreateFFTWfPlans();

  // Per-case process isolation, for the reason in the file header. Rendering a
  // single named case is already isolated by definition.
  const bool isolate = !inProcess && only.empty() && (mode == "--write" || mode == "--check");

  // --check: load the baseline first, so a missing file fails before we spend
  // time rendering.
  std::vector<std::pair<std::string, Fingerprint>> baseline;
  if (mode == "--check") {
    std::ifstream f(arg);
    if (!f) {
      std::fprintf(stderr, "cannot read baseline %s\n", arg.c_str());
      return 2;
    }
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#')
        continue;
      std::string name;
      Fingerprint fp;
      if (!parseLine(line, name, fp)) {
        std::fprintf(stderr, "malformed baseline line: %s\n", line.c_str());
        return 2;
      }
      baseline.emplace_back(name, fp);
    }
  }

  std::vector<std::string> out;
  int failures = 0, checked = 0;

  for (const auto& c : cases) {
    if (!only.empty() && c.name != only)
      continue;

    Fingerprint fp;
    std::vector<float> l, r;
    if (isolate) {
      if (!renderIsolated(c, fp)) {
        std::fprintf(stderr, "failed to render %s in a child process\n", c.name.c_str());
        return 2;
      }
    }
    else {
      render(c, l, r);
      fp = analyse(l, r);
    }

    // --repeat exists to catch same-machine, same-binary instability -- so the
    // only correct comparison is bit-exact (the hash), not the tolerant
    // fingerprint: the tolerance is sized for cross-machine baseline checks
    // and can hide a small state-leak bias. See docs/ROADMAP.md, Phase 2.1.
    for (int rep = 1; rep < repeat; ++rep) {
      std::vector<float> l2, r2;
      render(c, l2, r2);
      const Fingerprint fp2 = analyse(l2, r2);
      if (fp.hash != fp2.hash) {
        std::string why;
        compare(fp, fp2, why); // best-effort detail; the hash made the call
        std::printf("UNSTABLE %s run %d: hash %016llx -> %016llx  %s\n",
                    c.name.c_str(),
                    rep + 1,
                    (unsigned long long)fp.hash,
                    (unsigned long long)fp2.hash,
                    why.c_str());
      }
    }

    if (mode == "--print") {
      std::printf("%s\n", formatLine(c.name, fp).c_str());
      continue;
    }

    if (mode == "--wav") {
      writeWav(arg + "/" + c.name + ".wav", l, r);
      std::printf("%s\n", formatLine(c.name, fp).c_str());
      continue;
    }

    if (mode == "--write") {
      out.push_back(formatLine(c.name, fp));
      continue;
    }

    // --check
    const Fingerprint* want = nullptr;
    for (const auto& b : baseline)
      if (b.first == c.name)
        want = &b.second;

    if (!want) {
      std::printf("NEW   %s\n", c.name.c_str());
      ++failures;
      continue;
    }

    ++checked;
    std::string why;
    // A hash difference against the committed baseline is advisory, not a
    // failure: it legitimately moves with the compiled slice and FFTW planner
    // noise. It is only meaningful same-machine (--repeat); see the hash
    // comment above and docs/ROADMAP.md, Phase 2.1.
    const bool hashDiffers = want->hasHash && fp.hasHash && want->hash != fp.hash;
    if (compare(*want, fp, why)) {
      if (hashDiffers)
        std::printf("ok    %s  (hash %016llx -> %016llx)\n",
                    c.name.c_str(),
                    (unsigned long long)want->hash,
                    (unsigned long long)fp.hash);
      else
        std::printf("ok    %s\n", c.name.c_str());
    }
    else {
      std::printf("FAIL  %s: %s\n", c.name.c_str(), why.c_str());
      ++failures;
    }
  }

  if (mode == "--write") {
    std::ofstream f(arg);
    if (!f) {
      std::fprintf(stderr, "cannot write %s\n", arg.c_str());
      return 2;
    }
    f << "# dtblkfx_render fingerprints. Regenerate with tools/regen_baseline.sh\n"
      << "# sr " << (int)kSampleRate << "  block " << gBlockSize << "  samples " << kNumSamples
      << "  tempo " << (int)kTempo << "\n"
      << "# columns: name peak(L R) rms(L R) corr nan lag(samples corr) "
      << "segbands(" << kNumSegments << " segments x " << kNumBands
      << " bands, segment-major), all dB except corr/lagCorr\n";
    for (const auto& line : out)
      f << line << "\n";
    std::printf("wrote %zu fingerprints to %s\n", out.size(), arg.c_str());
    return 0;
  }

  if (mode == "--check") {
    std::printf("\n%d checked, %d failed\n", checked, failures);
    return failures == 0 ? 0 : 1;
  }

  return 0;
}
