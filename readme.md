# DtBlkFx (macOS Universal Edition)

**DtBlkFx** is a Fast Fourier Transform (FFT) based VST plug-in for macOS, originally created by Darrell Tam and updated for modern macOS (Universal Binary) and JUCE by haiori.

It works by processing audio in the frequency domain, allowing for unique effects like:
- **Spectral Vocoding**
- **Harmonic Filtering**
- **Frequency Shifting**
- **Spectral Smearing**

> [!NOTE]
>
> | [![AI:X EXECUTE](https://img.shields.io/badge/AI-EXECUTE-grey?style=flat&logoSize=auto&labelColor=13171f)](https://0rwx.com) | This forks macOS port and UX improvements (WIP) have been vibecoded based on the existing work on dtblkfx (see [Credits](#credits)). <br>I've guided the AI to the best of my knowledge but I cannot read or write C++ |
> | --- | --- |

## Features
- **Universal Support**: Native compatibility for both Apple Silicon (M1/M2/M3) and Intel processors.
- **Modern GUI**: Rebuilt user interface using the JUCE framework.
- **Stereo Processing**: True stereo operation for all effects.
- **Ad-hoc Signed**: Ready for local development and use in DAWs like Ableton Live.

---

## The Goal

Update DtBlkFx to a modern and nice-to-use state on macOS: a universal 
VST3 (and maybe AU in the future) that is as fun to use as the original 
2003 plugin, with a more faithful adaptaion of the GUI and parameters, 
which the original repo didn't reproduce.
The original engine should stay as close to the original as possible,
minor changes due to JUCE upgrades are to be expected.

## About this fork

This fork continues [haiori's macOS port](https://github.com/hai0ri/dtblkfx_mac).
I'm not a professional software developer but a huge fan of the original plugin 
so I wanted to get a modern macOS version of it asap.

Some notes if you wanna build it yourself:

- **One universal build.** `arm64` and `x86_64` in a single CMake invocation, so
  the plugin runs natively and under Rosetta. vcpkg is gone; FFTW is built from
  source for both slices by `tools/build_fftw_universal.sh`.
- **Built as `DtBlkFx Dev`** with its own plugin code, so it can be installed
  alongside an existing `DtBlkFx_GUI` build and A/B'd in the same session.

Start with [`BUILDING.md`](BUILDING.md) to build it,
[`CLAUDE.md`](CLAUDE.md) for how the code is laid out and what to watch out for
in the engine, and [`docs/ROADMAP.md`](docs/ROADMAP.md) for what is planned —
GUI rework, real parameter units, factory presets, and a true stereo sidechain
are all scoped there but not yet built. For what the effects actually do, see
the original [`docs/MANUAL.md`](docs/MANUAL.md).

## Installation

Building installs the plugin for you, to
`~/Library/Audio/Plug-Ins/VST3/DtBlkFx Dev.vst3`. Rescan in your DAW afterwards.

There is no Audio Unit build yet: JUCE 6 builds the AU's resource fork with
`Rez`, which requires a full Xcode install rather than just the Command Line
Tools. See `docs/ROADMAP.md`, Phase 4.

---

## Building from Source

### Prerequisites
- **CMake** 3.22+ and **Ninja** — `brew install cmake ninja`
- **Xcode Command Line Tools**

### Build Instructions

1.  **Clone the repository**. Note: *not* `--recursive`; only JUCE is needed.
    ```bash
    git clone https://github.com/miruu-xyz/dtblkfx_mac.git
    cd dtblkfx_mac
    git submodule update --init modules/juce
    ```

2.  **Build FFTW once** for both architectures:
    ```bash
    ./tools/build_fftw_universal.sh
    ```

3.  **Build the plugin**:
    ```bash
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ```

4.  **Check that the audio is unchanged**:
    ```bash
    ./tools/check_audio.sh
    ```

`BUILDING.md` covers the toolchain quirks this build works around, including why
the macOS SDK is pinned.

## Usage
- **Mix Back**: Controls the balance between the original and processed signal.
- **Delay**: Adds a delay to the processed signal.
- **FFT Length**: Adjusts the size of the FFT window (frequency resolution vs. time resolution).
- **Overlap**: Controls the overlap of FFT windows (smoother sound vs. CPU usage).
- **Effect Parameters**:
    - **Freq A/B**: Frequency range for the effect.
    - **Amp**: Amplitude of the effect.
    - **Val**: Effect-specific parameter.

Several of these controls pack a mode flag and an amount into one value and are
not monotonic — see the "Landmines" section of `CLAUDE.md` before automating them.

For a full walkthrough of every effect and mask type, see
[`docs/MANUAL.md`](docs/MANUAL.md) — Darrell Tam's original
DtBlkFx user guide, carried over from the
[upstream repo](https://codeberg.org/skullzy/DtBlkFx/src/branch/main/docs).

## License
This project is licensed under the GNU General Public License v2.0 (or later). See `COPYING` for details.

## Credits
- **Original Author**: Darrell Tam
- **macOS/JUCE Port**: haiori
- **Modernization / UX rework (WIP)**: miruu
