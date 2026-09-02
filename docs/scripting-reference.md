# Scripting reference

What your script can be told, what it can decide, and what it can change.

A script is a `.lua`, `.js` or `.ts` file. Put it next to a class or item JSON with the same
name, or call it `main.lua` at your mod's root and it loads on its own.

```lua
function on_event(e)
  if e.name == "player.on_kill" then
    sam_log("killed something")
  end
end
```

---

## The two kinds of hook

**Notifications** tell you something happened. You react; the game carries on regardless.

**Decisions** ask you what should happen. Return `false` and the game **skips what it was
about to do**. Return anything else, or nothing at all, and it proceeds as normal.

```lua
function on_event(e)
  if e.name == "player.on_item_use" and e.item_type == my_bomb then
    do_my_own_thing()
    return false        -- the game does NOT run its own item-use code
  end
end
```

Only a literal `false` cancels. Returning `nil`, `true`, or nothing means "carry on", so a
script written before decisions existed behaves exactly as it always did.

Every handler still runs even after one cancels, so two mods watching the same event both
see it and neither can silently starve the other.

### Hooks you can cancel

| Hook | Returning false means |
|---|---|
| `player.on_item_use` | Don't run the item's normal use behaviour |
| `player.on_before_item_pickup` | Refuse the pickup; the item stays on the floor |
| `player.on_spell_cast` | Stop the cast. Fires before mana is spent, so it costs nothing |
| `world.on_item_deployed` | Don't build the engine's gadget; you built your own |

Everything else is a notification. Returning `false` from one does nothing.

---

## New in 2.4: the creature toolkit

### Monsters

`sam_spawn_monster(x, y, "ns:monster")` — the third argument now takes **your mod's own
monster id** as well as a vanilla species name. It summons the variant's base creature and
then runs the same routine the dungeon generator uses, so the stats, equipment, traits,
body model and followers you declared all arrive with it. Returns the uid, or nil if that
id was never declared.

`sam_set_monster_name(uid, "Grimt the Proof")` — renames a living creature. **Host-side
only**: clients hold no Stat for an ordinary monster, which is also why
`sam_get_monster_name` already returns nil on a client. So this shows in singleplayer and
on the host, and carrying names to clients needs its own packet, which is not in this
release. Also note Barony treats a name containing `lesser`, `young`, `enslaved`,
`damaged`, `corrupted`, `cultist` or `encased` as generic and falls back to the species
name — vanilla behaviour, but surprising if your epithet table contains those words.

`sam_monster_equip(uid, slot, item [, beatitude [, status [, count]]])` — puts a real item
into one of the ten slots: `helmet breastplate gloves shoes shield weapon cloak amulet ring
mask`. It is worn, used in combat, and dropped when the creature dies. Anything already in
the slot is dropped on the floor rather than lost. Takes a vanilla name or a custom
`ns:item`. `sam_monster_unequip(uid, slot)` drops what is there.

`sam_attach_behavior(uid, "name")` / `sam_detach_behavior(uid)` — runs a registered
behaviour every tick for one **living** monster, **after** its own AI rather than in place
of it. This is the difference from `sam_register_behavior`, which replaces `behavior`
outright: correct for an entity you created, fatal for a monster, because it would delete
its AI, its death handling and its drops. Register the function with
`sam_register_behavior` first.

### Reading the run

`sam_get_seed()` — the run seed. Stable for the whole run, identical on host and clients,
0 on the main menu.

`sam_random(stream, lo, hi)` — an integer in `[lo, hi]` from a stream seeded by the run
seed, your namespace and the stream name. Two machines draw the same number without
sending anything, so a roll is safe to act on in co-op. It never touches the engine's own
dice: drawing from those would shift every later engine roll and change the dungeon.
`math.random` gives a different answer per machine and must not decide anything the world
can see.

`sam_get_flag(name)` — a lobby setting: `cheats friendlyfire minotaurs hunger traps
hardcore classic keep_inventory lifesaving assist_items`. Returns a boolean, or nil for an
unknown name. Valid on clients.

`sam_is_ghost(player)` / `sam_is_spirit_ghost(player)` — whether a player is a ghost that
can act, and whether that ghost is Project Spirit (the player is still alive) rather than
death. Use the second to avoid paying out a death reward to a living caster.

`sam_list_data_keys()` — the key names this mod has written with `sam_save_data`.

### Events

| Event | Cancellable | Fields |
|---|---|---|
| `player.on_became_ghost` | no | player, uid, x, y, spirit |
| `player.on_callout` | **yes** | player, cmd, type, target_uid, x, y, help_flags |
| `player.on_before_revive` | **yes** | player, floor, keep_gear |
| `world.on_before_chest_open` | **yes** | player, chest, x, y |
| `player.on_game_over` | no | player, tutorial, survived, placement, made_top |

