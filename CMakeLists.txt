# DtBlkFx — macOS universal build (arm64 + x86_64)
#
# Reworked for a local Homebrew/CLT toolchain:
#   * no vcpkg  — FFTW3f is vendored as a universal static lib in third_party/
#   * universal — one build produces both slices, for Live under Rosetta and native
#   * side-by-side — distinct product name & plugin code so it cannot collide with
#     the DtBlkFx_GUI.vst3 beta build
#
# See BUILDING.md for the local quirks, CLAUDE.md for the rules of the road.
cmake_minimum_required(VERSION 3.22)

# ── Architecture / SDK ────────────────────────────────────────────────────────
# Must be set before project(). Both slices in one build, lipo'd by the linker.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

# SDK choice is forced by JUCE, not by taste. It calls CGWindowListCreateImage,
# which the macOS 15 SDK marks *obsoleted* (a hard error, not a warning), and
# the Command Line Tools here default to the 26.2 SDK. That call isn't
# rewritten to use ScreenCaptureKit until JUCE 8.0.2 (confirmed against
# upstream's git history) -- staying on JUCE 7 (see docs/ROADMAP.md, Phase 4)
# means staying on a pre-15 SDK regardless. 12.3 is tried first in case a
# newer CLT install ships full headers for it, but on this machine its SDK
# bundle is missing usr/include/c++/v1 entirely, so it falls through to 12.1.
# Revisit when we move to JUCE 8+; override with -DCMAKE_OSX_SYSROOT=...
if(NOT DEFINED CMAKE_OSX_SYSROOT)
  set(_clt_sdks "/Library/Developer/CommandLineTools/SDKs")
  foreach(_sdk MacOSX12.3.sdk MacOSX12.1.sdk MacOSX12.sdk)
    if(EXISTS "${_clt_sdks}/${_sdk}/usr/include/c++/v1/cstdio")
      set(CMAKE_OSX_SYSROOT "${_clt_sdks}/${_sdk}" CACHE PATH "macOS SDK")
      break()
    endif()
  endforeach()
endif()
if(NOT CMAKE_OSX_SYSROOT)
  message(FATAL_ERROR
    "No JUCE 7-compatible macOS SDK found under /Library/Developer/CommandLineTools/SDKs.\n"
    "Pass one explicitly with -DCMAKE_OSX_SYSROOT=/path/to/MacOSXNN.sdk")
endif()

# ── Broken Command Line Tools libc++ workaround ───────────────────────────────
# /Library/Developer/CommandLineTools/usr/include/c++/v1 on this machine holds
# three stale files from an old install, and clang searches it *before* the
# SDK's real libc++ — so every C++ compile dies with "'cstdio' file not found".
# Point explicitly at the SDK's libc++. Harmless once the CLT is reinstalled.
#
# This has to happen before project(): the *_INIT variables are what seed the
# flags used by try_compile(), and JUCE probes the compiler during configure.
set(_sdk_cxx "${CMAKE_OSX_SYSROOT}/usr/include/c++/v1")
if(EXISTS "${_sdk_cxx}/cstdio")
  set(CMAKE_CXX_FLAGS_INIT    "-isystem${_sdk_cxx}")
  set(CMAKE_OBJCXX_FLAGS_INIT "-isystem${_sdk_cxx}")
  # JUCE builds its `juceaide` helper by shelling out to a *separate* cmake
  # invocation, which inherits none of the above. The environment is the only
  # channel that reaches it.
  set(ENV{SDKROOT} "${CMAKE_OSX_SYSROOT}")
  set(ENV{CXXFLAGS} "-isystem${_sdk_cxx}")
  set(ENV{OBJCXXFLAGS} "-isystem${_sdk_cxx}")
endif()

project(DTBLKFX VERSION 2.0.0)

option(DTBLKFX_BUILD_TOOLS "Build the offline render/regression harness" ON)

if(DEFINED CMAKE_CXX_FLAGS_INIT AND NOT CMAKE_CXX_FLAGS_INIT STREQUAL "")
  message(STATUS "Using SDK libc++ headers: ${_sdk_cxx}")
endif()

add_subdirectory(modules/juce)

# ── FFTW (single precision, universal, static) ────────────────────────────────
set(FFTW_UNIVERSAL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fftw-universal"
    CACHE PATH "Prefix holding a universal libfftw3f.a and fftw3.h")

if(NOT EXISTS "${FFTW_UNIVERSAL}/lib/libfftw3f.a")
  message(FATAL_ERROR
    "Universal libfftw3f.a not found at ${FFTW_UNIVERSAL}/lib.\n"
    "Build it with tools/build_fftw_universal.sh, or point -DFFTW_UNIVERSAL= at a prefix.")
endif()

add_library(fftw3f_universal STATIC IMPORTED GLOBAL)
set_target_properties(fftw3f_universal PROPERTIES
  IMPORTED_LOCATION "${FFTW_UNIVERSAL}/lib/libfftw3f.a"
  INTERFACE_INCLUDE_DIRECTORIES "${FFTW_UNIVERSAL}/include")

