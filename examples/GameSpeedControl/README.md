# Game Speed Control

Four preset keys that change how fast the world runs, each with its own speed slider, and a small
readout in the corner while the speed is not 1x. Singleplayer.

A community mod of the same name does this with a patched `barony.exe`. This folder does it with a
script, so it composes with every other mod, updates with the framework, and needs no reinstall
after Steam verifies the game's files.

## What the player sees

- **Settings > Controls > Bindings** gains four rows under *Game Speed Control*: slot 1 to 4, on
  Keypad 0 to 3 by default, rebindable like any other action. A rebinding survives the mod being
  unloaded and comes back when it is loaded again.
- **Settings > General** ends in a *MOD SETTINGS* section with a slider per slot (0.1x to 8x,
  defaults 0.5, 1, 2, 8) and a *Bullet time on a kill* toggle. Values persist per mod.
- In game, pressing a slot key sets that slot's speed. `/sam_gamespeed 2.5` does the same from the
  console; `/sam_gamespeed` alone prints the current speed.

## The three framework pieces

| what | function |
|---|---|
| a rebindable key in the Bindings page | `sam_register_action(id, label, default_key)` |
| a slider or toggle in the settings | `sam_register_setting(id, spec)` and `sam_get_setting(id)` |
| the speed itself | `sam_set_game_speed(multiplier [, ticks])`, `sam_get_game_speed()`, `game.on_speed_changed` |

Everything counted in game ticks follows the multiplier: monsters, hunger, effect durations, the
run timer, your own timers and `on_tick`. Everything on a wall clock does not: rendering, menus,
`sam_hitstop`, a screen flash. Above 1x is best effort; 8x needs every game tick to finish inside
2.5 ms, and a slower machine gets less.

Known limit: mouse look is applied once per game tick, so at 0.1x the camera turns in five steps a
second while the picture stays smooth.

## Try it

Copy this folder to `mods/`, load it, start a singleplayer game, press Keypad 2.
