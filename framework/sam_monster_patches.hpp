/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_monster_patches.hpp
	Desc: v0.7.0 Feature 5 — runtime overrides of a monster TYPE's base stats.

	Barony bakes a fresh monster's base stats in setDefaultMonsterStats() (a hardcoded
	switch on the type), which is re-run for every monster construction. So a "patch"
	needs NO vanilla snapshot: we keep a SAM-owned overlay keyed by monster type and
	apply it at the tail of setDefaultMonsterStats. Reverting is just emptying the
	overlay — subsequently spawned monsters get vanilla base again.

	This module is compiled into BOTH the game and the editor build because the apply()
	call site (stat_shared.cpp) lives in both. It is deliberately self-contained (only
	<stat.hpp>/<monster.hpp> types) so it links everywhere; the script-facing setter is
	host-gated in the runtime binding, not here.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

class Stat; // Barony stat.hpp

namespace SAMMonsterPatch
{
	// Override one base-stat field for a monster type (affects FUTURE spawns only).
	// `field` is a stat name: HP/MAXHP/MP/MAXMP/STR/DEX/CON/INT/PER/CHR/LVL and the
	// RANDOM_* spread variants. Returns false if monsterType is out of range
	// (must be a real species: > NOTHING and < NUMMONSTERS).
	bool set(int monsterType, const std::string& field, int value);

	// Drop every override for one type. Returns true if anything was removed.
	bool clearType(int monsterType);

	// Apply the overlay to a freshly built monster Stat (called at the end of
	// setDefaultMonsterStats). No-op unless this Stat's type has overrides.
	void apply(Stat* stats);

	// Drop ALL overrides (revert). Called from SAMLoader load-start + unload.
	void clear();
}
