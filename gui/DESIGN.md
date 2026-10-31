---
name: S.A.M Mod Builder
description: Barony's in-game menus, redrawn, as the place you build a Barony mod.
colors:
  floor: "#141925"
  window: "#1c2333"
  row: "#050608"
  edge: "#3c4454"
  iron: "#2f343e"
  outline: "#07080b"
  oak: "#4f301b"
  oak-lit: "#7d5434"
  oak-shade: "#2b180c"
  crimson: "#86202a"
  crimson-lit: "#c2463f"
  crimson-deep: "#45101a"
  plaque-gold: "#d9a23f"
  plaque-gold-lit: "#f7d677"
  plaque-gold-shade: "#8c5a15"
  gold: "#e2b04e"
  gold-dim: "#b88a3a"
  gold-ink: "#2b1907"
  bone: "#f2ead7"
  ink: "#ded7c6"
  ink-dim: "#b6b0a2"
  ink-faint: "#a49f93"
  label: "#dcc896"
  ruby: "#d4483d"
  ruby-text: "#f08378"
  sapphire: "#4f93d8"
  emerald: "#3f9a55"
  emerald-text: "#8fd49c"
  rail-teal: "#33454b"
  rail-teal-fill: "#5f8580"
  rail-teal-lit: "#8fb3ad"
  stone: "#6c707a"
  stone-lit: "#8a8e97"
  stone-shade: "#43464e"
typography:
  title:
    fontFamily: "Pixelify Sans, Barlow, system-ui, sans-serif"
    fontSize: "30px"
    fontWeight: 700
    lineHeight: 1.15
  band:
    fontFamily: "Pixelify Sans, Barlow, system-ui, sans-serif"
    fontSize: "17px"
    fontWeight: 500
    lineHeight: 1.2
  button:
    fontFamily: "Pixelify Sans, Barlow, system-ui, sans-serif"
    fontSize: "17px"
    fontWeight: 500
    lineHeight: 1.1
  nav:
    fontFamily: "Pixelify Sans, Barlow, system-ui, sans-serif"
    fontSize: "17px"
    fontWeight: 400
    lineHeight: 1.15
  label:
    fontFamily: "Pixelify Sans, Barlow, system-ui, sans-serif"
    fontSize: "15px"
    fontWeight: 400
    lineHeight: 1.3
  nav-group:
    fontFamily: "Pixelify Sans, Barlow, system-ui, sans-serif"
    fontSize: "13px"
    fontWeight: 400
  body:
    fontFamily: "Barlow, system-ui, sans-serif"
    fontSize: "16px"
    fontWeight: 400
    lineHeight: 1.55
  input:
    fontFamily: "Barlow, system-ui, sans-serif"
    fontSize: "15.5px"
    fontWeight: 400
    lineHeight: 1.4
  small:
    fontFamily: "Barlow, system-ui, sans-serif"
    fontSize: "14px"
    lineHeight: 1.5
  hint:
    fontFamily: "Barlow, system-ui, sans-serif"
    fontSize: "14px"
    lineHeight: 1.45
  title-compact:
    fontFamily: "Pixelify Sans, Barlow, system-ui, sans-serif"
    fontSize: "22px"
    fontWeight: 700
  micro:
    fontFamily: "Pixelify Sans, Barlow, system-ui, sans-serif"
    fontSize: "12px"
  tag:
    fontFamily: "Pixelify Sans, Barlow, system-ui, sans-serif"
    fontSize: "11px"
  code:
    fontFamily: "Consolas, Cascadia Mono, Courier New, monospace"
    fontSize: "0.85rem"
rounded:
  none: "0px"
spacing:
  pixel: "2px"
  plaque-edge: "6px"
  frame-edge: "10px"
  row-gap: "16px"
