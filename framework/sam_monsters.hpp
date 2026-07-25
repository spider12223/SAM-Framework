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

	// Mod-declared traits for a monster VARIANT, looked up by the variant's display name.
	// A custom monster is a variant of a vanilla Monster type, so it shares that type and
	// has no id of its own -- the name is the only thing that identifies it at the moment
	// the engine stamps a variant onto a live Stat. Returns 0 for anything unknown, which
	// is every vanilla monster, so the engine's trait check is a no-op without a mod.
	static unsigned long long traitsForName(const char* variantName);

	// Mod-declared custom BODY model id ("namespace:model") for a monster VARIANT, by the
	// variant's display name, or nullptr. Returns the ID STRING, not an engine model index:
	// models are appended to the engine table AFTER monster JSON is parsed, and an index is
	// load-order dependent while the id is stable. Resolved lazily by SAMBodies at draw time.
	static const char* bodyModelForName(const char* variantName);

	// True if ANY loaded mod declared a monster body. The renderer's fast path checks this
	// so a game with no body-declaring mod never touches a monster's Stat while drawing.
	static bool anyBodyDeclared();

	// Log which declared body models resolved to a real .vox and which did not. A monster's
	// body is the one id a modder hand-writes with no editor, so an unresolvable one used to
	// be completely silent. Called once per load, after models are appended.
	static void reportBodyResolution();

	// One trait NAME ("undead") -> its bit, or 0 if the name isn't a trait. Shared by the
	// JSON parser and the script binding so there is a single table: a name accepted in
	// mod JSON is the same name a script may query, and neither can drift from the other.
	static unsigned long long traitBitForName(const char* name);

	// Stats from the most recent applyAll(), for the load summary.
	static int count();          // monster variant files written
	static int declared();       // monster entries declared across all mods
	static int curveLevels();    // distinct levels a spawn curve was written for
};
