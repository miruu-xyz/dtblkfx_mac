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

### 6.3a — Per-row bypass ✅

A host parameter per FX set, `fxOn_0..7`, named `<n>: On`, default on.

The engine has no bypass: the only way to silence a set is to run the "Off"
effect in it. So this is a JUCE-layer parameter that masks `FX_TYPE` on the way
to the engine, in `DtBlkFxAudioProcessor::pushFxType`, which is now the single
writer of that engine value. The row's own `FX_TYPE` parameter is never
touched, which is the whole point — whatever effect is parked in a bypassed row
comes back exactly, with no GUI-side memory of it.

It replaces `ParameterRowComponent::lastActiveTypeId`, which remembered the
last non-Off effect in the editor and therefore lost it whenever the editor
closed.

- **Not randomised.** `startRandomization` already skips every id that is not
  `param_*` or one of the four unpacked globals, so this needed no change.
  Which lanes are running is a decision you made; the effects in them are what
  RANDOM is for.
- **State is plain APVTS XML**, so presets saved before this load fine and come
  back un-bypassed. No format bump.
- **"Off" stays in the FX menu.** It is a real `FX_TYPE` value that existing
  presets and automation can hold, and the engine answers `Off` for those table
  slots whatever the menu shows. Dropping it buys nothing until slots 9 and 10
  are actually filled, and then it is one line in 6.4.
- The `Off` effect has `_params_used` all zero, so **a bypassed row prints `-`
  for all four values** — which is exactly the greyed Variant3 the design draws.
  `tests/param_text_test.cpp` asserts that a bypassed row is indistinguishable
  from one running `Off`, and that `FX_TYPE` survives the round trip.

### 6.3b — The FX row, static ✅

Rewrite `ParameterRowComponent` as one painted component: dashed `#999DA6`
border, bevelled dropdown (128 × 26), four text cells and the `↔` grip, lock
and power glyphs. Values still change by dragging, but they render as the
design's text — no JUCE text boxes.

Includes **click-to-type value entry**. Phase 5 already wired `valueFromString`
for everything except `FX_VAL`, which accepts a bare 0..1 number only; that
limitation carries straight over and is not a bug introduced here.

**Settled before starting (grilled 2026-08-30):**

- **Row geometry**, from Figma `5:276`: 618 x 40. Four equal 109.75px value
  cells, a fixed 10px `↔` between the two frequency cells, the 128 x 26
  dropdown third, then a 25px lock+power group and 16px right padding. No
  column headers anywhere, so row 0's `Freq A / Freq B / Amp / Val / Fine`
  labels go. The row lock is the same 7x9 art as the header's, drawn 1:1 (the
  header renders it at 1.5x); the 6x7 `Small`/`Size3` variants are unused.
- **The four variants are Default / Hover / Selected / Variant3.** Hover is
  row-wide, on pointer enter. It raises the dropdown fill from
  `rgba(255,255,255,0.2)` to `0.5`, makes its bevel fully opaque, and takes the
  two out-of-range dimming overlays from 30% to their full 60% — so the
  frequency window reads harder under the pointer. Selected is Hover plus a 2px
  `#8A38F5` border on the dropdown, and lasts only while the menu is up.
  Variant3 is the row bypassed.
- **A bypassed row reads `---` in the dropdown**, matching the design, even
  though `FX_TYPE`'s text is formatted JUCE-side and would happily print the
  parked effect. The four values already print `-` because the engine formats
  them and sees `Off`. Showing the parked type but dashed values would give
  half the information from a display path with two sources; formatting amp and
  val outside the engine is the thing `CLAUDE.md` forbids.
- **The `ComboBox` goes.** It is a sealed stock widget: its frame can be
  restyled through the LookAndFeel but the menu it builds cannot, so the two
  columns and `NORMAL` / `MASK FX` / `STEREO FX` headers 6.4 needs are
  unreachable from it. Replaced here by a painted cell that pops a `PopupMenu`
  and writes `FX_TYPE` itself — roughly what the original's `_fxtype_ctrl` and
  its `_map_menu_to_param` table did — so 6.4 is purely the grouping.
