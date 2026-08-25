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

### Phase 3 — engine stability

**The problem.** Two `DtBlkFx` instances in one process do not produce the same
audio. Rendering the full sweep twice from the same binary with the same input
gives different numbers each time, and occasionally bursts of tens of thousands
of NaN samples appear in effects that were clean on the previous run. Rendering
a single case in a fresh process is reproducible to the bit, which is why the
harness spawns one process per case.

Reproduce it with:

```bash
./build/tools/dtblkfx_render --write /tmp/a.txt --in-process
./build/tools/dtblkfx_render --write /tmp/b.txt --in-process
diff /tmp/a.txt /tmp/b.txt
```

**Why it matters.** This is the best current explanation for the intermittent
misbehaviour people have reported in hosts — silence, garbage, effects that work
until they don't. A host holding several instances, or reopening one, is exactly
the situation the harness reproduces. It is a better suspect than the limiter
ever was.

**Where to look.** Uninitialised buffers in the engine's per-instance state
(`_chan[].x1` / `x2` and friends), and shared mutable globals in `FxRun1_0.cpp`
— the effect objects in `g_fft_fx_table` are file-scope singletons shared by
every instance, and several of them carry state.

**How to verify.** `--repeat <n>` renders one case repeatedly in a single
process and reports any variation. The phase is done when the full sweep is
reproducible with `--in-process`, at which point the per-process isolation
becomes an optimisation rather than a requirement.

### Phase 4 — JUCE upgrade

Moving off JUCE 6.0.7 unlocks, in one step: the AU build (no more `Rez`, so no
full-Xcode requirement), building against a current macOS SDK, and modern
parameter and bus APIs that Phases 5 and 7 both want. Pulled forward from the
back of the roadmap because those two phases build on top of it — better to
land the upgrade once than to build against the old APIs and redo the work.

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
