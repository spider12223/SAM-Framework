# S.A.M Framework v3.6.0

**Items that don't wear out, weapons that put an effect on what they hit, and innate spells that
are on the hotbar.** The modder who asked about custom spells and effects in 3.5.0 sent three more
requests: "an innate spell that can be applied to the hotbar", "custom effects that can be applied
through custom spells or attacks", and "unbreakable weapons like the artifact weapons". All three
are in, with one limit on the second: the effect on an attack comes from a melee hit with a mod
weapon that names an `on_hit_effect`, not from a thrown weapon, an arrow, or a monster's own bite
or claw.

## `unbreakable`

```json
"category": "ARMOR",
"slot": "EQUIPPABLE_IN_SLOT_HELM",
"traits": ["unbreakable"]
```

An item with the `unbreakable` trait is not worn down by use, hits, blocks, burning, or the
Repair and Destroy Armor scrolls. Two things can still lower its condition: a script that calls
`sam_set_item_status`, and an Automaton that carries it in its pack, which can pick it as one of the
two items it recycles and take two steps off it.

The game already asks one question, "is this item being preserved?", at nearly every place it wears
things down: melee swings, bows, magic staves, digging, armour, shields and held spellbooks that
are hit, burning cloaks, spell reflection, scrolls, amulets, torches, lockpicks and helmets hit by
boulders. The trait answers yes to that question for the item, so it covers weapons, armour,
shields, cloaks and spellbooks alike, whoever wears it, a monster included, and no mana is spent.
(Digging, torches, lockpicks, amulets, spell reflection and boulders are tied to particular vanilla
items, so a mod item never meets those six.) A few places never ask. Reading a spellbook never did,
so S.A.M added the same check there. An Automaton recycling items from its pack never asks either,
and S.A.M did not change it. Vanilla items have no traits, so they behave exactly as before.

- **It is stronger than the vanilla artifacts.** The game's artifact armour is exempt from ordinary
  wear, but three scrolls can still hurt a worn piece if they happen to pick it: a Scroll of
  Destroy Armor that is not cursed breaks it, a cursed Scroll of Repair wears it down by up to two
  steps, and a cursed Scroll of Charging breaks it. This is read from the code, not run. An item
  with the trait is safe from all three.
- **It only stops future wear.** An item that is already broken stays broken, and one that was
  rolled as worn stays worn.
- **A magic staff with it never runs out of charges**, because the staff asks the same question.
  Reading a spellbook with the trait does not wear it either.
- When the game wears armour on a hit, it first picks one worn piece at random and only then asks
  whether it is preserved. A pick that lands on an unbreakable piece wears nothing that time, and
  the unbreakable piece never breaks and drops out of the pick the way an ordinary piece does.
  `sam_break_armor` with no slot named uses the same pick, so it can land on the unbreakable piece
  and return false: name the slot to be sure. This is from reading the code and was not tested.
- `sam_item_has_trait(item_type, "UNBREAKABLE")` reports what a mod declared, in Lua and in
  JavaScript. A vanilla artifact answers false there: it never wears out under the game's own rules,
  but the call does not know that. `sam_break_armor` refuses an item with the trait, and
  `/sam_traits` lists it.
- The Mod Builder's Item Editor has a checkbox for it.

## A weapon's `on_hit_effect`

```json
"category": "WEAPON",
"slot": "EQUIPPABLE_IN_SLOT_WEAPON",
"on_hit_effect": "mymod:plague",
"on_hit_chance": 0.25,
"on_hit_duration": 250
```

`on_hit_effect` and `on_hit_chance` have been in the item JSON for a long time and did nothing; the
schema called them "PLANNED". Now a melee hit with the weapon puts the effect on the creature it
hits, by a player or by a monster wielding the weapon. It applies only when the hit's damage works
out above 0 and the target is still alive afterwards, so a fully blocked blow, a killing blow, a
swing at a door, chest or other object, and any swing by a shapeshifted player apply nothing.

- `on_hit_effect` is one of your own effects by its exact id (lowercase, with the namespace), or a
  vanilla name such as `EFF_SLOW`. A name that matches nothing is ignored, and `sam_log.txt` says so
  once at load, and says which effect an item did resolve.
