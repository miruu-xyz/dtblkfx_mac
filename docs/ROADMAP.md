# DtBlkFx macOS port — roadmap

Where the port is, what is planned, and in what order. Phases are meant to be
taken one at a time; each one should leave the plugin loadable and
`./tools/check_audio.sh` green (or leave a deliberately regenerated baseline
explaining what moved).

---

## Done

### Phase 0/1 — get it building

A universal (arm64 + x86_64) VST3, ad-hoc signed, installed to
`~/Library/Audio/Plug-Ins/VST3/DtBlkFx Dev.vst3`, plus a Standalone app.

Deliberate constraints, all recorded in `BUILDING.md`: FFTW is vendored and
built from source for both slices rather than taken from Homebrew; the SDK is
pinned to 12.1 because JUCE 6.0.7 cannot compile against macOS 15+; there is no
AU build because JUCE 6 needs full Xcode for `Rez`; and the product name and
plugin code differ from the `DtBlkFx_GUI` beta so both can be installed and
A/B'd side by side.

The output limiter defaults to **off**. It is a post-port addition, not part of
the original plugin. See Phase 9.

### Phase 2 — guardrails

`CLAUDE.md`, and an offline audio regression harness at `tools/render/`.

`./tools/check_audio.sh` renders a fixed stereo signal through the engine and
all 31 effect types, reduces each render to a fingerprint, and diffs against
`tests/baseline/core.fingerprint`. About eight seconds. Details and the rules
for using it are in `CLAUDE.md`.

Building it turned up three things worth knowing, all now documented under
"Landmines" in `CLAUDE.md`: the engine's output depends on the host buffer size;
several parameters pack a mode flag into a single 0..1 value and are not
monotonic; and — the big one — the engine does not render reproducibly. That
last one became Phase 3.

---

## Planned

### Phase 2.1 — exact comparison for the stability check

**Small, and it belongs before Phase 3.** The fingerprint compares with a
tolerance — 0.05 dB on levels and bands, 0.002 on correlation — sized to absorb
the arm64/x86_64 slice difference and FFTW planner noise. That is the right
tolerance for checking a render against a baseline committed from another
machine. It is the wrong one for `--repeat`, which renders the same case twice
from the same binary on the same machine: there the only correct answer is
*bit-identical*, and anything the tolerance swallows is exactly the residual
non-determinism Phase 3 exists to remove.

As it stands, Phase 3's completion test — "the full sweep is reproducible with
`--in-process`" — is defined by a comparison that cannot see a difference
smaller than 0.05 dB in an octave-wide band. The loud failure, a burst of NaNs,
is unmissable either way. The one that would let the phase be declared done too
early is a small state leak that biases the output slightly and stays under the
threshold.

**The change.** Add a hash of the raw output samples to the fingerprint (FNV or
CRC over the float data, both channels) and compare it exactly on the
same-machine paths: `--repeat`, and an `--in-process` run against an
`--in-process` reference. `--check` against the committed baseline should print
a hash difference but not fail on it — a different slice legitimately produces
different bits. A hash mismatch with every tolerant metric passing is precisely
the signal Phase 3 wants and cannot currently get.

One column in the baseline file, one comparison path, no new analysis.

One case will not comply: `fx02.Smear` is genuinely non-deterministic by design
and must stay that way. Seed it from the harness rather than exempting it — see
Phase 3.

Note what a hash does *not* solve. It is the right tool when the output is
expected to come back identical, and useless the moment the bits are
legitimately allowed to move — recompiling the core against a different SDK can
change the last bits without changing anything anyone can hear. That case is
Phase 4, and it needs the other half of the work. See there.

### Phase 3 — engine stability

**Root cause found (2026-08-25).** Not a state leak between instances, and not
the effect singletons in `g_fft_fx_table`. `DtBlkFx` allocates `_chan[].x0`,
`x1` and `x2` with `ScopeFFTWfMalloc` — `fftwf_malloc`, which does not zero
(`src/core/DtBlkFx.cpp`, the `resize` block in the constructor).
`DtBlkFx::init()` is captioned *"clear out all buffers"* but clears only `x3`,
the output FIFO, and `resume()` never calls it. So the first FFT window of an
instance's life transforms whatever the allocator happened to hand over.

The evidence:

