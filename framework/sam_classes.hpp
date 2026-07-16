/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_classes.hpp
	Desc: runtime custom character-class registry + application.

	Custom classes are stored under integer ids starting at SAM_CLASS_ID_BASE
	(1000). Barony's client_classes[] is a Sint32 array, so it can already hold
	these ids without any engine change; NUMCLASSES (26) only bounds the
	character-select UI carousel, which is patched separately.

	The registry/parsing half (loadFromManifest/clear/getClass/count) is fully
	decoupled from Barony. The application half (applyStats/applyLoadout/
	applySpells) touches Barony internals and is compiled only into the game
	(not the editor).

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>
#include <map>

struct SAMModManifest;  // from sam_workshop.hpp (full type only needed in the .cpp)
class Stat;             // Barony stat.hpp — forward declared so this header stays light

// Custom class ids occupy [1000, ...). Chosen well above NUMCLASSES (26).
static const int SAM_CLASS_ID_BASE = 1000;

// One entry in a class's starting_items array (mirrors class.schema.json).
struct SAMStartingItem
{
	std::string type;       // vanilla ItemType name ("IRON_AXE") or "namespace:item"
	std::string status = "SERVICABLE";
	int beatitude = 0;
	int count = 1;
	int appearance = 0;
	bool identified = true;
	bool equip = false;     // auto-equip on start?
	int hotbarSlot = -1;    // 0-9, or -1 for none
};

// One parsed class JSON (mirrors class.schema.json).
struct SAMClassDef
{
	std::string id;             // "namespace:class"
	int numericId = 0;          // assigned runtime id (>= SAM_CLASS_ID_BASE)
	std::string modNamespace;   // owning mod namespace
	std::string name;
	std::string description;

	// Attribute / HP / MP deltas (added on top of the race base). intel == INT.
	int str = 0, dex = 0, con = 0, intel = 0, per = 0, chr = 0, hp = 0, mp = 0;

	// Per-level growth row — Barony's ClassBaseGrowths (HP, MP, HP regen, MP regen).
	// The engine's table is indexed by class id, so custom ids fell out of bounds and
	// every custom class silently shared the engine's "default" row {3,3,3,3}. These
	// default to that same row, so omitting the JSON "growth" block leaves an existing
	// mod's behaviour byte-for-byte unchanged; setting it finally makes it tunable.
	int growthHP = 3, growthMP = 3, growthRegenHP = 3, growthRegenMP = 3;

	// Optional "mp_regen" block, applied on top of Barony's computed mana-regen rate
	// (in MP per minute; the engine then converts it to ticks-between-MP).
	//   base         — flat add
	//   statScaling  — stat name ("INT") -> MP/min per point. NOTE: the engine's own
	//                  table pins INT at 0.0 and skips any stat scored 0.0, so vanilla
	//                  INT contributes NOTHING to mana regen. This is how a class makes
	//                  INT (or any stat) actually drive regen.
	//   multiplier   — final scale, applied last
	// hasMpRegen stays false when the block is absent, so the vanilla result is untouched.
	bool hasMpRegen = false;
	double mpRegenBase = 0.0;
	double mpRegenMultiplier = 1.0;
	std::map<std::string, double> mpRegenStatScaling;

	// Optional "appearance" block — a look forced on every player of this class.
	//   appearanceHeads : race name -> head model id ("ns:model", or a vanilla head
	//                     name). The key "default" covers any race with no entry.
	//   appearanceHeadIdx : the same map resolved to engine model indices by
	//                     resolveAppearance(). -1 means unresolved/unknown.
	//   surviveShapeshift : keep the look while polymorphed/shapeshifted. Default false —
	//                     a class look that survives polymorph reads as a bug.
	// Only the HEAD is covered: it is the one limb the engine assigns unconditionally
	// every frame. Torso/arms/legs are only assigned when that armour slot is EMPTY, so
	// a forced body look would vanish the moment the player equips anything.
	std::map<std::string, std::string> appearanceHeads;
	std::map<std::string, int> appearanceHeadIdx;
	bool surviveShapeshift = false;

	std::map<std::string, int> skills;          // "PRO_X" -> 0..100
	std::vector<SAMStartingItem> startingItems;
	std::vector<std::string> startingSpells;    // "SPELL_X"
	std::vector<std::string> strongRolls;       // stat names ("STR"...)
	std::vector<std::string> weakRolls;         // stat names
	int gold = 0;

