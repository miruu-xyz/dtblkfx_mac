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

### Phase 2.1 — exact comparison for the stability check

Added a fourth fingerprint column: an FNV-1a hash of the raw output sample
bytes, both channels. `--check` against the committed baseline prints it as
advisory only (`ok ... (hash a -> b)`), never a failure — a different slice
legitimately produces different bits. `--repeat` now uses the hash, not the
tolerant metrics, to decide `UNSTABLE`: that path renders the same case twice
from the same binary on the same machine, where the only correct answer is
bit-identical, and the tolerant comparison is sized to absorb exactly the kind
of small bias a state leak would introduce. `parseLine` treats the hash column
as optional so it doesn't choke on old baseline files.

`fx02.Smear` is meant to be non-deterministic (see Phase 3) and would fail
every `--repeat` by design, so the harness now does `extern long g_rand_i;
g_rand_i = 1;` at the top of every `render()` call — reseeding the shared PRBS
from the test side before each render, without touching engine behaviour.
Confirmed this makes Smear bit-reproducible across in-process repeats too.

### Phase 3 — engine stability

Root cause was uninitialised `fftwf_malloc` memory: `_chan[].x0`, `x1`, `x2`
were never cleared, so the first FFT window of an instance's life transformed
whatever the allocator handed back — recycled heap, not zeroed pages, in any
process that has been running for a while (i.e. every real DAW session). Fixed
with a one-time `Clear()` of those three buffers in the `DtBlkFx` constructor,
right after the `resize()` calls (`src/core/DtBlkFx.cpp`). Not added to
`init()`/`suspend()` and no NaN guard on the output block, per the plan — see
the git history for the reasoning.

Verified with the Phase 2.1 hash: before the fix, `--in-process --repeat 4`
produced 168 `UNSTABLE` lines and NaN bursts (6,621–13,242 samples) scattered
across effects, run to run. After the fix, five in-process repeats of the full
sweep report zero `UNSTABLE` and zero NaN, `check_audio.sh` still passes
71/71, and the baseline needed no content changes — only the new hash column,
since process-per-case isolation already handed every case zeroed pages, which
is exactly why the bug was invisible there. `g_rand_i` was confirmed to still
behave as designed — untouched by this change, seeded only from the harness
(Phase 2.1) for the reproducibility check. Built and installed as `DtBlkFx
Dev.vst3` and tested in Live — sounds unchanged. That was the last item on
the "done when" list.

### Phase 4 — JUCE upgrade

