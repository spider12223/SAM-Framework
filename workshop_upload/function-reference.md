# S.A.M function reference

Every script function the framework exposes: **184 functions** and **72 events**.
All of them work identically in Lua, JavaScript and TypeScript.

This page is generated from the API definition, so it cannot fall behind the code. If a
function is missing here it is missing from the framework.

**Host-only** means the call is refused on a multiplayer client, where it becomes a logged
no-op rather than a crash. Read the value on the host and send it on if a client needs it.

For guides and worked examples, see [scripting-reference.md](scripting-reference.md).

## Contents

- [Combat](#combat) (1)
- [Context](#context) (3)
- [Custom events](#custom-events) (2)
- [Damage](#damage) (2)
- [Entities](#entities) (2)
- [Game content](#game-content) (4)
- [HUD](#hud) (3)
- [Hooks](#hooks) (2)
- [Input](#input) (3)
- [Inventory](#inventory) (3)
- [Live patching](#live-patching) (6)
- [Logging](#logging) (2)
- [Mechanisms](#mechanisms) (4)
- [Monsters](#monsters) (21)
- [Multiplayer](#multiplayer) (3)
- [Networking](#networking) (1)
- [Panels](#panels) (16)
- [Persistence](#persistence) (10)
- [Pictures](#pictures) (10)
- [Player state](#player-state) (13)
- [Presentation](#presentation) (8)
- [Rewards](#rewards) (5)
- [Spells](#spells) (8)
- [Status effects](#status-effects) (9)
- [Terrain](#terrain) (8)
- [Timers](#timers) (3)
- [Truth](#truth) (13)
- [World](#world) (12)
- [Your own logic](#your-own-logic) (7)
- [Events](#events) (72)


## Combat

### `sam_spawn_projectile(tile_x, tile_y, angle, speed, damage, lifetime, model, owner)`

> Host-only.

Fire a moving projectile with its own speed, model, damage and lifetime. Until this the only thing a script could launch was a fixed vanilla spell, which ruled out ranged enemies with real attack patterns, telegraphed boss volleys, and weapons that fire anything but an arrow. It stops on the first thing it hits and fires an "on_projectile_hit" event with .projectile, .target, .x, .y and .damage — spawn a follow-up there for a burst or an explosion. Giving an owner stops the shot killing the player who fired it on its first frame. Leave model empty and the projectile is INVISIBLE, which is almost never what you want. Host-only, like every other world-mutating call. MULTIPLAYER: everything that matters is decided on the host, so damage, collisions and the hit event are correct for everyone — but a connected client has no behaviour for a custom projectile and only moves it when a position update arrives, about 8 times a second, so the flight looks stepped rather than smooth on their screen. Fine for a shot that crosses a room; noticeable on a slow, long-lived one.

| argument | type |
|---|---|
| `tile_x` | number (fractional tiles allowed) |
| `tile_y` | number |
| `angle` | number (radians — sam_get_facing returns one) |
| `speed` | number (world pixels per tick; must be > 0) |
| `damage` | int (optional, default 0) |
| `lifetime` | int ticks (optional, default 100 ≈ 2s, max 1000) |
| `model` | string (optional — a model from your mod's "models", or a vanilla model index) |
| `owner` | int player 0..3 (optional, default -1 = unowned) |

**Returns:** the projectile's entity uid (int), or nil/null if it could not be spawned


## Context

### `sam_get_level_info()`

Everything about the current floor. sam_get_floor returns a bare number that cannot tell a secret branch from the main one, so location-gated content was impossible before this.

**Returns:** table { floor, name, author, width, height, secret, skybox, no_digging, no_teleport, no_levitation }

### `sam_get_mods()`

Every S.A.M mod loaded right now. Cross-mod integration with zero engine work: soft-depend on another mod, avoid double registering, or light up extra content when a partner mod is present.

**Returns:** array of { ns, name, version, author }

### `sam_is_mod_loaded(namespace)`

Is a given mod namespace loaded? The cheap form of sam_get_mods.

| argument | type |
|---|---|
| `namespace` | string |

**Returns:** boolean


## Custom events

### `sam_fire_hook(name, event)`

> Host-only.

Fire a custom event to ALL Lua + JS/TS scripts cross-runtime. Only number/bool/string fields cross over; recursion capped at depth 8.

| argument | type |
|---|---|
| `name` | string |
| `event` | table |

**Returns:** the number of scripts the event reached (number)

### `sam_register_hook(name)`

Declare a namespaced custom hook. Name must contain a colon ("namespace:hook_name").

| argument | type |
|---|---|
| `name` | string |

**Returns:** nothing


## Damage

### `sam_deal_damage(entity_uid, amount)`

> Host-only.

Deal `amount` damage to any entity by UID (positive = damage); existence-validated.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `amount` | int |

**Returns:** true on success (boolean)

### `sam_modify_damage(player, new_value)`

> Host-only.

Rewrite incoming damage (clamped to >= 0). ONLY valid inside an on_before_damage callback.

| argument | type |
|---|---|
| `player` | int |
| `new_value` | int |

**Returns:** nothing


## Entities

### `sam_get_facing(player)`

Read which way a player is looking. 0 = +x (east), increasing toward +y — so the forward unit vector is (cos yaw, sin yaw) and 'behind' is yaw + π. Use it to place things relative to a player's facing (a marker in front, a follower behind) or to aim. Host-authoritative for remote players; a client always sees its own facing correctly.

| argument | type |
|---|---|
| `player` | int |

**Returns:** the player's facing yaw in radians in [0, 2π) (number), or nil/null for an absent player

### `sam_get_nearby_entities(player, radius)`

> Host-only.

List UIDs of monsters/players within `radius` tiles of a player (never raw pointers).

| argument | type |
|---|---|
| `player` | int |
| `radius` | number |

**Returns:** an array/table of creature UIDs (max 32)


## Game content

### `sam_get_item_info(item)`

Look up one item by type number or by name. The attributes sub-table is where a tooltip's numbers come from (ATK, AC and so on), so this is enough to render your own item description in a panel.

| argument | type |
|---|---|
| `item` | int item type, or a name / "ns:id" string |

**Returns:** { type, name, unidentified, category, level, weight, value, custom, attributes } or nil/null if unknown

### `sam_list_items(category)`

List every item the game knows about, including items added by mods (those have custom = true). This is what a recipe browser, a shop's stock list or a bestiary of loot is built from — before it, a script could only ask about the item already in the player's hand.

| argument | type |
|---|---|
| `category` | string (optional filter, e.g. "WEAPON"; omit for everything) |

**Returns:** array of { type, name, unidentified, category, level, weight, value, custom }

### `sam_list_monsters()`

List the game's monster types. Note what this does NOT include: a S.A.M custom monster is a variant of a base species rather than a new entry in the engine's table, so it will not appear here as its own row — you will see the species it is built on. The NOTHING sentinel and the engine's reserved padding slots are filtered out. Pair with sam_spawn_monster for an arena mod, or with a panel for a bestiary.

**Returns:** array of { type, name }

### `sam_list_spells()`

List the spells a player can actually be given, with their mana cost. Spells the game hides from its own UI are left out, so what you get back is the set that is meaningful to show a player.

**Returns:** array of { id, name, cost }


## HUD

### `sam_hud_bar(id, x, y, w, h, frac, color)`

Show or update a horizontal bar — a custom resource, a charge meter, a boss health track. frac is clamped to 0..1; 0 draws as empty rather than a sliver.

| argument | type |
|---|---|
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `frac` | number (0..1) |
| `color` | int (0xRRGGBBAA) |

**Returns:** true on success (boolean)

### `sam_hud_clear(id)`

Remove one HUD element. No id removes the whole script HUD. The HUD is also dropped automatically when the mod unloads, so it can never outlive the mod that drew it.

| argument | type |
|---|---|
| `id` | string |

**Returns:** true if that id was showing (boolean)

### `sam_hud_text(id, x, y, text, color)`

Show or update a line of text on screen. Calling again with the same id moves/retitles the existing line rather than stacking a new one.

| argument | type |
|---|---|
| `id` | string |
| `x` | int |
| `y` | int |
| `text` | string |
| `color` | int (0xRRGGBBAA) |

**Returns:** true on success (boolean)


## Hooks

### `sam_modify_monster_damage(newValue)`

> Host-only.

Rewrite the damage a MONSTER is about to take. Only valid inside an on_before_monster_damage callback. No subject argument: only one monster is ever mid-dispatch.

| argument | type |
|---|---|
| `newValue` | number |

**Returns:** true on success (boolean)

### `sam_modify_value(newValue)`

> Host-only.

Rewrite the number the engine is about to use, from inside any hook that offers one (XP gained, gold gained, and every future modifiable hook). Only valid inside such a callback — the error names the hook you ARE inside, so a wrong-place call says something useful.

| argument | type |
|---|---|
| `newValue` | number |

**Returns:** true on success (boolean)


## Input

### `sam_get_action_binding(player, action)`

What the player actually has an action bound to — use it to print a correct prompt instead of guessing a key.

| argument | type |
|---|---|
| `player` | int |
| `action` | string — one of: `Attack`, `Defend`, `Use`, `Cast Spell`, `Sneak`, `Hotbar Up / Select`, `Hotbar Down / Cancel`, `Hotbar Left`, `Hotbar Right`, `Call Out`, `Command NPC`, `Quick Turn` |

**Returns:** the physical input, e.g. "Mouse3" (string; nil/null if unbound)

### `sam_is_action_held(player, action)`

Check whether a BOUND action is held. Reads Barony's own binding, so it follows whatever the player rebound it to (and works with mouse buttons, which raw keys can't see). Local player only — input never leaves its machine.

| argument | type |
|---|---|
| `player` | int |
| `action` | string — one of: `Attack`, `Defend`, `Use`, `Cast Spell`, `Sneak`, `Hotbar Up / Select`, `Hotbar Down / Cancel`, `Hotbar Left`, `Hotbar Right`, `Call Out`, `Command NPC`, `Quick Turn` |

**Returns:** whether the action is active (boolean)

### `sam_is_key_held(key_name)`

Check whether a supported RAW key is currently held (A-Z, 0-9, F1-F12). Ignores the player's keybinds — prefer sam_is_action_held, which follows them.

| argument | type |
|---|---|
| `key_name` | string — one of: `A-Z`, `0-9`, `F1-F12` |

**Returns:** whether the key is down (boolean)


## Inventory

### `sam_get_equipped_item(player, slot)`

Get the item NAME equipped in a slot (ARMOR==BREASTPLATE, BOOTS==SHOES). Vanilla items only — it can't name a custom item, so use sam_get_equipped_item_id to test for one.

| argument | type |
|---|---|
| `player` | int |
| `slot` | string — one of: `WEAPON`, `SHIELD`, `HELMET`, `ARMOR`, `BREASTPLATE`, `GLOVES`, `BOOTS`, `SHOES`, `RING`, `AMULET`, `CLOAK`, `MASK` |

**Returns:** the item name (string; nil/null if slot empty)

### `sam_get_equipped_item_id(player, slot)`

Get the item ID equipped in a slot. Compare it against sam_item_id("namespace:item") to check whether YOUR custom item is equipped — the id is a number, so the name-returning version above can never match it.

| argument | type |
|---|---|
| `player` | int |
| `slot` | string — one of: `WEAPON`, `SHIELD`, `HELMET`, `ARMOR`, `BREASTPLATE`, `GLOVES`, `BOOTS`, `SHOES`, `RING`, `AMULET`, `CLOAK`, `MASK` |

**Returns:** the numeric item id (int; nil/null if slot empty)

### `sam_get_inventory_count(player, item_name)`

Count how many of an item (vanilla or custom name) a player holds.

| argument | type |
|---|---|
| `player` | int |
| `item_name` | string |

**Returns:** total count held (number)


## Live patching

### `sam_add_class_passive(class, effect)`

> Host-only.

Grant a class a permanent status effect at character creation (bakes at creation; run at mod-load).

| argument | type |
|---|---|
| `class` | any — one of: `classnum (int)`, `"namespace:class" (string)` |
| `effect` | any — one of: `EFF_ id (int)`, `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** true on success (boolean)

### `sam_patch_class(class, patch)`

Override a class's STARTING stats/skills (patch = { STR, DEX, ..., MAXHP, skills = {...} }). Per-machine — call on every peer in multiplayer; reverts on unload.

| argument | type |
|---|---|
| `class` | any — one of: `classnum (int)`, `"namespace:class" (string)` |
| `patch` | table |

**Returns:** true on success (boolean)

### `sam_patch_item(item, patch)`

Override an item type's base fields live: { weight, value/gold_value, level, category, slot, tooltip, name/name_identified, name_unidentified, attributes = {...} }.

| argument | type |
|---|---|
| `item` | any — one of: `item id (int)`, `vanilla name (string)`, `"ns:item" (string)` |
| `patch` | table |

**Returns:** true on success (boolean)

### `sam_patch_monster(monster, patch)`

> Host-only.

Override a monster type's base stats (e.g. { HP, MAXHP, STR }) for future spawns; also zero RANDOM_* for exact values.

| argument | type |
|---|---|
| `monster` | any — one of: `monster type id (int)`, `monster type name (string)` |
| `patch` | table |

**Returns:** true if any field applied (boolean)

### `sam_remove_class_passive(class, effect)`

> Host-only.

Remove a class passive effect previously added.

| argument | type |
|---|---|
| `class` | any — one of: `classnum (int)`, `"namespace:class" (string)` |
| `effect` | any — one of: `EFF_ id (int)`, `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** true on success (boolean)

### `sam_unpatch_class(class)`

Revert a class stat/skill patch.

| argument | type |
|---|---|
| `class` | any — one of: `classnum (int)`, `"namespace:class" (string)` |

**Returns:** true on success (boolean)


## Logging

### `sam_log(msg)`

Write a line to sam_log.txt (the only output channel). Also exposed as sam.log(msg) in Lua.

| argument | type |
|---|---|
| `msg` | string |

**Returns:** nothing

### `sam_message(player, text)`

> Host-only.

Show a line in a player's in-game message log.

| argument | type |
|---|---|
| `player` | int |
| `text` | string |

**Returns:** true on success (boolean)


## Mechanisms

### `sam_power_entity(uid, on)`

> Host-only.

Power a mechanism on or off, as a switch wired to it would.

| argument | type |
|---|---|
| `uid` | uid |
| `on` | boolean |

**Returns:** true on success (boolean)

### `sam_set_door(uid, open)`

> Host-only.

Open or close a door. Find one with sam_find_entities(x, y, r, "door").

| argument | type |
|---|---|
| `uid` | uid |
| `open` | boolean |

**Returns:** true on success (boolean)

### `sam_set_door_locked(uid, locked)`

> Host-only.

Lock or unlock a door.

| argument | type |
|---|---|
| `uid` | uid |
| `locked` | boolean |

**Returns:** true on success (boolean)

### `sam_toggle_switch(uid)`

> Host-only.

Flip a lever or switch, driving whatever it is wired to.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** true on success (boolean)


## Monsters

### `sam_apply_monster_effect(uid, effect, ticks)`

> Host-only.

Apply a status effect to a monster by UID for N ticks.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `ticks` | int |

**Returns:** true unless immune (boolean)

### `sam_get_monster_data(uid, key)`

Read per-monster scratch data (boss phases, etc.); in-memory, cleared on shutdown.

| argument | type |
|---|---|
| `uid` | uid |
| `key` | string |

**Returns:** the stored value, or nil/undefined

### `sam_get_monster_effect_duration(uid, effect)`

How many ticks of an effect a monster has left.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** remaining ticks (int; 0 inactive, -1 permanent)

### `sam_get_monster_effect_strength(uid, effect)`

A monster effect's strength/magnitude.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** strength/tier (int; 0 if inactive)

### `sam_get_monster_effects(uid)`

Every active effect on a monster at once (custom slots appear as "CUSTOM:<id>").

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** array/table of { name, ticks, strength }

### `sam_get_monster_stat(uid, stat)`

Read a monster's stat by UID. DEX aliases SPEED.

| argument | type |
|---|---|
| `uid` | uid |
| `stat` | string — one of: `STR`, `DEX`, `SPEED`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `LEVEL`, `LVL` |

**Returns:** the stat value (number; 0 if not a monster)

### `sam_get_monster_target(uid)`

Get the player index a monster is currently targeting (if any).

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** the targeted player index, or -1 (number)

### `sam_kill_monster(uid)`

> Host-only.

Kill a monster by UID (runs its normal death + drops; fires on_monster_died).

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** true on success (boolean)

### `sam_monster_attack(uid)`

> Host-only.

Make a monster swing immediately, using whatever attack pose its current weapon calls for.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** true on success (boolean)

### `sam_monster_charge(uid, ticks)`

> Host-only.

Send a monster into a straight-line charge for N ticks (50 = 1 second, default 50, max 500). Aims at its target if it has line of sight, otherwise charges along its current facing.

| argument | type |
|---|---|
| `uid` | uid |
| `ticks` | int |

**Returns:** true on success (boolean)

### `sam_monster_equip(uid, slot, item)`

> Host-only.

Put an item into a monster's equipment slot. Resolves a custom "ns:item" first and falls back to a vanilla item name. An unknown slot is refused and the valid list is logged.

| argument | type |
|---|---|
| `uid` | int |
| `slot` | string — one of: `helmet`, `breastplate`, `gloves`, `shoes`, `shield`, `weapon`, `cloak`, `amulet`, `ring`, `mask` |
| `item` | string ("ns:item" from your mod, or a vanilla item name) |

**Returns:** true on success (boolean)

### `sam_monster_face(uid, tileX, tileY)`

> Host-only.

Turn a monster to look at a tile. Aims at the tile centre. Pair it with sam_monster_charge to aim a charge.

| argument | type |
|---|---|
| `uid` | uid |
| `tileX` | int |
| `tileY` | int |

**Returns:** true on success (boolean)

### `sam_monster_has_effect(uid, effect)`

The monster counterpart of sam_has_effect — e.g. react when a monster you just hit is POISONED. Pass a monster UID (from a monster event or sam_get_nearby_entities).

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** whether the monster has the effect (boolean)

### `sam_monster_path_to(uid, tileX, tileY)`

> Host-only.

Path a monster to a tile using the engine's real pathfinder, then put it in the hunt state so it walks there. Tile coordinates, matching sam_get_position. Returns false when the destination is unreachable.

| argument | type |
|---|---|
| `uid` | uid |
| `tileX` | int |
| `tileY` | int |

**Returns:** true if a path was found (boolean)

### `sam_monster_unequip(uid, slot)`

> Host-only.

Empty one of a monster's equipment slots. Pairs with sam_monster_equip for disarm effects and for swapping a creature's loadout mid-fight.

| argument | type |
|---|---|
| `uid` | int |
| `slot` | string — one of: `helmet`, `breastplate`, `gloves`, `shoes`, `shield`, `weapon`, `cloak`, `amulet`, `ring`, `mask` |

**Returns:** true on success (boolean)

### `sam_remove_monster_effect(uid, effect)`

> Host-only.

Clear a status effect from a monster by UID — the monster counterpart of sam_remove_effect.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** true on success (boolean)

### `sam_set_monster_data(uid, key, value)`

Store any primitive/table value in a monster's scratch store (JSON-marshaled).

| argument | type |
|---|---|
| `uid` | uid |
| `key` | string |
| `value` | any |

**Returns:** true on success (boolean)

### `sam_set_monster_name(uid, name)`

> Host-only.

Rename a living monster. The name is what the player sees when targeting it and what appears in the obituary, so this is how a scripted boss or a named rare gets its title.

| argument | type |
|---|---|
| `uid` | int |
| `name` | string |

**Returns:** true on success (boolean)

### `sam_set_monster_stat(uid, stat, value)`

> Host-only.

Set a monster's stat by UID (bounded).

| argument | type |
|---|---|
| `uid` | uid |
| `stat` | string — one of: `STR`, `DEX`, `SPEED`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `LEVEL`, `LVL` |
| `value` | int |

**Returns:** true on success (boolean)

### `sam_set_monster_target(uid, player)`

> Host-only.

Make a monster acquire a player as its attack target.

| argument | type |
|---|---|
| `uid` | uid |
| `player` | int |

**Returns:** true on success (boolean)

### `sam_spawn_monsters(near_uid, monster_type, count)`

> Host-only.

Spawn `count` (1-8) monsters of a type near an anchor entity's UID.

| argument | type |
|---|---|
| `near_uid` | uid |
| `monster_type` | string |
| `count` | int |

**Returns:** the number actually spawned (number)


## Multiplayer

### `sam_is_host()`

Whether this machine is the host. Most functions are host-only and warn on a client; check this first instead of letting a client fill the log with refusals.

**Returns:** true on the host or in singleplayer (boolean)

### `sam_local_player()`

The player index THIS machine controls.

**Returns:** player index (int)

### `sam_player_count()`

How many players are actually connected right now.

**Returns:** connected players (int)


## Networking

### `sam_send_packet(target, tag, payload)`

Send a mod-defined message to another machine. Barony's packet ids are a fixed table, so before this a co-op mod had no way to tell the other side anything at all. On a client the target is ignored and the packet always goes to the host. The other side receives an "on_packet" event with .from, .tag and .payload. One datagram only — use sam_save_data for bulk state.

| argument | type |
|---|---|
| `target` | int (player 0..3, or -1 for every client) |
| `tag` | string |
| `payload` | string |

**Returns:** true if sent (boolean)


## Panels

### `sam_ui_button(panel, id, x, y, w, h, text)`

Put a clickable button in a panel. Clicking it fires a "ui.on_click" event whose .panel and .widget match what you passed here, so one handler can serve every button by switching on .widget. The panel must have been opened with modal = true or the player will have no cursor to click with.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `text` | string |

**Returns:** true, or false if that panel is not open (boolean)

### `sam_ui_clear(panel)`

Remove every widget from a panel but leave the panel itself open. This is how you rebuild a changing screen — clear, then re-declare the rows — without the window flickering shut and open again.

| argument | type |
|---|---|
| `panel` | string |

**Returns:** true, or false if the panel is not open (boolean)

### `sam_ui_close(panel)`

Close one panel, or every panel your mod has open if you pass nothing. Closing the last modal panel restores the player's camera control. Always close your panels on player.on_death and game.on_game_start so a leftover window cannot follow the player into the next run.

| argument | type |
|---|---|
| `panel` | string (optional — omit to close ALL of your mod's panels) |

**Returns:** true, or false if that panel was not open (boolean)

### `sam_ui_font(panel, id, font)`

Change the font of one widget, or of an entire panel by passing an empty id -- which is the only way to restyle a panel's text in one call rather than widget by widget. Panels default to a small 16px face because the game's standard 32px font makes any list look enormous. The number after the first # is the pixel size — raise it for a heading, and raise the row height to match if it is a list.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (a widget's id, or "" to set the whole panel's font) |
| `font` | string (a font path, e.g. "fonts/pixel_maz_multiline.ttf#16#2") |

**Returns:** true, or false if that panel or widget does not exist (boolean)

### `sam_ui_image(panel, id, x, y, w, h, image, color)`

Put one of your mod's pictures in a panel, scaled to w by h. Resolves the same way sam_show_image does. The colour argument tints the picture and its alpha fades it, so the same file can be reused greyed-out for a locked entry.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `image` | string ("ns:id", a bare name, or a path inside your mod) |
| `color` | colour (optional, default white = untinted) |

**Returns:** true, or false if the picture could not be resolved (boolean)

### `sam_ui_input(panel, id, x, y, w, h, text)`

Put an editable text box in a panel — a search field, a name entry, a price offer. Read what the player typed with sam_ui_input_text. Place the box clear of any label: a label wide enough to overlap the box will sit on top of it.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `text` | string (optional starting contents) |

**Returns:** true, or false if that panel is not open (boolean)

### `sam_ui_input_text(panel, id)`

Read what the player has typed into one of your text boxes. Poll it from a button handler, or from on_tick if you want a search list to filter as they type. Pressing Enter in the box also fires "ui.on_submit" with the text in .value.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (the input's id) |

**Returns:** the current contents (string), or "" if there is no such input

### `sam_ui_is_open(panel)`

Ask whether one of your panels is on screen. Useful to make a key or an item toggle a window instead of re-opening it, and to skip expensive refresh work while it is closed.

| argument | type |
|---|---|
| `panel` | string |

**Returns:** true if that panel is currently open (boolean)

### `sam_ui_label(panel, id, x, y, w, text, color)`

Put a line of text in a panel. x/y are measured from the panel's top-left corner, not the screen. Give w enough room for the text or it will be cut off — sam_ui_text_size measures a string before you place it. Re-declaring the same id replaces the text, which is how you update a running total.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (your id for this widget) |
| `x` | int (relative to the panel) |
| `y` | int |
| `w` | int |
| `text` | string |
| `color` | colour (optional, default warm parchment) |

**Returns:** true, or false if that panel is not open (boolean)

### `sam_ui_list(panel, id, x, y, w, h)`

Create an empty scrolling list in a panel. Fill it with sam_ui_list_add. This is the widget for a shop's stock, a bestiary, a recipe index or a quest log — anything with more entries than fit on screen.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |

**Returns:** true, or false if that panel is not open (boolean)

### `sam_ui_list_add(panel, id, row_id, text, color)`

Append one row to a list. Clicking a row fires "ui.on_select" with .panel, .widget set to the list and .value set to the row_id you chose here — so make row_id something you can act on, like an item id, rather than a display string.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (the list's id) |
| `row_id` | string (your id for this row) |
| `text` | string |
| `color` | colour (optional) |

**Returns:** true, or false if that panel or list does not exist (boolean)

### `sam_ui_list_clear(panel, id)`

Empty one list without touching the rest of the panel. Use this before re-filling a list from a search box or a filter, so the old results do not pile up under the new ones.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (the list's id) |

**Returns:** true, or false if that panel or list does not exist (boolean)

### `sam_ui_list_row_height(panel, id, pixels)`

Set how tall each row of a list is. Raise it if you switched that list to a larger font, or rows will overlap.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (the list's id) |
| `pixels` | int |

**Returns:** true, or false if that panel or list does not exist (boolean)

### `sam_ui_open(panel, x, y, w, h, title, modal)`

Open one of your mod's panels at a position and size given in VIRTUAL screen units (1280x720 at the default UI scale, not your monitor's pixels). modal = true frees the mouse cursor so the player can click your widgets, and hands camera control back when the panel closes — use it for anything with buttons. A non-modal panel is display-only and leaves the player in normal look-around mode. Opening a panel id that is already open re-positions it instead of opening a second one.

| argument | type |
|---|---|
| `panel` | string (your id for this panel) |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `title` | string (optional, "" for none) |
| `modal` | boolean (optional, default false) |

**Returns:** true if the panel opened (boolean)

### `sam_ui_panel_style(panel, background, border, border_width)`

Recolour a panel's background and border. Nothing about a panel's look is fixed by the framework — set the background fully transparent for a bare overlay, or opaque for a solid window. Colours accept the same forms as the HUD calls.

| argument | type |
|---|---|
| `panel` | string |
| `background` | colour (0 = leave unchanged) |
| `border` | colour (0 = leave unchanged) |
| `border_width` | int (optional, omit to leave unchanged) |

**Returns:** true, or false if that panel is not open (boolean)

### `sam_ui_text_size(text, font)`

Measure a string before you place it. This is how you lay a panel out properly instead of guessing: size a label to its own text so it cannot overlap the widget beside it, right-align a column of numbers, or centre a heading in a panel of known width.

| argument | type |
|---|---|
| `text` | string |
| `font` | string (optional; defaults to the standard panel face, NOT whatever font you set on a particular panel — this call takes no panel) |

**Returns:** width, height in pixels (two ints), or nil/null if the font could not be loaded


## Persistence

### `sam_delete_data(key)`

Delete a persisted per-mod key.

| argument | type |
|---|---|
| `key` | string |

**Returns:** true (boolean)

### `sam_get_player_data(player, key)`

Read back a per-player in-memory value set by sam_set_player_data.

| argument | type |
|---|---|
| `player` | int |
| `key` | string |

**Returns:** the stored value, or nil/undefined if unset

### `sam_list_data_keys()`

List the keys sam_save_data has written for your mod, so you can iterate stored state without having to remember every key name. Returns an empty table when nothing has been saved yet.

**Returns:** an array/table of every key your mod has saved

### `sam_load_data(key)`

Read back a persisted per-mod value.

| argument | type |
|---|---|
| `key` | string |

**Returns:** the stored value, or nil/undefined if unset

### `sam_save_data(key, value)`

Persist a value (number/string/bool/table) for the calling mod under savegames/sam_mod_data/<ns>/.

| argument | type |
|---|---|
| `key` | string |
| `value` | any |

**Returns:** true on success (boolean)

### `sam_set_player_data(player, key, value)`

Store a per-player value (number/string/bool/table) in memory for THIS session — the right tool for cooldowns, ability flags and stack counters you read often. Unlike sam_save_data it never touches disk and is cleared on a new game.

| argument | type |
|---|---|
| `player` | int |
| `key` | string |
| `value` | any |

**Returns:** nothing

### `sam_world_clear(key)`

Forget one key for the current character. The whole store is dropped automatically when a run ends, so you only need this to reset something mid-run.

| argument | type |
|---|---|
| `key` | string |

**Returns:** true if there was something to remove (boolean)

### `sam_world_keys()`

List the keys your mod has saved for this character. Handy for migrating an older save's data, or for showing the player what a mod is remembering about their run.

**Returns:** array of your mod's stored key names (strings)

### `sam_world_load(key)`

Read a value back from the current character's savegame. nil on a key you have never written is the signal that this is a fresh character — that is the natural place to run first-time setup, like anchoring a home floor.

| argument | type |
|---|---|
| `key` | string |

**Returns:** the stored value, or nil/null if this character never stored one

### `sam_world_save(key, value)`

Save a value inside the CURRENT character's savegame. A brand new character starts with none of it, so a hub's unlock flags, a quest's progress or a bank balance cannot leak from one run into the next. Deliberately size-capped (8KB per value, 64KB across all mods) because oversized save data can produce a savegame that fails to load — keep flags and counters here and keep items in a stash chest, which the game persists properly on its own.

| argument | type |
|---|---|
| `key` | string |
| `value` | any JSON-able value |

**Returns:** true if stored, false if a size limit was hit (boolean)


## Pictures

### `sam_clear_model(uid)`

> Host-only.

Drop a script-set model and go back to whatever the entity would otherwise draw. Clients are told too, so a transformation can end cleanly.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true on success (boolean)

### `sam_get_image_size(image)`

The picture's own pixel size, so a script can centre or scale it instead of hard-coding the numbers it was exported at. Also the cheapest way to check a picture actually resolves.

| argument | type |
|---|---|
| `image` | string |

**Returns:** width, height (two numbers in Lua; a [w, h] array in JS/TS; nil/null if it could not be loaded)

### `sam_get_model(uid)`

Read back the model ID a script set on this entity. Returns nil for an entity drawing its ordinary model.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** the model ID string, or nil if the entity has no script-set model

### `sam_hide_image(player)`

Take the overlay away early. No player clears every player's.

| argument | type |
|---|---|
| `player` | int |

**Returns:** true if something was showing (boolean)

### `sam_hud_image(id, x, y, w, h, image, color)`

A PERSISTENT picture in the script HUD — a portrait, a custom gauge, a marker. Stays until sam_hud_clear(id) or the mod unloads, unlike the overlay. w/h of 0 means the picture's own pixel size. The colour is MIXED into the art, so white (the default) leaves it untouched and the alpha byte fades it.

| argument | type |
|---|---|
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `image` | string |
| `color` | int (0xRRGGBBAA) |

**Returns:** true on success (boolean)

### `sam_set_model(uid, model_id)`

> Host-only.

Swap any entity's model while the game is running. What crosses the wire is the model ID, never an index, so machines with different mod orders still agree. This is what makes transformations, boss phases and damage states possible; before it, a model was fixed at spawn.

| argument | type |
|---|---|
| `uid` | int |
| `model_id` | string ("ns:model" from your mod's models list) |

**Returns:** true on success (boolean)

### `sam_set_scale(uid, scale)`

> Host-only.

Scale an entity. Clamped at 1.99 with a logged warning, because Barony quantises scale on the wire in 1/128 steps with a cap just under 2 — a larger value would look right to you and be invisible to everyone else.

| argument | type |
|---|---|
| `uid` | int |
| `scale` | number (1.0 is normal) |

**Returns:** true on success (boolean)

### `sam_set_visible(uid, visible)`

> Host-only.

Show or hide an entity. Refused, with a logged reason, on an entity that has a custom body: the draw pass deliberately keeps those visible, so hiding one this way would not work consistently. Clear the model first, or move it out of sight.

| argument | type |
|---|---|
| `uid` | int |
| `visible` | boolean (optional, defaults to false) |

**Returns:** true on success (boolean)

### `sam_show_image(player, image, duration_ms, alpha, fit)`

Cover a player's screen with one of the mod's pictures, over the world AND the HUD, for duration_ms (0 or omitted = until sam_hide_image). This is the jumpscare / title-card / death-splash layer: it removes itself, so there is nothing to clean up. alpha is 0..255 (default 255). "contain" keeps the picture's aspect ratio; "stretch" (default) fills the view. In multiplayer the host forwards the image NAME to the owning client, which draws it from its own copy of the mod.

| argument | type |
|---|---|
| `player` | int |
| `image` | string |
| `duration_ms` | int |
| `alpha` | int |
| `fit` | string — one of: `stretch`, `contain` |

**Returns:** true if the picture resolved (boolean)

### `sam_show_image_at(player, image, x, y, w, h, duration_ms, alpha)`

The same overlay, placed rather than full-screen. Coordinates are virtual screen pixels (the space sam_hud_text uses), so a fixed layout survives any resolution. w or h of 0 means the picture's own size on that axis. Still drawn over the HUD — for a picture that sits IN the HUD, use sam_hud_image.

| argument | type |
|---|---|
| `player` | int |
| `image` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `duration_ms` | int |
| `alpha` | int |

**Returns:** true if the picture resolved (boolean)


## Player state

### `sam_add_move_speed(player, delta)`

> Host-only.

Add to a player's move-speed multiplier (the result is clamped to [0.1, 3.0]). Additive counterpart to sam_set_move_speed — use it to stack a bonus onto whatever the multiplier already is (e.g. +0.1 on top of a 2.0 from another ability). Host-only; syncs to the owning client.

| argument | type |
|---|---|
| `player` | int |
| `delta` | number |

**Returns:** the new multiplier (number)

### `sam_get_class(player)`

Get a player's class name (vanilla or custom).

| argument | type |
|---|---|
| `player` | int |

**Returns:** the class name (string; nil/null if invalid)

### `sam_get_floor()`

Get the current floor/dungeon level.

**Returns:** the current dungeon level (number, 0-based)

### `sam_get_kills(player)`

Get the SAM-tracked per-player kill count for this session.

| argument | type |
|---|---|
| `player` | int |

**Returns:** kills this session (number)

### `sam_get_move_speed(player)`

Read a player's move-speed multiplier. Readable on clients.

| argument | type |
|---|---|
| `player` | int |

**Returns:** the multiplier (number; 1.0 if unset/invalid)

### `sam_get_race(player)`

Get a player's race: a custom race's "namespace:race" id, or the vanilla race name ("human", "skeleton", …). Use it in a race behavior script to gate logic to players of that race.

| argument | type |
|---|---|
| `player` | int |

**Returns:** the race id (string; nil/null if invalid)

### `sam_get_stat(player, stat)`

> Host-only.

Read a live player stat. Refused on a multiplayer client.

| argument | type |
|---|---|
| `player` | int |
| `stat` | string — one of: `STR`, `DEX`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `GOLD`, `HUNGER`, `LEVEL`, `LVL`, `EXP` |

**Returns:** the stat value (number; 0 on client/invalid)

### `sam_get_time_played()`

Get elapsed game ticks for the current run.

**Returns:** ticks since the run started (number, 50/sec)

### `sam_is_defending(player)`

Whether the player is actually blocking right now — the real engine state, not just the Defend button being down. Works for remote players in multiplayer.

| argument | type |
|---|---|
| `player` | int |

**Returns:** whether the player is blocking (boolean)

### `sam_level_up(player, count)`

> Host-only.

Level a player up count times (default 1) through the real engine path: attribute rolls, HP/MP gain, the level-up screen and sound, and full client sync — the actual benefits, unlike bumping LVL with sam_set_stat. Host-only. Fires the player.on_level_up hook once per level.

| argument | type |
|---|---|
| `player` | int |
| `count` | int |

**Returns:** true on success (boolean)

### `sam_play_sound(sound_id, vol)`

> Host-only.

Play a sound for all connected players. sound_id is a vanilla numeric index OR the "namespace:sound" id of a custom sound bundled in the mod. vol 0-255 (default 128).

| argument | type |
|---|---|
| `sound_id` | int|string |
| `vol` | int |

**Returns:** true on success (boolean)

### `sam_set_move_speed(player, mult)`

> Host-only.

Set a player's move-speed multiplier, clamped to [0.1, 3.0]. Host-only; syncs to the owning client. 1.0 is normal speed.

| argument | type |
|---|---|
| `player` | int |
| `mult` | number |

**Returns:** true on success (boolean)

### `sam_set_stat(player, stat, value)`

> Host-only.

Set a live player stat, bounded (HP never exceeds MAXHP, stats clamped, etc.). Syncs the change to the owning client.

| argument | type |
|---|---|
| `player` | int |
| `stat` | string — one of: `STR`, `DEX`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `GOLD`, `HUNGER`, `LEVEL`, `LVL`, `EXP` |
| `value` | int |

**Returns:** true on success (boolean)


## Presentation

### `sam_camera_shake(player, magnitude)`

Shake a player's camera. 1 is a nudge, ~10 a solid hit, 20+ violent. Feeds Barony's own shake channels so it decays naturally; for a remote client the host forwards it.

| argument | type |
|---|---|
| `player` | int |
| `magnitude` | number (~1..20) |

**Returns:** true if accepted (boolean)

### `sam_damage_number(uid, amount, type)`

> Host-only.

The floating combat number the game shows on a hit. Lets a mod's custom damage read like real damage instead of being invisible.

| argument | type |
|---|---|
| `uid` | uid |
| `amount` | int |
| `type` | int |

**Returns:** true on success (boolean)

### `sam_hitstop(duration_ms)`

> Host-only.

Briefly freeze enemy and projectile logic — a freeze-frame — for duration_ms (capped ~400). The player, HUD weapon and hand magic keep animating, so it reads as a punchy impact beat. SINGLEPLAYER ONLY: freezing host logic in a netgame would desync clients.

| argument | type |
|---|---|
| `duration_ms` | int |

**Returns:** true if accepted (boolean)

### `sam_impact_frame(player, r, g, b, intensity, duration_ms, lines)`

The EXAGGERATED version of the flash: a colour pop PLUS manga speed lines converging on screen centre PLUS a bright core flare. Pair it with sam_camera_shake and sam_hitstop for a full impact beat. lines is the speed-line count (0 = a plain flash).

| argument | type |
|---|---|
| `player` | int |
| `r` | int |
| `g` | int |
| `b` | int |
| `intensity` | number (0..1) |
| `duration_ms` | int |
| `lines` | int |

**Returns:** true if accepted (boolean)

### `sam_play_sound_at(sound, tileX, tileY, volume)`

> Host-only.

Positional audio: it attenuates with distance and pans, so a trap firing across the level is quiet, and in co-op each player hears it from where THEY are.

| argument | type |
|---|---|
| `sound` | int | string ("ns:sound") |
| `tileX` | int |
| `tileY` | int |
| `volume` | int |

**Returns:** true on success (boolean)

### `sam_play_sound_entity(sound, uid, volume)`

> Host-only.

The same, but the sound follows the entity as it moves.

| argument | type |
|---|---|
| `sound` | int | string ("ns:sound") |
| `uid` | uid |
| `volume` | int |

**Returns:** true on success (boolean)

### `sam_screen_flash(player, r, g, b, intensity, duration_ms)`

Flash a player's whole screen in an RGB colour that fades to nothing — the anime "impact frame". intensity 0..1 is the peak opacity. Drawn on the machine the player lives on.

| argument | type |
|---|---|
| `player` | int |
| `r` | int |
| `g` | int |
| `b` | int |
| `intensity` | number (0..1) |
| `duration_ms` | int |

**Returns:** true if accepted (boolean)

### `sam_spawn_particle(kind, tileX, tileY, z, scale)`

> Host-only.

A vanilla particle burst at a tile, so a mod's own effect looks like part of the game.

| argument | type |
|---|---|
| `kind` | string — one of: `poof`, `explosion`, `bang`, `sleep` |
| `tileX` | int |
| `tileY` | int |
| `z` | number |
| `scale` | number |

**Returns:** true on success (boolean)


## Rewards

### `sam_get_item_category(item)`

The category of an item (WEAPON / ARMOR / GEM / POTION / SCROLL / SPELLBOOK / …). Pass an event's item_type to react by category — e.g. reward the player for identifying any GEM.

| argument | type |
|---|---|
| `item` | any — one of: `numeric item id (e.g. an event's item_type)`, `vanilla name`, `"namespace:item"` |

**Returns:** the category name (string) e.g. "GEM", or nil/undefined if unknown

### `sam_grant_gold(player, amount)`

> Host-only.

Add gold to a player (clamped to >= 0), syncing the client HUD.

| argument | type |
|---|---|
| `player` | int |
| `amount` | int |

**Returns:** true on success (boolean)

### `sam_grant_item(player, item_name)`

> Host-only.

Give a vanilla item (e.g. "IRON_DAGGER") to a player. Local player only for now.

| argument | type |
|---|---|
| `player` | int |
| `item_name` | string |

**Returns:** true on success (boolean)

### `sam_item_id(name)`

Resolve an item's numeric type id — compare it against event fields like on_block's shield_type to react only to a specific item. Accepts a vanilla name or a custom "namespace:item".

| argument | type |
|---|---|
| `name` | string — one of: `vanilla ITEM name`, `"namespace:item" (custom)` |

**Returns:** the item's numeric type id (int), or nil/null if unknown

### `sam_spawn_item(x, y, item_name, status, beatitude, count)`

> Host-only.

Spawn a ground item at a map tile. status, beatitude and count let you put an item back exactly as you found it — without them a stash could record that you owned a cursed, worn ring and then only ever hand back a pristine one. The uid comes back so you can move it (sam_set_position) or clear it (sam_remove_entity) later; a uid is never 0, so an older `if sam_spawn_item(...)` check still behaves as it did.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |
| `item_name` | string (a vanilla name, or a custom "namespace:item") |
| `status` | int (optional, default EXCELLENT; clamped BROKEN..EXCELLENT) |
| `beatitude` | int (optional, default 0; negative is cursed, positive blessed; clamped -100..100) |
| `count` | int (optional, default 1; clamped 1..1000) |

**Returns:** the spawned item's entity uid (int), or nil/null if the tile was invalid


## Spells

### `sam_cast_spell(player, spell)`

> Host-only.

Immediately FIRE a spell/bolt from a player in the direction they face (free, no mana). Great for 'shoot on block'. Don't call from an on_spell_cast handler.

| argument | type |
|---|---|
| `player` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** true if a projectile spawned (boolean)

### `sam_cast_spell_at(player, target_uid, spell)`

> Host-only.

Fire a spell AIMED at an entity (aims the bolt toward it) instead of straight ahead. Free cast, host-only.

| argument | type |
|---|---|
| `player` | int |
| `target_uid` | uid |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** the missile's uid (int), or nil/null

### `sam_cast_spell_pos(player, tile_x, tile_y, spell)`

> Host-only.

Fire a spell aimed at a map tile. Free cast, host-only.

| argument | type |
|---|---|
| `player` | int |
| `tile_x` | int |
| `tile_y` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** the missile's uid (int), or nil/null

### `sam_get_spells(player)`

List the spells a player currently knows.

| argument | type |
|---|---|
| `player` | int |

**Returns:** array/table of spell internal-name strings

### `sam_grant_spell(player, spell)`

> Host-only.

Grant a spell to a player: a vanilla SPELL_ name, or a custom "namespace:spell".

| argument | type |
|---|---|
| `player` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** true on success (boolean)

### `sam_monster_cast_spell(uid, spell)`

> Host-only.

Make a monster (or a companion) cast a spell along its own facing. Free cast, host-only.

| argument | type |
|---|---|
| `uid` | uid |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** the missile's uid (int), or nil/null

### `sam_player_knows_spell(player, spell)`

Check whether a player already knows a spell (vanilla or custom).

| argument | type |
|---|---|
| `player` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** whether the player knows it (boolean)

### `sam_remove_spell(player, spell)`

> Host-only.

Un-learn a spell from a player's known list (local player). The counterpart to sam_grant_spell.

| argument | type |
|---|---|
| `player` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** true if it was known and removed (boolean)


## Status effects

### `sam_apply_effect(player, effect, ticks, strength)`

> Host-only.

Apply a status effect to a player for N ticks (50 ticks = 1s). Optional strength sets the tier/magnitude for effects that carry one (e.g. GROWTH stacks) — omit it for the plain default. Targets the player, never a monster.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `ticks` | int |
| `strength` | int |

**Returns:** true unless immune/refused (boolean)

### `sam_clear_effects(player)`

> Host-only.

Strip EVERY active status effect from a player at once — buffs and debuffs, vanilla and custom.

| argument | type |
|---|---|
| `player` | int |

**Returns:** how many effects were cleared (int)

### `sam_get_effect_duration(player, effect)`

How many ticks of an effect are left (50 = 1s) — so a debuff can scale or decay by time remaining. Readable on clients.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** remaining ticks (int; 0 if inactive, -1 if permanent)

### `sam_get_effect_strength(player, effect)`

The effect's strength/magnitude for effects that store one (GROWTH tiers, potion STR). Readable on clients.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** strength/tier (int; 0 if inactive)

### `sam_get_effects(player)`

Every active effect on a player at once — react to "any debuff" or strip all buffs without polling each effect by name. Custom pseudo-effect slots appear as "CUSTOM:<id>".

| argument | type |
|---|---|
| `player` | int |

**Returns:** array/table of { name, ticks, strength }

### `sam_has_effect(player, effect)`

Check whether a player currently has a status effect.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** whether the effect is active (boolean)

### `sam_remove_effect(player, effect)`

> Host-only.

Clear a status effect from a player.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** true on success (boolean)

### `sam_set_effect_duration(player, effect, ticks)`

> Host-only.

Retime an ALREADY-ACTIVE effect in place, without re-triggering it. No-op if the effect isn't active (never spawns a fresh one). 50 ticks = 1s.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `ticks` | int |

**Returns:** true if the effect was active and retimed (boolean)

### `sam_set_effect_strength(player, effect, strength)`

> Host-only.

Change the magnitude/tier of an ALREADY-ACTIVE effect while keeping its remaining duration. strength 1-255.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `strength` | int |

**Returns:** true if the effect was active and changed (boolean)


## Terrain

### `sam_find_entities(x, y, radiusTiles, kind)`

Entities of a KIND near a tile. This is the gap sam_get_nearby_entities leaves: that one skips anything which is not a monster or a player, so doors, chests, levers, gold and dropped items were invisible to scripts.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |
| `radiusTiles` | number |
| `kind` | string — one of: `any`, `door`, `chest`, `fountain`, `sink`, `switch`, `gate`, `ladder`, `portal`, `item`, `gold`, `boulder`, `monster`, `player` |

**Returns:** array of uids

### `sam_get_container_items(uid)`

> Host-only.

What is inside a chest, or what a creature is carrying.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** array of tables { type, name, count, status, beatitude, identified } or nil

### `sam_get_light_at(x, y, player)`

How lit a tile is, computed exactly the way the engine computes it, so the number you get back is the number monster vision thresholds on rather than an approximation of it. Barony keeps one SHARED lightmap holding light that is there for everyone (a wall torch, a lit room) plus one per camera that also holds that player's own glow. This reads the shared one by default, because that is the one the AI reads. Pass a player index if you want what that player's screen actually shows instead.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |
| `player` | int |

**Returns:** 0..255

### `sam_get_tile(x, y)`

Read one map tile. Liquid comes from the FLOOR tile, and the engine decides which tiles are liquid from their image filename — so a mod's own tile named "...lava..." reports as lava here too.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |

**Returns:** table { wall, floor, ceiling, solid, water, lava, walkable } or nil for a tile off the map

### `sam_is_spawnable(x, y)`

Is this a sane place to put something: in bounds, not inside a wall, not lava. Check before spawning instead of dropping a monster into rock.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |

**Returns:** boolean

### `sam_line_of_sight(x1, y1, x2, y2, blockedByEntities)`

Can a straight line get from A to B? This is the engine's own trace, so it agrees with what is drawn — unlike plain distance, which sees through solid rock.

| argument | type |
|---|---|
| `x1` | int |
| `y1` | int |
| `x2` | int |
| `y2` | int |
| `blockedByEntities` | boolean |

**Returns:** visible, blockedX, blockedY (blocked coords are -1 when visible)

### `sam_set_tile(x, y, layer, tileId)`

> Host-only.

Write one map tile — dig a passage, wall something in, flood a room. Refuses out of bounds rather than corrupting the map array. Check sam_tiles_connected afterwards if the edit could seal the exit.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |
| `layer` | int (0=floor, 1=wall, 2=ceiling) |
| `tileId` | int |

**Returns:** true on success (boolean)

### `sam_tiles_connected(x1, y1, x2, y2, flying)`

Can something WALK (or fly) from A to B at all? The softlock check: after a mod edits terrain, ask whether the exit is still reachable before committing.

| argument | type |
|---|---|
| `x1` | int |
| `y1` | int |
| `x2` | int |
| `y2` | int |
| `flying` | boolean |

**Returns:** boolean


## Timers

### `sam_cancel_timer(id)`

Cancel a pending timer by id (for the calling mod).

| argument | type |
|---|---|
| `id` | string |

**Returns:** nothing

### `sam_set_repeating_timer(id, interval_ticks, callback)`

Run `callback` (a function) every interval_ticks until cancelled. Ticks host-side.

| argument | type |
|---|---|
| `id` | string |
| `interval_ticks` | int |
| `callback` | any |

**Returns:** nothing

### `sam_set_timer(id, delay_ticks, callback)`

Run `callback` (a function) once after delay_ticks (50/sec). Replaces any timer with the same id. Ticks host-side.

| argument | type |
|---|---|
| `id` | string |
| `delay_ticks` | int |
| `callback` | any |

**Returns:** nothing


## Truth

### `sam_get_ac(uid)`

Armor class as the damage formula sees it, gear included.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** number

### `sam_get_effective_stat(uid, stat)`

A stat as the game actually uses it — gear, effects and curses folded in — rather than the raw number on the sheet.

| argument | type |
|---|---|
| `uid` | uid |
| `stat` | string — one of: `STR`, `DEX`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `GOLD`, `HUNGER`, `LEVEL`, `LVL`, `EXP` |

**Returns:** number

### `sam_get_flag(flag)`

Read a lobby setting the host chose at game start. Lets a mod adapt to the run it is actually in — skip a hunger mechanic when hunger is off, or scale difficulty when hardcore is on.

| argument | type |
|---|---|
| `flag` | string — one of: `cheats`, `friendlyfire`, `minotaurs`, `hunger`, `traps`, `hardcore`, `classic`, `keep_inventory`, `lifesaving`, `assist_items` |

**Returns:** true or false, or nil if the flag name is unknown (the valid list is logged)

### `sam_get_monster_name(uid)`

For a mod's custom monster this is the variant name it was given ("Rathalos"). A plain vanilla creature carries an empty variant name, so this falls back to the species name and never hands a script an empty string.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** the creature's DISPLAY name (string; nil if not a creature)

### `sam_get_monster_type(uid)`

Identify a creature by name instead of the raw integer in an event payload. NOTE this is the BASE type: a custom monster is a variant of a vanilla species, so a mod's "Rathalos" built on a bat answers "bat". Use sam_get_monster_name for the variant's own name, or sam_monster_has_trait to tell modded creatures apart.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** the species name, e.g. "skeleton" (string; nil if not a creature)

### `sam_get_seed()`

Read the seed identifying this run. Pair it with sam_random when you want per-run variety that every player still agrees on.

**Returns:** the run's unique game key (number)

### `sam_get_skill(uid, skill, effective)`

A proficiency rank. Accepts both spellings — "PRO_SWORD" (the class schema) and "sword" (what player.on_proficiency_increased hands you). effective (default true) includes the equipment bonus the game actually uses; pass false for the raw trained rank. Ranks were completely unreadable before this, even though the framework has always fired the event.

| argument | type |
|---|---|
| `uid` | uid |
| `skill` | string |
| `effective` | boolean |

**Returns:** 0..100

### `sam_is_enemy(uid_a, uid_b)`

Would these two fight? The engine's own allegiance answer, so charm, race and faction are all accounted for.

| argument | type |
|---|---|
| `uid_a` | uid |
| `uid_b` | uid |

**Returns:** boolean

### `sam_is_friend(uid_a, uid_b)`

The other side of sam_is_enemy — allies, followers and charmed creatures.

| argument | type |
|---|---|
| `uid_a` | uid |
| `uid_b` | uid |

**Returns:** boolean

### `sam_is_ghost(player)`

Whether a dead player is walking around as a ghost. Worth checking before granting items or applying effects, since a ghost is not an ordinary player.

| argument | type |
|---|---|
| `player` | int |

**Returns:** true if that player is currently a ghost (boolean)

### `sam_is_spirit_ghost(player)`

The stricter ghost test: a spirit ghost specifically, rather than any ghost state.

| argument | type |
|---|---|
| `player` | int |

**Returns:** true if that player is a spirit ghost (boolean)

### `sam_monster_has_trait(uid, trait)`

Reads back what the mod declared in JSON. Without this a mod can SAY a monster is undead and the engine agrees, but the mod's own script cannot ask — so a "bonus vs undead" rule had no way to test for undead. False for every vanilla monster, so it is a no-op without a mod.

| argument | type |
|---|---|
| `uid` | uid |
| `trait` | string |

**Returns:** boolean

### `sam_random(stream, min, max)`

Deterministic random drawn from a named stream owned by your mod. Same run seed plus same stream plus same draw order gives the same number on every machine, which ordinary random() cannot promise. Use it for anything that must agree across a multiplayer party.

| argument | type |
|---|---|
| `stream` | string (any name; each stream is independent) |
| `min` | int |
| `max` | int |

**Returns:** an integer in [min, max]


## World

### `sam_companion_punch(uid)`

> Host-only.

Make a companion THRUST forward for a few ticks — the punch motion. Call it repeatedly on a fast repeating timer (e.g. every 3 ticks) during an ability to read as a continuous ORA-ORA flurry. Purely visual on the companion itself; combine with sam_cast_spell (forward projectile + real damage) and/or sam_get_nearby_entities + sam_deal_damage for the hits.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true if uid is a live companion (boolean); false otherwise

### `sam_get_inventory(player)`

List a player's inventory. Use each item's uid with sam_remove_item. Empty list for an invalid player.

| argument | type |
|---|---|
| `player` | int |

**Returns:** a list of items, each { uid, type, name, count, beatitude, status, identified, equipped }

### `sam_get_player_uid(player)`

Get a player's entity uid, so the uid-based world-ops (get/set position) can act on that player's body.

| argument | type |
|---|---|
| `player` | int |

**Returns:** the player's entity uid (int), or nil/null if not in-game

### `sam_get_position(uid)`

Read any entity's map-tile position (player, monster or ground item). Get a player's uid with sam_get_player_uid.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** tile x, tile y (two values in Lua; an [x, y] array in JS), or nil/null if the uid is gone

### `sam_remove_entity(uid)`

> Host-only.

Remove a non-player world entity by uid — a sam_spawn_portal marker, a spawned monster, a companion, a ground item, etc. Refuses players (use the normal death/teleport paths for those). Frees any light the entity owned.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true on success (boolean); false for an unknown uid or a player

### `sam_remove_item(item_uid)`

> Host-only.

Remove a whole item stack from a player's inventory by its uid (from sam_get_inventory). Refuses an equipped item — unequip it first.

| argument | type |
|---|---|
| `item_uid` | int |

**Returns:** true on success (boolean); false if the item is missing or currently equipped

### `sam_set_chest_stash(chest_uid, on)`

> Host-only.

Turn an existing chest into permanent storage. Its contents then live in the player's savegame instead of on the floor, surviving descending, dying later, quitting and loading. This is the game's own void-chest storage, so the window, the networking and the save round-trip are all vanilla. Two limits worth designing around: every stash chest in a run shares ONE set of contents, and the chest window holds 12 stacks — so this is a stash, not a bank. Converting a chest that already holds loot hides that loot until you turn the stash back off; prefer converting an empty one.

| argument | type |
|---|---|
| `chest_uid` | int (from sam_find_entities with kind "chest", or the on_chest_opened event) |
| `on` | boolean (optional, default true) |

**Returns:** true if the chest was converted (boolean)

### `sam_set_position(uid, tile_x, tile_y)`

> Host-only.

Move an entity to a map tile. Players go through the safe teleport path (can't tunnel into walls); other entities are relocated and re-broadcast to clients.

| argument | type |
|---|---|
| `uid` | int |
| `tile_x` | int |
| `tile_y` | int |

**Returns:** true on success (boolean); false if refused (out of bounds, or a player teleport blocked by a wall)

### `sam_spawn_companion(player, model_id, scale)`

> Host-only.

Spawn a floating COMPANION (a JoJo-style "Stand" / familiar) that renders one of your custom .vox models and trails the player a short distance behind, with a gentle hover. Follows the player every frame and faces where they face. Optional scale (default 1.0, capped at 8) sizes the model. Drive the punch motion with sam_companion_punch, and clear it with sam_remove_entity. It's a decorative follower (PASSABLE, no AI, does no damage on its own — pair it with sam_cast_spell / sam_deal_damage for the actual attack). Host-only; not network-synced (host renders it). Re-spawn it on each new floor (entities are cleared on descent).

| argument | type |
|---|---|
| `player` | int |
| `model_id` | string — one of: `a registered custom model id, e.g. "mymod:star_platinum"` |
| `scale` | number |

**Returns:** the new companion's entity uid (int), or nil/null (bad player / unregistered model / off-host)

### `sam_spawn_monster(tile_x, tile_y, monster_name, shop_type)`

> Host-only.

Summon a monster at a map tile. "shopkeeper" makes a working shop; the optional shop_type (0-14) picks the store kind.

| argument | type |
|---|---|
| `tile_x` | int |
| `tile_y` | int |
| `monster_name` | string — one of: `vanilla monster name, e.g. "skeleton", "shopkeeper"` |
| `shop_type` | int |

**Returns:** the new monster's uid (int), or nil/null if the name is unknown or the tile is blocked

### `sam_spawn_portal(tile_x, tile_y)`

> Host-only.

Spawn a purely-DECORATIVE portal (the swirling vortex) at a map tile — it animates and glows but is never interactive and never sends anyone to the next floor. Walkable, so a player can stand on it. Returns the uid so you can move it (sam_set_position) or clear it (sam_remove_entity) — e.g. a portal-gun marker. Host-only. Multiplayer: the portal is host-authoritative and NOT network-synced, so only the host renders it — connected clients won't see it (your teleport/logic still runs host-side).

| argument | type |
|---|---|
| `tile_x` | int |
| `tile_y` | int |

**Returns:** the new portal's entity uid (int), or nil/null if the tile is out of bounds

### `sam_travel_to_level(floor, opts)`

> Host-only.

Send the party to any floor, including BACK UP, which the game otherwise never does — a ladder only ever counts upward, so before this no hub, home base or shop you walk back to was possible. The trip is deferred exactly as a ladder defers it, so it is safe to call from inside an event handler. Refused, with a logged reason, on a client, while another level change is already under way, or before a game has started. Nothing on the old floor is preserved: floors regenerate from the map seed, so put anything that must survive in a stash chest or in sam_world_save.

| argument | type |
|---|---|
| `floor` | int (absolute floor number, 0-100) |
| `opts` | table/object (optional) — { secret = true } reads the floor from the secret levels list |

**Returns:** true if the trip was accepted (boolean)


## Your own logic

### `sam_attach_behavior(uid, behavior)`

> Host-only.

Attach one of your registered behaviours to a live monster. It runs AFTER vanilla AI each frame rather than replacing it, so the creature still fights and paths normally and your code layers on top.

| argument | type |
|---|---|
| `uid` | int |
| `behavior` | string (a name you passed to sam_register_behavior) |

**Returns:** true on success (boolean)

### `sam_detach_behavior(uid)`

Remove whatever behaviour a script attached to this entity. Local bookkeeping, so it is safe to call anywhere and on a uid that has none.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true (boolean)

### `sam_get_entity_facing(uid)`

Read which way an entity is pointing. sam_get_facing takes a PLAYER index and reads where that player looks; this takes an entity uid, which is what a behaviour is handed. Feed it straight to sam_spawn_projectile to fire where the thing is aiming.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** the facing in radians (number), or nil/null for an unknown uid

### `sam_look_at(uid, target_uid)`

> Host-only.

Turn one entity to face another. This is the one a turret wants — it does the trigonometry so you do not have to. Because your behaviour owns the entity, the engine has no opinion about which way it points; you do. Refuses to turn a PLAYER: their facing belongs to whoever is holding the mouse, and a script fighting their input every frame would feel broken.

| argument | type |
|---|---|
| `uid` | int (the entity to turn) |
| `target_uid` | int (what to face) |

**Returns:** true if it turned (boolean)

### `sam_register_behavior(name, fn)`

> Host-only.

Give a name to a function that will BE an entity's brain. Barony runs every entity through a function pointer once per frame; this puts yours behind one. Your function is called with the entity's uid, once per frame, for every entity you spawned with that behaviour — and everything else in this reference is available inside it, so it can look around, move, shoot, damage, or open a window. Nothing about what it does comes from a list. Register at the top of your script rather than inside a handler, so the name exists before you spawn anything with it. Registering the same name twice replaces the function, and entities already in the world follow the new code. Behaviours are dropped when mods reload.

| argument | type |
|---|---|
| `name` | string ("behaviour", or "namespace:behaviour") |
| `fn` | function(uid) |

**Returns:** true if registered (boolean)

### `sam_set_entity_facing(uid, radians)`

> Host-only.

Point an entity at an angle. The primitive under sam_look_at, for when you are computing a direction yourself — a sweep, a spin, a lead on a moving target. The angle is normalised, so a behaviour that keeps adding to it will not drift out of range.

| argument | type |
|---|---|
| `uid` | int |
| `radians` | number (the same convention sam_get_facing returns and sam_spawn_projectile takes) |

**Returns:** true if it turned (boolean)

### `sam_spawn_entity(tile_x, tile_y, behaviour, model)`

> Host-only.

Put something in the world that runs your behaviour. This is the other half of sam_register_behavior: that one supplies the code, this gives it a body. The entity starts passable with no collision of its own, because your behaviour decides what it collides with. Leave model empty and it is invisible, which is almost never what you want. Host-only.

| argument | type |
|---|---|
| `tile_x` | number (fractional tiles allowed) |
| `tile_y` | number |
| `behaviour` | string (a name you registered) |
| `model` | string (optional — a model from your mod's "models", or a vanilla model index) |

**Returns:** the new entity's uid (int), or nil/null


## Events

Handle these in `on_event(e)`. Every script receives every event; check `e.name`.

### `<namespace>:<hook_name>`

Fires a script calls sam_fire_hook("namespace:name", event); delivered cross-runtime (Lua<->JS<->TS) to every loaded script's on_event.

| field | type |
|---|---|
| `name` | string |
| `<any user field>` | any |

### `game.on_game_end`

Fires the game is won or the party wipes.

| field | type |
|---|---|
| `player` | int |
| `won` | int |
| `floor_reached` | int |
| `kills` | int |
| `time_played` | int |

### `game.on_game_start`

Fires a new game begins.

| field | type |
|---|---|
| `player` | int |
| `class_id` | int |
| `class_name` | string |
| `race` | int |
| `race_name` | string |

### `game.on_level_entered`

Fires a floor finishes loading.

| field | type |
|---|---|
| `player` | int |
| `floor` | int |
| `level_name` | string |

### `on_action_pressed`

Fires a BOUND action goes down — e.g. the player presses whatever they have "Use" mapped to.

| field | type |
|---|---|
| `player` | int |
| `action` | string |
| `binding` | string |

### `on_action_released`

Fires a bound action goes back up.

| field | type |
|---|---|
| `player` | int |
| `action` | string |
| `binding` | string |

### `on_before_damage`

> Cancellable: return `false` to stop it.

Fires before a player's HP is reduced (bracketed around Entity::modHP).

| field | type |
|---|---|
| `player` | int |
| `damage` | int |

### `on_before_monster_damage`

> Cancellable: return `false` to stop it.

Fires before a monster's HP is reduced.

| field | type |
|---|---|
| `monster_uid` | int |
| `monster_type` | int |
| `damage` | int |

Rewrite the number with sam_modify_monster_damage(n). Set 0 to negate the hit entirely. Not cancellable by returning false.

### `on_key_pressed`

Fires a supported RAW key transitions to down (A-Z, 0-9, F1-F12).

| field | type |
|---|---|
| `player` | int |
| `key_name` | string |
| `held` | int |

### `on_key_released`

Fires a supported key transitions to up (A-Z, 0-9, F1-F12).

| field | type |
|---|---|
| `player` | int |
| `key_name` | string |

### `on_monster_damaged`

Fires any monster takes damage.

| field | type |
|---|---|
| `monster_uid` | uid |
| `monster_type` | int |
| `damage` | int |
| `hp` | int |
| `max_hp` | int |
| `killer_uid` | uid |
| `floor` | int |

### `on_monster_died`

Fires any monster dies (melee, ranged, magic, or scripted).

| field | type |
|---|---|
| `monster_uid` | uid |
| `monster_type` | int |
| `hp` | int |
| `max_hp` | int |
| `killer_uid` | uid |
| `floor` | int |

### `on_projectile_hit`

Fires a projectile from sam_spawn_projectile stops against a wall or an entity.

| field | type |
|---|---|
| `projectile` | int (the uid sam_spawn_projectile returned) |
| `target` | int (the uid it struck, or 0 for a wall) |
| `x` | int (tile) |
| `y` | int (tile) |
| `damage` | int (the damage the projectile was configured with) |

### `on_tick`

Fires every game tick (50/sec), for every script that defines on_tick(event).

| field | type |
|---|---|
| `tick_count` | int |
| `delta_ticks` | int |

### `player.on_attack_start`

Fires a player starts an attack swing (any weapon).

| field | type |
|---|---|
| `player` | int |
| `weapon_type` | int |
| `target_uid` | uid |

### `player.on_became_ghost`

Fires a dead player becomes a ghost.

| field | type |
|---|---|
| `player` | int |

Pairs with sam_is_ghost. Fires once on the transition, not every frame while dead.

### `player.on_before_equip`

> Cancellable: return `false` to stop it.

Fires before a player equips an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |

Return false to refuse the equip. Use it for class or race restrictions the vanilla slot rules cannot express.

### `player.on_before_item_pickup`

> Cancellable: return `false` to stop it.

Fires before an item the player walked over enters the inventory.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `item_count` | int |
| `category` | int |

Return false to refuse the pickup; the item stays on the floor. Only fires for genuine world pickups, never for starting gear or internal grants.

### `player.on_before_revive`

> Cancellable: return `false` to stop it.

Fires before a dead player is brought back.

| field | type |
|---|---|
| `player` | int |

Return false to refuse the revive. This is the hook for a permadeath rule or a resurrection cost.

### `player.on_bleed_tick`

Fires bleeding ticks damage on a player.

| field | type |
|---|---|
| `player` | int |
| `damage` | int |
| `stacks_remaining` | int |

### `player.on_block`

Fires a player blocks a hit while defending with a shield (partial or full).

| field | type |
|---|---|
| `player` | int |
| `shield_type` | int |
| `full_block` | int |
| `damage_taken` | int |
| `attacker_uid` | uid |
| `attacker_type` | int |
| `damage_blocked` | int |

### `player.on_callout`

Fires a player uses the callout / ping command.

| field | type |
|---|---|
| `player` | int |

A free player-driven input channel: a mod can treat a callout as a custom command without binding a key.

### `player.on_chest_opened`

Fires a player opens a chest.

| field | type |
|---|---|
| `player` | int |
| `chest_uid` | uid |
| `floor_x` | int |
| `floor_y` | int |

### `player.on_damage_taken`

Fires a player takes damage from any source.

| field | type |
|---|---|
| `player` | int |
| `damage` | int |
| `hp` | int |
| `maxhp` | int |
| `lethal` | int |
| `source_uid` | uid |
| `source_type` | int |

### `player.on_death`

Fires a player dies.

| field | type |
|---|---|
| `player` | int |
| `killer_type` | int |
| `killer_uid` | uid |
| `killer_monster` | int |
| `obituary` | string |

### `player.on_effect_applied`

Fires a status effect is newly applied to a player (a genuine off→on transition, not a refresh).

| field | type |
|---|---|
| `player` | int |
| `effect_name` | string |
| `duration_ticks` | int |
| `strength` | int |

### `player.on_effect_expired`

Fires a status effect runs out on a player.

| field | type |
|---|---|
| `player` | int |
| `effect` | int |

The counterpart to player.on_effect_applied. Use it to clean up anything the effect granted.

### `player.on_effect_removed`

Fires a status effect ends (cleared or expired).

| field | type |
|---|---|
| `player` | int |
| `effect_name` | string |
| `effect` | int |

### `player.on_equip`

Fires a player equips an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `slot` | string |

### `player.on_floor_change`

Fires a player descends to a new floor.

| field | type |
|---|---|
| `player` | int |
| `old_floor` | int |
| `new_floor` | int |

### `player.on_game_over`

Fires the run ends.

| field | type |
|---|---|
| `player` | int |

Your last chance to write per-run state with sam_save_data before the run is gone.

### `player.on_gold_collected`

Fires a player picks up gold.

| field | type |
|---|---|
| `player` | int |
| `amount` | int |
| `total_gold` | int |

### `player.on_hit`

Fires a player's melee weapon hits an entity.

| field | type |
|---|---|
| `player` | int |
| `target_uid` | uid |
| `target_type` | int |
| `damage` | int |
| `weapon_type` | int |
| `lethal` | int |

### `player.on_hunger_change`

Fires hunger crosses a tier edge.

| field | type |
|---|---|
| `player` | int |
| `hunger` | int |
| `hunger_level` | int |
| `old_hunger_level` | int |

### `player.on_item_bought`

Fires a player buys from a shop.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `gold_spent` | int |

### `player.on_item_broken`

Fires a player's equipped item breaks.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `slot` | string |

### `player.on_item_dropped`

Fires a player drops an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `floor_x` | int |
| `floor_y` | int |

### `player.on_item_identified`

Fires a player identifies an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `item_name` | string |

### `player.on_item_pickup`

Fires a player picks an item up off the ground.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `count` | int |
| `item_name` | string |

### `player.on_item_sold`

Fires a player sells to a shop.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `gold_received` | int |

### `player.on_item_use`

> Cancellable: return `false` to stop it.

Fires a player uses a consumable (potion / scroll / food).

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `item_count` | int |
| `category` | string |

### `player.on_kill`

Fires a player's melee blow kills an entity.

| field | type |
|---|---|
| `player` | int |
| `target_uid` | uid |
| `target_type` | int |
| `was_lethal` | int |

### `player.on_level_up`

Fires a player gains a level.

| field | type |
|---|---|
| `player` | int |
| `level` | int |
| `amount` | int |
| `stats` | int |

### `player.on_miss`

Fires a player's melee swing connects with nothing.

| field | type |
|---|---|
| `player` | int |
| `target_uid` | uid |
| `weapon_type` | int |

### `player.on_player_joined`

Fires a client joins the lobby.

| field | type |
|---|---|
| `player_index` | int |
| `player_name` | string |
| `class_id` | int |
| `race` | int |

### `player.on_player_left`

Fires a client disconnects or times out.

| field | type |
|---|---|
| `player_index` | int |
| `player_name` | string |

### `player.on_player_revived`

Fires a downed player is revived on a new floor.

| field | type |
|---|---|
| `player` | int |
| `revived_by` | int |
| `floor` | int |
| `revive_type` | int |

### `player.on_poison_tick`

Fires poison ticks damage on a player.

| field | type |
|---|---|
| `player` | int |
| `damage` | int |
| `stacks_remaining` | int |

### `player.on_proficiency_increased`

Fires a skill rank goes up.

| field | type |
|---|---|
| `player` | int |
| `proficiency` | int |
| `proficiency_name` | string |
| `old_rank` | int |
| `new_rank` | int |

### `player.on_shop_entered`

Fires a player opens trade with a shopkeeper.

| field | type |
|---|---|
| `player` | int |
| `shopkeeper_uid` | uid |

### `player.on_spell_cast`

> Cancellable: return `false` to stop it.

Fires a player casts a spell.

| field | type |
|---|---|
| `player` | int |
| `spell_id` | int |
| `spell_name` | string |
| `target_uid` | uid |

### `player.on_spell_failed`

Fires a cast fizzles or is blocked.

| field | type |
|---|---|
| `player` | int |
| `spell_id` | int |
| `spell_name` | string |
| `reason` | string |

### `player.on_spell_learned`

Fires a player learns a spell.

| field | type |
|---|---|
| `player` | int |
| `spell_id` | int |
| `spell_name` | string |

### `player.on_status_effect_tick`

Fires an active status effect ticks.

| field | type |
|---|---|
| `player` | int |
| `effect` | int |
| `effect_name` | string |
| `ticks_remaining` | int |

### `player.on_unequip`

Fires a player unequips an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `item_count` | int |
| `slot` | string |

### `player.on_xp_gained`

Fires a player gains XP from a kill.

| field | type |
|---|---|
| `player` | int |
| `amount` | int |
| `source_type` | string |
| `monster_type` | int |

The engine's one value-rewrite hook: set event.amount (or use sam_modify_value) and the engine adopts it. Set it to 0 to detach levelling from kills entirely.

### `ui.on_click`

Fires the player clicks a button you placed with sam_ui_button.

| field | type |
|---|---|
| `mod` | string (owning namespace) |
| `panel` | string |
| `widget` | string (the button id) |
| `value` | string (empty for a button) |

### `ui.on_select`

Fires the player clicks a row in a list you built with sam_ui_list / sam_ui_list_add.

| field | type |
|---|---|
| `mod` | string |
| `panel` | string |
| `widget` | string (the LIST's id, not the row's) |
| `value` | string (the row_id you passed to sam_ui_list_add) |

### `ui.on_submit`

Fires the player commits the contents of a text box placed with sam_ui_input.

| field | type |
|---|---|
| `mod` | string |
| `panel` | string |
| `widget` | string (the input id) |
| `value` | string (what they typed) |

### `world.on_before_chest_open`

> Cancellable: return `false` to stop it.

Fires before a chest opens.

| field | type |
|---|---|
| `player` | int |
| `chest_uid` | int |

Return false to keep the chest shut. Combine with sam_spawn_monsters for a mimic or an ambush.

### `world.on_boulder_triggered`

Fires a boulder trap launches.

| field | type |
|---|---|
| `floor_x` | int |
| `floor_y` | int |

### `world.on_chest_found`

Fires a player first walks up close to a chest (proximity — fires once per chest, NOT on opening it).

| field | type |
|---|---|
| `player` | int |
| `chest_uid` | uid |
| `floor_x` | int |
| `floor_y` | int |

### `world.on_door_opened`

Fires a player opens a wooden door.

| field | type |
|---|---|
| `player` | int |
| `door` | uid |
| `status` | int |
| `type` | string |

### `world.on_fountain_used`

Fires a player drinks from / uses a fountain.

| field | type |
|---|---|
| `player` | int |
| `fountain` | uid |
| `effect` | int |

### `world.on_item_deployed`

> Cancellable: return `false` to stop it.

Fires a thrown gadget lands and something must be built there.

| field | type |
|---|---|
| `item_type` | int |
| `player` | int |
| `x` | int |
| `y` | int |
| `status` | int |
| `beatitude` | int |

Return false after spawning your own thing, to skip the engine's built-in gadget list. This is how a mod makes custom traps and turrets.

### `world.on_monster_spawned`

Fires a monster is summoned at runtime.

| field | type |
|---|---|
| `monster_uid` | uid |
| `monster_type` | int |
| `monster_name` | string |
| `floor_x` | int |
| `floor_y` | int |
| `floor` | int |

### `world.on_orb_placed`

Fires a player places an orb on a pedestal.

| field | type |
|---|---|
| `player` | int |
| `pedestal` | uid |
| `orb_type` | int |
| `correct` | int |

### `world.on_projectile_hit`

Fires a fired projectile strikes an entity.

| field | type |
|---|---|
| `player` | int |
| `shooter_uid` | uid |
| `target_uid` | uid |
| `target_type` | int |

### `world.on_sink_used`

Fires a player uses a sink.

| field | type |
|---|---|
| `player` | int |
| `sink` | uid |
| `outcome_code` | int |
| `outcome` | string |

### `world.on_switch_toggled`

Fires a player flips a lever or switch.

| field | type |
|---|---|
| `player` | int |
| `switch` | uid |
| `state` | int |

### `world.on_teleport`

Fires a player uses a teleporter (pad or tunnel-spell).

| field | type |
|---|---|
| `player` | int |
| `teleporter` | uid |
| `type` | int |
| `dest_x` | int |
| `dest_y` | int |

### `world.on_trap_triggered`

Fires an arrow / spike / magic trap fires.

| field | type |
|---|---|
| `trap_type` | int |
| `player` | int |
| `floor_x` | int |
| `floor_y` | int |
| `damage` | int |
| `spell` | int |