	// Optional custom class-select portrait (carousel icon).
	//   portrait      = path exactly as written in the JSON (relative to the mod
	//                   folder), kept for round-tripping / diagnostics.
	//   portraitPath  = resolved ABSOLUTE file path, set at load time ONLY if the
	//                   file actually exists on disk; empty otherwise, in which
	//                   case the carousel falls back to the placeholder icon.
	// The carousel loads it lazily by path through Barony's UI Image system
	// (Image::get -> IMG_Load), so no entry in the sprites[] array is needed —
	// UI icons are path-based, not sprite-index-based.
	std::string portrait;
	std::string portraitPath;
};

// v0.7.0 Feature 5: a runtime override of a class's STARTING stats, applied at the
// end of initClassStats (absolute values, not deltas). Only the keys present are
// overridden. `stats` keys are stat names (STR/DEX/CON/INT/PER/CHR/HP/MAXHP/MP/
// MAXMP/GOLD/LVL/EXP); `skills` keys are "PRO_X" proficiency names (0..100).
struct SAMClassStatPatch
{
	std::map<std::string, int> stats;
	std::map<std::string, int> skills;
};

class SAMClasses
{
public:
	// Read + register every class JSON declared in a mod manifest. Additive
	// across manifests within one load cycle; call clear() first each cycle.
	static void loadFromManifest(const SAMModManifest& manifest);

	// Wipe the registry and reset the id counter to SAM_CLASS_ID_BASE.
	static void clear();

	// Look up a registered class by its runtime id (>= 1000). null if none.
	static const SAMClassDef* getClass(int classId);

	// Number of custom classes currently registered.
	static int count();

	// Enumerate registered classes by carousel index (0..count()-1), in
	// ascending runtime-id order. Returns the runtime class id, or -1 if out of
	// range. Used by the character-select carousel to append custom classes.
	static int classIdAtIndex(int index);

	// Reverse lookup: runtime class id for a "namespace:class" id string, or -1.
	// Used to recover the class id from a carousel button's name.
	static int classIdForIdString(const std::string& idString);

	// --- v0.7.0 Feature 5: modify existing content (revert on unload) ----------
	// Override a class's STARTING stats. classnum is a vanilla id (0..NUMCLASSES-1)
	// or a registered custom id (>= SAM_CLASS_ID_BASE). Absolute values; affects only
	// characters created AFTER the call. Returns false if a custom id is unregistered.
	// Range-validation of vanilla ids is the caller's job (done in the script binding).
	static bool patchClass(int classnum, const SAMClassStatPatch& patch);
	// Drop a single class's stat patch (the blanket clear() on unload also covers it).
	static void unpatchClass(int classnum);
	// Grant / remove a permanent status effect (EFF_* id) applied to characters of a
	// class at creation. Returns false for an unregistered custom class / bad effect.
	static bool addClassPassive(int classnum, int effectId);
	static bool removeClassPassive(int classnum, int effectId);

	// --- application into the running game (defined only in the game build) ---
	// Apply attribute/skill deltas to a Stat (called from initClassStats). Does
	// NOT clamp — the caller's unconditional clamp block still runs afterwards.
	static void applyStats(int classnum, Stat* myStats);
	// v0.7.0 F5: apply any sam_patch_class stat overrides (absolute) + sam_add_class_passive
	// effects. Called from initClassStats after all deltas, before the HP/MP clamp.
	static void applyStatOverrides(int classnum, Stat* myStats);

	// Apply a custom class's "mp_regen" block to the engine's computed mana-regen rate.
	// `statValues` is indexed in Barony's STAT_* order (STR, DEX, CON, INT, PER, CHR) and
	// holds the player's already-resolved stat totals; passing them in keeps this side of
	// the framework free of Barony's stat internals. No-op for a vanilla class, an
	// unregistered id, or a class without an mp_regen block.
	static void applyManaRegen(int classnum, const int* statValues, int numStats, double& regenPerMinute);

	// ---- per-class appearance -------------------------------------------------
	// Resolve every class's appearance block against the model table. Call once after
	// models are registered (custom heads must exist before we can index them).
	static void resolveAppearance();

	// The head model index this class forces for `playerRace`, or -1 to leave the
	// player's own head alone. Resolution is races[race] -> races["default"] -> -1, so a
	// class never imposes a look on a race its author didn't write for — that's what
	// keeps custom classes playable on every race.
	static int headSpriteFor(int classnum, int playerRace);

	// Is `sprite` a head this framework registered? Barony's Entity::isPlayerHeadSprite
	// is a hardcoded switch over vanilla indices, and a custom head answers false there
	// — which breaks the client's player-entity binding. This backs the patch widening it.
	static bool isCustomHeadSprite(int sprite);
	static void applyPassives(int classnum, Stat* myStats);
	// Grant starting items (called from initClass items chain).
	static void applyLoadout(int player);
	// Grant starting spells (called from initClass spells chain).
	static void applySpells(int player);
};
