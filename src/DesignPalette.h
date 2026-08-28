/*
  ==============================================================================

    DesignPalette.h

    Every colour and typeface the redesigned GUI paints with, in one place.

    The source of truth is the Figma file, node 6-669:
    https://www.figma.com/design/K3L19ljhEUqG5VJoMwWyQg/DtBlkFx?node-id=6-669

    The design is a Windows 95 window: one base grey, a four-step bevel, the
    classic navy-to-cyan title gradient, and purple as the only accent. Values
    below are the literal hex/rgba from the design file -- if one looks wrong,
    check it against Figma rather than nudging it here.

  ==============================================================================
*/

#pragma once

#include "BinaryData.h"
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace design {

namespace colour {

// Window chrome. Two greys, not one: the window ground is near-white, while
// control faces (buttons, the help box, dropdowns) stay Win95 #C0C0C0 -- that
// is what `help_button.svg` fills its own rect with.
inline const juce::Colour windowBg{0xffe8eaef};
inline const juce::Colour baseGrey{0xffc0c0c0};
inline const juce::Colour bevelLight{0xffffffff};     // outermost highlight
inline const juce::Colour bevelLightSoft{0xffdfdfdf}; // inner highlight
inline const juce::Colour bevelDarkSoft{0xff808080};  // inner shadow
inline const juce::Colour bevelDark{0xff0a0a0a};      // outermost shadow

inline const juce::Colour titleBarLeft{0xff000080};
inline const juce::Colour titleBarRight{0xff1084d0};
inline const juce::Colour titleBarText{0xffffffff};

// Purple is the only accent in the design: the POWR label, active FX rows,
// the glow behind the global headings, the selected item in the FX menu.
inline const juce::Colour accent{0xff933bbf};
inline const juce::Colour accentBright{0xffa83ff8};
inline const juce::Colour accentGlow{0x66a83ff8}; // rgba(168,63,248,0.4)

// Text. The design leans on three levels of black rather than three greys.
inline const juce::Colour text{0xff000000};
inline const juce::Colour textDim{0x99000000};   // rgba(0,0,0,0.6) -- headings
inline const juce::Colour textFaint{0x4d000000}; // rgba(0,0,0,0.3) -- FILT
inline const juce::Colour warning{0xffd20000};   // the BlkLen "*"

// FX rows
inline const juce::Colour rowOutline{0xff999da6}; // dashed border
inline const juce::Colour fieldFill{0x33ffffff};  // dropdown, rgba(255,255,255,0.2)
inline const juce::Colour rangeDim{0x4dffffff};   // outside the frequency range

} // namespace colour

//==============================================================================
/** The three cuts the design uses, loaded once and shared.

    Hold one via `juce::SharedResourcePointer<design::FontStore>` -- the
    typefaces are ~800KB together and there is a LookAndFeel per component, so
    loading them per instance would be wasteful, and a file-scope static would
    outlive JUCE's leak detector.

    Sizes are the Figma px values. `withPointHeight` is the one that matches a
    CSS `font-size`; `withHeight` measures ascent+descent and would come out
    noticeably small.
*/
struct FontStore {
  FontStore()
      : xenon(load(BinaryData::MonaspaceXenonRegular_otf,
                   BinaryData::MonaspaceXenonRegular_otfSize))
      , xenonWide(load(BinaryData::MonaspaceXenonWideLight_otf,
                       BinaryData::MonaspaceXenonWideLight_otfSize))
      , xenonWideItalic(load(BinaryData::MonaspaceXenonWideLightItalic_otf,
                             BinaryData::MonaspaceXenonWideLightItalic_otfSize))
      , playerSans(load(BinaryData::PlayerSansMono8x8Classic_ttf,
                        BinaryData::PlayerSansMono8x8Classic_ttfSize))
  {
  }

  /** Row values, frequencies, dB readouts. Figma: 16px, tracking -0.48px. */
  juce::Font value(float px = 16.0f) const
  {
    return juce::Font(xenon).withPointHeight(px).withExtraKerningFactor(tracking);
  }

  /** Delay / Ovrlp / BlkLen headings. Figma: 28px wide-light, tracking -0.84px.

      The headings are set in the italic cut. A real italic cut rather than
      `Font::italicised`, which cannot synthesise a slant for an embedded
      typeface -- it would silently come back upright. */
  juce::Font heading(float px = 28.0f, bool italic = true) const
  {
    return juce::Font(italic ? xenonWideItalic : xenonWide)
        .withPointHeight(px)
        .withExtraKerningFactor(tracking);
  }

  /** Everything small and pixelly: the title bar, FILT/POWR, the sub-readouts. */
  juce::Font pixel(float px = 10.0f) const
  {
    return juce::Font(playerSans).withPointHeight(px);
  }

  juce::Typeface::Ptr xenon, xenonWide, xenonWideItalic, playerSans;

private:
  // Both Monaspace sizes in the design are tracked at -3% of the font size.
  static constexpr float tracking = -0.03f;

  static juce::Typeface::Ptr load(const void* data, int size)
  {
    return juce::Typeface::createSystemTypefaceFor(data, (size_t)size);
  }
};

//==============================================================================
/** The design's glyphs, as the exact path data from the Figma export.

    They are drawn on a 6x7 / 7x9 pixel grid, so they are taken verbatim rather
    than redrawn -- hand-approximating a 7px padlock does not survive contact
    with the design. The `.svg` files they came from are kept in `assets/icons/`
    for provenance; only the path data is compiled in.

    Path data rather than embedded SVG for two reasons: the colour is a state
    (a locked padlock, a lit power symbol, a hovered glyph) and has to be set at
    paint time, and `help_button.svg` carries its Win95 edge as an SVG inner-
    shadow filter, which JUCE's parser does not implement -- `drawBevel` draws
    that far better.

    Hold one via `juce::SharedResourcePointer<design::Glyphs>`.
*/
struct Glyphs {
  Glyphs()
      : lockLocked(parse("M2 4H5V1H6V4H7V8H6V9H0V4H1V1H2V4ZM5 1H2V0H5V1Z"))
      , lockUnlocked(parse("M2 4H7V8H6V5H1V8H6V9H0V4H1V1H2V4ZM6 2H5V1H6V2ZM5 1H2V0H5V1Z"))
      , lockLockedSmall(parse("M2 3H4V1H5V3H6V6H5V7H0V3H1V1H2V3ZM4 1H2V0H4V1Z"))
      , lockUnlockedSmall(parse("M2 3H6V6H5V4H1V6H5V7H0V3H1V1H2V3ZM5 2H4V1H5V2ZM4 1H2V0H4V1Z"))
      // On and off share one path in the export and differ only in fill, so
      // here the colour is the whole state.
      , power(parse("M6 9H2V8H6V9ZM2 8H1V7H2V8ZM7 8H6V7H7V8ZM1 7H0V3H1V7ZM8 7H7V3H8V7ZM4.5 "
                    "5H3.5V0H4.5V5ZM2 3H1V2H2V3ZM7 3H6V2H7V3Z"))
      , beatSyncOn(parse("M6 9H3V8H6V9ZM3 6H2V7H1V8H0V5H3V6ZM3 8H2V7H3V8ZM7 8H6V7H7V8ZM8 "
                         "7H7V5H8V7ZM2 4H1V2H2V4ZM9 4H6V3H7V2H8V1H9V4ZM3 2H2V1H3V2ZM7 "
                         "2H6V1H7V2ZM6 1H3V0H6V1Z"))
      // The off state ships as two paths: the arrows, cut back at the lower
      // right to make room, and the cross that goes there. Kept apart so the
      // arrows can be placed on the same grid as the on state's -- scaling the
      // combined shape to fit made them visibly smaller, because the cross
      // pushes the bounding box out to 10x10 against the on state's 9x9.
      , beatSyncOff(parse("M4 9H3V8H4V9ZM3 6H2V7H1V8H0V5H3V6ZM3 8H2V7H3V8ZM2 4H1V2H2V4ZM9 "
                          "4H6V3H7V2H8V1H9V4ZM3 2H2V1H3V2ZM7 2H6V1H7V2ZM6 1H3V0H6V1Z"))
      , beatSyncCross(parse("M9 5H10V6H9V5ZM5 5H6V6H5V5ZM8 6H9V7H8V6ZM6 6H7V7H6V6ZM7 7H8V8H7V7ZM8 "
                            "8H9V9H8V8ZM6 8H7V9H6V8ZM9 9H10V10H9V9ZM5 9H6V10H5V9Z"))
      , help(parse("M8.0625 15.1875V11.9375H11.3125V15.1875H8.0625ZM6.4375 7.0625V5.4375H8.0625V7"
                   ".0625H6.4375ZM8.0625 10.3125V8.6875H9.6875V7.0625H11.3125V5.4375H8.0625V3.812"
                   "5H12.9375V5.4375H14.5625V8.6875H12.9375V10.3125H8.0625Z"))
  {
  }

  juce::Path lockLocked, lockUnlocked, lockLockedSmall, lockUnlockedSmall;
  juce::Path power, beatSyncOn, beatSyncOff, beatSyncCross, help;

  /** Every beat-sync part is drawn on this grid. The arrows occupy 0..9 of it
      in both states, so they land identically whichever state is showing; the
      cross uses the last unit. */
  static constexpr float beatSyncGrid = 10.0f;

  const juce::Path& lock(bool locked, bool big) const
  {
    if (big)
      return locked ? lockLocked : lockUnlocked;
    return locked ? lockLockedSmall : lockUnlockedSmall;
  }

  /** The glyph scaled to sit centred in `target`, ready to draw or outline. */
  static juce::Path scaled(const juce::Path& path, juce::Rectangle<float> target)
  {
    if (path.isEmpty())
      return {};

    auto copy = path;
    copy.applyTransform(
        path.getTransformToScaleToFit(target, true, juce::Justification::centred));
    return copy;
  }

  /** As `scaled`, but mapping an explicit `source` box rather than the path's
      own ink bounds.

      Needed whenever two states of the same glyph must line up: scaling each by
      its own bounds sizes them to their own ink, so a state that happens to
      draw one pixel further out comes back smaller and offset. Pass both the
      same `source` and they land on the same grid. */
  static juce::Path
  scaledFrom(const juce::Path& path, juce::Rectangle<float> source, juce::Rectangle<float> target)
  {
    if (path.isEmpty() || source.isEmpty())
      return {};

    const auto scale = juce::jmin(target.getWidth() / source.getWidth(),
                                  target.getHeight() / source.getHeight());

    auto copy = path;
    copy.applyTransform(
        juce::AffineTransform::translation(-source.getX(), -source.getY())
            .scaled(scale, scale)
            .translated(target.getCentreX() - source.getWidth() * scale * 0.5f,
                        target.getCentreY() - source.getHeight() * scale * 0.5f));
    return copy;
  }

  /** Fill a glyph centred in `target`, scaled to fit, in `c`. */
  static void draw(juce::Graphics& g,
                   const juce::Path& path,
                   juce::Rectangle<float> target,
                   juce::Colour c)
  {
    g.setColour(c);
    g.fillPath(scaled(path, target));
  }

private:
  static juce::Path parse(const char* d) { return juce::Drawable::parseSVGPath(d); }
};

//==============================================================================
/** `text` set as a path, so it can be outlined and shadowed. `drawText` cannot
    do either, and it also clips to its box -- which is what was cropping the
    header readouts. */
inline juce::Path textAsPath(const juce::Font& font,
                             const juce::String& text,
                             float x,
                             float baselineY)
{
  juce::GlyphArrangement arrangement;
  arrangement.addLineOfText(font, text, x, baselineY);

  juce::Path path;
  arrangement.createPath(path);
  return path;
}

/** The design's raised lettering: a purple glow behind, a white outline, then
    the fill. Used for the global headings and the glyphs beside them.

    `outline` has to scale with the artwork. The headings are 28px and take a
    1.2px edge happily; the glyphs beside them are 9px, where the same edge
    closes up the counters and turns a padlock into a blob. */
inline void
drawRaised(juce::Graphics& g, const juce::Path& path, juce::Colour fill, float outline = 1.2f)
{
  if (path.isEmpty())
    return;

  juce::DropShadow(colour::accentBright.withAlpha(0.6f), 4, {0, 1}).drawForPath(g, path);

  g.setColour(colour::bevelLight);
  g.strokePath(
      path,
      juce::PathStrokeType(outline, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

  g.setColour(fill);
  g.fillPath(path);
}

} // namespace design