components:
  button-primary:
    backgroundColor: "{colors.plaque-gold}"
    textColor: "{colors.gold-ink}"
    typography: "{typography.button}"
    rounded: "{rounded.none}"
    padding: "2px 14px"
    height: "40px"
  button-secondary:
    backgroundColor: "{colors.oak}"
    textColor: "{colors.bone}"
    typography: "{typography.button}"
    rounded: "{rounded.none}"
    padding: "2px 14px"
    height: "40px"
  button-danger:
    backgroundColor: "{colors.crimson}"
    textColor: "{colors.bone}"
    typography: "{typography.button}"
    rounded: "{rounded.none}"
    padding: "2px 14px"
    height: "40px"
  button-positive:
    backgroundColor: "{colors.emerald}"
    textColor: "{colors.bone}"
    typography: "{typography.button}"
    rounded: "{rounded.none}"
    padding: "2px 14px"
    height: "40px"
  input:
    backgroundColor: "{colors.row}"
    textColor: "{colors.ink}"
    typography: "{typography.input}"
    rounded: "{rounded.none}"
    padding: "3px 6px"
  panel:
    backgroundColor: "{colors.window}"
    textColor: "{colors.ink}"
    rounded: "{rounded.none}"
  panel-band:
    backgroundColor: "{colors.crimson}"
    textColor: "{colors.bone}"
    typography: "{typography.band}"
    padding: "4px 44px"
    height: "36px"
  nav-item:
    textColor: "{colors.bone}"
    typography: "{typography.nav}"
    padding: "0 8px 0 2px"
    height: "48px"
  nav-item-hover:
    backgroundColor: "{colors.oak}"
    textColor: "{colors.bone}"
  nav-item-active:
    backgroundColor: "{colors.crimson}"
    textColor: "{colors.bone}"
---

# Design System: S.A.M Mod Builder

## Overview

**Creative North Star: "The Options Window"**

The Mod Builder is Barony's own Settings and Play windows, redrawn so that building a mod feels like part
of the game rather than a website about it. Every editor is a framed window on a speckled slate floor. Each
section is opened by the crimson option band from Barony's Settings menu, and every value sits on a black
option row. The art is pixel work drawn in `scripts/pixel-art.mjs` (oak-and-iron frames with gold rivets,
plaques, brackets, gems, the fleur-de-lis) and scaled by whole numbers, 2px to a pixel, so no edge ever
blurs. Nothing is taken from the game except the item icons the site already ships for its pickers.

It is a working tool used for an hour at a time, so the world contributes type, palette, density and one
signature, the crimson band, and never the layout or the controls: a rail of destinations on the left, the
work on the right, ordinary buttons, inputs, selects and checkboxes underneath the pixel art.

**Key Characteristics:**
- A crimson option band with a fleur-de-lis either side opens every section.
- Gold plaques for the main action, oak plaques for secondary actions and tabs, crimson for removing.
- Black option rows for every value, with a 2px slate edge that turns gold on focus.
- Barony's gold corner brackets mark the current page and whatever has keyboard focus.
- Two voices: pixel type for the menu, Barlow for anything read or typed.
- Real Barony item icons (36px, 1:1) beside each destination.

## Colors

A full palette with named roles, taken from the materials of a Barony menu: slate, oak, iron, crimson
cloth, gold, and the three gems.

### Primary
- **Crimson Band** (`#86202a`, lit `#c2463f`, deep `#45101a`): the option band on every section, the
  chosen nav row, the chosen tab, remove buttons, text selection.
- **Plaque Gold** (`#d9a23f`, lit `#f7d677`, shade `#8c5a15`): the primary plaque. Lettering on it is
  **Gold Ink** (`#2b1907`).

### Secondary
- **Oak** (`#4f301b`, lit `#7d5434`, shade `#2b180c`): the frame wood, secondary plaques, steppers, the
  hovered nav row.
- **Gold** (`#e2b04e`, dim `#b88a3a`): emphasis in prose, links, nav group names.

### Tertiary
- **The gems**: Ruby (`#d4483d`, text `#f08378`) is an error, Sapphire (`#4f93d8`) is advice, Emerald
  (`#3f9a55`, text `#8fd49c`) is done or on. They appear on the title bar and lead state lines.

