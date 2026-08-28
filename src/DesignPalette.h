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

namespace design {

namespace colour {

// Window chrome
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
      , playerSans(load(BinaryData::PlayerSansMono8x8Classic_ttf,
                        BinaryData::PlayerSansMono8x8Classic_ttfSize))
  {
  }

  /** Row values, frequencies, dB readouts. Figma: 16px, tracking -0.48px. */
  juce::Font value(float px = 16.0f) const
  {
    return juce::Font(xenon).withPointHeight(px).withExtraKerningFactor(tracking);
  }

  /** Delay / Ovrlp / BlkLen headings. Figma: 28px wide-light, tracking -0.84px. */
  juce::Font heading(float px = 28.0f) const
  {
    return juce::Font(xenonWide).withPointHeight(px).withExtraKerningFactor(tracking);
  }

  /** Everything small and pixelly: the title bar, FILT/POWR, the sub-readouts. */
  juce::Font pixel(float px = 10.0f) const
  {
    return juce::Font(playerSans).withPointHeight(px);
  }

  juce::Typeface::Ptr xenon, xenonWide, playerSans;

private:
  // Both Monaspace sizes in the design are tracked at -3% of the font size.
  static constexpr float tracking = -0.03f;

  static juce::Typeface::Ptr load(const void* data, int size)
  {
    return juce::Typeface::createSystemTypefaceFor(data, (size_t)size);
  }
};

} // namespace design
