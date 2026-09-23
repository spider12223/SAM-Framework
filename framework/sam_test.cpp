/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_test.cpp
	Desc: see sam_test.hpp.

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros.
// Must precede every include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_test.hpp"
#include "sam_logger.hpp"
#include "sam_loader.hpp"   // samScriptLoadFailures

#include "main.hpp"        // mainloop, PHYSFS, datadir, TICKS_PER_SECOND, ticks
#include "mod_tools.hpp"   // Mods::mountedFilepaths, Mods::loadMods
#include "game.hpp"        // loadnextlevel, currentlevel, intro
#include "player.hpp"      // players[]

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* MOD = "TEST";


namespace SAMTest
{
namespace
{
	bool s_active = false;
	std::vector<std::string> s_mods;
	std::string s_class = "barbarian";
	// A fixed default, not a random one: the whole point is that the same command twice
	// walks the same dungeon, so a failure can be looked at again.
	unsigned int s_seed = 0x5A4D0001u;
	bool s_seedGiven = true;
	int s_timeout = 180;
	int s_floor = 0;
	Uint32 s_lastDescend = 0;

	// 0 every check passed, 1 something failed, 2 nothing ever finished, 3 the mods would
	// not load. Starts at 2: a run that dies without reaching done() has not passed, and
	// "the test never finished" is the honest name for that.
	int s_exitCode = 2;
	bool s_finished = false;
	Uint32 s_startMs = 0;
	bool s_started = false;

	// Everything after the '=' of a flag, or null when the flag does not match.
	const char* valueOf(const char* arg, const char* flag)
	{
		const std::size_t n = std::strlen(flag);
		if ( std::strncmp(arg, flag, n) != 0 ) { return nullptr; }
		return arg + n;
	}

	void splitMods(const char* csv)
	{
		std::string cur;
		for ( const char* p = csv; ; ++p )
		{
			if ( *p == ',' || *p == '\0' )
			{
				// Trim: a command line written by hand tends to have spaces after the commas.
				while ( !cur.empty() && (cur.front() == ' ' || cur.front() == '"') ) { cur.erase(cur.begin()); }
				while ( !cur.empty() && (cur.back() == ' ' || cur.back() == '"') ) { cur.pop_back(); }
				if ( !cur.empty() ) { s_mods.push_back(cur); }
				cur.clear();
				if ( *p == '\0' ) { break; }
				continue;
			}
			cur += *p;
		}
	}

	// Take the stairs, for a mod whose checks run on arriving at a floor. game.on_level_entered
	// is fired from the level-change path (game.cpp, inside `if (loadnextlevel)`), and the floor
	// a new game opens on does not go through it -- so a test written around that event has
	// nothing to react to until something descends. This is the same flag /nextlevel sets.
	void driveToFloor()
	{
		if ( s_floor <= 0 || currentlevel >= s_floor ) { return; }
		if ( loadnextlevel || loading || intro ) { return; }
		if ( multiplayer == CLIENT ) { return; }
		if ( !players[0] || !players[0]->entity ) { return; }   // the game has not really begun
		// A second between floors: the load itself runs no ticks, and asking again while one is
		// in flight would skip a floor the mod was meant to see.
		if ( s_lastDescend != 0 && (Uint32)(ticks - s_lastDescend) < TICKS_PER_SECOND ) { return; }
		s_lastDescend = ticks;
		loadnextlevel = true;
		SAM_INFO(MOD, "Test run: taking the stairs down to floor " + std::to_string(currentlevel + 1)
			+ " of " + std::to_string(s_floor) + ".");
	}

