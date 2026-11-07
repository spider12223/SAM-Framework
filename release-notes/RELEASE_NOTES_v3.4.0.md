# S.A.M Framework v3.4.0

**Mod hats that sit the right way up, and charged hits.** Two requests from players: a hat in
the S.A.M port of More Classes was worn sideways, and a comment on the Workshop page asked how a
script can tell that a melee hit was fully charged.

## Hats, masks, gloves and boots: `worn_like`

```json
"slot": "EQUIPPABLE_IN_SLOT_HELM",
"model": "models/mymod/plague_hat.vox",
"worn_like": "HAT_BOUNTYHUNTER"
```

The game does not read where a hat goes from the hat. It matches the hat's model against its own
models and looks up how that one sits: how high, how far back, which way it is turned, and how it
fits with a mask. A model a mod ships matches none of them, so it was worn exactly as built. The
game's own hats are built lying on their side and turned upright when worn, so a mod hat made the
same way sat sideways on the head. That was the More Classes plague hat.

`worn_like` names the vanilla item a mod item is worn like, and it then sits there on every body
the game dresses, the inventory paper doll included, with or without a mask. Masks work the same
way, by naming a vanilla mask. Naming `TOOL_GLASSES` or `MONOCLE` puts a mod mask where the game
puts worn glasses or the worn monocle (the two sit slightly differently on some bodies), while it
still draws its own model.

**Gloves and boots** are drawn differently: the game swaps the whole arm or leg for a model of
that arm wearing that pair, picked from its own list by item type. A mod's gloves or boots were on
no list, so the leg kept whatever model it had before, and so did the arm, except on a player
holding a weapon or shield: that arm stepped to a different, unrelated model every tick and could
end up drawing nothing. Now `worn_like` picks which vanilla pair
is drawn, on the body, in your own hands in first person, and on the hands that cast spells.
Without it, a mod pair is drawn as the gloves or boots its `model_from_item` names, or else as
plain `GLOVES` or `LEATHER_BOOTS`. A mod's own model for gloves or boots still shows only on the
floor; that is how the game draws them.

A few rules, all also in the log when they apply:

- `worn_like` has to name a vanilla item from the same slot, and only works on a hat, mask, gloves
  or boots.
- It belongs to the model file. The game knows a hat only by its model, so every hat or mask that
  uses the same `.vox` sits the same way, and the log names any item that shares a file with one
  that has a `worn_like`.
- Models in `model_states` count as the item's own, so a hat whose only model of its own is its
  cursed or blessed look can use it.
- When a mod helmet's model looks built on its side and the item has no `worn_like`, the log says
  so and names the item.

The Mod Builder's Item Editor has a **Worn like** field for the four slots it works in.

**For the More Classes port:** add `"worn_like": "HAT_BOUNTYHUNTER"` to `items/plague_hat.json`.

## `sam_get_limb`

```lua
local hat = sam_get_limb(uid, "helmet")
-- hat.sprite, hat.model, hat.visible, hat.roll, hat.focalx, hat.focaly, hat.focalz, hat.scalex ...
```

Reads one body part of a player, or of a creature the game can put a helmet on (a human, goblin,
skeleton, gnome, kobold and the like), as the game left it this tick: the model the game put on
it, whether the game hid it, where it is, and how it is turned and scaled. The parts are torso,
the two arms and legs, weapon, shield, cloak, helmet and mask. It answers nil for every other
creature (a rat, a slime, a ghoul, even a bugbear with its sword and shield) and for a player in
rat or spider form. On a creature with a custom body, or a part a script gave a model with
`sam_set_model`, what is drawn on screen is not what this reports. It is how the tests for this
release check where a hat really ended up, and a mod can use it to line an effect up with a hand
or a head. Read-only, and the same in Lua and JavaScript.

## Charged melee hits

`player.on_before_hit` and `player.on_hit` carry three new fields:

- `charge`: how far the swing was wound up, from 0 to `max_charge`.
- `max_charge`: 30, unless the Ensemble flute shortens it. Compare against this, not against 30.
- `fully_charged`: 1 when the swing was held all the way.

