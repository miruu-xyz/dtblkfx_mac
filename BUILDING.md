# Building DtBlkFx Dev (macOS universal)

Produces a universal (arm64 + x86_64) VST3 plus a Standalone app, ad-hoc signed
and installed to `~/Library/Audio/Plug-Ins/VST3/DtBlkFx Dev.vst3`.

The product name is deliberately **DtBlkFx Dev**, distinct from the upstream
`DtBlkFx_GUI` beta, so both can be installed and A/B'd in the same session.

## One-time setup

```bash
brew install cmake ninja
git submodule update --init modules/juce   # note: NOT --recursive, see below
./tools/build_fftw_universal.sh
```

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

No environment variables needed; the workarounds below are baked into
`CMakeLists.txt`.

## Local quirks this build works around

**Do not clone with `--recursive`.** The tree carries a `libpng` gitlink that
has no matching entry in `.gitmodules`, so a recursive clone or
`submodule update --init --recursive` fails. Only `modules/juce` is needed.

**Broken Command Line Tools libc++.**
`/Library/Developer/CommandLineTools/usr/include/c++/v1/` holds three stale
files from an old install and shadows the SDK's real libc++, so every C++
compile fails with `'cstdio' file not found`. The build injects
`-isystem <sdk>/usr/include/c++/v1` to route around it, including via the
environment, because JUCE builds its `juceaide` helper in a separate cmake
process that inherits nothing else.

To fix it properly and drop the workaround (requires an admin password):

```bash
sudo mv /Library/Developer/CommandLineTools/usr/include/c++/v1 \
        /Library/Developer/CommandLineTools/usr/include/c++/v1.stale
```

**SDK pinned to 12.1.** JUCE 6.0.7 calls `CGWindowListCreateImage`, which the
macOS 15 SDK marks *obsoleted* — a hard error. The Command Line Tools default
to the 26.2 SDK, so the build selects `MacOSX12.1.sdk`, the newest installed SDK
JUCE 6 still compiles against. Deployment target is 10.13.

**No AU.** JUCE 6 builds the Audio Unit's `.rsrc` with `Rez`, which needs full
Xcode. VST3 covers Ableton. Both constraints disappear on JUCE 7+.

**FFTW is vendored, not vcpkg'd.** `tools/build_fftw_universal.sh` builds
single-precision FFTW 3.3.10 for both arches and lipos them into
`third_party/fftw-universal/`. The x86_64 slice stops at SSE2 on purpose, since
it runs under Rosetta.

## Checking your build

```bash
./tools/check_audio.sh
```

Renders a fixed signal through every effect type and diffs the result against
`tests/baseline/core.fingerprint`. Takes about eight seconds. Run it after any
change to `src/core/`, the processor, or the build flags — see `CLAUDE.md` for
the rules, and `docs/ROADMAP.md` for what it has already turned up.

## Current deviations from upstream

- The DSP engine is a separate `dtblkfx_core` static library, so the offline
  harness can link it without JUCE.
- Output limiter defaults to **off** (`src/DtBlkFxProcessor.cpp`). It is a
  post-port addition rather than part of the original plugin, and has not been
  reviewed — see `docs/ROADMAP.md`, Phase 9.
