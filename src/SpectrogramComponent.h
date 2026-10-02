/*
  ==============================================================================

    SpectrogramComponent.h

    Phase 6.7: the spectrogram, rebuilt to the original's behaviour
    (dtblkfx_src/Spectrogram.cpp) on the shared coordinate model.

    Frequency runs across on the same axis as the FX rows' handles
    (DesignAxis.h), so a column sits exactly over the frequency a handle at that
    x would set. Time runs up: each new line enters at the bottom and the
    display scrolls upwards, as the original did.

    It is a display, not a control. Hovering draws a hairline -- mirrored on
    the other spectrogram -- and reads out the frequency under it; clicking
    pauses. While a row's range is being worked, the region that row processes
    is shown inverted, as the original's FxCtrl::doHiliteSgrams did.

  ==============================================================================
*/

#pragma once

#include "DesignAxis.h"
#include "DesignPalette.h"
#include <array>
#include <juce_gui_basics/juce_gui_basics.h>

namespace design {

/** The original's colour map (Gui.cpp): black, blue, cyan, green, yellow, red,
    linear between those six stops, over log power from 1e-8 (-80 dB) to 0.04
    (-14 dB) -- Spectrogram.cpp's `_pwr_min` and `_pwr_max`. The engine scales
    the data the way the original's did, so those thresholds carry over. */
inline juce::Colour spectrogramColour(float power)
{
  static const std::array<juce::Colour, 1000> lut = [] {
    const float stops[][4]{{0.0f, 0.0f, 0.0f, 0.0f},
                           {0.2f, 0.0f, 0.0f, 1.0f},
                           {0.4f, 0.0f, 1.0f, 1.0f},
                           {0.6f, 0.0f, 0.8f, 0.0f},
                           {0.8f, 1.0f, 1.0f, 0.0f},
                           {1.0f, 1.0f, 0.0f, 0.0f}};

    std::array<juce::Colour, 1000> out;
    for (size_t i = 0; i < out.size(); ++i) {
      const float t = (float)i / (float)(out.size() - 1);
      size_t s = 0;
      while (s + 2 < std::size(stops) && t > stops[s + 1][0])
        ++s;

      const auto& a = stops[s];
      const auto& b = stops[s + 1];
      const float f = juce::jlimit(0.0f, 1.0f, (t - a[0]) / (b[0] - a[0]));
      out[i] = juce::Colour::fromFloatRGBA(a[1] + (b[1] - a[1]) * f,
                                           a[2] + (b[2] - a[2]) * f,
                                           a[3] + (b[3] - a[3]) * f,
                                           1.0f);
    }
    return out;
  }();

  constexpr float floor = 1e-8f, ceiling = 0.04f;
  if (!(power > floor)) // NaN included
    return lut.front();

  const float t = (std::log(power) - std::log(floor)) / (std::log(ceiling) - std::log(floor));
  return lut[(size_t)juce::jlimit(0, (int)lut.size() - 1, (int)(t * (float)lut.size() + 0.5f))];
}

} // namespace design

class SpectrogramComponent : public juce::Component {
public:
  explicit SpectrogramComponent(const juce::String& labelText)
      : label(labelText)
  {
    setOpaque(true);
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
  }

  /** History shown top to bottom, at any sample rate. */
  static constexpr double historySeconds = 1.5;

  void setLabel(const juce::String& newLabel)
  {
    label = newLabel;
    repaint();
  }

  /** The other spectrogram, which mirrors this one's hover hairline. */
  void setLinked(SpectrogramComponent* other) { linked = other; }

  //============================================================================
  /** One block of FFT power from the engine (bins 0..fftLen/2). Reduced to
      columns at once and max-held until the next row comes into view, so a
      transient between two rows is never lost. */
  void pushBlock(const float* data, int numBins, double sampleRate)
  {
    if (paused || pending.empty() || numBins < 2)
      return;

    const int fftLen = (numBins - 1) * 2;
    if (fftLen != edgesFftLen || sampleRate != edgesRate) {
      edges = design::axis::binEdges((int)pending.size(), fftLen, sampleRate);
      edgesFftLen = fftLen;
      edgesRate = sampleRate;
    }

    for (int c = 0; c < (int)pending.size(); ++c) {
      const auto bins = design::axis::columnBins(edges, c);
      float v = 0.0f;
      for (int b = bins.getStart(); b < bins.getEnd(); ++b)
        v = std::max(v, data[b]);
      pending[(size_t)c] = std::max(pending[(size_t)c], v);
    }
    hasPending = true;
    lastDataMs = juce::Time::getMillisecondCounterHiRes();
  }

