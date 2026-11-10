# S.A.M Framework v3.9.1

**An extra limb can swing side to side, and a saved game remembers which race a character is.** A modder's
catfolk tail swayed "on a weird plane": the only sway there was copied the salamander's tail, which tilts the
limb up and down. This release adds a sideways swing, makes a save keep the race it was made with, and collects
a round of fixes. There is no new script function: 343 functions, 87 events, as in 3.9.0.

## A tail that swings side to side

```json
"extra_limbs": [
  { "model": "models/catfolk/tail.vox", "attach": "body", "offset": { "forward": -1.5 }, "sway": "side" }
]
```

- `"sway": "side"` on an extra limb swings it left and right, turning it about the spot it is placed at (where
  it attaches, moved by its `offset`). It uses the same rhythm as before: faster while the player moves, slowly while they stand still.
- `"sway": true` is unchanged: the salamander tail's motion, up and down with a slight twist, which only reads
  as a swish on a model built like that tail. `"sway": "tilt"` is another way to write it.
- Any other value is reported in `sam_log.txt` and that limb does not sway. An exe older than 3.9.1 ignores
  `"side"` and the limb does not sway.
- `sam_get_limb(uid, "extra_1")`, up to `"extra_13"`, now reads a player's race's extra limbs, in the order
  the race lists them. It answers nil on a creature, for a slot the race does not fill, and while the player
  is not in the race's own body.

## Saved games remember which race a character is

A save stores a custom race as a number, and S.A.M hands the numbers out by sorting the loaded mods by
namespace, so adding or removing a race mod can move them. A save made as one race could then come back as
another: in testing for 3.9.0, a save made as a race on the human body showed a skeleton head on its Continue
card once another mod's skeleton-body race held its number, and loading it would have made that character the
other race.

- A save made with race mods loaded now also records which race each number meant (`sam_raceids`, the same way
  custom items have been recorded since 2.3.1). When the save is read, for its Continue card and when it is loaded, each player's
  race number is pointed back at the race with that id.
- A race whose mod is not loaded comes back as a human, with a line in `sam_log.txt` that names the race. It
  stays a human if the game is saved again without that mod; load the mod first to keep the race.
- This only happens while a set of mods is loaded: with no mods loaded the save is left as it is. With mods
  loaded, a race that none of them provides comes back as a human, as above.
- A save written by 3.9.0 or earlier has no such record and is still read by number, so for those, load the
  same set of mods you saved with.
- The character the lobby pre-selects for a new game still remembers its race by number.

## The character sheet's race tooltip with the race's mod missing

Since 3.8.1 the race line's tooltip shows a custom race's own description. For a saved custom race on the human
body loaded without its mod, it showed an empty card, because the check for "in the race's own body" took the
human body for that race. It now shows the human text, as it did before 3.8.1.

## Mod Builder fixes (live since 7 November)

The Mod Builder is the website, so these went live when they were made:

- The Monster Editor crashed the whole site on an item whose type is a list (a random pick, which the schema
  allows). Such an item is now edited as "a, b". Saving a monster also kept dropping an item's `appearance` and
  `identified` and the body's `cast` and `death`; it keeps them now.
- The Race Editor dropped a limb's scale and offset on the first change. The Effect Editor reopened an effect
  saved without a duration as 500. The Class Editor misread old-style star ratings.
- Uploading a picture and then clearing its path box left a note promising a file that was never exported, in
  the Race, Spell and Class editors. The Class Editor's selected portrait, whose hint asked for a second PNG,
  now has an upload button.
- Importing a mod and exporting it again lost its `images`, `rooms` and `plugins` lists in mod.json.
- Custom blocks were left out of "Use this script" right after the page loaded. Search boxes picked the first
  option containing what you typed even when you had typed an option exactly, and could not take a typed value
  that is part of an option's name. Duplicating a class or race now copies its script. A block pack with a null
  entry no longer crashes the import, and autosave keeps as many pictures and sounds as fit instead of dropping
  them all when the browser's storage is full.

## Also

- Code nothing used was removed from the framework (eight functions) and the Mod Builder.
- Release notes now live in `release-notes/`, the old versioned installer specs are gone, and the README's
  repository layout lists what is there.
- The scripting reference has a new section, "Races: a tail that swings side to side", and the paragraph on
  saved race numbers under "Races: a picture for saved games and the HUD" is rewritten for 3.9.1.

## Numbers

343 script functions, 87 events (unchanged). No new JSON field: an extra limb's `sway` takes `"side"` and
`"tilt"` as well as true or false, and `sam_get_limb` takes `extra_1` to `extra_13`. Engine files touched:
`actplayer.cpp` (the sideways swing), `scores.cpp` (pointing a saved race back at its id) and `MainMenu.cpp`
(the tooltip). Nothing new crosses the network: the race record lives in the save file, and every machine
works out a limb's swing for itself.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:** a new test, `SwayTest`, hangs five salamander tails on
a race (side, true, none, tilt and a misspelled value) and measures each one with `sam_get_limb` for three
seconds while the player stands still: "side" turns 0.286 radians and does not tilt or twist, true and "tilt"
tilt 0.286 and twist 0.143 and do not turn, the other two stay still, and the extra_N reads answer nil where
they should. 13 passed; on the 3.9.0 build 9 of those 13 fail. The rest of the suite was run on the release build: Hello 11,
Speed 41, Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60,
Addons 27, `QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with its 28 log checks) and `MoveTest` 28,
with no failures. Three of these tests have checks that need a person at the keyboard and nobody did them:
Vision's sneaking checks, the tin in both Neutral runs, and HelmTwin's checks where the player wears the items.

**Seen in the files:** a test play as a custom race writes `sam_raceids` (`200=raceiconplay:iconplay`) into its
save.

**Outside the game:** the Mod Builder's tests pass (180 for the block builder, 25 for the effect-name check, 32
for the table of looks, and 11, 8, 11 and 6 new ones for the picture paths, the mod.json round trip, the search
boxes and block packs; on the code before these fixes, 5, 2, 2 and 4 of those new checks fail) and it builds.
The Mod Builder fixes were reproduced first, in the dev server or by a failing test, and checked again after
the fix, except two: duplicating a class's script and the autosave fallback were only checked after the change
(the second needs the browser's storage to fill up, which was not tried).

**Not tested:**

- **Watching a tail swing side to side.** The swing was measured, not looked at.
- **A save read under a different set of mods.** Pointing the race back at its id, on the Continue card and
  when the save loads, was read in the code; it was not tried in the game. Neither was the human fallback.
- **The race tooltip with the race's mod missing.** Read in the code.
- **Multiplayer.** Nothing was tried with two machines.