- Pre-fill the heap with `0xFF` — a NaN bit pattern read as `float` — before
  constructing, and the engine emits **1024 NaN samples**: one block on both
  channels, samples 100–611, the first 14 ms of that instance's output.
- Rendering the sweep four times in a single process, **46 of 71** cases differ
  run to run, with 6,621–59,589 NaN samples per sweep landing in a different
  set of effects each time.
- A fresh process is reproducible only because the kernel hands out new pages
  already zeroed. **Per-case isolation is therefore a clean room, not a fix.** A
  DAW is the opposite of a fresh process: Ableton has been allocating and
  freeing since the session opened, so every instance loaded into it gets
  recycled memory. This is a live defect in the plugin, not an artefact of the
  harness.

**Why it is worth fixing: it is the one failure mode that kills a channel.** A
NaN entering the output does not stay a blip — it feeds back through the
overlap-add buffers, Live mutes the track, and it propagates downstream. That
is the whole justification for this phase. Everything else the engine does
oddly is the engine being itself.

**The change.** Clear those three buffers once, in the constructor, immediately
after the `resize` calls. That is the entire fix. Do not extend it further:

- **Not in `init()` / `suspend()`.** After the constructor clear, `x0` holds old
  audio rather than uninitialised bytes, so the read window walking over the
  not-yet-rewritten tail on transport restart produces a stale-audio blip, never
  a NaN. No dead-channel risk, so no reason to change the behaviour.
- **No NaN guard on the output block.** It would cost on the audio thread every
  block, and it would mask faults instead of surfacing them. With the root cause
  gone there is nothing for it to catch.

**Cost, measured, not assumed.** 4.54 MB to clear (`x0` is 1.65 MB per channel,
`x1` and `x2` about 315 KB each): **42 µs**, against 85 µs to construct an
instance as it stands today. One time, at plugin load, on the message thread.
Zero on the audio thread. Fresh pages are being zeroed by the kernel on first
touch anyway — the memset only really costs anything in the recycled case,
which is precisely the case being fixed.

**It has already been verified to change nothing audible.** With the clear
applied as a throwaway patch: `./tools/check_audio.sh` reproduces all **71**
committed fingerprints; three consecutive `--in-process` sweeps become
bit-identical; and 70 of 71 cases match the isolated baseline. The sound does
not move. The fix only makes every process behave like a freshly launched one —
which is the behaviour the baseline was captured from and the beta has been
A/B'd against all along.

**What must survive this phase.** The 71st case, `fx02.Smear.hi`, still varies
in-process after the fix, and that is correct. `g_rand_i` (`FxRun1_0.cpp`) is a
single process-global PRBS that randomises phase in Smear, shared by every
instance and never reseeded, so two Smear instances in a session never repeat
and never correlate. That is Darrell's own design and part of what the plugin
sounds like. **Leave it alone**, and leave a comment saying why, so it does not
get "fixed" later.

For the exact in-process stability check in Phase 2.1, seed it from the *test
side* instead: `g_rand_i` is a non-static global, so the harness can declare
`extern long g_rand_i;` and set it to 1 before each case. Confirmed to make
Smear bit-reproducible across renders in one process without touching engine
behaviour.

**Already investigated, no work needed.** Silent input does *not* produce NaN,
despite the effects power-matching by dividing by input power. `MatchPwr`
(`src/core/fftw_support.h`) returns 0 when the target power is `<= 0`, only
divides when the current power is `> 0`, and clamps the result at both ends.
Checked empirically across all 31 effects × three amp settings × digital
silence, denormal `1e-38` and −140 dBFS: clean everywhere. Recorded here so it
does not get re-investigated.

**Done when.** Three `--in-process` sweeps agree exactly (Smear excepted, or
with the harness-side seed in place) and report zero NaN, `check_audio.sh` still
passes 71/71, and the plugin has been loaded in Live for a listen. At that point
per-process isolation in the harness becomes an optimisation rather than a
requirement — but the in-process stability check should stay, and should stay
wired into the guardrail, because it is the only test that models a host.

**Left open.** The NaN window was confirmed at instance startup only; nothing
proves some other path does not read unwritten memory later. The
`suspend()`/`resume()` stale-audio blip above is reasoned, not measured. Both
are worth a look while in this code, neither blocks the fix.

### Phase 4 — JUCE upgrade

