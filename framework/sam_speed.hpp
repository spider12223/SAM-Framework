/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_speed.hpp
	Desc: the simulation speed multiplier (sam_set_game_speed / sam_get_game_speed,
	      /sam_gamespeed): how many game ticks a real millisecond buys.

	WHY THIS EXISTS

	The community's Game Speed Control is a patched exe with one console command,
	/gamespeed 0.1-8. The engine makes that a one-number change: real time enters the
	simulation in exactly one place, the accumulator in handleEvents() (game.cpp,
	`time_diff += timesync`), which is drained in 20 ms slices into gameLogic() calls
	and capped at five a frame. Scale what goes INTO the accumulator and the whole
	world runs at Nx while the renderer, the menus, the message feed and everything
	else on a wall clock carries on at 1x. This module owns that number; the engine
	reads it twice a frame and never anywhere else.

	WHAT SCALES AND WHAT DOES NOT

	Anything counted in ticks scales, which is the point: monster AI, hunger, effect
	durations, regen, fades, the run timer the game displays, script timers, on_tick.
	Anything on the wall clock does not: rendering, the UI, sam_hitstop (an SDL_GetTicks
	deadline), a screen flash, sam_get_real_time, sam_get_fps. The two time seams
	compose without code: a 200 ms hitstop during 4x freezes 40 ticks of monster logic
	instead of 10, while the player and the HUD animate at 4x.

	SINGLEPLAYER ONLY. `ticks` is a per-machine counter that no packet ever syncs, so
	a host-only multiplier is not a lockstep desync; it is worse in a plainer way. The
	host's world (monsters, and every player's hunger, regen and effects, which are
	host-side) runs at Nx real time while every joiner's own body, which its own
	machine moves, runs at 1x. set() refuses in any netgame and says so once.
	Splitscreen is multiplayer == SINGLE sharing one loop and one `ticks`: fine.

	A TEMPORARY WINDOW (bullet time on a critical hit) is counted in GAME TICKS at the
	new speed, not in real time: that is the unit every other S.A.M duration uses
	(timers, effects, on_tick), and it is counted from the same loop that advances
	`ticks`, so it needs no second clock and repeats exactly. A mod that wants "half a
	real second at 0.25x" converts: ticks = seconds * 50 * multiplier (0.5 * 50 * 0.25
	= 6). The window pauses with a level load, which runs no ticks, and keeps counting
	while the game is paused, as script timers do (`++ticks` is unconditional).

	WHAT 0.1x LOOKS LIKE, HONESTLY. Rendering stays at the frame limit, but the world
	moves in tick-sized steps (5 a second at 0.1x) and so does mouse look, because
	the camera turns inside the tick (actplayer.cpp handlePlayerCameraUpdate(false)).
	The engine's per-frame look path exists only with /usecamerasmoothing, and that
	path also moves the BODY per frame, which is the wrong thing under a multiplier.
	There is no cheap fix; it is documented instead.

	THE IRON RULE. With no speed set, active() is one comparison, the accumulator
	line evaluates the vanilla expression and the catch-up cap is the vanilla 5.

-------------------------------------------------------------------------------*/

#pragma once

namespace SAMSpeed
{
	// The allowed range. 0 is refused, not clamped: it would starve `ticks`, and with
	// it script timers, fades and the unattended test's floor cadence. Above 8 is
	// refused because 8x is already CPU-bound: 400 ticks a second means every
	// gameLogic() must finish inside 2.5 ms, or the catch-up cap drops ticks and the
	// real speed sags below what was asked for.
	constexpr double MIN_MULT = 0.1;
	constexpr double MAX_MULT = 8.0;

	// ---- what the engine reads (game.cpp handleEvents) ------------------------------

	// True while a speed other than 1.0 is set, in singleplayer. One comparison.
	bool active();

	// The multiplier the accumulator applies. 1.0 whenever active() is false, which
	// includes any netgame whatever was set: a missed reset can never scale a netgame.
	// Also what sam_get_game_speed reports, so it stays the set speed while paused.
	double multiplier();

	// active(), and the game is not paused: whether the multiplier reaches the clock this
	// frame. Paused, the world is frozen (every entity update checks gamePaused) and all
	// that still counts ticks is the menus: a slider's key repeat, the text cursor, the
	// widget glow. At 8x those ran eight times too fast, so one tap on a mod's slider in
	// the pause menu moved it six steps. The speed is kept, not reset, and comes back on
	// unpause.
	bool drivesClock();

	// The catch-up cap for this frame, given vanilla's. Above 1x it grows with the
	// multiplier, so 8x at 60 fps (6.67 ticks a frame) is not silently clamped to
	// vanilla's 5 (= 6x); at or below 1x it is `vanilla` untouched.
	int maxTicksPerFrame(int vanilla);

	// Once per game tick, from the tick loop right after ++ticks. Counts a temporary
	// window down and restores the previous speed when it ends. No-op without one, and
	// while paused: a window is measured in ticks of the world, which is not running.
	void tick();

	// Above 1x a frame runs more ticks than 60 fps vanilla ever does, and every tick
	// re-reads the same mouse delta (mousexrel and the virtual mouse's rel), so look
	// would turn ticks-per-frame times too far. holdLookDelta() is called before every
	// tick of a frame except the first: the first call saves the deltas and zeroes
	// them the way the main loop's tail does, so the later ticks see no motion.
	// releaseLookDelta() after the loop puts them back, because the frame's UI pass
	// (ui/Frame.cpp, interface/drawstatus.cpp) reads them afterwards to know the mouse
	// moved at all; the loop tail then zeroes them as it always did. Both are no-ops
	// at or below 1x, so a vanilla frame is untouched.
	void holdLookDelta();
	void releaseLookDelta();

	// ---- what scripts and the console call --------------------------------------------

	// Set the multiplier. Refuses (false, one log line per session) NaN, 0, anything
	// outside [MIN_MULT, MAX_MULT], and any netgame; nothing changes on a refusal.
	// forTicks > 0 makes it a temporary window: after that many game ticks the speed
	// goes back to what it was before the window, and a second window opened inside
	// the first keeps the ORIGINAL base (two bullet-time crits in a row end at normal
	// speed, not at the first crit's slow motion). forTicks <= 0 is a plain set and
	// cancels any window. Setting the value that is already set fires nothing (a window
	// is still re-armed), so a preset key or an on_tick caller may call this every
	// frame. Fires game.on_speed_changed whenever the value really changes.
	bool set(double mult, int forTicks);

	// Back to 1.0 with no window. From doNewGame, which every route between two runs
	// passes (a restart in place skips doEndgame), and from doEndgame, because
	// gameLogic keeps ticking the main-menu map after a run and a speed left set would
	// run the menu background at Nx. Fires the event if the value changes.
	void reset();
}