	void stop(int code, const std::string& why)
	{
		s_exitCode = code;
		s_finished = true;
		SAM_INFO(MOD, why);
		SAM_INFO(MOD, "Test run finished with exit code " + std::to_string(code) + ".");
		// The same graceful stop the /exit console command uses, so the game still saves its
		// settings and frees its own memory on the way out. A test that leaks or crashes on
		// shutdown should look different from one that does not.
		mainloop = 0;
	}
}

bool parseArg(const char* arg)
{
	if ( !arg ) { return false; }
	if ( const char* v = valueOf(arg, "-samtest=") )
	{
		splitMods(v);
		s_active = true;
		// Everything else has a default, so one flag is enough to run a test.
		return true;
	}
	if ( const char* v = valueOf(arg, "-samtestclass=") )
	{
		if ( *v ) { s_class = v; }
		return true;
	}
	if ( const char* v = valueOf(arg, "-samtestseed=") )
	{
		s_seed = (unsigned int)std::strtoul(v, nullptr, 10);
		s_seedGiven = true;
		return true;
	}
	if ( const char* v = valueOf(arg, "-samtesttimeout=") )
	{
		s_timeout = std::atoi(v);
		return true;
	}
	if ( const char* v = valueOf(arg, "-samtestfloor=") )
	{
		s_floor = std::atoi(v);
		return true;
	}
	return false;
}

void applyToStartup()
{
	if ( !s_active ) { return; }
	// -quickstart is the engine's own "start a game, skip every menu" path, and it is gated
	// on this one string being non-empty. Setting it here is what makes -samtest enough on
	// its own: the run needs no other flag and no menu.
	strncpy(classtoquickstart, s_class.c_str(), sizeof(classtoquickstart) - 1);
	classtoquickstart[sizeof(classtoquickstart) - 1] = 0;
}

bool active() { return s_active; }
unsigned int seed() { return s_seedGiven ? s_seed : 0u; }
const std::string& className() { return s_class; }

bool loadMods()
{
	if ( !s_active ) { return false; }

	SAM_INFO(MOD, "Unattended test run: " + std::to_string((int)s_mods.size()) + " mod(s), class "
		+ s_class + ", seed " + std::to_string(s_seed) + ", timeout "
		+ ( s_timeout > 0 ? std::to_string(s_timeout) + "s" : std::string("none") ) + ".");

	for ( const std::string& folder : s_mods )
	{
		// The same two steps the Mods menu takes for a local mod: mount the folder into the
		// virtual filesystem, then record it so Mods::loadMods finds it. Mounting is the
		// menu's job, not loadMods's, which is why doing only the second half loads nothing.
		std::string full = std::string(datadir) + "/mods/" + folder;
		if ( !PHYSFS_mount(full.c_str(), nullptr, 0) )
		{
			// Try the output directory too: a mod a player installed sits beside their saves
			// rather than beside the game, depending on how the game was installed.
			const std::string alt = std::string(outputdir) + "/mods/" + folder;
			if ( !PHYSFS_mount(alt.c_str(), nullptr, 0) )
			{
				stop(3, "Could not mount mods/" + folder + ". Checked " + full + " and " + alt + ".");
				return false;
			}
			full = alt;
		}
		Mods::mountedFilepaths.push_back(std::make_pair(full, folder));
		SAM_INFO(MOD, "Mounted mods/" + folder + " for this test run.");
	}

	if ( Mods::mountedFilepaths.empty() )
	{
		stop(3, "-samtest named no mods, so there is nothing to test.");
		return false;
	}

	Mods::numCurrentModsLoaded = (int)Mods::mountedFilepaths.size();
	Mods::loadMods();
	// Mods::loadMods() is void and a script that will not parse is only logged, so without this
	// a mod with a syntax error mounts, registers nothing, and is called HUNG by the watchdog
	// three minutes later with advice about --floor 1, which is the wrong fix. "Could not be
	// loaded" is the honest name for it, and 3 is that name's exit code.
	const int failed = samScriptLoadFailures();
	if ( failed > 0 )
	{
		stop(3, std::to_string(failed) + " script(s) failed to load (a parse error, or an error while"
			" running the top level); the LUA/JS lines above say which.");
		return false;
	}
	return true;
}

void tick()
{
	if ( !s_active || s_finished ) { return; }
	if ( !s_started )
	{
		s_started = true;
		s_startMs = SDL_GetTicks();
		return;
	}
	driveToFloor();
	if ( s_timeout <= 0 ) { return; }
	// The watchdog is a wall-clock promise ("nobody at the keyboard: stop it in N real
	// seconds"), so it is measured in real milliseconds, not in `ticks`. A test that runs
	// the game at 0.1x with sam_set_game_speed would otherwise turn 180 seconds into thirty
	// real minutes, and at 8x a hung test would be called after 22. The floor cadence above
	// stays in ticks: it is about the game, not the clock.
	if ( (Uint32)(SDL_GetTicks() - s_startMs) < (Uint32)s_timeout * 1000u ) { return; }
	// A mod that never calls sam_test_done gets stopped here. This is not a fallback for
	// convenience: a test that hangs has failed, and saying so is the difference between a
	// suite that means something and one that quietly reports nothing at all.
	stop(2, "No mod called sam_test_done within " + std::to_string(s_timeout)
		+ " seconds, so the run was stopped. Whatever the mod wrote to the log up to this"
		" point is still there.");
}

bool done(int passed, int failed)
{
	// Outside a test run this does nothing at all, which is what lets a published mod keep
	// the call in without quitting somebody's game.
	if ( !s_active ) { return false; }
	if ( s_finished ) { return true; }
	if ( passed < 0 ) { passed = 0; }
	if ( failed < 0 ) { failed = 0; }
	stop(failed > 0 ? 1 : 0, "The mod reported " + std::to_string(passed) + " passed and "
		+ std::to_string(failed) + " failed.");
	return true;
}

int overrideExitCode(int engineCode)
{
	if ( !s_active ) { return engineCode; }
	// The engine's own non-zero return means it failed to start or crashed on the way out,
	// and that is a worse answer than anything the test found: report it instead.
	if ( engineCode != 0 ) { return engineCode; }
	return s_exitCode;
}
}
