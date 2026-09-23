/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_test.hpp
	Desc: run a mod's own tests with nobody at the keyboard.

	WHY THIS EXISTS

	A test mod writes what it found to sam_log.txt, which is the right place for it, but
	getting there meant a person launching Barony, opening the Mods menu, loading the mod,
	starting a game, waiting, and quitting. That is fine once. It is not fine as the thing
	standing between a change and knowing whether the change broke anything, so in practice
	the tests were run rarely and late.

	With -samtest the game does all of it by itself:

	    barony.exe -samtest=RulesTest -windowed -size=640x480

	mounts mods/RulesTest, loads it, starts a singleplayer game on a fixed seed, lets the
	mod run, and exits. The exit CODE is the answer: 0 every check passed, 1 something
	failed, 2 the mod never finished and the watchdog stopped it, 3 the mods could not be
	loaded at all. A script can run that and read the number.

	HOW A MOD ENDS THE RUN

	    sam_test_done(passed, failed)

	The mod calls it when its last check is in. Anything else -- a script error, an infinite
	loop, a mod that simply never gets there -- is caught by the watchdog, which is the
	honest default: a test that hangs has failed, it has not passed.

	THE IRON RULE. Without -samtest on the command line every function here returns false or
	zero and nothing in the engine behaves differently. active() is one bool.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

namespace SAMTest
{
	// ---- the command line -------------------------------------------------------------
	//
	// Parsed in game.cpp's argument loop, before anything else has started.
	//   -samtest=<mod>[,<mod>...]   the mod FOLDER names under mods/, in load order
	//   -samtestclass=<class>       the class to start as (default "barbarian")
	//   -samtestseed=<n>            the dungeon seed (default fixed, so runs repeat)
	//   -samtesttimeout=<seconds>   the watchdog (default 180, 0 disables it)
	//   -samtestfloor=<n>           descend to this floor before leaving the mod to it
	//                               (default 0, stay where the game starts)
	bool parseArg(const char* arg);

	// Hand the test's class to the engine's own quickstart, which is the path that starts a
	// game with no menus. Called once after the whole argument loop rather than from
	// parseArg, because -samtestclass may be written after -samtest on the command line.
	void applyToStartup();

	// Whether -samtest was given. Every other entry point here is a no-op when this is false.
	bool active();

	// ---- starting the game ------------------------------------------------------------
	//
	// Mount every named mod and load it, the way the Mods menu would. Called from game.cpp
	// immediately before the quickstart path calls doNewGame, because the menu that normally
	// does this work is exactly what is being skipped. False means a mod folder was missing
	// or would not mount: the run is over and the exit code is already set.
	bool loadMods();

	// The seed a test run uses, so the same command twice gives the same dungeon. Zero when
	// no seed was given and the caller should keep the engine's own time-based one.
	unsigned int seed();

	// The class name -samtestclass asked for, for the quickstart matcher.
	const std::string& className();

	// ---- during the run ---------------------------------------------------------------
	//
	// Called once per frame from the main loop. Runs the watchdog and nothing else.
	void tick();

	// A mod says its run is over. Writes the verdict to the log, sets the exit code from
	// `failed`, and stops the main loop. Returns false (and does nothing) outside test mode,
	// which is what makes sam_test_done safe to leave in a published mod.
	bool done(int passed, int failed);

	// ---- the answer -------------------------------------------------------------------
	//
	// Wraps the value main() was going to return. Outside test mode it hands `engineCode`
	// straight back, so a normal run still reports whatever the engine reported.
	int overrideExitCode(int engineCode);
}
