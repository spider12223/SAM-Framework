# S.A.M Framework v3.15.0

**A spell that checks before it is cast, items of a race's own, and a polymorph that works.** Two modders asked.
One turned a Leap into an innate spell and wanted it refused when the player wears heavy armour or has no gap ahead,
and cast normally otherwise. The other wanted items only one race starts with, and found that a spell's polymorph
effect only showed its icon and that a bolt written with zero damage still hurt. One new event and one new race field:
365 functions, 97 events.

## Refuse a cast before it starts: `player.on_before_spell_cast`

```lua
function on_event(e)
  if e.name == "player.on_before_spell_cast" and e.spell_name == "mymod:leap" then
    if not leap_is_possible(e.player) then
      sam_hud_text("leap_refused", 20, 120, "There is nowhere to land.", nil, e.player)
      return false            -- the cast never starts: no animation, no mana
    end
  end
end
```

- Return `false` and the cast never begins: no casting animation, no mana, no gold, no bolt, and
  `player.on_spell_cast` does not fire. It costs nothing in single-player or online. Let it through and the spell is
  cast and charged as usual; do the spell's work in `player.on_spell_cast`.
- It fires after the game's own refusals (not enough mana or gold, paralysed, asleep), so it is asked only about a
  cast that would otherwise start. It covers a player's own casts from the spell list, the hotbar and a spellbook.
  Magic staffs, scrolls and `sam_cast_spell` are not asked.
- It runs on the caster's own machine: the host's game for the host and splitscreen players, a joiner's own game for
  a joiner, as `player.on_before_equip` does. A refusal is silent. Say why with `sam_hud_text`: `sam_message` is a host
  function and is refused in a joiner's game. Decide from what that machine can see: the caster's own equipped items,
  facing and position, and the map's tiles.
- Returning `false` from `player.on_spell_cast` still stops a cast, but in single-player the casting animation has
  already spent most of the mana by then. The player can still turn while the animation plays, so if the answer can
  change by the time the cast is let go, check again there.
- The scripting reference has the whole Leap: light armour, a gap in front, floor beyond.

## A race's own starting items

```json
{ "id": "mymod:ratkin", "name": "Ratkin", "host_body": "human",
  "starting_items": [ { "type": "FOOD_CHEESE", "count": 3 }, { "type": "mymod:rat_cloak", "equip": true } ] }
```

- A race can list `"starting_items"`, the same entries as a class's (`type`, `count`, `status`, `beatitude`,
  `appearance`, `identified`, `equip`, `hotbar_slot`). Every character of the race gets them, whatever the class,
  after the class's own kit. An item with `"equip": true` is put on even when the class's kit already filled that
  slot, and the class's item goes to the backpack.
- Only for a character who plays the race with its abilities: one who picked it for its look alone gets none, as with
  `starting_spells`. Only a new character gets them.
- The Race Editor has the Class Editor's loadout board for them, without the gold.
- For anything else only a race should get, a race script checks `sam_get_race(player) == "mymod:ratkin"`. The block
  builder has a new condition for it, "is playing a race".

## A real polymorph

- An `on_hit_effect` of `EFF_POLYMORPH` (or `"polymorph"`) on a spell or a melee weapon now does what the game's
  polymorph potion does: a monster turns into a random other monster, for good, and a player takes a random monster's
  form for `on_hit_duration` (5 seconds if left out). Bosses, shopkeepers and the other monsters the game never
  polymorphs are left as they are. Before, a monster only got the status icon and stayed what it was.
- On a monster the game's own polymorph rules apply in place of the usual effect rules: species immunities and
  `player.on_before_effect_applied` are not asked.
- It happens on the host a moment after the hit, because the game deletes the monster it replaces.

## `"damage_max": 0`

- A spell's `"damage_max": 0` now means the bolt does no damage. Before, 0 was read as left out and the bolt did its
  payload's own damage, so a spell meant to do no harm hurt what it hit. Leave `damage_max` out to keep the payload's
  damage. A bolt that hits still counts as an attack, so a monster it hits turns on the caster.

## Numbers

365 script functions and 97 events (one new). One new JSON field, a race's `starting_items`. The engine changes are a
call each in `magic/castSpell.cpp` (the event), `magic/actmagic.cpp` (a spell's polymorph) and `charclass.cpp` (the
race's items). S.A.M adds no new network messages: the event runs on the caster's machine and sends nothing, and the
race's items and the polymorph go through the game's own paths.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:**

- `tests/BeforeCastTest`, 9 checks, as a wizard. A 10-mana spell is put in the player's hands and the script presses
  Cast Spell; the handler refuses it. The event fired once for that spell, `player.on_spell_cast` never fired, and the
  mana did not go down (it read 50 before and after). Then the handler lets the same press through: the event fires again, `player.on_spell_cast` fires
  once, and 10 mana is taken.
- `tests/RaceItemsTest`, 5 checks as a Packrat wizard (a vanilla class, so only the race can give these): 3 cheese, the
  race's leather helm worn and the wizard's hat in the backpack, a ruby on hotbar slot 7, and the class's quarterstaff
  still wielded. Then 5 checks as a human wizard, the control: none of the race's items, and the hat stays on.
- `tests/PolymorphTest`, 6 checks: a force bolt with `"damage_max": 0` slows a goblin and takes no health, a control
  with `"damage_max": 3` takes health, and a polymorph bolt replaces the goblin it hits with a monster that is not a
  goblin.
- A build made for this check, with the old behaviour put back in all four places, failed each of these tests: the
  refused cast took its 10 mana, the Packrat got none of its items, the damage-0 bolt hurt the goblin, and the
  goblin stayed a goblin.
- The Mod Builder, in a browser: the Race Editor's board writes `starting_items`, and the Class Editor's board still
  works.

The regression script (`handoff/run_regress.sh`) was run on the release build, with no reruns: Hello 11, Speed 41,
Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27,
`QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with its log checks), `MoveTest` 28, `SwayTest` 13,
`RootSwayTest` 21, `CompendiumTest` 2 (with its log checks), `TauntTest` 4 (with its log checks), `EmoteTest` 3, 3 and
1 for its three races, `MomentumTest` 26, `PartyTest` 45, `ItemMoveTest` 56, `PartyFixTest` 13 (with its log checks),
`NoneSpellTest` 6, `BeforeCastTest` 9, `RaceItemsTest` 5 and 5, `PolymorphTest` 6 and the installer layout check 14,
with no failures. Three of these tests have checks that need a person at the keyboard and nobody did them: Vision's
sneaking checks, the tin in both Neutral runs, and HelmTwin's checks where the player wears the items. `ChargeTest`,
which needs a person to swing at a goblin, is not in the script and was not run.

**Not tested:**

- **Multiplayer.** Nothing was tried with two machines. The event in a joiner's game, and a joiner's race items, are
  read in the code.
- **A melee weapon's `EFF_POLYMORPH`**: it goes through the same queue as a spell's (read in the code).
- **A Packrat made at the menu by hand.** The test run's character is made after the race is set, through the same call
  in the game's character setup, and the log shows that call giving the items; nobody made one at the menu.
- **A spell with a gold cost and too little gold** is not asked about, because the game refuses it first (read in the
  code).
