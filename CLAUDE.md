# DtBlkFx — working notes for Claude

DtBlkFx is an FFT/spectral audio effect written by Darrell Tam between 2003 and
2006 for Windows VST2, GPL v2-or-later. This repo is a macOS port: the original
DSP engine, unchanged in behaviour, wrapped in JUCE and built as a universal
VST3.

The engine is the asset. It is twenty-year-old code that nobody currently
understands end to end, and the whole point of the port is that it still sounds
exactly like DtBlkFx. Treat `src/core/` as something to preserve, not to
modernise.

## Layout

| Path | What it is |
| --- | --- |
| `src/core/` | The DSP engine. **This is what gets compiled.** |
| `src/DtBlkFxProcessor.*` | JUCE `AudioProcessor` wrapper |
| `src/DtBlkFxEditor.*` | JUCE editor, plus `RetroLookAndFeel.h`, `SpectrogramComponent.h` |
| `src/core/vst2_stub.h` | Hand-written stand-in for the VST2 SDK, which cannot be redistributed |
| `dtblkfx_src/` | Pristine unmodified upstream copy. **Not compiled.** Reference only — diff against it to see what the port changed. |
| `tools/render/` | Offline render + audio regression harness |
| `tests/baseline/` | Checked-in audio fingerprints |
| `docs/ROADMAP.md` | What is planned, in what order, and why |
| `BUILDING.md` | Build instructions and the local toolchain quirks |

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Installs a signed universal VST3 to `~/Library/Audio/Plug-Ins/VST3/DtBlkFx Dev.vst3`.
See `BUILDING.md` for one-time setup and for why the SDK is pinned to 12.1.

## The audio guardrail

**Run this before and after any change that touches `src/core/`, the processor,
or the build flags:**

```bash
./tools/check_audio.sh
```

It renders a fixed signal through all 31 effect types plus the engine on its
own, reduces each render to a fingerprint (peak, RMS, L/R correlation, NaN
count, eight spectral bands), and diffs against `tests/baseline/core.fingerprint`.
Exit 0 means the audio did not move.

Rules for it:

- **A FAIL is information, not an obstacle.** It says the output changed. Decide
  whether that change was intended before doing anything else.
- **Never run `tools/regen_baseline.sh` to make a red check go green.**
  Regenerate only after listening to the change and concluding the new output is
  correct, and commit the regenerated baseline as its own change explaining what
  moved.
- **It is a change detector, not a correctness oracle.** It cannot tell you the
  plugin sounds good, only that it sounds the same as last time.
- Cheap enough to run constantly: about eight seconds.

Adding an effect or a parameter means adding a case in
`tools/render/dtblkfx_render.cpp` and regenerating.

## Landmines

Things in this codebase that have already cost time, or are waiting to:

- **The engine used to read uninitialised memory at startup — fixed in Phase
  3, watch for regressions.** `_chan[].x0`, `x1` and `x2` come from
  `fftwf_malloc`, which does not zero, and `DtBlkFx::init()` clears only `x3`
  despite its "clear out all buffers" comment. Left alone, the first FFT
  window of an instance's life transforms whatever the allocator handed over,
  which is where the NaN bursts came from. The constructor now clears those
  three buffers once, right after the `resize()` calls, in
  `src/core/DtBlkFx.cpp` — do not remove that clear or move it into
  `init()`/`suspend()` (see `docs/ROADMAP.md`, Phase 3, for why those are the
  wrong places). **The harness running one case per process does not exercise
  this at all — it hides it**, because a fresh process gets zeroed pages from
  the kernel and a DAW does not; `--in-process --repeat` is what actually
  proves it, via the fingerprint hash added in Phase 2.1.
- **`g_rand_i` is shared by every instance and must stay that way.** A single
  process-global PRBS in `FxRun1_0.cpp` randomises phase in Smear. It is why two
  Smear instances never sound the same, it is original behaviour, and it is the
  one thing in the engine that is *supposed* to be non-deterministic. Do not
  "fix" it; seed it from the test side if a test needs reproducibility.
- **`unsigned long` is 32-bit on Windows and 64-bit here.** `misc_stuff.h`
  typedefs `uint32` to `unsigned long`, and `LittleEndianMemStr::put32/get32`
  assume four bytes. Anything touching chunk serialisation needs checking
  against this.
- **`vst2_stub.h` leaves `VstTimeInfo` uninitialised** and hands the same
  instance back from every `getTimeInfo()` call, so the engine's tempo tracking
  reads whatever is on the stack. `setInitialDelay()` is a no-op, so plugin
  latency is never reported to the host. The harness pins `timeInfo` explicitly.
- **Parameters are not what they look like.** Several 0..1 controls pack a mode
  flag and an amount into one value, and are not monotonic:
  - `MIX_BACK` — lower half is power-match mode, upper half filter mode; `0.5`
    is *fully dry*, not half. Use `BlkFxParam::getMixbackParam`.
  - `DELAY` — integer half selects beats vs milliseconds; `0.4` is 6.4 beats
    while `0.6` is 1.2 seconds. Use `BlkFxParam::Delay::msec` / `::beats`.
  - `FX_TYPE` — quantised to Renoise's effect numbering,
    `(long)(param * 255.0) / 8`. Use `getEffectTypeInv(index)`.
  - `OVERLAP` — triangular morph, so `0.15` and `0.85` give the same overlap
    amount; the upper half is supposed to add beat-sync.
- **The requested FFT length is not the one used.** `guessFFTLen` caps it by how
  much input is buffered, which `DELAY` controls, so `FFT_LEN` above roughly 0.3
  does nothing at zero delay.
- **Output depends on the host buffer size.** Rendering at 2048 samples per
  block gives measurably different audio from 512. Baselines are always taken at
  512.
- **The engine is compiled `AUDIO_CHANNELS = 2`** and always dereferences
  channel index 1, while `isBusesLayoutSupported` currently accepts a mono
  layout. See Phase 7.

## House rules

- **Do not modify `dtblkfx_src/`.** It is the reference copy.
- **Do not touch `~/Downloads/DtBlkFx_GUI.vst3.zip` or install anything named
  `DtBlkFx_GUI`.** That is the user's saved beta, kept for A/B comparison. This
  build is deliberately `DtBlkFx Dev` with plugin code `DtB3` so the two can
  coexist.
- **Do not launch the Standalone build unattended.** JUCE's standalone wrapper
  opens the default audio input *and* output, which can feed back through
  monitors.
- **Do not commit unless asked.**
- Warnings inside `src/core/` are suppressed on purpose (`-w`, and the headers
  are `SYSTEM` includes). Do not "clean up" the engine's warnings; that is how
  behaviour changes sneak in.
