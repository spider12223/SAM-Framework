# S.A.M function reference

Every script function the framework exposes: **336 functions** and **87 events**.
All of them work identically in Lua, JavaScript and TypeScript.

This page is generated from the API definition, so it cannot fall behind the code. If a
function is missing here it is missing from the framework.

## Multiplayer

Your script's runtime code (its events, on_tick and timers) runs on the **host**. You name
players by index, and S.A.M carries each call to the machine it has to run on. Every function
below ends with a **Multiplayer:** line that starts with one of seven kinds. The game enforces
the kind, so the line is always what really happens:

| kind | what it means for your script |
|---|---|
| `host` (150) | Runs on the host, where your events and timers already run. A client's call is refused with a one-time warning. The result reaches every player. |
| `owner` (10) | Changes something that lives on the player's own machine (their backpack, their spells). Call it on the host; S.A.M carries it to that player's machine. |
| `screen` (29) | Shows something on one player's screen. Name the player, or leave it out inside an event about a player; -1 means every player. S.A.M carries it to their machine. |
| `read` (50) | Reads a player. The host can read everyone; a client can read only its own player. |
| `all` (15) | Changes a table every machine keeps (class and item patches, species resists). A host call runs everywhere and reaches players who join later. |
| `local` (26) | Answers for the machine running it: its clock, its files, its music. |
| `any` (56) | The same answer on every machine. Safe anywhere. |

Every event ends with a **Multiplayer:** line that says which machine it fires on and for
whom. The whole model, with examples, and how to test co-op on one computer:
[multiplayer.md](multiplayer.md).

For guides and worked examples, see [scripting-reference.md](scripting-reference.md).

## Contents

