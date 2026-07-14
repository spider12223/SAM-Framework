/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_monster_patches.cpp
	Desc: implementation of v0.7.0 Feature 5 monster base-stat overrides.

	Compiled into both the game and the editor build (the apply() call in
	stat_shared.cpp exists in both). Self-contained: only touches Stat + the Monster
	enum, no networking / PhysFS / logging, so it links in either target. The overlay
	is only ever populated in the game build (the script setter is host-gated there);
	in the editor build it stays empty and apply() is a no-op.

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_monster_patches.hpp"

#include "main.hpp"     // establishes the engine context stat.hpp needs (FMOD/windows.h)
#include "game.hpp"
#include "stat.hpp"     // Stat, MAX_PLAYER_STAT_VALUE, RANDOM_* fields
#include "monster.hpp"  // Monster enum, NUMMONSTERS, NOTHING

#include <map>

namespace
{
	// type -> { field name -> absolute value }. A map (not a fixed array) keeps the
	// storage sparse; apply() is called only at monster construction, never per-frame.
	std::map<int, std::map<std::string, int>> g_overrides;

	inline int clampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

namespace SAMMonsterPatch
{
	bool set(int monsterType, const std::string& field, int value)
	{
		if ( monsterType <= NOTHING || monsterType >= NUMMONSTERS ) { return false; }
		g_overrides[monsterType][field] = value;
		return true;
	}

	bool clearType(int monsterType)
	{
		return g_overrides.erase(monsterType) > 0;
	}

	void apply(Stat* stats)
	{
		if ( !stats ) { return; }
		auto it = g_overrides.find(static_cast<int>(stats->type));
		if ( it == g_overrides.end() ) { return; }

		bool setMaxHP = false, setHP = false;
		for ( const auto& kv : it->second )
		{
			const std::string& f = kv.first;
			const int v = kv.second;
			if      ( f == "HP" )    { stats->HP = v; setHP = true; }
			else if ( f == "MAXHP" ) { stats->MAXHP = (v < 1 ? 1 : v); setMaxHP = true; }
			else if ( f == "MP" )    { stats->MP = v; }
			else if ( f == "MAXMP" ) { stats->MAXMP = (v < 0 ? 0 : v); }
			else if ( f == "STR" )   { stats->STR = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
			else if ( f == "DEX" )   { stats->DEX = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
			else if ( f == "CON" )   { stats->CON = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
			else if ( f == "INT" )   { stats->INT = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
			else if ( f == "PER" )   { stats->PER = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
			else if ( f == "CHR" )   { stats->CHR = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
			else if ( f == "LVL" || f == "LEVEL" ) { stats->LVL = clampI(v, 1, 255); }
			else if ( f == "RANDOM_HP" )    { stats->RANDOM_HP = v; }
			else if ( f == "RANDOM_MAXHP" ) { stats->RANDOM_MAXHP = v; }
			else if ( f == "RANDOM_MP" )    { stats->RANDOM_MP = v; }
			else if ( f == "RANDOM_MAXMP" ) { stats->RANDOM_MAXMP = v; }
			else if ( f == "RANDOM_STR" )   { stats->RANDOM_STR = v; }
			else if ( f == "RANDOM_DEX" )   { stats->RANDOM_DEX = v; }
			else if ( f == "RANDOM_CON" )   { stats->RANDOM_CON = v; }
			else if ( f == "RANDOM_INT" )   { stats->RANDOM_INT = v; }
			else if ( f == "RANDOM_PER" )   { stats->RANDOM_PER = v; }
			else if ( f == "RANDOM_CHR" )   { stats->RANDOM_CHR = v; }
			else if ( f == "RANDOM_LVL" )   { stats->RANDOM_LVL = v; }
			// Unknown keys are silently ignored (the runtime binding validates + warns).
		}
		// Vanilla coupling: if MAXHP was set without HP, pin HP to it; always resync OLDHP.
		if ( setMaxHP && !setHP ) { stats->HP = stats->MAXHP; }
		stats->OLDHP = stats->HP;
	}

	void clear()
	{
		g_overrides.clear();
	}
}
