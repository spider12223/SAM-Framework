/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_speed.cpp
	Desc: see sam_speed.hpp.

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros.
// Must precede every include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_speed.hpp"
#include "sam_event.hpp"    // game.on_speed_changed: one build, both runtimes
#include "sam_logger.hpp"
#include "sam_net.hpp"      // warnOnce: a refusal inside on_tick must not write fifty lines a second

#include "main.hpp"         // mousexrel / mouseyrel, MAXPLAYERS, real_t, Sint32
#include "game.hpp"         // multiplayer, SINGLE
#include "player.hpp"       // inputs, Inputs::VirtualMouse

#include <cmath>
#include <cstdio>
#include <string>

static const char* MOD = "SPEED";

namespace SAMSpeed
{
namespace
{
	double s_mult = 1.0;        // what is set
	double s_base = 1.0;        // where a temporary window returns to
	int    s_windowTicks = 0;   // game ticks left in the window; 0 = none

	// The look delta held back between the ticks of one frame (see holdLookDelta).
	bool   s_lookHeld = false;
	Sint32 s_heldX = 0, s_heldY = 0;
	struct HeldRel { Sint32 xrel = 0, yrel = 0; real_t fxrel = 0.0, fyrel = 0.0; };
	HeldRel s_heldV[MAXPLAYERS];

	std::string fmt(double m)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.2f", m);   // bare: the engine macro-defines it on Windows
		return std::string(buf);
	}

	bool inRange(double m)
	{
		// NaN fails both comparisons, which is exactly what refuses it; +-inf fail one each.
		return m >= MIN_MULT && m <= MAX_MULT;
	}

	// Told AFTER the value is stored, so sam_get_game_speed() inside a handler already
	// answers the new speed. The event's fields are integers -- the runtimes' Event type
	// carries integers and strings only -- so the multiplier crosses as a whole percent
	// (100 = 1x, 250 = 2.5x); a handler that wants the exact double reads the getter.
	void announce(double oldMult, double newMult, bool temporary)
	{
		SAM_INFO(MOD, "Game speed " + fmt(oldMult) + "x -> " + fmt(newMult) + "x"
			+ (temporary ? " for " + std::to_string(s_windowTicks) + " ticks" : std::string()) + ".");
		SamEvent e("game.on_speed_changed");
		e.i("old_percent", (long long)std::llround(oldMult * 100.0))
		 .i("new_percent", (long long)std::llround(newMult * 100.0))
		 .i("temporary", temporary ? 1 : 0);
		e.fire();   // not cancellable: the speed is already what it is
	}
}

bool active()
{
	return s_mult != 1.0 && multiplayer == SINGLE;
}

double multiplier()
{
	return active() ? s_mult : 1.0;
}

bool drivesClock()
{
	return active() && !gamePaused;
}

int maxTicksPerFrame(int vanilla)
{
	if ( !drivesClock() || s_mult <= 1.0 ) { return vanilla; }
	return (int)std::ceil((double)vanilla * s_mult);
}

void tick()
{
	if ( s_windowTicks <= 0 || gamePaused ) { return; }
	if ( --s_windowTicks > 0 ) { return; }
	const double old = s_mult;
	s_mult = s_base;
	if ( old != s_mult ) { announce(old, s_mult, false); }
}

void holdLookDelta()
{
	if ( !drivesClock() || s_mult <= 1.0 ) { return; }
	if ( s_lookHeld ) { return; }   // already zero since this frame's first hold
	s_lookHeld = true;
	s_heldX = mousexrel;
	s_heldY = mouseyrel;
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		const auto* vm = inputs.getVirtualMouse(i);   // the type is private to Inputs, its fields are not
		s_heldV[i].xrel = vm->xrel;
		s_heldV[i].yrel = vm->yrel;
		s_heldV[i].fxrel = vm->floatxrel;
		s_heldV[i].fyrel = vm->floatyrel;
	}
	// The same zeroing the main loop's tail does after a frame that ran a tick (game.cpp,
	// `if (ranframes)`), done early so the frame's remaining ticks read no motion.
	mousexrel = 0;
	mouseyrel = 0;
	inputs.updateAllRelMouse();
}

void releaseLookDelta()
{
	if ( !s_lookHeld ) { return; }
	s_lookHeld = false;
	mousexrel = s_heldX;
	mouseyrel = s_heldY;
	for ( int i = 0; i < MAXPLAYERS; ++i )
	{
		auto* vm = inputs.getVirtualMouse(i);
		vm->xrel = s_heldV[i].xrel;
		vm->yrel = s_heldV[i].yrel;
		vm->floatxrel = s_heldV[i].fxrel;
		vm->floatyrel = s_heldV[i].fyrel;
	}
}

bool set(double mult, int forTicks)
{
	if ( !inRange(mult) )
	{
		// Once per session, not once per frame: a preset key held down, or an on_tick caller
		// with a bad number, would otherwise flood the log with the same line.
		SAMNet::warnOnce("sam_set_game_speed|range", "sam_set_game_speed: refused " + fmt(mult)
			+ ", the multiplier must be " + fmt(MIN_MULT) + " to " + fmt(MAX_MULT) + " (0 would stop the"
			" clock every timer and fade runs on, and past 8x the engine cannot keep up). Nothing changed.");
		return false;
	}
	if ( multiplayer != SINGLE )
	{
		SAMNet::warnOnce("sam_set_game_speed|singleplayer", "sam_set_game_speed: singleplayer only, so it did"
			" nothing. The host's world would run at " + fmt(mult) + "x while every joiner's own body, which"
			" its own machine moves, stayed at 1x.");
		return false;
	}
	const double old = s_mult;
	if ( forTicks > 0 )
	{
		// A window opened inside a window keeps the ORIGINAL base, so two bullet-time crits
		// in a row end at normal speed and not at the first crit's slow motion.
		if ( s_windowTicks <= 0 ) { s_base = s_mult; }
		s_windowTicks = forTicks;
	}
	else
	{
		s_windowTicks = 0;
		s_base = mult;
	}
	if ( mult == old ) { return true; }   // idempotent: nothing changed, nothing to tell
	s_mult = mult;
	announce(old, mult, forTicks > 0);
	return true;
}

void reset()
{
	s_windowTicks = 0;
	s_base = 1.0;
	const double old = s_mult;
	s_mult = 1.0;
	if ( old != 1.0 ) { announce(old, 1.0, false); }
}
}