Moved JUCE 6.0.7 → 7.0.12. Chosen over JUCE 8 to keep the jump small (still
C++17); the trade-off, discovered mid-phase, is that `CGWindowListCreateImage`
— the API forcing the SDK pin — isn't rewritten to use ScreenCaptureKit until
JUCE 8.0.2 (confirmed against upstream's git history), so the SDK pin stays,
just for a different reason than "JUCE 6.0.7 specifically". AU was left off
`FORMATS` on purpose: JUCE 7.0.4+ dropped the `Rez` step, so Command Line Tools
alone should now be enough to build one, but turning it on is deferred to a
later phase rather than bundled here.

**Opening task, done first and on the old toolchain, per the plan.** The
tolerant fingerprint before this phase reduced a whole 65536-sample render to
eight log-spaced bands from one FFT — no time axis, phase discarded, wide
enough (1.25 octaves/band) to hide window misalignment or transient smear.
Replaced in `tools/render/dtblkfx_render.cpp` with:

- **Time resolution.** Bands are now per segment, not over the whole render.
  8 segments of 8192 samples each, matching the test signal's impulse period
  exactly, so a change also localises to which transient it hit.
- **More bands.** 8 → 24 (about a third of an octave each, down from 1.25).
- **Phase.** A new metric: circular cross-correlation (FFT-based, `analyseLag`)
  between the dry mono input and the wet mono output, reported as peak lag and
  its normalized correlation. Moves if the overlap-add alignment shifts,
  independent of level — which the band energies can't see. Searches
  non-negative lags up to `kNumSamples/2` samples, enough headroom for the
  500 ms `DELAY` case.

Baseline regenerated on JUCE 6 *before* touching the submodule, so the new
metrics are measured against a same-toolchain reference. `check_audio.sh`
still runs in ~7s.

**The upgrade itself** needed two source changes, both compile-time API
removals rather than behaviour changes: `AudioBuffer::getArrayOfWritePointers`
now returns `float* const*` (`DtBlkFxProcessor.cpp`, fixed with a
`const_cast` — the samples were never actually const, only the pointer array);
and `FileChooser::browseForFileToSave`/`browseForFileToOpen` are gone in
favour of `launchAsync`, which needs the chooser kept alive past the
call (`DtBlkFxEditor.h/.cpp` — now owns a `std::unique_ptr<juce::FileChooser>`
member instead of a stack-local one).

**Result:** `check_audio.sh` 71/71, and the render hash is bit-identical to
the pre-upgrade baseline for every case — not just within tolerance, the exact
same bytes — because `src/core/` isn't linked against JUCE at all and nothing
in its compile flags changed. `--in-process --repeat 3` still reports zero
`UNSTABLE`. Built and installed as `DtBlkFx Dev.vst3`, universal, ad-hoc
signed, and tested in Live — sounds unchanged. That was the last item on the
"done when" list.

---

## Planned

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

**Scoped first, per the plan.** `dtblkfx_render --params` walks every parameter
and all 31 entries of `g_fft_fx_table`, calling the engine's own display
functions, and emits `docs/PARAMETERS.md`. Every string in that file is what the
engine actually prints; nothing in it is hand-written. Regenerate it rather than
editing it.

What scoping changed about the picture above:

- **Note-name entry never shipped.** `NoteToHz` has zero call sites in
  `dtblkfx_src/` — it is dead code in the original, not a feature that was lost.
  Note *display* on the frequency readout was commented out too
  (`// HzToNote(str, hz);`, `dtblkfx_src/FxCtrl.cpp`); notes survive only in the
  spectrogram hover text. Wiring it up is a small addition, not a restoration.
- **Two display paths disagree.** `DtBlkFx::getParameterDisplay` (host-facing)
  bin-rounds frequency via `guessRoundHz` and prints amp mix as `33%`. The
  original GUI (`FxCtrl.cpp` / `GlobalCtrl.cpp`) used raw Hz, `33.3 %`, and
  appended ` *` to BlkLen when the FFT is longer than the delay — an asterisk the
  manual documents.
- **Context dependence is wider than "effect type and FFT length".**
  `FX_VAL`/`FX_AMP`/`FrqA`/`FrqB` follow `FX_TYPE`; `FrqA`/`FrqB` also follow the
  FFT length, which follows `FFT_LEN` *and* `DELAY`; `FFT_LEN` and `OVERLAP`
  follow `DELAY`; `DELAY` follows host tempo and sample rate. At `DELAY = 0` the
  whole `FFT_LEN` range collapses onto one block size and both it and `OVERLAP`
  freeze, so `docs/PARAMETERS.md` is generated at `DELAY = 1 sec`.
- **The display functions do not have to be called through
  `getParameterDisplay`.** That one ignores any value you would pass it and reads
  `_params.getInput(index)`, so using it from JUCE means mutating the engine to
  ask a question. The two functions underneath —
  `DtBlkFx::getParamDisplayGlobal(p, v, str)` and
  `FxState1_0::getParamDisplay(p, v, str)` — both take the value as an argument
  and are public. Calling those directly keeps `stringFromValue` read-only, which
  also settles the audio-thread concern.
- No `dispVal` override names its `FxState1_0*`, so FX_VAL text is a pure
  function of (effect, value) and `NULL` is safe — the engine itself calls it
  that way when it builds the preset menus.
- Slots 9 and 10 of `g_fft_fx_table` are both `Off`.
- Shift, HarmShift, HarmRepitch and Resample have *relative* value presets
  ("Change by +N notes" offsets the current value). That is menu behaviour, so it
  lands in Phase 6, not here.

**Decisions taken at the end of scoping:**

1. **Display convention: the core's, plus the asterisk.** Bin-rounded Hz and
   `33%` from `getParamDisplayGlobal` / `FxState1_0::getParamDisplay`, but BlkLen
   also gets the ` *` suffix when the delay is capping the block size, because
   the manual documents it as a user-facing signal. Phase 6's GUI may still print
   its own raw-Hz text — that split is what the original had.
2. **`MIX_BACK` and `OVERLAP` are replaced, not kept.** The host sees
   `Mix Back %`, `Power`, `Overlap %` and `Sync` as four separate automatable
   parameters; the packed `param_0` and `param_3` stop existing at the host
   boundary. The engine still gets one float each, recombined via
   `BlkFxParam::getMixbackParam` and `getOverlapParam` — both verified to
   round-trip exactly against `getMixBackFrac` / `getPwrMatch` and
   `getOverlapPart` / `getBlkSync`. One writer per engine value, so no automation
   fight. The cost is that a Live set which automated Mix Back or Overlap on
   `DtBlkFx Dev` loses those lanes; the beta is a separate plugin code and is
   untouched, and Phase 8 preset porting is unaffected since those store raw
   values and get decoded into the pair on load.
3. **`FX_TYPE` stays an `AudioParameterFloat`** with custom text rather than
   becoming an `AudioParameterChoice`. A choice parameter renormalises, which
   would make old automation and ported presets select different effects; the
   `(long)(param * 255.0) / 8` mapping is what they encode.
4. **`valueFromString` covers the globals, `FrqA`/`FrqB`, `FX_AMP` and
   `FX_TYPE`.** Every inverse needed already exists — `getMixbackParam`,
   `Delay::beats`/`::msec`, `getFFTLenParam`, `getOverlapParam`, `getAmpParam`,
   `getEffectTypeInv`, and `HzToNoteOffs` for frequency — plus `NoteToHz` for
   note-name entry (`c#4:-45`), which is worth wiring even though the original
   never did. `FX_VAL` accepts a bare 0..1 number only; the 31 bespoke parsers
   needed to invert `dispVal` are deferred to `docs/FUTURE-ROADMAP.md`.

**Done.** `createParameterLayout` in `src/DtBlkFxProcessor.cpp` now builds every
parameter with a `stringFromValue` / `valueFromString` pair, and the four
unpacked globals replace the two packed ones. Parameter ids: `param_1`,
`param_2` and `param_4`..`param_43` are unchanged so existing automation
survives; `param_0` and `param_3` are gone, replaced by `mixBack` + `power` and
`overlap` + `sync`. Host names follow `docs/MANUAL.md` (MixBack, Power, Delay,
BlkLen, Overlap, Sync, and `<n>: FreqA` … one-based). `updateHostDisplay()` is
coalesced through an `AsyncUpdater` and fired when `DELAY` or any `FX_TYPE`
moves.

**Two engine display functions ignore the value they are handed.** Scoping
established that `getParamDisplayGlobal(p, v, str)` and
`FxState1_0::getParamDisplay(p, v, str)` take the value as an argument; what it
missed is that two of their branches then do not use it:

- `FFT_LEN` calls `guessFFTLen()`, which reads the engine's own fft-len param.
- `FX_TYPE` calls `getFxRun()`, which returns the effect the set is *currently*
  on.

For a VST2 host that only ever asks "what does the current value read as", both
are correct. In a VST3 automation lane, where the host asks for text at
arbitrary points, both print one constant across the whole range. Both are
therefore rendered in the JUCE layer instead — `blkLenText()` redoes
`guessFFTLen`'s arithmetic driven by the value, and `FX_TYPE` looks the name up
directly via `GetFxRun1_0(getEffectType(v))`. Same tables, same formatting, same
strings; the core is untouched. Everything else still goes through the engine.

**One core change:** `NoteToHz` in `src/core/NoteFreq.cpp` passed `substr(colon_pos)`
to `strtod`, leading with the `':'`, so it always parsed zero cents. It has no
callers in the original source, which is why nobody noticed; decision 4 gives it
one. Fixed to `substr(colon_pos + 1)`.

**Verification.** `check_audio.sh` cannot see any of this — `dtblkfx_render`
drives the core with no JUCE in the process — so it staying green proves only
that the parameter work did not disturb the engine, which is exactly what it is
for here. The counterpart is `tests/param_text_test.cpp`, built as
`dtblkfx_paramtext`: it instantiates the real `AudioProcessor` and checks that
every parameter's text survives a round trip back through `valueFromString`,
that typed delay units and note names land where they should, that every effect
is selectable by the name the plugin prints for it, and that the mixback and
overlap packing is lossless.

```bash
cmake --build build --target dtblkfx_paramtext && ./build/dtblkfx_paramtext
```

Two things it deliberately does not round-trip. **Delay** prints the delay the
engine will actually apply (`getDelaySamps` minus the reported latency), not the
amount the parameter asks for, so text in and text out need not agree; the test
checks that typed units and amounts are honoured instead. **BlkLen with the
asterisk** prints the delay rather than a block length, and the delay is not one
of the 34 available FFT sizes.

**Overlap reads 0%..85%, not 0%..100%,** because that is where
`getBlkShiftFwd`'s interpolation ends — the block step never falls below 15% of
the block length. Both the engine (`getParamDisplayGlobal`) and the original GUI
(`dtblkfx_src/GlobalCtrl.cpp:228`) display the achieved overlap rather than the
request, so `overlapText` does the same, and `overlapValueForText` inverts it so
typing `40%` gives 40%. That figure depends on the block length, so `FFT_LEN`
joins `DELAY` and `FX_TYPE` in triggering `updateHostDisplay()`.

`getOverlapParam` clamps its two halves to `0.499` / `0.501` so they cannot
collide, which loses just under 0.2% of the request. That is the original's own
arithmetic — `dtblkfx_src/GlobalCtrl.cpp:466` packs its overlap slider and sync
toggle exactly this way — and it is below the rounding of the displayed
percentage, so the split reaches the same 0% and 85% the packed parameter did.
The test asserts that.

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

**Group FX types so the mask effects stop looking broken.** `HarmMask`,
`AutoHarmMask`, `ASubH1Mask`, `ASubH2Mask`, `ASubH3Mask` and `ThreshMask`
produce a mask/envelope rather than audible output on their own, so picking
one in an otherwise plain FX slot sounds like nothing happened — not a bug,
but indistinguishable from one without context. Group the FX type menu (or
otherwise visually separate) so mask effects read as modifiers meant to be
combined with something else, rather than effects in their own right.

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
