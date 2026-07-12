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

	// --- application into the running game (defined only in the game build) ---
	// Apply attribute/skill deltas to a Stat (called from initClassStats). Does
	// NOT clamp — the caller's unconditional clamp block still runs afterwards.
	static void applyStats(int classnum, Stat* myStats);
	// Grant starting items (called from initClass items chain).
	static void applyLoadout(int player);
	// Grant starting spells (called from initClass spells chain).
	static void applySpells(int player);
};
