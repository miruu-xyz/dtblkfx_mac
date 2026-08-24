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
 * Each case is rendered in its own process. That is not paranoia: state leaks
 * between DtBlkFx instances — a second instance in the same process picks up
 * what the first left behind — so a whole-sweep run gives different numbers
 * every time, occasionally including bursts of tens of thousands of NaN
 * samples. One case per process is reproducible to the bit. Use --in-process
 * to watch the bug instead; see docs/ROADMAP.md, Phase 3.
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
constexpr int kNumBands = 8;

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
  double peak[2] = {0, 0};      // dBFS
  double rms[2] = {0, 0};       // dBFS
  double correlation = 0;       // L/R Pearson correlation; ~1.0 means collapsed
  int nonFinite = 0;            // NaN or Inf samples, must always be 0
  double band[kNumBands] = {0}; // per-band energy, dB, left+right summed
};

double toDb(double lin)
{
  return lin > 1e-12 ? 20.0 * std::log10(lin) : -240.0;
}

void analyseBands(const std::vector<float>& l, const std::vector<float>& r, double* bands)
{
  std::vector<double> mono((size_t)kNumSamples);
  for (int i = 0; i < kNumSamples; ++i) {
    const double a = std::isfinite(l[i]) ? l[i] : 0.0;
    const double b = std::isfinite(r[i]) ? r[i] : 0.0;
    // Hann window, so band edges do not smear from the block boundary
    const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)kNumSamples);
    mono[i] = (a + b) * 0.5 * w;
  }

  std::vector<float> in((size_t)kNumSamples);
  for (int i = 0; i < kNumSamples; ++i)
    in[i] = (float)mono[i];

  const int nbins = kNumSamples / 2 + 1;
  fftwf_complex* out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * (size_t)nbins);
  fftwf_plan plan = fftwf_plan_dft_r2c_1d(kNumSamples, in.data(), out, FFTW_ESTIMATE);
  fftwf_execute(plan);

  // Eight bands, log-spaced from 40 Hz to Nyquist.
  const double lo = 40.0, hi = kSampleRate * 0.5;
  const double step = std::log(hi / lo) / (double)kNumBands;
  for (int b = 0; b < kNumBands; ++b) {
    const double fLo = lo * std::exp(step * b);
    const double fHi = lo * std::exp(step * (b + 1));
    const int kLo = (int)(fLo * kNumSamples / kSampleRate);
    const int kHi = std::min(nbins - 1, (int)(fHi * kNumSamples / kSampleRate));
    double e = 0.0;
    for (int k = kLo; k <= kHi; ++k)
      e += (double)out[k][0] * out[k][0] + (double)out[k][1] * out[k][1];
    bands[b] = toDb(std::sqrt(e) / (double)kNumSamples);
  }

  fftwf_destroy_plan(plan);
  fftwf_free(out);
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

  analyseBands(l, r, fp.band);
  return fp;
}

// ── Render ───────────────────────────────────────────────────────────────────
void render(const Case& c, std::vector<float>& outL, std::vector<float>& outR)
{
  using namespace BlkFxParam;

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
  char buf[512];
  int n = std::snprintf(buf,
                        sizeof buf,
                        "%-28s peak %8.3f %8.3f  rms %8.3f %8.3f  corr %7.4f  nan %d  bands",
                        name.c_str(),
                        fp.peak[0],
                        fp.peak[1],
                        fp.rms[0],
                        fp.rms[1],
                        fp.correlation,
                        fp.nonFinite);
  for (int b = 0; b < kNumBands; ++b)
    n += std::snprintf(buf + n, sizeof buf - (size_t)n, " %8.3f", fp.band[b]);
  return buf;
}

bool parseLine(const std::string& line, std::string& name, Fingerprint& fp)
{
  char nameBuf[128], w1[16], w2[16], w3[16], w4[16], w5[16];
  int consumed = 0;
  const int got = std::sscanf(line.c_str(),
                              "%127s %15s %lf %lf %15s %lf %lf %15s %lf %15s %d %15s%n",
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
                              &consumed);
  if (got < 12)
    return false;

  const char* p = line.c_str() + consumed;
  for (int b = 0; b < kNumBands; ++b) {
    char* end = nullptr;
    fp.band[b] = std::strtod(p, &end);
    if (end == p)
      return false;
    p = end;
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
  for (int i = 0; i < kNumBands; ++i) {
    char what[16];
    std::snprintf(what, sizeof what, "band%d", i);
    ok &= dbOk(what, a.band[i], b.band[i]);
  }
  if (std::fabs(a.correlation - b.correlation) > kTolCorr) {
    char buf[128];
    std::snprintf(
        buf, sizeof buf, "corr %.4f -> %.4f; ", a.correlation, b.correlation);
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

  char line[1024];
  const bool got = std::fgets(line, sizeof line, pipe) != nullptr;
  const int rc = pclose(pipe);

  if (!got || rc != 0)
    return false;

  std::string name;
  return parseLine(line, name, fp) && name == c.name;
}

int usage()
{
  std::fprintf(stderr,
               "usage: dtblkfx_render <mode>\n"
               "  --list                 list case names\n"
               "  --write <file>         render everything, write fingerprints\n"
               "  --check <file>         render everything, diff against fingerprints\n"
               "  --wav <dir>            render to 32-bit float WAVs\n"
               "  --print                render one case, print its fingerprint\n"
               "  --case <name>          restrict to one case\n"
               "  --block <n>            host block size (default 512; baselines use 512)\n"
               "  --repeat <n>           render each case n times; catches instability\n"
               "  --in-process           skip per-case process isolation (shows the\n"
               "                         uninitialised-memory bug; not reproducible)\n");
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
    else if (a == "--list" || a == "--print") {
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

    // --repeat exists because the engine is not currently stable across
    // instances in one process. Any variation printed here is that bug, not
    // anything to do with the host or the test signal.
    for (int rep = 1; rep < repeat; ++rep) {
      std::vector<float> l2, r2;
      render(c, l2, r2);
      const Fingerprint fp2 = analyse(l2, r2);
      std::string why;
      if (!compare(fp, fp2, why))
        std::printf("UNSTABLE %s run %d: %s\n", c.name.c_str(), rep + 1, why.c_str());
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
    if (compare(*want, fp, why)) {
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
      << "# columns: name peak(L R) rms(L R) corr nan bands(0..7), all dB except corr\n";
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
