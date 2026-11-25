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
#include "stat.hpp"        // stats[0]: -samtestrace puts the race on
#include "sam_races.hpp"   // -samtestrace: the race registry
#include "sam_models.hpp"  // idForModelIndex: names a mod's model in the compendium log
#include "files.hpp"       // openDataFile: models.txt names the game's models in that log
#include "sam_sounds.hpp"  // idForEngineIndex: names a race's emote sound in the log
#include "sam_ally_items.hpp" // -samtestprobe=allygive: a player's half of a give to an ally
#include "sam_net.hpp"     // SAMNet::Writer: that half's request, as the host would send it
#include "sam_items.hpp"   // SAMItems::destroyNow: the probe takes its leftovers away again
#include "items.hpp"       // newItem, uidToItem
#include "sam_spells.hpp"  // -samtestprobe=select:<spell>: the spell to put in player 1's hands
#include "magic/magic.hpp" // getSpellFromID
#include <algorithm>       // std::max

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
	// (v3.2.0) -samtestrace: a custom race's "namespace:race", or empty for the class's own.
	std::string s_race;
	// A fixed default, not a random one: the whole point is that the same command twice
	// walks the same dungeon, so a failure can be looked at again.
	unsigned int s_seed = 0x5A4D0001u;
	bool s_seedGiven = true;
	int s_timeout = 180;
	int s_floor = 0;
	Uint32 s_lastDescend = 0;
	// (from 3.14.0) -samtestprobe=<name>: one engine-level check run once the game has begun.
	std::string s_probe;
	bool s_probed = false;

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

	// What the custom races' compendium pages hold, for a mod's log check: where each sits in the two
	// lists, its unlock state, body, text, and every part of its model, named by file (models.txt) so a
	// check can say WHICH part sits in which slot without knowing a single model number.
	void logCompendiumPages()
	{
		using Pages = Compendium_t::CompendiumMonsters_t;
		std::vector<std::string> files;
		if ( const char* dir = PHYSFS_getRealDir("models/models.txt") )
		{
			const std::string path = std::string(dir) + PHYSFS_getDirSeparator() + "models/models.txt";
			if ( File* fp = openDataFile(path.c_str(), "rb") )
			{
				char name[PATH_MAX];
				while ( !fp->eof() )
				{
					fp->gets2(name, PATH_MAX);
					std::string s = name;
					while ( !s.empty() && (s.back() == '\r' || s.back() == '\n') ) { s.pop_back(); }
					const std::size_t slash = s.find_last_of('/');
					files.push_back(slash == std::string::npos ? s : s.substr(slash + 1));
				}
				FileIO::close(fp);
			}
		}
		auto modelName = [&](int sprite) {
			if ( sprite >= 0 && sprite < (int)files.size() ) { return files[sprite]; }
			const std::string id = SAMModels::idForModelIndex(sprite);
			return id.empty() ? std::to_string(sprite) : id;
		};
		auto num = [](real_t v) { char b[16]; snprintf(b, sizeof(b), "%.2f", (double)v); return std::string(b); };

		for ( const char* order : { "default", "alphabetical" } )
		{
			const auto& v = Pages::contents_unfiltered[order];
			int headings = 0, pages = 0;
			std::string where;
			for ( std::size_t i = 0; i < v.size(); ++i )
			{
				if ( v[i].first == "-" && v[i].second.find("CUSTOM RACES") != std::string::npos ) { ++headings; }
				if ( !SAMRaces::isCompendiumPage(v[i].first) ) { continue; }
				++pages;
				where += " '" + v[i].second + "' after '" + (i > 0 ? v[i - 1].second : std::string()) + "';";
			}
			SAM_INFO(MOD, std::string("Compendium ") + order + " list: " + std::to_string(headings) + " custom race heading(s), "
				+ std::to_string(pages) + " race page(s):" + where + " " + std::to_string((int)v.size()) + " entries in all.");
		}
		for ( const auto& entry : Pages::contents_unfiltered["default"] )
		{
			if ( !SAMRaces::isCompendiumPage(entry.first) ) { continue; }
			const auto page = CompendiumEntries.monsters.find(entry.first);
			if ( page == CompendiumEntries.monsters.end() ) { SAM_INFO(MOD, "Compendium page " + entry.first + ": MISSING"); continue; }
			const auto& p = page->second;
			std::string blurb, lines, resist;
			for ( const auto& s : p.blurb ) { blurb += (blurb.empty() ? "" : " / ") + s; }
			for ( const auto& s : p.abilities ) { lines += (lines.empty() ? "" : " | ") + s; }
			for ( int r : p.resistances ) { resist += std::to_string(r) + " "; }
			const auto unlock = Pages::unlocks.find(entry.first);
			SAM_INFO(MOD, "Compendium page " + entry.first + ": unlock "
				+ ( unlock == Pages::unlocks.end() ? std::string("none") : std::to_string((int)unlock->second) )
				+ ", body " + monstertypename[p.monsterType] + ", resist " + resist + "|| blurb: " + blurb
				+ " || lines: " + lines);
			const auto model = CompendiumEntries.compendiumObjectLimbs.find(entry.first);
			if ( model == CompendiumEntries.compendiumObjectLimbs.end() || model->second.entities.empty() )
			{
				SAM_INFO(MOD, "Compendium model " + entry.first + ": none");
				continue;
			}
			const Entity& body = model->second.entities[0];
			std::string parts;
			int i = 0;
			for ( const Entity& e : model->second.entities )
			{
				parts += " " + std::to_string(i++) + ":" + modelName(e.sprite) + "@(" + num(e.x - body.x) + "," + num(e.y - body.y)
					+ "," + num(e.z) + ")f(" + num(e.focalx) + "," + num(e.focaly) + "," + num(e.focalz) + ")s" + num(e.scalex)
					+ "r(" + num(e.yaw - body.yaw) + "," + num(e.pitch) + "," + num(e.roll) + ")";
			}
			SAM_INFO(MOD, "Compendium model " + entry.first + ":" + parts);
		}
	}

	// What the game's taunts call player 1: getMonsterLocalizedName given the player's stats, the call the
	// taunt sites make. In the race's own body as it is now, then with its abilities off, polymorphed,
	// shapeshifted, as another body and as a vanilla race, each put back before the next.
	void logTauntNames()
	{
		Stat& s = *stats[0];
		const Monster host = (Monster)SAMRaces::hostMonsterForRace(s.playerRace);
		const Monster other = host == GOBLIN ? HUMAN : GOBLIN;
		auto said = [&](Monster m) { return "'" + getMonsterLocalizedName(m, &s) + "'"; };
		std::string line = "own body " + said(host);
		const Uint32 appearance = s.stat_appearance;
		s.stat_appearance = 1;
		line += ", abilities off " + said(host);
		s.stat_appearance = appearance;
		for ( const int eff : { EFF_POLYMORPH, EFF_SHAPESHIFT } )
		{
			const Uint8 was = s.getEffectActive(eff);
			s.setEffectValueUnsafe(eff, 1);
			line += (eff == EFF_POLYMORPH ? ", polymorphed " : ", shapeshifted ") + said(host);
			s.setEffectValueUnsafe(eff, was);
		}
		line += std::string(", as a ") + monstertypename[other] + " " + said(other);
		const Sint32 race = s.playerRace;
		s.playerRace = RACE_HUMAN;
		line += ", as the vanilla human race " + said(host);
		s.playerRace = race;
		SAM_INFO(MOD, "Taunt name for player 1: " + line + ".");
	}

	// (from 3.14.0) The player's half of a give to an ally from another machine
	// (SAMAllyItems::onGiveRequest), run on player 0 as if the host had asked, in singleplayer,
	// where the answer goes nowhere. The item must be off the player AT ONCE, before the answer
	// is read: it used to go a tick later, through the destroy queue, and in that tick it could
	// still be dropped or put on and end up in two places. A whole stack, then part of one.
	void probeAllyGive()
	{
		Item* gem = newItem(GEM_RUBY, EXCELLENT, 0, 1, 0, true, &stats[0]->inventory);
		Item* apples = newItem(FOOD_APPLE, EXCELLENT, 0, 5, 0, true, &stats[0]->inventory);
		if ( !gem || !apples ) { SAM_WARN(MOD, "Ally give probe: could not make the items."); return; }
		const Uint32 gemUid = gem->uid, appleUid = apples->uid;
		auto ask = [](Uint32 ticket, Uint32 uid, int count) {
			SAMNet::Writer w;
			w.u32(ticket);
			w.u32(uid);
			w.u16((std::uint16_t)count);
			SAMAllyItems::onGiveRequest(w.buf);
		};
		ask(9001, gemUid, 1);
		const bool gemGone = uidToItem(gemUid) == nullptr;
		ask(9002, appleUid, 2);
		Item* left = uidToItem(appleUid);
		const int applesLeft = left ? (int)left->count : -1;
		if ( left ) { SAMItems::destroyNow(left, 0); }
		SAM_INFO(MOD, std::string("Ally give probe: whole stack ") + (gemGone ? "gone at once" : "still there")
			+ ", part of a stack " + std::to_string(applesLeft) + " of 5 left at once.");
	}

	// (from 3.14.1) -samtestprobe=select:<spell>: teach player 1 a spell and put it in their hands, as
	// picking it would, so a test can press Cast Spell and cast it the real way (no script call selects
	// a spell, and in singleplayer the mana is taken by the casting animation, not by castSpell).
	void probeSelectSpell(const std::string& name)
	{
		const int id = SAMSpells::resolveSpellRef(name);
		spell_t* sp = id >= 0 ? getSpellFromID(id) : nullptr;
		if ( !sp ) { SAM_WARN(MOD, "Select probe: no spell called '" + name + "'."); return; }
		SAMSpells::grantSpell(0, id);
		players[0]->magic.equipSpell(sp);
		SAM_INFO(MOD, "Select probe: player 1 holds " + name + ".");
	}

	// (from 3.13.0) The noise player 1 makes on a Use that hits nothing: SAMRaces::emoteSound, what
	// collision.cpp asks. In the race's own body as it is, then with its abilities off, polymorphed,
	// shapeshifted and in another body, each put back before the next. "none" = the body's own noise,
	// "silent" = no noise at all (the race lists sounds, but the one picked cannot play).
	void logEmoteSounds()
	{
		Stat& s = *stats[0];
		const Monster type = s.type;
		const Monster host = (Monster)SAMRaces::hostMonsterForRace(s.playerRace);
		auto said = [&]() {
			const int slot = SAMRaces::emoteSound(s);
			if ( slot < 0 ) { return std::string("none"); }
			if ( slot == 0 ) { return std::string("silent"); }
			return "'" + SAMSounds::idForEngineIndex(slot) + "'";
		};
		s.type = host;
		std::string line = "own form " + said();
		const Uint32 appearance = s.stat_appearance;
		s.stat_appearance = 1;
		line += ", abilities off " + said();
		s.stat_appearance = appearance;
		for ( const int eff : { EFF_POLYMORPH, EFF_SHAPESHIFT } )
		{
			const Uint8 was = s.getEffectActive(eff);
			s.setEffectValueUnsafe(eff, 1);
			line += (eff == EFF_POLYMORPH ? ", polymorphed " : ", shapeshifted ") + said();
			s.setEffectValueUnsafe(eff, was);
		}
		s.type = host == GOBLIN ? HUMAN : GOBLIN;
		line += ", in another body " + said();
		s.type = type;
		SAM_INFO(MOD, "Emote sound for player 1: " + line + ".");
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
	if ( const char* v = valueOf(arg, "-samtestrace=") )
	{
		s_race = v;
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
	if ( const char* v = valueOf(arg, "-samtestprobe=") )
	{
		s_probe = v;
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

	// -samtestrace. The quickstart built the character (clearStats + initClass) before any mod was
	// loaded, so no custom race existed yet. It is put on now, the two ways a character made at
	// the menu gets one: its stat deltas (the tail of initClassStats) and its innate spells (the
	// isLocalPlayer branch of initClass). Building the character again instead would hand out the
	// class's starting gear twice, because clearStats leaves the inventory alone.
	if ( !s_race.empty() )
	{
		const int id = SAMRaces::raceIdForIdString(s_race);
		if ( id < 0 || !stats[0] )
		{
			stop(3, "-samtestrace=" + s_race + ": no mod in this run registered that race. It is the"
				" race's \"id\" from its JSON, \"namespace:race\".");
			return false;
		}
		stats[0]->playerRace = id;
		stats[0]->stat_appearance = 0;   // 0 keeps the race's abilities on, as the menu's default does
		SAMRaces::applyStats(id, stats[0]);
		stats[0]->HP = std::max(1, stats[0]->HP);
		stats[0]->MAXHP = std::max(1, stats[0]->MAXHP);
		stats[0]->MP = std::max(1, stats[0]->MP);
		stats[0]->MAXMP = std::max(1, stats[0]->MAXMP);
		stats[0]->OLDHP = stats[0]->HP;
		SAMRaces::applySpells(0);
		SAM_INFO(MOD, "Playing as race " + s_race + " (id " + std::to_string(id) + ").");
		logTauntNames();
		logEmoteSounds();
	}

	// A custom race's compendium page is built when the compendium opens, which a run with nobody at
	// the keyboard never does. Build the pages here instead: once as a player with the arachnophobia
	// filter on (its own log lines name the pose each page used), then twice without it (the second
	// call must replace the first, not add to it), and write down what they hold for a mod's log
	// check. The Denizens completion figure is taken on both sides: the pages must not move it.
	if ( SAMRaces::any() )
	{
		Compendium_t::updateLorePointCounts();
		const int before = Compendium_t::CompendiumMonsters_t::completionPercent;
		SAM_INFO(MOD, "Compendium pages, built as with the arachnophobia filter on:");
		SAMRaces::addCompendiumPages(true);
		SAM_INFO(MOD, "Compendium pages, built twice with the filter off:");
		SAMRaces::addCompendiumPages(false);
		SAMRaces::addCompendiumPages(false);
		Compendium_t::updateLorePointCounts();
		SAM_INFO(MOD, "Compendium pages built for this run: Denizens " + std::to_string(before) + "% complete before them, "
			+ std::to_string(Compendium_t::CompendiumMonsters_t::completionPercent) + "% after.");
		logCompendiumPages();
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
	if ( !s_probed && !s_probe.empty() && !intro && !loading && players[0] && players[0]->entity && stats[0] )
	{
		s_probed = true;
		if ( s_probe == "allygive" ) { probeAllyGive(); }
		else if ( s_probe.rfind("select:", 0) == 0 ) { probeSelectSpell(s_probe.substr(7)); }
		else { SAM_WARN(MOD, "No test probe is called '" + s_probe + "'."); }
	}
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
