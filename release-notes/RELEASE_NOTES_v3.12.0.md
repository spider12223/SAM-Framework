# S.A.M Framework v3.12.0

**A race's compendium page can tell its story, and its own arms hang right.** The modder who asked for
compendium pages tried them with a catfolk race and found two things: its arms sat out of place, and the box
under the model showed its description, which in the modder's screenshot is a list of perks and drawbacks
written for the character-select picker. One
new race field, `lore`, and no new script function: 343 functions, 87 events, as in 3.11.0.

## A race's own text for its page

- `"lore"` is new and optional. When a race has it, the box under the model on its compendium page shows it in
  place of `description`. The `description` stays in the character-select picker and in the tooltip on the
  character sheet, so the picker can keep a list of what the race does while the compendium tells its story.
- Each line break (`\n`) starts a new line, and a line too long for the box is wrapped. The box was made for the
  game's own texts, which are about six short lines.
- The Race Editor has a Compendium text box for it.

```json
"lore": "Territorial and aloof by nature, catfolk live in small clans that do not welcome outsiders."
```

## A race's own limbs sit where the game puts them

- In 3.11.0 a page put the race's head, torso, legs and arms where the host body's compendium pose had its own,
  with the pose's turning points and angles. Those belong to the models the pose shows. The human pose holds both
  arms bent, gripping something, and a bent arm turns about a point further forward and higher than a hanging
  one, so a human-bodied race's own arms sat forward and up, out of place.
- Now a part the race replaces moves only by what the game would move it for the new model on a player of that
  body: S.A.M asks the game's own placement code where the pose's model and the race's model go, and applies the
  difference. The race's own arms hang, holding nothing.
- The same mistake, found while fixing it and fixed by the same code: a torso that is not a gremlin's own turns
  about another point, a torso that is not an insectoid's own sits 1 higher, and legs that are not a salamander's
  own feet are not pushed back. On a myconid body, whose pose has a short head, a race head also moves the arms
  and legs the way the game moves them under a head that is not short.
- Parts the race does not replace keep the pose's own, as before, so an arm the race does not replace still holds
  the pose's grip, around a weapon the page takes off.
- Known limit: under a torso that is not a gremlin's own, the game sets the gremlin's legs 0.2 higher, and the page
  does not, because it depends on another part.

## Numbers

343 script functions, 87 events (unchanged). One new race field, `lore`, in `schemas/race.schema.json` and the
Mod Builder. The change is in the framework (`sam_races.cpp`); no engine file changed. Nothing crosses the
network: each machine makes the pages from the races it has loaded.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:** `CompendiumTest` gains three races: a human body with
its own arms, built the way the catfolk is; a gremlin with its own torso; and a salamander with its own legs. Its
insectoid race gains its own torso, and the human-bodied race has a `lore` with a line break. The expected
numbers were worked out from the game's `limbs.txt` files and its placement code, not from S.A.M:

- On the human body, the race's arms hang about 0, 0, 1.5 with no turn; the pose holds them bent about 0.75, 0,
  0.75, the right one raised. On the goblin body they hang about 0, 0, 2.
- The gremlin's torso turns about -0.26, 0, 0.75 instead of -1.26, 0, 1.25.
- The salamander's legs are not pushed back 0.5.
- The insectoid's torso sits 1 higher.
- The page's text lines hold the lore, one per line break, and a race without lore still holds its description
  (read from the log; nobody has looked at the lore on screen).

27 of 27 checks pass on the release build. A build of the 3.11.0 page code, with the log line that records each
part's angle added, fails the five placement checks (21 passed, 5 failed, before the lore was in the test). The
build before `lore` existed fails the lore check (26 passed, 1 failed). The rest of the regression script
(`handoff/run_regress.sh`) was run on the release build, with no reruns: Hello 11, Speed 41, Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3,
HelmTwin 55, SpellEffect 15, Follower 60, Addons 27, `QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with
its 28 log checks), `MoveTest` 28, `SwayTest` 13 and `RootSwayTest` 21, with no failures. Three of these tests have
checks that need a person at the keyboard and nobody did them: Vision's sneaking checks, the tin in both Neutral
runs, and HelmTwin's checks where the player wears the items. `ChargeTest`, which needs a person to swing at a
goblin, is not in the script and was not run.

**Seen in the game by a person:** the seven test races' pages, opened with P during a game, after the placement
fix and before `lore` was added: the arms hang and the replaced parts sit right.

**Not tested:**

- **The modder's own catfolk race.** Its files were not available; the test race is built the way the screenshot
  shows it, a human body with its own arms.
- **The lore on screen**, and a lore longer than the box.
- **A myconid body with a race head.** The rule above was read in the game's code; no test race has it.
- **Multiplayer.** Nothing was tried with two machines.