- **The four value cells reuse `DraggableValue`**, which already is the
  original's `DtPopupHSlider`. Menus on amp and val only, ported from
  `FxCtrl::updateAmpMenu` (dB or percent, chosen by the effect's
  `ampMixMode()`) and `updateFxValMenu` (the effect's own `getValueName(i)`
  list), rebuilt when the row's effect changes. The frequencies get no menu —
  the original never populated one for them.
- **`↔` is the original's "drag both" gesture**, which `FxCtrl.cpp:413`
  bound to right-button or ctrl+left over the frequency area. It moves both
  frequencies by the same delta *in parameter space*, so the ratio between them
  holds rather than the Hz difference. Horizontal drag only, left-right cursor,
  no menu and no typed entry. It needs no coordinate model, so it ships here
  rather than in 6.6.
- **No wedge and no frequency window yet.** Both wait for 6.5's mapping, so
  between 6.3b and 6.6 the rows look flat. A static wedge drawn on the wrong
  mapping gets redrawn in 6.6 anyway, and a row that looks finished but does not
  respond is worse to QA than one that visibly is not.

**What 6.3b landed** -- `src/DesignRow.h/.cpp`, kept beside `DesignChrome` for
the same reason: the row and the chrome are separate diffs.

- `RowValue` x4, `FreqLink`, `FxTypeCell`, and `FxRow` which paints the dashed
  border and the two glyphs and owns the rest.
- **Hover is recomputed from the pointer, not from the direction of travel.**
  A pointer moving from a row onto one of its own children sends the row a
  `mouseExit`, so the children report through a `HoverRelay` and `updateHover`
  asks `getMouseXYRelative()` where the pointer actually is.
- **A bypassed row fades rather than being drawn differently** --
  `setAlpha(0.3)` on the value cells, the border dimmed to match. Nothing
  shifts as a row goes off and back on. The picker takes its *full*-strength
  fill and bevel while bypassed, which the design does too: only the resting
  row has it at half.
- **`↔` and `↕` are drawn, not typed.** Neither embedded typeface is
  guaranteed to carry U+2194 or U+2195, and a tofu box mid-row would be loud --
  the same call `RetroLookAndFeel::drawComboBox` already made. Both get a stem
  between the heads; two heads alone read as a pair of blobs at 10px.
- **The picker's bevel is a single inset step**, not
  `RetroLookAndFeel::drawBevel`'s two: the design gives it 1px of `#808080` top
  left and 1px of white bottom right, and the heavier edge reads wrong at
  128 x 26.
- The FX menu is flat and in engine order for now, with the duplicate `Off`
  suppressed -- the effect table has two adjacent no-op slots and both report
  the same name. 6.4 is the grouping.

**6.3b follow-ups.** A first review pass produced four changes:

- **The value cells take a left-right cursor.** The drag still sums both axes;
  the cursor was claiming otherwise, and in a row that reads left to right the
  vertical hint was simply misleading.
