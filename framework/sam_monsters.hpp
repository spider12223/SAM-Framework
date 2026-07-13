/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_monsters.hpp
	Desc: custom monster/enemy support.

	Barony has a FIXED set of creature types (enum Monster, 48 usable species) —
	sprite, model, limbs, and AI are hard-coded, so brand-new types can't be added
	at runtime. What Barony DOES support is per-file VARIANTS of an existing type:
	a data/custom-monsters/<name>.json that overrides that base type's stats,
	equipment, inventory, name, proficiencies, and behaviour flags. Those files are
	read lazily (at spawn time) by MonsterStatCustomManager, and a monster variant
	is only ever loaded if something references its filename — either another
	monster's follower list, a map-editor placement, or the level spawn table in
	data/monstercurve.json (which vanilla does not ship).

	SAMMonsters bridges a friendly, namespaced monster JSON (monster.schema.json)
	to that raw system. For every monster declared by a mod it:
	  * validates base_type / skills / slots / item names (turning Barony's silent
	    "unknown -> nothing" degradation into an actionable load error),
	  * translates to Barony's raw custom-monster key tree — always emitting the
	    five sections MonsterStatCustomManager reads UNCONDITIONALLY (stats,
	    misc_stats, proficiencies, equipped_items, inventory_items) so a sparse mod
	    file can never crash the reader,
	  * namespaces the on-disk filename (<ns>_<slug>.json) and rewrites
	    "namespace:monster" follower references to match,
	  * merges every monster's `spawn` block into a single data/monstercurve.json,
	and writes it all into a private prepend-mounted overlay (mirroring sam_patcher)
	so Barony's own lazy reads pick it up. Nothing vanilla is modified; the overlay
	is wiped on every load/unload.

	Game build only (needs PhysFS mounting, Barony's outputdir, and the
	monstertypename[] / ItemTooltips tables). The call in sam_loader.cpp is
	#ifndef EDITOR-guarded and this .cpp is in GAME_SOURCES only.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

struct SAMModManifest;  // from sam_workshop.hpp (full type only needed in the .cpp)

// Nominal id base for logging/count symmetry with classes (5000) and items
// (5000). The engine keys custom monsters by filename, not a numeric id — this is
// only used to give each registered monster a stable ordinal in the log.
static const int SAM_MONSTER_ID_BASE = 9000;

class SAMMonsters
{
public:
	// Read + validate + translate every monster JSON declared by every mod (in
	// dependency load order), write the raw custom-monster files and a merged
	// monstercurve.json into the overlay, and prepend-mount it. Fully self-
	// cleaning: unmounts + wipes any previous overlay first. Call from
	// SAMLoader::load() alongside SAMPatcher::applyAll().
	static void applyAll(const std::vector<SAMModManifest>& mods);

	// Unmount + delete the overlay folder (call from SAMLoader::unload()).
	static void clear();

	// Stats from the most recent applyAll(), for the load summary.
	static int count();          // monster variant files written
	static int declared();       // monster entries declared across all mods
	static int curveLevels();    // distinct levels a spawn curve was written for
};