Moving off JUCE 6.0.7 unlocks, in one step: the AU build (no more `Rez`, so no
full-Xcode requirement), building against a current macOS SDK, and modern
parameter and bus APIs that Phases 5 and 7 both want. Pulled forward from the
back of the roadmap because those two phases build on top of it — better to
land the upgrade once than to build against the old APIs and redo the work.

**The harness cannot see most of this phase, and can be fooled by the rest.**
Two separate problems, and the second one is work that has to happen first.

*Not covered at all.* `dtblkfx_render` drives the core directly, with no JUCE in
the process. Nothing in `src/DtBlkFxProcessor.*` is exercised, so the JUCE-side
work — new parameter and bus APIs — gets no regression check from
`check_audio.sh` whatsoever. A green harness says the core still behaves; it
says nothing about the wrapper. That half is verified by loading the plugin in a
host.

*Covered, but weakly.* Rebuilding `src/core/` against a current SDK and a newer
toolchain can change floating-point codegen, so the audio is allowed to differ
in its last bits while sounding identical. The Phase 2.1 hash is advisory by
design for exactly this reason, which leaves the tolerant fingerprint as the
only evidence — and the tolerant fingerprint is thin. `analyseBands` reduces the
whole 65536-sample render to eight log-spaced magnitude bands from a single FFT:
phase discarded, no time axis. That is a good detector for what it was built to
catch — a refactor that shifts levels, an effect that stops processing, a
channel that collapses — and a poor one for window misalignment, an off-by-one
in the hop or the bin reconstruction, pre-echo, or transient smear. Those
redistribute energy in time, or across bins *within* a band, and can leave all
eight band levels inside the 0.05 dB tolerance. The test signal already carries
an impulse every 8192 samples for transient response, and the analysis throws
that information away.

**So scope the metrics first, as the opening task of the phase, and regenerate
the baseline before the upgrade** — so the new toolchain is measured against a
reference the current one produced:

- **Time resolution.** Band energies per segment rather than for the whole
  render. The signal is a sweep, so a per-segment breakdown doubles as a coarse
  frequency check, and it localises a transient error to where it happened.
- **Phase.** Something that moves when the overlap-add alignment moves —
  cross-correlation lag against the dry input, or group delay in a few bands.
  Per-bin phase is more baseline than it is worth.
- **More bands.** Eight bands from 40 Hz to Nyquist is about an octave and a
  quarter each, wide enough to hide a birdie or a shifted resonance.

The same requirement applies to any later change of the same shape — vectorising
the core, changing the float flags, another compiler or SDK move. Bits move,
perception must not, and today's fingerprint cannot tell those two apart.

### Phase 5 — parameter semantics and value display

**What is missing.** The original plugin displayed parameter values with real
units and real meaning. Delay read `x.xx beats`. Frequencies read in Hz rounded
to the actual FFT bin, and could be typed as note names. The FX value control
changed meaning per effect — a percentage and `both` / `odd` / `even` /
`between` for the harmonic effects, a mix percentage where the effect
crossfades, dB elsewhere — and showed `-` when the effect does not use that
control at all. The JUCE port currently exposes all 44 parameters as bare
`AudioParameterFloat` over 0..1 with no formatting, which is why it reads as
flat and loses the nuance the original had.

**The good news: none of this has to be rewritten.** All of it is already
compiled into the plugin, just not connected to anything:

| What | Where |
| --- | --- |
| Full formatted display for any parameter | `DtBlkFx::getParameterDisplay` |
| Short parameter names (`Mixbk`, `0.FrqA`, …) | `DtBlkFx::getParameterName` |
| Global params — beats/seconds, block size, sync, mix mode | `DtBlkFx::getParamDisplayGlobal` |
| Per-effect FX value text | `FxRun1_0::dispVal`, ~20 overrides |
| `both` / `odd` / `even` / `between` | `HarmDispVal`, `FxRun1_0.cpp` |
| Which of the 5 FX params this effect actually uses | `FxRun1_0::paramUsed` |
| Is amp a mix percentage or dB | `FxRun1_0::ampMixMode` |
| Value preset menu (name + value pairs per effect) | `FxRun1_0::getValueName` / `getValue` |
| Note name ⇄ Hz, both directions | `NoteFreq::HzToNote`, `NoteToHz` |
| Frequency snapped to the real FFT bin | `DtBlkFx::guessRoundHz` |

