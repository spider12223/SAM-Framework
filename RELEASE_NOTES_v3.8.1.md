# S.A.M Framework v3.8.1

**A custom race now has its own name on the in-game character sheet, and the Class Editor has boxes for a
class's description and an item's look.** A modder asked whether the race name shown in game is tied to the
host body. On the character sheet it was: a race on the human body read "Human Landguard" (the body, then a
skin tone). This is a small release: one engine change for the race line, two Class Editor boxes, and the
Mod Builder's new look, which went live earlier and is described below. There is no new script function and
no new JSON field: 343 functions, 87 events, as in 3.8.0.

## The character sheet shows a custom race's own name

The race line on the in-game character sheet used to print the host body's species, and on a human body a
skin tone after it. A custom race on the human body read "Human Landguard" and one on the goblin body read
"Goblin", whatever the race was called. The race's own name showed in the character-select picker, on its
info card and on the main-menu leaderboards, but not here.

- While the player is in the race's own body, the line now prints the race's `name`. It was seen in the game
  for a race on the human body ("Owl", "Wanderer"), and the log line below was seen for a third ("Ratkin").
- While the player is in the race's own body and has not ticked "disable monster abilities", hovering the
  race line shows the race's `description` in place of the stock text of the host body's species, which
  listed that species' innate abilities (read in the code, not hovered in the game). The tooltip's own
  heading is unchanged.
- If the player has ticked "disable monster abilities", the line reads "Guised" and then the name, the way
  it does for a vanilla race, but the tooltip keeps the host species' stock text. A player who is polymorphed,
  or shapeshifted without that box ticked, still sees what they are now. All of this is read in the code and
  was not tried.
- The hover texts of the Resistance and Regeneration rows on the sheet still name the host body's species,
  because the numbers in them come from that body.
- A vanilla race is not changed. A character with no custom race is not changed.
- `sam_log.txt` gets a line, `character sheet race line: "<the line's text>"`, each time the line's text
  changes for a custom race. The sheet is only drawn while the inventory is open, so the line only appears
  then.
