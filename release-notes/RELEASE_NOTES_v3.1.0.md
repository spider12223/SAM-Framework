# S.A.M Framework v3.1.0

**Two community mods, rebuilt as things any mod can do.** Game Speed Control and Item Randomizer are
two Barony mods on the Workshop, and each one is a replacement file: a patched exe for the first, a
replacement `items.json` for the second. So each one conflicts with anything else that touches the
same file. This release takes what they do and turns it into functions, events and
menu rows that any S.A.M mod can use, and ships both mods rebuilt on top as examples.

Twenty new functions, taking the API from 316 to 336, and eleven new events, from 76 to 87. There
is also a way to test a mod without playing it.

## Game speed

```lua
sam_set_game_speed(2)          -- everything at double speed until you say otherwise
sam_set_game_speed(0.25, 6)    -- bullet time: quarter speed for 6 game ticks, then back
sam_get_game_speed()           -- 1.0 when nothing is set
```

From 0.1x to 8x, in singleplayer. Everything the game counts in ticks follows: monsters,
projectiles, hunger, regeneration, effect durations, your own timers and `on_tick`. Rendering,
`sam_hitstop` and screen flashes stay on the wall clock, and while the game is paused the
multiplier is set aside, so the menus behave normally at any speed. Measured on the development machine:
50.00, 200.25, 25.00 and 5.00 game ticks per real second at 1x, 4x, 0.5x and 0.1x.

A temporary window goes back on its own, and a window opened inside another one returns to the
original speed, so two quick kills in a row do not leave the player stuck at a quarter speed.
`game.on_speed_changed` tells a HUD when to redraw, and `/sam_gamespeed 2.5` does the same from the
console. In a netgame the speed cannot be changed and the call says so once in the log, because a
joiner's own body is moved by their own machine at 1x.

**[docs/game-speed.md](docs/game-speed.md)**

## Keys and settings in the game's own menus

```lua
sam_register_action("slot1", "Game speed slot 1", "Keypad 0")
sam_register_setting("slot1_speed", { type = "slider", label = "Slot 1 speed",
	min = 0.1, max = 8, step = 0.1, default = 0.5 })
```

`sam_register_action` adds a row to **Settings > Controls > Bindings** under your mod's name. The
player rebinds it like any vanilla action, it is saved with their other bindings, Restore Defaults
brings your default back, and it fires `on_action_pressed` like a vanilla action. In co-op a
joiner's press reaches the host with the joiner's own binding.

`sam_register_setting` adds a slider, toggle, dropdown, number or text box to a new **MOD SETTINGS**
section at the end of **Settings > General**, in the main menu and the pause menu. Confirm, Discard
and Restore Defaults work on it the way they work on everything else. The value is saved in your
mod's own data folder, never in `config.json`, and it survives a restart and your mod being
unloaded. `sam_get_setting`, `sam_set_setting` and `sam_list_settings` read and change it from a
script, and `mod.on_setting_changed` fires when it changes.

From the keyboard or a controller a slider moves in its own steps, so a slider from 0.1 to 8 in
steps of 0.1 goes 0.5, 0.6, 0.7 rather than jumping a whole unit.

**[docs/mod-settings.md](docs/mod-settings.md)**

## The loot pool

```lua
sam_set_loot_weight("GEM_ROCK", 0)                    -- never rolled (garbage chests still hold rocks)
sam_set_loot_weight("ARTIFACT_SWORD", 3)              -- three times as likely as a vanilla entry
sam_set_loot_context("ARTIFACT_SWORD", "shop", false) -- it drops, but no shop stocks it
sam_set_loot_fallback("WEAPON", "IRON_SWORD")         -- an empty pool gives a sword, not a rock
```