`player.on_callout` carries the callout type Barony already computes for every ping (33 of
them, from TRAP and CHEST through SECRET_EXIT and NPC_ENEMY). Refusing it stops the ping
before it is sent.

`player.on_before_revive` fires at the one place Barony's co-op death rule actually
happens: the block that frees all ten equipment slots, purges the inventory and stands the
player up on the next floor. Refuse it and that rebuild is skipped.


## Hooks you can change the number of

Some hooks hand you a value the engine is about to use. Rewrite it and the game uses yours.

| Hook | Rewrite with |
|---|---|
| `on_before_damage` (player takes damage) | `sam_modify_damage(player, n)` |
| `on_before_monster_damage` | `sam_modify_monster_damage(n)` |
| `player.on_xp_gained` | `sam_modify_value(n)` |

```lua
if e.name == "on_before_monster_damage" and is_undead(e.monster_type) then
  sam_modify_monster_damage(e.damage * 2)   -- smite
end
```

Setting damage to `0` negates the hit entirely. Values are clamped at zero — you can't heal
something by passing a negative.

> `sam_modify_value` is the general form used by newer hooks. The two damage functions are
> older and specific. Calling the wrong one is a no-op and logs a warning telling you which
> hook you're actually inside.

---

## Every event

Fields vary per event; `sam_log(e.name)` inside your handler and read the log if unsure.

### Player

`player.on_attack_start` `player.on_hit` `player.on_miss` `player.on_kill`
`player.on_block` `player.on_damage_taken` `player.on_death` `player.on_level_up`
`player.on_xp_gained` `player.on_gold_collected` `player.on_hunger_change`
`player.on_floor_change` `player.on_player_joined` `player.on_player_left`
`player.on_player_revived` `player.on_proficiency_increased`

### Items

`player.on_item_use` `player.on_before_item_pickup` `player.on_item_pickup`
`player.on_item_dropped` `player.on_item_broken` `player.on_item_identified`
`player.on_equip` `player.on_unequip` `player.on_item_bought` `player.on_item_sold`
`player.on_shop_entered`

> `on_before_item_pickup` fires **before** the item enters your inventory and can refuse it.
> `on_item_pickup` fires **after** a successful world pickup and is a notification. They are
> different moments — don't assume one is an alias of the other.

### Magic and effects

`player.on_spell_cast` `player.on_spell_failed` `player.on_spell_learned`
`player.on_effect_applied` `player.on_effect_removed` `player.on_effect_expired`
`player.on_status_effect_tick` `player.on_poison_tick` `player.on_bleed_tick`

### Monsters

`on_monster_damaged` `on_monster_died` `on_before_monster_damage`
`world.on_monster_spawned`

### World

`world.on_door_opened` `world.on_chest_found` `player.on_chest_opened`
`world.on_trap_triggered` `world.on_boulder_triggered` `world.on_switch_toggled`
`world.on_fountain_used` `world.on_sink_used` `world.on_orb_placed`
`world.on_projectile_hit` `world.on_teleport` `world.on_item_deployed`

### Game

`game.on_game_start` `game.on_game_end` `game.on_level_entered`

Plus `on_tick` (every frame), input events, and `sam_fire_hook` for script-to-script calls.

---

## Never hardcode an id

This is the single most common way to lose an evening. The enum values are not what you'd
guess — `TOOL_TORCH` is 148, `RAT` is 2, and the status effect is `FAST`, not `EFF_FAST`.
A wrong number doesn't error, it just silently never matches.

```lua
local torch = sam_item_id("TOOL_TORCH")      -- do this
local torch = 8                              -- not this (it's 148)
```

`sam_item_id()` takes a vanilla name **or** your own `"namespace:item"`, so the same call
works for both. If you must compare a monster type, log `e.monster_type` once and read it
out of the log rather than guessing.

### Player index is not a uid

The other silent-failure trap, and the more common one. A **player index** is 0-3. A **uid**
identifies any entity in the world. They are both integers, nothing type-checks them, and
passing the wrong one gives you `nil` or somebody else's data rather than an error.

```lua
sam_get_position(e.player)                       -- wrong, silently
sam_get_position(sam_get_player_uid(e.player))   -- right
```

Which one a function wants:

