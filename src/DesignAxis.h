/*
  ==============================================================================

    DesignAxis.h

    Phase 6.5: the one coordinate model the FX rows and the spectrograms share.
    If each grew its own mapping they would drift apart on screen and the
    alignment would get fixed twice.

    The frequency axis is the frequency *parameter*, linearly. That is already a
    log-frequency axis -- the parameter is a note offset, 0..1 spanning 127.5
    semitones from C0 (16.35 Hz) to 25.8 kHz, with 0 special-cased to 0 Hz --
    and it is exactly what the original drew: `BlkFxParam::genPixelToHz` sets
    `param = x / width`. So the right edge is 25.8 kHz whatever the sample rate,
    and a handle sits at its raw parameter value, not snapped to an FFT bin.

    The amp axis is the amp parameter, linearly, on the same kind of strip, with
    the 0 dB point wherever the engine puts it rather than where the Figma drew
    it.

    Nothing here draws. See docs/PHASE6.md, 6.5.

  ==============================================================================
*/

#pragma once

#include "BlkFxParam.h"
#include <juce_graphics/juce_graphics.h>
#include <vector>

namespace design::axis {

/** Both ends of an axis sit this far in from the component's edges. A handle
    is a 9px triangle centred on its value, so without it one at 0 Hz or
    25.8 kHz would be half outside the row. The original did the same, laying
    its pixel table over `getWidth() - 10`. The spectrograms use the same inset,
    which is what puts their first and last columns over those handles. */
inline constexpr float inset = 5.0f;

/** The strip an axis is laid across, in the caller's coordinates. */
inline juce::Range<float> span(juce::Rectangle<float> bounds)
{
  return {bounds.getX() + inset, bounds.getRight() - inset};
}

//==============================================================================
// Parameter <-> x. Both axes are a 0..1 parameter laid linearly across a span.

inline float paramToX(float param, juce::Range<float> s)
{
  return s.getStart() + juce::jlimit(0.0f, 1.0f, param) * s.getLength();
}

inline float xToParam(float x, juce::Range<float> s)
{
  return s.getLength() <= 0.0f ? 0.0f
                               : juce::jlimit(0.0f, 1.0f, (x - s.getStart()) / s.getLength());
}

//==============================================================================
// Frequency. The engine's own conversions, not a second copy of the formula.

inline float paramToHz(float param)
{
  return BlkFxParam::getHz(param);
}

inline float hzToParam(float hz)
{
  return hz <= 0.0f ? 0.0f : juce::jlimit(0.0f, 1.0f, HzToNoteOffs(hz) / BlkFxParam::noteSpan());
}

//==============================================================================
// Amp.

/** The amp parameter's 0 dB point -- 0.6. For the ten `ampMixMode()` effects
    this is also where the value stops being a linear dry/wet fraction and
    becomes gain, which is the split 6.6's wedge has to show. Whether a row is
    in mix mode belongs to its parked effect, not to the axis. */
inline float ampUnityParam()
{
  return BlkFxParam::getAmpParam0dB();
}

//==============================================================================
// Spectrogram columns. A port of `dtblkfx_src/PixelFreqBin.cpp`.

/** FFT bin edges for `columns` spectrogram columns laid across a frequency
    span: column c shows the power in bins `[edges[c], edges[c + 1])`. Returns
    `columns + 1` edges, non-decreasing, from 0 up to whichever comes first of
    one past Nyquist (`fftLen / 2 + 1`) and the axis's own top, 25.8 kHz.

    Column c covers parameters `[c, c + 1) / columns`, which is what
    `paramToX` puts under screen column c. The original put each pixel's
    boundaries half a pixel either side of `c / columns` instead -- that is its
    `half_pix` factor -- because it labelled pixels by their left edge; defined
    by edges, the same thing falls out directly.

    Two deliberate differences from the original. Columns wholly above Nyquist
    come back empty; the original clamped them to the Nyquist bin and so
    repeated it across the top of the display. And at 88.2/96 kHz, where
    Nyquist is past the axis, the last column stops at 25.8 kHz rather than
    folding everything up to Nyquist into one pixel. */
inline std::vector<int> binEdges(int columns, int fftLen, double sampleRate)
{
  std::vector<int> edges((size_t)juce::jmax(columns, 0) + 1, 0);
  if (columns <= 0 || fftLen <= 0 || sampleRate <= 0.0)
    return edges;

  const int end = fftLen / 2 + 1; // one past the Nyquist bin
  const double binsPerHz = (double)fftLen / sampleRate;

  for (int c = 1; c < columns; ++c) {
    const double bin = BlkFxParam::getHz((float)c / (float)columns) * binsPerHz;
    edges[(size_t)c] = juce::jlimit(edges[(size_t)c - 1], end, juce::roundToInt(bin));
  }

  const int top = juce::roundToInt(BlkFxParam::getHz(1.0f) * binsPerHz);
  edges[(size_t)columns] = juce::jlimit(edges[(size_t)columns - 1], end, top);
  return edges;
}

/** The bins column c actually shows. Never empty below Nyquist: at the low end
    several columns fall inside one bin, and each of them shows that bin -- as
    the original's `getMaxVals` does with its "always do at least one bin". Empty
    above Nyquist, which draws as silence. `edges.back()` is the end of the
    displayable range, not necessarily of the FFT. */
inline juce::Range<int> columnBins(const std::vector<int>& edges, int column)
{
  const int start = edges[(size_t)column];
  const int end = edges.back();
  if (start >= end)
    return {};

  return {start, juce::jmax(edges[(size_t)column + 1], start + 1)};
}

} // namespace design::axis