```lua
if e.name == "player.on_before_hit" and e.fully_charged == 1 then
  e.damage = e.damage + 5
end
```

A fully charged swing already does double damage in vanilla (a rapier a little more), and
`damage` includes that. In co-op the swing reaches the host with its charge, so nothing new is
sent.

## Also

- The Mod Builder now puts brass knuckles, iron knuckles and spiked gauntlets in the Hands slot of
  a class's starting gear, and the quilted cap in the Head slot, which is where the game wears
  them. It had them as weapons and backpack items.

## Numbers

336 functions to 337 (`sam_get_limb`), still 87 events. Nothing was removed. One thing looks
different without touching a mod: gloves and boots from a mod are now drawn as a vanilla pair, on
the body, in your own first-person hands and on the hands that cast spells, where before the arm
or leg kept whatever model it had (or, holding a weapon, stepped through unrelated ones). The log
also has new lines about `worn_like` and about hat models that look built on their side.
Otherwise a mod written for 3.3.0 keeps working.

## What has and has not been tested

In the real game, in singleplayer:

- `HelmTwinTest` (55 checks), using the plague hat's own model: each item put on a human and a
  goblin (gloves on the human only, since a goblin's arms draw no gloves) and read back with
  `sam_get_limb`. A mod hat sits exactly where `HAT_BOUNTYHUNTER` sits on
  both bodies, which place it differently; the same model without `worn_like` still lies at the
  old angle; `worn_like HAT_WIZARD` follows the game's hardcoded wizard-hat rules; a mod mask sits
  like `MASK_BANDIT`; a glasses-like mask sits like worn glasses with its own model; a mod hat with
  a mask is fitted exactly like the vanilla one; a cursed-only hat model works; gloves and boots
  draw the named pair and the defaults. JavaScript's `sam_get_limb` read the same values as Lua.
- On an earlier build, before the review fixes, a person wore the mod hat and the bounty hunter
  hat on the player: each drew its own model, and both sat in exactly the same place. That has not
  been repeated on the release build, where nobody was at the keyboard for that step.
- `ChargeTest` (14 checks), with a person swinging at a goblin: a tap reported charge 0 of 30 and
  did 11 damage, a fully charged swing reported 30 of 30, `fully_charged` 1, and did 22. Both
  events and JavaScript agreed on every swing.

A review of the change from five angles found 15 problems before release, all fixed: the shared
model files, `model_states`-only hats, the spellcasting hands, a player in rat or spider form in
`sam_get_limb`, a Mod Builder item keeping its `worn_like` after its slot changed, the Mod
Builder's slots for knuckles and the quilted cap, and wording in the docs. A check of these notes
against the code then corrected thirteen sentences before they were published.

**Not tested:** multiplayer (every machine works the placement out from its own copy of the mod,
and nothing new crosses the network); every creature body other than the human and the goblin
(each of the other 14 places a mask in its own code); mod masks, gloves and boots on the player
rather than on a creature; the first-person hands, including the spellcasting hands; a player in
rat or spider form; gloves or boots that take their pair from `model_from_item` (only the plain
`GLOVES` and `LEATHER_BOOTS` fallback was tested); `worn_like` `MONOCLE` (only `TOOL_GLASSES`
was); and `max_charge` under the Ensemble flute. If a hat or mask sits wrong, nothing logs it as
it happens: send a screenshot and the `sam_log.txt` of the machine that shows it, which lists
every `worn_like` that machine loaded.

**Known, not fixed here:** `sam_fire_hook` turns every number into a whole number by dropping the
part after the decimal point (1.9 arrives as 1, and -1.9 as -1), and turns `true` into `1` and
`false` into `0`, in both Lua and JavaScript. Strings cross unchanged. Until it is fixed, send
flags as 1 or 0 and compare them with `== 1` (0 counts as true in a Lua `if`), and send decimals
multiplied up and rounded.