- [Camera](#camera) (8)
- [Combat](#combat) (20)
- [Context](#context) (13)
- [Custom events](#custom-events) (2)
- [Damage](#damage) (10)
- [Entities](#entities) (10)
- [Game content](#game-content) (5)
- [HUD](#hud) (3)
- [Hooks](#hooks) (2)
- [Input](#input) (4)
- [Inventory](#inventory) (30)
- [Live patching](#live-patching) (6)
- [Logging](#logging) (3)
- [Loot](#loot) (12)
- [Mechanisms](#mechanisms) (4)
- [Monsters](#monsters) (26)
- [Multiplayer](#multiplayer) (3)
- [Networking](#networking) (1)
- [Panels](#panels) (16)
- [Persistence](#persistence) (13)
- [Pictures](#pictures) (16)
- [Player state](#player-state) (13)
- [Presentation](#presentation) (9)
- [Rewards](#rewards) (5)
- [Rules](#rules) (15)
- [Settings](#settings) (4)
- [Sound & music](#sound-music) (6)
- [Spells](#spells) (9)
- [Status effects](#status-effects) (9)
- [Terrain](#terrain) (9)
- [Timers](#timers) (3)
- [Truth](#truth) (17)
- [World](#world) (23)
- [Your own logic](#your-own-logic) (7)
- [Events](#events) (87)


## Camera

### `sam_get_camera(player)`

Where the camera actually is, in the same tile units the setters take. mode is "vanilla", "orbit" or "absolute".

It reports what is ON SCREEN, not what you asked for. Those differ the moment the boom hits a wall, and a mod placing a camera has no other way to find out whether it got there.

| argument | type |
|---|---|
| `player` | int |

**Returns:** a table/object with x, y, height, yaw, pitch, mode, or nil

**Multiplayer:** `local`. Answers for the machine running the script. Given a player on another machine (`player`), it is refused with a one-time warning and returns nil (undefined in JavaScript). Returns nil for a player on another machine, and logs it once.

### `sam_reset_camera(player)`

Hand the camera back to the engine. Call it when your mod unloads, or the player keeps your camera.

| argument | type |
|---|---|
| `player` | int |

**Returns:** true if anything was being overridden (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_set_camera_angle(player, [yaw], [pitch])`

Point the camera somewhere fixed. Yaw is radians, the same way sam_get_facing reports them. Called with no angle it goes back to following the player's own look, which is what the mouse drives.

Pitch is positive DOWNWARD, which is Barony's convention throughout, and is held inside a right angle so the view cannot roll over — the engine clamps the player's own pitch for the same reason.

| argument | type |
|---|---|
| `player` | int |
| `yaw` *(optional)* | number |
| `pitch` *(optional)* | number |

**Returns:** true on success (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_set_camera_collision(player, on)`

Whether the boom shortens when a wall is between the player and the camera. On by default. Turn it off for a camera meant to pass through geometry.

Only WALLS shorten the boom. It walks the level's wall tiles from the player out to the camera, not the engine's line trace, because that trace also stops at creatures and a rat walking behind you would yank the camera into your back.

| argument | type |
|---|---|
| `player` | int |
| `on` | boolean |

**Returns:** true on success (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_set_camera_offset(player, back, [up], [right])`

Put the camera behind the player and keep it there: `back` tiles behind, `up` tiles above, `right` tiles to their right. It follows their yaw and pitch, so looking down swings the camera up and over rather than sliding along the floor, and the boom shortens when a wall is in the way. This is the whole of a third-person camera.

Pair it with sam_show_own_body(player, true) or you will be looking at the back of an invisible character. Barony's own /thirdperson only sets that visibility flag, and because both of the engine's camera writers skip a player who has it set, nothing then moves the camera at all, which is why it stays where you turned it on. This supplies the half that was missing. A camera belongs to the machine that draws it, so the host's call for a player on another machine is carried to that machine.

| argument | type |
|---|---|
| `player` | int |
| `back` | number |
| `up` *(optional)* | number |
| `right` *(optional)* | number |

**Returns:** true if the camera is now yours (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_set_camera_position(player, x, y, [height])`

Pin the camera in the world instead of on the player: a security camera, a cutscene, a fixed view of a room. x and y are tiles, height is tiles above the floor.

A standing eye is about 0.64 above the floor, and a room is one tile high, so 1.0 is the ceiling. It keeps looking wherever the player looks unless you also call sam_set_camera_angle or sam_set_camera_target, which is usually what a fixed camera wants.

| argument | type |
|---|---|
| `player` | int |
| `x` | number |
| `y` | number |
| `height` *(optional)* | number |

**Returns:** true if the camera is now yours (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_set_camera_target(player, uid)`

Keep the camera pointed at something, recomputed every frame so it tracks a moving subject. Pass 0 to stop.

If the subject dies the camera holds its last angle rather than snapping to a default, and stops tracking. Combine with sam_set_camera_position for a fixed camera that follows the action, or with sam_set_camera_offset for a lock-on.

| argument | type |
|---|---|
| `player` | int |
| `uid` | uid |

**Returns:** true on success (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_show_own_body(player, on)`

Whether you can see your own character. Normally you cannot: the camera is inside your head, so the engine hides your body and draws a first-person weapon instead. Turning this on shows the body and hides that weapon, which is what any camera outside the head needs.

This is the engine's own switch, the one /thirdperson flips, so it costs nothing and behaves exactly as the game already does. Refused while a player is on the death camera, which owns the same switch — taking it would strand them.

| argument | type |
|---|---|
| `player` | int |
| `on` | boolean |

**Returns:** true on success (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.


## Combat

### `sam_break_armor(entity_uid, [slot])`

Degrade a worn piece of armour, possibly breaking it. With no slot named, the engine's own picker chooses, so the odds and the exclusions match a real hit. player.on_item_broken has existed as an event with no verb able to cause it.

False is a real answer as well as a failure: the engine refuses shadows and liches outright, and refuses artifacts, quivers and anything preserved.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `slot` *(optional)* | string — one of: `helmet`, `breastplate`, `armor`, `gloves`, `boots`, `shoes`, `shield`, `cloak`, `mask` |

**Returns:** true if the piece degraded (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_consume_mp(entity_uid, amount)`

Spend mana only if the creature has it. Nothing is taken when it cannot afford the cost, which makes this the right one for a custom ability's cost.

One engine exception: a VAMPIRE player who cannot afford the cost has the shortfall taken out of health instead, and still gets true back. That is Barony's rule for vampires, not the framework's.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `amount` | int |

**Returns:** true if it could afford it and it was spent (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_drain_mp(entity_uid, amount, [notify])`

Take mana, and take anything you cannot afford out of HEALTH instead. That overdraw is the point — it is how a blood-magic cost is expressed.

This can kill. It has its own name rather than hiding inside sam_mod_mp behind a negative number for exactly that reason. Use sam_consume_mp when you want the safe version.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `amount` | int |
| `notify` *(optional)* | boolean |

**Returns:** true if it ran (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_get_attack(entity_uid)`

The melee attack figure the engine itself would use for this creature's next swing, weapon and stats included.

| argument | type |
|---|---|
| `entity_uid` | uid |

**Returns:** the melee attack value, or nil (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_bonus_attack_vs(entity_uid, target_uid)`

How much extra attack this creature gets against that particular target — slayer enchantments and the like.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `target_uid` | uid |

**Returns:** extra attack against that specific target, or nil (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_healring(entity_uid)`

The regeneration bonus this creature carries. It is what makes sam_get_regen_interval shorter.

| argument | type |
|---|---|
| `entity_uid` | uid |

**Returns:** the regeneration bonus from equipment and effects combined, or nil (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_hp(entity_uid)`

Read any creature's health by UID.

Fills a real gap: sam_get_stat takes a player INDEX and sam_get_monster_stat refuses anything that is not a monster, so a uid out of sam_find_entities could not reach another player's or a companion's health at all. nil rather than 0 for a door or a chest, because those have no health rather than none left.

| argument | type |
|---|---|
| `entity_uid` | uid |

**Returns:** current health, or nil for anything that is not a creature (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_max_hp(entity_uid)`

Read any creature's maximum health by UID.

| argument | type |
|---|---|
| `entity_uid` | uid |

**Returns:** maximum health, or nil (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_max_mp(entity_uid)`

Read any creature's maximum mana by UID.

| argument | type |
|---|---|
| `entity_uid` | uid |

**Returns:** maximum mana, or nil (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_mp(entity_uid)`

Read any creature's mana by UID.

The batch that added sam_mod_mp, sam_drain_mp and sam_consume_mp needs this: without it the only way to observe mana on anything but a player was to call one of those mutators and read what it returned, and a mutator is not a reader.

| argument | type |
|---|---|
| `entity_uid` | uid |

**Returns:** current mana, or nil for anything that is not a creature (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_ranged_attack(entity_uid, [quiver_bonus])`

The ranged attack figure for this creature, optionally including a quiver bonus.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `quiver_bonus` *(optional)* | int |

**Returns:** the ranged attack value, or nil (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_regen_interval(entity_uid)`

How often this creature regenerates health naturally. SMALLER is faster.

| argument | type |
|---|---|
| `entity_uid` | uid |

**Returns:** ticks between natural regeneration ticks, or nil (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript). For a client's own player it can miss the vampiric-aura bonus while more than 5 seconds of it remain (clients do not count effect timers); read it on the host for an exact value.

### `sam_get_thrown_attack(entity_uid)`

The attack figure this creature would apply to a thrown weapon.

| argument | type |
|---|---|
| `entity_uid` | uid |

**Returns:** the thrown attack value, or nil (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_is_parrying(player)`

Whether a player's parry window is currently open. The engine consumes this in melee resolution to produce parried damage; nothing exposed it before.

| argument | type |
|---|---|
| `player` | int |

**Returns:** true while the parry window is open (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_mod_mp(entity_uid, amount)`

Change a creature's mana by a relative amount. Negative takes it away.

Before this there was only the absolute sam_set_stat(player, "MP", n), so every "spend 5 mana" had to read, subtract and clamp by hand, and could not reach a monster.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `amount` | int |

**Returns:** the MP after the change, or nil (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_obituary(killer_uid, victim_uid, [from_spell])`

Give a scripted kill a proper death message and credit the killer. sam_kill_monster just sets health to 0, so today a scripted kill produces the generic message and nobody gets credit.

Call it AFTER the killing blow. Setting health rewrites the obituary to the generic string on every change, so calling it first would have it immediately overwritten.

| argument | type |
|---|---|
| `killer_uid` | uid |
| `victim_uid` | uid |
| `from_spell` *(optional)* | boolean |

**Returns:** true if it was recorded (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_revive_player(player, [x], [y])`

Bring a dead player back with half their health, at a tile you name or at their ghost's own position.

Revives any player in co-op whose game runs S.A.M: their own machine takes its ghost down. A player whose game does not run S.A.M is refused with a warning, because only their machine can do that, and returning true while they stayed dead would be worse than saying so. The gear the death dropped or bagged is taken back from the revived player, so nothing is duplicated.

| argument | type |
|---|---|
| `player` | int |
| `x` *(optional)* | int |
| `y` *(optional)* | int |

**Returns:** true if the player is back on their feet (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Revives any player in co-op whose game runs S.A.M; a player without S.A.M is refused with a warning, because only their own machine can take their ghost down. The gear the death dropped or bagged is removed from the revived player, so nothing is duplicated.

### `sam_set_defending(player, on)`

Put a player into or out of the blocking stance.

IT LASTS ONE FRAME. The engine writes this field from the player's block input every single frame, so your value is an override for the current frame and never a latch. Set it immediately before reading combat maths, or re-apply it from a per-frame handler. sam_is_defending has existed since v1.2 with no way to cause it, and this is that missing half with its real lifetime stated.

| argument | type |
|---|---|
| `player` | int |
| `on` | boolean |

**Returns:** true if it changed anything (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. On a player on another machine the override lasts exactly one host logic pass. It only changes the host's hit resolution: the player's own screen and the other players never show the raised shield.

### `sam_set_parry(player, ticks)`

Open a parry window for a number of ticks. 0 closes it.

Clamped to an hour. The engine counts this down, and a script that passed milliseconds by mistake would otherwise buy a permanent parry.

| argument | type |
|---|---|
| `player` | int |
| `ticks` | int |

**Returns:** true if set (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_spawn_projectile(tile_x, tile_y, angle, speed, [damage], [lifetime], [model], [owner])`

Fire a moving projectile with its own speed, model, damage and lifetime. Until this the only thing a script could launch was a fixed vanilla spell, which ruled out ranged enemies with real attack patterns, telegraphed boss volleys, and weapons that fire anything but an arrow. It stops on the first thing it hits and fires an "on_projectile_hit" event with .projectile, .target, .x, .y and .damage: spawn a follow-up there for a burst or an explosion. Giving an owner stops the shot killing the player who fired it on its first frame. Leave model empty and the projectile is INVISIBLE, which is almost never what you want.

| argument | type |
|---|---|
| `tile_x` | number (fractional tiles allowed) |
| `tile_y` | number |
| `angle` | number (radians — sam_get_facing returns one) |
| `speed` | number (world pixels per tick; must be > 0) |
| `damage` *(optional)* | int (optional, default 0) |
| `lifetime` *(optional)* | int ticks (optional, default 100 ≈ 2s, max 1000) |
| `model` *(optional)* | string (optional — a model from your mod's "models", or a vanilla model index) |
| `owner` *(optional)* | int player 0..3 (optional, default -1 = unowned) |

**Returns:** the projectile's entity uid (int), or nil/undefined if it could not be spawned

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Damage, collisions and the hit event are decided on the host, and every player sees the shot fly smoothly between network updates (it used to stand still between them).


## Context

### `sam_get_date()`

Today's date on this machine, which is how you make content that only appears at Halloween or over Christmas. Per-machine, so treat it as decoration rather than as a rule.

**Returns:** a table/object with year, month, day, hour, min, sec

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_get_fps()`

How fast this machine is drawing. Local to whoever asks, so never let it decide anything shared: two players will get different numbers and their games will disagree.

**Returns:** this machine's render rate (number)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_get_game_speed()`

How fast the world is running compared with real time, as set by sam_set_game_speed or /sam_gamespeed. 1.0 with nothing set, and always 1.0 in a netgame, where a speed cannot be set.

**Returns:** the simulation speed multiplier (number, 1.0 = normal)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_level_info()`

Everything about the current floor. sam_get_floor returns a bare number that cannot tell a secret branch from the main one, so location-gated content was impossible before this.

**Returns:** table { floor, name, author, width, height, secret, skybox, no_digging, no_teleport, no_levitation }

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_mods()`

Every S.A.M mod loaded right now. Cross-mod integration with zero engine work: soft-depend on another mod, avoid double registering, or light up extra content when a partner mod is present.

**Returns:** array of { ns, name, version, author }

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_get_real_time()`

This machine's wall clock. Same warning as sam_get_fps: two players' clocks differ, so this must not feed a dice roll or anything you save.

**Returns:** seconds since 1970 (number)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_get_run_time()`

The run clock the game itself displays. It stops during the intro, while you are dead, and while THIS machine has the game paused. Not the same as sam_get_time_played, which counts wall time since the program started, menus included. In multiplayer each machine counts its own, and pausing is local, so a player who spent a minute in their menu is a minute behind everyone else: read it on the host if a rule depends on it.

**Returns:** seconds of actual play this run (number)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_get_tick_rate()`

Logic frames per second. Barony's step is fixed, so there is no delta time to ask for; this is the constant every per-second conversion needs.

**Returns:** 50 (number)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_is_in_game()`

Whether a run is actually in progress. Worth checking at the top of a timer callback, which can otherwise fire while nobody is playing.

**Returns:** false while the main menu or intro is up (boolean)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_is_loading()`

Whether the game is mid-level-change. Entities are being destroyed and rebuilt during this, so it is the wrong moment to touch uids you were holding.

**Returns:** true during a level change (boolean)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_is_mod_loaded(namespace)`

Is a given mod namespace loaded? The cheap form of sam_get_mods.

| argument | type |
|---|---|
| `namespace` | string |

**Returns:** boolean

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_is_paused()`

Whether the player has the game paused. Each machine has its own answer in multiplayer, where the world keeps running for everyone else.

**Returns:** true if this machine has the game paused (boolean)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_set_game_speed(multiplier, [ticks])`

Singleplayer only. Run the simulation at multiplier times real time, 0.1 to 8. Everything counted in game ticks scales with it (monsters, hunger, effect durations, the run timer, your timers and on_tick); everything on a wall clock does not (rendering, the message feed, sam_hitstop, a screen flash, sam_get_real_time). ticks > 0 makes it temporary: after that many GAME ticks at the new speed it goes back to what it was, which is how a mod does bullet time on a critical hit (ticks = real_seconds * 50 * multiplier, so half a real second at 0.25x is 6). A window opened inside a window still returns to the original speed. Setting the value already set does nothing and fires nothing, so a preset key may call it every frame. 0, NaN and anything outside 0.1-8 are refused and change nothing. Above 1x is best effort: 8x needs every game tick to finish inside 2.5 ms, and a slower machine gets less than it asked for.

Mouse look turns once per game tick, so at 0.1x it steps five times a second; the engine has no per-frame look path that does not also move the body. The run timer, playtime and hunger count game seconds, not real ones. sam_hitstop is on the wall clock, so a 200 ms hitstop at 4x freezes 40 ticks of monster logic instead of 10. While the game is paused the multiplier does not reach the clock, so the pause menu runs at normal speed and a temporary window does not count down; the speed is kept and sam_get_game_speed still reports it.

| argument | type |
|---|---|
| `multiplier` | number |
| `ticks` *(optional)* | int |

**Returns:** true if accepted (boolean)

**Multiplayer:** `local`. Answers for the machine running the script. In a netgame it does nothing and says so once in the log: the host's world would run at Nx while every joiner's own body, moved by its own machine, stayed at 1x.


## Custom events

### `sam_fire_hook(name, [event])`

Fire a custom event to ALL Lua + JS/TS scripts cross-runtime. Only number/bool/string fields cross over; recursion capped at depth 8.

| argument | type |
|---|---|
| `name` | string |
| `event` *(optional)* | table |

**Returns:** the number of scripts the event reached (number)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.

### `sam_register_hook(name)`

Declare a namespaced custom hook. Name must contain a colon ("namespace:hook_name").

| argument | type |
|---|---|
| `name` | string |

**Returns:** nothing

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.


## Damage

### `sam_add_damage_multiplier(fraction)`

Contribute to the damage multiplier for the hit currently being resolved. 0.25 is +25%, -0.5 is half.

Valid ONLY inside an on_damage_multiplier handler, and it says so rather than dropping the number in silence. Positives ADD and negatives MULTIPLY, which is Barony's own rule: two mods each contributing +0.2 give +40%, and two each contributing -0.5 give a quarter rather than nothing.

| argument | type |
|---|---|
| `fraction` | number |

**Returns:** true if the contribution was taken (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_clear_species_damage_resist([species], [type])`

Undo sam_set_species_damage_resist. No arguments clears everything, a species alone clears every type for it.

| argument | type |
|---|---|
| `species` *(optional)* | string — one of: `human`, `rat`, `goblin`, `slime`, `troll`, `bat`, `spider`, `ghoul`, `skeleton`, `scorpion`, `imp`, `crab`, `gnome`, `demon`, `succubus`, `mimic`, `lich`, `minotaur`, `devil`, `shopkeeper`, `kobold`, `scarab`, `crystalgolem`, `incubus`, `vampire`, `shadow`, `cockatrice`, `insectoid`, `goatman`, `automaton`, `lichice`, `lichfire`, `sentrybot`, `spellbot`, `gyrobot`, `dummybot`, `bugbear`, `dryad`, `myconid`, `salamander`, `gremlin`, `revenant_skull`, `minimimic`, `monster_adorcised_weapon`, `flame_elemental`, `hologram`, `moth`, `earth_elemental`, `duck_small` |
| `type` *(optional)* | string — one of: `sword`, `mace`, `axe`, `polearm`, `ranged`, `magic`, `unarmed` |

**Returns:** how many overrides were removed (int)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns 0.

### `sam_deal_damage(entity_uid, amount)`

Deal `amount` damage to any entity by UID (positive = damage); existence-validated.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `amount` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_deal_damage_typed(entity_uid, amount, type)`

Deal damage of a particular weapon class, so the target's own resistance applies. 10 magic damage is 5 against something that halves magic and 20 against something that doubles it, without your script needing to know which. Both stages the engine applies are applied here, in its order: the species damage table, then live effects such as blood ward and sanctuary.

Barony's damage types are WEAPON CLASSES, not elements. There is no fire/ice/lightning axis anywhere in the damage funnel — the seven are sword, mace, axe, polearm, ranged, magic and unarmed. Returns 0 honestly when resistance eats the hit.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `amount` | int |
| `type` | string — one of: `sword`, `mace`, `axe`, `polearm`, `ranged`, `magic`, `unarmed` |

**Returns:** the damage actually dealt after resistance (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.

### `sam_get_damage_resist(entity_uid, [type])`

How much of a given weapon class this creature actually takes: 1.0 normal, 0.5 half, 2.0 double. Equipment, effects and magic resistance are all included — it is the same call the character sheet makes to draw the number a player sees. Defaults to "magic".

Needs the target's stats, so it is host-only for monsters. Reading it for your own player on a client is safe.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `type` *(optional)* | string — one of: `sword`, `mace`, `axe`, `polearm`, `ranged`, `magic`, `unarmed` |

**Returns:** the damage multiplier this creature takes, or nil (number)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_magic_resist(entity_uid)`

The raw magic-resistance point count. Each point is a separate reduction: this is the input, sam_get_damage_resist(uid, "magic") is the result.

| argument | type |
|---|---|
| `entity_uid` | uid |

**Returns:** magic resistance POINTS, or nil (int)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`entity_uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_heal(entity_uid, amount)`

Restore health to any player or monster by UID. Returns what actually landed, not what you asked for: health is clamped to the maximum, so a 50-point heal on something three short of full restores three.

sam_deal_damage cannot do this. It forces its amount negative, so both signs damage — before this the only way to heal was an absolute sam_set_stat write, which does not exist for monsters.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `amount` | int |

**Returns:** the HP actually restored, or nil if the uid is not a creature (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_modify_damage(player, new_value)`

Rewrite incoming damage (clamped to >= 0). ONLY valid inside an on_before_damage callback.

| argument | type |
|---|---|
| `player` | int |
| `new_value` | int |

**Returns:** nothing

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_preview_damage(attacker_uid, target_uid)`

Work out what one creature's melee swing would do to another, dealing nothing. Built from the same three terms the real melee path combines: attack, the target's AC effectiveness, and its AC.

A preview, not a promise. The real swing then folds in weapon multipliers, backstab and capstone bonuses, so it usually lands higher. It is NOT bounded by sam_get_attack either: AC has no floor in Barony, so against a target in cursed armour or under DISRUPTED the preview legitimately comes out above the attacker's raw attack.

| argument | type |
|---|---|
| `attacker_uid` | uid |
| `target_uid` | uid |

**Returns:** what a melee swing would deal right now, or nil (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_set_species_damage_resist(species, type, multiplier)`

Change how much of a weapon class an entire species takes. 1.0 normal, 0.5 halves it, 2.0 doubles it. Every creature of that species, now and later.

IT CANNOT GRANT IMMUNITY. Your number only seeds the multiplier; the engine then runs its own bonus pool over it and floors the result at 0.1, so the least any species can be made to take is a TENTH, not none. Pass 0 and you get 0.1. For real immunity use sam_set_damage_immune, or veto on_before_damage or on_damage_multiplier. Every S.A.M player's machine gets the same table (a client's character sheet reads its own copy), and a player who joins later gets the host's whole table. Cleared when mods unload.

| argument | type |
|---|---|
| `species` | string — one of: `human`, `rat`, `goblin`, `slime`, `troll`, `bat`, `spider`, `ghoul`, `skeleton`, `scorpion`, `imp`, `crab`, `gnome`, `demon`, `succubus`, `mimic`, `lich`, `minotaur`, `devil`, `shopkeeper`, `kobold`, `scarab`, `crystalgolem`, `incubus`, `vampire`, `shadow`, `cockatrice`, `insectoid`, `goatman`, `automaton`, `lichice`, `lichfire`, `sentrybot`, `spellbot`, `gyrobot`, `dummybot`, `bugbear`, `dryad`, `myconid`, `salamander`, `gremlin`, `revenant_skull`, `minimimic`, `monster_adorcised_weapon`, `flame_elemental`, `hologram`, `moth`, `earth_elemental`, `duck_small` |
| `type` | string — one of: `sword`, `mace`, `axe`, `polearm`, `ranged`, `magic`, `unarmed` |
| `multiplier` | number |

**Returns:** true if it took (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false.


## Entities

### `sam_get_distance(uid_a, uid_b)`

Distance between two entities on the FLOOR PLANE. Height is ignored, so a bat hovering directly overhead reads as zero tiles away. That matches how the game's own range checks work, which is why it is not corrected for here. Use this rather than working it out from sam_get_position, which rounds to whole tiles and so is wrong by up to a tile in each axis.

| argument | type |
|---|---|
| `uid_a` | int |
| `uid_b` | int |

**Returns:** distance in tiles (number), or nil if either entity is gone

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.

### `sam_get_distance_to(uid, x, y)`

Distance from an entity to a tile, measured to the centre of that tile, which is where the game places things.

| argument | type |
|---|---|
| `uid` | int |
| `x` | int (tile) |
| `y` | int (tile) |

**Returns:** distance in tiles (number), or nil

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.

### `sam_get_entity_size(uid)`

The entity's collision box. Any overlap test or aim cone written in script needs this, and it was not readable before. In JavaScript this returns an array.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** sizex, sizey in pixels (numbers), or nil

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.

### `sam_get_entity_sprite(uid)`

Which model an entity is currently drawing. Pairs with sam_get_model, which reports only a model your own script set.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** the model index (number), or nil

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.

### `sam_get_entity_ticks(uid)`

Age of an entity in frames. Divide by sam_get_tick_rate for seconds. Useful for despawning your own spawns after a while without keeping a table of them.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** frames this entity has existed (number), or nil

**Multiplayer:** `local`. Answers for the machine running the script. Each machine answers from its own copy. Each machine counts separately, and a client's count starts when that client first received the entity. Use the host's value for timing.

### `sam_get_entity_type(uid)`

What kind of thing a uid refers to. Lets one handler deal with a mixed list of uids without guessing from what other calls happen to succeed. Every word it returns is one sam_find_entities accepts, so you can read a kind and then go looking for more of the same.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** one of ENTITY_KINDS (string), or nil

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.

### `sam_get_facing(player)`

Read which way a player is looking. 0 = +x (east), increasing toward +y — so the forward unit vector is (cos yaw, sin yaw) and 'behind' is yaw + π. Use it to place things relative to a player's facing (a marker in front, a follower behind) or to aim. Host-authoritative for remote players; a client always sees its own facing correctly.

| argument | type |
|---|---|
| `player` | int |

**Returns:** the player's facing yaw in radians in [0, 2π) (number), or nil/undefined for an absent player

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.

### `sam_get_nearby_entities(player, radius)`

List UIDs of monsters/players within `radius` tiles of a player (never raw pointers). In JavaScript both arguments are required.

| argument | type |
|---|---|
| `player` | int |
| `radius` | number |

**Returns:** an array/table of creature UIDs (max 32)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Works on a client too (it used to return an empty table there). Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.

### `sam_get_position_precise(uid)`

Exact position, including the sub-tile fraction and the z axis. sam_get_position rounds to a whole tile and drops z entirely, so nothing in script could tell a flying bat from a rat standing underneath it, or two creatures sharing one tile. In JavaScript this returns an array [x, y, z].

| argument | type |
|---|---|
| `uid` | int |

**Returns:** x, y, z in world pixels (fractional), or nil

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.

### `sam_get_velocity(uid)`

How fast something is moving and in what direction. Enough to lead a moving target, or to tell a charging monster from a standing one. In JavaScript this returns an array.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** vx, vy, vz in pixels per tick (numbers), or nil

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.


## Game content

### `sam_get_food_satiation(item_type)`

How filling a food is. Takes an item TYPE rather than a uid, so you can price food your mod has not spawned yet. Vanilla foods only: the engine's table has no entry for custom items, so your own food answers 0.

| argument | type |
|---|---|
| `item_type` | int |

**Returns:** hunger restored (number), or nil for an unknown type

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_item_info(item)`

Look up one item by type number or by name. The attributes sub-table is where a tooltip's numbers come from (ATK, AC and so on), so this is enough to render your own item description in a panel.

| argument | type |
|---|---|
| `item` | int item type, or a name / "ns:id" string |

**Returns:** { type, name, unidentified, category, level, weight, value, custom, attributes } or nil/undefined if unknown

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_list_items([category])`

List every item the game knows about, including items added by mods (those have custom = true). This is what a recipe browser, a shop's stock list or a bestiary of loot is built from. The name it gives you is accepted by sam_grant_item, sam_spawn_item, sam_item_id and the rest, so listing and then granting works; if two items happen to share a displayed name the call refuses and names both rather than guessing.

| argument | type |
|---|---|
| `category` *(optional)* | string (optional filter, e.g. "WEAPON"; omit for everything) |

**Returns:** array of { type, name, unidentified, category, level, weight, value, custom }

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_list_monsters()`

List the game's monster types. Note what this does NOT include: a S.A.M custom monster is a variant of a base species rather than a new entry in the engine's table, so it will not appear here as its own row — you will see the species it is built on. The NOTHING sentinel and the engine's reserved padding slots are filtered out. Pair with sam_spawn_monster for an arena mod, or with a panel for a bestiary.

**Returns:** array of { type, name }

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_list_spells()`

List the spells a player can actually be given, with their mana cost. Spells the game hides from its own UI are left out, so what you get back is the set that is meaningful to show a player.

**Returns:** array of { id, name, cost }

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.


## HUD

### `sam_hud_bar(id, x, y, w, h, frac, [color], [player])`

Show or update a horizontal bar — a custom resource, a charge meter, a boss health track. frac is clamped to 0..1; 0 draws as empty rather than a sliver.

| argument | type |
|---|---|
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `frac` | number (0..1) |
| `color` *(optional)* | int (0xRRGGBBAA) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true on success (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_hud_clear([id], [player])`

Remove one HUD element. No id removes the whole script HUD: sam_hud_clear(nil, p) clears everything on one player's screen. The HUD is also dropped automatically when the mod unloads, so it can never outlive the mod that drew it.

| argument | type |
|---|---|
| `id` *(optional)* | string |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true if that id was showing (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_hud_text(id, x, y, text, [color], [player])`

Show or update a line of text on screen. Calling again with the same id moves/retitles the existing line rather than stacking a new one.

| argument | type |
|---|---|
| `id` | string |
| `x` | int |
| `y` | int |
| `text` | string |
| `color` *(optional)* | int (0xRRGGBBAA) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true on success (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message. To name a player without a colour, pass nil (null in JS) for the colour.


## Hooks

### `sam_modify_monster_damage(newValue)`

Rewrite the damage a MONSTER is about to take. Only valid inside an on_before_monster_damage callback. No subject argument: only one monster is ever mid-dispatch.

| argument | type |
|---|---|
| `newValue` | number |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_modify_value(newValue)`

Rewrite the number the engine is about to use, from inside any hook that offers one (player.on_xp_gained today, and every future modifiable hook; player.on_gold_collected offers no value to rewrite). Only valid inside such a callback: the error names the hook you ARE inside, so a wrong-place call says something useful.

| argument | type |
|---|---|
| `newValue` | number |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.


## Input

### `sam_get_action_binding(player, action)`

What the player actually has an action bound to — use it to print a correct prompt instead of guessing a key.

| argument | type |
|---|---|
| `player` | int |
| `action` | string — one of: `Attack`, `Defend`, `Use`, `Cast Spell`, `Sneak`, `Hotbar Up / Select`, `Hotbar Down / Cancel`, `Hotbar Left`, `Hotbar Right`, `Call Out`, `Command NPC`, `Quick Turn` |

**Returns:** the physical input, e.g. "Mouse3" (string; nil/undefined if unbound)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine answers with the binding their game reported (nil until it has).

### `sam_is_action_held(player, action)`

Check whether a BOUND action is held. Reads Barony's own binding, so it follows whatever the player rebound it to (and works with mouse buttons, which raw keys can't see).

| argument | type |
|---|---|
| `player` | int |
| `action` | string — one of: `Attack`, `Defend`, `Use`, `Cast Spell`, `Sneak`, `Hotbar Up / Select`, `Hotbar Down / Cancel`, `Hotbar Left`, `Hotbar Right`, `Call Out`, `Command NPC`, `Quick Turn` |

**Returns:** whether the action is active (boolean)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. On the host it works for every player: a joiner's game reports its buttons (false for a player whose game does not run S.A.M).

### `sam_is_key_held(key_name, [player])`

Check whether a RAW key is currently held. Takes any key name the game itself uses, so what sam_get_action_binding hands back works here: single letters and digits, F1 to F12, and the spelled-out keys such as "Space", "Return", "Escape" and "Left Shift". A MOUSE binding has no key behind it and cannot be answered here; it says so in the log rather than returning false forever. Ignores the player's keybinds: prefer sam_is_action_held, which follows them and handles every binding kind.

| argument | type |
|---|---|
| `key_name` | string |
| `player` *(optional)* | int (whose keyboard; left out: the player the current event is about, else this machine's own) |

**Returns:** whether the key is down (boolean)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. Left out, `player` is the player the current event is about, else this machine's own. On the host, a player on another machine reads the keys their game reported: A-Z, 0-9 and F1-F12 only, and false for a player whose game does not run S.A.M.

### `sam_register_action(id, label, default_key, [default_pad])`

Give your mod its own rebindable row in Settings > Controls > Bindings, under your mod's name. The action's full name is "<namespace>:<id>": that is what on_action_pressed / on_action_released carry in `action`, and what sam_is_action_held and sam_get_action_binding take. `id` is letters, digits, '_', '-' and '.'. `default_key` is the name the Bindings page shows for a key: a letter or digit ("F", "3"), "F1".."F12", "Keypad 0".."Keypad 9" (NOT "KP0"), "Space", "Return", "Tab", "Left Shift", "Left Ctrl", "Left Alt", "Escape", "Backspace", "Up"/"Down"/"Left"/"Right", "Home", "End", "Page Up"/"Page Down", "Insert", "Delete", a punctuation key as its character, "Mouse1".."Mouse15", "MouseWheelUp", "MouseWheelDown", or "[unbound]". `default_pad` is a controller input without the seat prefix: "ButtonA/B/X/Y" (or just "Y"), "ButtonBack", "ButtonStart", "ButtonLeftBumper", "ButtonRightBumper", "ButtonLeftStick", "ButtonRightStick", "LeftTrigger", "RightTrigger", "DpadX+/X-/Y+/Y-", "StickLeftX+".."StickRightY-", or "[unbound]" (the default). They are DEFAULTS: whatever the player already has bound to this action in config.json wins, survives a restart and survives the mod being unloaded, and Restore Defaults puts your default back. Call it at top level: the registry is rebuilt on every load, registering the same id again is a no-op that refreshes the label, and a row registered from an event only exists from that moment.

The game has no conflict check: an action on the same key as a vanilla one (or another mod's) fires alongside it. Pick a default nothing uses (the keypad is empty in every vanilla layout) and let the player rebind. Bare letters are stored in SDL's spelling ("f" becomes "F"); pass "Keypad 0", not "KP0", which is refused. A mod action is polled and reported exactly like a vanilla one, on every machine.

| argument | type |
|---|---|
| `id` | string |
| `label` | string |
| `default_key` | string |
| `default_pad` *(optional)* | string |

**Returns:** true if the action exists now (boolean); false, with the reason in the log, for a bad id or a key name the game does not know

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.


## Inventory

### `sam_can_items_stack(player, uid_a, uid_b)`

Whether two items would actually combine, using the game's own comparison rather than a guess at it, so things that never stack (readable books, for instance) correctly answer false. Passing the same item twice is false.

| argument | type |
|---|---|
| `player` | int |
| `uid_a` | int |
| `uid_b` | int |

**Returns:** true if the two would merge (boolean), or nil when either uid names no item this machine can see

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Returns nil, not false, when either uid names no item this machine can see, because false here means the two would not merge.

### `sam_can_unequip(player, uid)`

Check before promising the player a swap, so a cursed item does not silently refuse halfway through what your mod said it would do.

| argument | type |
|---|---|
| `player` | int |
| `uid` | int |

**Returns:** false if the item is cursed onto them (boolean), or nil when the uid names no item this machine can see

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Returns nil, not false, when the uid names no item this machine can see, because false here means cursed and cannot come off.

### `sam_get_equipped_item(player, slot)`

Get the item NAME equipped in a slot (ARMOR==BREASTPLATE, BOOTS==SHOES). Vanilla items only — it can't name a custom item, so use sam_get_equipped_item_id to test for one. In JavaScript player and slot are required.

| argument | type |
|---|---|
| `player` | int |
| `slot` | string — one of: `WEAPON`, `SHIELD`, `HELMET`, `HELM`, `ARMOR`, `BREASTPLATE`, `GLOVES`, `BOOTS`, `SHOES`, `RING`, `AMULET`, `CLOAK`, `MASK` |

**Returns:** the item name (string; nil/undefined if slot empty)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Ask on the host for anyone but yourself: the host's copy of every player's worn items is exact.

### `sam_get_equipped_item_id(player, slot)`

Get the item ID equipped in a slot. Compare it against sam_item_id("namespace:item") to check whether YOUR custom item is equipped — the id is a number, so the name-returning version above can never match it. In JavaScript player and slot are required.

| argument | type |
|---|---|
| `player` | int |
| `slot` | string — one of: `WEAPON`, `SHIELD`, `HELMET`, `HELM`, `ARMOR`, `BREASTPLATE`, `GLOVES`, `BOOTS`, `SHOES`, `RING`, `AMULET`, `CLOAK`, `MASK` |

**Returns:** the numeric item id (int; nil/undefined if slot empty)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Ask on the host for anyone but yourself: the host's copy of every player's worn items is exact.

### `sam_get_inventory_count(player, item_name)`

Count how many of an item (vanilla or custom name) a player holds.

| argument | type |
|---|---|
| `player` | int |
| `item_name` | string |

**Returns:** total count held (number), or nil if this machine cannot see that player's backpack

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine is answered from a copy their own game sends a moment after joining and whenever it changes (at most five times a second). nil means this machine cannot see that player's things: the host before their game has reported, or a player whose game does not run S.A.M. That 'not reported yet' moment comes at the start of EVERY run, not only after a join: the host drops the last character's copy when a new run starts and that player's game sends the new one a moment later, so read another player's backpack or spells from a timer or a later event, never from game.on_game_start. A removal is no more visible than any other change until that next report, so never write 'if they still have it, give the reward' without a flag of your own.

### `sam_get_item(uid)`

Everything plain about one item in a single call, rather than a dozen separate getters. The computed values live in their own functions (sam_get_item_name, sam_get_item_value, sam_get_item_weight) because each runs real engine code instead of reading a field.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** a table/object with type, count, beatitude, status, status_name, identified, appearance, owner_uid, droppable, grid_x, grid_y, or nil

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.

### `sam_get_item_ac(uid, [player])`

The armour value. Same caveat as sam_get_item_attack: the optional wearer only matters for cursed-item inversion, not for their skill or stats.

| argument | type |
|---|---|
| `uid` | int |
| `player` *(optional)* | int (optional) |

**Returns:** the armour value (number), or nil

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.

### `sam_get_item_attack(uid, [player])`

The weapon's attack value. The optional player is passed to the engine, but it only affects a few special cases such as shapeshifting and cursed-item inversion: it does NOT add that character's skill or strength, so two ordinary humans get the same number. For a real to-hit you still need the character's own stats.

| argument | type |
|---|---|
| `uid` | int |
| `player` *(optional)* | int (optional) |

**Returns:** the weapon's attack value (number), or nil

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.

### `sam_get_item_name(uid)`

The item's name, using the alias an unidentified item shows rather than its true name. Note this is the bare name: the blessed, cursed and condition wording the player sees in the tooltip is added separately by the game and is not included here.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** the item's name (string), or nil

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.

### `sam_get_item_owner(uid)`

Who this item belongs to. It is what the shopkeeper's theft rules read, so it is also how a mod knows whether something was taken rather than bought. An item nobody owns returns nil rather than zero.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** the owning entity's uid (number), nil if nobody owns it, or false when the uid names no item this machine can see

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. Returns false, not nil, when the uid names no item this machine can see, because nil already means nobody owns it. On the host it reads a remote player's items from the copy their game reports.

### `sam_get_item_slot(item_type)`

Where this kind of item is worn. Works for custom items too, so an auto-equip mod does not need its own table.

| argument | type |
|---|---|
| `item_type` | int |

**Returns:** one of WEAPON, SHIELD, MASK, HELM, GLOVES, BOOTS, BREASTPLATE, CLOAK, AMULET, RING, or NONE for something that cannot be worn

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_item_value(uid)`

What this pile is worth: the engine's per-item value multiplied by how many you have. Blessing and condition are not part of it, because the engine's own gold value ignores them too; the shop applies those separately when it quotes a price.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** gold value of the whole stack (number), or nil

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.

### `sam_get_item_weight(uid)`

Weight of the whole stack, with the engine's quiver rule applied. Add these up across sam_get_inventory for a carried total.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** weight of this stack (number), or nil

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.

### `sam_get_max_stack(player, uid)`

The ceiling for this item and this player, which differs for arrows, thrown gems and scrap. Read it before writing a count.

| argument | type |
|---|---|
| `player` | int |
| `uid` | int |

**Returns:** the largest this stack may grow (number), or nil

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_identify_item(player, uid)`

Identify an item the way a scroll does, through the engine's own path, so player.on_item_identified fires (once, on the host) and the owning player's screen updates. Calling it on an already-identified item succeeds quietly. An item on the floor, in a chest or in a shop belongs to nobody and is identified for the player you name; only an item in ANOTHER player's backpack is refused.

| argument | type |
|---|---|
| `player` | int |
| `uid` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `uid`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.

### `sam_inventory_has_space(player)`

Whether the bag has room. This one is genuinely local-only: the inventory grid exists on the machine drawing it, so asking about a remote player would answer about the wrong bag. It returns nil and logs why rather than lying.

| argument | type |
|---|---|
| `player` | int |

**Returns:** true if there is a free slot (boolean), or nil for a remote player

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Answers only for a player on this machine, because it reads that machine's inventory grid: nil for anyone else, on the host too.

### `sam_is_better_armor(uid_new, [uid_current])`

The armour counterpart of sam_is_better_weapon, covering shields, helmets, breastplates, cloaks, boots, gloves and masks. Omit the second item to ask whether it is worth wearing at all, which is only ever true for something that actually goes in one of those slots.

| argument | type |
|---|---|
| `uid_new` | int |
| `uid_current` *(optional)* | int (optional) |

**Returns:** true if the first is an upgrade (boolean), or nil when a uid you gave names no item this machine can see

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. Returns nil, not false, when either uid names no item this machine can see, including a second uid that was given but names nothing, so it never silently compares against nothing.

### `sam_is_better_weapon(uid_new, [uid_current])`

The same comparison monsters use when deciding what to pick up. Omit the second item to ask whether it is worth taking at all, which is only ever true for an actual weapon.

| argument | type |
|---|---|
| `uid_new` | int |
| `uid_current` *(optional)* | int (optional; omit to compare against nothing) |

**Returns:** true if the first is an upgrade (boolean), or nil when a uid you gave names no item this machine can see

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. Returns nil, not false, when either uid names no item this machine can see, including a second uid that was given but names nothing, so it never silently compares against nothing.

### `sam_is_item_equipped(player, uid)`

Whether this specific item is currently equipped, as opposed to merely being in the bag. A spare identical ring in the bag does not count.

| argument | type |
|---|---|
| `player` | int |
| `uid` | int |

**Returns:** true if that player is wearing or wielding this exact item (boolean), or nil when the uid names no item this machine can see

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host it answers for a remote player's items too, from the copy their game reports. It returns nil when the uid names no item this machine can see, and it is true only for the exact item being worn, so a spare identical ring does not read as equipped. It agrees with the equipped flag in sam_get_inventory.

### `sam_is_melee_weapon(uid)`

The engine's own test, so it agrees with what the game counts as melee for skill and damage purposes.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true for a melee weapon (boolean), or nil when the uid names no item this machine can see (a warning names the function once)

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. On the host this answers for every player's items, including the uids sam_get_inventory hands back for a remote player. When the uid names nothing this machine can see it returns nil rather than false, and a warning names the function once.

### `sam_is_potion_bad(uid)`

The same judgement the game's own AI uses when deciding whether to throw a potion at you rather than drink it.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true if drinking this is a bad idea (boolean), or nil when the uid names no item this machine can see (a warning names the function once)

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. On the host this answers for every player's items, including the uids sam_get_inventory hands back for a remote player. When the uid names nothing this machine can see it returns nil rather than false, and a warning names the function once.

### `sam_is_ranged_weapon(item_type)`

Takes a TYPE, so it answers for an item you have not spawned.

| argument | type |
|---|---|
| `item_type` | int |

**Returns:** true for a bow, crossbow or sling (boolean)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_is_shield(uid)`

Covers everything worn in the shield hand, not only shields: lanterns, torches, quivers, spellbooks and crystal shards all count, as does any custom item your mod marks for the shield slot.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true if this occupies the offhand slot (boolean), or nil when the uid names no item this machine can see (a warning names the function once)

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. On the host this answers for every player's items, including the uids sam_get_inventory hands back for a remote player. When the uid names nothing this machine can see it returns nil rather than false, and a warning names the function once.

### `sam_item_has_trait(item_type, trait)`

The item counterpart of sam_monster_has_trait. Takes a TYPE. Answers for all eleven traits a mod can declare, checking what the item declared and then the game's own rule for a vanilla one. An unknown trait name is refused with the valid list logged rather than silently returning false.

| argument | type |
|---|---|
| `item_type` | int |
| `trait` | string — one of: `RANGED`, `QUIVER`, `FOCI`, `INSTRUMENT`, `THROWN_BALL`, `SHIELD_SLOT`, `POTION_BAD`, `AUTOMATON_FOOD`, `TINKER_THROWABLE`, `USABLE`, `BEATITUDE_AC` |

**Returns:** true if the type has that trait (boolean)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_set_item_appearance(uid, appearance)`

Set the appearance number, which chooses a readable book's contents and which potion or scroll look an unidentified item shows. REFUSED on the types where this field is not decoration: a spell tome stores its spell here, a loot bag its contents, the robots their health and a scepter its charges, and all of that is written to the save, so changing it would permanently alter what the item is.

| argument | type |
|---|---|
| `uid` | int |
| `appearance` | int |

**Returns:** true on success (boolean), false if the type is refused

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `uid`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.

### `sam_set_item_beatitude(uid, value)`

Bless or curse an item. Clamped to a range the tooltips and damage maths can actually represent. A worn item's new blessing reaches the host's copy used for combat.

| argument | type |
|---|---|
| `uid` | int |
| `value` | int (-100 to 100; negative is cursed) |

**Returns:** true on success (boolean)

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `uid`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.

### `sam_set_item_count(uid, count)`

Set how many are in a stack, up to the item's own limit; a larger number is refused rather than silently wrapped, and sam_get_max_stack tells you the ceiling. A count of zero or less DESTROYS the item, which happens a moment later rather than instantly: the game is still using that item at the point your script runs, so the removal is queued and carried out on the next frame. Destroying an equipped item is refused, because the engine's cleanup identifies items by their contents and cannot tell two identical ones apart.

| argument | type |
|---|---|
| `uid` | int |
| `count` | int |

**Returns:** true if accepted (boolean)

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `uid`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.

### `sam_set_item_droppable(uid, droppable)`

Set an item's droppable flag. Know what the game reads it for: a monster's items when it dies (and no item uid can reach a monster's inventory today), and worn armour a thief steals, which copies the flag. On a player's item it therefore matters only if a thief steals it while it is worn. The true or false is required: leaving it out is refused rather than treated as false.

| argument | type |
|---|---|
| `uid` | int |
| `droppable` | boolean (required) |

**Returns:** true on success (boolean)

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `uid`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.

### `sam_set_item_owner(uid, owner_uid)`

Reassign ownership. This is how a soulbound or stolen-goods mod says what it means using the game's own bookkeeping instead of inventing a parallel one.

| argument | type |
|---|---|
| `uid` | int |
| `owner_uid` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `uid`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning. An owner set on a client player's item stays while it is in their backpack, but is lost if they drop it: the game's drop packet carries no owner.

### `sam_set_item_status(uid, status)`

Set an item's condition, as a name or as a number from 0 (BROKEN) to 4 (EXCELLENT). Anything outside that range is refused rather than quietly rounded to the nearest end, so a wrong number tells you instead of half working. A string is always read as a name, so pass 3 and not "3". Note that dropping a worn item to BROKEN does not take it off: the game refuses to USE a broken item but leaves it equipped, and that is vanilla behaviour rather than something this call gets wrong.

| argument | type |
|---|---|
| `uid` | int |
| `status` | string name or int 0-4 — one of: `BROKEN`, `DECREPIT`, `WORN`, `SERVICABLE`, `EXCELLENT` |

**Returns:** true on success (boolean), false if the status is out of range

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `uid`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.


## Live patching

### `sam_add_class_passive(class, effect)`

Grant a class a permanent status effect at character creation (bakes at creation; run at mod-load).

| argument | type |
|---|---|
| `class` | any — one of: `classnum (int)`, `"namespace:class" (string)` |
| `effect` | any — one of: `EFF_ id (int)`, `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.

### `sam_patch_class(class, patch)`

Override a class's STARTING stats/skills (patch = { STR, DEX, ..., MAXHP, skills = {...} }). Reverts on unload. Tables are read the same way in Lua and JavaScript: keys are case-insensitive, number fields take only numbers and text fields only strings.

| argument | type |
|---|---|
| `class` | any — one of: `classnum (int)`, `"namespace:class" (string)` |
| `patch` | table |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table. A patch left over from an earlier singleplayer game can still shape a client's FIRST character in a co-op run, because that character is built before the host's table arrives: patch classes when your mod loads, or reload mods before hosting.

### `sam_patch_item(item, patch)`

Override an item type's base fields live: { weight, value/gold_value, level, category, slot, tooltip, name/name_identified, name_unidentified, attributes = {...} }. An unrecognised category or slot name is refused rather than being read as WEAPON or NO_EQUIP, which are real values that would have applied silently. The whole patch is checked before any of it is written, so a false really does mean the item is untouched. Tables are read the same way in Lua and JavaScript: keys and category and slot names are matched without regard to case, number fields take only numbers and text fields only strings, and value beats gold_value and name_identified beats name.

| argument | type |
|---|---|
| `item` | any — one of: `item id (int)`, `vanilla name (string)`, `"ns:item" (string)` |
| `patch` | table |

**Returns:** true on success (boolean); false if a category or slot name is not recognised, and then nothing at all is changed

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.

### `sam_patch_monster(monster, patch)`

Override a monster type's base stats (e.g. { HP, MAXHP, STR }) for future spawns; also zero RANDOM_* for exact values.

| argument | type |
|---|---|
| `monster` | any — one of: `monster type id (int)`, `monster type name (string)` |
| `patch` | table |

**Returns:** true if any field applied (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_remove_class_passive(class, effect)`

Remove a class passive effect previously added.

| argument | type |
|---|---|
| `class` | any — one of: `classnum (int)`, `"namespace:class" (string)` |
| `effect` | any — one of: `EFF_ id (int)`, `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.

### `sam_unpatch_class(class)`

Revert a class stat/skill patch.

| argument | type |
|---|---|
| `class` | any — one of: `classnum (int)`, `"namespace:class" (string)` |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.


## Logging

### `sam_log(msg)`

Write a line to sam_log.txt (the only output channel). Also exposed as sam.log(msg) in Lua.

| argument | type |
|---|---|
| `msg` | string |

**Returns:** nothing

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_message(player, text)`

Show a line in a player's in-game message log.

| argument | type |
|---|---|
| `player` | int |
| `text` | string |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A line sent to a player on another machine is shown with one leading space, so that player's game can never mistake it for one of Barony's own messages that it acts on.

### `sam_test_done(passed, failed)`

End an unattended test run started with barony.exe -samtest=YourMod. Writes the verdict to sam_log.txt and quits the game, setting the process exit code to 0 when failed is 0 and 1 otherwise, so a script can run your mod's tests and read the answer without anyone playing.

Outside a -samtest run it does nothing and returns false, so it is safe to leave in a published mod. A run that never calls it is stopped by the watchdog and reports exit code 2: a test that hangs has failed, not passed.

| argument | type |
|---|---|
| `passed` | int (how many checks passed) |
| `failed` | int (how many failed) |

**Returns:** true if this was an unattended test run and it has now ended (boolean)

**Multiplayer:** `local`. Answers for the machine running the script.


## Loot

### `sam_add_item_to_container(uid, item, [count], [status], [beatitude], [identified], [appearance])`

Put an item into a chest (open or closed), a mimic, a creature's pockets, or a shopkeeper's stock. Defaults: one, EXCELLENT, uncursed, identified, a random appearance. A shopkeeper's stock is laid out again by price so the new item has a slot the window can show; a potion added to a shop takes its standard appearance, as generated stock does.

A creature's pockets are what it DROPS when it dies, so this is also "give this monster a drop". A worn slot (helmet, weapon) is not a container: use sam_monster_equip for those. A chest a player has open goes through the engine's own add path, which tells that player's machine; a closed chest is served whole on the next open.

| argument | type |
|---|---|
| `uid` | int (a chest, or a creature) |
| `item` | any — one of: `item type (int)`, `vanilla name`, `"namespace:item"` |
| `count` *(optional)* | int |
| `status` *(optional)* | int (0 BROKEN .. 4 EXCELLENT) |
| `beatitude` *(optional)* | int |
| `identified` *(optional)* | boolean |
| `appearance` *(optional)* | int |

**Returns:** the new item's uid (int), or nil/undefined when refused

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Host only. A chest's contents are served to whoever opens it and a shop's when it is entered, so nothing else needs sending. A player on another machine who is browsing the shop has their window closed with the engine's own SHPC; their next talk serves the new stock. A chest they have open gets the item as a new stack, so their window shows it.

### `sam_get_loot_pool(category, min_level, max_level, [context])`

The set of items a random roll of that category and level window would draw from: the sheet's item_level test (so a sam_patch_item level counts), mod-registered items, and the loot tables (an item weighted 0, outside its floor window, or kept out of the given context is absent). Without a context the context rule is not tested. The per-roll chance drops the engine makes on five items (tin opener, lantern, frying pan, backpack, grass sprig), the store-type exclusions and the GEM shortcut (nine gem rolls in ten return glass before the pool is read) are not in it; they happen inside the roll.

level in each entry is the sheet's item_level, the floor threshold; weight is the table's weight (1 unless you set one). An empty array means the roll would return the fallback (GEM_ROCK, or what sam_set_loot_fallback named).

| argument | type |
|---|---|
| `category` | string — one of: `WEAPON`, `ARMOR`, `AMULET`, `POTION`, `SCROLL`, `MAGICSTAFF`, `RING`, `SPELLBOOK`, `GEM`, `THROWN`, `TOOL`, `FOOD`, `BOOK` |
| `min_level` | int |
| `max_level` | int |
| `context` *(optional)* | string — one of: `floor`, `chest`, `shop`, `monster`, `recipe`, `console`, `other` |

**Returns:** array of { type, name, level, weight }, or nil/undefined for a category that is not one

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads tables every S.A.M machine keeps a copy of, so a client answers the same as the host once the host's table calls have reached it.

### `sam_remove_item_from_container(uid, item, [count])`

Take an item out of a chest, a mimic, a creature's pockets or a shop's stock: by the item's own uid, or by type (the first stack of that type). count takes that many from the stack; 0 or omitted removes it.

sam_get_container_items does not list item uids, so removal by type is the usual form. A chest a player has open is closed first (there is no packet for "the host took this out"); the next open serves the contents as they now are. A shop window open on it is closed the same way. Called from inside world.on_loot_rolled or world.on_before_spellbook_reroll on the very item the event is about, the removal happens at the next frame (the engine is still writing to that item); it is gone before anyone can see it.

| argument | type |
|---|---|
| `uid` | int (a chest, or a creature) |
| `item` | any — one of: `an item uid the container's list holds`, `item type (int)`, `vanilla name`, `"namespace:item"` |
| `count` *(optional)* | int (0 or omitted: the whole stack) |

**Returns:** true if something was removed (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Host only; see sam_add_item_to_container.

### `sam_roll_loot(category, min_level, max_level, [context])`

One roll of the engine's own loot curve, exactly as a chest or a shop makes it: the level window, the loot tables, world.on_before_loot_roll, the spellbook re-draw, world.on_loot_rolled. Use it for your own drops instead of re-implementing the curve. The context (default "other") is what the tables and the events see.

It draws from the game's own local generator, the one gameplay uses for a monster's attack roll: a seeded run's floors come from the seed and are unchanged by it; in an unseeded run each call advances the stream the next floor's seed is drawn from, exactly as that attack roll does. It answers with a TYPE, not an item: make the item with sam_spawn_item or sam_add_item_to_container. Refused from inside a loot event handler (world.on_before_loot_roll, world.on_loot_rolled, ...): a nested roll would fire a nested event into the store the outer handler is writing.

| argument | type |
|---|---|
| `category` | string — one of: `WEAPON`, `ARMOR`, `AMULET`, `POTION`, `SCROLL`, `MAGICSTAFF`, `RING`, `SPELLBOOK`, `GEM`, `THROWN`, `TOOL`, `FOOD`, `BOOK` |
| `min_level` | int |
| `max_level` | int |
| `context` *(optional)* | string — one of: `floor`, `chest`, `shop`, `monster`, `recipe`, `console`, `other` |

**Returns:** an item type (int), or nothing (Lua: no value, JS: undefined) when refused

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.

### `sam_set_loot_category_weight(category, weight)`

How often the "any category" draw lands on a category. Four places make that draw: a random floor item, a completely-random chest, the general store, and a troll's hoard. 1 is vanilla, 0 never, N is N times as likely. With no category weighted the draw is the vanilla code, including its quirks (a floor THROWN result is re-rolled two times in three, a floor BOOK half the time); the moment any category is weighted the draw becomes one weighted pick and those quirks are gone for it.

Weighting every category 0 is refused at roll time (the vanilla draw is used and the log says so once). This does not affect rolls that name their category (a scroll chest rolls SCROLL whatever you set). The host re-sends every loot table to a joiner when they say hello, so a value set at the main menu, from mod.on_setting_changed or in an earlier run reaches them too.

| argument | type |
|---|---|
| `category` | string — one of: `WEAPON`, `ARMOR`, `AMULET`, `POTION`, `SCROLL`, `MAGICSTAFF`, `RING`, `SPELLBOOK`, `GEM`, `THROWN`, `TOOL`, `FOOD`, `BOOK` |
| `weight` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.

### `sam_set_loot_context(item, context, allowed)`

Keep an item out of one kind of roll (false) or let it back in (true, the default): "artifacts on the floor and in chests, never in a store" is sam_set_loot_context("ARTIFACT_SWORD", "shop", false). The contexts: floor (items placed while the level is built), chest (a chest or mimic being filled), shop (a shopkeeper stocking), monster (a creature's starting gear), recipe (the potion an alchemy skill-up teaches), console (/sam_loot, /itemlevelcurve), other (sam_roll_loot with no context).

The recipe context exists so a randomizer does not teach the player to brew an artifact: keep everything that is not a potion out of "recipe" if you open the POTION pool. The lockpick capstone reward rolls in the chest context. The host re-sends every loot table to a joiner when they say hello, so a value set at the main menu, from mod.on_setting_changed or in an earlier run reaches them too.

| argument | type |
|---|---|
| `item` | any — one of: `item type (int)`, `vanilla name`, `"namespace:item"` |
| `context` | string — one of: `floor`, `chest`, `shop`, `monster`, `recipe`, `console`, `other` |
| `allowed` | boolean |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.

### `sam_set_loot_fallback(category, item, [context])`

What a roll returns when NOTHING in the category fits its window, instead of the engine's GEM_ROCK. The Hamlet's Arms & Armour store rolls weapons from level 10 up: a sheet where every weapon is level 0 fills it with fourteen rocks. A fallback swaps each rock for the one named item, so it does not make that store work: widen its window in world.on_before_loot_roll (e.min_level) for that. Set per category, and per context when it should differ (a rock in a chest is fine, in a shop it is a bug); "ANY" is the any-category draw the lockpick capstone reward makes. item nil removes the rule.

The fallback is not weighed, windowed or context-tested: it is returned as named. world.on_loot_rolled reports is_fallback = 1 for these rolls, so a handler can tell a fallback rock from a garbage-chest rock. The host re-sends every loot table to a joiner when they say hello, so a value set at the main menu, from mod.on_setting_changed or in an earlier run reaches them too.

| argument | type |
|---|---|
| `category` | string — one of: `ANY`, `WEAPON`, `ARMOR`, `AMULET`, `POTION`, `SCROLL`, `MAGICSTAFF`, `RING`, `SPELLBOOK`, `GEM`, `THROWN`, `TOOL`, `FOOD`, `BOOK` |
| `item` | any — one of: `item type (int)`, `vanilla name`, `"namespace:item"`, `nil: vanilla again` |
| `context` *(optional)* | string — one of: `floor`, `chest`, `shop`, `monster`, `recipe`, `console`, `other` |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.

### `sam_set_loot_floor_range(item, min_floor, [max_floor])`

A window on the CURRENT FLOOR NUMBER inside which the item may roll, checked beside the sheet's item_level. max_floor is the ceiling vanilla has no knob for ("leather armour stops appearing after floor 10"); leave it out for no ceiling. sam_set_loot_floor_range(item, 0) removes the rule.

This is the dungeon floor the roll happens on, not the roll's level window: a chest on floor 3 rolls its contents with max_level 8, and this rule still asks about floor 3. Refused when max_floor is below min_floor. The host re-sends every loot table to a joiner when they say hello, so a value set at the main menu, from mod.on_setting_changed or in an earlier run reaches them too.

| argument | type |
|---|---|
| `item` | any — one of: `item type (int)`, `vanilla name`, `"namespace:item"` |
| `min_floor` | int |
| `max_floor` *(optional)* | int |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.

### `sam_set_loot_weight(item, weight)`

How likely one item is inside any roll that considers it: 1 is vanilla, 0 is never, 5 is five times as likely as a weight-1 item in the same draw. Applies everywhere the engine rolls from the item sheet: floor items, chests, shops, monster gear, the lockpick reward, sam_roll_loot. Last call wins.

Weight 0 removes the item from the pool but does not change how many random numbers the engine draws, so a seeded dungeon still walks the same rooms; any weight above 1 turns that roll into a weighted draw and the dungeon's item sequence changes from that roll on. A weight does not put an item INTO the pool: an item with item_level -1 (artifacts, orbs, class books) needs sam_patch_item(item, { level = N }) first. The host re-sends every loot table to a joiner when they say hello, so a value set at the main menu, from mod.on_setting_changed or in an earlier run reaches them too.

| argument | type |
|---|---|
| `item` | any — one of: `item type (int)`, `vanilla name, e.g. "ARTIFACT_SWORD"`, `"namespace:item"` |
| `weight` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table. A client of a host without S.A.M, and a stock 5.0.2 client, roll vanilla floor loot from the seed, so their floors differ from the host's.

### `sam_set_shop_stock(shopkeeper_uid, items)`

Replace a shopkeeper's stock with exactly this list, then run the engine's price-sorted slot layout. The data-driven consumables the store type always sells (the bottom-right row) stay. Meant for world.on_before_shop_stock (return false, then stock it from world.on_shop_stocked) or for any shop at any time. An empty list empties the shop of everything but the consumables.

Each entry defaults to one, SERVICABLE, uncursed, identified. The whole list is validated before the shop is touched, so false always means the shop is as it was. The mysterious merchant keeps his fixed arrangement: new items there take the next free slot.

| argument | type |
|---|---|
| `shopkeeper_uid` | int |
| `items` | table (array of { item, count, status, beatitude, identified } tables, or bare item names / ids) |

**Returns:** true on success (boolean); false and nothing changed when an entry is not an item

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Host only. A player on another machine who has the shop open has their window closed with the engine's own SHPC; their next talk serves the new stock.

### `sam_set_shop_type(shopkeeper_uid, store_type)`

What kind of store a shopkeeper opens. Only before it has stocked: the tick it was spawned with sam_spawn_monster, or a shopkeeper placed by the map before its first tick. After that the type has been read and the call is refused with the reason.

For a shopkeeper that has already stocked, world.on_before_shop_stock is too late as well; change what it SELLS with sam_set_shop_stock instead. sam_spawn_monster's own shop_type argument does the same thing at spawn time.

| argument | type |
|---|---|
| `shopkeeper_uid` | int |
| `store_type` | int (0 arms, 1 hats, 2 jewelry, 3 books, 4 apothecary, 5 staves, 6 food, 7 hardware, 8 hunting, 9 general, 10 mysterious) |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Host only; the store type travels with the shopkeeper's stats.

### `sam_set_spell_droppable(spell, allowed, [min_floor])`

Whether a spell's book (or tome) may come out of a spellbook roll, from min_floor (default 0), or never. Every rolled spellbook is re-drawn by the engine from the spell table after the item roll, using each spell's drop_table floor and hiding class, race and monster spells outright; this overrides that rule for one spell. This is the way to put a class spellbook into the loot pool: a level patch on the book alone is undone by the re-draw.

A spell that has no spellbook or tome can never be produced by a roll however droppable it is declared; the call warns and keeps the entry. The difficulty ladder (spell difficulty against floor) still applies to an allowed spell; force one from world.on_before_spellbook_reroll to skip it. The host re-sends every loot table to a joiner when they say hello, so a value set at the main menu, from mod.on_setting_changed or in an earlier run reaches them too.

| argument | type |
|---|---|
| `spell` | any — one of: `"SPELL_FIREBALL" (a SPELL_ name)`, `"namespace:spell"`, `spell id (int)` |
| `allowed` | boolean |
| `min_floor` *(optional)* | int |

**Returns:** true on success (boolean)

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.


## Mechanisms

### `sam_power_entity(uid, on)`

Power a mechanism on or off, as a switch wired to it would.

| argument | type |
|---|---|
| `uid` | uid |
| `on` | boolean |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_door(uid, open)`

Open or close a door. Find one with sam_find_entities(x, y, r, "door").

| argument | type |
|---|---|
| `uid` | uid |
| `open` | boolean |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_door_locked(uid, locked)`

Lock or unlock a door.

| argument | type |
|---|---|
| `uid` | uid |
| `locked` | boolean |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_toggle_switch(uid)`

Flip a lever or switch, driving whatever it is wired to.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.


## Monsters

### `sam_alert_allies(uid, [attacker_uid])`

Wake every ally near this monster onto an attacker, the way the engine does when something is hit in a room full of its friends. The attacker may be left out for "alerted by nothing in particular".

| argument | type |
|---|---|
| `uid` | uid |
| `attacker_uid` *(optional)* | uid |

**Returns:** true if the call ran (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_apply_monster_effect(uid, effect, ticks)`

Apply a status effect to a monster by UID for N ticks.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `ticks` | int |

**Returns:** true unless immune (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_clear_monster_target(uid, [force])`

Make a monster forget its current target.

The engine can REFUSE — a monster whose AI insists keeps its target unless you pass force — and that refusal is passed straight through, so false means "it would not" rather than "nothing happened".

| argument | type |
|---|---|
| `uid` | uid |
| `force` *(optional)* | boolean |

**Returns:** true if it let go (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_get_monster_data(uid, key)`

Read per-monster scratch data (boss phases, etc.); in-memory, cleared on shutdown.

| argument | type |
|---|---|
| `uid` | uid |
| `key` | string |

**Returns:** the stored value, or nil/undefined

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_monster_effect_duration(uid, effect)`

How many ticks of an effect a monster has left.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** remaining ticks (int; 0 inactive, -1 permanent; nil on a client)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_monster_effect_strength(uid, effect)`

A monster effect's strength/magnitude.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** strength/tier (int; 0 if inactive)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_get_monster_effects(uid)`

Every active effect on a monster at once. Custom slots appear under the id you declared them with, like "mymod:frostbite", and vanilla ones under the lowercase name the effect events use.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** array/table of { name, ticks, strength }

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns an empty table (an empty array in JavaScript). In Lua the names now match JavaScript and the effect events: lowercase for a vanilla effect, the mod's 'ns:effect' for a custom one, and 'CUSTOM:<id>' for an unnamed slot. A Lua script comparing against 'POISONED' must change.

### `sam_get_monster_stat(uid, stat)`

Read a monster's stat by UID. DEX aliases SPEED.

| argument | type |
|---|---|
| `uid` | uid |
| `stat` | string — one of: `STR`, `DEX`, `SPEED`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `LEVEL`, `LVL` |

**Returns:** the stat value (number; 0 if not a monster)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_get_monster_target(uid)`

Get the player index a monster is currently targeting (if any).

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** the targeted player index, or -1 (number); nil on a client

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). A client's call returns nil, never 0, because 0 would read as "hunting the host".

### `sam_get_monster_target_uid(uid)`

Read back what a monster is currently hunting, as a uid, whether that is a player, another monster or anything else with a body.

sam_get_monster_target answers a player INDEX and -1 for everything else, so it cannot see a monster hunting another monster. This is the reader that matches sam_set_monster_target_uid. The stored number is resolved before it is handed back: a monster does not forget its target when that target dies, and the engine reuses uids, so a raw read could name something else entirely.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** the uid of whatever this monster is hunting, or 0 for nobody (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_kill_monster(uid)`

Kill a monster by UID (runs its normal death + drops; fires on_monster_died).

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_monster_attack(uid)`

Make a monster swing immediately, using whatever attack pose its current weapon calls for.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_monster_can_wield(uid, item_type)`

Whether a creature's AI knows how to use a kind of item. Only five species have that behaviour at all: goblins, humans, goatmen, automatons and shadows. Everything else answers false, including custom races, even though sam_monster_equip will happily put the item in their hand. It runs on the host, because it reads the monster's stats.

| argument | type |
|---|---|
| `uid` | int (monster) |
| `item_type` | int |

**Returns:** true if that creature's AI will pick up and use the item (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_monster_charge(uid, [ticks])`

Send a monster into a straight-line charge for N ticks (50 = 1 second, default 50, max 500). Aims at its target if it has line of sight, otherwise charges along its current facing.

Self-terminating: it stops when the timer ends OR the instant it hits anything, so you cannot wedge a monster in a wall. Drives a charge behaviour that shipped in the engine fully written but unreachable.

| argument | type |
|---|---|
| `uid` | uid |
| `ticks` *(optional)* | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_monster_equip(uid, slot, item, [beatitude], [status], [count])`

Put an item into a monster's equipment slot. Resolves a custom "ns:item" first and falls back to a vanilla item name. An unknown slot is refused and the valid list is logged. The last three arguments are the same three sam_grant_item takes, so a monster can be given a cursed, a broken or a stacked item.

| argument | type |
|---|---|
| `uid` | int |
| `slot` | string — one of: `helmet`, `breastplate`, `gloves`, `shoes`, `shield`, `weapon`, `cloak`, `amulet`, `ring`, `mask` |
| `item` | string ("ns:item" from your mod, or a vanilla item name) |
| `beatitude` *(optional)* | int (default 0; negative is cursed, positive blessed) |
| `status` *(optional)* | int (0 BROKEN to 4 EXCELLENT, default 4) |
| `count` *(optional)* | int (default 1) |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_monster_face(uid, tileX, tileY)`

Turn a monster to look at a tile. Aims at the tile centre. Pair it with sam_monster_charge to aim a charge.

| argument | type |
|---|---|
| `uid` | uid |
| `tileX` | int |
| `tileY` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_monster_has_effect(uid, effect)`

The monster counterpart of sam_has_effect — e.g. react when a monster you just hit is POISONED. Pass a monster UID (from a monster event or sam_get_nearby_entities).

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** whether the monster has the effect (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_monster_path_to(uid, tileX, tileY)`

Path a monster to a tile using the engine's real pathfinder, then put it in the hunt state so it walks there. Tile coordinates, matching sam_get_position. Returns false when the destination is unreachable.

Barony has no per-species AI — every creature runs one shared state machine — so this steers that machine rather than replacing a brain.

| argument | type |
|---|---|
| `uid` | uid |
| `tileX` | int |
| `tileY` | int |

**Returns:** true if a path was found (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_monster_unequip(uid, slot)`

Empty one of a monster's equipment slots. Pairs with sam_monster_equip for disarm effects and for swapping a creature's loadout mid-fight.

| argument | type |
|---|---|
| `uid` | int |
| `slot` | string — one of: `helmet`, `breastplate`, `gloves`, `shoes`, `shield`, `weapon`, `cloak`, `amulet`, `ring`, `mask` |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_remove_monster_effect(uid, effect)`

Clear a status effect from a monster by UID — the monster counterpart of sam_remove_effect.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_monster_data(uid, key, value)`

Store any primitive/table value in a monster's scratch store (JSON-marshaled). Returns true in Lua and JavaScript alike.

| argument | type |
|---|---|
| `uid` | uid |
| `key` | string |
| `value` | any |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_monster_name(uid, name)`

Rename a living monster. The name is what the player sees when targeting it and what appears in the obituary, so this is how a scripted boss or a named rare gets its title.

| argument | type |
|---|---|
| `uid` | int |
| `name` | string |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A follower's new name reaches its owner's ally panel and nametag, if the owner's game runs S.A.M.

### `sam_set_monster_stat(uid, stat, value)`

Set a monster's stat by UID (bounded). MAXHP is capped at 32767, the enemy HP bar's wire size.

| argument | type |
|---|---|
| `uid` | uid |
| `stat` | string — one of: `STR`, `DEX`, `SPEED`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `LEVEL`, `LVL` |
| `value` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A remote player's follower shows its new MAX HP and level on its owner's ally panel.

### `sam_set_monster_target(uid, player)`

Make a monster acquire a player as its attack target.

| argument | type |
|---|---|
| `uid` | uid |
| `player` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_monster_target_uid(uid, target_uid, [was_hit])`

Point a monster at ANY entity, not just a player: another monster, a companion, anything with a body.

sam_set_monster_target takes a player INDEX and can only ever aim at a player, so monster-versus-monster aggro was unreachable even though the engine method itself accepts any entity.

| argument | type |
|---|---|
| `uid` | uid |
| `target_uid` | uid |
| `was_hit` *(optional)* | boolean |

**Returns:** true if the monster took the target (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_spawn_monsters(near_uid, monster_type, count)`

Spawn `count` (1-8) monsters of a type near an anchor entity's UID.

| argument | type |
|---|---|
| `near_uid` | uid |
| `monster_type` | string |
| `count` | int |

**Returns:** the number actually spawned (number)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.


## Multiplayer

### `sam_is_host()`

Whether this machine is the host. Functions of kind host warn once on a client; check this first to skip a whole block of host work.

Scripts load before multiplayer starts, so while mods load this answers true on every machine. Call it inside events, and never cache the answer at load.

**Returns:** true on the host or in singleplayer (boolean)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_local_player()`

The player index THIS machine controls.

Not always 0. On a multiplayer client it is that client's own index, which is the assumption most singleplayer-tested mods quietly bake in. While mods load it answers 0 on every machine (scripts load before multiplayer starts), so call it inside events and never cache it at load.

**Returns:** player index (int)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_player_count()`

How many players are actually connected right now.

**Returns:** connected players (int)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Correct on every machine after a player times out or is kicked with /kick (while a mod is loaded).


## Networking

### `sam_send_packet(target, tag, payload)`

Send a mod-defined message to another machine. Barony's packet ids are a fixed table, so before this a co-op mod had no way to tell the other side anything at all. On a client the target is ignored and the packet always goes to the host. The other side receives an "on_packet" event with .from, .tag and .payload. The tag is 1 to 32 characters and the payload at most 400 bytes: one datagram, so split bulk data into several packets.

| argument | type |
|---|---|
| `target` | int (player 0..3, or -1 for every client) |
| `tag` | string |
| `payload` | string |

**Returns:** true if sent (boolean)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Works only once the game has started: in the lobby it is refused with a warning, and in singleplayer it returns false. Delivery is reliable but NOT ordered, so number your packets if order matters. A payload may contain zero bytes: Lua handlers receive the exact bytes, JavaScript handlers receive text (invalid UTF-8 is replaced), so send text or JSON from mods meant for both runtimes. The host ignores a packet that claims to come from slot 0 or from an empty slot. This is how a client's own script (in on_packet) tells the host something.


## Panels

### `sam_ui_button(panel, id, x, y, w, h, text, [player])`

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
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel is not open (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_clear(panel, [player])`

Remove every widget from a panel but leave the panel itself open. This is how you rebuild a changing screen — clear, then re-declare the rows — without the window flickering shut and open again.

| argument | type |
|---|---|
| `panel` | string |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if the panel is not open (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_close([panel], [player])`

Close one panel, or every panel your mod has open if you pass nothing (sam_ui_close(nil, p) closes all of them on one player's screen). Closing the last modal panel restores the player's camera control. Always close your panels on player.on_death and game.on_game_start so a leftover window cannot follow the player into the next run.

| argument | type |
|---|---|
| `panel` *(optional)* | string (optional — omit to close ALL of your mod's panels) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel was not open (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_font(panel, id, font, [player])`

Change the font of one widget, or of an entire panel by passing an empty id -- which is the only way to restyle a panel's text in one call rather than widget by widget. Panels default to a small 16px face because the game's standard 32px font makes any list look enormous. The number after the first # is the pixel size — raise it for a heading, and raise the row height to match if it is a list.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (a widget's id, or "" to set the whole panel's font) |
| `font` | string (a font path, e.g. "fonts/pixel_maz_multiline.ttf#16#2") |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel or widget does not exist (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_image(panel, id, x, y, w, h, image, [color], [player])`

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
| `color` *(optional)* | colour (optional, default white = untinted) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if the picture could not be resolved (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_input(panel, id, x, y, w, h, [text], [player])`

Put an editable text box in a panel — a search field, a name entry, a price offer. Read what the player typed with sam_ui_input_text. Place the box clear of any label: a label wide enough to overlap the box will sit on top of it.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `text` *(optional)* | string (optional starting contents) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel is not open (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_input_text(panel, id, [player])`

Read what the player has typed into one of your text boxes. Poll it from a button handler, or when a ui.on_submit arrives. Pressing Enter in the box also fires "ui.on_submit" with the text in .value.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (the input's id) |
| `player` *(optional)* | int (whose panel; left out: the player the current event is about, else this machine's own) |

**Returns:** the current contents (string), or "" if there is no such input

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). Left out, `player` is the player the current event is about, else this machine's own. On the host, a player on another machine is answered from what their game last reported (typing is reported a few times a second), so right after sam_ui_input it can still hold the old text for a moment.

### `sam_ui_is_open(panel, [player])`

Ask whether one of your panels is on screen. Useful to make a key or an item toggle a window instead of re-opening it, and to skip expensive refresh work while it is closed.

| argument | type |
|---|---|
| `panel` | string |
| `player` *(optional)* | int (whose panel; left out: the player the current event is about, else this machine's own) |

**Returns:** true if that panel is currently open (boolean)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. Left out, `player` is the player the current event is about, else this machine's own. On the host, a player on another machine is answered from what their game last reported, so right after sam_ui_open it can still say closed for a moment.

### `sam_ui_label(panel, id, x, y, w, text, [color], [player])`

Put a line of text in a panel. x/y are measured from the panel's top-left corner, not the screen. Give w enough room for the text or it will be cut off — sam_ui_text_size measures a string before you place it. Re-declaring the same id replaces the text, which is how you update a running total.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (your id for this widget) |
| `x` | int (relative to the panel) |
| `y` | int |
| `w` | int |
| `text` | string |
| `color` *(optional)* | colour (optional, default warm parchment) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel is not open (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_list(panel, id, x, y, w, h, [player])`

Create an empty scrolling list in a panel. Fill it with sam_ui_list_add. This is the widget for a shop's stock, a bestiary, a recipe index or a quest log — anything with more entries than fit on screen.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel is not open (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_list_add(panel, id, row_id, text, [color], [player])`

Append one row to a list. Clicking a row fires "ui.on_select" with .panel, .widget set to the list and .value set to the row_id you chose here — so make row_id something you can act on, like an item id, rather than a display string.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (the list's id) |
| `row_id` | string (your id for this row) |
| `text` | string |
| `color` *(optional)* | colour (optional) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel or list does not exist (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_list_clear(panel, id, [player])`

Empty one list without touching the rest of the panel. Use this before re-filling a list from a search box or a filter, so the old results do not pile up under the new ones.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (the list's id) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel or list does not exist (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_list_row_height(panel, id, pixels, [player])`

Set how tall each row of a list is. Raise it if you switched that list to a larger font, or rows will overlap.

| argument | type |
|---|---|
| `panel` | string |
| `id` | string (the list's id) |
| `pixels` | int |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel or list does not exist (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_open(panel, x, y, w, h, [title], [modal], [player])`

Open one of your mod's panels at a position and size given in VIRTUAL screen units (1280x720 at the default UI scale, not your monitor's pixels). modal = true frees the mouse cursor so the player can click your widgets, and hands camera control back when the panel closes — use it for anything with buttons. A non-modal panel is display-only and leaves the player in normal look-around mode. Opening a panel id that is already open re-positions it instead of opening a second one.

| argument | type |
|---|---|
| `panel` | string (your id for this panel) |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `title` *(optional)* | string (optional, "" for none) |
| `modal` *(optional)* | boolean (optional, default false) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true if the panel opened (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message. A modal panel shown to a client frees that client's mouse, and typing into its text box does not move that player. Clicks fire ui.on_click, ui.on_select and ui.on_submit on the host, with .player set to who clicked.

### `sam_ui_panel_style(panel, background, border, [border_width], [player])`

Recolour a panel's background and border. Nothing about a panel's look is fixed by the framework — set the background fully transparent for a bare overlay, or opaque for a solid window. Colours accept the same forms as the HUD calls.

| argument | type |
|---|---|
| `panel` | string |
| `background` | colour (0 = leave unchanged) |
| `border` | colour (0 = leave unchanged) |
| `border_width` *(optional)* | int (optional, omit to leave unchanged) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true, or false if that panel is not open (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_ui_text_size(text, [font])`

Measure a string before you place it. This is how you lay a panel out properly instead of guessing: size a label to its own text so it cannot overlap the widget beside it, right-align a column of numbers, or centre a heading in a panel of known width.

| argument | type |
|---|---|
| `text` | string |
| `font` *(optional)* | string (optional; defaults to the standard panel face, NOT whatever font you set on a particular panel — this call takes no panel) |

**Returns:** width, height in pixels (two ints), or nil/undefined if the font could not be loaded

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.


## Persistence

### `sam_delete_data(key)`

Delete a persisted per-mod key.

| argument | type |
|---|---|
| `key` | string |

**Returns:** true (boolean)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_get_player_data(player, key)`

Read back a per-player in-memory value set by sam_set_player_data.

| argument | type |
|---|---|
| `player` | int |
| `key` | string |

**Returns:** the stored value, or nil/undefined if unset

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.

### `sam_has_data(key)`

Whether a key exists, without loading it. This genuinely distinguishes stored-but-empty from never-stored: saving nil writes a real entry, so sam_has_data is true while sam_load_data gives you nothing back. Use sam_delete_data when you want a key to actually be gone.

| argument | type |
|---|---|
| `key` | string |

**Returns:** true if your mod has saved something under this key (boolean)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_list_data_keys()`

List the keys sam_save_data has written for your mod, so you can iterate stored state without having to remember every key name. Returns an empty table when nothing has been saved yet.

**Returns:** an array/table of every key your mod has saved

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_load_data(key)`

Read back a persisted per-mod value.

| argument | type |
|---|---|
| `key` | string |

**Returns:** the stored value, or nil/undefined if unset

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_save_data(key, value)`

Persist a value (number/string/bool/table) for the calling mod under savegames/sam_mod_data/<ns>/.

| argument | type |
|---|---|
| `key` | string |
| `value` | any |

**Returns:** true on success (boolean)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_set_player_data(player, key, value)`

Store a per-player value (number/string/bool/table) in memory for THIS session: the right tool for cooldowns, ability flags and stack counters you read often. Unlike sam_save_data it never touches disk, and it is cleared when a game starts.

| argument | type |
|---|---|
| `player` | int |
| `key` | string |
| `value` | any |

**Returns:** nothing

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet. A value written in player.on_player_joined (the lobby) is cleared when the game starts: set per-player state up in game.on_game_start or game.on_level_entered.

### `sam_world_bytes()`

How much of the savegame's mod-state budget is in use. sam_world_save shares one 64 KB allowance between every mod in the run.

**Returns:** bytes currently used across all mods (number); nil on a client

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.

### `sam_world_bytes_free()`

How much room is left before sam_world_save starts refusing writes. Check this before storing something large, rather than discovering the ceiling when a save quietly fails mid-run.

**Returns:** bytes still available (number); nil on a client

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.

### `sam_world_clear(key)`

Forget one key for the current character. The whole store is dropped automatically when a run ends, so you only need this to reset something mid-run.

| argument | type |
|---|---|
| `key` | string |

**Returns:** true if there was something to remove (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.

### `sam_world_keys()`

List the keys your mod has saved for this character. Handy for migrating an older save's data, or for showing the player what a mod is remembering about their run.

**Returns:** array of your mod's stored key names (strings)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns an empty table (an empty array in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.

### `sam_world_load(key)`

Read a value back from the current character's savegame. nil on a key you have never written is the signal that this is a fresh character — that is the natural place to run first-time setup, like anchoring a home floor.

| argument | type |
|---|---|
| `key` | string |

**Returns:** the stored value, or nil/undefined if this character never stored one

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.

### `sam_world_save(key, value)`

Save a value inside the CURRENT character's savegame. A brand new character starts with none of it, so a hub's unlock flags, a quest's progress or a bank balance cannot leak from one run into the next. Deliberately size-capped (8KB per value, 64KB across all mods) because oversized save data can produce a savegame that fails to load — keep flags and counters here and keep items in a stash chest, which the game persists properly on its own.

| argument | type |
|---|---|
| `key` | string |
| `value` | any JSON-able value |

**Returns:** true if stored, false if a size limit was hit (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Uses the host's store; a client's call is refused. Send values a client's panel needs with sam_send_packet.


## Pictures

### `sam_clear_model(uid)`

Drop a script-set model and go back to whatever the entity would otherwise draw. Clients are told too, so a transformation can end cleanly.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Reaches every S.A.M player in order, so the entity goes back to its original look on their screens too.

### `sam_get_entity_flag(uid, flag)`

Read one of Barony's entity flags by name. An unknown name gives you nil, never false: false is a real answer to a question like "is this passable", so handing it back for a typo would send your script down the wrong branch without a word. Readable on a client, but a client's copy of a flag is only as fresh as the last update the host sent about that entity.

| argument | type |
|---|---|
| `uid` | int |
| `flag` | string — one of: `BRIGHT`, `INVISIBLE`, `NOUPDATE`, `UPDATENEEDED`, `GENIUS`, `OVERDRAW`, `SPRITE`, `BLOCKSIGHT`, `BURNING`, `BURNABLE`, `UNCLICKABLE`, `PASSABLE`, `USERFLAG1`, `USERFLAG2`, `INVISIBLE_DITHER`, `NOCLIP_WALLS`, `NOCLIP_CREATURES`, `ENTITY_SKIP_CULLING`, `STASIS_DITHER` |

**Returns:** true or false (boolean), or nil/undefined if the flag name is not one of the ones listed, if the uid is gone, or if the uid is a shared engine marker (0, -2, -3, -4)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's copy of the entity. On a client it can lag the host, and a flag the host clears without telling clients can stay set there. Read on the host when the answer matters.

### `sam_get_image_size(image)`

The picture's own pixel size, so a script can centre or scale it instead of hard-coding the numbers it was exported at. Also the cheapest way to check a picture actually resolves.

| argument | type |
|---|---|
| `image` | string |

**Returns:** width, height (two numbers in Lua; a [w, h] array in JS/TS; nil/undefined if it could not be loaded)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_model(uid)`

Read back the model ID a script set on this entity, or the mod model a spawned entity was created with. Returns nil for an entity drawing its ordinary model.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** the model ID string, or nil if the entity has no script-set model

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Answers the same on every machine, because the model crosses the network by name. One exception: a player whose game never received the announcement -- it joined while the host already had more than 512 calls waiting for it, or it is not running S.A.M -- answers nil there. The host writes a line to the log when it has to refuse an announcement, so this is visible rather than silent.

### `sam_get_scale(uid)`

Read an entity's scale. The counterpart to sam_set_scale, which shipped without a reader. In JavaScript this returns an array.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** x, y, z scale (numbers), or nil

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's copy; on a client the scale is rounded to steps of 1/128.

### `sam_hide_image([player])`

Take the overlay away early. With no player it hides the overlay of the player the current event is about (outside an event: your own, and in splitscreen every local player's); -1 hides every player's in multiplayer.

| argument | type |
|---|---|
| `player` *(optional)* | int |

**Returns:** true if something was showing (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names (left out: the player the current event is about, else this machine's own); -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_hud_image(id, x, y, w, h, image, [color], [player])`

A PERSISTENT picture in the script HUD — a portrait, a custom gauge, a marker. Stays until sam_hud_clear(id) or the mod unloads, unlike the overlay. w/h of 0 means the picture's own pixel size. The colour is MIXED into the art, so white (the default) leaves it untouched and the alpha byte fades it.

| argument | type |
|---|---|
| `id` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `image` | string |
| `color` *(optional)* | int (0xRRGGBBAA) |
| `player` *(optional)* | int (whose screen; left out: the player the current event is about, else this machine's own; -1: every player) |

**Returns:** true on success (boolean)

**Multiplayer:** `screen`. Shows on one player's own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine's own; -1 is every player. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen. Inside an event about a player, leaving the player out shows it to THAT player. A timer callback is not an event: there, with no player, it shows on the host's own screen, so keep the player in a local (local p = e.player) and pass it. Update on change rather than every tick: every call for a remote player is a network message.

### `sam_is_visible(uid)`

Whether an entity is visible. The counterpart to sam_set_visible.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true if the entity is being drawn (boolean), or nil

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's copy of the entity. On a client it can lag the host, and a flag the host clears without telling clients can stay set there. Read on the host when the answer matters. When a monster's invisibility effect ends, every player now sees it again (while scripts are loaded).

### `sam_set_elevation(uid, z)`

Set how high an entity floats. This is the engine's raw z, the third value sam_get_position_precise gives you, so reading and writing it round-trips. Barony's z axis points DOWN (gravity adds to it), so negative numbers are up and 0 is the floor. Clamped to -1023..1023, which is what the network can carry. REFUSED on players and monsters: their height is rewritten from scratch by their own species code on every single frame, so the call would report success and be erased before the next frame drew. Lift a creature with a levitation effect instead. Also refused on a companion, whose own hover curve rewrites its height every tick for the same reason. Use this on props, ground items, spawned portals, and entities your script owns through sam_register_behavior (though if your own handler writes the height, it wins).

| argument | type |
|---|---|
| `uid` | int |
| `z` | number (the same z sam_get_position_precise returns; NEGATIVE IS UP) |

**Returns:** true on success (boolean); false for a player, a monster or a companion

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A ground item's height reaches S.A.M players. On a ground item this sets the height WITHOUT waking it, so an item you have already placed stays at the height you give it and it no longer matters whether you call this before or after sam_set_position. An item that is still falling or sliding is under the engine's physics and pulls itself back down, so lift items that are at rest.

### `sam_set_entity_flag(uid, flag, on)`

Turn one of Barony's entity flags on or off, and tell the other players about it. PASSABLE for a decoration nobody should bump into, BLOCKSIGHT for a prop that should cast a shadow, UNCLICKABLE for scenery, BRIGHT for something that glows, BURNABLE to make a prop able to catch fire. Four flags are read-only here and the refusal tells you why: INVISIBLE belongs to sam_set_visible and BURNING to sam_set_on_fire, both of which know extra rules this one does not, while NOUPDATE and UPDATENEEDED are how the network sweep decides who to tell about what, and STASIS_DITHER is rewritten from the stasis effect on every frame so setting it would be undone before you saw it.

| argument | type |
|---|---|
| `uid` | int |
| `flag` | string — one of: `BRIGHT`, `GENIUS`, `OVERDRAW`, `SPRITE`, `BLOCKSIGHT`, `BURNABLE`, `UNCLICKABLE`, `PASSABLE`, `USERFLAG1`, `USERFLAG2`, `INVISIBLE_DITHER`, `NOCLIP_WALLS`, `NOCLIP_CREATURES`, `ENTITY_SKIP_CULLING` |
| `on` | boolean |

**Returns:** true on success (boolean); false for an unknown or read-only flag, or an entity you may not write to

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Also refused where the game rewrites the flag every frame: BLOCKSIGHT on players and monsters, INVISIBLE_DITHER on players, and BURNABLE on ground items.

### `sam_set_entity_size(uid, size, [size_y])`

Set an entity's collision box. The number is a half-extent in world units, 16 to a tile, so 4 is the usual monster and 0 means nothing collides with it. size_y defaults to the same value. Clamped to 0..127 in every mode: the network carries the size as one signed byte, so anything larger arrives negative on the other machines and turns the hitbox inside-out there while looking correct to you. Pair it with sam_set_scale when you grow a model and want the swing to match. Unlike sam_set_elevation this sticks, because the engine only writes sizes when an entity is created. In JavaScript a size that is not a whole number is refused, as in Lua.

| argument | type |
|---|---|
| `uid` | int |
| `size` | int (0-127, half-width in world units; 16 is one tile) |
| `size_y` *(optional)* | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. On a player on another machine the new size reaches their own machine if it runs S.A.M; a player whose game does not run S.A.M is refused, because their machine could not be told and they would stick in doorways. A pinned prop's size reaches S.A.M players.

### `sam_set_model(uid, model_id)`

Swap any entity's model while the game is running. What crosses the wire is the model ID, never an index, so machines with different mod orders still agree. This is what makes transformations, boss phases and damage states possible; before it, a model was fixed at spawn.

| argument | type |
|---|---|
| `uid` | int |
| `model_id` | string ("ns:model" from your mod's models list) |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. The model crosses BY NAME and in order, and a player who joins later is told as well, so every machine that has the mod draws the same model whatever order their mods loaded in. A player whose game does not run S.A.M keeps seeing the entity's original look.

### `sam_set_scale(uid, scale)`

Scale an entity. A scale of 0 or less is REFUSED, not quietly treated as 1.0 the way it used to be, which turned a script easing a model down to nothing into a model that popped back to full size on the last frame. To make something disappear use sam_set_visible. Clamped at the bottom to 1/128 with a warning as well, because the wire packs scale into one byte as scale times 128, so anything smaller arrives as 0 and vanishes on every other machine. Clamped at 1.99 with a logged warning, in EVERY mode including singleplayer: Barony quantises scale on the wire in 1/128 steps with a cap just under 2, so a larger value looks right to you and wrong to everyone else. The clamp used to be skipped in singleplayer, which meant a mod authored at 3.0 worked for its author and was broken the moment anyone hosted it.

| argument | type |
|---|---|
| `uid` | int |
| `scale` | number (1.0 is normal) |

**Returns:** true on success (boolean); false for a scale of 0 or less

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Refused for players and slimes, whose scale the game rewrites every frame on every machine. A pinned prop's exact scale reaches S.A.M players.

### `sam_set_visible(uid, visible)`

Show or hide an entity. The flag is REQUIRED: leaving it out is refused rather than guessed. A numeric 0 counts as false in both runtimes. Refused, with a logged reason, for players, monsters, ground items and a creature's LIMB (its weapon, shield, helmet or an arm), whose visibility the game rewrites every frame: use sam_apply_effect(player, "INVISIBLE", ticks) for a player or sam_apply_monster_effect(uid, "INVISIBLE", ticks) for a monster. On a limb INVISIBLE is how the game says the slot is EMPTY, so hiding one lasted a single tick; take the item off the creature instead. sam_set_model on a limb still works, which is how a cosmetic is floated in an empty hand. On props, spawned entities and companions it works even when they have a custom model.

| argument | type |
|---|---|
| `uid` | int |
| `visible` | boolean |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Players, monsters, ground items and a creature's limb are refused, because the game rewrites their visibility every frame; use sam_apply_effect(uid, 'INVISIBLE', ticks) for a creature. On props, spawned entities and companions it works even when they have a custom model, and the change reaches every player.

### `sam_show_image(player, image, [duration_ms], [alpha], [fit])`

Cover a player's screen with one of the mod's pictures, over the world AND the HUD, for duration_ms (0 or omitted = until sam_hide_image). This is the jumpscare / title-card / death-splash layer: it removes itself, so there is nothing to clean up. alpha is 0..255 (default 255). "contain" keeps the picture's aspect ratio; "stretch" (default) fills the view. In multiplayer it is drawn on that player's own machine, from its own copy of the mod.

| argument | type |
|---|---|
| `player` | int |
| `image` | string |
| `duration_ms` *(optional)* | int |
| `alpha` *(optional)* | int |
| `fit` *(optional)* | string — one of: `stretch`, `contain` |

**Returns:** true if the picture resolved (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_show_image_at(player, image, x, y, w, h, [duration_ms], [alpha])`

The same overlay, placed rather than full-screen. Coordinates are virtual screen pixels (the space sam_hud_text uses), so a fixed layout survives any resolution. w or h of 0 means the picture's own size on that axis. Still drawn over the HUD — for a picture that sits IN the HUD, use sam_hud_image.

| argument | type |
|---|---|
| `player` | int |
| `image` | string |
| `x` | int |
| `y` | int |
| `w` | int |
| `h` | int |
| `duration_ms` *(optional)* | int |
| `alpha` *(optional)* | int |

**Returns:** true if the picture resolved (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.


## Player state

### `sam_add_move_speed(player, delta)`

Add to a player's move-speed multiplier (the result is clamped to [0.1, 3.0]). Additive counterpart to sam_set_move_speed: use it to stack a bonus onto whatever the multiplier already is (e.g. +0.1 on top of a 2.0 from another ability).

| argument | type |
|---|---|
| `player` | int |
| `delta` | number |

**Returns:** the new multiplier (number)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. Reaches every S.A.M player's machine in order, like sam_set_move_speed. A player whose game does not run S.A.M still walks at vanilla speed.

### `sam_get_class(player)`

Which class a player is, as an identifier you can act on: a custom class's "namespace:class" id, or the vanilla class's own name. Accepted by sam_patch_class, sam_add_class_passive and the rest, so you can read a player's class and then change it. It is not a display string; a custom class returns its id rather than its title.

| argument | type |
|---|---|
| `player` | int |

**Returns:** the class id (string; nil/undefined if invalid)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_floor()`

Get the current floor/dungeon level.

**Returns:** the current dungeon level (number, 0-based)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_kills(player)`

Get the SAM-tracked per-player kill count for this session.

| argument | type |
|---|---|
| `player` | int |

**Returns:** kills this session (number)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_get_move_speed(player)`

Read a player's move-speed multiplier.

| argument | type |
|---|---|
| `player` | int |

**Returns:** the multiplier (number; 1.0 if unset/invalid)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Every S.A.M machine holds every player's multiplier, so a client can read anyone's and gets the host's answer.

### `sam_get_race(player)`

Get a player's race: a custom race's "namespace:race" id, or the vanilla race name ("human", "skeleton", …). Use it in a race behavior script to gate logic to players of that race.

| argument | type |
|---|---|
| `player` | int |

**Returns:** the race id (string; nil/undefined if invalid)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_stat(player, stat)`

Read a live player stat.

| argument | type |
|---|---|
| `player` | int |
| `stat` | string — one of: `STR`, `DEX`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `GOLD`, `HUNGER`, `LEVEL`, `LVL`, `EXP` |

**Returns:** the stat value (number; 0 for an unknown stat). A refused call answers with nothing at all, never 0, because 0 is a real max HP

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. A client's own HUNGER is at most 5 seconds behind the host's.

### `sam_get_time_played()`

Get elapsed game ticks for the current run.

**Returns:** ticks since the run started (number, 50/sec)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_is_defending(player)`

Whether the player is actually blocking right now: the real engine state, not just the Defend button being down.

| argument | type |
|---|---|
| `player` | int |

**Returns:** whether the player is blocking (boolean)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_level_up(player, [count])`

Level a player up count times (default 1) through the real engine path: attribute rolls, HP/MP gain, the level-up screen and sound, and full client sync. These are the actual benefits, unlike bumping LVL with sam_set_stat. Fires the player.on_level_up hook once per level.

Grants exactly what the next count levels cost under the XP curve (sam_set_xp_curve), not a flat 100 each, so it still grants exactly count levels when a mod has changed the curve.

| argument | type |
|---|---|
| `player` | int |
| `count` *(optional)* | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_play_sound(sound_id, [vol])`

Play a sound for every player. sound_id is a vanilla numeric index OR one of your mod's sounds: a file sounds/boom.ogg is "mymod:boom", or just "boom" from your own scripts. vol 0-255 (default 128).

A sound with several files plays one at random each time, and a sound's own volume (mod.json) multiplies vol. To CHANGE one of the game's sounds there is nothing to play: drop a file named after it in sounds/replace/ (docs/vanilla-sounds.md).

| argument | type |
|---|---|
| `sound_id` | int|string |
| `vol` *(optional)* | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Plays once per computer, however many splitscreen players share it. A mod sound reaches the other players by NAME, so everyone hears the same sound whatever order their mods loaded in; a player without the mod hears nothing for it. A client's call is refused with one log line, so an event that fires on every machine never plays it twice and needs no sam_is_host() guard.

### `sam_set_move_speed(player, mult)`

Set a player's move-speed multiplier, clamped to [0.1, 3.0]. 1.0 is normal speed.

| argument | type |
|---|---|
| `player` | int |
| `mult` | number |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Reaches every S.A.M player's machine in order, and is caught up at the start of every run. A player whose game does not run S.A.M still walks at vanilla speed, because their own machine computes their movement.

### `sam_set_stat(player, stat, value)`

Set a live player stat, bounded (HP never exceeds MAXHP, stats clamped, etc.). The change reaches the player's own machine, and a LVL write updates every player's party display at once.

| argument | type |
|---|---|
| `player` | int |
| `stat` | string — one of: `STR`, `DEX`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `GOLD`, `HUNGER`, `LEVEL`, `LVL`, `EXP` |
| `value` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.


## Presentation

### `sam_camera_shake(player, magnitude)`

Shake a player's camera. 1 is a nudge, ~10 a solid hit, 20+ violent. Feeds Barony's own shake channels so it decays naturally; for a remote client the host forwards it. In JavaScript player and magnitude are required.

| argument | type |
|---|---|
| `player` | int |
| `magnitude` | number (~1..20) |

**Returns:** true if accepted (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A shake for a player on another machine reaches their screen; nothing is sent to an empty player slot.

### `sam_damage_number(uid, amount, [type])`

The floating combat number the game shows on a hit. Lets a mod's custom damage read like real damage instead of being invisible.

| argument | type |
|---|---|
| `uid` | uid |
| `amount` | int |
| `type` *(optional)* | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_gib(entity_uid, [sprite])`

Throw a chunk of gore off a creature. The optional sprite overrides the model.

The one member of the spawn family sam_spawn_particle could not carry: bang, poof, explosion and sleep all take a position, and a gib takes a PARENT — it inherits the creature's colour and flies off it.

| argument | type |
|---|---|
| `entity_uid` | uid |
| `sprite` *(optional)* | int |

**Returns:** true if a chunk was thrown (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. The chunks are seen by every player. A custom gib sprite crosses as a model number, so use vanilla model numbers for gibs in multiplayer.

### `sam_hitstop(duration_ms)`

Singleplayer only. Briefly freeze enemy and projectile logic, a freeze-frame, for duration_ms (capped ~400). The player, HUD weapon and hand magic keep animating, so it reads as a punchy impact beat.

| argument | type |
|---|---|
| `duration_ms` | int |

**Returns:** true if accepted (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. In multiplayer it does nothing and says so once in the log: freezing the host's logic would desync the clients.

### `sam_impact_frame(player, r, g, b, [intensity], [duration_ms], [lines])`

The EXAGGERATED version of the flash: a colour pop PLUS manga speed lines converging on screen centre PLUS a bright core flare. Pair it with sam_camera_shake and sam_hitstop for a full impact beat. lines is the speed-line count (0 = a plain flash). In JavaScript player, r, g and b are required: a missing colour is refused instead of flashing black.

| argument | type |
|---|---|
| `player` | int |
| `r` | int |
| `g` | int |
| `b` | int |
| `intensity` *(optional)* | number (0..1) |
| `duration_ms` *(optional)* | int |
| `lines` *(optional)* | int |

**Returns:** true if accepted (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_play_sound_at(sound, tileX, tileY, [volume])`

Positional audio: it attenuates with distance and pans, so a trap firing across the level is quiet, and in co-op each player hears it from where THEY are.

| argument | type |
|---|---|
| `sound` | int|string |
| `tileX` | int |
| `tileY` | int |
| `volume` *(optional)* | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Every player hears it from where they stand, and a mod sound goes by name. A client's call is refused with one log line, so an event that fires on every machine never plays it twice.

### `sam_play_sound_entity(sound, uid, [volume])`

The same, at an entity's position -- and if that entity is a monster or wears an item with its own "sounds" map, that map applies.

It plays once where the entity IS; it does not follow it as it moves.

| argument | type |
|---|---|
| `sound` | int|string |
| `uid` | uid |
| `volume` *(optional)* | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Every player hears it. A sound map's sound reaches a player without the mod as the game sound it replaces. A client's call is refused with one log line.

### `sam_screen_flash(player, r, g, b, [intensity], [duration_ms])`

Flash a player's whole screen in an RGB colour that fades to nothing — the anime "impact frame". intensity 0..1 is the peak opacity. Drawn on the machine the player lives on. In JavaScript player, r, g and b are required: a missing colour is refused instead of flashing black.

| argument | type |
|---|---|
| `player` | int |
| `r` | int |
| `g` | int |
| `b` | int |
| `intensity` *(optional)* | number (0..1) |
| `duration_ms` *(optional)* | int |

**Returns:** true if accepted (boolean)

**Multiplayer:** `screen`. Shows on the screen of the player `player` names; -1 is every player in a multiplayer game. On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.

### `sam_spawn_particle(kind, tileX, tileY, [z], [scale])`

A vanilla particle burst at a tile, so a mod's own effect looks like part of the game.

| argument | type |
|---|---|
| `kind` | string — one of: `poof`, `explosion`, `bang`, `sleep` |
| `tileX` | int |
| `tileY` | int |
| `z` *(optional)* | number |
| `scale` *(optional)* | number |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A poof's scale is clamped to 0.01..655 with a warning, so every player sees the same size.


## Rewards

### `sam_get_item_category(item)`

The category of an item (WEAPON / ARMOR / GEM / POTION / SCROLL / SPELLBOOK / …). Pass an event's item_type to react by category — e.g. reward the player for identifying any GEM.

| argument | type |
|---|---|
| `item` | any — one of: `numeric item id (e.g. an event's item_type)`, `vanilla name`, `"namespace:item"` |

**Returns:** the category name (string) e.g. "GEM", or nil/undefined if unknown

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_grant_gold(player, amount)`

Add gold to a player (clamped to >= 0), syncing the client HUD.

| argument | type |
|---|---|
| `player` | int |
| `amount` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_grant_item(player, item_name, [beatitude], [status], [count])`

Give an item to a player: a vanilla name (e.g. "IRON_DAGGER") or a custom "namespace:item". The optional beatitude, status and count shape the item. A grant never asks player.on_before_item_pickup, so it cannot be vetoed.

| argument | type |
|---|---|
| `player` | int |
| `item_name` | string |
| `beatitude` *(optional)* | int (default 0; negative is cursed, positive blessed) |
| `status` *(optional)* | int (0 BROKEN to 4 EXCELLENT, default 4) |
| `count` *(optional)* | int (default 1) |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A player on another machine receives it through the game's own item packet, so this works even for a player whose game does not run S.A.M; a mod item arrives as a rock for a player whose game does not have that mod. In singleplayer, slots 1 to 3 are not players and are refused.

### `sam_item_id(name)`

Resolve an item's numeric type id — compare it against event fields like on_block's shield_type to react only to a specific item. Accepts a vanilla name or a custom "namespace:item".

| argument | type |
|---|---|
| `name` | string — one of: `vanilla ITEM name`, `"namespace:item" (custom)` |

**Returns:** the item's numeric type id (int), or nil/undefined if unknown

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_spawn_item(x, y, item_name, [status], [beatitude], [count])`

Spawn a ground item at a map tile. status, beatitude and count let you put an item back exactly as you found it — without them a stash could record that you owned a cursed, worn ring and then only ever hand back a pristine one. The uid comes back so you can move it (sam_set_position) or clear it (sam_remove_entity) later; a uid is never 0, so an older `if sam_spawn_item(...)` check still behaves as it did.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |
| `item_name` | string (a vanilla name, or a custom "namespace:item") |
| `status` *(optional)* | int (optional, default EXCELLENT; clamped BROKEN..EXCELLENT) |
| `beatitude` *(optional)* | int (optional, default 0; negative is cursed, positive blessed; clamped -100..100) |
| `count` *(optional)* | int (optional, default 1; clamped 1..1000) |

**Returns:** the spawned item's entity uid (int), or nil/undefined if the tile was invalid

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). A ground stack above 255 shows as its count modulo 256 on other players' screens (the game's own packet carries one byte), but picking it up gives the full count. Spawn smaller stacks if the display matters.


## Rules

### `sam_add_monster_stat_modifier(uid, stat, id, [add], [multiply])`

The same for one creature.

DIES WITH THE FLOOR. Monster tables are dropped on every level change, because the engine reuses entity uids and a remembered one can come to name something else. All nine stats work on a creature; SPEED scales how fast it walks, chases and flees. Refused for a uid that is not a living monster, a player's included: players have sam_add_stat_modifier.

| argument | type |
|---|---|
| `uid` | uid |
| `stat` | string — one of: `STR`, `DEX`, `CON`, `INT`, `PER`, `CHR`, `AC`, `ATTACK`, `SPEED` |
| `id` | string |
| `add` *(optional)* | number |
| `multiply` *(optional)* | number |

**Returns:** true if it took (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_add_stat_modifier(player, stat, id, [add], [multiply])`

Contribute to one of a player's computed stats. Adds are summed and multipliers multiplied ACROSS EVERY MOD, then applied as (base + adds) * multipliers — so two mods each giving +2 STR give +4, and two each halving give a quarter. Neither mod has to know the other exists. SPEED takes a multiplier only (add must be 0): a player's speed and a monster's are scaled from different bases, so an add would mean different things on each. Add is limited to +-10000 and multiply to 100.

The id is yours, and scoped to your mod: another mod using the same word gets an entry of its own, and cannot replace or remove yours. Adding again with the same id REPLACES that contribution, which makes a per-tick "recalculate my buff" loop safe, and sam_remove_stat_modifier takes back everything under it and touches nothing else. Survives floors (these are keyed by player slot, because a player's entity is rebuilt on the stairs and its uid changes) but not a new run: a new character, or a loaded save, starts with none, so re-apply anything permanent in game.on_game_start. The totals reach every S.A.M player's machine in order, since a player's own character sheet and walking speed are computed there.

| argument | type |
|---|---|
| `player` | int |
| `stat` | string — one of: `STR`, `DEX`, `CON`, `INT`, `PER`, `CHR`, `AC`, `ATTACK`, `SPEED` |
| `id` | string |
| `add` *(optional)* | number |
| `multiply` *(optional)* | number |

**Returns:** true if it took (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_clear_immunities()`

Drop every immunity YOUR MOD declared, per player, per creature and per species.

Immunities belong to the mods that set them. A creature two mods made immune stays immune until both have let go, so this never cancels another mod's.

**Returns:** how many were removed (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.

### `sam_clear_stat_modifiers([player])`

Take back every modifier YOUR MOD made, from one player, or with no argument from every player and every monster — which is what a mod's teardown wants. Other mods' contributions are left alone.

| argument | type |
|---|---|
| `player` *(optional)* | int |

**Returns:** how many were removed (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.

### `sam_clear_xp_curve()`

Take back your mod's XP curve. Levels no other mod has set go back to a flat 100; a level another mod also set keeps that mod's threshold.

**Returns:** how many of YOUR MOD's entries were removed (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.

### `sam_get_stat_modifier(player, stat, id)`

Read back what your own id currently contributes, so a mod does not have to remember. Only your mod's ids are visible to you.

| argument | type |
|---|---|
| `player` | int |
| `stat` | string — one of: `STR`, `DEX`, `CON`, `INT`, `PER`, `CHR`, `AC`, `ATTACK`, `SPEED` |
| `id` | string |

**Returns:** a table/object with add and multiply, or nil

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_xp_threshold(level)`

Read the current threshold. Answers 100 unless a mod said otherwise, so it is also how to read vanilla's number.

| argument | type |
|---|---|
| `level` | int |

**Returns:** what that level costs right now (int)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_grant_xp(player, amount)`

Give a player experience. Crossing the threshold levels them up naturally on the next tick, firing player.on_level_up, and respects any curve set with sam_set_xp_curve.

Nothing granted an arbitrary amount before this: sam_level_up adds whole levels and sam_set_stat("EXP") is an absolute write.

| argument | type |
|---|---|
| `player` | int |
| `amount` | int |

**Returns:** the player's experience afterwards, or nil (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_is_immune(uid, effect)`

Ask before wasting a cast, which is impossible in vanilla.

It answers about the MOD table only. Barony's own immunities are a switch with no way to query it, so a false here does not promise the effect will land.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** whether a mod has made it immune (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_remove_monster_stat_modifier(uid, id)`

The monster twin of sam_remove_stat_modifier.

| argument | type |
|---|---|
| `uid` | uid |
| `id` | string |

**Returns:** how many stats carried that id (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.

### `sam_remove_stat_modifier(player, id)`

Take back everything your mod contributed under one id, across every stat, leaving other mods' contributions alone.

| argument | type |
|---|---|
| `player` | int |
| `id` | string |

**Returns:** how many stats carried that id (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.

### `sam_set_immunity(player, effect, [on])`

Make a player immune to a named effect — poison, curse, polymorph, anything the game has.

Barony's own immunities are a hardcoded species switch inside setEffect with no table and no way to ask it anything. This is a table your mod owns, checked before that switch runs. Survives floors but not a new run, like the stat modifiers. If two mods make the player immune, it lasts until both turn it off.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `on` *(optional)* | boolean |

**Returns:** true if it took (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_monster_immunity(uid, effect, [on])`

Make one creature immune to a named effect.

Dies with the floor, like the monster stat modifiers and for the same reason. Use sam_set_species_immunity for something permanent. Turning it on is refused for a uid that is not a living monster; use sam_set_immunity for a player.

| argument | type |
|---|---|
| `uid` | uid |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `on` *(optional)* | boolean |

**Returns:** true if it took (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_species_immunity(species, effect, [on])`

Make every creature of a kind immune to an effect, now and later. Skeletons that cannot be poisoned, automatons that cannot be charmed.

Unlike the per-creature form this survives floors, because a species is not a uid.

| argument | type |
|---|---|
| `species` | string — one of: `human`, `rat`, `goblin`, `slime`, `troll`, `bat`, `spider`, `ghoul`, `skeleton`, `scorpion`, `imp`, `crab`, `gnome`, `demon`, `succubus`, `mimic`, `lich`, `minotaur`, `devil`, `shopkeeper`, `kobold`, `scarab`, `crystalgolem`, `incubus`, `vampire`, `shadow`, `cockatrice`, `insectoid`, `goatman`, `automaton`, `lichice`, `lichfire`, `sentrybot`, `spellbot`, `gyrobot`, `dummybot`, `bugbear`, `dryad`, `myconid`, `salamander`, `gremlin`, `revenant_skull`, `minimimic`, `monster_adorcised_weapon`, `flame_elemental`, `hologram`, `moth`, `earth_elemental`, `duck_small` |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `on` *(optional)* | boolean |

**Returns:** true if it took (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_xp_curve(level, threshold)`

How much experience the next level costs. Barony's is a flat 100 at every level — the vanilla scaling is literally commented out beside it in the engine — so every progression mod has had to fake it by writing EXP directly. The XP bar follows the curve, on clients too, and sam_level_up grants what the curve charges.

Level -1 sets the flat value for EVERY level, which is the one-line version of a slower or faster game. Naming a level overrides just that one, so a whole curve is a handful of calls. A threshold under 1 is refused: the engine grants a level on every tick that EXP reaches the threshold, so it would level the player up forever. If two mods set the same level, the most recent one is in force; each mod's entries are its own, so when one clears its curve the other's shows through.

| argument | type |
|---|---|
| `level` | int |
| `threshold` | int |

**Returns:** true if it took (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A curve declared when your mod loads reaches clients on the first floor. The XP bar and a client's own EXP stay correct above 255 on S.A.M clients; a player whose game does not run S.A.M sees EXP modulo 256 on the bar.


## Settings

### `sam_get_setting(id)`

The value one of your settings holds right now on THIS machine: what the player confirmed in Settings, else what your settings file held at load, else the default. Cheap: read it when you need it rather than caching it.

| argument | type |
|---|---|
| `id` | string |

**Returns:** the value in force: a number for a slider or number, a boolean for a toggle, a string for a dropdown or text; nil/undefined for an id this mod never registered

**Multiplayer:** `local`. Answers for the machine running the script. Per machine. The host's slider is the host's; a joiner's slider is the joiner's. A mod that needs one value everywhere reads it on the host and sends it with sam_send_packet.

### `sam_list_settings()`

Everything your mod declared with sam_register_setting and what each one holds now. min and max appear only when declared, step only for a slider, options only for a dropdown. For a debug print or a panel of your own; the game's Settings screen already shows the same rows.

**Returns:** array of { id, type, label, value, default, min?, max?, step?, options? } for this mod's settings, in registration order (array)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_register_setting(id, spec)`

Give your mod its own row in Settings > General, in a MOD SETTINGS section under your mod's name, in both the main menu and the pause menu. A slider needs min and max and takes an optional step (values are snapped to it); a toggle takes a boolean default; a dropdown needs options and defaults to the first one; a number takes an optional min and max; a text holds up to 31 characters and may leave the default out (empty). The default must fit the declaration. The value in force is what the player confirmed in Settings, else what your mod's own settings file holds from an earlier run, else the default: it is read from the file the first time each id is registered per load, so a value survives a restart and survives the mod being unloaded, and it is never in config.json. Call it at top level, next to sam_register_action; registering the same id again refreshes the declaration and keeps the current value when the new declaration still accepts it.

The player's edits are a copy until Confirm: Discard changes nothing and fires nothing, Restore Defaults stages every mod setting's default and Confirm makes it real. Read a value with sam_get_setting when you need it, or listen to mod.on_setting_changed; do not cache it at load. A mod with many settings makes the General tab long (it scrolls).

| argument | type |
|---|---|
| `id` | string |
| `spec` | table — one of: `type: "slider" | "toggle" | "dropdown" | "number" | "text"`, `label: string`, `tip: string (tooltip)`, `default: number | boolean | string`, `min, max: number (slider: required, within +-1e9, at most 1000000 steps apart; number: optional, finite)`, `step: number (slider: 0 = continuous)`, `options: array of strings (dropdown)` |

**Returns:** true if the setting exists now (boolean); false, with the reason in the log, for a bad declaration

**Multiplayer:** `all`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns false. Call it on the host or when your mod loads. A host call applies on every S.A.M player's machine, and a player who joins later gets the host's whole table.

### `sam_set_setting(id, value)`

Change one of your settings from the script: validated against the declaration (a slider is snapped to its step), written to your settings file at once, and announced with mod.on_setting_changed with source "script". Setting the value already in force is true and silent. The Settings screen shows the new value the next time it opens.

If the player has the Settings window open and has moved that same setting, their Confirm wins over your call.

| argument | type |
|---|---|
| `id` | string |
| `value` | number | boolean | string |

**Returns:** true if the value is now in force (boolean); false, with the reason in the log, for an unregistered id, a value of the wrong kind, a slider or number outside its range, a dropdown value that is not one of its options, or a text longer than 31 characters

**Multiplayer:** `local`. Answers for the machine running the script. Changes this machine's value only.


## Sound & music

### `sam_get_music()`

What is playing right now: one of a mod's tracks ("mymod:boss") or a vanilla name ("mines02", "shop").

A vanilla track a mod replaced answers with the VANILLA name, since that is what the game asked for. Works on clients: it reports what this machine is playing.

**Returns:** the track playing on this machine, or nil (string)

**Multiplayer:** `local`. Answers for the machine running the script.

### `sam_list_music()`

The ids sam_play_music and a monster's "music" accept. /sam_music in the console prints the same, plus the vanilla names a replacement can target.

**Returns:** every track a loaded mod declares (table/array)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_list_sounds()`

The ids sam_play_sound accepts. /sam_sounds in the console prints the same; /sam_sounds vanilla <text> searches the game's own.

**Returns:** every sound a loaded mod adds (table/array)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_play_music(track, [fade_seconds], [loop], [persist])`

Take over the music for every player -- a boss fight, a cutscene, a victory sting. track is a file in your mod's music/ folder: "boss" from your own scripts, or "mymod:boss". It plays until sam_stop_music(), or until the floor changes unless persist is true.

fade_seconds defaults to 1.5; 0 cuts straight to it. A track that does not loop hands the music back when it ends, which makes a sting. A monster can have a theme with no script at all (its "music" field), and a script's track outranks it.

| argument | type |
|---|---|
| `track` | string |
| `fade_seconds` *(optional)* | number |
| `loop` *(optional)* | boolean |
| `persist` *(optional)* | boolean |

**Returns:** true if it started (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Every player hears it, including a player who joins while it plays, and it is dropped when a game ends or a new one starts. "Until the floor changes" really means the floor; it used to last until the area name changed.

### `sam_stop_music()`

Hand the music back. The game crossfades to whatever it would have been playing.

**Returns:** whether a script's track was playing (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_stop_sound(sound)`

Stop every playing copy of one of your sounds, for every player. The way to end a sound declared with "loop": true, which otherwise plays until the floor changes.

| argument | type |
|---|---|
| `sound` | string |

**Returns:** how many copies stopped on this machine (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0. Reaches S.A.M players in order after the play it stops.


## Spells

### `sam_cast_spell(player, spell)`

Immediately FIRE a spell/bolt from a player in the direction they face (free, no mana). Great for 'shoot on block'. Accepts the number sam_get_tome_spell returns, like sam_cast_spell_at and sam_cast_spell_pos. Don't call from an on_spell_cast handler.

| argument | type |
|---|---|
| `player` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** true if a projectile spawned (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_cast_spell_at(player, target_uid, spell)`

Fire a spell AIMED at an entity (aims the bolt toward it) instead of straight ahead. Free cast, host-only.

| argument | type |
|---|---|
| `player` | int |
| `target_uid` | uid |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** the missile's uid (int), or nil/undefined

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_cast_spell_pos(player, tile_x, tile_y, spell)`

Fire a spell aimed at a map tile. Free cast, host-only.

| argument | type |
|---|---|
| `player` | int |
| `tile_x` | int |
| `tile_y` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** the missile's uid (int), or nil/undefined

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_spells(player)`

List the spells a player currently knows.

| argument | type |
|---|---|
| `player` | int |

**Returns:** array/table of the spells the player knows (a mod's spell as its "namespace:spell", a vanilla one as its internal name), or nil if this machine cannot see that player's spells

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine is answered from a copy their own game sends a moment after joining and whenever it changes (at most five times a second). nil means this machine cannot see that player's things: the host before their game has reported, or a player whose game does not run S.A.M. That 'not reported yet' moment comes at the start of EVERY run, not only after a join: the host drops the last character's copy when a new run starts and that player's game sends the new one a moment later, so read another player's backpack or spells from a timer or a later event, never from game.on_game_start. A removal is no more visible than any other change until that next report, so never write 'if they still have it, give the reward' without a flag of your own. A remote player's spells are reported by NAME, so they are right whatever order the mods loaded in.

### `sam_get_tome_spell(uid)`

Which spell a spellbook or a spell tome contains. This is the bridge from an item to the spell functions: pair it with sam_grant_spell to teach whatever a book holds without hardcoding the pairing. Anything that is not a book returns nil.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** the spell id (number), nil if the item teaches no spell, or false when the uid names no item this machine can see

**Multiplayer:** `read`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own. Returns false, not nil, when the uid names no item this machine can see, because nil already means the item teaches no spell. On the host it reads a remote player's items from the copy their game reports.

### `sam_grant_spell(player, spell)`

Teach a player a spell: a vanilla SPELL_ name, a custom "namespace:spell", or the number sam_get_tome_spell returns. player.on_spell_learned fires once, on the host, when the spell is actually learned.

| argument | type |
|---|---|
| `player` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** true on success (boolean); for a player on another machine, true means the call was sent

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `player`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Carried to the player's own machine, where their spell list lives. Works for every player whose game runs S.A.M; a player without S.A.M is refused with a warning. When granting to another player prefer the NAME, because a mod's spell numbers follow mod load order. If their game refuses (the spell is already known), the reason shows in the host's log.

### `sam_monster_cast_spell(uid, spell)`

Make a monster (or a companion) cast a spell along its own facing. Free cast, host-only.

| argument | type |
|---|---|
| `uid` | uid |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** the missile's uid (int), or nil/undefined

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_player_knows_spell(player, spell)`

Check whether a player already knows a spell (vanilla or custom).

| argument | type |
|---|---|
| `player` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** whether the player knows it (boolean), or nil if this machine cannot see that player's spells

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine is answered from a copy their own game sends a moment after joining and whenever it changes (at most five times a second). nil means this machine cannot see that player's things: the host before their game has reported, or a player whose game does not run S.A.M. That 'not reported yet' moment comes at the start of EVERY run, not only after a join: the host drops the last character's copy when a new run starts and that player's game sends the new one a moment later, so read another player's backpack or spells from a timer or a later event, never from game.on_game_start. A removal is no more visible than any other change until that next report, so never write 'if they still have it, give the reward' without a flag of your own.

### `sam_remove_spell(player, spell)`

Un-learn a spell. Also takes the spell's item out of the backpack and hotbar (and a vanilla spell's shapeshift twin), and drops it from the selected, hotbar-alternate and quick-cast slots. It happens at the end of the tick, so removing a spell inside a cast or item handler is safe. Until then sam_player_knows_spell still answers true, and a sam_grant_spell of the same spell CANCELS the queued removal -- so "remove the vanilla spell, then grant my own version of it" in one handler leaves the player with the spell rather than with neither. The counterpart to sam_grant_spell.

| argument | type |
|---|---|
| `player` | int |
| `spell` | string — one of: `vanilla SPELL_ name`, `"namespace:spell" (custom)` |

**Returns:** true if the player knew it and it is queued for removal (boolean); for a player on another machine, true means the call was sent

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `player`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Carried to the player's own machine. Works for every player whose game runs S.A.M; a player without S.A.M is refused with a warning.


## Status effects

### `sam_apply_effect(player, effect, ticks, [strength])`

Apply a status effect to a player for N ticks (50 ticks = 1s). Optional strength sets the tier/magnitude for effects that carry one (e.g. GROWTH stacks) — omit it for the plain default. Targets the player, never a monster.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `ticks` | int |
| `strength` *(optional)* | int |

**Returns:** true unless immune/refused (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_clear_effects(player)`

Strip EVERY active status effect from a player at once — buffs and debuffs, vanilla and custom.

| argument | type |
|---|---|
| `player` | int |

**Returns:** how many effects were cleared (int)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns 0.

### `sam_get_effect_duration(player, effect)`

How many ticks of an effect are left (50 = 1s), so a debuff can scale or decay by time remaining.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** remaining ticks (int; 0 if inactive, -1 if permanent; nil on a client)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). A client does not count effect timers (it only knows whether an effect is on), so its call is refused and returns nil rather than a misleading 0.

### `sam_get_effect_strength(player, effect)`

The effect's strength/magnitude for effects that store one (GROWTH tiers, potion STR).

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** strength/tier (int; 0 if inactive)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_get_effects(player)`

Every active effect on a player at once: react to "any debuff" or strip all buffs without polling each effect by name. Names match the effect events in Lua and JavaScript alike: the lowercase vanilla name ("poisoned"), a mod's "namespace:effect" id, or "CUSTOM:<id>" for an unnamed custom slot, so a list from here can be compared directly against an event's effect_name.

Lua used to name vanilla effects in capitals here. A Lua script comparing against "POISONED" must compare against "poisoned" now.

| argument | type |
|---|---|
| `player` | int |

**Returns:** array/table of { name, ticks, strength }

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns an empty table (an empty array in JavaScript). A client does not count effect timers, so the ticks it could give would be wrong; its call is refused.

### `sam_has_effect(player, effect)`

Check whether a player currently has a status effect.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** whether the effect is active (boolean)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_remove_effect(player, effect)`

Clear a status effect from a player.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_effect_duration(player, effect, ticks)`

Retime an ALREADY-ACTIVE effect in place, without re-triggering it. No-op if the effect isn't active (never spawns a fresh one). 50 ticks = 1s.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `ticks` | int |

**Returns:** true if the effect was active and retimed (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_effect_strength(player, effect, strength)`

Change the magnitude/tier of an ALREADY-ACTIVE effect while keeping its remaining duration. strength 1-255.

| argument | type |
|---|---|
| `player` | int |
| `effect` | string — one of: `ASLEEP`, `POISONED`, `STUNNED`, `CONFUSED`, `DRUNK`, `INVISIBLE`, `BLIND`, `GREASY`, `MESSY`, `FAST`, `PARALYZED`, `LEVITATING`, `TELEPATH`, `VOMITING`, `BLEEDING`, `SLOW`, `MAGICRESIST`, `MAGICREFLECT`, `VAMPIRICAURA`, `SHRINE_RED_BUFF`, `SHRINE_GREEN_BUFF`, `SHRINE_BLUE_BUFF`, `HP_REGEN`, `MP_REGEN`, `PACIFY`, `POLYMORPH`, `KNOCKBACK`, `WITHDRAWAL`, `POTION_STR`, `SHAPESHIFT`, `WEBBED`, `FEAR`, `MAGICAMPLIFY`, `DISORIENTED`, `SHADOW_TAGGED`, `TROLLS_BLOOD`, `FLUTTER`, `DASH`, `DISTRACTED_COOLDOWN`, `MIMIC_LOCKED`, `ROOTED`, `NAUSEA_PROTECTION`, `CON_BONUS`, `PWR`, `AGILITY`, `RALLY`, `MARIGOLD`, `ENSEMBLE_FLUTE`, `ENSEMBLE_LYRE`, `ENSEMBLE_DRUM`, `ENSEMBLE_LUTE`, `ENSEMBLE_HORN`, `LIFT`, `GUARD_SPIRIT`, `GUARD_BODY`, `DIVINE_GUARD`, `NIMBLENESS`, `GREATER_MIGHT`, `COUNSEL`, `STURDINESS`, `BLESS_FOOD`, `PINPOINT`, `PENANCE`, `SACRED_PATH`, `DETECT_ENEMY`, `BLOOD_WARD`, `TRUE_BLOOD`, `DIVINE_ZEAL`, `MAXIMISE`, `MINIMISE`, `WEAKNESS`, `INCOHERENCE`, `OVERCHARGE`, `ENVENOM_WEAPON`, `MAGIC_GREASE`, `COMMAND`, `MIMIC_VOID`, `CURSE_FLESH`, `NUMBING_BOLT`, `DELAY_PAIN`, `SEEK_CREATURE`, `TABOO`, `COURAGE`, `COWARDICE`, `SPORES`, `ABUNDANCE`, `GREATER_ABUNDANCE`, `PRESERVE`, `MIST_FORM`, `FORCE_SHIELD`, `LIGHTEN_LOAD`, `ATTRACT_ITEMS`, `RETURN_ITEM`, `DEMESNE_DOOR`, `REFLECTOR_SHIELD`, `DIZZY`, `SPIN`, `CRITICAL_SPELL`, `MAGIC_WELL`, `STATIC`, `ABSORB_MAGIC`, `FLAME_CLOAK`, `DUSTED`, `NOISE_VISIBILITY`, `RATION_SPICY`, `RATION_SOUR`, `RATION_BITTER`, `RATION_HEARTY`, `RATION_HERBAL`, `RATION_SWEET`, `GROWTH`, `THORNS`, `BLADEVINES`, `BASTION_MUSHROOM`, `BASTION_ROOTS`, `FOCI_LIGHT_PEACE`, `FOCI_LIGHT_JUSTICE`, `FOCI_LIGHT_PROVIDENCE`, `FOCI_LIGHT_PURITY`, `FOCI_LIGHT_SANCTUARY`, `STASIS`, `HP_MP_REGEN`, `DISRUPTED`, `FROST`, `MAGICIANS_ARMOR`, `PROJECT_SPIRIT`, `DEFY_FLESH`, `PINPOINT_DAMAGE`, `SALAMANDER_HEART`, `DIVINE_FIRE`, `HEALING_WORD`, `HOLY_FIRE`, `SIGIL`, `SANCTUARY`, `DUCKED` |
| `strength` | int |

**Returns:** true if the effect was active and changed (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.


## Terrain

### `sam_find_entities(x, y, radiusTiles, [kind])`

Entities of a KIND near a tile. This is the gap sam_get_nearby_entities leaves: that one skips anything which is not a monster or a player, so doors, chests, levers, gold and dropped items were invisible to scripts. A kind you spell wrong is logged by name and returns nothing, rather than returning the empty list that looks exactly like "nothing nearby"; each distinct wrong word is reported once. It never returns the engine's shared marker uids (particles, flames, a client's own local effects), so every uid it returns works with the other functions.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |
| `radiusTiles` | number |
| `kind` *(optional)* | string — one of: `any`, `player`, `monster`, `item`, `gold`, `door`, `chest`, `fountain`, `sink`, `switch`, `gate`, `ladder`, `portal`, `boulder`, `gib`, `other` |

**Returns:** array of uids

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Never returns the engine's shared marker uids (particles, flames, a client's own local effects), so every uid it returns can be used with the other functions. Each machine answers from its own copy, which on a client can lag the host.

### `sam_get_container_items(uid)`

What is inside a chest, or what a creature is carrying.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** array of tables { type, name, count, status, beatitude, identified } or nil

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_light_at(x, y, [player])`

How lit a tile is, computed exactly the way the engine computes it, so the number you get back is the number monster vision thresholds on rather than an approximation of it. Barony keeps one SHARED lightmap holding light that is there for everyone (a wall torch, a lit room) plus one per camera that also holds that player's own glow. This reads the shared one by default, because that is the one the AI reads. Pass a player index if you want what that player's screen actually shows instead.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |
| `player` *(optional)* | int |

**Returns:** a light level from 0 to 255 (number)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. The shared lightmap the monster AI reads exists only on the host, so a client's call is refused with a warning. It answers with nothing at all rather than 0, because 0 is a real light level: pitch darkness.

### `sam_get_tile(x, y)`

Read one map tile. Liquid comes from the FLOOR tile, and the engine decides which tiles are liquid from their image filename — so a mod's own tile named "...lava..." reports as lava here too.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |

**Returns:** table { wall, floor, ceiling, solid, water, lava, walkable } or nil for a tile off the map

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_is_spawnable(x, y)`

Is this a sane place to put something: in bounds, not inside a wall, not lava. Check before spawning instead of dropping a monster into rock.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |

**Returns:** boolean

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_is_tile_diggable(x, y)`

Whether the map's rules allow digging here: it returns false for water and lava, the Hell and fortress edges, and any tile marked no-dig. It does NOT check that there is a wall to dig, so it is true on ordinary open floor too. Pair it with sam_get_tile if you need something solid to be there. Out-of-bounds coordinates return false rather than reading past the map.

| argument | type |
|---|---|
| `x` | int (tile) |
| `y` | int (tile) |

**Returns:** true if this tile's terrain permits digging (boolean)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_line_of_sight(x1, y1, x2, y2, [blockedByEntities])`

Can a straight line get from A to B? This is the engine's own trace, so it agrees with what is drawn — unlike plain distance, which sees through solid rock.

| argument | type |
|---|---|
| `x1` | int |
| `y1` | int |
| `x2` | int |
| `y2` | int |
| `blockedByEntities` *(optional)* | boolean |

**Returns:** visible, blockedX, blockedY (blocked coords are -1 when visible)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_set_tile(x, y, layer, tileId)`

Write one map tile — dig a passage, wall something in, flood a room. Refuses out of bounds rather than corrupting the map array. Check sam_tiles_connected afterwards if the edit could seal the exit.

| argument | type |
|---|---|
| `x` | int |
| `y` | int |
| `layer` | int (0=floor, 1=wall, 2=ceiling) |
| `tileId` | int |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Edits reach every S.A.M player in the order they were made, including a player who finishes loading late. A player whose game does not run S.A.M sees only dug walls (a wall set to 0), not placed walls or floor and ceiling changes. Tile ids only mean the same tile when every player has the same mods.

### `sam_tiles_connected(x1, y1, x2, y2, [flying])`

Can something WALK (or fly) from A to B at all? The softlock check: after a mod edits terrain, ask whether the exit is still reachable before committing.

| argument | type |
|---|---|
| `x1` | int |
| `y1` | int |
| `x2` | int |
| `y2` | int |
| `flying` *(optional)* | boolean |

**Returns:** boolean

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Answers correctly on a client after the host changes terrain.


## Timers

### `sam_cancel_timer(id)`

Cancel a pending timer by id (for the calling mod).

| argument | type |
|---|---|
| `id` | string |

**Returns:** nothing

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_set_repeating_timer(id, interval_ticks, callback)`

Run `callback` (a function) every interval_ticks until cancelled.

| argument | type |
|---|---|
| `id` | string |
| `interval_ticks` | int |
| `callback` | any |

**Returns:** nothing

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. Timers run on the host only. Every machine drops all timers when a run starts, so set them from game.on_game_start or later, never from top-level script code. A timer callback is not an event: a screen call inside it with no player shows on the host's screen.

### `sam_set_timer(id, delay_ticks, callback)`

Run `callback` (a function) once after delay_ticks (50/sec). Replaces any timer with the same id.

| argument | type |
|---|---|
| `id` | string |
| `delay_ticks` | int |
| `callback` | any |

**Returns:** nothing

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. Timers run on the host only. Every machine drops all timers when a run starts, so set them from game.on_game_start or later, never from top-level script code. A timer callback is not an event: a screen call inside it with no player shows on the host's screen, so keep the player in a local and pass it.


## Truth

### `sam_get_ac(uid)`

Armor class as the damage formula sees it, gear included.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** number

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_effective_stat(uid, stat)`

A stat as the game actually uses it — gear, effects and curses folded in — rather than the raw number on the sheet.

| argument | type |
|---|---|
| `uid` | uid |
| `stat` | string — one of: `STR`, `DEX`, `CON`, `INT`, `PER`, `CHR`, `HP`, `MAXHP`, `MP`, `MAXMP`, `GOLD`, `HUNGER`, `LEVEL`, `LVL`, `EXP` |

**Returns:** number

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`uid`), and anything else is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_flag(flag)`

Read a lobby setting the host chose at game start. Lets a mod adapt to the run it is actually in — skip a hunger mechanic when hunger is off, or scale difficulty when hardcore is on.

| argument | type |
|---|---|
| `flag` | string — one of: `cheats`, `friendlyfire`, `minotaurs`, `hunger`, `traps`, `hardcore`, `classic`, `keep_inventory`, `lifesaving`, `assist_items` |

**Returns:** true or false, or nil if the flag name is unknown (the valid list is logged)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_monster_name(uid)`

For a mod's custom monster this is the variant name it was given ("Rathalos"). A plain vanilla creature carries an empty variant name, so this falls back to the species name and never hands a script an empty string. DISPLAY only: a named creature answers with its own epithet (a shopkeeper is "Adrian"), and no call takes that back. To clone or patch what you are looking at, use sam_get_monster_type, which returns the species name that sam_spawn_monster and sam_patch_monster accept.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** the creature's DISPLAY name (string; nil if not a creature)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_monster_type(uid)`

Identify a creature by name instead of the raw integer in an event payload. NOTE this is the BASE type: a custom monster is a variant of a vanilla species, so a mod's "Rathalos" built on a bat answers "bat". Use sam_get_monster_name for the variant's own name, or sam_monster_has_trait to tell modded creatures apart.

| argument | type |
|---|---|
| `uid` | uid |

**Returns:** the species name, e.g. "skeleton" (string; nil if not a creature)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_get_seed()`

Read the seed identifying this run. Pair it with sam_random when you want per-run variety.

**Returns:** the run's unique game key (number)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_skill(uid, skill, [effective])`

A proficiency rank. Accepts both spellings — "PRO_SWORD" (the class schema) and "sword" (what player.on_proficiency_increased hands you). effective (default true) includes the equipment bonus the game actually uses; pass false for the raw trained rank. Ranks were completely unreadable before this, even though the framework has always fired the event.

| argument | type |
|---|---|
| `uid` | uid |
| `skill` | string |
| `effective` *(optional)* | boolean |

**Returns:** a proficiency rank from 0 to 100 (number)

**Multiplayer:** `read`. The host can read every creature; a client can read only its own player's uid (`uid`), and anything else is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_is_enemy(uid_a, uid_b)`

Would these two fight? The engine's own allegiance answer, so charm, race and faction are all accounted for.

| argument | type |
|---|---|
| `uid_a` | uid |
| `uid_b` | uid |

**Returns:** boolean

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_is_friend(uid_a, uid_b)`

The other side of sam_is_enemy — allies, followers and charmed creatures.

| argument | type |
|---|---|
| `uid_a` | uid |
| `uid_b` | uid |

**Returns:** boolean

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_is_ghost(player)`

Whether a dead player is walking around as a ghost. Worth checking before granting items or applying effects, since a ghost is not an ordinary player.

| argument | type |
|---|---|
| `player` | int |

**Returns:** true if that player is currently a ghost (boolean)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_is_spirit_ghost(player)`

The stricter ghost test: a spirit ghost specifically, rather than any ghost state.

| argument | type |
|---|---|
| `player` | int |

**Returns:** true if that player is a spirit ghost (boolean)

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_monster_has_trait(uid, trait)`

Reads back what the mod declared in JSON. Without this a mod can SAY a monster is undead and the engine agrees, but the mod's own script cannot ask — so a "bonus vs undead" rule had no way to test for undead. False for every vanilla monster, so it is a no-op without a mod.

| argument | type |
|---|---|
| `uid` | uid |
| `trait` | string |

**Returns:** boolean

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript.

### `sam_random(stream, min, max)`

Deterministic random drawn from a named stream owned by your mod. The same run seed, the same stream and the same order of calls give the same number, which ordinary random() cannot promise.

| argument | type |
|---|---|
| `stream` | string (any name; each stream is independent) |
| `min` | int |
| `max` | int |

**Returns:** an integer in [min, max]

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.

### `sam_random_chance(stream, percent)`

Rolls a percentage chance. 0 or less is always false and 100 or more is always true, so you never have to clamp. Drawn from one of your mod's named streams, so the same run and the same call order give the same rolls.

| argument | type |
|---|---|
| `stream` | string |
| `percent` | number (0 to 100) |

**Returns:** true or false

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nothing at all -- no value in Lua, `undefined` in JavaScript. Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.

### `sam_random_float(stream)`

A deterministic fraction from one of your mod's named streams. The same run, the same stream and the same call order give the same number.

| argument | type |
|---|---|
| `stream` | string (any name; each stream is independent) |

**Returns:** a number from 0.0 to 1.0

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.

### `sam_random_from_list(stream, list)`

Picks one entry at random, deterministically. Note that Lua lists start at 1 and JavaScript arrays start at 0; each version follows its own language, so the same list gives the same element in both.

| argument | type |
|---|---|
| `stream` | string |
| `list` | table/array |

**Returns:** one element, or nil for an empty list

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.

### `sam_random_weighted(stream, weights)`

Picks a key with probability proportional to its weight, for loot tables and spawn tables. Weights need not add up to anything in particular, and a weight of zero or less can never come up. Number keys count by their text in Lua and JavaScript alike ({[1]=5} and {1:5} both mean the key "1"), and the key comes back as a string.

| argument | type |
|---|---|
| `stream` | string |
| `weights` | table/object of key to weight |

**Returns:** one key, or nil if no weight is positive

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Draws happen on the host only and are not synchronized between machines: roll on the host and send the result with sam_send_packet if a client needs it.


## World

### `sam_apply_force(uid, force, angle, [ticks])`

Shove an entity, using the engine's own knockback. The angle is a Barony yaw in radians, the same number sam_get_facing gives you, so away-from-you is atan2(theirY - myY, theirX - myX). This is deliberately not a raw velocity write: both act functions throw velocity away unless the knockback effect is active, and a player takes the impulse in a completely different field from a monster, so a hand-written version does nothing at all to the two targets you would actually aim it at. The optional ticks is how long the stagger lasts and defaults to 30, which is what the engine uses. Force is capped at 7 because a single step bigger than that can jump clean over a wall instead of hitting it. Returns false, with a logged reason, for a creature that refuses knockback outright: liches, minotaurs, the devil and shopkeepers are immune, and the engine's own knockback does nothing to them either. It also returns false for anything whose behaviour never reads velocity at all, which includes the decorative portals sam_spawn_portal creates: use sam_move_entity on those. The angle is wrapped into 0 to 2 pi for you, because the network carries it as a fixed-point number that overflows past about 128 radians.

| argument | type |
|---|---|
| `uid` | int |
| `force` | number (0.6 is an arrow hit, 1.4 a strong one, 7 the ceiling) |
| `angle` | number (radians, same as sam_get_facing) |
| `ticks` *(optional)* | int |

**Returns:** true if something will act on the shove (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A resting gold bag moves too (it used to do nothing on any machine), and a shoved item or bag slides on every player's screen and ends where the host's copy comes to rest.

### `sam_can_stand(uid, tile_x, tile_y)`

Ask whether THIS entity could stand on that tile. Different from sam_is_spawnable, which only reads the map and cannot see other entities or the asker's own collision profile: levitation, body size and the pass-through set all change the answer. Check with this before sam_set_position instead of dropping a monster inside a wall. True is necessary but not sufficient for a player teleport, which applies extra rules of its own (no teleporting on the minotaur levels, and MFLAG_DISABLETELEPORT maps).

| argument | type |
|---|---|
| `uid` | int |
| `tile_x` | int |
| `tile_y` | int |

**Returns:** true if that entity would fit at that tile (boolean); false if it is blocked, or the tile is off the map, or the uid is gone; nil on a client

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). The entity grid this test reads exists only on the host, so a client's call is refused with a warning and returns nil ("cannot answer here") rather than a confident false.

### `sam_companion_punch(uid)`

Make a companion THRUST forward for a few ticks — the punch motion. Call it repeatedly on a fast repeating timer (e.g. every 3 ticks) during an ability to read as a continuous ORA-ORA flurry. Purely visual on the companion itself; combine with sam_cast_spell (forward projectile + real damage) and/or sam_get_nearby_entities + sam_deal_damage for the hits.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true if uid is a live companion (boolean); false otherwise

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. The lunge is animated on every S.A.M player's machine.

### `sam_get_exit_position()`

Where the ladder or portal off this floor is, found the same way the game's own dowsing does it. In JavaScript this returns an array.

**Returns:** tile x, y of the way onward, or nil if none was found

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_inventory(player)`

List a player's inventory. Use each item's uid with the item functions and sam_remove_item. name is the vanilla internal name, or a mod item's own "namespace:item" id (in Lua and JavaScript alike), so it can be told apart from other mod items and fed back into sam_grant_item. equipped is true only for the exact item being worn, so a spare identical ring in the bag does not count; it agrees with sam_is_item_equipped.

| argument | type |
|---|---|
| `player` | int |

**Returns:** a list of items, each { uid, type, name, count, beatitude, status, identified, equipped }; empty for an invalid player; nil if this machine cannot see that player's backpack

**Multiplayer:** `read`. The host can read every player; a client can read only its own player (`player`), and asking about another is refused with a one-time warning and returns nil (undefined in JavaScript). On the host, a player on another machine is answered from a copy their own game sends a moment after joining and whenever it changes (at most five times a second). nil means this machine cannot see that player's things: the host before their game has reported, or a player whose game does not run S.A.M. That 'not reported yet' moment comes at the start of EVERY run, not only after a join: the host drops the last character's copy when a new run starts and that player's game sends the new one a moment later, so read another player's backpack or spells from a timer or a later event, never from game.on_game_start. A removal is no more visible than any other change until that next report, so never write 'if they still have it, give the reward' without a flag of your own. The uids a remote player's list gives you work with every item function: readers answer from the host's copy, writers are carried to that player's machine. A remote player's item crosses as a type NUMBER, and a mod's item numbers follow the order that machine's mods loaded, so the host names it from its own table: right when both machines load the same mods in the same order, wrong otherwise. An item whose type this host has no definition for is left out of that player's list entirely rather than guessed at.

### `sam_get_map_flags()`

The rules this particular map sets. Worth checking before a mod grants levitation or teleports someone, because a map that forbids it will simply undo your effect and the player will not know why.

**Returns:** a table/object of booleans (no_digging, no_teleport, no_levitation, no_opening, no_messages, no_hunger, gen_adjacent) plus perimeter_gap, which is a tile count

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_map_seed()`

The per-floor generation seed, which is the same number on the host and on every client because a client rebuilds the floor from it. Distinct from sam_get_seed, which identifies the whole run.

**Returns:** the seed this floor was generated from (number)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_playable_bounds()`

The rectangle the level generator is actually allowed to use, which excludes the perimeter gap some maps reserve. A spawner that ignores this can place things inside the outer wall. In JavaScript this returns an array.

**Returns:** x1, y1, x2, y2 in tiles, upper bounds exclusive

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_player_uid(player)`

Get a player's entity uid, so the uid-based world-ops (get/set position) can act on that player's body.

| argument | type |
|---|---|
| `player` | int |

**Returns:** the player's entity uid (int), or nil/undefined if not in-game

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_get_position(uid)`

Read any entity's map-tile position (player, monster or ground item). Get a player's uid with sam_get_player_uid.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** tile x, tile y (two values in Lua; an [x, y] array in JS), or nil/undefined if the uid is gone

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's own copy of the entity, which on a client is interpolated between the host's updates and can lag it by a fraction of a second. Two machines can therefore answer slightly differently at the same moment: decide anything that depends on the exact number on the host and send the verdict with sam_send_packet, rather than working it out inside on_packet.

### `sam_is_damage_immune(uid)`

Whether a script has made this entity immune to damage with sam_set_damage_immune.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true or false (boolean); nil on a client

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). The immunity set exists only on the host, so a client's call returns nil rather than a confident false.

### `sam_is_dark_level()`

Whether this floor is one of the unlit ones.

**Returns:** true on a dark floor (boolean)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_move_entity(uid, dx, dy)`

Nudge an entity by a relative distance, sliding along whatever it runs into rather than stopping dead or passing through. The answer is the distance it MANAGED, so 0 means something is right there and 0.3 out of a requested 2 means it hit a corner: a plain true or false would have hidden the difference. Distances are in tiles, like every other spatial call here, and a long move is walked in short steps so it cannot skip over a wall: the engine's collision test only looks at where you land, not at the path, so a single two-tile step used to step clean over a one-tile wall and report the whole distance as clear. A ground item or gold bag is woken, so it settles exactly like a dropped item (drops to the floor, falls into a pit, floats on water), in singleplayer too. Use sam_set_position to teleport, or sam_apply_force to shove.

| argument | type |
|---|---|
| `uid` | int |
| `dx` | number (tiles, can be fractional) |
| `dy` | number (tiles, can be fractional) |

**Returns:** how far it actually moved, in tiles (number), or nil/undefined if the uid is refused

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). On a connected player it really moves them: their own machine applies the move through its own collision, and the returned distance is what the host computed. A player whose game does not run S.A.M is corrected with the engine's own position packet, which can occasionally be lost. A ground item or gold bag moves for every player. Moving a prop Barony never updates on a client (a gate, a torch) reaches players who run S.A.M; a player without S.A.M keeps seeing the old spot, and the log says so once.

### `sam_remove_entity(uid)`

Remove a non-player world entity by uid: a sam_spawn_portal marker, a spawned monster, a companion, a ground item, etc. Refuses players (use the normal death/teleport paths for those). Frees any light the entity owned, and closes the chest UI first if it is a chest somebody has open. A follower is also taken off its leader's follower list and ally HUD, on the leader's own machine too. The removal is QUEUED and happens on the next frame, so the uid still resolves for the rest of the current event. That is deliberate: your handler was called from inside the engine, which is still holding a pointer to that entity, so freeing it immediately corrupted memory.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true on success (boolean); false for an unknown uid or a player

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Removing a follower also takes it out of its leader's follower list and ally HUD, on the leader's own machine too.

### `sam_remove_item(item_uid)`

Remove a whole item stack from a player's inventory by its uid (from sam_get_inventory). Refuses an equipped item; unequip it first. The removal is queued and happens at the end of the tick, so it is safe inside an item event.

| argument | type |
|---|---|
| `item_uid` | int |

**Returns:** true on success (boolean); false if the item is missing or currently equipped

**Multiplayer:** `owner`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by `item_uid`) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns false. Call it on the host. An item uid that sam_get_inventory(p) gave you for a player on another machine is carried to that player's machine and changed there, so true means the change was SENT; if their game refuses it (the item was used up, it is equipped, it is over the stack limit) the reason shows in the host's log. The host's copy shows the change after that player's next report, a few ticks later, so reading it back in the same tick still gives the old value. Do NOT call one of these every tick for another player's item: every call is carried over the reliable channel whether or not it changes anything, so call it when the value changes. If the item is one that player is WEARING, the host's own copy of it (the one combat, AC and sam_can_unequip read) is corrected too; if they swap to a different item in the same instant the correction is dropped and logged rather than applied to the wrong item, unless the two share a type AND an appearance, which the host cannot tell apart. A player whose game does not run S.A.M is refused with a warning.

### `sam_set_chest_stash(chest_uid, [on])`

Turn an existing chest into permanent storage. Its contents then live in the player's savegame instead of on the floor, surviving descending, dying later, quitting and loading. This is the game's own void-chest storage, so the window, the networking and the save round-trip are all vanilla. Two limits worth designing around: every stash chest in a run shares ONE set of contents, and the chest window holds 12 stacks — so this is a stash, not a bank. Converting a chest that already holds loot hides that loot until you turn the stash back off; prefer converting an empty one.

| argument | type |
|---|---|
| `chest_uid` | int (from sam_find_entities with kind "chest", or the on_chest_opened event) |
| `on` *(optional)* | boolean (optional, default true) |

**Returns:** true if the chest was converted (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_damage_immune(uid, on)`

Make a player or a monster stop taking damage, or let them take damage again. It works at the one place every point of damage to a CREATURE is applied, right beside the engine's own invulnerabilities, so for a creature nothing gets through by another route. Anything else is refused and the refusal says why: chests, doors, furniture and breakable decorations do not have health in that sense, they carry their own separate pools that a hit decrements directly, and no immunity here could reach them. The hit still lands, the sound still plays and the knockback still happens; only the loss of health is stopped, which is what invulnerable means everywhere else in Barony. You could already do this from an on_before_damage handler, and still can; this costs nothing per hit and needs no bookkeeping. It is session state: never saved, and cleared on every floor and at the start of a run, because entity uids restart from 1 on each level and a leftover entry would hand your boss's invulnerability to a rat downstairs.

| argument | type |
|---|---|
| `uid` | int |
| `on` | boolean |

**Returns:** true on success (boolean); false for anything that is not a player or a monster

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_on_fire(uid, [on])`

Set something alight, or put it out with sam_set_on_fire(uid, false). The answer is whether it is burning NOW, which is deliberately not what the engine's own function returns: that one answers false for an entity that was already on fire, so a script retrying on false would retry for ever. Two things stop a fire starting and each logs its reason. An entity that is not BURNABLE will never light (turn the flag on with sam_set_entity_flag first), and skeletons, automatons, anyone in a machinist apron and anyone wearing an amulet of burning resistance are immune. One thing to know about props: the burn timer only runs for players and monsters, so a chest or a decoration you light stays lit for the rest of the level and hurts nothing. That is useful for a brazier, and the second argument is how you undo it.

| argument | type |
|---|---|
| `uid` | int |
| `on` *(optional)* | boolean (optional, defaults to true) |

**Returns:** true if the entity is on fire once the call finishes (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_set_position(uid, tile_x, tile_y)`

Move an entity to a map tile. Players go through the safe teleport path and cannot tunnel into walls; everything else is relocated and re-broadcast to clients. Anything that is not a player is placed where you asked even when the tile is blocked, because putting a decoration inside a wall alcove is a real thing mods do, but you get a logged warning: a monster dropped into a wall is stuck there for good. Call sam_can_stand first when the answer matters. A shared engine marker uid (0 or a negative number) and a limb uid are both refused with a reason.

| argument | type |
|---|---|
| `uid` | int |
| `tile_x` | int |
| `tile_y` | int |

**Returns:** true on success (boolean); false if refused (out of bounds, or a player teleport blocked by a wall)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. Items, gold bags and pinned props reach every player that can be told: S.A.M players see props Barony never updates on a client (a gate, a torch) move too, while a player without S.A.M keeps seeing the old spot and the log says so once. A teleported player on another machine does not snap back. A ground item or a bag of gold that has already come to REST stays at rest: it is placed exactly where you ask -- in mid-air, over a pit, inside a wall alcove -- and it does not fall, in any mode. Use sam_push_entity when you want it to fall or slide; that call wakes the item on purpose. An item still in flight is simply moved mid-fall and lands where it lands.

### `sam_spawn_companion(player, model_id, [scale])`

Spawn a floating COMPANION (a JoJo-style "Stand" / familiar) that renders one of your custom .vox models and trails the player a short distance behind, with a gentle hover. Follows the player every frame and faces where they face. The optional scale (default 1.0) sizes the model; it is clamped to 1.99 with a warning, the most the network can carry. Drive the punch motion with sam_companion_punch, and clear it with sam_remove_entity. It's a decorative follower (PASSABLE, no AI, does no damage on its own; pair it with sam_cast_spell / sam_deal_damage for the actual attack). Re-spawn it on each new floor (entities are cleared on descent).

| argument | type |
|---|---|
| `player` | int |
| `model_id` | string — one of: `a registered custom model id, e.g. "mymod:star_platinum"` |
| `scale` *(optional)* | number |

**Returns:** the new companion's entity uid (int), or nil/undefined (bad player / unregistered model / on a client)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Every S.A.M player sees it follow and animate. A player whose game does not run S.A.M cannot draw a companion's mod model at all.

### `sam_spawn_monster(tile_x, tile_y, monster_name, [shop_type])`

Summon a monster at a map tile. "shopkeeper" makes a working shop; the optional shop_type (0-14) picks the store kind.

| argument | type |
|---|---|
| `tile_x` | int |
| `tile_y` | int |
| `monster_name` | string — one of: `vanilla monster name, e.g. "skeleton", "shopkeeper"` |
| `shop_type` *(optional)* | int |

**Returns:** the new monster's uid (int), or nil/undefined if the name is unknown or the tile is blocked

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript).

### `sam_spawn_portal(tile_x, tile_y)`

Spawn a purely DECORATIVE portal (the swirling vortex) at a map tile: it animates and glows but is never interactive and never sends anyone to the next floor. Walkable, so a player can stand on it. Returns the uid so you can move it (sam_set_position) or clear it (sam_remove_entity), e.g. a portal-gun marker. In JavaScript both tile arguments are required.

| argument | type |
|---|---|
| `tile_x` | int |
| `tile_y` | int |

**Returns:** the new portal's entity uid (int), or nil/undefined if the tile is out of bounds

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Every player sees it (it used to show on the host only).

### `sam_travel_to_level(floor, [opts])`

Send the party to any floor, including BACK UP, which the game otherwise never does: a ladder only ever counts upward, so before this no hub, home base or shop you walk back to was possible. The trip is deferred exactly as a ladder defers it, so it is safe to call from inside an event handler. Refused, with a logged reason, on a client, while another level change is already under way, or before a game has started. Nothing on the old floor is preserved: floors regenerate from the map seed, so put anything that must survive in a stash chest or in sam_world_save. opts.secret follows one rule in both runtimes: in Lua, { secret = 0 } means not secret, as in JavaScript.

| argument | type |
|---|---|
| `floor` | int (absolute floor number, 0-100) |
| `opts` *(optional)* | table/object (optional) — { secret = true } reads the floor from the secret levels list |

**Returns:** true if the trip was accepted (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. The engine's periodic level-change reminder is held back while a change is pending, so a player cannot be sent to the wrong floor. In Lua, opts.secret = 0 now means not secret, as it does in JavaScript.


## Your own logic

### `sam_attach_behavior(uid, behavior)`

Attach one of your registered behaviours to a live monster. It runs AFTER vanilla AI each frame rather than replacing it, so the creature still fights and paths normally and your code layers on top.

| argument | type |
|---|---|
| `uid` | int |
| `behavior` | string (a name you passed to sam_register_behavior) |

**Returns:** true on success (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_detach_behavior(uid)`

Remove whatever behaviour a script attached to this entity. Safe on a uid that has none.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** true (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false.

### `sam_get_entity_facing(uid)`

Read which way an entity is pointing. sam_get_facing takes a PLAYER index and reads where that player looks; this takes an entity uid, which is what a behaviour is handed. Feed it straight to sam_spawn_projectile to fire where the thing is aiming. Always in [0, 2π): an entity whose raw yaw was negative used to read as nil.

| argument | type |
|---|---|
| `uid` | int |

**Returns:** the facing in radians in [0, 2π) (number), or nil/undefined for an unknown uid

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler. Reads this machine's copy of the entity. On a client it can lag the host, and a flag the host clears without telling clients can stay set there. Read on the host when the answer matters.

### `sam_look_at(uid, target_uid)`

Turn one entity to face another. This is the one a turret wants — it does the trigonometry so you do not have to. Because your behaviour owns the entity, the engine has no opinion about which way it points; you do. Refuses to turn a PLAYER: their facing belongs to whoever is holding the mouse, and a script fighting their input every frame would feel broken.

| argument | type |
|---|---|
| `uid` | int (the entity to turn) |
| `target_uid` | int (what to face) |

**Returns:** true if it turned (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A spawned entity turns smoothly on every player's screen; a pinned prop turns for S.A.M players.

### `sam_register_behavior(name, fn)`

Give a name to a function that will BE an entity's brain. Barony runs every entity through a function pointer once per frame; this puts yours behind one. Your function is called with the entity's uid, once per frame, for every entity you spawned with that behaviour — and everything else in this reference is available inside it, so it can look around, move, shoot, damage, or open a window. Nothing about what it does comes from a list. Register at the top of your script rather than inside a handler, so the name exists before you spawn anything with it. Registering the same name twice replaces the function, and entities already in the world follow the new code. Behaviours are dropped when mods reload.

| argument | type |
|---|---|
| `name` | string ("behaviour", or "namespace:behaviour") |
| `fn` | function(uid) |

**Returns:** true if registered (boolean)

**Multiplayer:** `any`. The same answer on every machine; safe to call anywhere, including a client's on_packet handler.

### `sam_set_entity_facing(uid, radians)`

Point an entity at an angle. The primitive under sam_look_at, for when you are computing a direction yourself — a sweep, a spin, a lead on a moving target. The angle is normalised, so a behaviour that keeps adding to it will not drift out of range.

| argument | type |
|---|---|
| `uid` | int |
| `radians` | number (the same convention sam_get_facing returns and sam_spawn_projectile takes) |

**Returns:** true if it turned (boolean)

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns false. A spawned entity turns smoothly on every player's screen; a pinned prop turns for S.A.M players.

### `sam_spawn_entity(tile_x, tile_y, behaviour, [model])`

Put something in the world that runs your behaviour. This is the other half of sam_register_behavior: that one supplies the code, this gives it a body. The entity starts passable with no collision of its own, because your behaviour decides what it collides with. Leave model empty and it is invisible, which is almost never what you want.

| argument | type |
|---|---|
| `tile_x` | number (fractional tiles allowed) |
| `tile_y` | number |
| `behaviour` | string (a name you registered) |
| `model` *(optional)* | string (optional — a model from your mod's "models", or a vanilla model index) |

**Returns:** the new entity's uid (int), or nil/undefined

**Multiplayer:** `host`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns nil (undefined in JavaScript). Every player sees it move, turn, rise and fall smoothly, and its mod model is sent by NAME, so a player whose mods loaded in another order still sees the right model; sam_get_model(uid) answers the model it was spawned with on every machine. A player whose game does not run S.A.M sees a spawned entity with a special vanilla model (a torch, door, gate, chest lid, portal or gold bag) behave like that object, and it may stay where it first appeared: use a mod model or an ordinary vanilla model for anything that moves.


## Events

Handle these in `on_event(e)`. Every script receives every event; check `e.name`.
Most events fire only on the host, which is where your script's runtime code runs; the
**Multiplayer:** line under each says where and for whom.

### `<namespace>:<hook_name>`

Fires a script calls sam_fire_hook("namespace:name", event); delivered cross-runtime (Lua<->JS<->TS) to every loaded script's on_event.

| field | type |
|---|---|
| `name` | string |
| `<any user field>` | any |

not a fixed event — the name is author-defined and MUST contain a colon; only number/bool/string fields survive the crossing; host-only; recursion capped at depth 8

**Multiplayer:** Fires on the machine that calls sam_fire_hook, which is the host (a client's call is refused).

### `game.on_game_end`

Fires the game is won or the party wipes.

| field | type |
|---|---|
| `player` | int |
| `won` | int |
| `floor_reached` | int |
| `kills` | int |
| `time_played` | int |

won is 0/1

**Multiplayer:** Fires on the host, once per run (a party wipe and every win path alike).

### `game.on_game_start`

Fires a new game begins.

| field | type |
|---|---|
| `player` | int |
| `class_id` | int |
| `class_name` | string |
| `race` | int |
| `race_name` | string |

**Multiplayer:** Fires on the host, once for every player.

### `game.on_level_entered`

Fires a floor finishes loading.

| field | type |
|---|---|
| `player` | int |
| `floor` | int |
| `level_name` | string |

**Multiplayer:** Fires on the host, once for every connected player on every arrival (ladders, portals, teleports and sam_travel_to_level alike).

### `game.on_speed_changed`

Fires the simulation speed really changes: a sam_set_game_speed or /sam_gamespeed that set a different value, a temporary window ending, and the reset to 1x when a run ends or a new one starts.

| field | type |
|---|---|
| `old_percent` | int |
| `new_percent` | int |
| `temporary` | int |

A HUD that shows the speed listens here and reads sam_get_game_speed(); it does not need to poll.

Percent, because event fields are whole numbers: 100 is 1x, 250 is 2.5x. temporary is 0/1: 1 while a timed window is running, 0 when one has ended or a plain set was made. The value is already stored when this fires, so sam_get_game_speed() inside the handler is the exact new multiplier. Setting the value already set fires nothing.

**Multiplayer:** Fires on the host, in singleplayer.

### `mod.on_setting_changed`

Fires the value one of a mod's settings holds really changes: the player pressed Confirm in Settings (source "ui"), pressed Restore Defaults and then Confirm ("reset"), or a script called sam_set_setting ("script").

| field | type |
|---|---|
| `mod` | string |
| `id` | string |
| `type` | string ("slider", "toggle", "dropdown", "number", "text") |
| `old` | string |
| `new` | string |
| `source` | string ("ui", "script", "reset") |

A mod that keeps a derived value (a speed table, a HUD line) recomputes it here instead of polling sam_get_setting every tick.

old and new are TEXT, because an event field is a whole number or a string and a slider's 0.5 is neither: "2.5", "true", "run". Use tonumber(e.new) / e.new == "true", or simply call sam_get_setting(e.id), which already answers the new value (it is stored and written to disk before this fires). Once per setting that changed: nothing fires for a Confirm that changed nothing, for Discard, or for sam_set_setting with the value already in force. Every loaded mod hears every mod's changes, so check e.mod against your own namespace first.

**Multiplayer:** Fires on the machine whose setting changed.

### `on_action_pressed`

Fires a BOUND action goes down — e.g. the player presses whatever they have "Use" mapped to.

| field | type |
|---|---|
| `player` | int |
| `action` | string |
| `binding` | string |

no player. prefix. This is the one to use for button-mapped abilities: it reads Barony's named actions, so it follows the player's own keybinds and can never collide with them (it claims no key of its own), and it sees mouse buttons, which the raw-key hooks cannot. Observation only, so vanilla blocking/attacking/hotbar keep working. `binding` is the physical input ("Mouse3") for prompts, and is that player's OWN binding (a joiner's comes from their game). Single-fire per press; use sam_is_action_held for continuous checks. While the chat box or the console has the keyboard, every bound action reads as not held and no press fires, on every machine and in singleplayer -- an action that was already down when the box opened still fires its release. On a splitscreen machine that applies to every seat while one of them is typing, because the game's text focus belongs to the machine and not to a seat. A joiner's buttons reach the host from the moment their game says hello, and anything they were already holding at that moment is reported as a press, so press and release stay paired.

**Multiplayer:** Fires on the host (and in singleplayer), for every player: each local seat, and each joiner's buttons as their game reports them. A joiner whose game does not run S.A.M produces none.

### `on_action_released`

Fires a bound action goes back up.

| field | type |
|---|---|
| `player` | int |
| `action` | string |
| `binding` | string |

no player. prefix; the release twin of on_action_pressed

**Multiplayer:** Fires on the host (and in singleplayer), for every player, the same way as on_action_pressed.

### `on_before_damage`

> Cancellable: return `false` to stop it.

Fires before a player's HP is reduced (bracketed around Entity::modHP).

| field | type |
|---|---|
| `player` | int |
| `damage` | int |

no player. prefix; the ONLY cancellable event — call sam_modify_damage(player, new) to reduce/cancel the incoming hit

**Multiplayer:** Fires on the host, for every player.

### `on_before_effect_applied`

> Cancellable: return `false` to stop it.

Fires a status effect is about to be applied to any creature, before the engine's own immunity checks.

| field | type |
|---|---|
| `uid` | uid |
| `player` | int |
| `effect` | int |
| `effect_name` | string |
| `strength` | int |
| `duration_ticks` | int |
| `was_active` | int |
| `immune` | int |

no player. prefix, and player is -1 for a monster. The scarcest kind of hook: returning false REFUSES the effect outright. You can also rewrite duration_ticks and strength — so a mod can halve a poison rather than only blocking it — or clear the pre-filled `immune` field to let something through that sam_set_immunity would have stopped. Fires only for an effect being APPLIED, never a removal: a gate over removals would trap a creature in whatever it was already carrying. A rewritten duration_ticks of 0 refuses the effect, like strength 0 does; a negative one makes it permanent (-1). Host-side.

**Multiplayer:** Fires on the host.

### `on_before_monster_damage`

> Cancellable: return `false` to stop it.

Fires before a monster's HP is reduced.

| field | type |
|---|---|
| `monster_uid` | int |
| `monster_type` | int |
| `damage` | int |

Rewrite the number with sam_modify_monster_damage(n). Set 0 to negate the hit entirely. Not cancellable by returning false.

**Multiplayer:** Fires on the host.

### `on_damage_multiplier`

> Cancellable: return `false` to stop it.

Fires while a hit's damage multiplier is being decided, after every vanilla effect has had its say.

| field | type |
|---|---|
| `target_uid` | uid |
| `attacker_uid` | uid |
| `damage_type` | int |
| `multiplier_x1000` | int |
| `projectile_uid` | uid |
| `spell_id` | int |

no player. prefix. Fires for melee, arrows and every magic path -- one choke point covers all three. The multiplier crosses as THOUSANDTHS because the event bus carries whole numbers only: 1000 is normal damage, 1500 is +50%, 0 is immune. Contribute with sam_add_damage_multiplier (positives add, negatives multiply, the engine's own rule) or assign multiplier_x1000 directly for an absolute override; returning false makes the hit do nothing. attacker_uid is real here, unlike on_before_damage where it is always 0.

**Multiplayer:** Fires on the host.

### `on_key_pressed`

Fires a supported RAW key transitions to down (A-Z, 0-9, F1-F12).

| field | type |
|---|---|
| `player` | int |
| `key_name` | string |
| `held` | int |

no player. prefix; single-fire per press — use sam_is_key_held for continuous checks. Reads the PHYSICAL key and ignores the player's keybinds, so it can collide with whatever they've bound there, and it can't see mouse buttons — prefer on_action_pressed unless you specifically want a raw key. While the chat box or the console has the keyboard, every key reads as not held and no press fires, on every machine and in singleplayer -- a key that was already down when the box opened still fires its release. On a splitscreen machine that applies to every seat while one of them is typing. A joiner's keys reach the host from the moment their game says hello, and anything they were already holding at that moment is reported as a press, so press and release stay paired.

**Multiplayer:** Fires on the host, for every player: the host's keyboard (player is the seat holding it) and each joiner's keys (A-Z, 0-9, F1-F12) as their game reports them, a moment after the press. A joiner whose game does not run S.A.M produces no key events.

### `on_key_released`

Fires a supported key transitions to up (A-Z, 0-9, F1-F12).

| field | type |
|---|---|
| `player` | int |
| `key_name` | string |

no player. prefix; no `held` field (present only on press)

**Multiplayer:** Fires on the host, for every player, the same way as on_key_pressed.

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

no player. prefix; use killer_uid to identify the attacker (0 = environmental)

**Multiplayer:** Fires on the host.

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

no player. prefix; the reliable kill hook for ranged/magic (player.on_kill is melee-only). killer_uid > 0 means an actual killer (skip traps/starvation)

**Multiplayer:** Fires on the host.

### `on_packet`

Fires a mod-defined message sent with sam_send_packet arrives.

| field | type |
|---|---|
| `from` | int (the sender's player index; 0 is the host) |
| `tag` | string |
| `payload` | string |

no player. prefix. The only event a client's own scripts run besides player.on_before_equip and player.on_game_over, which makes it how a host tells a client's script something and the client answers (sam_send_packet from inside the handler). Delivery is reliable but NOT ordered.

**Multiplayer:** Fires on the machine the packet was sent to: a client's scripts for a packet from the host, the host's scripts for a packet from a client.

### `on_projectile_hit`

Fires a projectile from sam_spawn_projectile stops against a wall or an entity.

| field | type |
|---|---|
| `projectile` | int (the uid sam_spawn_projectile returned) |
| `target` | int (the uid it struck, or 0 for a wall) |
| `x` | int (tile) |
| `y` | int (tile) |
| `damage` | int (the damage the projectile was configured with) |

fires just BEFORE the projectile is removed, so the uid is still valid when your handler runs but will not be a moment later -- removing it yourself from here is safe. target is 0 for a wall, so check it before treating the hit as a creature. .damage is what the projectile CARRIES, not what landed: it is reported unchanged on a wall, and on anything without a health bar nothing was actually dealt. It fires on the host, like the spawn call.

**Multiplayer:** Fires on the host.

### `on_tick`

Fires every game tick (50/sec), for every script that defines on_tick(event).

| field | type |
|---|---|
| `tick_count` | int |
| `delta_ticks` | int |

no player. prefix; delivered to the separate on_tick(event) handler, not on_event; host-only and silent

**Multiplayer:** Fires on the host only (and in singleplayer). A client gets no tick: update HUD and panels when something changes and pass the player, because every call for a remote player is a network message.

### `player.on_attack_start`

Fires a player starts an attack swing (any weapon).

| field | type |
|---|---|
| `player` | int |
| `weapon_type` | int |
| `target_uid` | uid |

**Multiplayer:** Fires on the host, for every player.

### `player.on_became_ghost`

Fires a dead player becomes a ghost.

| field | type |
|---|---|
| `player` | int |

Pairs with sam_is_ghost. Fires once on the transition, not every frame while dead.

**Multiplayer:** Fires on the host, for every player.

### `player.on_before_equip`

> Cancellable: return `false` to stop it.

Fires before a player equips an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `category` | int |

Return false to refuse the equip. Use it for class or race restrictions the vanilla slot rules cannot express. It covers wielding from the item menu, the hotbar and Alt+right-click, as well as the equip-by-Use path. Starting gear, shops and grants are never asked. A refusal is SILENT: the framework used to print an English "You cannot equip that." that no mod could suppress and that was untranslated on a non-English install, and it no longer says anything at all. Say why yourself, with sam_hud_text: this handler runs on the equipping player's own machine, so a screen call for that player works in the host's game and in a joiner's alike, while sam_message and sam_play_sound are host functions and are refused there. Read carefully too: sam_get_player_data is a host function and returns nil in a joiner's game, so a restriction driven by data you stored with sam_set_player_data passes on every joiner and blocks only the host -- it fails OPEN, silently. Decide from something every machine can see: sam_get_class, sam_get_race, sam_get_stat and the item argument are all readable there. If you must use your own data, send it to the joiner's machine yourself (sam_send_packet in game.on_game_start, stored by their on_packet) and read that copy here.

**Multiplayer:** Fires on the equipping player's own machine, before anything is equipped or sent: the host for its own and splitscreen players, the joiner's own game for a joiner. In a joiner's game, use only functions that work there (reads of that player, and kinds any and local) and return false to refuse. A joiner whose game does not run S.A.M is never asked.

### `player.on_before_hit`

> Cancellable: return `false` to stop it.

Fires a player's melee swing has connected and the damage is decided, but not yet applied.

| field | type |
|---|---|
| `player` | int |
| `attacker_uid` | uid |
| `target_uid` | uid |
| `target_type` | int |
| `damage` | int |
| `backstab` | int |
| `flanking` | int |
| `weapon_type` | int |

The only place in the engine where the attacker, the crit state and a writable damage figure are all available at once. backstab and flanking are Barony's nearest thing to a critical hit and die as locals everywhere else, so this is the only way to see one. Rewrite with sam_modify_value or `event.damage = x`; returning false makes the blow land for nothing. Host-side, melee only -- arrows and spells do not come through here, use on_damage_multiplier for those.

**Multiplayer:** Fires on the host, for every player.

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

**Multiplayer:** Fires on the host, for every player (a client's pickup is decided there). A grant with sam_grant_item never fires it.

### `player.on_before_revive`

> Cancellable: return `false` to stop it.

Fires before a dead player is brought back.

| field | type |
|---|---|
| `player` | int |
| `floor` | int (the floor being loaded) |
| `keep_gear` | int (1 when the server has keep-inventory on) |

Return false to refuse the revive. This is the hook for a permadeath rule or a resurrection cost. floor is the floor being loaded and keep_gear is 1 when the server keeps inventory on death, so a cost can depend on both. The FIRST floor load of a run is never asked -- there is no level-change packet at that point to carry an answer to the clients on, so a run that begins with a dead player in the party (loading a co-op save) revives them as vanilla does. A player you refuse stays dead for the rest of the run: their body is taken down again on every later floor, player.on_death does NOT fire a second time, and on every machine they get a camera to watch from with no fresh game-over prompt.

**Multiplayer:** Fires on the host only (or in splitscreen singleplayer), once per dead player, before the level change. Its answer applies on every machine; a joiner's game no longer asks its own scripts.

### `player.on_bleed_tick`

Fires bleeding ticks damage on a player.

| field | type |
|---|---|
| `player` | int |
| `damage` | int |
| `stacks_remaining` | int |

**Multiplayer:** Fires on the host, for every player.

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

Fires on ANY block while actively defending with a shield and getting hit — not on right-click alone. full_block is 1 when all damage was negated (0 on a partial block); damage_taken is what leaked through. shield_type is the blocking shield's item id; gate with sam_item_id("namespace:item") to react only to your own shield.

**Multiplayer:** Fires on the host, for every player.

### `player.on_callout`

> Cancellable: return `false` to stop it.

Fires a player uses the callout / ping command.

| field | type |
|---|---|
| `player` | int |
| `cmd` | int |
| `type` | int |
| `target_uid` | uid |
| `x` | int |
| `y` | int |
| `help_flags` | int |

A free player-driven input channel: a mod can treat a callout as a custom command without binding a key. Return false to hide the ping.

**Multiplayer:** Fires on the host, for every player (a joiner's ping arrives there). A veto hides the ping from everyone; on a S.A.M joiner it also takes the marker off their own screen, but their ping sound has already played. A joiner without S.A.M keeps their own marker.

### `player.on_chest_opened`

Fires a player opens a chest.

| field | type |
|---|---|
| `player` | int |
| `chest_uid` | uid |
| `floor_x` | int |
| `floor_y` | int |

**Multiplayer:** Fires on the host, for every player.

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

read-only, and fires AFTER the HP is already gone — use it to react (a sound, a screen effect, a counter). To CHANGE how much lands, use on_before_damage, which fires ahead of the hit and can also negate it.

**Multiplayer:** Fires on the host, for every player.

### `player.on_death`

Fires a player dies.

| field | type |
|---|---|
| `player` | int |
| `killer_type` | int |
| `killer_uid` | uid |
| `killer_monster` | int |
| `obituary` | string |

**Multiplayer:** Fires on the host, for every player.

### `player.on_effect_applied`

Fires a status effect is newly applied to a player (a genuine off→on transition, not a refresh).

| field | type |
|---|---|
| `player` | int |
| `effect_name` | string |
| `duration_ticks` | int |
| `strength` | int |

Re-applying an already-active effect does NOT re-fire this. To watch a refresh or stack change, poll sam_get_effect_strength / sam_get_effect_duration in on_tick.

**Multiplayer:** Fires on the host, for every player.

### `player.on_effect_expired`

Fires a status effect runs out on a player.

| field | type |
|---|---|
| `player` | int |
| `effect` | int |

The counterpart to player.on_effect_applied. Use it to clean up anything the effect granted.

**Multiplayer:** Fires on the host, for every player.

### `player.on_effect_removed`

Fires a status effect ends (cleared or expired).

| field | type |
|---|---|
| `player` | int |
| `effect_name` | string |
| `effect` | int |

the numeric `effect` id is only present on expiry

**Multiplayer:** Fires on the host, for every player.

### `player.on_equip`

Fires a player equips an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `slot` | string |

slot is a lowercase name, e.g. "cloak"

**Multiplayer:** Fires on the host, for every player (a joiner's equip arrives there as the game's own echo).

### `player.on_floor_change`

Fires the party takes a ladder to a new floor.

| field | type |
|---|---|
| `player` | int |
| `initiator` | int (the player who climbed) |
| `old_floor` | int |
| `new_floor` | int |

Fires once for EVERY connected player, so check player == initiator to act once per descent. Ladders only: portals, teleporters and sam_travel_to_level do not fire it, while game.on_level_entered fires for every player on every arrival. new_floor is an estimate (a secret branch can change it). A good place to reset per-floor state.

**Multiplayer:** Fires on the host, once for every connected player when the party takes a ladder; initiator is the player who climbed.

### `player.on_game_over`

Fires a player's game-over window opens on their own machine.

| field | type |
|---|---|
| `player` | int |
| `tutorial` | int |
| `survived` | int |
| `placement` | int |
| `made_top` | int |

Your last chance to write per-run state with sam_save_data (a file on that machine) before the run is gone. Not cancellable.

**Multiplayer:** Fires on the dying player's own machine (a client's own game for a client) when that player's game-over window opens. In co-op that can be a death while others still live (survived tells you), not only the end of the run. The host never sees a remote player's.

### `player.on_gold_collected`

Fires a player picks up gold.

| field | type |
|---|---|
| `player` | int |
| `amount` | int |
| `total_gold` | int |

**Multiplayer:** Fires on the host, for every player.

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

melee only; lethal is 0/1

**Multiplayer:** Fires on the host, for every player (melee is resolved there).

### `player.on_hunger_change`

Fires hunger crosses a tier edge.

| field | type |
|---|---|
| `player` | int |
| `hunger` | int |
| `hunger_level` | int |
| `old_hunger_level` | int |

**Multiplayer:** Fires on the host, for every player (hunger is counted there).

### `player.on_item_bought`

Fires a player buys from a shop.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `gold_spent` | int |

**Multiplayer:** Fires on the host, for every player.

### `player.on_item_broken`

Fires a player's equipped item breaks.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `slot` | string |

**Multiplayer:** Fires on the host, for every player.

### `player.on_item_dropped`

Fires a player drops an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `floor_x` | int |
| `floor_y` | int |

**Multiplayer:** Fires on the host, for every player.

### `player.on_item_identified`

Fires a player identifies an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `item_name` | string |

**Multiplayer:** Fires on the host, for every player, once per identification: directly for the host's own players, and as reported by a joiner's own game (appraisal, a scroll or spell, a curse revealed on equip, a carried sam_identify_item). A joiner whose game does not run S.A.M reports nothing, except a curse the host sees on its copy of their worn item.

### `player.on_item_pickup`

Fires a player picks an item up off the ground.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `count` | int |
| `item_name` | string |

in-world pickups only — does NOT fire for starting inventory. gate with sam_item_id("ns:item") == event.item_type to react to your own item

**Multiplayer:** Fires on the host, for every player.

### `player.on_item_sold`

Fires a player sells to a shop.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `gold_received` | int |

**Multiplayer:** Fires on the host, for every player.

### `player.on_item_use`

> Cancellable: return `false` to stop it.

Fires a player uses a consumable (potion / scroll / food).

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `item_count` | int |
| `category` | string |

**Multiplayer:** Fires on the host, for every player. A joiner whose game runs S.A.M asks the host before using anything deliberately, so a veto uses up nothing and an allowed use happens one network round trip later. For a joiner without S.A.M (or in the first moment of a game) a veto stops the effect, but their game has already used the item up.

### `player.on_kill`

Fires a player's melee blow kills an entity.

| field | type |
|---|---|
| `player` | int |
| `target_uid` | uid |
| `target_type` | int |
| `was_lethal` | int |

melee only; for ranged/magic kills use on_monster_died and its killer_uid

**Multiplayer:** Fires on the host, for every player (melee is resolved there).

### `player.on_level_up`

Fires a player gains a level.

| field | type |
|---|---|
| `player` | int |
| `level` | int |
| `amount` | int |
| `stats` | int |

**Multiplayer:** Fires on the host, for every player.

### `player.on_miss`

Fires a player's melee swing connects with nothing.

| field | type |
|---|---|
| `player` | int |
| `target_uid` | uid |
| `weapon_type` | int |

**Multiplayer:** Fires on the host, for every player.

### `player.on_player_joined`

Fires a client joins the lobby.

| field | type |
|---|---|
| `player` | int |
| `player_index` | int |
| `player_name` | string |
| `class_id` | int |
| `race` | int |

carries the slot twice: player_index is the historical name and player is the same number, so that a screen call written with no player inside this handler is about the joiner. It is still a lobby notification -- anything sent to the joiner here is dropped.

**Multiplayer:** Fires on the host, in the lobby, when a client joins. It is a lobby notification only: per-player state written here (sam_set_player_data) is cleared when the game starts, and anything sent to the joiner here is dropped. Set per-player state up in game.on_game_start or game.on_level_entered.

### `player.on_player_left`

Fires a client leaves, times out or is kicked, in the lobby or in a game.

| field | type |
|---|---|
| `player` | int |
| `player_index` | int |
| `player_name` | string |

carries the slot twice: player_index is the historical name and player is the same number, there so that a screen call written with no player inside this handler is about the player who left rather than about the host. They have already gone, so such a call is refused -- name the players who are still here.

**Multiplayer:** Fires on the host, exactly once per departure: a leave, a keep-alive drop or a kick (including /kick), in the lobby or in a game. It pairs with player.on_player_joined.

### `player.on_player_revived`

Fires a dead player comes back: on a new floor, or when their ghost respawns.

| field | type |
|---|---|
| `player` | int |
| `revived_by` | int |
| `floor` | int |
| `revive_type` | string ("floor_load" or "ghost_respawn") |

**Multiplayer:** Fires on the host. floor_load fires once the new level has loaded, just before game.on_level_entered; ghost_respawn fires when a dead player's ghost respawns (the host's own or a joiner's).

### `player.on_poison_tick`

Fires poison ticks damage on a player.

| field | type |
|---|---|
| `player` | int |
| `damage` | int |
| `stacks_remaining` | int |

**Multiplayer:** Fires on the host, for every player.

### `player.on_proficiency_increased`

Fires a skill rank goes up.

| field | type |
|---|---|
| `player` | int |
| `proficiency` | int |
| `proficiency_name` | string |
| `old_rank` | int |
| `new_rank` | int |

**Multiplayer:** Fires on the host, for every player.

### `player.on_shop_entered`

Fires a player opens trade with a shopkeeper.

| field | type |
|---|---|
| `player` | int |
| `shopkeeper_uid` | uid |

**Multiplayer:** Fires on the host, for every player.

### `player.on_spell_cast`

> Cancellable: return `false` to stop it.

Fires a player casts a spell.

| field | type |
|---|---|
| `player` | int |
| `spell_id` | int |
| `spell_name` | string |
| `target_uid` | uid |

**Multiplayer:** Fires on the host, for every player (a joiner's cast arrives before mana is spent).

### `player.on_spell_failed`

Fires a cast fizzles or is blocked.

| field | type |
|---|---|
| `player` | int |
| `spell_id` | int |
| `spell_name` | string |
| `reason` | string |

**Multiplayer:** Fires on the host, for every player: a fizzle from the host's own cast, and not_enough_mana as reported by the caster's own game. A joiner whose game does not run S.A.M reports no mana failures.

### `player.on_spell_learned`

Fires a player learns a spell.

| field | type |
|---|---|
| `player` | int |
| `spell_id` | int |
| `spell_name` | string |

**Multiplayer:** Fires on the host, for every player, once, when the spell is actually learned: a joiner's own game reports it (including a carried sam_grant_spell). A joiner whose game does not run S.A.M reports nothing.

### `player.on_status_effect_tick`

Fires an active status effect ticks.

| field | type |
|---|---|
| `player` | int |
| `effect` | int |
| `effect_name` | string |
| `ticks_remaining` | int |

throttled to ~1/sec

**Multiplayer:** Fires on the host, for every player (effect timers are counted there).

### `player.on_unequip`

Fires a player unequips an item.

| field | type |
|---|---|
| `player` | int |
| `item_type` | int |
| `item_count` | int |
| `slot` | string |

**Multiplayer:** Fires on the host, for every player (a joiner's unequip arrives there as the game's own echo).

### `player.on_xp_gained`

Fires a player gains XP from a kill.

| field | type |
|---|---|
| `player` | int |
| `amount` | int |
| `source_type` | string |
| `monster_type` | int |

The engine's one value-rewrite hook: set event.amount (or use sam_modify_value) and the engine adopts it. Set it to 0 to detach levelling from kills entirely.

**Multiplayer:** Fires on the host, for every player.

### `ui.on_click`

Fires the player clicks a button you placed with sam_ui_button.

| field | type |
|---|---|
| `player` | int (who clicked) |
| `mod` | string (owning namespace) |
| `panel` | string |
| `widget` | string (the button id) |
| `value` | string (empty for a button) |

only reachable while the panel is open with modal = true: a non-modal panel gives the player no cursor to click with. Answer the clicking player by passing .player to the panel calls (or leave the player out: inside this event it defaults to the clicker).

**Multiplayer:** Fires on the host (in singleplayer, locally), with player = who clicked. A client's click is sent to the host and does not fire in the client's own scripts.

### `ui.on_select`

Fires the player clicks a row in a list you built with sam_ui_list / sam_ui_list_add.

| field | type |
|---|---|
| `player` | int (who clicked) |
| `mod` | string |
| `panel` | string |
| `widget` | string (the LIST's id, not the row's) |
| `value` | string (the row_id you passed to sam_ui_list_add) |

the row is in .value and the list is in .widget — easy to swap by accident. Give each row a row_id you can act on directly, such as an item id.

**Multiplayer:** Fires on the host (in singleplayer, locally), with player = who clicked the row. A client's click is sent to the host and does not fire in the client's own scripts.

### `ui.on_submit`

Fires the player commits the contents of a text box placed with sam_ui_input.

| field | type |
|---|---|
| `player` | int (who typed) |
| `mod` | string |
| `panel` | string |
| `widget` | string (the input id) |
| `value` | string (what they typed) |

you can also read the box at any time with sam_ui_input_text; this event just tells you when they finished.

**Multiplayer:** Fires on the host (in singleplayer, locally), with player = who typed. A client's submit is sent to the host and does not fire in the client's own scripts.

### `world.on_before_chest_fill`

> Cancellable: return `false` to stop it.

Fires a chest (or a mimic, which is a chest with legs) is about to be filled, after its type and quality floor are decided and before any item is made.

| field | type |
|---|---|
| `chest_uid` | int |
| `is_mimic` | int (0/1) |
| `x` | int (tile) |
| `y` | int (tile) |
| `floor` | int |
| `chest_type` | int (0 random, 1 garbage, 2 food, 3 treasure, 4 equipment, 5 tools, 6 magic, 7 potions, 8 empty) |
| `min_quality` | int (the level floor deep chests use for weapons and armour: 0, 5 from floor 18, 10 from floor 32) |

Rewrite chest_type to change what the engine fills it with, min_quality to raise or drop the deep-floor threshold, or return false to leave it EMPTY and fill it yourself from world.on_chest_filled with sam_add_item_to_container (the vampire quest book is still placed).

The map's own chests are filled while the level is built, on the loader thread, so this fires only for a chest summoned during play (a spell, a script). For the chests a floor came with, use game.on_level_entered + sam_find_entities(x, y, r, "chest") + the container functions; the loot TABLES do apply to them.

**Multiplayer:** Fires on the host, on the main thread only, for a chest filled during play.

### `world.on_before_chest_open`

> Cancellable: return `false` to stop it.

Fires before a chest opens.

| field | type |
|---|---|
| `player` | int |
| `chest_uid` | int |

Return false to keep the chest shut. Combine with sam_spawn_monsters for a mimic or an ambush.

**Multiplayer:** Fires on the host, for every player.

### `world.on_before_loot_roll`

Fires before the engine rolls a random item from the item sheet during play: a shopkeeper stocking, a creature's starting gear, a chest summoned during play, the lockpick capstone reward, the potion an alchemy skill-up teaches, /sam_loot, /itemlevelcurve and sam_roll_loot. It does NOT fire for the rolls made while a floor is generated (floor items, the map's own chests, the mimics made from them): those run on the level loader's thread, where scripts cannot be entered, and are governed by the loot tables.

| field | type |
|---|---|
| `category` | string (WEAPON .. BOOK; "ANY" for the any-category gold-value draw) |
| `min_level` | int (-1 for a gold-value draw) |
| `max_level` | int (-1 for a gold-value draw) |
| `min_value` | int (-1 for a level draw) |
| `max_value` | int (-1 for a level draw) |
| `item_type` | int (-1; set it to force a type) |
| `context` | string (floor chest shop monster recipe console other) |
| `floor` | int |
| `shop_type` | int (-1 unless context is shop) |
| `shop_uid` | int (-1 unless context is shop) |
| `chest_uid` | int (-1 unless context is chest) |
| `chest_type` | int (-1 unless context is chest) |
| `monster_uid` | int (-1 unless context is monster) |
| `monster_type` | int (-1 unless context is monster) |

Rewrite category, min_level and max_level to change the pool the roll draws from (the Hamlet rock fix is `if e.context == "shop" then e.min_level = 0 end`), or set item_type to a valid item to skip the roll entirely. A forced type costs the dungeon's random generator nothing; a handler that changes nothing leaves it exactly where vanilla leaves it. Not cancellable: every caller makes an item of whatever comes back, so "no item" is a per-site decision (world.on_before_chest_fill, world.on_before_shop_stock, world.on_fixture_loot).

context == "recipe" is an alchemy recipe being learned, not a drop: a randomizer that forces artifacts here teaches the player to brew one. A chest's uid is -1 while the chest is a mimic being built. The lockpick reward arrives with min_value/max_value (80..600 gold) and category "ANY".

**Multiplayer:** Fires on the host, on the main thread only (never on the level loader's thread, never on a client), for every roll during play.

### `world.on_before_shop_stock`

> Cancellable: return `false` to stop it.

Fires a shopkeeper is about to stock its store, after the map's and the editor's flags decided the store type, the item count, the shop level and the blessing tier, and before any item is made.

| field | type |
|---|---|
| `shopkeeper_uid` | int |
| `floor` | int |
| `store_type` | int (0 arms, 1 hats, 2 jewelry, 3 books, 4 apothecary, 5 staves, 6 food, 7 hardware, 8 hunting, 9 general, 10 mysterious; -1 = generate nothing) |
| `num_items` | int |
| `shop_level` | int (the level window's top; the floor number, at least 15 in a Shopping Spree run) |
| `blessed` | int (1..3, how blessed the good gear may be) |

Rewrite the four fields, or return false to have the shopkeeper generate nothing (it keeps its store label) and stock it yourself from world.on_shop_stocked with sam_set_shop_stock. shop_level is the top of the level window the store rolls with; store 0 and 8 also apply MINIMUMS from floor 18 (weapons from level 10), which is what an all-level-0 sheet trips over -- see world.on_before_loot_roll for that fix.

Fires on the shopkeeper's first tick, not at map generation, so it fires for every shopkeeper (placed by the map or spawned). The data-driven consumables (the bottom-right row) are added after this whatever you return.

**Multiplayer:** Fires on the host, for every shopkeeper on its first tick.

### `world.on_before_spellbook_reroll`

Fires a roll produced a spellbook and the engine is about to replace it with a spell drawn from the spell table (its drop_table floor, its school rotation, its difficulty ladder), before that candidate list is built.

| field | type |
|---|---|
| `item_type` | int (the spellbook the item roll picked) |
| `item_level` | int (the floor the re-draw is for) |
| `keep` | int (0; set 1 to keep the picked book and skip the re-draw) |
| `spell_id` | int (-1; set a spell with a book to force it, skipping the school rotation and the ladder) |
| `allow_hidden` | int (0; set 1 to let sheet-hidden spells -- class, race, monster books -- into the normal draw) |
| `context` | string |
| `floor` | int |

keep = 1 is what makes a level-patched class spellbook survive: without it the re-draw throws the book away whatever its level. sam_set_spell_droppable is the table form of allow_hidden for one spell at a time and needs no handler.

A forced spell_id with no spellbook or tome is ignored with a log line and the normal re-draw goes ahead. Keeping the book costs the random generator nothing; forcing or allowing changes what the draw consumes.

**Multiplayer:** Fires on the host, on the main thread only, for every spellbook rolled during play (shops, monster pockets, a summoned chest, sam_roll_loot).

### `world.on_boulder_triggered`

Fires a boulder trap launches.

| field | type |
|---|---|
| `floor_x` | int |
| `floor_y` | int |

**Multiplayer:** Fires on the host.

### `world.on_chest_filled`

Fires a chest's (or mimic's) inventory has been generated and laid out, or left empty because world.on_before_chest_fill said so.

| field | type |
|---|---|
| `chest_uid` | int |
| `is_mimic` | int (0/1) |
| `x` | int (tile) |
| `y` | int (tile) |
| `floor` | int |
| `chest_type` | int |
| `item_count` | int |

Add or remove with sam_add_item_to_container / sam_remove_item_from_container; read with sam_get_container_items.

Same limit as world.on_before_chest_fill: not for the chests a floor is generated with.

**Multiplayer:** Fires on the host, on the main thread only, for a chest filled during play.

### `world.on_chest_found`

Fires a player first walks up close to a chest (proximity — fires once per chest, NOT on opening it).

| field | type |
|---|---|
| `player` | int |
| `chest_uid` | uid |
| `floor_x` | int |
| `floor_y` | int |

This is "spotted a chest nearby", not "opened a chest". For the moment a player actually opens one, use player.on_chest_opened.

**Multiplayer:** Fires on the host, for every player.

### `world.on_door_opened`

Fires a player opens a wooden door.

| field | type |
|---|---|
| `player` | int |
| `door` | uid |
| `status` | int |
| `type` | string |

opens only (not closes); wooden doors only, not iron gates. status is the swing direction (1 or 2)

**Multiplayer:** Fires on the host, for every player.

### `world.on_fixture_loot`

> Cancellable: return `false` to stop it.

Fires one of the small fixtures that never consult the item sheet is about to hand out an item: a fountain's potion splash (goatmen), a sink's ring, the luckstone a boulder leaves when it blocks the only way on, and the lockpick capstone's chest reward.

| field | type |
|---|---|
| `source` | string (fountain, sink, boulder, lockpick) |
| `player` | int (-1 for a boulder: nobody in particular caused it) |
| `x` | int (tile) |
| `y` | int (tile) |
| `floor` | int |
| `item_type` | int |
| `status` | int |
| `beatitude` | int |
| `count` | int |

Rewrite the item, or return false for no item at all (the fixture still counts as used; a boulder is still removed and its message still shown; the lockpick still trains the skill).

A fountain potion whose type you change loses the potion's standard appearance (it belonged to the old type) and takes 0. The lockpick reward was ALSO a world.on_before_loot_roll (category "ANY", min_value/max_value) a moment earlier, in the chest context.

**Multiplayer:** Fires on the host, for the player who used the fixture (player is -1 for a boulder).

### `world.on_fountain_used`

Fires a player drinks from / uses a fountain.

| field | type |
|---|---|
| `player` | int |
| `fountain` | uid |
| `effect` | int |

fires once, just before the fountain dries up. effect is the fountain's rolled effect type

**Multiplayer:** Fires on the host, for every player.

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

player is -1 when a monster threw it

**Multiplayer:** Fires on the host (a thrown gadget lands there), for every player.

### `world.on_loot_rolled`

Fires after a rolled item's final type is known (after the spellbook re-draw and the thrown-weapon status rule) and the item exists: every roll world.on_before_loot_roll covers, plus a monster's rolled worn slots and pockets and a troll's or ghoul's rolled loot, which run no post-processing in vanilla and get this event directly.

| field | type |
|---|---|
| `item_type` | int |
| `status` | int (0 BROKEN .. 4 EXCELLENT) |
| `beatitude` | int |
| `count` | int |
| `appearance` | int |
| `identified` | int (0/1) |
| `is_fallback` | int (1 when the pool was empty and the roll returned GEM_ROCK or the sam_set_loot_fallback item) |
| `was_spellbook_rerolled` | int (1 when the engine re-drew a spellbook from the spell table) |
| `context` | string |
| `floor` | int |
| `shop_type` | int |
| `shop_uid` | int |
| `chest_uid` | int |
| `chest_type` | int |
| `monster_uid` | int |
| `monster_type` | int |

Rewrite any of the six writable fields and the item takes them (a floor entity's skill[10..15], or the Item itself). item_type must be a valid item or the rolled one is kept and the log says so. is_fallback is the honest way to catch the rock: a garbage chest's rock is a real rock.

For the floor items placed while a level is generated this event does not fire (loader thread); read them from game.on_level_entered with sam_find_entities(x, y, r, "item") instead. At a monster's worn slots and pockets was_spellbook_rerolled is always 0, because vanilla never re-draws those.

**Multiplayer:** Fires on the host, on the main thread only, for every rolled item during play.

### `world.on_monster_inventory`

Fires a creature's species init has finished making its starting gear (worn slots and pockets alike), on its first tick.

| field | type |
|---|---|
| `monster_uid` | int |
| `monster_type` | int |
| `monster_name` | string (the creature's own name, else the species name) |
| `floor` | int |
| `item_count` | int (pockets) |
| `worn_count` | int (equipped slots) |
| `is_shopkeeper` | int (0/1) |

This is the one place to reach the ~700 hardcoded species gear tables without an engine edit per species: read with sam_get_container_items, rewrite pockets with sam_add_item_to_container / sam_remove_item_from_container and worn slots with sam_monster_equip. Pockets are what the creature drops on death.

A shopkeeper's store has already been stocked by now (world.on_before_shop_stock and world.on_shop_stocked fired inside its init). Player allies summoned by a script get it too.

**Multiplayer:** Fires on the host, for every creature on its first tick.

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

**Multiplayer:** Fires on the host.

### `world.on_orb_placed`

Fires a player places an orb on a pedestal.

| field | type |
|---|---|
| `player` | int |
| `pedestal` | uid |
| `orb_type` | int |
| `correct` | int |

correct is 1 when the orb matched the pedestal (ritual advanced), 0 for the wrong orb

**Multiplayer:** Fires on the host, for every player.

### `world.on_projectile_hit`

Fires a fired projectile strikes an entity.

| field | type |
|---|---|
| `player` | int |
| `shooter_uid` | uid |
| `target_uid` | uid |
| `target_type` | int |

player is the shooter's index, or -1 when a monster or trap fired it. target_type is the struck monster's type, or -1 for a non-creature (chest, etc.)

**Multiplayer:** Fires on the host.

### `world.on_shop_stocked`

Fires a shopkeeper's stock has been generated, laid out by price and had its consumables added (potions are normalised right after).

| field | type |
|---|---|
| `shopkeeper_uid` | int |
| `store_type` | int |
| `floor` | int |
| `item_count` | int (the consumables included) |

Rewrite the stock with sam_set_shop_stock, or adjust it with sam_add_item_to_container / sam_remove_item_from_container; both lay the slots out again.

**Multiplayer:** Fires on the host, for every shopkeeper on its first tick.

### `world.on_sink_used`

Fires a player uses a sink.

| field | type |
|---|---|
| `player` | int |
| `sink` | uid |
| `outcome_code` | int |
| `outcome` | string |

outcome is one of "ring" / "slime" / "nutrition" / "damage"

**Multiplayer:** Fires on the host, for every player.

### `world.on_switch_toggled`

Fires a player flips a lever or switch.

| field | type |
|---|---|
| `player` | int |
| `switch` | uid |
| `state` | int |

state is the NEW value after the flip (1 = on / powered, 0 = off)

**Multiplayer:** Fires on the host, for every player.

### `world.on_teleport`

Fires a player uses a teleporter (pad or tunnel-spell).

| field | type |
|---|---|
| `player` | int |
| `teleporter` | uid |
| `type` | int |
| `dest_x` | int |
| `dest_y` | int |

same-floor teleport only; dest_x/dest_y are the destination tile. For descending floors use player.on_floor_change

**Multiplayer:** Fires on the host, for every player.

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

carries `damage` for physical traps OR `spell` (spell id) for magic traps, not both. player is -1 when a monster or the dungeon set it off, which for a magic or spike trap is ALWAYS: only an arrow trap names a player

**Multiplayer:** Fires on the host.