# ── DSP core ──────────────────────────────────────────────────────────────────
# Darrell Tam's 2003-2006 engine, patched for clang/arm64. Deliberately kept
# free of JUCE so the offline harness below can drive it without a plugin host.
add_library(dtblkfx_core STATIC
    src/core/DtBlkFx.cpp
    src/core/FxRun1_0.cpp
    src/core/FxState1_0.cpp
    src/core/GlobalData.cpp
    src/core/fftw_support.cpp
    src/core/rfftw_float.cpp
    src/core/fft_frac_shift.cpp
    src/core/misc_stuff.cpp
    src/core/NoteFreq.cpp
    src/core/sweep1_coeff.cpp
    src/core/sweep2_coeff.cpp
    src/core/sweep3_coeff.cpp
    src/core/sweep4_coeff.cpp
    src/core/sweep5_coeff.cpp)

# SYSTEM: the legacy headers are not warning-clean, and their noise would drown
# out warnings in our own code. Warnings inside src/core are silenced below.
target_include_directories(dtblkfx_core SYSTEM PUBLIC src/core)

# The engine is compiled for two channels; see BlkFxParam.h. STEREO also gates
# the stereo-only effect types (Vocode, CrossMix, WarpMix, HarmMatch L/R).
target_compile_definitions(dtblkfx_core PUBLIC STEREO=1)

target_link_libraries(dtblkfx_core PUBLIC fftw3f_universal)
target_compile_features(dtblkfx_core PUBLIC cxx_std_17)

# Not warning-clean and never will be. Silence it here only, so warnings in our
# own src/*.cpp stay visible.
target_compile_options(dtblkfx_core PRIVATE -w)

# ── Plugin ────────────────────────────────────────────────────────────────────
# PRODUCT_NAME and PLUGIN_CODE deliberately differ from the DtBlkFx_GUI beta so
# both can be installed and A/B'd in the same Live session.
juce_add_plugin(DtBlkFx
    PRODUCT_NAME "DtBlkFx Dev"
    COMPANY_NAME "haiori"
    PLUGIN_MANUFACTURER_CODE DtBl
    PLUGIN_CODE DtB3
    # JUCE 7.0.4+ no longer runs Rez for AU, so Command Line Tools alone
    # should now be enough -- but adding AU is deliberately out of scope for
    # Phase 4 (see docs/ROADMAP.md). Left as VST3 + Standalone only.
    FORMATS VST3 Standalone
    VST3_CATEGORIES Fx Filter
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    # Install is handled below instead, so that ad-hoc signing happens *before*
    # the bundle lands in the plug-in folder.
    COPY_PLUGIN_AFTER_BUILD FALSE)

juce_add_binary_data(DtBlkFxAssets SOURCES dtblkfx_src/dtblkfx_logo.png)

target_sources(DtBlkFx PRIVATE
    src/DtBlkFxProcessor.cpp
    src/DtBlkFxEditor.cpp)

target_include_directories(DtBlkFx PRIVATE
    src
    ${CMAKE_BINARY_DIR}/juce_binarydata_DtBlkFxAssets/JuceLibraryCode)

target_compile_definitions(DtBlkFx PUBLIC
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(DtBlkFx
    PUBLIC
        DtBlkFxAssets
    PRIVATE
        dtblkfx_core
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

target_compile_features(DtBlkFx PRIVATE cxx_std_17)

# ── Ad-hoc sign, then install ─────────────────────────────────────────────────
# The linker ad-hoc signs the Mach-O, but the *bundle* stays unsigned
# ("Info.plist=not bound"), which hosts can reject. Sign the whole bundle, then
# copy it into the user plug-in folder — never the system one, and never over
# the DtBlkFx_GUI beta, which has a different product name.
if(TARGET DtBlkFx_VST3)
  set(_vst3_dir "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
  add_custom_command(TARGET DtBlkFx_VST3 POST_BUILD
    COMMAND codesign --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_DIR:DtBlkFx_VST3>"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_vst3_dir}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_vst3_dir}/$<TARGET_BUNDLE_DIR_NAME:DtBlkFx_VST3>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<TARGET_BUNDLE_DIR:DtBlkFx_VST3>"
            "${_vst3_dir}/$<TARGET_BUNDLE_DIR_NAME:DtBlkFx_VST3>"
    VERBATIM
    COMMENT "Signing and installing DtBlkFx Dev.vst3")
endif()

# ── Parameter text check ──────────────────────────────────────────────────────
# Phase 5's counterpart to the audio harness: check_audio.sh drives the engine
# directly and never builds a JUCE parameter, so it is blind to everything the
# host actually reads. This links the plugin's shared code and exercises it.
add_executable(dtblkfx_paramtext tests/param_text_test.cpp)
target_link_libraries(dtblkfx_paramtext PRIVATE
    DtBlkFx
    dtblkfx_core
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_extra
    juce::juce_recommended_config_flags)
target_include_directories(dtblkfx_paramtext PRIVATE src)
target_compile_features(dtblkfx_paramtext PRIVATE cxx_std_17)

# ── Offline render / regression harness ───────────────────────────────────────
# See CLAUDE.md and docs/ROADMAP.md. Host-free, so it builds and runs fast.
if(DTBLKFX_BUILD_TOOLS)
  add_subdirectory(tools/render)
endif()