- `on_hit_chance` is a fraction from 0.0 to 1.0. Leave it out for every hit. An explicit 0 means
  never. A value above 1 is read as 1 with a warning. A chance that is negative or not a number is
  read as left out, so the weapon applies the effect on every hit, with no warning.
- **A spell's `on_hit_chance` is not on the same scale.** On a spell it is a whole percent, 0 to
  100, and 0 means every hit. Do not copy the number from one to the other: a spell's 25 on a
  weapon is read as 1.0, which is every hit, with a warning, and a weapon's 0.25 on a spell is cut
  down to 0, which is also every hit, with no warning.
- `on_hit_duration` is new: ticks, 50 to a second. Leave it out for the custom effect's own
  `duration_ticks`, otherwise 250.
- The usual effect rules apply (species immunities, the `on_before_effect_applied` event). The hit
  never shortens an effect the target already has, and leaves a permanent one alone.
- **Melee only.** It does not apply when the weapon is thrown or fired, or for a monster's natural
  attack (a bite, a claw). A golem smash or a bugbear shield bash does not count as a weapon hit.
  A damage-over-time effect that kills credits nobody, as with a spell.
- **This changes what an existing mod does.** The schema's own example item carried
  `"on_hit_effect": "EFF_BLIND", "on_hit_chance": 0.25`. A mod that copied it now blinds people a
  quarter of the time.
- The Mod Builder's Item Editor has controls for the effect, the chance (as a percent) and the
  duration on a weapon, and warns, as the Spell Editor does, when the effect id will not match.

## Innate spells on the hotbar, and `sam_get_hotbar`

A custom race or class lists spells in `starting_spells`. A new character that starts with them now
has them on the first free hotbar slots as well as in the spell list, after the class's own layout
is applied.

Before, they were in the spell list and never on the bar. The game gives a new character's spells
the first free slots, and then rebuilds the whole bar from a file that can name only the vanilla
classes and the vanilla spells, so a custom race's or class's spells were left without a slot. The
race's spells come first, then the class's. A spell the layout already placed stays where it is. If
the bar is full the rest stay in the spell list. The player can still move or clear anything on the
bar afterwards.

- **Only a new game gets this.** The placement runs when a new game starts and is skipped when a saved
  game loads, so a saved game keeps the bar it was saved with, and a character saved on 3.5.0 does
  not get its spells added to the bar. This is from reading the code; nobody loaded a save.
- It happens even if the player has turned off the game's Add Items to Hotbar setting, as with the
  game's own starting spells and items, because that setting only covers items collected during
  play.

```lua
local bar = sam_get_hotbar(0)     -- { { slot = 0, kind = "item", name = "iron_axe", type = 10, uid = ... }, ... }
```

`sam_get_hotbar(player)` reads the bar: one entry per occupied slot, `slot` 0 to 9 (the numbering a
class's `starting_items` `hotbar_slot` uses), `kind` `"spell"` or `"item"`, `name`, `type` and
`uid`. For a spell, `name` is a mod spell's `"namespace:spell"` or a vanilla spell's internal name
and `type` is the spell's id; for an item, `name` is its internal name (a mod item's
`"namespace:item"`) and `type` is the item type. It is read only and changes nothing on the bar. No
script call places something on the bar or chooses a slot, though a spell or item that a script
grants can still land on a free slot through the game's own add-new-items-to-hotbar option (read in
the code, not run).

The bar belongs to the machine its player plays on, so the call gives nil (undefined in JavaScript)
for a player on another machine, and logs it once for each such player. The bar is built before
`game.on_game_start` fires on the host and in singleplayer, so it should be readable in that event
or later; the test read it about two seconds after the start, and reading inside the event itself
was not tried. While a character is shapeshifted into a rat, spider, troll or imp, the game swaps
that form's own bar into the slots, so the call returns the form's bar. This is from reading the
code, not from running it in a form.

## Also

- The Mod Builder's start page still showed a "New in" panel from the 3.0 era and said the reference
  lists "184 functions and 72 events". The panel now describes this release, and the sentence no
  longer carries counts that go stale.
