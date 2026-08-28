# Phase 6 — GUI redesign

The working breakdown for the GUI rebuild. `docs/ROADMAP.md` keeps the one-
paragraph summary of why Phase 6 exists; this file is the day-to-day
reference.

## The design

Figma, file `K3L19ljhEUqG5VJoMwWyQg`:

| Frame | Node | What it covers |
| --- | --- | --- |
| DtBlkFx Plugin | [`6-669`](https://www.figma.com/design/K3L19ljhEUqG5VJoMwWyQg/DtBlkFx?node-id=6-669) | The whole window, 652 × 912 |
| Interactions | [`6-662`](https://www.figma.com/design/K3L19ljhEUqG5VJoMwWyQg/DtBlkFx?node-id=6-662) | Mask-FX outline behaviour |
| Components | [`6-1653`](https://www.figma.com/design/K3L19ljhEUqG5VJoMwWyQg/DtBlkFx?node-id=6-1653) | FX row states, FX type menu, lock/power/beat-sync glyphs |

It is a Windows 95 window: `#C0C0C0` base, a four-step bevel, a navy-to-cyan
title bar, and purple as the only accent. The current editor is a dark theme
with red accents, so this is a repaint, not a re-skin.

**The FX row contains no JUCE slider.** In the design the row *is* the control:
a full-width amp wedge, a frequency window with triangular drag handles, and
five text cells you click to type into. That is the largest single piece of
work in this phase and it is deliberately last but one.

## Fonts

Both are SIL OFL 1.1 and are vendored in `assets/fonts/` with their licences,
embedded via `juce_add_binary_data`, and loaded through `design::FontStore`.
Nothing has to be installed on the user's machine.

| Cut | File | Used for | Figma size |
| --- | --- | --- | --- |
| Monaspace Xenon Regular | `MonaspaceXenonRegular.otf` | row values, frequencies, dB | 16px, tracking −0.48 |
| Monaspace Xenon Wide Light | `MonaspaceXenonWideLight.otf` | Delay / Ovrlp / BlkLen headings | 28px, tracking −0.84 |
| Player Sans Mono 8x8 Classic | `PlayerSansMono8x8Classic.ttf` | title bar, FILT/POWR, sub-readouts | 10–12px |

Two things about them that have already cost time:

- **Both Monaspace cuts report the family name `Monaspace Xenon`.** They cannot
  be told apart by name, so always build fonts from the `Typeface::Ptr` in
  `FontStore`, never from `juce::Font("Monaspace Xenon", …)`. The check in
  `tests/param_text_test.cpp` measures `"BlkLen"` in both cuts (105px vs 131px
  at 28pt) precisely because a name comparison would not catch a mis-wire.
- **`withPointHeight`, not `withHeight`.** Figma's px sizes are CSS
  `font-size`, which is em size. `juce::Font::withHeight` sets ascent+descent
  and comes out noticeably small.

Pixel-accuracy on Player Sans is explicitly *not* a goal — the design scales it
off its 8×8 grid in places on purpose, for the blurry retro feel.

## Sub-phases

Each one ends with `./tools/check_audio.sh` green (nothing here touches
`src/core/`, so a FAIL means something is badly wrong) and, where parameter
text is involved, `./build/dtblkfx_paramtext`.

**Grill before starting each sub-phase** — walk the Figma frame together and
confirm the hover/click/drag behaviour before writing code, since the design
file shows states but not transitions.

### 6.1 — Fonts and palette ✅

Embed the three cuts; add `src/DesignPalette.h` as the single home for every
colour and typeface; repoint `RetroLookAndFeel` and the editor's hard-coded
colours at it. Layout does not move — this is the current editor in the new
typeface and colours, so that the thing that would look wrong for longest is
fixed first.

Done. See "What 6.1 landed" below.

### 6.2 — Chrome and shell ✅

The Win95 title bar (gradient, `?` help button), the outer 652 × 912 frame and
bevel, the footer (Presets dropdown, RANDOM button), and the header row: the
38px FILT/POWR knob with its rotated labels, plus the three "Top Options" units
(28px heading, 10px readout beneath, lock and beat-sync glyphs top-right).

Static-ish and independent of the row rewrite, so it can land early and make
the window read correctly.

**Settled before starting (grilled 2026-08-28):**

- **652 x 912, fixed for now.** The Figma is built on auto-layout, so lay the
  chrome out structurally rather than pixel-placing it -- resizing gets tested
  and fixed after 6.7, and a layout of hard-coded coordinates would have to be
  written twice.
- **The three global headings are the control.** The whole heading block drags
  (the original was a `DtPopupHSlider` -- drag *and* a popup menu of named
  values); right-click opens the menu, double-click types a value. Change the
  mouse cursor on hover so the affordance is not invisible.
- **The knob is `MixBack`.** Drag vertically or horizontally for the amount;
  the "75" is the mix-back percentage. FILT and POWR are the `power` boolean --
  click either word to switch, and the active one renders purple
  (`#933BBF`), the inactive one at 30% black. Phase 5 already split these into
  two host parameters, so the knob and the words drive one each.
- **The beat-sync glyph means two different things.** On Delay it toggles beats
  vs milliseconds (the integer half of the packed `DELAY` param); on Ovrlp it is
  the `sync` boolean. BlkLen has no mode flag and so has no glyph.
- **The `?` is help only.** The Limiter panel, Smooth slider, Fine knob and
  channel selectors come out of the layout -- but **their code and parameters
  stay in place**, to be re-added later. Do not delete `LimiterComponent`, the
  smooth slider, or any `limiter*` parameter.
- **Title bar reads "DtBlkFx Revived"**, while the build stays `DtBlkFx Dev` /
  `DtB3` so it can sit beside the saved beta in one Live session. The RANDOM
  button's 180-degree rotation is deliberate.
- **Full Win95 hover and press feedback**: everything bevels inward on press,
  glyphs brighten toward `#A83FF8` on hover.

Done. See "What 6.2 landed" below.

### 6.3 — The FX row, static

Rewrite `ParameterRowComponent` as one painted component: dashed `#999DA6`
border, bevelled dropdown (128 × 26), five text cells, lock and power glyphs.
Values still change by dragging, but they render as the design's text — no JUCE
text boxes.

Includes **click-to-type value entry**. Phase 5 already wired `valueFromString`
for everything except `FX_VAL`, which accepts a bare 0..1 number only; that
limitation carries straight over and is not a bug introduced here.

### 6.4 — FX type menu

Two-column `PopupMenu` with `addSectionHeader` for NORMAL / MASK FX / STEREO
FX, per the Components frame. This is also the roadmap's "group the mask
effects so they stop looking broken" item — the design already specifies the
grouping exactly. Small and self-contained.

### 6.5 — Frequency and amp coordinate model

One Hz↔x mapping, ported from `dtblkfx_src/PixelFreqBin.cpp`, shared by the row
handles and the spectrograms. Nothing visible ships in this sub-phase.

It exists because 6.6 and 6.7 both need it and must agree: if each grows its
own mapping they will drift apart on screen and the alignment gets fixed twice.

### 6.6 — Row interactions

The amp wedge, the frequency window with its two dimming overlays
(`rgba(255,255,255,0.6)` at 50%), the draggable min (bottom-left) and max
(top-right) triangle handles, and the Mask-FX rule from the Interactions frame:
selecting a Mask effect outlines **that row and the one below it** in dashed
purple.

### 6.7 — Spectrograms

Repaint both against 6.5's mapping so their columns line up with the row
handles below. Then, optionally, restore the original's behaviour where the
spectrogram is a control surface and dragging on it sets a row's frequency
range — that is in `dtblkfx_src/Spectrogram.cpp`, not in the Figma, so it needs
a scope decision.

## Seeing the GUI without a DAW

```bash
./build/dtblkfx_paramtext --shot window.png
```

Renders the real editor offscreen to a PNG. The Standalone build must not be
launched unattended -- JUCE's wrapper opens the default audio input *and*
output and can feed back through monitors (CLAUDE.md) -- and this is the
substitute. With no arguments the same binary still runs the parameter checks.

## Open questions

- **Resizing.** The window is fixed at 652 x 912 through 6.7. The Figma is
  auto-layout, so the intent is to make it resizable afterwards -- keep layout
  code structural rather than pixel-placed so that is a tuning job, not a
  rewrite.
- **Spectrogram-as-control-surface** (6.7) is not in the Figma and needs a
  scope decision.

## What 6.2 landed

`src/DesignChrome.h/.cpp` -- the window furniture, kept out of
`DtBlkFxEditor.cpp` because 6.3 rewrites the FX row in that file and the two
diffs should not collide.

- `DraggableValue` -- the shared behaviour behind every global control: drag
  either axis, right-click for a menu of named values, double-click to type.
  This is what `DtPopupHSlider` did in the original.
- `MixBackKnob` -- **not a rotary.** The Figma export makes it a circle with a
  purple level rising from the bottom (a 32px circle mask over a rect), which is
  why there is no pointer anywhere in the design. FILT and POWR are set
  vertically either side and are the `power` boolean; clicking one *selects*
  that mode rather than toggling, so the click always does what the word says.
- `GlobalHeading` -- Delay / Ovrlp / BlkLen. The lock and beat-sync glyphs
  anchor to the right edge of the heading *text*, not of the component, which is
  how the design positions them. The readout renders the BlkLen asterisk in
  `#D20000`.
- `TitleBar` and `RandomButton`.
- The four menus are ported verbatim from `GlobalCtrl.cpp`, including the
  tempo-dependent rebuild. **Delay's first entry swaps beats/msec by
  recomputing the parameter so the delay keeps the same length** -- flipping the
  flag alone would jump the time, because the fractional part means something
  different either side of it.
- Glyphs live in `design::Glyphs` as the exact path data from the Figma export,
  fed through `juce::Drawable::parseSVGPath`. Not embedded SVG: the colour is a
  state and has to be set at paint time, and `help_button.svg` carries its Win95
  edge as an SVG inner-shadow filter that JUCE's parser does not implement.
- Window is 652 x 912, laid out by removing bands from the top rather than by
  absolute coordinates, so resizing later is a matter of changing band sizes.
- Out of the layout, code retained: the limiter panel, the smooth slider, the
  logo. The two placeholder "Factory" presets are gone.

Two crash risks were closed while writing this, both specific to plug-ins: the
host can close the editor while a `showMenuAsync` menu is still up, and a
`TextEditor` must not be deleted from inside its own focus-lost callback. Both
now go through `Component::SafePointer`.

Guardrails after: `check_audio.sh` 71/71, `dtblkfx_paramtext` all passed.

**Still wrong on screen after 6.2, by design:** the FX rows are the old
sliders-and-knobs squeezed into the design's 40px lanes, and the spectrograms
are still on the old linear frequency mapping. 6.3 and 6.7.

## What 6.1 landed

- `assets/fonts/` — the three cuts plus `Monaspace-OFL.txt` and
  `PlayerSansMono-OFL.txt`, added to `juce_add_binary_data`.
- `src/DesignPalette.h` — `design::colour::*` (every hex from the design file)
  and `design::FontStore`. The store is held via
  `juce::SharedResourcePointer`: the typefaces are ~800KB together and there is
  a `LookAndFeel` per component, so a per-instance load would be wasteful, and
  a file-scope static would outlive JUCE's leak detector.
- `src/RetroLookAndFeel.h` — rewritten against the palette. Adds a static
  `drawBevel()` (the Win95 two-step edge, `raised` for buttons and `!raised`
  for sunken fields) and overrides `getLabelFont` / `getComboBoxFont` /
  `getPopupMenuFont` / `getTextButtonFont`, which is what carries the typeface
  to every stock control without editing each call site. A `Slider`'s text box
  is a `Label`, so `getLabelFont` covers those too.
- Editor, limiter, footer and spectrogram colours now come from the palette;
  the redundant per-component `setColour` calls the LookAndFeel now handles
  were deleted.
- `tests/param_text_test.cpp` grew an embedded-font check (see above).

Guardrails after: `check_audio.sh` 71/71, `dtblkfx_paramtext` all passed.

**Still wrong on screen after 6.1, by design:** the layout is the old one, so
the header still shows the logo and four labelled sliders rather than the
design's knob and three headings, and the FX rows are still knobs and text
boxes. 6.2 and 6.3 fix those.