- Engine files touched: `GameUI.cpp` (the race line) and `MainMenu.cpp` (the tooltip; the code that fills a
  custom race's card moved into one function that the character-select card and the tooltip share). Nothing
  new crosses the network.
- There is no per-race portrait picture in the game's race screens, so there is nothing for a race to
  override there; the picture on the sheet is the body's model.

## Class Editor: a Description box

A class's `description` is shown on its details card in character select (read in the code), and the class
schema has always had the field, but the Class Editor had no box for it, so it could only be written by hand.
The editor already kept and saved it. It now has a Description panel. Leaving it empty leaves the card text
blank. The Race Editor has had a description box all along. Typing in the box was seen to write
`"description"` into the class JSON in the Mod Builder; the card in the game was not looked at.

## Class Editor: a Look box for items with several looks

A starting item's `"appearance"` number picks one of the item's looks, counting from 0: a plain cloak
has 4 looks, so 0 to 3, and a number past the last one wraps around. The Class Editor only had a picker for it on
readable books, so for a cloak you wrote it into the JSON by hand. The loadout board's detail strip now has a
**Look** box, "Look 3 of 4" writing `"appearance": 2`, for the 61 vanilla item types that have more than one
look, among them five of the cloaks (4 each), most hats (2 to 4), rings (5), amulets (6) and some potions. A number written by
hand is kept as written when the class is opened and saved again.

- The box is left off the types where the number is not a look: a readable book (it has its own picker), a
  spell item, a spell tome (the number is the spell it teaches), a spellbook (its cover follows the spell's
  school and difficulty, whatever the number says), a scepter, an enchanted feather, a scroll of mail, a loot
  bag, the four tinkering bots and the rubber duck. The schema text and the getting-started guide name most
  of these.
- The table of looks is generated from the game's own `items.json` by `gui/scripts/gen-item-looks.mjs`.
- Whether a cloak's look changes the model it is worn as, or only its picture and its model on the floor,
  was not checked in the game.

## The Mod Builder looks like Barony's menus

The Mod Builder website was redesigned and went live on 31 October; this is the first release note for it.
Every editor is a framed window on a slate floor, each section opens with a crimson band like the game's
Settings menu, buttons are gold or oak plaques, values sit on black rows, and the current page and keyboard
focus are marked with Barony's gold corner brackets. The frames, buttons, icons and ornaments are drawn in
code (`gui/scripts/pixel-art.mjs`), the two fonts are free ones (Pixelify Sans and Barlow, both under the SIL
Open Font License), and no art or font was taken from the game's menus; the Barony item icons the site
already shipped are used for the nav and for pictures of items. The emoji in the editors became drawn icons.
Along the way: an API Reference line that could not wrap made the page thousands of pixels wide, five rows
did not fit a phone, and the nav now stays on screen while a long editor scrolls. `gui/DESIGN.md` records the
style so new pages match. It was checked at desktop width and at 375 pixels (no page scrolls sideways), not
on a real phone.

## Also

- The race schema's `name` and `description` text and the Race Editor's description hint say where they show.
- The scripting reference has a short section, "Races: the name on the character sheet", and the
  getting-started guide explains `appearance` on a starting item.
- The Mod Builder Dashboard's "New in" panel describes 3.8.1.

## Numbers

343 script functions, 87 events (unchanged). No new JSON field. Two engine files touched, `GameUI.cpp` and
`MainMenu.cpp`, both only for a custom race; with no race mod loaded the game reads and draws exactly as before.

## What has and has not been tested

**Played by a person, in singleplayer:** the race line on the character sheet, with the inventory open, for
two races on the human body: "Owl" (`VisionTest`) and "Wanderer" (`NeutralTest`), seen in screenshots of the
running game. Its log line was also seen for a third, "Ratkin" (`QuickWinsTest`), in a run where the
inventory was opened.

**Automated, in the real game, nobody at the keyboard:** nothing covers the new line, because the sheet is
only drawn with the inventory open and a test script cannot open it. The rest of the suite was run on the
release build: Hello 11, Speed 41, Settings 88, Loot 90, Audio 31,
Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27, `QuickWinsTest` 10 as a
Ratkin and 10 as a Skelkin (each with its 23 log checks), and `MoveTest` 28, with no failures. Three of the
tests have checks that need a person at the keyboard and nobody did them: Vision's sneaking checks, the tin
in both Neutral runs, and HelmTwin's checks where the player wears the items.

**Outside the game:** the Mod Builder's tests pass (180 for the block builder, 25 for the effect-name check
and 32 new ones for the table of looks and for a hand-written `appearance` surviving a save; a table that
offered a Look on the spell tomes fails four of them, and one that offered it on a scepter fails one), and
it builds. The Look box and the
Description box were driven in the Mod Builder dev server; the redesign was looked at on desktop and at 375
pixels.

**Not tested:**

- **The hover tooltip** on the race line, the "Guised" reading, and a polymorphed or shapeshifted player. The
  code was read; none of it was run.
- **A race on a body that is not human.** The races whose sheet line was seen are all on the human body; a
  goblin-body or skeleton-body race was not looked at with the inventory open.
- **Multiplayer.** A player's character sheet is drawn on their own machine; nothing was tried with two
  machines.
- **The Look box in the game:** whether a chosen look shows on the starting item, and on a cloak worn.
- **The Description box in the game:** the text on the class card was not looked at.
- **The redesign on a real phone, in other browsers, and the hover and item-picker states.**

## Not in this release

Asked for and not done: a small body with no innate abilities (the gremlin body brings +2 sneak vision and a
smaller sneaking slowdown, the gnome body senses gold while blind and picks up extra gold; only the sneak
vision can be cancelled today, with a negative Sneak vision), a resistance and weakness field on a race (a
script can do it with `on_damage_multiplier`), and a race portrait (the game has no screen for one).