- The item schema's opening description said custom 3D models and on-hit effects were planned and not
  yet loaded. Models have loaded for a long time, and on-hit effects now do.
- `sam_item_has_trait`'s description said it applies the game's own rule for a vanilla item. That is
  true for five of the twelve traits (ranged, quiver, foci, instrument, thrown_ball). For the other
  seven, `unbreakable` included, it reports only what a mod declared.

## Numbers

338 functions to 339 (`sam_get_hotbar`), still 87 events. Nothing was removed.

Three things can change for a mod written against 3.5.0 that you do not edit. In a new game, the
spells a custom race or class lists in `starting_spells` are now also put on the hotbar, as long
as there is room, and a loaded saved game keeps the bar it was saved with. A weapon that already has
an `on_hit_effect` in its JSON now applies it on a melee hit, on every hit if it has no
`on_hit_chance`, unless the name matches no effect. And an item that already lists `unbreakable`
in its `traits`, which 3.5.0 ignored with a warning, now stops losing condition, in a loaded
saved game as well. Otherwise a mod written for 3.5.0 should keep working: from reading the code,
nothing else changes what it does, and the regression runs that exist for both versions gave the
same pass counts. No mod from outside the project was tried, and nothing was tried in multiplayer.

If your mod uses the `unbreakable` trait, a weapon's `on_hit_effect` or `sam_get_hotbar`, set its
`framework_min_version` to 3.6.0. A player on an older S.A.M still loads the mod, with a warning in
`sam_log.txt`, and gets none of it: the trait is ignored, the weapon puts nothing on what it hits,
and `sam_get_hotbar` does not exist there, so check that it exists before you call it. The
multiplayer lobby does not compare S.A.M versions, so nobody is warned about a player on an older
build.

## What has and has not been tested

In the real game, in singleplayer, started and run by the test runner with nobody at the keyboard:

- `AddonsTest` and `AddonsTestJS` (27 checks), run with a custom race on the human body, given to a
  vanilla Barbarian, whose `starting_spells` are two custom spells and a vanilla one:
  - **The hotbar.** Each of the three spells is on the bar exactly once, the bar still holds items
    as well as the spells (the check needs at least one; the run showed five: the Barbarian's iron
    axe, wooden shield, bronze tomahawk and torch, plus the polymorph potion that a race that is not
    human is given), a spell reports kind spell, and JavaScript's `sam_get_hotbar` returned the same
    eight entries as Lua's, matched on slot, kind, name and type (the uid was not compared). The
    three spells took slots 4, 5 and 6, the first free ones, in the order the race lists them; that
    was observed, not asserted. With the placement switched off in a temporary build, exactly the
    three spell checks failed and none of the three spells was on the bar, so the placement is what
    puts them there.
  - **Unbreakable.** Two helmets that differ only by the trait are worn by paralysed goblins, and
    the test calls `sam_break_armor` on each helmet eight times (nothing strikes the goblins). The
    ordinary one wore out (four of the eight calls degraded it, then it was gone) and the trait
    helmet did not (none of the eight did, and it was still worn). `sam_item_has_trait` read the
    trait in Lua and in JavaScript.
  - **The weapon effect.** A goblin wielding each of five blades hunted the player, who was the
    victim. The blade with no chance given landed the effect (497 ticks left a moment later). A
    blade with chance 0, a blade whose effect name matches nothing, and a blade with no
    `on_hit_effect` did not land the effect. In each case the player's health fell after a full
    heal while a goblin carrying that blade was alive, and the blade that names the effect and gives
    no chance did land it in the same setup, so "no effect" means something. A blade with
    `on_hit_duration` 100 left it with 84 ticks, not the effect's 500. Hitting a player who already
    had the effect for 500 ticks with that blade left 375, so a short hit did not shorten a longer
    effect. (That last check also passes on 3.5.0, where nothing is applied, so it rules out
    shortening but does not by itself show the hit refreshed anything.)
  - The same test was run on the shipped 3.5.0 build. 14 checks passed and 13 failed. The 13
    failures: `sam_get_hotbar` does not exist (6 checks), the trait is unknown (2), the trait helmet
    was worn down by four of the eight calls and then broke (2), and neither weapon that should
    apply an effect did (3). Of the 14 passes, 8 are set-up or control checks (the goblins hit, the
    ordinary helmet wore out) and 6 cannot fail on 3.5.0, such as an empty hotbar having no two
    entries in one slot.