- **`refresh()` repaints only when what a cell reads has actually changed.**
  The editor polls ten times a second, and the row was repainting regardless --
  eight rows of unconditional repaints competing on the message thread with the
  cell being dragged, so the paints coalesced and a drag felt sluggish. The old
  comment at the call site already claimed this ("`updateText()` is a no-op when
  the string has not changed"), which was true of the sliders it was written
  for and stopped being true here.
- **`FxRow::paint` clips the glyphs out early.** The value cells are not opaque,
  so every repaint of a cell being dragged also runs the row's `paint` -- and
  `drawRaised` builds a `DropShadow` image per glyph, which is far too
  expensive to do on every drag event. Together with the change above this is
  what actually fixed the drag feel.
- **The white line through both arrows** was the stem winding against the heads
  in a shared `Path`, so the overlap filled as a hole. The stem is a separate
  `fillRect` now; nothing relies on winding.
- **The picker brightens on its own hover, not the row's** -- a deliberate
  departure from the Figma. The row's hover cue belongs to the frequency window
  (6.6); having the picker light up from anywhere in the row makes it look
  clickable when the pointer is nowhere near it. `FxRow::hovered` is therefore
  tracked but drives nothing until 6.6.

**Open for QA:** the `---` on a bypassed row is drawn at 30% black. The Figma's
generated markup puts no opacity on that text, but its own render reads lighter
than the `Clip` beside it, so this is a judgment call rather than a measurement.

### 6.4 — FX type menu ✅

Two-column `PopupMenu` with `addSectionHeader` for NORMAL / MASK FX / STEREO
FX, per the Components frame. This is also the roadmap's "group the mask
effects so they stop looking broken" item — the design already specifies the
grouping exactly. Small and self-contained.

**Settled before starting (grilled 2026-09-13):**

- **Engine names, not the Figma's spellings.** The design writes `---`,
  `Thresh`, `ShiftConst` and `HarmFilter`; the engine says `Off`, `Threshold`,
  `ConstShift` and `HarmFilt`. The manual and the host both use the engine's,
  so a row set to `Off` reads `Off`. A *bypassed* row still reads `---`
  (6.3b), which keeps "bypassed" and "set to Off" visibly different.
- **The two column headers sit level.** In the Figma column 2 is vertically
  centred against column 1, which puts `MASK FX` beside `Contrast`. JUCE
  top-aligns columns; one disabled empty row at the top of column 2 puts
  `MASK FX` beside `NORMAL` instead, which reads as a grid.
- **Hover is the design's purple fill under a dashed yellow focus ring.** The
  current effect is marked by accent-purple text alone -- no fill, no tick -- so
  it cannot be mistaken for the hover when the pointer is elsewhere.
- **One menu look for the whole plugin.** The spec lives in `RetroLookAndFeel`,
  so the presets and every right-click value menu get it too. The frame is 1px
  black now, replacing 6.2's thin grey.
- **JUCE's default placement**: below the picker, flipping above when there is
  no room on screen.

**What 6.4 landed:**

- `design::buildFxTypeMenu` in `DesignRow.cpp`. Free rather than a member of
  `FxTypeCell`, so the test binary can render the real thing. Masks are grouped
  by the engine's own `isMask()`; there is no stereo equivalent -- those five
  are just the tail of the table under `#ifdef STEREO` -- so they are named, and
  an effect added later falls into NORMAL until it is listed, which is the safe
  direction to be wrong in.
- `RetroLookAndFeel`: 22px rows, 126px columns, 14px Xenon items, 7px Player
  Sans headers at 40% black, a 1px border instead of V2's 2. **Section headers
  lean on a JUCE internal**: `HeaderItemComponent::getIdealSize` adds half the
  height and a quarter of the width to whatever the LookAndFeel returns, and
  headers are the only caller that passes `standardMenuItemHeight = -1`. So a
  header asks for 15 x 100 and comes back 22 x 125. Marked `ponytail:` in the
  source; if a JUCE upgrade drops the padding, headers come out short and the
  fix is a custom header component.
- `design::drawDashedRect` in the palette -- the row border, the menu's focus
  ring, and 6.6's mask outlines are all the same thing.
- The open picker's border is `#8A38F5`, the design's value, rather than the
  `accentBright` 6.3b used by mistake.

**Open for QA:** the hover's dashed yellow ring. The render shows layout, the
current-item colour and the headers, but a snapshot cannot show a hover.

### 6.5 — Frequency and amp coordinate model ✅

One Hz↔x mapping, ported from `dtblkfx_src/PixelFreqBin.cpp`, shared by the row
handles and the spectrograms. Nothing visible ships in this sub-phase.

It exists because 6.6 and 6.7 both need it and must agree: if each grows its
own mapping they will drift apart on screen and the alignment gets fixed twice.

**Settled before starting (grilled 2026-09-13):**

- **The frequency axis is the frequency parameter, linearly** -- exactly the
  original's `genPixelToHz`. The parameter is a note offset, so this is already
  log frequency: 0 Hz at the left edge, C0 (16.35 Hz) one step in, 25.8 kHz at
  the right edge whatever the sample rate. At 44.1 kHz the last ~2% of the
  width is above Nyquist and stays empty. Handles sit at their raw parameter,
  not snapped to an FFT bin, so at the low end a handle can move a few pixels
  while its bin-snapped readout holds still. The original's slider handles did
  the same.
- **The spectrogram rotates back to the original's orientation** in 6.7:
  frequency across, the newest line entering at the bottom and the display
  scrolling up. Today's is rotated 90 degrees, frequency vertical and linear in
  bins, so it can never line up with anything in the rows.
- **Both axes are inset 5px each side**, the spectrograms included, so a handle
  at either extreme is fully visible and still sits over the spectrogram's
  first or last column. The original laid its pixel table over `getWidth() - 10`.
- **The amp wedge is its own gauge on its own axis**, the amp parameter across
  the row, with the frequency window drawn over it. **0 dB sits at exactly 0.6**
  (`getAmpParam0dB`), not where the Figma eyeballed it. How the dry/wet-to-gain
  split *looks* is a 6.6 decision.

**What 6.5 landed:** `src/DesignAxis.h`, header-only.

- `span()`, `paramToX()` / `xToParam()`, `paramToHz()` / `hzToParam()` (the
  engine's own `getHz` and `HzToNoteOffs`, not a second copy of the formula),
  and `ampUnityParam()`.
- `binEdges()` / `columnBins()`, the port of `PixelFreqBin`. Columns are defined
  by their edges -- column c covers parameters `[c, c + 1) / columns`, which is
  what `paramToX` puts under screen column c -- and that is why the original's
  `half_pix` factor is not needed: it placed boundaries half a pixel either
  side of each pixel's frequency because it labelled pixels by their left edge.
- **One deliberate difference from the original:** columns wholly above Nyquist
  come back empty and draw as silence. The original clamped them to the Nyquist
  bin, which repeated that bin across the top of its display.
- `dtblkfx_paramtext` checks the round trips, the known points (param 0.5 is
  649.6 Hz, param 1 is 25.8 kHz), that the amp split reads `0.0 dB` in the
  host's own text, and that the bin edges tile the FFT with the empty columns
  exactly past Nyquist -- for FFTs of 256 and 80640 and at 96 kHz, where Nyquist
  is past the axis altogether. Putting the original's Nyquist clamp back makes
  it fail.

**Two things the numbers say about 6.7:**

- **The low end is blocky at short block lengths, and that is inherent.** On the
  630px strip, a 256-point FFT at 44.1 kHz leaves 237 columns -- well over a
  third of the width -- showing only bins 0 and 1. At 1024 points it is 118; at
  the largest FFT it is one. The original had exactly the same property, since
  a log axis over a linear FFT always does.
- **At 88.2/96 kHz the rightmost column carries everything from 25.8 kHz to
  Nyquist,** because the edges tile the whole FFT and the axis stops short of
  it. Taking the max over that range is what the original did; whether to drop
  it instead is a small 6.7 call.

### 6.6a — Defaults and reset ✅

Grilled 2026-09-13 as part of 6.6, landed separately because it is parameter
work rather than drawing.

- **Option-click resets a value to its parameter default** -- the same default
  the host's own reset uses, so the two cannot disagree. It lives in
  `DraggableValue::mouseDown`, so it covers every row value, the three headings
  and the dry/wet gauge. Subclasses handle their own click targets first, so
  locks, sync, FILT/POWR, power and the FX picker are never reset by accident.
  It does nothing on the `↔` grip. An option-double-click is two resets, not a
  reset and an editor. 6.6b's handles get it too.
- **FreqB defaults to 25.8 kHz** (param 1.0), so a fresh row is fully open, as
  the design draws it, and resetting the max handle opens the window rather than
  collapsing it. Nothing audible depends on this: a fresh row is Filter at 0 dB,
  a no-op over any range. (At 44.1 kHz it reads `22.1kHz`, the Nyquist bin.)
- **Delay defaults to 1 beat**, which is what `docs/MANUAL.md` says it is. The
  engine zeroes every parameter at startup, so it was effectively 0 before. At
  zero the delay pinned BlkLen to its minimum block, so BlkLen above ~0.3 did
  nothing on a fresh instance. **The cost: a fresh instance's output is one beat
  late** (500 ms at 120 BPM), and `setInitialDelay()` is still a no-op, so Live
  does not compensate. Accepted knowingly; latency reporting is its own job.
- **Init is every parameter's default**, the limiter excepted, as with RANDOM.
  It used to be a hand-copied list that had already drifted from the defaults in
  three places: Delay, FreqB, and amp (-inf dB rather than 0 dB). That includes
  switching every row back on, since bypass defaults to on.

`check_audio.sh` is blind to all of this -- the harness drives the engine
directly with pinned values and never sees a JUCE default. `dtblkfx_paramtext`
checks the two new defaults.

### 6.6b — Row interactions ✅

The amp wedge, the frequency window with its two dimming overlays, the
draggable min and max triangle handles, and the Mask-FX rule from the
Interactions frame (`6-662`).

**Settled before starting (grilled 2026-09-13):**

- **The wedge is the design as drawn, at the engine's true 0 dB.** The Figma's
  shape (`Vector 3`) is a ramp from nothing at the left edge to full height at
  x = 370.1 of 618 -- 0.599, which *is* the engine's 0 dB point -- then full
  height to the right edge. Only the purple fill was eyeballed: a 420px
  rectangle, i.e. +8 dB, under a row reading `0.0dB`. So the kink already sits
  on the split: below it the ramp is wet % for the ten mix-mode effects and
  attenuation for the rest, above it is gain. Filled `#CDB9DC` from the left
  edge to the current amp, nothing drawn past it. The wedge spans the whole row,
  as in the Figma; the frequency axis is the one inset 5px each side.
- **The handles cross freely.** A is always the bottom triangle, B the top. With
  A above B the engine processes the *outside* of the range
  (`SplitMaskProcess`), so the dimming flips to the band between them.
- **What an effect does not use is not drawn.** An unused frequency hides its
  handle; the window and its dimming need both frequencies, so HarmMask (whose
  one frequency is a fundamental, not a range edge) shows a lone A handle; an
  unused amp hides the wedge. All decided by the *parked* effect.
- **Only the triangles are grabbable**, padded to about 15 x 12 and hit-tested
  above the cells. Horizontal and absolute, keeping the offset they were grabbed
  at so they never jump. Left-right cursor; accent purple on hover and while
  dragged; the row stays in its hover state for the whole drag. Double-click
  opens the same inline editor as the handle's readout; option-click resets
  (6.6a); right-click does nothing. The wedge and the window are display-only.
- **The mask outline is solid `#8A38F5`** around the mask row and the row below
  -- not dashed, as this file used to say. **Solid when live, 30% when the
  pairing does nothing**: the mask bypassed, the row below bypassed, `Off`, or
  on an effect that ignores masks, or no row below at all (a mask on row 8
  outlines itself alone). That is the roadmap's "masks look broken" item: they
  were silently doing nothing, and now they show it.
- **A bypassed row** keeps its wedge in grey (`#C1C1C1` at 30%) and its window
  and handles at 20%, as Variant3 draws it -- and the handles stay draggable,
  as the cells stay editable, so a lane can be set up before it is switched on.

**What 6.6b landed:**

- `FxRow` paints, back to front: wedge, dimming, dashed border, glyphs. A
  `HandleLayer` over the whole row paints the handles and claims the pointer
  only over them -- its `hitTest` is false everywhere else, so clicks fall
  through to the cells beneath without the cells knowing about it.
- **`FxRow::refresh()` is the one way in.** Every change made inside the row
  calls it at once -- cell drags through a new `DraggableValue::valueChanged()`
  hook, the `↔` grip, the handles, the picker, power -- and the editor's 10Hz
  poll calls it for automation. It compares against a cached `Shown` and
  repaints only the strip each value moved across: a full-row repaint would also
  rebuild the two glyphs' drop shadows on every drag event, which was most of
  6.3b's sluggish drag. Crossing the handles, bypass and a change of effect
  repaint the whole row, because they change *what* is drawn, not where.
- **`paint` draws from that cache, not from the parameters**, so a partial
  repaint can never show half of an old window and half of a new one.
- `design::consumesMask` names the effects that *ignore* a mask -- `Off`, the
  masks, Vocode, HarmMatchLR/RL -- since the engine has no flag. Taken from a
  brace-matched read of every effect class's body. (Correcting the grilling:
  the sweeps do consume masks. Triangles to Sweep are all `HarmMatchFx`, which
  runs `MaskedRun`.)
- The editor paints the outlines in `paintOverChildren`, recomputing them on
  the 10Hz poll and repainting the rows only when one changes.
- Long effect names (`AutoHarmMask`, `HarmRepitch`, `HarmMatchLR`) squash to
  fit the picker, down to 75%, instead of being clipped. A 6.3b bug the demo
  scene exposed.
- `dtblkfx_paramtext --demo-shot <png>` renders every row state at once, and
  the same scene backs a check of the mask-outline states. Marking Vocode as a
  consumer makes it fail.

**Open for QA** -- none of this can be seen in a still: the row-hover dimming
going from 30% to 60%, handle hover and drag feel, the grab offset, the row
staying lit through a drag, option-click and double-click on a handle.

### 6.7 — Spectrograms

Repaint both against 6.5's mapping so their columns line up with the row
handles below. Then, optionally, restore the original's behaviour where the
spectrogram is a control surface and dragging on it sets a row's frequency
range — that is in `dtblkfx_src/Spectrogram.cpp`, not in the Figma, so it needs
a scope decision.

### 6.8 — Persistent locks

The eleven locks — three global, eight per-row — are ephemeral: bare
`ToggleButton`s with no parameter, read only by `startRandomization`. They are
lost when the editor closes. Making them persist is a state-format change
affecting all eleven at once, so it is its own small phase rather than a rider
on the row rewrite.

## Seeing the GUI without a DAW

```bash
./build/dtblkfx_paramtext --shot window.png
```

Renders the real editor offscreen to a PNG. `--demo-shot <file.png>` does the
same with the rows set up to show every state at once (6.6b). `--menu-shot <file.png>` does the
same for the FX-type menu, which `--shot` cannot see because a `PopupMenu` is a
separate desktop window; it flashes on screen for a moment. The Standalone build must not be
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

### 6.2 follow-ups

A first review pass produced eight changes, all in `DesignChrome`:

- **Headings and glyphs are drawn as paths, not `drawText`.** `drawText` clips
  to its box, which was cropping the readouts, and cannot outline or shadow.
  `textAsPath` + `drawRaised` give the design's treatment: purple glow, white
  edge, grey fill. The outline width is a parameter because 1.2px suits a 28px
  heading and closes up a 9px padlock.
- **Headings are set in a real italic cut**, `MonaspaceXenonWideLightItalic`.
  `Font::italicised` cannot synthesise a slant for an embedded typeface -- it
  comes back upright without complaint.
- **The gauge reads dry/wet**, inverting MixBack. See the note in `CLAUDE.md`;
  the drag direction, the menu labels and typed entry all invert with it.
- **The gauge is centred between FILT and POWR**, which is not the centre of its
  component.
- **Click targets inside drag targets get a pointing-hand cursor**: FILT, POWR,
  the locks and the sync glyphs. Their hit areas are padded by 3px, both so a
  9px glyph is clickable and so the hover highlight has room.
- **A hovered lock or sync glyph brightens the ground behind it.** At 9px the
  glyph alone is too small for a colour change to register.
- **Overlap appends " sync"** when the flag is on. Turning it on otherwise
  changes nothing visible, since the percentage stays put.
- **Hovering any lock outlines RANDOM in purple and lifts its face**, via
  `design::LockHoverState`. Sources register by address rather than setting a
  shared bool, because several locks hover and unhover independently and the
  last to report must not clear the others. It is owned by the editor, never a
  global -- a host can have several windows open on different instances. The
  eight FX-row locks report through a mouse listener for now; 6.3's glyphs will
  report directly.

A second pass:

- **The window ground is `#E8EAEF`, not `#C0C0C0`.** Sampled from the Figma
  render, confirmed against the design. There are two greys now: `windowBg` for
  the ground, and `baseGrey` for control faces -- buttons, the help box, the
  dropdowns -- which stay Win95 grey, as `help_button.svg` filling its own rect
  with `#C0C0C0` confirms.
- The gauge uses the same cursor as the headings, and has lost its white ring.
- **A closed padlock is `textDim`**, the same 60% black as the heading beside it.
- The lock and sync glyphs moved down (`y` 5 and 22) so `glyphHitArea`'s 3px
  padding -- and therefore the hover highlight -- stays inside the component.
  It was being clipped at the top.
- **The gap under the title bar is 10px, not the design's 26.** At this size 26
  reads as a gap rather than as padding. The header band is 46 rather than the
  design's 40, which buys the air between heading and readout and the room the
  glyph highlights need.
- **The footer is anchored to the bottom** rather than flowed to it, so leftover
  rounding collects above it instead of below.
- **The two beat-sync states now match.** The off glyph ships as one path with
  the cross included, which pushes its bounding box to 10x10 against the on
  state's 9x9 -- scaling each to fit its own ink therefore drew the off arrows
  smaller and offset, and they jumped on every click. The cross is split into
  `beatSyncCross`, and `Glyphs::scaledFrom` places all three parts from a shared
  10x10 grid instead of from their own bounds.
- **The inline editor can be dismissed again.** JUCE only fires `focusLost`
  when focus actually moves somewhere, and it finds that somewhere by walking up
  from whatever was clicked -- so with every painted component declining focus,
  the editor kept it and went on swallowing clicks and keys. `DraggableValue`
  and the top-level editor now both `setWantsKeyboardFocus(true)`, which gives
  clicks somewhere to land; `mouseDown` also returns early while an editor is
  open, so dismissing does not simultaneously start a drag underneath.
- **Right-click menus use the plugin's LookAndFeel.** `PopupMenu` resolves its
  LookAndFeel from `menu.lookAndFeel`, falling back to the *menu window's* own
  -- which is a desktop-level component, so it got stock JUCE styling while the
  preset dropdown beside it did not. `menu.setLookAndFeel(&getLookAndFeel())`
  fixes it, and it is safe across the editor closing: the member is a
  `WeakReference`, and `windowIsStillValid()` dismisses a menu whose target
  component has gone. `drawPopupMenuBackground` gives it the design's flat white
  box with a thin grey edge instead of V4's rounded, shadowed one.
- **Inactive lock and inactive sync are both 60% black**, the same as the
  heading beside them. The open and closed padlock shapes carry the lock state,
  not the weight.
- **The glyph outline is 0.45px.** The white outline is a
  fixed width, so it takes a far larger share of a 9px glyph's stroke than of a
  28px letterform; at 0.7px it washed the glyphs out toward the background
  whatever colour was underneath them.

`tools/pngpick.py` reads pixel colours out of a PNG, which is how the ground
colour was taken off the Figma render. Export a frame, sample the same point in
both it and a `--shot`, compare. It decodes the PNG itself because there is no
PIL here and adding one for six lines of colour-picking is not worth it.

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
