# Game speed

`sam_set_game_speed(multiplier [, ticks])` runs the world faster or slower than real time, from
0.1x to 8x, in singleplayer. It changes how many game ticks a real second buys: at 4x the game runs
200 ticks a second instead of 50.

```lua
sam_set_game_speed(2)          -- everything at double speed until you say otherwise
sam_set_game_speed(0.25, 6)    -- bullet time: quarter speed for 6 game ticks, then back
sam_get_game_speed()           -- 1.0 when nothing is set
```

Everything the game counts in ticks follows: monsters, projectiles, hunger, regeneration, effect
durations, fades, the run timer the game displays, your own timers and `on_tick`. Everything on a
wall clock does not: rendering stays at your frame rate, the message feed is unchanged, the pause
menu runs at normal speed (see "While paused" below), `sam_hitstop` and a screen flash last their
real duration, `sam_get_real_time` and `sam_get_fps` are untouched.

Above 1x is best effort. The engine runs as many ticks as your machine can fit in a frame, so 8x on
a slow machine is less than 8x. The acceptance test on the development machine measured 50.00,
200.25, 25.00 and 5.00 ticks per real second at 1x, 4x, 0.5x and 0.1x.

## Temporary windows

A second argument makes the change temporary: after that many **game ticks at the new speed** it
goes back to what it was, with no timer of your own to keep. Convert from real time as
`ticks = seconds * 50 * multiplier`, so half a real second at 0.25x is 6. A window opened inside a
window keeps the original speed to return to, so two quick kills do not strand the player at 0.25x.

## What is refused

Calling it with the value already set does nothing and fires nothing, so a preset bound to a key
may call it every frame. 0, NaN and anything outside 0.1 to 8 are refused and leave the speed
alone. In a netgame it is refused with one warning: the host's world would run at Nx while every
joiner's own body, which their own machine moves, stayed at 1x. `sam_get_game_speed()` is always
1.0 in a netgame.

The speed resets to 1x when a run ends and when a new one starts.

## While paused

The multiplier stops reaching the clock while the game is paused. The world is frozen then anyway,
and what still counts ticks is the menus: a slider's key repeat, the text cursor, the glow on the
selected widget. Left at 8x, one tap on a slider in the pause menu moved it six steps. The speed is
kept, not reset: `sam_get_game_speed()` still reports it, nothing fires, and it is back on unpause.
A temporary window does not count down while paused either, so pausing during bullet time does not
use it up.

## The event

`game.on_speed_changed` fires on the host, in singleplayer, whenever the speed really changes,
including when a window ends and when a run resets it to 1x. It carries `old_percent` and
`new_percent` (100 is 1x; event fields are whole numbers) and `temporary` (1 while a window is
running). The value is already stored when it fires, so `sam_get_game_speed()` inside the handler
is the exact new multiplier. A HUD that shows the speed listens here instead of polling:

```lua
if e.name == "game.on_speed_changed" then
	local mult = sam_get_game_speed()
	if mult == 1 then sam_hud_clear("speed")
	else sam_hud_text("speed", 8, 8, string.format("%.2gx", mult), 0xFFFFFFFF, 0) end
end
```

## The console

`/sam_gamespeed 2.5` does the same from the console; `/sam_gamespeed` alone prints the current
speed.

## Known limit

Mouse look is applied once per game tick, so at 0.1x the camera turns in five steps a second while
the picture stays smooth. The engine's per-frame look path (`/usecamerasmoothing`) also moves the
body per frame, which is wrong under a multiplier, so it is not used. Above 1x the framework holds
the frame's mouse delta so sensitivity does not multiply with the number of ticks in a frame.

## A whole mod

`examples/GameSpeedControl` is the community mod of the same name rebuilt on this: four rebindable
preset keys with their own speed sliders in the settings, and a corner readout. See
[Mod settings](mod-settings.md) for the keys and sliders.