### Neutral
- **Floor** (`#141925`): the page, speckled with `#191e2c`, `#1c2232` and `#26304a`.
- **Window** (`#1c2333`): inside every frame, speckled lighter than the floor so windows stand off it.
- **Row** (`#050608`): option rows, inputs, wells. Edge **Slate** (`#3c4454`).
- **Rail Teal** (`#33454b`, filled `#5f8580`, lit `#8fb3ad`): the slider rail, after Barony's Settings
  sliders. **Stone** (`#6c707a`, lit `#8a8e97`, shade `#43464e`): the scrollbar's stone.
- **Bone** (`#f2ead7`) pixel chrome; **Ink** (`#ded7c6`) body; **Ink Dim** (`#b6b0a2`) secondary;
  **Ink Faint** (`#a49f93`) hints (6:1 on a window); **Label** (`#dcc896`) field labels.

### Named Rules
**The Three Gems Rule.** State is said with Barony's gems: ruby for an error, sapphire for advice, emerald
for done. A state line leads with its gem sprite, never a warning glyph.

**The Black Row Rule.** A value the user can read or change sits on a black row. Windows are slate, the
floor is darker slate, and only rows are black, so a field always reads as a field.

## Typography

**Chrome Font:** Pixelify Sans (SIL OFL), standing in for Barony's own bitmap face.
**Body Font:** Barlow (SIL OFL), a sturdy, slightly squared sans that sits beside pixel type.
**Code Font:** Consolas stack, for ids, paths and generated JSON or Lua only.

**Character:** the pixel face carries the menu the way the game's does; Barlow carries everything a person
reads closely or types, because a paragraph in a pixel face is tiring over an hour of editing.

### Hierarchy
- **Title** (700, 30px): the name in the title plaque, bone with a 2px black drop.
- **Band** (500, 17px): section names on the crimson band.
- **Button / Nav** (500 / 400, 17px): plaque lettering and destinations.
- **Label** (400, 15px): field labels, in Label gold.
- **Nav group** (400, 13px): the group names in the rail.
- **Micro** (400, 12px) slot labels, counts and badges; **Tag** (11px) the host tag; **Title compact**
  (700, 22px) the title on a phone.
- **Body** (Barlow 400, 16px, 1.55): prose. **Input** (15.5px, tabular figures). **Small** 14px,
  **Hint** 14px in Ink Faint.

### Named Rules
**The Two Voices Rule.** Pixel type for titles, bands, buttons, the nav and labels; Barlow for values,
hints and explanations. A sentence longer than a label is never set in the pixel face.

## Layout

A title bar across the top (a framed bar with the name plaque, three gems, and a lit gold rail to each edge), a sticky
236px nav rail on the left, and the pages to its right, left-aligned beside the rail with their own maximum
widths (64rem, 80rem), and every paragraph stops near 75 characters however wide the column is. Sections stack 16px apart. Below 760px the rail
becomes a strip that scrolls sideways under the title bar, the rails and knots drop, and nothing on any page
scrolls the page sideways (wide code blocks scroll inside themselves).

## Elevation & Depth

Barony's menus have no soft shadows: depth is drawn. A frame's oak band is lit on its outer top-left and
inner bottom-right; a plaque is lit along its top and shaded along its bottom, and a press swaps the two and
drops the label a pixel. Pixel lettering on dark ground carries a 1px or 2px black drop beneath it, as the
game's does. There are no box shadows anywhere.

### Named Rules
**The Drawn Light Rule.** Light comes from the art, not from CSS: bevels are pixels in the maps, presses
swap to the pressed map. No blur shadows, no gradients pretending to be material.

## Shapes

Square, always. Corners are notched by one art pixel in the maps (frames, plaques, rows), never rounded with
`border-radius`. Everything is drawn on a 2px grid.

## Components