| Wants a **player index** | Wants a **uid** |
|---|---|
| `sam_get_stat` `sam_set_stat` `sam_message` | `sam_get_position` `sam_set_position` |
| `sam_grant_item` `sam_get_inventory` | `sam_get_monster_stat` `sam_kill_monster` |
| `sam_apply_effect` `sam_has_effect` | `sam_monster_has_trait` `sam_monster_has_effect` |
| `sam_get_equipped_item_id` `sam_get_class` | `sam_spawn_monsters` (spawns *near* a uid) |

Two more that catch people:

- `sam_spawn_monster(x, y, "RAT")` takes **tile coordinates**. `sam_spawn_monsters` (plural)
  takes a **uid** to spawn near. Same prefix, different first argument.
- `sam_play_sound(76)` takes the **sound first**. There is no player argument.

Events differ too: `player.on_kill` gives you both `e.player` and `e.target_uid`, but
`on_before_monster_damage` gives you `e.monster_uid` and **no player at all** — the engine
doesn't record who swung. If your rule needs to know, capture it in `player.on_hit` and
stash it.

---

## Item traits

Barony decides what an item *is* from hardcoded lists of vanilla items. A custom item is
never on those lists, so it can't be a bow, hold arrows, or be thrown as a gadget — however
you configure it. Traits put it on the list.

```json
"traits": ["ranged"]
```

| Trait | Makes the item |
|---|---|
| `ranged` | Fire like a bow, train Ranged |
| `quiver` | Hold ammo |
| `foci` | A spell focus |
| `instrument` | Playable |
| `thrown_ball` | A throwable ball |
| `shield_slot` | Go in the shield hand |
| `potion_bad` | Recognised as a harmful potion |
| `automaton_food` | Edible by an automaton |
| `tinker_throwable` | Throwable/deployable, like a bomb or trap |
| `usable` | Offer a **Use** option in the inventory |
| `beatitude_ac` | Blessing or cursing it raises or lowers its AC. This is what a custom **mask** needs to gain armour from being blessed, and it also makes the mask breakable like a real one |

Traits make an item *recognised*. What it then does is up to your script — pair
`tinker_throwable` with `world.on_item_deployed` to build a real trap.

---

## Monster traits

Same idea. A custom monster is a variant of a vanilla creature, so it inherits that
creature's type — traits are how you say it's something else.

```json
"traits": ["boss", "never_retreat"]
```

| Trait | Makes the monster |
|---|---|
| `boss` | A boss: health bar, music, boss death handling |
| `trader` | Something you can shop with |
| `untargetable` | Invisible to autoaim, AI and tooltips — scenery and props |
| `immobile_turret` | Rooted in place, attacks from where it stands |
| `never_retreat` | Fight to the death. Also immune to Fear, Pacify and Cowardice |
| `water_walking` | Able to cross water **and lava** unharmed |
| `undead` | Vulnerable to smite and holy damage |
| `ally_recolour` | Take the ally tint even if its base race normally wouldn't |
| `tinker_construct` | A tinkering construct: salvageable, repairable, commandable like a robot |
| `no_digestion` | No gut. It never vomits and can't be made nauseous |
| `pass_through` | Walk straight through it, and it won't block pathing, but it stays targetable and killable |

**Before you tick `tinker_construct`:** a monster marked as a construct is worth **zero
experience** to kill. That's how the engine treats robots, and it applies to your monster
too. Fine for a summon or a turret, wrong for a boss.

---

## Giving a monster its own body

A custom monster is a variant of a vanilla creature, so by default it wears that creature's
model. Point it at one of your own `.vox` files instead:

```json
{
  "id": "mymod:rathalos",
  "base_type": "skeleton",
  "traits": ["boss", "never_retreat"],
  "body": { "model": "mymod:rathalos_body" }
}
```

The model id is one you declared in your mod's top-level `models` list:

```json
"models": [
  { "id": "mymod:rathalos_body", "file": "models/mymod/rathalos.vox" }
]
```

Note this is **not** how item models work. An item's `model` field takes a path, and the path
is its own id. A monster body needs a declared `ns:id`. See [custom-models.md](custom-models.md)
for the whole picture, and run `/sam_models` in game to see what actually registered.

It changes **only how the creature looks**. Its behaviour, AI, hitbox, speed and attacks all
still come from `base_type` — so choose the base whose movement suits you, and set the stats
you want. That is the honest trade: you get any body you can model, on top of an existing
creature's brain.

Three things worth knowing before you build around it:

- **Pick the base for its behaviour, not its looks.** A boss on a `rat` base moves like a rat.
- **In multiplayer, other players see the base creature.** The host sees your model. Nothing
  breaks, but model parity across the network is not in yet.
