# S.A.M Framework v3.9.0

**A custom race can now have its own small picture on the saved-game cards.** A modder asked whether a race can
have a small pixel portrait on the saved games and on the HUD. The game picks that picture from one table of
head pictures keyed by the host body, so a race on the human body showed a human head. A race can now name a
PNG of its own with the new `icon` field. This is a small release: one new race field, one Race Editor box, and
two engine files touched, only for a race that sets it. There is no new script function: 343 functions, 87
events, as in 3.8.1.

## A race's own picture

```json
{ "id": "frostborn:frostborn", "name": "Frostborn", "host_body": "human", "icon": "races/frostborn.png" }
```

`icon` is a PNG of about 32 by 32 pixels, written from the folder that holds `mod.json`, with no leading slash.

- **Saved-game cards.** On the Continue screen the card of a saved game of that race draws your picture where
  the host body's head was. Seen in the game (see the tests below).
- **Status bars in a networked co-op game.** The status bars the game draws for the other players use the same
  picture, whatever form that player is in (the code checks only the race). This is set in the code for the
  same field and was not run. The game draws these bars only in a networked game, not in local splitscreen.
- **A path that does not work is reported.** The file is looked for once, when the mods load. A file that is not
  found is named in `sam_log.txt` with the path that was looked for, and so is a path that climbs out of the mod
  folder; in both cases the host body's picture is kept. A found picture logs `Race [id] custom icon -> <path>`.
- **A saved game remembers a race by its number, not its name.** S.A.M hands out those numbers by sorting the
  loaded mods by namespace (a mod's dependencies first), so they depend on which mods are loaded, not on how
  the list is arranged: 200 for the first race, 201 for the next. The card shows the picture of whichever race
  holds that number when the Continue screen opens, or that race's body head if it has no `icon`. With
  different mods loaded than when the game was saved, that is another race's picture or head. This was seen: a
  save made as a race on the human body showed a skeleton head while another mod's race on the skeleton body
  held its number. Load the same set of mods you saved with.
- **Where it does not apply.** The lobby cards, the main-menu leaderboards and the character sheet have no small
  picture to replace and are not changed. A recruited follower is a monster, not a race, so it keeps its
  species' picture. A race without an `icon`, and a game with no race mod, draw exactly as before.
- **Race Editor.** An **Icon path** box and an **Upload PNG** button. By the code, a picture over 256 KB is
  refused and an uploaded PNG is put in the exported zip at the icon path; neither was tried (see below).
- **Engine files touched:** `MainMenu.cpp` (the saved-game card) and `GameUI.cpp` (the status bars). Nothing new
  crosses the network: each machine reads the picture from its own copy of the mod.

## Also

- The race schema has the `icon` field, with the published copy in step, and the schema reference is regenerated.
- The scripting reference has a section, "Races: a picture for saved games and the HUD".
- The Mod Builder Dashboard's "New in" panel describes 3.9.0.

## Numbers

343 script functions, 87 events (unchanged). One new JSON field, `icon` on a race. Two engine files touched,
`MainMenu.cpp` and `GameUI.cpp`, both only for a custom race that sets `icon`.

## What has and has not been tested

**Seen in the running game:** the Continue screen card of a saved game of a race with an icon draws the
picture. The test mod was a race on the human body whose icon is a magenta square with a yellow border and a white X,
loaded together with nine Workshop mods. The lobby's race list did not take the test's mouse clicks, so the
saved race number in a test save was changed by hand from 200 to 203 (the race's number in that session) and the
original file was put back afterwards; a save made by picking the race in the lobby was not looked at. The same
save, while another mod's skeleton-body race held number 200, drew a skeleton head, which is how the note about
race numbers above was found.

**Automated, in the real game, nobody at the keyboard:** `QuickWinsTest` checks the three cases in the log: a good
path is found and logged, a missing file is named with the path looked for, and a path that climbs out of the mod
folder is refused and never used. All 28 of its log checks passed as a Ratkin and as a Skelkin (the 23 it already had
and these 5), and its own 10 in-game checks passed each time. On the 3.8.1 build three of those five new checks
fail, so they are not passing by accident. The rest of the suite was run on the release build: Hello 11, Speed 41, Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55,
SpellEffect 15, Follower 60, Addons 27, `MoveTest` 28 and `QuickWinsTest` as above, with no failures. Three of
these tests have checks that need a person at the keyboard and nobody did them: Vision's sneaking checks, the
tin in both Neutral runs, and HelmTwin's checks where the player wears the items.

**Outside the game:** The Mod Builder's tests pass (180 for the block builder, 25 for the effect-name check and 32 for the table of
looks) and it builds. The Race Editor was driven in the dev server: typing an icon path writes
`"icon"` into the race JSON, and the Upload PNG button is there; no file was uploaded through it.

**Not tested:**

- **The status bars of other players in a networked co-op game.** The code sets the picture; nothing was run.
- **A picture that is not about 32 by 32.** The card draws the picture in a 32 by 32 box; other sizes were not tried.
- **A save made by picking the race in the lobby** and then opening the Continue screen (see above).
- **Uploading a PNG in the Race Editor** and exporting it.
- **Changing the picture without restarting the game.**
- **Multiplayer.** Nothing was tried with two machines.

## Not in this release

Asked for and not done: a small body with no innate abilities (the gremlin body brings +2 sneak vision and a
smaller sneaking slowdown, the gnome body senses gold while blind and picks up extra gold; only the sneak
vision can be cancelled today, with a negative Sneak vision) and a resistance and weakness field on a race (a
script can do it with `on_damage_multiplier`).
