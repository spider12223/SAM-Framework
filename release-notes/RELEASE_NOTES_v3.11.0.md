# S.A.M Framework v3.11.0

**Every custom race gets a page in the Dungeon Compendium.** A modder asked for entries in the compendium for
custom races. The game's compendium only takes its own monster types, and a mod that ships its own copy of the
list replaces the game's whole list, so S.A.M now adds the pages itself. There is nothing new to write: a page
is made from the race file you already have. No new field and no new script function: 343 functions, 87 events,
as in 3.10.1.

## What a race page holds

- **Where.** The Denizens list. In the list's own order the pages come last, under a CUSTOM RACES heading;
  sorted A to Z, each one sits in its place among the game's names.
- **Name and text.** The title is the race's `name` in capitals. The left page shows its `description`, a line
  for each line break (`\n`), with a line too long for the box wrapped.
- **Model.** The host body as the compendium already shows it, in the pose with the least armour on its body,
  with its weapons, shields, hats, cloaks and quivers taken off. Then the race's own `limb_models` (with their
  `scale`, `offset`, `pitch` and `roll`) and its `extra_limbs` are put on where the game puts them on a player.
  It stands still: `sway` does not move it. A limb the race does not replace keeps the pose's own, which can be a
  booted leg or a gloved arm. The camera is the one the game uses for that body's own page, which faces the
  front, so a tail that hangs behind the back is mostly hidden behind the body.
- **Rat and spider bodies.** With the race's abilities on (the box "disable monster abilities" unticked), the
  game draws a player on those two bodies as one model, with no limbs and no extra limbs, so their pages show
  the game's own picture of the body (for a spider, its crab when the arachnophobia filter is on) and add nothing
  to it, not even the race's `head`.
- **The right page.** It reads PLAYABLE and the body's kind, for example PLAYABLE HUMANOID. The HP, AC and other
  ranges are the game's figures for a monster you fight, so a race page shows a dash. The damage row is the host
  body's, because a player of the race takes damage as that body does (as a human, with "disable monster
  abilities" ticked).
- **ABILITIES** lists what the race changes: its body, its stat changes, `vision`, `sneak_vision`, `blood_diet`,
  `can_open_tins`, `starting_spells`, `recruits`, `allies`, `enemies` and `neutral`. INVENTORY shows a dash.
  There is no RECORDS box: the game counts kills by monster type, so they would be the host body's.
- **Progress.** A race page is always unlocked, costs no lore points, counts for nothing in the completion
  figures and is never written into the game's compendium save (`savegames/compendium_progress.json`), so taking
  the mod out leaves that save as it was.
- **When.** The pages are made each time the compendium opens, from the races loaded at that moment, whether it is
  opened from the main menu or with P during a game. `sam_log.txt` has a line for each page, naming the pose it
  used and how many parts are the race's own.

## Also

- `sam_log.txt` now warns when a race on a rat or spider body declares `extra_limbs`: the game does not draw them
  on those two bodies unless the player ticks "disable monster abilities". It already warned about body
  `limb_models` there.

## Numbers

343 script functions, 87 events (unchanged). No new JSON field. The change is in the framework (`sam_races.cpp`)
and two engine files: `MainMenu.cpp` (the pages are made when the compendium opens; their text is wrapped, the
PLAYABLE line replaces the level, the records box is hidden) and `mod_tools.cpp` (the pages are left out of the
completion count and the compendium save). Nothing crosses the network: each machine makes the pages from the
races it has loaded.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:** a new test mod, `CompendiumTest`, declares four races:
a goblin body wearing a skeleton's head, chest, legs and arms (the chest scaled 1.25) with a salamander tail, a
plain human, an insectoid with a goblin's head, and a spider with a stick. The test run makes their pages (once
as with the arachnophobia filter on, then twice without it) and writes what each one holds to the log, and a log
check compares that with numbers worked out from the race files and the game's own compendium files: the
CUSTOM RACES heading in the list's own order, the four pages in both orders, and 70 and 62 entries in all (one
heading and four pages, so the second build replaced the first instead of adding to it), every part
of every model named by its file and in its slot, the chest's scale, where the tail sits and the point it turns
about, the lines and resistances of each page, the Denizens completion figure the same before and after, the
compendium save written during the run with none of the pages in it, the crab with the filter on, and the new
warning. 23 of 23 pass. On the 3.10.1 exe 19 of them fail, because it writes none of those lines; the other 4
pass there too (the three save checks, since 3.10.1 has no pages to save, and the script's own verdict). The
script's two checks are that the player of the first race wears the same head and tail its page shows. The rest of
the suite was run on the release build, with no reruns: Hello 11, Speed 41, Settings 88, Loot 90, Audio 31, Vision
17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27, `QuickWinsTest` 10 as a Ratkin and 10
as a Skelkin (each with its 28 log checks), `MoveTest` 28, `SwayTest` 13 and `RootSwayTest` 21, with no failures.
Three of these tests have checks that need a person at the keyboard and nobody did them: Vision's sneaking checks,
the tin in both Neutral runs, and HelmTwin's checks where the player wears the items.

**Seen in the game by a person:** the four pages, opened with P during a game: the models, the wrapped
description, the PLAYABLE line, the abilities list and no records box.

**Not tested:**

- **A modder's own race.** Only the four test races were drawn. For the other 14 bodies a race can use, a script
  worked out which pose a page takes and which parts it keeps; nobody looked at them.
- **The A-Z order and the arachnophobia filter on screen.** The log checks both; nobody looked.
- **Turning the camera.** The game's preview turns its camera while numpad 4 or 6 is held, which should show a
  tail behind the back. Read in the code, not tried.
- **Multiplayer.** Nothing was tried with two machines.
