# Multiplayer

How a S.A.M mod behaves in a co-op game, and how to test one on your own computer.

Every one of the framework's script functions and events has a defined multiplayer behaviour,
and the game enforces it. You do not write a "host version" and a "client version" of your mod.
You write one script, and S.A.M carries each call to the machine that has to run it.

> Looking for one function? [function-reference.md](function-reference.md) ends every function
> and every event with a **Multiplayer:** line, generated from the same table the game enforces.

## Contents

- [The model](#the-model)
- [The seven kinds](#the-seven-kinds)
- [Naming the player, and -1](#naming-the-player-and--1)
- [What a client-side script can and cannot do](#what-a-client-side-script-can-and-cannot-do)
- [Players without S.A.M, and players without your mod](#players-without-sam-and-players-without-your-mod)
- [Players who join late](#players-who-join-late)
- [Backpacks, items and spells](#backpacks-items-and-spells)
- [Sounds, music and the camera](#sounds-music-and-the-camera)
- [HUD, panels and pictures](#hud-panels-and-pictures)
- [Entities, terrain and models](#entities-terrain-and-models)
- [Stats, effects, classes and stored data](#stats-effects-classes-and-stored-data)
- [Events and input](#events-and-input)
- [Randomness and timers](#randomness-and-timers)
- [Lua and JavaScript now agree](#lua-and-javascript-now-agree)
- [Testing multiplayer on one computer](#testing-multiplayer-on-one-computer)

---

## The model

**Your script's runtime code runs on the host.** `on_tick`, every timer and nearly every event
fire on the host machine only. That is true in singleplayer too, where your machine is the host.

**Scripts still load on every machine.** Each player's game reads your mod and runs the
top-level code of your script file. After that, a client runs your code in only three places:
the load itself, `on_packet`, and the handful of events that have to be decided where the thing
happened (see [below](#what-a-client-side-script-can-and-cannot-do)).

**You name players by index.** Player 0 is the host, 1 to 3 are the other players in a four
player game. Every function that is about a player takes that number, and it means the same
thing on every machine.

**S.A.M carries each call to the right machine.** Some things live on the host (monsters, the
dungeon, the rules). Some live on a player's own machine (their backpack, their spell list,
their screen, their keyboard). When your host-side code calls one of the second kind for a
player on another computer, the framework sends the call there, runs the same function on that
machine, and your call returns as soon as it is sent.

So a mod written as if for one machine works in co-op, as long as you name the player you mean.

```lua
function on_event(e)
  if e.name == "player.on_kill" then
    sam_apply_effect(e.player, "FAST", 200)        -- rules: happens on the host
    sam_hud_text("combo", 40, 40, "Nice hit!", 0xFFFFFFFF, e.player)  -- their screen
    sam_grant_spell(e.player, "spell_fireball")    -- their spell list, on their machine
  end
end
```

## The seven kinds

Every function is one of seven kinds. The kind is written in
`framework/sam_mp_contracts.inc`, printed in the function reference, and applied by the game
before the function body runs, so the documented behaviour is always the real one.

| kind | what it means for your script | example |
|---|---|---|
| `host` | The host decides, and the result reaches every player. Call it from your events and timers, which already run there. A client's own call is refused with a one time warning. | `sam_apply_effect(1, "POISONED", 200)` poisons player 1 wherever they are. |
| `owner` | The thing lives on that player's own machine (their backpack, their spells). Call it on the host; S.A.M carries it to that machine and runs it there. It returns true once it is **sent**. | `sam_grant_spell(1, "spell_fireball")` teaches player 1 on player 1's computer. |
| `screen` | Shows something to one player. Carried to that player's machine. | `sam_screen_flash(1, 255, 0, 0, 0.6)` flashes red on player 1's screen only. |
| `read` | Reads a player. The host can read everyone. A client can read only its own player, and asking about anyone else is refused. | `sam_get_stat(2, "HP")` works on the host for anybody. |
| `all` | Changes a table every machine keeps its own copy of. A host call runs on the host and on every S.A.M client, and is replayed to a player who joins later. | `sam_patch_class("barbarian", { STR = 20 })` on every machine at once. |
| `local` | Answers for the machine running it: its clock, its files, its music, its keyboard. Given a player on another machine it refuses. | `sam_local_player()` is 0 on the host and 1 on the first joiner. |
| `any` | The same answer on every machine, because the data is replicated or the function is pure maths. Safe to call anywhere, including a client's `on_packet` handler. | `sam_get_distance(a, b)` |

An `owner` or `screen` call for a player on another machine answers **true once it has been handed
to the channel** — not once it has been done there. It answers false only when the framework could
not take it at all: a body over 16 KB (a very long label is the usual cause), or a queue with no
room. If you build a panel with a long description for another player, check the return value.

A refused call does not crash your script. It logs one warning naming the function, then returns
the refusal value listed for it in the function reference (false, nil, 0, an empty table, or
nothing at all). The reference spells out which one, because the difference matters: a refusal
must never look like an answer.

Where 0 or false would be a real answer — a stat, a kill count, an effect strength, a light
level, a move speed, and the yes-or-no questions about a player or a monster — the refusal is
**nothing at all**: no value in Lua, `undefined` in JavaScript. It is the only value that
compares false in both runtimes (`null <= 0` and `0 <= 0` are both true in JavaScript), so
`if sam_get_stat(e.who, "HP") <= 0 then` in a client-side handler now raises with a line number
instead of quietly firing for a living player. Twenty-four readers answer this way:
`sam_get_stat`, `sam_get_skill`, `sam_get_kills`, `sam_get_effect_strength`,
`sam_get_monster_effect_strength`, `sam_get_monster_stat`, `sam_get_monster_target_uid`,
`sam_get_light_at`, `sam_add_move_speed`, `sam_is_ghost`, `sam_is_spirit_ghost`,
`sam_is_defending`, `sam_is_parrying`, `sam_has_effect`, `sam_is_immune`,
`sam_monster_has_effect`, `sam_monster_has_trait`, `sam_monster_can_wield`, `sam_is_enemy`,
`sam_is_friend`, `sam_is_action_held`, `sam_is_key_held`, `sam_ui_is_open` and
`sam_random_chance`.

## Naming the player, and -1

Most `screen` functions take the player as an **optional last argument**, after everything else
— the HUD and every panel widget.

```lua
sam_hud_text("id", 40, 40, "hello", 0xFFFFFFFF)      -- see below
sam_hud_text("id", 40, 40, "hello", 0xFFFFFFFF, 2)   -- player 2's screen
sam_hud_text("id", 40, 40, "hello", nil, -1)         -- everyone's screen, default colour
```

To name a player without also picking a colour, pass `nil` (`null` in JavaScript) for the
colour, as the third line does. Leaving a hole out is not an option: the player argument is
found by its **position**, so a number in the colour's place is read as the colour.

**The other family takes the player FIRST.** `sam_screen_flash`, `sam_impact_frame`,
`sam_show_image`, `sam_show_image_at`, `sam_show_own_body` and the six camera functions
(`sam_set_camera_position`, `_target`, `_angle`, `_offset`, `_collision`, `sam_reset_camera`)
name the player as their **first, required** argument; `sam_hide_image` names it first too but
may be left out. The player is not an afterthought for any of them:

```lua
sam_screen_flash(e.player, 255, 0, 0, 0.5, 300)      -- player first: red flash on their screen
```

Writing one of those in the optional-last form fails silently — `sam_screen_flash(255, 0, 0, 30,
e.player)` reads 255 as the player, finds no such player, and does nothing at all. The
[function reference](function-reference.md) shows the real argument order for every one of them,
and its **Multiplayer:** line names the argument that picks the machine.

Left out (where it may be left out), the player is **the player the current event is about**.
Outside an event it is this machine's own player. So inside `player.on_damage_taken` you can
write the plain form and it shows on the right person's screen without you doing anything.

`-1` means every player. In a singleplayer game that is simply your own player, so a call written
with `-1` does the same thing in both and you can test it without a second machine.

- On a machine with more than one local seat (splitscreen), `-1` reaches **every** seat for the
  per-seat calls — the camera, the screen flash, the impact frame and the full-screen picture
  (`sam_show_image`, `sam_show_image_at`, `sam_hide_image`), each of which is drawn into one
  seat's own viewport, so the call is made once for each of them. The HUD and panels are drawn
  once for the whole window, so every local seat sees them whatever you pass.
- **On a client**, `-1` means that client's own screen: for the HUD and panels, and for the
  player-first calls too. `-1` and `sam_local_player()` do the same thing there. Expanding
  "everyone" to the other machines is the host's job, and a client only ever draws on itself.

Two things to watch:

- **A timer callback is not an event.** A screen call inside a timer with no player named shows
  on the host's screen. Keep the player in a local (`local p = e.player`) when you set the timer
  and pass it.
- `sam_hide_image` with no player is carried to the machine of the player the current event is
  about (outside an event it stays here) and clears that machine's picture — every local seat's,
  on a splitscreen machine. `-1` clears everyone's.

`sam_ui_is_open(panel [, player])`, `sam_ui_input_text(panel, id [, player])` and
`sam_is_key_held(key [, player])` take the same optional player, and follow the same default.

**Update HUD and panels when something changes, not every tick.** `on_tick` and timers run only
on the host, and every screen call for a remote player is a network message.

## What a client-side script can and cannot do

A client's copy of your script runs in exactly three places.

**1. Load time.** Your top-level code runs on every machine when mods load. Use it to declare
content, register behaviours, patch classes and items, and set up the XP curve. Two traps:

- `sam_is_host()` returns true and `sam_local_player()` returns 0 on **every** machine while
  mods load, because scripts load before multiplayer starts. Call them inside events, and never
  cache the answer at load time.
- Never set a timer from top-level code. Every machine drops all timers when a run starts. Set
  them from `game.on_game_start` or later.

**2. `on_packet`.** A client receives what the host sends with `sam_send_packet`, and can reply
with `sam_send_packet(0, tag, payload)`. This is how a client-side UI, or a client-side check,
gets its data. What a client may call there is more than `any`:

- functions of kind `any` (`sam_send_packet`, `sam_player_count`, `sam_get_race`, the maths);
- functions of kind `local` — `sam_local_player`, `sam_is_host`, `sam_log`, `sam_save_data`,
  `sam_load_data`, `sam_get_real_time` — which is how the client knows who it is, writes its
  own log and keeps its own file;
- `read` functions about **its own player** (`sam_get_stat`, `sam_get_inventory`,
  `sam_player_knows_spell`, `sam_ui_is_open`) — another player's is refused;
- `screen` functions **for its own screen**, which is what makes a client-side UI possible at
  all: `sam_ui_open`, `sam_ui_label`, `sam_hud_text` and the rest draw on the machine that
  calls them. Name that client's own player, or leave the player out (the default is its own).

What is refused there is `host`, `owner` and `all` — the things the host decides.

**3. A few events that must be decided where they happen.** The important one is
`player.on_before_equip`: it fires on the **equipping player's own machine**, so for a joiner
your handler runs inside the joiner's game. Return false to refuse the equip. Use what works
there: the same list as above — `any`, `local`, reads about that same player, and its own
screen.

Everything else — `host`, `owner` and `all`, and a `read`, `local` or `screen` call about
somebody else's player — is refused on a client, with one warning per function. That is
deliberate: a refusal you can see beats a number that looks right and is not.

If a client needs a value the host has, send it with `sam_send_packet`. Delivery is reliable but
**not ordered**, so number your packets if order matters, and split bulk data into several
packets. `sam_save_data` writes a file on one machine and reaches nobody.

## Players without S.A.M, and players without your mod

A stock, unmodified Barony client can join a S.A.M host and play. It ignores every S.A.M
message, so anything that needs your mod on their machine cannot reach them. S.A.M never
pretends otherwise: the call is refused and the reason is written once to the host's log.

What still works for a player without S.A.M:

- Everything the host decides and the game's own packets carry: damage, effects, stats, monsters,
  gold, items granted with `sam_grant_item`, dug walls, spawned vanilla entities.
- Sounds from a sound map arrive as the vanilla sound your mod replaced, rather than silence.

What cannot reach them: their screen (HUD, panels, pictures, screen flash), their spell list
(`sam_grant_spell`, `sam_remove_spell`), their backpack as a readable copy, their keys and
buttons, custom models, `sam_set_entity_size`, `sam_revive_player`, a follower rename, and the
move speed multiplier (their own machine computes their movement).

A player who runs S.A.M but not **your** mod is in between: the wire carries mod content by
name, so a sound or model they do not have simply does not play or draw for them.

**A word about their log.** A stock game says hello to nobody, and the framework takes about
twenty seconds to be sure of that. Until it is, a few of the older S.A.M packets (a custom body,
a scripted tile edit, a full-screen picture) may still reach that machine, and it writes one
"mystery packet" line for each and carries on. After those twenty seconds nothing S.A.M-only is
sent to them at all and their log stays clean. The trade is deliberate: a player who is merely
slow to load, and a player on an older S.A.M build, keep working in the meantime.

**A word about trust.** The player slot written in a S.A.M frame is the sender's own claim about
who it is, exactly as it is in every one of Barony's own packets. On a direct connection the host
can check the address the datagram came from; over Steam or EOS there is no source address to
check, because the peer-to-peer reader hands the game a buffer without one. So a modified client
can put its own numbers where its own player's numbers go — an arbitrary status and blessing on
the host's copy of gear it is *wearing*, for instance (the stack count is capped at the engine's
own stack limit; those two are not), which is the same trust vanilla's own equip packets place in
a client. What is guaranteed either way: a forged frame cannot make the host treat a machine
without S.A.M as one that has it, cannot open a channel with anything but a hello, and cannot
wedge another player's channel for more than a second at a time. Write your mod for co-op with
friends, not as an anti-cheat.

## Players who join late

A client that joins a game in progress says hello to the host, and the host sends it the state
it missed: the class and item patches, the species resist table, stat modifier totals, the XP
curve, move speed multipliers, its own full experience number, every terrain edit made on this
floor, the pinned state of props, the forced music track (including "off"), and the current
custom model of anything the host changed.

The one gap: a class patch left over from an earlier singleplayer game can still shape a client's
**first** character of a co-op run, because the character is built before the host's table
arrives. Patch classes when your mod loads, or reload mods before hosting.

**While a player's game has not said hello yet**, anything you send them is held rather than
dropped, and delivered in order the moment it does. A client keeps saying hello until the host
answers and gives up after twenty seconds; once that window closes nothing new is added to the
pile, your call is refused and your script is told. So check the return value of anything you
send to one player in the first seconds of a run, and prefer doing it a beat later
(`player.on_player_joined` plus a moment, or a later tick).

Two consequences worth knowing:

- The host answers **every** hello by re-sending the whole catch-up, not only the first. A new
  run and a repaired channel both start with a fresh hello, so anything you do when a player
  joins must be safe to do twice — send the whole picture, never the part that changed.
- A run boundary (a new game, and "Restart Game" from the pause menu) resets these channels on
  every machine. Nothing you asked the framework to carry survives that moment: send it again
  from `game.on_game_start`.

Both piles have a size, and both are honest about it in the log:

- **512 calls waiting for one player who has not said hello.** Past that the *newest* call is
  refused rather than the oldest quietly dropped — the order is worth more than the message,
  because a panel you opened and then filled must never arrive filled-then-opened.
- **1024 `all` calls kept for a player who joins later.** Past that the first 1024 are what a
  late joiner receives and each later one is named in the log. Make your table changes when your
  mod loads — every machine runs its own copy of that code — rather than from
  `game.on_game_start`, and you cannot reach this.

---

## Backpacks, items and spells

**The host can read every player's backpack and spells.** `sam_get_inventory`,
`sam_get_inventory_count`, `sam_get_spells` and `sam_player_knows_spell` answer for a remote
player from a copy their own game sends the host. A S.A.M client sends it a moment after joining
and whenever something changes, at most five times a second.

- Those readers return **nil**, not an empty list, 0 or false, when this machine cannot see that
  player's things: a client asking about another player, or the host before that player's game
  has reported (or when it does not run S.A.M).
- **"Not reported yet" happens at the start of every run, not only after a join.** The host drops
  its copy of the last character when a new run begins and that player's game sends the new one a
  moment later. Read another player's backpack or spells from a timer or a later event, never from
  `game.on_game_start`.
- The item uids `sam_get_inventory(p)` returns on the host also name remote players' items, and
  every item function accepts them. Readers answer from the host's copy. Writers
  (`sam_set_item_beatitude`, `_status`, `_count`, `_appearance`, `_droppable`, `_owner`,
  `sam_identify_item`, `sam_remove_item`) are carried to that player's machine and done there.
- A change carried to another machine returns true once it is **sent**, not once it is done. If
  their game refuses it (the item was used up, it is equipped, it is over the stack limit), the
  reason shows up in the host's log.
- The host's copy of a remote backpack shows a carried change only after that player's next
  report, a few ticks later. Reading an item back in the same tick you changed it still gives the
  old value. That includes a removal: after `sam_remove_item` or `sam_set_item_count(uid, 0)` the
  item is still in the host's copy for a moment. Never write "if they still have it, give the
  reward" without a flag of your own, or the reward is given twice.
- **Do not call an item setter every tick for another player's item**; call it when the value
  changes. Each call is carried to that player's machine over the reliable channel whether or not
  it changes anything.
- A change to an item another player is **wearing** also corrects the host's own copy — the one
  combat, AC and `sam_can_unequip` read. If that player swaps to a different item in the same
  instant, the correction is dropped and logged rather than applied to the wrong item, unless the
  two items share a type *and* an appearance, which the host cannot tell apart. The next time the
  item is equipped settles it either way.
- A remote player's item is reported to the host as a type **number**, and a mod's item numbers
  follow the order that machine's mods loaded. The host names the item from its own table, so the
  name is right when both machines load the same mods in the same order and wrong otherwise. An
  item whose type this host has no definition for is left out of that player's backpack entirely
  rather than guessed at.

**Items.** `sam_grant_item` gives items to remote players through the game's own item packet, so
it works even for a player without S.A.M. A mod item arrives as a rock for a player whose game
does not have that mod.

- When an item uid names nothing this machine can see, the yes or no item questions
  (`sam_is_melee_weapon`, `sam_is_shield`, `sam_is_potion_bad`, `sam_is_better_weapon`,
  `sam_is_better_armor`, `sam_is_item_equipped`, `sam_can_unequip`, `sam_can_items_stack`)
  return nil instead of false, and a warning names the function once. `sam_get_item_owner` and
  `sam_get_tome_spell` return false in that case, because nil already means "nobody" and
  "teaches no spell".
- `sam_is_better_weapon` and `sam_is_better_armor` return nil when the second uid is given but
  names no item, instead of silently comparing against nothing.
- `sam_identify_item(player, uid)` refuses when the item is not that player's.
- `sam_get_inventory`'s `equipped` is true only for the exact item being worn, so a spare
  identical ring no longer reads as equipped. It now matches `sam_is_item_equipped`.
- `sam_set_item_droppable` only matters for items a monster can drop. On a player's item the flag
  matters only if a thief steals it while it is worn.
- An owner set with `sam_set_item_owner` on a client player's item stays with the item while it is
  in their backpack, but is lost if they drop it: the game's drop packet carries no owner.
- `sam_get_equipped_item` and `sam_get_equipped_item_id`, like every other player reader, let a
  client read only its own player. Ask on the host for anyone else.
- `sam_inventory_has_space` answers only for a player on this machine, because it reads that
  machine's inventory grid.

**Spells.** `sam_grant_spell` and `sam_remove_spell` work for every player whose game runs
S.A.M. For a player without S.A.M they are refused with a warning. `player.on_spell_learned`
fires once, on the host, when the spell is actually learned.

- `sam_remove_spell` also takes away the spell's item from the backpack and hotbar (and a vanilla
  spell's shapeshift twin). It happens at the end of the tick, so removing a spell inside a cast
  or item handler is safe. Until the end of that frame `sam_player_knows_spell` still answers
  true, and a `sam_grant_spell` of the same spell **cancels** the queued removal — so "remove the
  vanilla spell, then grant my own version of it" in one handler leaves the player with the
  spell, not with neither.
- `sam_grant_spell` accepts the number `sam_get_tome_spell` returns as well as a spell name. When
  granting to another player, prefer the name (`"ns:spell"` or `"spell_fireball"`), because a
  mod's spell numbers follow mod load order.

## Sounds, music and the camera

- `sam_play_sound` plays once per computer, however many splitscreen players share it.
- A sound a script plays reaches the other players **by name**, so everyone hears the same sound
  whatever order their mods loaded in. A player without the mod hears nothing for it.
- A monster's or item's own sound (a sound map) reaches a player without the mod as the game
  sound it replaces, instead of silence.
- `sam_play_sound`, `sam_play_sound_at` and `sam_play_sound_entity` are host functions: a client's
  call is refused with one log line, so an event that fires on every machine no longer plays the
  sound twice, and you need no `sam_is_host()` guard around it.
- `sam_stop_sound` reaches S.A.M clients in order after the play it stops.
- A forced music track (`sam_play_music`, or a monster theme) is dropped when a game ends or a new
  one starts, and a player who joins while one is playing hears it.
- `sam_play_music`'s "until the floor changes" now really means the floor. It used to last until
  the area name changed.
- `sam_get_camera` returns nil for a player on another machine, and logs it once.
- `sam_hitstop` is singleplayer only. In multiplayer it does nothing and says so once in the log.

## HUD, panels and pictures

- HUD, panel, camera, screen flash, impact frame and picture calls reach a remote player's screen
  if you pass that player, or call them inside an event about that player with no player given.
  That player's game must run S.A.M with your mod; a stock client is refused with a log line.
- **On one machine the HUD and panels are shared by every local seat.** The player argument picks
  the *machine*, not the seat: on a splitscreen host, `sam_hud_text(..., 1)` is drawn once over
  the whole window and seat 0 sees it too, and a modal panel frees the mouse for that window.
  The camera, the screen flash, the impact frame and the full-screen picture are the per-seat
  exceptions — each of those is drawn into one seat's own viewport, so they really do belong to
  one seat.
- `ui.on_click`, `ui.on_select` and `ui.on_submit` now carry `player` (who clicked). In
  multiplayer they fire **on the host** for every player's click, and a client's own scripts no
  longer see its clicks.
- **In splitscreen there is one shared set of panels**, and only the seat holding the keyboard can
  interact with them: `ui.on_click`, `ui.on_select` and `ui.on_submit` report *that* seat as
  `player`, never the seat the panel was opened "for". A shop panel opened for seat 2 charges
  seat 1's gold. `sam_ui_is_open` and `sam_ui_input_text` answer for the machine as well, so
  `if not sam_ui_is_open("shop", p) then ... end` is already true for every local seat once any
  one of them has it open. All three are exactly right in a real co-op game, where each machine
  has one local player.
- `sam_ui_is_open(panel [, player])` and `sam_ui_input_text(panel, id [, player])` answer for a
  player on another machine from what their game last reported, so right after `sam_ui_open` they
  can still say closed for a moment.
- A modal panel shown to a client now frees that client's mouse, and typing into its text box no
  longer moves that player.

## Entities, terrain and models

**Spawning and moving.**

- Entities made by `sam_spawn_entity`, `sam_spawn_projectile` and `sam_spawn_companion` now move,
  turn, rise and fall smoothly on every player's screen, and projectiles fly between network
  updates instead of standing still.
- A spawned entity's mod model is sent by name, so every player with the mod sees the right model
  even if their mod list is ordered differently. `sam_get_model(uid)` on a spawned entity returns
  the model id it was spawned with, on every machine — with one exception: a player whose game
  never received the announcement (it joined while more than 512 calls were waiting for it, or it
  does not run S.A.M) answers nil there. The host writes a line to the log when it has to refuse
  an announcement, so this is visible rather than silent.
- `sam_clear_model` on an entity a framework spawner made puts back the model it was spawned with
  rather than leaving it with nothing to draw, and `sam_get_model` then reports that model. There
  is no way to strip such an entity's own model: there is nothing behind it.
- `sam_spawn_portal` and `sam_spawn_companion` are visible to every player. A companion's lunge
  from `sam_companion_punch` is animated on every S.A.M player's machine. A companion's scale is
  clamped to 1.99 (with a warning), the most the network can carry.
- `sam_move_entity` on a connected player really moves them: their own machine applies the move
  through its own collision. A player without S.A.M is corrected with the engine's own position
  packet, which can occasionally be lost. The returned distance is what the host computed.
- `sam_set_position` and `sam_move_entity` on a ground item or a bag of gold that has **already
  come to rest** place it exactly where you ask and leave it there: in mid-air, in a wall alcove, on a ledge, over a pit.
  It does not fall, in any mode. Use `sam_push_entity` when you want it to fall or slide — that
  call wakes the item on purpose. An item still in flight is simply moved mid-fall and lands where
  it lands. Every player whose game runs S.A.M sees the move; a player whose game does not keeps
  seeing it where it was, and the host says so once in the log.
- `sam_apply_force` now moves a resting gold bag (it used to do nothing on any machine), and a
  shoved item or bag slides on every player's screen and ends where the host's copy comes to rest.
- Moving, turning, lifting or scaling a ground item or a prop Barony never updates on a client (a
  gate, a torch) reaches players who run S.A.M. A player without S.A.M keeps seeing the old state,
  and the log says so once.
- `sam_remove_entity` on a follower removes it from its leader's follower list and ally HUD, on the
  leader's own machine too.
- `sam_gib` chunks are seen by every player. A custom gib sprite is sent as a model number, so use
  vanilla model numbers for gibs in multiplayer.
- `sam_spawn_particle` clamps a poof's scale to 0.01 to 655 with a warning, so every player sees
  the same size.
- `sam_find_entities` never returns the engine's shared marker uids (particles, flames, a client's
  own local effects), so every uid it returns can be used with the other functions.

**A player without S.A.M** sees a spawned entity with a special vanilla model (a torch, door,
gate, chest lid, portal or gold bag) behave like that object, and it may stay where it first
appeared. Use a mod model or an ordinary vanilla model for anything that moves. A companion's
model is always a mod model, which such a player cannot draw at all.

**Terrain.**

- `sam_set_tile` edits reach every S.A.M player in the order they were made, including a player
  who finishes loading late. A player without S.A.M sees only dug walls (a wall set to 0), not
  placed walls or floor and ceiling changes. Tile ids only mean the same tile when every player
  has the same mods.
- `sam_tiles_connected` answers correctly on a client after the host changes terrain.

**Visibility, flags and scale.**

- `sam_set_visible` refuses players, monsters, ground items and a creature's **limb** (its weapon,
  shield, helmet or an arm), whose visibility the game rewrites every frame — on a limb INVISIBLE
  is how the game says the slot is empty, so hiding one lasted a single tick. Use
  `sam_apply_effect(uid, "INVISIBLE", ticks)` for creatures, or take the item off. On props,
  spawned entities and companions it now works even when they have a custom model (it used to be
  refused). `sam_set_model` on a limb, on the other hand, works again — floating a cosmetic in an
  empty hand is the usual reason — and it draws on every machine.
- When a monster's invisibility effect ends, every player sees it again (it used to stay invisible
  for everyone but the host). This applies while scripts are loaded.
- `sam_set_entity_flag` refuses BLOCKSIGHT on players and monsters, INVISIBLE_DITHER on players,
  INVISIBLE and INVISIBLE_DITHER on a creature's limb, and BURNABLE on ground items, because the
  game rewrites them every frame. Every other flag on a limb is still allowed.
- `sam_set_scale` refuses players and slimes, whose scale the game rewrites every frame on every
  machine.
- `sam_set_entity_size` on a player whose game is already known not to run S.A.M is refused (their
  own machine could not be told, and they would stick in doorways). On a S.A.M player the new size
  reaches their own machine. Called in the first seconds of a run, before that player's game has
  said hello, it is accepted — but if they then turn out to be running a stock game the size is
  put back and the log explains why. Set a player's size a beat after
  `player.on_player_joined`, or on a later tick.
- `sam_set_elevation` on a ground item reaches S.A.M players. It sets the height **without waking
  the item**, so something that has come to rest stays at the height you give it, and it no longer
  matters whether you call it before or after `sam_set_position`. An item that is still falling or
  sliding is under the engine's physics and pulls itself back down, so lift items that are at rest.

**Reading on a client.** `sam_get_entity_flag`, `sam_is_visible`, `sam_get_scale` and
`sam_get_entity_facing` read this machine's copy. On a client it can lag the host, and a flag the
host clears without telling clients can stay set there; a client's scale is rounded to steps of
1/128. Read on the host when it matters.

The same is true of the position and shape readers, which are otherwise the same answer
everywhere: `sam_get_position`, `sam_get_position_precise`, `sam_get_velocity`, `sam_get_facing`,
`sam_get_entity_size`, `sam_get_entity_sprite`, `sam_get_entity_type`, `sam_get_distance`,
`sam_get_distance_to`, `sam_find_entities` and `sam_get_nearby_entities` all read **this
machine's copy** of the entity, which on a client is interpolated between network updates and can
lag the host by a fraction of a second. Resolving a hit inside `on_packet` with `sam_get_distance`
gives a slightly different answer on each machine; decide it on the host and send the verdict.

- `sam_get_entity_ticks` counts on each machine separately; a client's count starts when that
  client first received the entity. Use the host's value for timing.
- `sam_get_light_at` runs on the host only (a client gets a warning), because the lightmap the
  monster AI reads exists only there.
- `sam_can_stand` and `sam_is_damage_immune` run on the host only; a client's call is refused with
  a warning and returns nil.
- `sam_get_nearby_entities` now works on a client (it used to return an empty table there).
- `sam_get_entity_facing` always returns a value in [0, 2*pi), and now returns one for entities
  whose raw yaw was negative (it used to return nil for them).

## Stats, effects, classes and stored data

**Reading players on a client.** `sam_get_stat`, `sam_get_effect_strength`, `sam_has_effect`,
`sam_is_defending` and `sam_is_spirit_ghost` read only that client's own player; another player's
is refused with a one time warning. The combat readers (`sam_get_hp`, `sam_get_max_hp`,
`sam_get_mp`, `sam_get_max_mp`, `sam_get_attack`, `sam_get_ranged_attack`,
`sam_get_thrown_attack`, `sam_get_bonus_attack_vs`, `sam_get_regen_interval`, `sam_get_healring`,
`sam_get_damage_resist`, `sam_get_magic_resist`, `sam_get_effective_stat`, `sam_get_ac`,
`sam_get_skill`) answer only for the client's own player's uid; anything else is refused with a
warning. Read other players on the host.

**What a refused read gives back.** Where nil is not itself a real answer the refusal is nil;
where 0 or false would be a real answer it is **nothing at all** — no value in Lua, `undefined`
in JavaScript. Twenty-four calls answer that way, whether they are refused as a read about
somebody else's player or as a host-only call on a client: `sam_get_stat`, `sam_get_skill`,
`sam_get_kills`, `sam_get_effect_strength`, `sam_has_effect`, `sam_is_defending`,
`sam_is_parrying`, `sam_is_spirit_ghost`, `sam_is_ghost`, `sam_is_immune`, `sam_is_enemy`,
`sam_is_friend`, `sam_get_light_at`, `sam_add_move_speed`, `sam_get_monster_stat`,
`sam_get_monster_effect_strength`, `sam_get_monster_target_uid`, `sam_monster_has_effect`,
`sam_monster_has_trait`, `sam_monster_can_wield`, `sam_is_action_held`, `sam_is_key_held`,
`sam_ui_is_open` and `sam_random_chance`. This only ever happens in a client-side handler
(`on_packet`, `player.on_before_equip`, `player.on_game_over`), because a refusal cannot happen
anywhere else — but there it is loud on purpose: in Lua the comparison raises with a line number,
and in JavaScript `undefined <= 0` is false where `0 <= 0` would have been true.

- A client's own HUNGER is now at most 5 seconds behind the host's.
- `sam_get_regen_interval` for a client's own player can miss the vampiric aura bonus while more
  than 5 seconds of it remain (clients do not count effect timers). Read it on the host for an
  exact value.
- `sam_get_kills`, `sam_get_effect_duration`, `sam_get_effects`, the `sam_get_monster_*` readers,
  `sam_monster_has_effect`, `sam_get_stat_modifier`, `sam_is_immune`, `sam_preview_damage`,
  `sam_is_parrying`, `sam_is_enemy`, `sam_is_friend`, `sam_get_monster_type`,
  `sam_get_monster_name` and `sam_monster_has_trait` are host only; on a client they are refused
  with a warning.

**Writing.**

- `sam_set_stat(p, "LVL", n)` updates every player's party display immediately.
- Move speed multipliers reach every S.A.M client in order and are caught up at the start of every
  run. A player whose game does not run S.A.M still walks at vanilla speed.
- An XP curve declared when your mod loads now reaches clients on the first floor. The XP bar and
  a client's own experience stay correct above 255 on S.A.M clients; a stock client's bar shows
  experience modulo 256.
- `sam_set_defending` on a remote player lasts exactly one host logic pass. It only changes the
  host's hit resolution: the player's own screen and the other players never show the raised
  shield.
- `sam_revive_player` revives any player in co-op whose game runs S.A.M; a player without S.A.M is
  refused with a warning. The gear the death dropped or bagged is removed from the revived player,
  so nothing is duplicated.
- `sam_set_monster_stat` caps MAXHP at 32767, the enemy HP bar's wire size. A remote player's
  follower shows new MAX HP and level on its owner's ally panel.
- `sam_set_monster_name` on a follower reaches its owner's ally panel and nametag, if the owner
  runs S.A.M. Vanilla has no rename packet, so an owner without S.A.M keeps the old name.
- `sam_spawn_item`: a ground stack above 255 shows as its count modulo 256 on other players'
  screens, but picking it up gives the full count. Spawn smaller stacks if the display matters.

**Tables every machine keeps.** Call `sam_set_species_damage_resist`,
`sam_clear_species_damage_resist`, `sam_patch_class`, `sam_unpatch_class`,
`sam_add_class_passive`, `sam_remove_class_passive` and `sam_patch_item` on the host or at load
time. A client's call is refused; a host call applies on every S.A.M client, and a client that
joins gets the host's whole table. A player whose game does not run S.A.M is named in the host's
log once, rather than skipped in silence — and `sam_hud_text(..., -1)` and its neighbours now say
the same thing, so writing `-1` and naming that player directly no longer tell you different
stories.

**Stored data.** `sam_world_save`, `sam_world_load`, `sam_world_clear`, `sam_world_keys`,
`sam_world_bytes`, `sam_world_bytes_free`, `sam_set_player_data` and `sam_get_player_data` use the
**host's** store. A client's call is refused; send values a client UI needs with
`sam_send_packet`.

## Events and input

Every event's **Multiplayer:** line in the function reference says which machine it fires on and
for whom. The rules that matter most:

- `on_key_pressed` and `on_key_released` fire on the host for every player. A joiner's keys come
  from their own game (A to Z, 0 to 9, F1 to F12), a moment after the press. In splitscreen,
  `player` is the seat holding the keyboard. A joiner whose game does not run S.A.M produces no
  key events.
- `on_action_pressed` and `on_action_released` fire on the host for every player, splitscreen
  seats included. `binding` is that player's own binding.
- **While a chat box or the console is open, every key and every bound action reads as not held
  and fires no press event** — on every machine, in singleplayer too, including the machine's own
  keyboard. A key that was already down when the box opened still fires its release. In
  splitscreen one seat typing quietens every seat, because the game's text focus belongs to the
  machine and not to a seat. This is the one change most likely to surprise a mod that was only
  ever tested in singleplayer, and it is deliberate: the alternative was the same script behaving
  differently for the host and for a joiner.
- `sam_is_key_held(key [, player])`: on the host, a player on another machine reads the keys their
  game reported (A to Z, 0 to 9, F1 to F12 only; false if their game does not run S.A.M). A client
  can only ask about its own player.
- `sam_is_action_held(player, action)` works on the host for every player, from what each joiner's
  game reports. It is false for a joiner whose game does not run S.A.M, and on a client for anyone
  but itself.
- `sam_get_action_binding(player, action)`: on the host, a joiner's binding is the one their game
  reported (nil until it has).
- `sam_message` to a player on another machine shows the line with one leading space. That way the
  joiner's game can never mistake it for one of Barony's own messages that it acts on.
- `sam_player_count` is correct on every machine after a player is dropped for timing out or
  kicked with `/kick` (while a mod is loaded).
- `player.on_item_use`: for a joiner whose game runs S.A.M, the host decides a deliberate Use
  before the joiner's game does anything. A veto uses up nothing and runs nothing, and an allowed
  use happens one network round trip later. For a joiner without S.A.M (or in the first moment of
  a game), a veto stops the effect, but their game has already used the item up. A joiner who
  clicks the same item twice while the host is deciding fires the event **once**; a stack still
  asks once per item in it, so drinking two potions out of a stack of five is two events.
- `player.on_before_equip` fires on the equipping player's own machine; for a joiner, the handler
  runs in the joiner's game. Use only functions that work there (reads of that player, functions
  of kind `any`) and return false to refuse. It covers wielding from the item menu, the hotbar and
  Alt plus right click, as well as the equip by Use path. Starting gear, shops and grants are
  never asked. The event carries `category` as well as `player` and `item_type`. A refusal is
  **silent** — the framework used to add an English "You cannot equip that." that no mod could
  suppress and that was untranslated on a non-English install, and it no longer says anything at
  all — so tell the player why yourself, with `sam_hud_text`. A screen call for the player whose
  handler it is works in the host's game and in a joiner's alike, because it draws on the machine
  the handler is running on; `sam_message` is a **host** function and is refused in a joiner's
  game, so it is the wrong tool here even though it reads like the right one.
- `player.on_item_identified`, `player.on_spell_learned` and `player.on_spell_failed` (reason
  `not_enough_mana`) fire on the host for every player, as reported by each joiner's game. A
  joiner without S.A.M reports nothing.
- `player.on_floor_change` fires once for every connected player when the party takes a ladder;
  `initiator` is the player who climbed. Check `player == initiator` to act once per descent.
  Ladders only: use `game.on_level_entered` for every arrival.
- `player.on_player_left` also fires for lobby leaves, keep alive drops and kicks, and exactly
  once per departure, so it pairs with `player.on_player_joined`.
- `player.on_player_revived` also fires when a ghost respawns (`revive_type` `"ghost_respawn"`).
  The floor load revive (`"floor_load"`) fires once the new level has loaded, just before
  `game.on_level_entered`.
- `player.on_before_revive` fires once per dead player, on the host only (or in splitscreen
  singleplayer), before the level change. Its answer applies on every machine; a joiner's game no
  longer asks its own scripts.
- `game.on_game_end` fires once per run.
- `player.on_callout`: a veto hides the ping from everyone. On a S.A.M joiner it also removes the
  marker from their own screen, but their ping sound has already played. A joiner without S.A.M
  keeps their own marker.
- `player.on_player_joined` is a lobby notification only. Per player state written there
  (`sam_set_player_data`) is cleared when the game starts, and anything sent to the joiner there is
  dropped. Set per player state up in `game.on_game_start` or `game.on_level_entered`.
- `sam_send_packet` works only once the game has started; in the lobby it is refused with a
  warning. A payload may contain zero bytes. Lua handlers receive the exact bytes; JavaScript
  handlers receive text (invalid UTF-8 is replaced), so send text or JSON from mods meant for both
  runtimes. The host ignores a packet that claims to come from slot 0 or from an empty slot.

## Randomness and timers

- `sam_set_timer` and `sam_set_repeating_timer` run on the host only; a client's call is refused
  with a warning. Every machine drops all timers when a run starts, so set timers from
  `game.on_game_start` or later, never from top-level script code.
- `sam_random`, `sam_random_float`, `sam_random_chance`, `sam_random_from_list` and
  `sam_random_weighted` run on the host only; a client's call is refused. The same run seed and the
  same order of calls give the same results on the host. Draws are **not** synchronised between
  machines, so roll on the host and send the result if a client needs it.
- `sam_random_weighted` counts number keys by their text in both Lua and JavaScript (`{[1]=5}` and
  `{1:5}` both mean the key `"1"`) and returns the key as a string. `sam_random_from_list` indexes
  Lua lists from 1 and JavaScript arrays from 0; the same list gives the same element.

## Lua and JavaScript now agree

The multiplayer work closed the remaining places where the two runtimes behaved differently.
These are behaviour changes, so check your mod if it relied on the old ones.

- In Lua, `sam_get_effects` and `sam_get_monster_effects` name effects the way JavaScript and the
  effect events always did: lowercase (`"poisoned"`) for vanilla, the mod's `"ns:effect"` for
  custom, and `"CUSTOM:<id>"` for an unnamed slot. **A Lua script comparing against `"POISONED"`
  must change.**
- In Lua, `sam_travel_to_level(floor, { secret = 0 })` now means "not secret", as it does in
  JavaScript.
- In Lua, `sam_get_effective_stat`, `sam_get_ac`, `sam_get_skill`, `sam_is_enemy` and
  `sam_is_friend` treat a uid of 0 or less as no entity, as JavaScript always did.
- `sam_set_monster_data` returns true in Lua as in JavaScript, and `sam_set_player_data` returns
  nothing in JavaScript as in Lua.
- **Wherever Lua answers `nil`, JavaScript now answers `undefined`. No S.A.M call ever gives back
  `null`.** That was not true of every function before, and it matters: `null < 10` is **true** in
  JavaScript while `undefined < 10` is false, so a read that could not answer used to take the
  branch it was meant to fail. `sam_world_load` for a key never stored and `sam_get_item_category`
  for an unknown item are two that changed; the rule now has no exceptions to memorise.
- In JavaScript, `sam_get_inventory` and `sam_get_spells` name a mod's item or spell by its
  `"ns:id"`, as Lua always did. In Lua, a mod item's `name` in `sam_get_inventory` is its
  `"ns:item"` id too (it used to say `"custom"`).
- `sam_cast_spell` accepts the numeric spell id `sam_get_tome_spell` returns, like
  `sam_cast_spell_at` and `sam_cast_spell_pos`.
- `sam_patch_class` and `sam_patch_item` read tables the same way in Lua and JavaScript: keys are
  case insensitive, number fields accept only numbers and text fields only strings, `value` beats
  `gold_value` and `name_identified` beats `name`.
- `sam_modify_value` works inside `player.on_xp_gained`; `player.on_gold_collected` offers no value
  to rewrite.
- **In JavaScript a missing or malformed argument is now an error, as Lua raises, instead of
  quietly becoming 0, 1.0 or player 0.** This covers `sam_set_stat`, `sam_apply_effect`,
  `sam_set_effect_duration`, `sam_set_move_speed`, `sam_add_move_speed`, `sam_spawn_item`,
  `sam_cast_spell` and its `_at` and `_pos` twins, `sam_set_monster_stat`, `sam_get_monster_stat`,
  `sam_monster_has_effect`, `sam_get_monster_target`, `sam_set_monster_target`,
  `sam_monster_path_to`, `sam_monster_face`, `sam_modify_damage`, `sam_modify_monster_damage`,
  `sam_modify_value`, `sam_get_player_data`, `sam_set_player_data`, `sam_screen_flash`,
  `sam_impact_frame`, `sam_camera_shake`, `sam_hitstop`, `sam_hud_text`, `sam_hud_bar`, the camera
  functions, the positional sound functions, `sam_spawn_portal`, `sam_set_entity_size`,
  `sam_get_nearby_entities`, `sam_send_packet`, `sam_random`, `sam_random_chance`,
  `sam_get_equipped_item`, `sam_get_equipped_item_id`, `sam_is_item_equipped`, `sam_can_unequip`,
  `sam_can_items_stack`, `sam_identify_item`, `sam_grant_spell`, `sam_player_knows_spell`,
  `sam_remove_spell` and `sam_remove_item`.
- **In JavaScript, nine more functions now refuse a missing player argument instead of inventing
  player 0**: `sam_get_stat`, `sam_get_move_speed`, `sam_message`, `sam_is_action_held`,
  `sam_get_action_binding`, `sam_get_inventory_count`, `sam_has_effect`, `sam_get_inventory` and
  `sam_get_effect_strength`. They log an error naming the argument and return `undefined`
  (`false` for `sam_message`). The Lua versions always raised; this is the parity fix. If your
  TypeScript mod reads the player out of an event field that event does not carry, you now see it
  in the log instead of silently getting "normal speed" or "carries nothing".
- **Everything in a table you pass to an `all` call has to be able to cross the network** —
  numbers, strings, booleans and tables of those, nested up to four deep. A function in the table
  (the common `apply = function(p) ... end` beside the stats) means the call is refused on **every**
  machine, with one error naming the argument, instead of quietly patching the host's table and
  nobody else's. Keep your own metadata in a separate table. Singleplayer is unaffected either way,
  so this shows up the first time the mod is hosted.

---

## Testing multiplayer on one computer

You do not need a second computer or a second copy of Barony. Start the same install twice and
have the second copy join the first over the local loopback address.

**1. Start the first copy** (this will be the host) and leave it at the main menu.

**2. Start the second copy from the same folder**, windowed so both fit on screen:

```
barony.exe -windowed -size=1280x720
```

On Windows, run that from a command prompt in the game folder, or make a shortcut with those
arguments. (The first copy can stay fullscreen.)

**3. Host a LAN game in the first copy.** Play Game, then Host LAN Party. The port is **57165**.

**4. Join from the second copy.** Play Game, then Join LAN Party, and enter:

```
127.0.0.1
```

(Port 57165, which is the default.) The second copy is now player 1 in the first copy's game, on
the same machine.

**Each copy writes its own log.** The first copy writes `sam_log.txt` as always. The second copy
notices that another copy of the game already holds it and writes **`sam_log_2.txt`** instead (a
third would write `sam_log_3.txt`). It does not rotate, archive or truncate the first copy's live
log, and the first line of its own log says which copy it is — so if the banner says "this copy of
the game is instance 2", that session's log is `sam_log_2.txt`. A machine that cannot create the
lock file at all falls back to `sam_log.txt`, exactly as it behaved before instance slots existed. So you can watch both sides of your
mod at once:

```
tail -f sam_log.txt        # the host: your events, timers, PASS/FAIL lines
tail -f sam_log_2.txt      # the joiner: what its on_packet handler received
```

**What to test with two copies.** The joiner is player 1, so the host's script should exercise
everything against player 1: a HUD line and a panel on their screen, granting and removing an item
and a spell, changing a stat and their move speed, a custom sound, a spawned entity, and a key or
a panel click made in the second window arriving on the host with `player = 1`.

**Write the test as a script, not a checklist.** Have the host's script run each check in turn and
print a PASS, FAIL or SKIP line carrying the value it actually saw, then a total at the end. A
whole pass is then one read of `sam_log.txt` rather than a session of watching two windows, and
you can repeat it after every change.

Three rules make such a test worth trusting.

**Decide a two-machine check from what the joiner reported back, never from what the host asked
for.** A change carried to another machine returns true once it is *sent*, not once it is done,
so that return value says nothing about whether it worked. Put the other half of the check in
your own script's `on_packet` handler, which runs in the joiner's game: have it read the result
there with a function a client may call, and send the answer home with `sam_send_packet`. Assert
on what comes back. Anything you cannot get an answer back for is a SKIP, not a PASS.

**Put everything back, and do not assume the run ends tidily.** Record the old value beside the
change and restore it the moment the check is done. A change to a table every machine keeps its
own copy of, such as a race's resistances or an item's gold value, outlives the run inside the
process, so restore those from `game.on_game_start` as well, in case the last run was abandoned
or quit to the menu. A change to a character cannot be put back at all once that run is gone, so
keep those windows short and leave anything the engine cannot undo, a level up above all, out of
the test entirely.

**Play a character nobody minds losing, on both copies.** Even a careful test grants items, moves
stats and changes move speed, and a bug in the test lands in somebody's save.

**Give the late-join path a pass of its own.** A player who joins part way through is caught up
by separate code from the ordinary path, and it is the half more likely to be wrong. Run the whole
set again whenever a floor is entered with more players than the last run measured, then, with the
joiner already in the game, take one staircase.

**Things worth checking by eye**, because no script can observe them: that a HUD line really
appears in the second window, that the screen flash is visible there, that a sound is audible
once and not twice, and that a spawned entity moves smoothly rather than teleporting.