- **Limbs stay vanilla on humanoid bases.** On a base creature built from separate body parts,
  your model replaces the main body; the limbs still come from the base. Single-model bases
  (rat, spider, scarab and similar) have no limbs and swap cleanly.

There is no flight, and no way to replace a creature's AI. A monster that hovers (levitation
plus a raised position) and has scripted, telegraphed attacks reads as a flying boss, and that
is buildable today.

`ally_recolour` only does anything for base races that are excluded by default (human,
slime, and the robots). Every other monster already recolours.

A script reads them back with `sam_monster_has_trait(uid, "undead")`. That's how you write
a rule about a *kind* of monster rather than one specific monster — a custom boss is still
a skeleton as far as `monster_type` is concerned, so the type would match every skeleton in
the dungeon. The trait is the real question.

---

## Appearing in the world

An item only shows up in chests, shops and floor loot if it says what depth it belongs to:

```json
"level": 3
```

`-1` (the default) keeps it out of random generation entirely — which is what you want for
quest items and anything a class starts with.

---

## Your own pictures on screen

A mod can put its **own** art on the screen. Two lifetimes, because the two uses want
opposite things.

**An overlay** covers the view for a set time and then removes itself. This is the
jumpscare, the title card, the death splash. There is nothing to clean up.

**A HUD picture** stays where you put it until you clear it. This is the portrait, the
custom gauge, the marker.

Declare the picture once in `mod.json`:

```json
"images": [
  { "id": "mymod:jumpscare", "file": "art/jumpscare.png" }
]
```

Then use it:

```lua
-- full screen on player 0 for 900 milliseconds, then gone
sam_show_image(0, "mymod:jumpscare", 900)

-- placed, and it stays until you clear it
sam_hud_image("portrait", 24, 24, 96, 96, "mymod:jumpscare")
sam_hud_clear("portrait")
```

PNG and JPG both work. PNG keeps transparency. Around 1280x720 is a good size for a
full-screen overlay; pass `"contain"` as the last argument if you would rather letterbox
it than stretch it to the screen shape.

**Name a picture three ways**, tried in this order:

| What you pass | What it means |
|---|---|
| `"mymod:jumpscare"` | an id from any loaded mod's `images` list |
| `"jumpscare"` | the same id, with your own namespace assumed |
| `"art/jumpscare.png"` | a path inside your own mod folder |

The first two are worth preferring. A declared image is checked when the mod loads, so a
typo is one line in `sam_log.txt` at load time. A raw path can only fail at the moment you
try to draw it, which looks like nothing happening for no reason.

`sam_get_image_size(image)` gives you the picture's own pixel size, so you can centre or
scale it instead of hard-coding the numbers you exported at. It also returns nothing when
the picture cannot be loaded, which makes it the cheapest way to check a name resolves.

**In multiplayer** the picture appears on the screen of the player you named, even when
that player is on another machine. Scripts run on the host, so the host sends the image
**name** and the client draws it from its own copy of the mod. A client without the mod
draws nothing rather than desyncing.

---

## Two other things worth knowing

**Weapons need to say what they are.** A custom weapon with no `weapon_skill` trains no
skill, gets no damage variance, and its tooltip claims Sword. Set `"weapon_skill": "axe"`
(or `sword`, `mace`, `polearm`).

**Spellbooks and foci teach by attribute:**

```json
"attributes": { "spellbook_spell": 1 }
```

---

## Testing without playing

| Command | Answers |
|---|---|
| `/sam_items` | What's registered, and can it appear as loot? |
| `/sam_loot RING 100` | Does my item actually roll in the loot table? |
| `/sam_traits ns:item` | Which traits did this item really get? |
| `/sam_montraits` | What traits do nearby monsters have? |
| `/sam_spell ns:item` | What spell does this book teach? |
| `/sam_testhooks` | Fire the hooks directly and report what happened |
| `/sam_reload` | Re-read mods from disk (main menu only) |

Everything these print also goes to `sam_log.txt`, and so does `sam_log()` from your script.
The log is one file per session, with older runs archived in `sam_logs/`.

---

## A worked example

`examples/BoneHunter/` is a small complete mod that uses every capability on this page
exactly once, each in its own numbered section: a monster drop, an item that appears in
world loot, a thrown item that becomes a real trap, a rewritten damage number, a refused
equip, monster traits, and crafting at the Hunter's Workbench.

Copy the folder, change `namespace` in `mod.json`, delete the sections you don't want.

`examples/SkeletonScare/` is the smallest useful mod on this page: one skeleton runs you
down the corridor, and when it reaches you the mod's own picture takes the whole screen.
Replace `art/scare.png` with your own picture and it is your mod.