Most random items in Barony (floor items, chest contents, most shop stock, a creature's rolled
gear) come out of one engine function, and S.A.M now sits beside it. Six tables shape what it
returns: a weight per item, a weight per category, a floor window per item
(with the ceiling vanilla never had), where an item may appear (floor, chest, shop, monster,
alchemy recipe), what an empty pool falls back to, and which spells' books may drop.
`sam_get_loot_pool` shows the candidates the level window and the tables allow, with their weights,
and `sam_roll_loot` draws from them the way the engine does. The tables do not reach what the game
picks by name or from a fixed range (a goblin's weapon, the apothecary's potions, a garbage
chest's rocks), the gold-value draw (the lockpick reward, automatons, cockatrices), or the nine gem
rolls in ten that the engine turns into glass before it looks at the pool. The events and the
container functions below reach most of those.

Nine events cover what the tables cannot: before and after an item is rolled, before a spellbook
is re-drawn, before and after a chest is filled, before and after a shop is stocked, a creature's
starting gear, and the fixed rewards (the fountain, the sink, the luckstone, the lockpick). The
"before" events can change what is rolled, and the chest and shop ones can cancel the fill or the
stocking outright. Four functions rewrite a chest, a creature's pockets or a shop directly.

**One limit, stated plainly.** The game generates each floor on a background thread while the
loading screen is up, and no script can run there. So the events fire for everything rolled during
play (a shop stocking, a creature's pockets, a chest summoned by a spell, `sam_roll_loot`) and
never for the items and chests a floor is generated with. The tables govern those, and that is
what the tables are for.

The original Item Randomizer documents a bug in its own notes: its deep Arms & Armour stores sell
nothing but rocks. From floor 18 the store rolls weapons from level 10, armour from 5 and thrown
items from 8; the edited sheet has nothing at those levels, and the engine's answer to an empty
pool is a rock. The rebuilt mod fixes the store by widening its window, with `e.min_level = 0` in
`world.on_before_loot_roll` for shop rolls. A fallback is not the fix for that: it swaps the rock
for one named item, so a store whose whole window is empty would sell copies of that one item.

**[docs/loot-and-shops.md](docs/loot-and-shops.md)**

## The two mods

`examples/GameSpeedControl` has four rebindable speed keys on the keypad, a slider behind each one,
and an optional bullet time on every kill. `examples/ItemRandomizer` puts every item in the game
into the loot pool on every floor, including artifacts, orbs, keys and class gear, keeps the deep shops
selling, and has a Lite toggle that keeps artifacts and orbs out of shops. Neither one replaces a
file, so both work alongside any other mod, including mods that add their own items: those join
the randomizer's pool through the same rules.

## Testing a mod without playing it

```bash
tools/run_mod_test.sh HelloTest
```

`barony.exe -samtest=YourMod` loads the mod, starts a game, lets the mod run its own checks and
exits with a code: 0 everything passed, 1 something failed, 2 it never finished, 3 the mod could
not be loaded. A mod reports its result with the new `sam_test_done(pass, fail)`, which does
nothing outside a test run, so it is safe to leave in a mod people play. `examples/HelloTest` is
the smallest mod that can be run this way and a template for your own. The watchdog reports a
hung run as a failure, not a pass. The seed is fixed by default, so the same command walks the same
dungeon twice.

It is the real game on a real dungeon, not a simulation. Every system in this release was tested
this way before anyone played it, and it found two real bugs that playing would probably have
missed. One of them made `sam_roll_loot` return the same item 200 times in a row.

**[docs/testing-mods.md](docs/testing-mods.md)**

## Also

- `sam_patch_item` changes to an item's level used to be undone by `/loaditems` and by the data
  reload at the start of a game. They are now reapplied after every reload.
- `/sam_reload` is refused for the whole of a run. It used to check whether the player had a body,
  so a dead player in singleplayer could reload mid-run while the save still held the old ids.
- The function reference now fails the ship check if a function's documented arguments disagree
  with what either runtime accepts, or if an event is documented but never fired.

## Numbers

316 functions to 336. 76 events to 87. Nothing was removed and no argument list changed meaning,
so a mod written for 3.0.0 keeps working.

## What has and has not been tested

In the real game, in singleplayer: game speed (`SpeedTest`, 41 checks), mod settings and
persistence across a restart (`SettingsTest`, 88 checks), the loot tables, events and container
functions (`LootTest`, 90 checks), sounds and music again (`AudioTest`, 31 checks) and the new
`HelloTest` example (11 checks), all passing against the release build. Game Speed Control and
Item Randomizer load together with no errors or warnings. The four speed keys in Game Speed Control
have been played by a person. The keyboard slider and bullet time in Game Speed Control, Item
Randomizer in a normal game, and the multiplier being set aside while paused (a test mod cannot
pause the game) have not been played by a person yet.

**Multiplayer has not been played for any of this.** The loot tables, mod actions and settings rows
are carried to joiners and replayed to late joiners the same way as the 3.0.0 functions, and they
passed an audit that checked the co-op paths. But reading code is not the same as playing co-op. If
something is wrong in co-op, the host's `sam_log.txt` should say so, and I would rather hear about
it than have you work around it.