  /** Commit the next row: the max held since the last one, or -- when no block
      has arrived since -- a repeat of the last, so a block spans the rows its
      time covers instead of leaving gaps. Public so the offscreen check can
      drive it without a clock. */
  void writeLine()
  {
    if (image.isNull())
      return;

    if (hasPending) {
      current = pending;
      std::fill(pending.begin(), pending.end(), 0.0f);
      hasPending = false;
    }

    {
      juce::Image::BitmapData normal(image, juce::Image::BitmapData::writeOnly);
      juce::Image::BitmapData inverse(inverted, juce::Image::BitmapData::writeOnly);
      for (int c = 0; c < normal.width; ++c) {
        const auto colour = design::spectrogramColour(current[(size_t)c]);
        normal.setPixelColour(c, writeRow, colour);
        inverse.setPixelColour(c, writeRow, invert(colour));
      }
    }

    writeRow = (writeRow + 1) % image.getHeight();
    repaint();
  }

  void setPaused(bool shouldBe)
  {
    if (paused == shouldBe)
      return;

    paused = shouldBe;
    std::fill(pending.begin(), pending.end(), 0.0f);
    hasPending = false;
    repaint();
  }

  bool isPaused() const { return paused; }

  /** Invert the region a row processes, in frequency-parameter terms -- `a` to
      `b`, or everything outside them when they are crossed, as the engine
      treats it. */
  void setHighlight(bool on, float a = 0.0f, float b = 0.0f)
  {
    if (on == highlightOn && (!on || (a == highlightA && b == highlightB)))
      return;

    highlightOn = on;
    highlightA = a;
    highlightB = b;
    repaint();
  }

  //============================================================================
  void paint(juce::Graphics& g) override
  {
    using namespace design;

    g.fillAll(juce::Colours::black);
    if (image.isNull())
      return;

    const auto span = axis::span(getLocalBounds().toFloat());
    const float x0 = (float)juce::roundToInt(span.getStart());
    const int w = image.getWidth(), h = image.getHeight();

    // Oldest at the top, newest at the bottom: the circular buffer drawn in two
    // pieces either side of the next row to be written. The image is one row
    // taller than the component, and the whole stack sits `scroll` of a row
    // higher -- so the newest row rises into view continuously instead of
    // arriving all at once, which is what made it look choppy.
    const float y0 = -(float)scroll;
    auto drawScrolled = [&](const juce::Image& img) {
      if (h - writeRow > 0)
        g.drawImageTransformed(img.getClippedImage({0, writeRow, w, h - writeRow}),
                               juce::AffineTransform::translation(x0, y0));
      if (writeRow > 0)
        g.drawImageTransformed(img.getClippedImage({0, 0, w, writeRow}),
                               juce::AffineTransform::translation(x0, y0 + (float)(h - writeRow)));
    };

    drawScrolled(image);

    if (highlightOn) {
      const float xA = axis::paramToX(highlightA, span), xB = axis::paramToX(highlightB, span);
      juce::RectangleList<int> region;
      auto add = [&](float from, float to) {
        region.add(juce::Rectangle<float>(from, 0.0f, to - from + 1.0f, (float)h)
                       .getSmallestIntegerContainer());
      };

      if (highlightA <= highlightB) {
        add(xA, xB);
      }
      else {
        add(span.getStart(), xB);
        add(xA, span.getEnd());
      }

      juce::Graphics::ScopedSaveState save(g);
      g.reduceClipRegion(region);
      drawScrolled(inverted);
    }

    // Octave ticks along the bottom edge, at every C, as the original drew
    // them -- here in the row outline's grey so they read as part of the same
    // scale as the handles beneath.
    g.setColour(colour::rowOutline);
    for (int octave = 0; octave * 12.0f <= BlkFxParam::noteSpan(); ++octave) {
      const float x = axis::paramToX(octave * 12.0f / BlkFxParam::noteSpan(), span);
      g.fillRect(juce::roundToInt(x), getHeight() - tickHeight, 1, tickHeight);
    }

    if (hoverParam >= 0.0f) {
      g.setColour(colour::rowOutline);
      g.fillRect(juce::roundToInt(axis::paramToX(hoverParam, span)), 0, 1, getHeight());
    }

    // Paused: the whole display, ground included, fades a quarter of the way
    // toward the window's own grey -- the component is opaque, so this is how
    // it lowers its opacity. Before the caption, which stays crisp and says
    // "PAUSED".
    if (paused)
      g.fillAll(colour::windowBg.withAlpha(pausedFade));

    // White with a black outline, as the original's OutlineDrawString did, so
    // it reads over whatever the spectrogram is showing beneath it.
    const auto font = fonts->pixel(10.0f);
    const auto text = caption();
    const auto path = textAsPath(font,
                                 text,
                                 (float)getWidth() - 7.0f - font.getStringWidthFloat(text),
                                 7.0f + font.getAscent());
    g.setColour(juce::Colours::black);
    g.strokePath(path, juce::PathStrokeType(2.0f, juce::PathStrokeType::mitered));
    g.setColour(colour::bevelLight);
    g.fillPath(path);
  }