**Scope it before writing any code.** Per the plan: produce a document first
that enumerates, for all 31 effect types, what each of the five FX parameters
means, what it displays, whether it is used, and what its value presets are.
Most of that can be generated mechanically by walking `g_fft_fx_table` and
calling the functions above — a small addition to `tools/render/` could emit the
whole table. Only once that exists is it clear what the GUI has to show.

**Then wire it up.** The JUCE-side work is `stringFromValue` / `valueFromString`
on the parameters, delegating to the core. Two things to watch: the core's
display functions read live engine state (current effect type, current FFT
length), so the text is context-dependent and cannot be a pure function of the
parameter value; and they are called from the message thread while the engine
runs on the audio thread.

This phase and Phase 6 are two halves of the same complaint and should probably
be scoped together, but implemented in this order — the GUI cannot show what the
parameter layer does not expose.

### Phase 6 — GUI

**What is wanted.** The current interface is not liked: it looks wrong, and it
is further from the original than it needs to be. Move it back toward the
original's look and layout, and bring back the features that were lost —
notably the **interactive spectrogram**, which in the original was not a display
but a control surface for setting effect frequency ranges directly.

**Reference material is in the repo.** `dtblkfx_src/` holds the complete
original VSTGUI implementation, unmodified: `Gui.cpp/.h` (main panel),
`FxCtrl.cpp/.h` (the per-effect lane), `GlobalCtrl.cpp/.h` (the global
controls), `Spectrogram.cpp/.h` and `PixelFreqBin.cpp/.h` (the spectrogram and
its frequency-to-pixel mapping), `PngVstGui.cpp/.h` and `VstGuiSupport.cpp/.h`
(the VSTGUI glue). None of it can be compiled — VSTGUI 3.5 is Carbon-only and
dead on modern macOS — but it is a precise specification of the intended
behaviour and layout, including the mouse interactions on the spectrogram.

The current JUCE editor is `src/DtBlkFxEditor.cpp/.h` with
`RetroLookAndFeel.h` and `SpectrogramComponent.h`.

Depends on Phase 5 for anything involving parameter text.

### Phase 7 — true stereo sidechain

**Not a bug fix.** Several effects (`Vocode`, `CrossMix`, `WarpMix`,
`HarmMatchLR`, `HarmMatchRL`) collapse the two channels — the harness measures
L/R correlation of exactly 1.0 for them. That is by design: they use one channel
as a carrier or modulator for the other, which is why the original is a stereo
plugin at all.

**The plan is to grow the idea rather than repair it.** Modern hosts can route a
separate sidechain bus into a plugin. Give DtBlkFx a proper sidechain input so
the carrier/modulator source can be a *different track*, in true stereo, instead
of borrowing one of the plugin's own channels and forcing an L+R mono fallback.
That turns a limitation into the feature these effects always wanted to be.

Work involved: a second input bus in `BusesProperties` and
`isBusesLayoutSupported`; a way for the engine to read carrier and signal from
different buffers, which today it cannot — `AUDIO_CHANNELS` is 2 and the effects
index channels 0 and 1 directly; and a decision about what the affected effects
do when no sidechain is connected (presumably today's behaviour, so existing
projects are unchanged).

Note while in this area: `isBusesLayoutSupported` currently accepts a mono
layout, but the engine is compiled `AUDIO_CHANNELS = 2` and always dereferences
channel index 1. That is an out-of-bounds read, and it should be closed as part
of this phase.

### Phase 8 — factory presets

The original ships 43 presets in `resources/stereo_presets.txt`, one per line as
`name:v1 v2 v3 …`, loaded by `VstProgram<N>`. The port currently declares
`g_blk_fx_presets` empty in `src/core/GlobalData.cpp` and reports a single
program. Port them into the JUCE program interface, or into a preset browser.

### Phase 9 — limiter review

The output limiter is a post-port addition, not part of the original DtBlkFx. It
was suspected of causing intermittent silence; that has not recurred since the
port was rebuilt from source, and Phase 3 is the better explanation. It stays
defaulted **off** regardless: it is unreviewed and untuned, and off is what
matches the plugin people know.

When it is looked at: decide whether it belongs in the plugin at all, and if it
does, give it makeup-gain behaviour that is not surprising and a fingerprint
case in the harness.
