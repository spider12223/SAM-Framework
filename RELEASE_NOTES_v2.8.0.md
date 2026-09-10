# S.A.M Framework v2.8.0

**Combat.** A script could always deal damage. It could not heal, could not ask what a hit would
actually do, and could not change how much damage a kind of creature takes. Thirty-two new
functions and two new hooks, taking the API from 255 to 287.

## Healing was genuinely unreachable

`sam_deal_damage` forces its amount negative, so both signs damage. The only route to healing was
an absolute `sam_set_stat(player, "HP", n)` write, which makes you read, add and clamp by hand and
does not exist at all for a monster.

```lua
local restored = sam_heal(uid, 20)   -- returns what LANDED, not what you asked for
```

Health is clamped to the maximum, so a 20-point heal on something three short of full restores
three, and it tells you so. A lifesteal effect needs the real number.

## Damage that respects what the target is

```lua
local dealt = sam_deal_damage_typed(uid, 10, "magic")
```

Ten magic damage is five against something that halves magic and twenty against something that
doubles it, without your script knowing which. Both stages the engine applies are applied here, in
its order: the species damage table, then live effects like blood ward and sanctuary.

Barony's damage types are **weapon classes, not elements**. There is no fire, ice or lightning axis
anywhere in the damage funnel — the seven are `sword`, `mace`, `axe`, `polearm`, `ranged`, `magic`
and `unarmed`. A mod that wants fire damage picks a class and keeps its own label.

## Reading the combat maths

`sam_get_hp`, `sam_get_max_hp`, `sam_get_mp`, `sam_get_max_mp`, `sam_get_attack`,
`sam_get_ranged_attack`, `sam_get_thrown_attack`, `sam_get_bonus_attack_vs`,
`sam_get_damage_resist`, `sam_get_magic_resist`, `sam_preview_damage`, `sam_get_regen_interval`,
`sam_get_healring`.

All take a uid, and all answer nothing for a thing that is not a creature rather than zero —
because a door does not have zero health, it has none.

`sam_get_hp` fills a real gap: `sam_get_stat` takes a player index and `sam_get_monster_stat`
refuses anything that is not a monster, so a uid out of `sam_find_entities` could not reach another
player's or a companion's health at all. `sam_get_stat` now also works **on a client**, for that
client's own player, which is what a client-side HUD mod needs and was being refused data its own
machine already held.

## Mana, with the verbs health already had

`sam_mod_mp` changes it by a relative amount. `sam_consume_mp` spends only if affordable.
`sam_drain_mp` takes what it can and the rest out of **health** — that overdraw is the point, it is
how a blood-magic cost is expressed, and it has its own name rather than hiding inside `sam_mod_mp`
behind a negative number.

## Blocking, parrying, aggro, consequences

`sam_set_defending` is the half `sam_is_defending` never had. `sam_is_parrying` / `sam_set_parry`
expose a field nothing could see, though the engine has always used it to produce parried damage.

`sam_set_monster_target_uid` points a monster at **anything with a body** — another monster, a
companion — which `sam_set_monster_target` cannot express because it takes a player index.
`sam_get_monster_target_uid` reads it back, `sam_clear_monster_target` undoes it, `sam_alert_allies`
wakes the room.

`sam_break_armor` gives `player.on_item_broken` a cause. `sam_gib` throws a chunk off a creature.
`sam_obituary` gives a scripted kill a proper death message and credits the killer, which
`sam_kill_monster` never did. `sam_revive_player` brings a dead player back at half health.

## A whole species' resistance

```lua
sam_set_species_damage_resist("skeleton", "mace", 2.0)   -- maces shatter bone
sam_set_species_damage_resist("slime", "sword", 0.25)    -- swords barely cut
```

Every creature of that species, now and later. Cleared when your mods unload.

**It cannot grant immunity, and that is worth knowing before you rely on it.** Your number seeds
the multiplier; the engine then runs its own bonus pool over it and floors the result at `0.1`, so
the least any species can be made to take is a tenth. Pass `0` and you get `0.1`. For real immunity
use `sam_set_damage_immune`, or veto `on_before_damage` or `on_damage_multiplier`.

## Two new hooks

**`on_damage_multiplier`** is for changing how much damage something takes as a *rule*, rather than
one hit at a time. It sits at the single point every damage path in the game goes through, so
melee, arrows and every spell all arrive there.

```lua
if e.name == "on_damage_multiplier" then
  if sam_get_monster_type(e.target_uid) == "skeleton" and e.damage_type == 1 then
    sam_add_damage_multiplier(1.0)      -- +100% mace damage to skeletons
  end
end
```

Positives **add** and negatives **multiply**, which is Barony's own rule for its bonus pool: two
mods each contributing `+0.2` give +40%, and two each contributing `-0.5` give a quarter rather
than nothing. So two mods that both change damage both apply.

**`player.on_before_hit`** fires when a player's melee swing has connected and the damage is decided
but not yet applied. It is the only place in the engine where the attacker, the crit state and a
writable damage figure exist together:

```lua
if e.name == "player.on_before_hit" and e.backstab == 1 then
  e.damage = e.damage * 3      -- assassin
end
```

`e.backstab` and `e.flanking` are Barony's nearest thing to a critical hit and die as stack locals
everywhere else, so this is the only way to see one. `e.attacker_uid` is real here, unlike
`on_before_damage` where it is always 0 — the engine's damage funnel carries no attacker, which is
why every older damage event reports zero and still does.

## Nothing changes without a mod loaded

Three engine call-outs were added and all three are inert in a plain game: the species override is
one bool test against an empty table, and both hooks ask whether any script exists at all and
return. Nothing new crosses the wire, and a stock Barony 5.0.2 client can still join and play.

## Installing

Run `SAM_Framework_Installer.exe`. It finds Barony on its own and keeps your original. If you
already have S.A.M, run the same installer again — it always fetches the newest build.

Everyone playing together needs the same version.