  void resized() override
  {
    const int columns = juce::jmax(1, (int)design::axis::span(getLocalBounds().toFloat()).getLength());
    const int rows = juce::jmax(1, getHeight()) + 1; // the one rising into view

    image = juce::Image(juce::Image::RGB, columns, rows, true);
    inverted = juce::Image(juce::Image::RGB, columns, rows, false);
    inverted.clear(inverted.getBounds(), juce::Colours::white);

    pending.assign((size_t)columns, 0.0f);
    current.assign((size_t)columns, 0.0f);
    hasPending = false;
    edgesFftLen = 0;
    writeRow = 0;
    scroll = 0.0;
  }

  //============================================================================
  void mouseMove(const juce::MouseEvent& e) override
  {
    const auto span = design::axis::span(getLocalBounds().toFloat());
    const float x = (float)e.x;
    setHover(span.contains(x) || x == span.getEnd() ? design::axis::xToParam(x, span) : -1.0f, true);
  }

  void mouseExit(const juce::MouseEvent&) override { setHover(-1.0f, false); }

  /** Pauses both spectrograms: they show the same moment, and freezing one
      while the other runs on would break that. */
  void mouseDown(const juce::MouseEvent&) override
  {
    const bool pause = !paused;
    setPaused(pause);
    if (linked != nullptr)
      linked->setPaused(pause);
  }

  /** Hairline at `param` (negative clears it), mirrored on the linked
      spectrogram; the readout only on this one. */
  void setHover(float param, bool showReadout)
  {
    hoverParam = param;
    readout = showReadout && param >= 0.0f;
    repaint();

    if (linked != nullptr) {
      linked->hoverParam = param;
      linked->readout = false;
      linked->repaint();
    }
  }

private:
  static juce::Colour invert(juce::Colour c)
  {
    return juce::Colour(255 - c.getRed(), 255 - c.getGreen(), 255 - c.getBlue());
  }

  /** The label, or the frequency under the pointer -- Hz and note, in the
      engine's own formatting, as the original's info overlay showed them. */
  juce::String caption() const
  {
    juce::String text = label;

    if (readout) {
      const float hz = design::axis::paramToHz(hoverParam);
      CharArray<64> buf;
      std::memset(buf.data, 0, sizeof(buf.data));
      buf << sprnum(hz, /*min unit*/ 1.0f) << "Hz  " << HzToNote(hz);
      text = juce::String(buf.data);
    }

    if (paused)
      text = readout ? "PAUSED  " + text : juce::String("PAUSED");
    return text;
  }

  /** Called on every display refresh. Moves the display up at a constant
      rate -- `historySeconds` top to bottom -- committing rows as they come
      into view. Holds still while paused, and when no block has arrived for a
      quarter of a second: a host that stops processing should not scroll the
      display into nothing. */
  void onFrame()
  {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = juce::jmin(now - lastFrameMs, 100.0);
    lastFrameMs = now;

    if (paused || image.isNull() || now - lastDataMs > 250.0)
      return;

    const double rowsPerMs = (double)(image.getHeight() - 1) / (historySeconds * 1000.0);
    scroll += dt * rowsPerMs;

    for (; scroll >= 1.0; scroll -= 1.0)
      writeLine();

    repaint();
  }

  static constexpr int tickHeight = 7;
  static constexpr float pausedFade = 0.25f;

  juce::SharedResourcePointer<design::FontStore> fonts;
  juce::String label;
  SpectrogramComponent* linked = nullptr;

  juce::Image image, inverted;
  int writeRow = 0;

  std::vector<float> pending, current;
  bool hasPending = false;
  std::vector<int> edges;
  int edgesFftLen = 0;
  double edgesRate = 0.0;

  double scroll = 0.0, lastFrameMs = 0.0, lastDataMs = 0.0;
  bool paused = false;

  float hoverParam = -1.0f;
  bool readout = false;

  bool highlightOn = false;
  float highlightA = 0.0f, highlightB = 0.0f;

  // Last, so it is destroyed first: it calls back into everything above.
  juce::VBlankAttachment vblank{this, [this] { onFrame(); }};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrogramComponent)
};
