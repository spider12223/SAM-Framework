# S.A.M Framework v3.2.0

**Races that see in the dark.** A comment on the Workshop page asked for "a sneak vision bonus, like
the Gremlin has" for a custom race. This release adds it, along with vision at all times, and
lets scripts grant either one.

## In a race's JSON

```json
{
  "id": "mymod:nightstalker",
  "host_body": "human",
  "sneak_vision": 2,
  "vision": 1
}
```

**`sneak_vision`** is the Gremlin's Improved Sneak Vision: extra tiles of light radius while the
player sneaks without a torch, lantern or other light source. The Gremlin's is 2. From -6 to 6.

**`vision`** is sight in the dark at all times. It goes where Perception's light bonus goes, so it
reaches every light the player carries, sneaking or not, and it is added on top of Perception's
own bonus. From -4 to 6. A negative number is a race that sees less.

Both are ignored for a player who turns the race's abilities off at character creation, the same
way the race's innate spells are. A race on the gremlin body already gets the Gremlin's own +2 from
the engine, and `sneak_vision` adds to it.

The Race Editor has a field for each, and the character sheet's "Sneaking Bonus Vision" line counts
them.

## From a script

`sam_add_stat_modifier` takes two new stat names:

```lua
sam_add_stat_modifier(player, "SNEAK_VISION", "cats_eye_ring", 2)   -- a ring of cat's eyes
sam_add_stat_modifier(player, "VISION", "night_potion", 3)           -- a potion of night sight
sam_remove_stat_modifier(player, "night_potion")
```

Bonuses from different mods add up, and each mod takes back only its own. The numbers are extra
tiles on the radius of the player's own light, so grant sight with the add. S.A.M's part is worked
out on its own: (the race's number + every add) x every multiplier, capped at -4 to 6 for VISION and
-6 to 6 for SNEAK_VISION, because a light's cost grows with the square of its radius. The engine's
own part (Perception, an eyepatch, the Gremlin's +2) is added afterwards, never scaled and never
capped away. So a multiplier of 0 cancels what the race and other mods gave, and does not blind
anyone.

A creature carries no light of its own, so `sam_add_monster_stat_modifier` refuses both names.

The rat, spider, troll and imp bodies have their own sneaking lights, which ignore the eyepatch.
S.A.M's sneaking bonus reaches those too, worked out without the eyepatch, so shapeshifting keeps
the race's bonus exactly.

## Testing a race without playing it

```bash
tools/run_mod_test.sh YourMod --race yourmod:nightstalker
```

`--race` (or `-samtestrace=` on the game's command line) starts the unattended test as a custom
race, with its stat changes and innate spells applied the way a character made in the menu gets
them. A race that no loaded mod registers stops the run with exit code 3, and the runner now prints
the reason for any stop, so a typo names itself.

## Also

- A player on this version reads a host's stat-modifier message even when it carries fewer stats
  than this version knows, as a 3.1.0 host's does. It used to be all or nothing.

## Numbers

Still 336 functions and 87 events: this release adds two stat names, two race fields and a test
flag. Nothing was removed and nothing changed meaning, so a mod written for 3.1.0 keeps working.

## What has and has not been tested

In the real game, in singleplayer: a test race with vision 6 and sneak_vision 3, measured through
the player's own light while the modifiers change (`VisionTest`, 21 checks). That includes the
sneaking light, measured with the Sneak key really held down. The test also fails as it should when
the race is left off. The other test mods (`HelloTest`, `SpeedTest`, `SettingsTest`, `LootTest`,
`AudioTest`) still pass. A review of the change found four problems before release, all fixed:
the eyepatch leaking into the rat, spider, troll and imp sneaking lights, Perception eating into a
race's vision, a missing note about gremlin-bodied races, and an unclear error for a mistyped
`--race`. A check of these notes against the code then found that a VISION multiplier also scaled
Perception's own bonus; it no longer does, and the test now checks that too.

**Not tested:** multiplayer, and the shapeshifted forms in play. If something is wrong in co-op,
the host's `sam_log.txt` should say so.