- The regression suite on the release build: Hello 11, Speed 41, Settings 88, Loot 90, Audio 31,
  Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27, with no
  failures. Three of those tests have checks that need a person at the keyboard, and nobody did
  them this time: Vision's sneaking checks, the tin in both Neutral runs, and HelmTwin's checks where
  the player wears the items.

Outside the game, the Mod Builder's existing tests still pass (171 for the block builder, 25 for
the effect-name check). Neither was extended for this release and neither loads the Item Editor
page. They touch its new code only through the list of keys the editor saves and the effect-name
check it reuses. The Item Editor's new controls and checkbox were built but not opened in a
browser.

**Not tested:**

- **Unbreakable** was run one way only: a script called `sam_break_armor` on the helmet of a
  paralysed goblin, and the trait helmet held while the plain one wore out. No player wore an
  unbreakable item, no real hit landed on one, and nobody swung an unbreakable weapon. A weapon
  swing, a shield block, a bow, a magic staff, a spellbook, a cloak, the Repair and Destroy Armor
  scrolls, the claim that no mana is spent, and the `/sam_traits` command were read in the code, not
  run.
- **The weapon effect** when a player swings (the victim in the test was the player), when it hits
  a monster, and when it is thrown or fired (not built). The golem smash and bugbear shield bash
  exclusions, and a monster's natural attack. Damage-over-time kills. Also read in the code and not
  run: a vanilla effect name such as `EFF_SLOW`; a chance between 0 and 1, so the dice roll itself; a
  chance above 1 and its warning; the 250 tick fallback duration; a target that has the effect
  permanently; species immunity; a script that cancels or changes the effect through
  `on_before_effect_applied`; an effect that comes from a different mod than the weapon; and a hit
  that kills or does no damage, where the code applies nothing.
- **The hotbar** for a custom class's `starting_spells`, on their own, beside a race's, or beside
  items the class pins with `hotbar_slot` (the test gave a custom race to a vanilla Barbarian, who
  starts with no spells), for a class that starts with spells of its own, where the layout may
  already have placed one the race lists, for a joiner's machine, in a loaded saved game, with a
  bar that is already full, and slot 9 on a gamepad, where the game's face menu does not use it.
- **Multiplayer**, every part of it. The wear gate is asked on whichever machine decides the wear,
  and from reading the code a player on an older S.A.M does not know the trait, so wear that their
  own machine decides can still happen to an item there. A melee hit is decided by the host.
- `ChargeTest` was not run again: it needs a person to swing.

**Known, not fixed here:**

- `sam_fire_hook` turns every number into a whole number by dropping the part after the decimal
  point (1.9 arrives as 1, and -1.9 as -1), and turns `true` into `1` and `false` into `0`, in both
  Lua and JavaScript. Strings cross unchanged. A field whose value is a table, an array or a
  function is dropped as well, with nothing in the log. Until it is fixed, send only numbers, flags
  as 1 or 0 (compare them with `== 1`, since 0 counts as true in a Lua `if`), and strings, and send
  decimals multiplied up and rounded.
- A custom effect on a **monster** is quieter than on a player, from reading the code. Its
  `move_speed_mult` does nothing to a monster, and its `hp_per_second` drains silently: no damage
  numbers, the health bar does not refresh, and a monster it kills gives nobody experience.
- There is no way to put a spell on a chosen hotbar slot yet, and no script call whose job is to put
  something on the bar. A script can still cause it: if the player has the game's option to add new
  items to the hotbar on (it is on by default), a spell taught with `sam_grant_spell` is put on a
  free slot by the game, and so is an item from `sam_grant_item` if its kind is switched on for the
  hotbar (by default only spells, thrown items and a few tools such as torches and lockpicks). The
  game picks the slot, usually the first free one. This is from reading the code, not from running
  it.