### Buttons
- **Shape:** a plaque drawn with `border-image` (6px edge, notched corners), 40px tall, 14px side padding.
- **Primary:** Plaque Gold with Gold Ink lettering (`.sam-btn`).
- **Secondary / tabs:** Oak with bone lettering (`.sam-btn-oak`); the chosen tab is Crimson (`.sam-btn-red`).
- **Positive:** Emerald (`.sam-btn-green`), used for an "on" toggle and for export.
- **Hover:** brightness up 10%. **Press:** the pressed map and the label 2px lower. **Focus:** gold corner
  brackets. **Disabled:** desaturated and dimmed.
- **Steppers and row tools** (`.sam-step`): 30px oak squares; remove is crimson with a drawn X; edit, copy
  and drag are drawn sprites, not glyphs.

### Inputs / Fields
- **Style:** a black row (`.sam-input`, `.sam-valuebox`) with a 6px drawn edge; Barlow, tabular figures.
- **Hover:** the edge lightens. **Focus:** the edge turns gold. **Invalid:** `aria-invalid` turns it crimson.
- **Select:** the same row with a drawn bone chevron; the open list is black with crimson for the chosen row.
- **Checkbox** (`.sam-check`): a 20px black square that fills gold with a dark drawn check.
- **Slider** (`.sam-slider`): Barony's grey-teal rail, lighter where filled, with a wooden block thumb.

### Navigation
A rail of rows, each a 36px Barony item icon and a pixel label. Hover lays an oak plaque under the row; the
current page is a crimson plaque inside gold corner brackets. Group names sit above in dim gold. The rail is
sticky so the way out of a long editor stays on screen.

### Icons
- **Drawn icons** (`<PixelIcon name>`, 20px): the six attributes (str dex con int per chr), the sixteen
  skills (sword axe mace polearm bow shield stealth key lens coin crown orb sparkle amulet flask) and the
  marks hand and mana. Each is a 10x10 fill map in `scripts/pixel-art.mjs` (`ICONS`); the generator rings
  it in black, as Barony outlines its own icons.
- **Item pictures** are real Barony item icons at 36px or 72px, never scaled by anything else. An item with
  no picture of its own shows the icon of an item of its kind or category (`src/data/fallbackIcons.js`); an
  empty paperdoll slot shows its kind's icon faintly. The nav and the Mod Builder lists use the same icon
  per editor (`src/data/navIcons.js`).
- **State** is a gem (`<Gem kind>`), never a glyph; a toggle says its state in words or with an emerald gem.

### The Option Band (signature)
Every `Panel` opens with a 36px crimson band: a black line, a lit row, crimson, a deep row, a black line,
with three drawn crosses at each end and a fleur-de-lis either side of the name. Small sticky bands of the
same cloth head the categories in the item picker.

### Windows and wells
`.sam-panel` is a 10px oak-and-iron frame with gold rivets around speckled slate. `.sam-well` is a black row
used as a container for a list or a status value. Inside a window, separate sub-sections with a heading and
`.sam-divider`, not with another bordered box.

## Do's and Don'ts

### Do:
- **Do** open every section with the crimson option band (`Panel`).
- **Do** put values on black rows and let the edge carry hover, focus and error.
- **Do** draw new ornaments, icons and frames as maps in `scripts/pixel-art.mjs`, then run it; show an
  icon with `<PixelIcon>` and an item with its real item icon.
- **Do** lead a state line with its gem: ruby, sapphire or emerald.
- **Do** use the gold corner brackets for the chosen item and for keyboard focus.
- **Do** keep sentences in Barlow and keep pixel type to menu chrome.

### Don't:
- **Don't** copy menu art or fonts from the game; the item icons in `public/item-icons/` are the only
  game art the site ships.
- **Don't** round corners or add box shadows; depth and corners are in the pixel maps.
- **Don't** nest a bordered box inside a window to make a sub-section.
- **Don't** use emoji or font glyphs as icons on buttons or row tools.
- **Don't** scale pixel art by anything but whole numbers.
