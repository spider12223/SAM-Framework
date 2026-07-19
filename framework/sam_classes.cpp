/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_classes.cpp
	Desc: implementation of the custom class registry + application.

	Two halves:
	  * Registry/parsing (loadFromManifest/clear/getClass/count) — decoupled
	    from Barony; compiled into both the game and the editor.
	  * Application (applyStats/applyLoadout/applySpells) — touches Barony
	    internals; compiled only into the game (#ifndef EDITOR).

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros that
// would break nlohmann/std. Must come before any include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_classes.hpp"
#include "sam_workshop.hpp"
#include "sam_items.hpp"     // SAMItems::itemIdForIdString (custom-item validation)
#include "sam_models.hpp"    // SAMModels::modelIndexForId (resolve a class appearance head)
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include <fstream>
#include <sstream>
#include <set>

#ifndef EDITOR
#include "main.hpp"          // MAXPLAYERS, CLIENT, multiplayer
#include "game.hpp"          // client_classes, intro
#include "stat.hpp"          // Stat, PRO_* constants
#include "items.hpp"         // ItemType, Status, newItem, itemPickup, useItem
#include "player.hpp"        // players[], hotbar, NUM_HOTBAR_SLOTS
#include "net.hpp"
#include "mod_tools.hpp"     // ItemTooltips (itemNameStringToItemID, spellItems)
#include "magic/magic.hpp"   // addSpell
#include "sam_spells.hpp"    // SAMSpells::getSpellByName (custom starting spells)
#include <cctype>
#include <cstdlib>           // free
#endif

using nlohmann::json;

static const char* MOD = "CLASSES";

/*-------------------------------------------------------------------------------
	Registry storage (game-decoupled)
-------------------------------------------------------------------------------*/
static std::map<int, SAMClassDef> s_registry;
static int s_nextClassId = SAM_CLASS_ID_BASE;

// v0.7.0 Feature 5 overlays (game-decoupled storage — reverted in clear()). Keyed by
// classnum so vanilla (0..25) and custom (>=1000) share one path.
static std::map<int, SAMClassStatPatch> s_classPatches;
static std::map<int, std::set<int>> s_classPassives;

// Hotbar pins a class asked for (player -> [{slot, item uid}]), recorded as applyLoadout
// grants them. Barony's own ClassHotbarConfig_t::assignHotbarSlots() runs a few frames
// LATER and zeroes every hotbar slot, so the pins have to be re-applied after it — see
// reapplyHotbarPins(), called at the end of assignHotbarSlots.
static std::map<int, std::vector<std::pair<int, Uint32>>> s_hotbarPins;

static bool readWholeFile(const std::string& path, std::string& out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if ( !f.is_open() )
	{
		return false;
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

static std::string joinPath(const std::string& dir, const std::string& file)
{
	if ( dir.empty() )
	{
		return file;
	}
	const char back = dir.back();
	return (back == '/' || back == '\\') ? (dir + file) : (dir + "/" + file);
}

// Normalize backslashes to forward slashes. SDL_image's IMG_Load accepts '/' on
// every platform; keeping one separator style also makes log lines readable and
// avoids any confusion when the path is later handed to the UI Image loader.
static std::string toForwardSlashes(std::string s)
{
	for ( char& c : s )
	{
		if ( c == '\\' )
		{
			c = '/';
		}
	}
	return s;
}

static bool fileExists(const std::string& path)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	return f.is_open();
}

/*-------------------------------------------------------------------------------
	SAMClasses::loadFromManifest
-------------------------------------------------------------------------------*/
#ifndef EDITOR
// Game-only: validate a parsed class's enum-name references (item types, skills,
// spells, statuses, stat rolls) against the real game data, emitting rich
// "did you mean?" diagnostics. Defined below, near the other game helpers.
static void validateClassSemantics(const SAMClassDef& def, const std::string& fileLabel);
#endif

void SAMClasses::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const std::string& relPath : manifest.classes )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		const std::string fileLabel = SAMErrors::displayFile(manifest.ns, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "Class file not found: " + path + " (declared by [" + manifest.ns + "]) — class not loaded.");
			continue;
		}

		json j;
		try
		{
			j = json::parse(text);
		}
		catch ( const json::parse_error& e )
		{
			SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "class not loaded.");
			continue;
		}
		if ( !j.is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
				"a JSON object: { ... }", "wrap the file contents in { }", "class not loaded.");
			continue;
		}

		// Small typed accessors that never throw.
		auto getStr = [&](const json& o, const char* k) -> std::string {
			auto it = o.find(k);
			return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
		};
		auto getInt = [&](const json& o, const char* k, int dv) -> int {
			auto it = o.find(k);
			return (it != o.end() && it->is_number()) ? it->get<int>() : dv;
		};
		auto getBool = [&](const json& o, const char* k, bool dv) -> bool {
			auto it = o.find(k);
			return (it != o.end() && it->is_boolean()) ? it->get<bool>() : dv;
		};
		auto getReal = [&](const json& o, const char* k, double dv) -> double {
			auto it = o.find(k);
			return (it != o.end() && it->is_number()) ? it->get<double>() : dv;
		};

		SAMClassDef def;
		def.id = getStr(j, "id");
		def.name = getStr(j, "name");
		def.description = getStr(j, "description");
		def.modNamespace = manifest.ns;

		if ( def.id.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/id", "", "missing (required)",
				"an id in \"namespace:class\" form, e.g. \"" + manifest.ns + ":assassin\"",
				"add an \"id\" field", "class not loaded.");
			continue;
		}
		if ( def.name.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/name", "", "missing (required)",
				"the class's display name, e.g. \"Assassin\"", "add a \"name\" field",
				"class [" + def.id + "] not loaded.");
			continue;
		}

		// Optional custom portrait: resolve to an absolute file path and confirm it
		// exists NOW (load time). If it's missing we log a warning and leave
		// portraitPath empty so the carousel uses the placeholder — a bad portrait
		// must never block the class or crash. The actual image decode happens
		// lazily in the UI (Image::get); a corrupt-but-present file simply draws
		// no icon there, which is likewise non-fatal.
		def.portrait = getStr(j, "portrait");
		// Path-traversal guard: def.portrait is untrusted mod JSON joined onto
		// modPath and later handed to the UI image loader. Drop it if it escapes.
		if ( !def.portrait.empty() && SAMErrors::relPathEscapes(def.portrait) )
		{
			SAM_WARN(MOD, "Class [" + def.id + "] portrait path '" + def.portrait + "' escapes the mod folder — ignoring it.");
			def.portrait.clear();
		}
		if ( !def.portrait.empty() )
		{
			const std::string abs = toForwardSlashes(joinPath(manifest.modPath, def.portrait));
			if ( fileExists(abs) )
			{
				def.portraitPath = abs;
				SAM_INFO(MOD, "Class [" + def.id + "] portrait -> " + abs);
			}
			else
			{
				SAM_WARN(MOD, "Class [" + def.id + "] portrait not found (using placeholder): " + abs);
			}
		}

		if ( j.contains("stats") && j["stats"].is_object() )
		{
			const json& s = j["stats"];
			def.str = getInt(s, "STR", 0);
			def.dex = getInt(s, "DEX", 0);
			def.con = getInt(s, "CON", 0);
			def.intel = getInt(s, "INT", 0);
			def.per = getInt(s, "PER", 0);
			def.chr = getInt(s, "CHR", 0);
			def.hp = getInt(s, "HP", 0);
			def.mp = getInt(s, "MP", 0);
		}

		// Per-level HP/MP growth + regen (Barony's ClassBaseGrowths row). Named
		// "hp_mp_growth" to stay clearly distinct from "stat_growth" above, which is a
		// different thing entirely (which ATTRIBUTES roll high/low on level-up).
		// Defaults are the engine's "default" row, which is what custom classes silently
		// used before, so omitting this block is a no-op for existing mods.
		if ( j.contains("hp_mp_growth") && j["hp_mp_growth"].is_object() )
		{
			const json& g = j["hp_mp_growth"];
			def.growthHP      = getInt(g, "HP", 3);
			def.growthMP      = getInt(g, "MP", 3);
			def.growthRegenHP = getInt(g, "HP_REGEN", 3);
			def.growthRegenMP = getInt(g, "MP_REGEN", 3);
		}

		// Optional per-class look. Only the head is supported — see SAMClassDef for why
		// the body limbs can't be forced (they're armour-gated).
		if ( j.contains("appearance") && j["appearance"].is_object() )
		{
			const json& a = j["appearance"];
			def.surviveShapeshift = getBool(a, "survive_shapeshift", false);
			if ( a.contains("races") && a["races"].is_object() )
			{
				for ( auto it = a["races"].begin(); it != a["races"].end(); ++it )
				{
					if ( !it.value().is_object() ) { continue; }
					const json& entry = it.value();
					const std::string head = getStr(entry, "head");
					if ( head.empty() ) { continue; }
					def.appearanceHeads[it.key()] = head;
				}
			}
			// Optional whole-body model (e.g. a jet): one custom .vox forced as the entire
			// body, race-independent. Resolved to an engine index in resolveAppearance().
			def.bodyModel = getStr(a, "body_model");
		}

		// Optional mana-regen tuning, applied on top of the engine's computed rate.
		// Unknown stat names are warned about and ignored rather than failing the load.
		if ( j.contains("mp_regen") && j["mp_regen"].is_object() )
		{
			const json& r = j["mp_regen"];
			def.hasMpRegen = true;
			def.mpRegenBase = getReal(r, "base", 0.0);
			def.mpRegenMultiplier = getReal(r, "multiplier", 1.0);
			if ( r.contains("stat_scaling") && r["stat_scaling"].is_object() )
			{
				static const std::set<std::string> kStatNames = { "STR", "DEX", "CON", "INT", "PER", "CHR" };
				for ( auto it = r["stat_scaling"].begin(); it != r["stat_scaling"].end(); ++it )
				{
					if ( !it.value().is_number() ) { continue; }
					if ( kStatNames.find(it.key()) == kStatNames.end() )
					{
						SAM_WARN(MOD, "Class [" + def.id + "] mp_regen.stat_scaling has unknown stat '"
							+ it.key() + "' (expected STR/DEX/CON/INT/PER/CHR) — ignored.");
						continue;
					}
					def.mpRegenStatScaling[it.key()] = it.value().get<double>();
				}
			}
		}

		if ( j.contains("skills") && j["skills"].is_object() )
		{
			for ( auto it = j["skills"].begin(); it != j["skills"].end(); ++it )
			{
				if ( it.value().is_number() )
				{
					def.skills[it.key()] = it.value().get<int>();
				}
			}
		}

		if ( j.contains("starting_items") && j["starting_items"].is_array() )
		{
			for ( const json& e : j["starting_items"] )
			{
				if ( !e.is_object() )
				{
					continue;
				}
				SAMStartingItem si;
				si.type = getStr(e, "type");
				if ( si.type.empty() )
				{
					continue;
				}
				si.status = getStr(e, "status");
				if ( si.status.empty() ) { si.status = "SERVICABLE"; }
				si.beatitude = getInt(e, "beatitude", 0);
				si.count = getInt(e, "count", 1);
				si.appearance = getInt(e, "appearance", 0);
				si.identified = getBool(e, "identified", true);
				si.equip = getBool(e, "equip", false);
				si.hotbarSlot = getInt(e, "hotbar_slot", -1);
				def.startingItems.push_back(si);
			}
		}

		if ( j.contains("starting_spells") && j["starting_spells"].is_array() )
		{
			for ( const json& e : j["starting_spells"] )
			{
				if ( e.is_string() ) { def.startingSpells.push_back(e.get<std::string>()); }
			}
		}

		if ( j.contains("stat_growth") && j["stat_growth"].is_object() )
		{
			const json& g = j["stat_growth"];
			if ( g.contains("strong_rolls") && g["strong_rolls"].is_array() )
			{
				for ( const json& e : g["strong_rolls"] )
				{
					if ( e.is_string() ) { def.strongRolls.push_back(e.get<std::string>()); }
				}
			}
			if ( g.contains("weak_rolls") && g["weak_rolls"].is_array() )
			{
				for ( const json& e : g["weak_rolls"] )
				{
					if ( e.is_string() ) { def.weakRolls.push_back(e.get<std::string>()); }
				}
			}
			// Explicit per-attribute growth weights: { "STR": 6, "DEX": 1, ... }. Higher =
			// more likely to be one of the 3 stats raised on level-up. Clamped to 0..99
			// (0 = never). Overrides strong/weak for any attribute it names.
			if ( g.contains("weights") && g["weights"].is_object() )
			{
				for ( auto it = g["weights"].begin(); it != g["weights"].end(); ++it )
				{
					if ( it.value().is_number_integer() )
					{
						int w = it.value().get<int>();
						if ( w < 0 ) { w = 0; }
						if ( w > 99 ) { w = 99; }
						def.statGrowthWeights[it.key()] = w;
					}
				}
			}
		}

		def.gold = getInt(j, "gold", 0);

#ifndef EDITOR
		// Load-time validation of enum-name references, with "did you mean?" hints.
		// Warns (never blocks) — a bad reference just isn't applied at play time.
		validateClassSemantics(def, fileLabel);
#endif

		def.numericId = s_nextClassId++;
		s_registry[def.numericId] = def;

		SAM_INFO(MOD, "Registering class: " + def.name + " [" + def.id + "] -> runtime id "
			+ std::to_string(def.numericId));
		SAM_DEBUG(MOD, "  stats: STR " + std::to_string(def.str) + " DEX " + std::to_string(def.dex)
			+ " CON " + std::to_string(def.con) + " INT " + std::to_string(def.intel)
			+ " PER " + std::to_string(def.per) + " CHR " + std::to_string(def.chr)
			+ " HP " + std::to_string(def.hp) + " MP " + std::to_string(def.mp)
			+ " GOLD " + std::to_string(def.gold));
		SAM_DEBUG(MOD, "  " + std::to_string(def.skills.size()) + " skill(s), "
			+ std::to_string(def.startingItems.size()) + " starting item(s), "
			+ std::to_string(def.startingSpells.size()) + " starting spell(s)");
	}
}

void SAMClasses::clear()
{
	s_registry.clear();
	s_nextClassId = SAM_CLASS_ID_BASE;
	// v0.7.0 Feature 5: emptying the overlays fully reverts sam_patch_class + class
	// passives (nothing vanilla is written at rest — initClassStats recomputes each
	// creation, so subsequently created characters get vanilla stats again).
	s_classPatches.clear();
	s_classPassives.clear();
}

// --- v0.7.0 Feature 5: runtime class overrides (game-decoupled storage) ---------
bool SAMClasses::patchClass(int classnum, const SAMClassStatPatch& patch)
{
	if ( classnum >= SAM_CLASS_ID_BASE && !getClass(classnum) )
	{
		SAM_WARN(MOD, "patchClass: no custom class registered for id " + std::to_string(classnum));
		return false;
	}
	s_classPatches[classnum] = patch;
	SAM_INFO(MOD, "Patched class " + std::to_string(classnum) + " ("
		+ std::to_string(patch.stats.size()) + " stat override(s), "
		+ std::to_string(patch.skills.size()) + " skill(s))");
	return true;
}

void SAMClasses::unpatchClass(int classnum)
{
	if ( s_classPatches.erase(classnum) > 0 )
	{
		SAM_INFO(MOD, "Unpatched class " + std::to_string(classnum));
	}
}

bool SAMClasses::addClassPassive(int classnum, int effectId)
{
	if ( classnum >= SAM_CLASS_ID_BASE && !getClass(classnum) )
	{
		SAM_WARN(MOD, "addClassPassive: no custom class registered for id " + std::to_string(classnum));
		return false;
	}
	if ( effectId < 0 ) { return false; }
	s_classPassives[classnum].insert(effectId);
	SAM_INFO(MOD, "Added passive effect " + std::to_string(effectId) + " to class " + std::to_string(classnum));
	return true;
}

bool SAMClasses::removeClassPassive(int classnum, int effectId)
{
	auto it = s_classPassives.find(classnum);
	if ( it != s_classPassives.end() )
	{
		it->second.erase(effectId);
		if ( it->second.empty() ) { s_classPassives.erase(it); }
	}
	SAM_INFO(MOD, "Removed passive effect " + std::to_string(effectId) + " from class " + std::to_string(classnum));
	return true;
}

const SAMClassDef* SAMClasses::getClass(int classId)
{
	auto it = s_registry.find(classId);
	return (it != s_registry.end()) ? &it->second : nullptr;
}

int SAMClasses::count()
{
	return static_cast<int>(s_registry.size());
}

int SAMClasses::classIdAtIndex(int index)
{
	if ( index < 0 )
	{
		return -1;
	}
	int i = 0;
	for ( const auto& kv : s_registry )
	{
		if ( i == index )
		{
			return kv.first;
		}
		++i;
	}
	return -1;
}

int SAMClasses::classIdForIdString(const std::string& idString)
{
	for ( const auto& kv : s_registry )
	{
		if ( kv.second.id == idString )
		{
			return kv.first;
		}
	}
	return -1;
}

/*-------------------------------------------------------------------------------
	Application into the running game (game build only)
-------------------------------------------------------------------------------*/
#ifndef EDITOR

static std::string toLower(std::string s)
{
	for ( char& c : s ) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
	return s;
}

// "PRO_SWORD" -> the proficiency index from stat.hpp, or -1 if unknown.
static int proIdFromName(const std::string& n)
{
	static const std::map<std::string, int> m = {
		{ "PRO_LOCKPICKING", PRO_LOCKPICKING }, { "PRO_STEALTH", PRO_STEALTH },
		{ "PRO_TRADING", PRO_TRADING }, { "PRO_APPRAISAL", PRO_APPRAISAL },
		{ "PRO_THAUMATURGY", PRO_THAUMATURGY }, { "PRO_LEADERSHIP", PRO_LEADERSHIP },
		{ "PRO_MYSTICISM", PRO_MYSTICISM }, { "PRO_SORCERY", PRO_SORCERY },
		{ "PRO_RANGED", PRO_RANGED }, { "PRO_SWORD", PRO_SWORD },
		{ "PRO_MACE", PRO_MACE }, { "PRO_AXE", PRO_AXE },
		{ "PRO_POLEARM", PRO_POLEARM }, { "PRO_SHIELD", PRO_SHIELD },
		{ "PRO_UNARMED", PRO_UNARMED }, { "PRO_ALCHEMY", PRO_ALCHEMY },
	};
	auto it = m.find(n);
	return (it != m.end()) ? it->second : -1;
}

static Status statusFromName(const std::string& n)
{
	if ( n == "BROKEN" ) { return BROKEN; }
	if ( n == "DECREPIT" ) { return DECREPIT; }
	if ( n == "WORN" ) { return WORN; }
	if ( n == "EXCELLENT" ) { return EXCELLENT; }
	return SERVICABLE; // Barony's spelling; also the default
}

// Map a schema item type name to a vanilla ItemType. Barony's itemNameStrings
// are lowercase ("iron_axe"); the schema uses the uppercase enum name
// ("IRON_AXE"), so we lowercase before looking up. Custom "namespace:item"
// references are not resolvable yet (the item loader is a later session).
static bool itemTypeFromName(const std::string& n, ItemType& out)
{
	if ( n.find(':') != std::string::npos )
	{
		return false; // custom item — not loaded yet
	}
	auto it = ItemTooltips.itemNameStringToItemID.find(toLower(n));
	if ( it != ItemTooltips.itemNameStringToItemID.end() )
	{
		out = static_cast<ItemType>(it->second);
		return true;
	}
	return false;
}

// "SPELL_FORCEBOLT" -> spell id, via the lowercase spellItems internalName.
static int spellIdFromName(const std::string& n)
{
	const std::string lower = toLower(n);
	for ( const auto& kv : ItemTooltips.spellItems )
	{
		if ( kv.second.internalName == lower )
		{
			return kv.first;
		}
	}
	return -1;
}

/*-------------------------------------------------------------------------------
	Load-time enum validation + "did you mean?" diagnostics (game build only)
-------------------------------------------------------------------------------*/
static std::string toUpper(std::string s)
{
	for ( char& c : s ) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
	return s;
}

// Uppercased vanilla item names (schema convention), built once for suggestions.
static const std::vector<std::string>& validItemTypeNames()
{
	static std::vector<std::string> v;
	if ( v.empty() )
	{
		for ( const auto& kv : ItemTooltips.itemNameStringToItemID ) { v.push_back(toUpper(kv.first)); }
	}
	return v;
}
static const std::vector<std::string>& validSpellNames()
{
	static std::vector<std::string> v;
	if ( v.empty() )
	{
		for ( const auto& kv : ItemTooltips.spellItems ) { v.push_back(toUpper(kv.second.internalName)); }
	}
	return v;
}
static const std::vector<std::string> kSkillNames = {
	"PRO_LOCKPICKING", "PRO_STEALTH", "PRO_TRADING", "PRO_APPRAISAL", "PRO_THAUMATURGY",
	"PRO_LEADERSHIP", "PRO_MYSTICISM", "PRO_SORCERY", "PRO_RANGED", "PRO_SWORD", "PRO_MACE",
	"PRO_AXE", "PRO_POLEARM", "PRO_SHIELD", "PRO_UNARMED", "PRO_ALCHEMY"
};
static const std::vector<std::string> kStatusNames = { "BROKEN", "DECREPIT", "WORN", "SERVICABLE", "EXCELLENT" };
static const std::vector<std::string> kRollNames = { "STR", "DEX", "CON", "INT", "PER", "CHR" };

static bool listHas(const std::vector<std::string>& v, const std::string& s)
{
	for ( const std::string& x : v ) { if ( x == s ) { return true; } }
	return false;
}

static void validateClassSemantics(const SAMClassDef& def, const std::string& fileLabel)
{
	// Skills — must be a PRO_ name.
	for ( const auto& kv : def.skills )
	{
		if ( proIdFromName(kv.first) < 0 )
		{
			const std::string sug = SAMErrors::suggest(kv.first, kSkillNames);
			SAMErrors::reportSemantic(MOD, fileLabel, "/skills/" + kv.first, kv.first, "not a known skill",
				"a PRO_ skill name (16 total; see class.schema.json)",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
				"class [" + def.id + "] skips this skill.", /*warn=*/true);
		}
	}
	// Starting items — must be a vanilla ItemType or a loaded custom "ns:item".
	for ( size_t i = 0; i < def.startingItems.size(); ++i )
	{
		const SAMStartingItem& si = def.startingItems[i];
		ItemType t;
		const bool isCustomRef = si.type.find(':') != std::string::npos;
		const bool vanilla = itemTypeFromName(si.type, t);
		const bool custom = isCustomRef && (SAMItems::itemIdForIdString(si.type) >= 0);
		if ( !vanilla && !custom )
		{
			const std::string sug = isCustomRef ? "" : SAMErrors::suggest(si.type, validItemTypeNames());
			SAMErrors::reportSemantic(MOD, fileLabel,
				"/starting_items/" + std::to_string(i) + "/type", si.type,
				isCustomRef ? "custom item not found (is that mod loaded?)" : "not a known ItemType",
				"a vanilla ItemType (e.g. \"IRON_SWORD\") or a \"namespace:item\" id",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
				"class [" + def.id + "] skips this starting item.", /*warn=*/true);
		}
		if ( !si.status.empty() && !listHas(kStatusNames, si.status) )
		{
			const std::string sug = SAMErrors::suggest(si.status, kStatusNames);
			SAMErrors::reportSemantic(MOD, fileLabel,
				"/starting_items/" + std::to_string(i) + "/status", si.status, "not a known status",
				"one of BROKEN, DECREPIT, WORN, SERVICABLE, EXCELLENT",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"? (note Barony's spelling: SERVICABLE)"),
				"treated as SERVICABLE.", /*warn=*/true);
		}
	}
	// Starting spells — either a vanilla SPELL_ name or a custom "namespace:spell" id.
	// Custom spells register AFTER classes in the load loop, so we accept them by shape
	// (contains ':') here and resolve them at grant time.
	for ( const std::string& sp : def.startingSpells )
	{
		const bool looksCustom = sp.find(':') != std::string::npos;
		if ( !looksCustom && spellIdFromName(sp) < 0 )
		{
			const std::string sug = SAMErrors::suggest(sp, validSpellNames());
			SAMErrors::reportSemantic(MOD, fileLabel, "/starting_spells", sp, "not a known spell",
				"a SPELL_ name (e.g. \"SPELL_FORCEBOLT\") or a custom \"namespace:spell\" id",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
				"class [" + def.id + "] skips this spell.", /*warn=*/true);
		}
	}
	// Stat-growth rolls — must be a stat name.
	auto checkRolls = [&](const std::vector<std::string>& rolls, const char* which) {
		for ( const std::string& r : rolls )
		{
			if ( !listHas(kRollNames, r) )
			{
				const std::string sug = SAMErrors::suggest(r, kRollNames);
				SAMErrors::reportSemantic(MOD, fileLabel, std::string("/stat_growth/") + which, r,
					"not a known stat", "one of STR, DEX, CON, INT, PER, CHR",
					sug.empty() ? "" : ("did you mean \"" + sug + "\"?"), "ignored.", /*warn=*/true);
			}
		}
	};
	checkRolls(def.strongRolls, "strong_rolls");
	checkRolls(def.weakRolls, "weak_rolls");
}

void SAMClasses::applyStats(int classnum, Stat* stat)
{
	if ( !stat ) { return; }
	const SAMClassDef* def = getClass(classnum);
	if ( !def )
	{
		SAM_WARN(MOD, "applyStats: no custom class registered for id " + std::to_string(classnum));
		return;
	}

	stat->STR += def->str;
	stat->DEX += def->dex;
	stat->CON += def->con;
	stat->INT += def->intel;
	stat->PER += def->per;
	stat->CHR += def->chr;
	stat->MAXHP += def->hp;  stat->HP += def->hp;
	stat->MAXMP += def->mp;  stat->MP += def->mp;
	stat->GOLD += def->gold;

	for ( const auto& kv : def->skills )
	{
		const int pro = proIdFromName(kv.first);
		if ( pro >= 0 )
		{
			stat->setProficiency(pro, kv.second);
		}
		else
		{
			SAM_WARN(MOD, "class [" + def->id + "] unknown skill '" + kv.first + "' — ignored.");
		}
	}

	SAM_INFO(MOD, "Applied stats for [" + def->id + "] (STR " + std::to_string(def->str)
		+ " DEX " + std::to_string(def->dex) + " CON " + std::to_string(def->con)
		+ " INT " + std::to_string(def->intel) + " PER " + std::to_string(def->per)
		+ " CHR " + std::to_string(def->chr) + " HP " + std::to_string(def->hp)
		+ " MP " + std::to_string(def->mp) + " GOLD " + std::to_string(def->gold) + "), "
		+ std::to_string(def->skills.size()) + " skill(s).");
	// NOTE: HP/MP clamping is intentionally left to initClassStats' unconditional
	// clamp block (charclass.cpp ~635-690), which runs after this returns.
}

// v0.7.0 Feature 5: apply sam_patch_class ABSOLUTE stat overrides. Runs at the end of
// initClassStats (after all vanilla/SAM/challenge-run deltas), so the override is the
// final word; the caller's HP/MP clamp + OLDHP recompute still runs afterwards.
void SAMClasses::applyStatOverrides(int classnum, Stat* stat)
{
	if ( !stat ) { return; }
	auto it = s_classPatches.find(classnum);
	if ( it == s_classPatches.end() ) { return; }
	const SAMClassStatPatch& p = it->second;

	auto clampI = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
	for ( const auto& kv : p.stats )
	{
		const std::string& k = kv.first;
		const int v = kv.second;
		if      ( k == "STR" ) { stat->STR = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( k == "DEX" ) { stat->DEX = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( k == "CON" ) { stat->CON = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( k == "INT" ) { stat->INT = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( k == "PER" ) { stat->PER = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( k == "CHR" ) { stat->CHR = clampI(v, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( k == "HP" )    { stat->HP = v; }              // clamped >=1 by caller
		else if ( k == "MAXHP" ) { stat->MAXHP = (v < 1 ? 1 : v); }
		else if ( k == "MP" )    { stat->MP = v; }
		else if ( k == "MAXMP" ) { stat->MAXMP = (v < 0 ? 0 : v); }
		else if ( k == "GOLD" )  { stat->GOLD = (v < 0 ? 0 : v); }
		else if ( k == "LVL" || k == "LEVEL" ) { stat->LVL = clampI(v, 1, 255); }
		else if ( k == "EXP" )   { stat->EXP = clampI(v, 0, 99); }
		else { SAM_WARN(MOD, "sam_patch_class: unknown stat '" + k + "' — ignored."); }
	}
	for ( const auto& kv : p.skills )
	{
		const int pro = proIdFromName(kv.first);
		if ( pro >= 0 ) { stat->setProficiency(pro, clampI(kv.second, 0, 100)); }
		else { SAM_WARN(MOD, "sam_patch_class: unknown skill '" + kv.first + "' — ignored."); }
	}
	SAM_INFO(MOD, "Applied class override for id " + std::to_string(classnum) + " ("
		+ std::to_string(p.stats.size()) + " stat(s), " + std::to_string(p.skills.size()) + " skill(s))");
}

namespace
{
	// Every head index this framework registered, so the engine's hardcoded
	// isPlayerHeadSprite switch can be widened to accept them.
	std::set<int> s_customHeadSprites;

	// Barony's RACE_* -> the Monster enum name a modder writes in "races". Kept local so
	// this file doesn't depend on the engine's race tables; an unknown key simply never
	// matches and falls through to "default", which is the safe outcome.
	const char* samRaceKey(int playerRace)
	{
		switch ( playerRace )
		{
			case 0:  return "HUMAN";
			case 1:  return "SKELETON";
			case 2:  return "VAMPIRE";
			case 3:  return "SUCCUBUS";
			case 4:  return "GOATMAN";
			case 5:  return "AUTOMATON";
			case 6:  return "INCUBUS";
			case 7:  return "GOBLIN";
			case 8:  return "INSECTOID";
			case 9:  return "RAT";
			case 10: return "TROLL";
			case 11: return "SPIDER";
			case 12: return "IMP";
			case 13: return "GNOME";
			case 14: return "GREMLIN";
			case 15: return "DRYAD";
			case 16: return "MYCONID";
			case 17: return "SALAMANDER";
			default: return "";
		}
	}
}

void SAMClasses::resolveAppearance()
{
	s_customHeadSprites.clear();
	for ( auto& kv : s_registry )
	{
		SAMClassDef& def = kv.second;
		def.appearanceHeadIdx.clear();
		for ( const auto& hv : def.appearanceHeads )
		{
			// A custom .vox registered by SAMModels wins; otherwise fall back to a plain
			// numeric index so a modder can name a vanilla head directly.
			int idx = SAMModels::modelIndexForId(hv.second);
			if ( idx < 0 )
			{
				char* end = nullptr;
				const long n = std::strtol(hv.second.c_str(), &end, 10);
				if ( end && *end == '\0' && n >= 0 ) { idx = (int)n; }
			}
			if ( idx < 0 )
			{
				SAM_WARN(MOD, "Class [" + def.id + "] appearance head '" + hv.second
					+ "' for race '" + hv.first + "' is not a registered model — ignoring it "
					+ "(that race keeps its normal head).");
				continue;
			}
			def.appearanceHeadIdx[hv.first] = idx;
			// Only OUR models need the isPlayerHeadSprite widening; a vanilla index is
			// already in the engine's switch.
			if ( SAMModels::modelIndexForId(hv.second) >= 0 ) { s_customHeadSprites.insert(idx); }
			SAM_DEBUG(MOD, "  [" + def.id + "] head for " + hv.first + " -> model " + std::to_string(idx));
		}

		// Whole-body model (jet etc.): resolve its path to an index, and register it as a
		// "custom head sprite" — actPlayer draws it as my->sprite, the slot the engine
		// otherwise treats as the head, so isPlayerHeadSprite must accept it.
		def.bodyModelIdx = -1;
		if ( !def.bodyModel.empty() )
		{
			int bidx = SAMModels::modelIndexForId(def.bodyModel);
			if ( bidx < 0 )
			{
				char* end = nullptr;
				const long n = std::strtol(def.bodyModel.c_str(), &end, 10);
				if ( end && *end == '\0' && n >= 0 ) { bidx = (int)n; }
			}
			if ( bidx >= 0 )
			{
				def.bodyModelIdx = bidx;
				s_customHeadSprites.insert(bidx);
				SAM_DEBUG(MOD, "  [" + def.id + "] body model -> " + std::to_string(bidx));
			}
			else
			{
				SAM_WARN(MOD, "Class [" + def.id + "] body_model '" + def.bodyModel
					+ "' is not a registered model — ignoring (class keeps its normal body).");
			}
		}
	}
}

int SAMClasses::bodyModelFor(int classnum)
{
	const SAMClassDef* def = getClass(classnum);
	return def ? def->bodyModelIdx : -1;
}

std::vector<std::string> SAMClasses::appearanceModelPaths()
{
	std::vector<std::string> out;
	auto looksLikeFile = [](const std::string& s) {
		return !s.empty() && (s.find('/') != std::string::npos || s.find(".vox") != std::string::npos);
	};
	for ( const auto& kv : s_registry )
	{
		if ( looksLikeFile(kv.second.bodyModel) ) { out.push_back(kv.second.bodyModel); }
		for ( const auto& hv : kv.second.appearanceHeads )
		{
			if ( looksLikeFile(hv.second) ) { out.push_back(hv.second); }
		}
	}
	return out;
}

int SAMClasses::headSpriteFor(int classnum, int playerRace)
{
	const SAMClassDef* def = getClass(classnum);
	if ( !def || def->appearanceHeadIdx.empty() ) { return -1; }

	const char* key = samRaceKey(playerRace);
	if ( key && key[0] )
	{
		auto it = def->appearanceHeadIdx.find(key);
		if ( it != def->appearanceHeadIdx.end() ) { return it->second; }
	}
	// No entry for this race — fall back to "default" if the author wrote one, else
	// leave the player alone entirely. Never force a look onto a race nobody authored
	// for: the limb offset table is indexed BY RACE, so a mismatched head would sit at
	// the wrong focal point.
	auto def_it = def->appearanceHeadIdx.find("default");
	return ( def_it != def->appearanceHeadIdx.end() ) ? def_it->second : -1;
}

bool SAMClasses::isCustomHeadSprite(int sprite)
{
	return s_customHeadSprites.find(sprite) != s_customHeadSprites.end();
}

void SAMClasses::applyManaRegen(int classnum, const int* statValues, int numStats, double& regenPerMinute)
{
	const SAMClassDef* def = getClass(classnum);
	if ( !def || !def->hasMpRegen ) { return; } // vanilla class, or no mp_regen block

	regenPerMinute += def->mpRegenBase;

	if ( statValues && numStats > 0 )
	{
		// Barony's STAT_* order. Kept local so this file stays free of stat.hpp.
		static const char* const kStatOrder[] = { "STR", "DEX", "CON", "INT", "PER", "CHR" };
		const int kStatOrderCount = (int)(sizeof(kStatOrder) / sizeof(kStatOrder[0]));
		for ( const auto& kv : def->mpRegenStatScaling )
		{
			for ( int i = 0; i < kStatOrderCount && i < numStats; ++i )
			{
				if ( kv.first == kStatOrder[i] )
				{
					regenPerMinute += statValues[i] * kv.second;
					break;
				}
			}
		}
	}

	regenPerMinute *= def->mpRegenMultiplier;
}

// v0.7.0 Feature 5: grant a class's registered passive effects (EFF_* ids) to a Stat
// at creation. Written straight onto the Stat's EFFECTS array with an indefinite (-1)
// timer so it never expires; Entity may not exist yet, so Entity::setEffect is avoided.
void SAMClasses::applyPassives(int classnum, Stat* stat)
{
	if ( !stat ) { return; }
	auto it = s_classPassives.find(classnum);
	if ( it == s_classPassives.end() ) { return; }
	for ( int eff : it->second )
	{
		if ( eff >= 0 && eff < NUMEFFECTS )
		{
			stat->setEffectActive(eff, 1);
			stat->EFFECTS_TIMERS[eff] = -1; // indefinite (positive timers decrement + expire)
		}
	}
	SAM_INFO(MOD, "Applied " + std::to_string(it->second.size()) + " passive effect(s) to class " + std::to_string(classnum));
}

void SAMClasses::applyLoadout(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return; }
	const SAMClassDef* def = getClass(client_classes[player]);
	if ( !def ) { return; }

	const bool isLocalPlayer = players[player]->isLocalPlayer();
	// Defensive multiplayer guard (the charclass.cpp hook also returns before
	// calling us for players we don't own).
	if ( !isLocalPlayer && multiplayer == CLIENT && intro == false ) { return; }

	auto& hotbar = players[player]->hotbar.slots();
	s_hotbarPins[player].clear(); // rebuilt below; re-applied after assignHotbarSlots wipes the bar
	int given = 0;
	for ( const SAMStartingItem& si : def->startingItems )
	{
		ItemType type;
		const bool isCustomRef = si.type.find(':') != std::string::npos;
		bool resolved = false;
		if ( isCustomRef )
		{
			// "namespace:item" — resolve to the runtime slot (>= SAM_ITEM_ID_BASE)
			// that SAMItems registered this custom item into. Custom items are
			// registered at mod-load, which is before any game starts, so the
			// lookup is populated by the time a class grants its loadout.
			const int customId = SAMItems::itemIdForIdString(si.type);
			if ( customId >= 0 ) { type = static_cast<ItemType>(customId); resolved = true; }
		}
		else
		{
			resolved = itemTypeFromName(si.type, type);
		}
		if ( !resolved )
		{
			SAM_ERROR(MOD, "class [" + def->id + "] references item '" + si.type + "' — "
				+ (isCustomRef ? "custom item not registered (is that mod loaded?)"
				               : "not a known item type") + " — skipping.");
			continue;
		}
		const Status status = statusFromName(si.status);
		Item* item = newItem(type, status, static_cast<Sint16>(si.beatitude),
			static_cast<Sint16>(si.count), static_cast<Uint32>(si.appearance), si.identified, nullptr);
		if ( !item ) { continue; }
		if ( isCustomRef )
		{
			SAM_INFO(MOD, "class [" + def->id + "] granted CUSTOM starting item '" + si.type
				+ "' (runtime slot " + std::to_string(static_cast<int>(type)) + ").");
		}
		else
		{
			SAM_DEBUG(MOD, "class [" + def->id + "] granted starting item '" + si.type + "'.");
		}

		if ( isLocalPlayer )
		{
			Item* item2 = itemPickup(player, item);
			if ( item2 )
			{
				// useItem() drinks/reads consumables and can free item2 via
				// consumeItem() (count -> 0), leaving item2 dangling. Capture the
				// uid BEFORE useItem so the hotbar write never touches freed memory.
				const Uint32 pickedUid = item2->uid;
				if ( si.equip ) { useItem(item2, player); }
				if ( si.hotbarSlot >= 0 && si.hotbarSlot < static_cast<int>(NUM_HOTBAR_SLOTS) )
				{
					hotbar[si.hotbarSlot].item = pickedUid;
					s_hotbarPins[player].push_back({ si.hotbarSlot, pickedUid }); // re-applied later
				}
			}
			free(item);
		}
		else
		{
			// Host applying to another player: equip if requested, else discard
			// the temp (consumables are only granted to local players, as vanilla).
			if ( si.equip ) { useItem(item, player); }
			else { free(item); }
		}
		++given;
		SAM_DEBUG(MOD, "  gave " + si.type + " x" + std::to_string(si.count)
			+ (si.equip ? " (equipped)" : "")
			+ (si.hotbarSlot >= 0 ? (" -> hotbar " + std::to_string(si.hotbarSlot)) : std::string()));
	}
	SAM_INFO(MOD, "Applied loadout for [" + def->id + "]: " + std::to_string(given) + " item stack(s).");
}

// Re-pin the class's hotbar items after Barony's ClassHotbarConfig_t::assignHotbarSlots()
// has zeroed and rebuilt the bar (it runs a few frames after applyLoadout and would
// otherwise erase every custom pin). Called at the end of assignHotbarSlots.
void SAMClasses::reapplyHotbarPins(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return; }
	// Only a currently-custom class has fresh pins. applyLoadout isn't called for a vanilla
	// class, so without this guard a vanilla character following a custom-class game would
	// re-apply the previous game's stale uids and clobber its correct vanilla hotbar.
	if ( !getClass(client_classes[player]) ) { s_hotbarPins.erase(player); return; }
	auto it = s_hotbarPins.find(player);
	if ( it == s_hotbarPins.end() || it->second.empty() ) { return; }
	auto& hotbar = players[player]->hotbar.slots();
	for ( const auto& pin : it->second )
	{
		const int slot = pin.first;
		const Uint32 uid = pin.second;
		if ( slot < 0 || slot >= static_cast<int>(NUM_HOTBAR_SLOTS) ) { continue; }
		// The default-layout pass may have auto-placed this same item in another slot;
		// clear those so a pinned item isn't duplicated across the bar.
		for ( int i = 0; i < static_cast<int>(NUM_HOTBAR_SLOTS); ++i )
		{
			if ( i != slot && hotbar[i].item == uid ) { hotbar[i].item = 0; }
		}
		hotbar[slot].item = uid;
	}
	SAM_DEBUG(MOD, "Re-applied " + std::to_string(it->second.size()) + " hotbar pin(s) for player "
		+ std::to_string(player) + ".");
}

void SAMClasses::applySpells(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return; }
	const SAMClassDef* def = getClass(client_classes[player]);
	if ( !def ) { return; }

	const bool isLocalPlayer = players[player]->isLocalPlayer();
	if ( !isLocalPlayer && multiplayer == CLIENT && intro == false ) { return; }

	int learned = 0;
	for ( const std::string& sp : def->startingSpells )
	{
		const int id = spellIdFromName(sp);
		if ( id < 0 )
		{
			// Not a vanilla spell. If it's a registered CUSTOM spell ("namespace:spell"),
			// grant it via the engine spell_t that SAMSpells built at load.
			if ( SAMSpells::getSpellByName(sp) )
			{
				if ( SAMSpells::grantCustomSpell(player, sp) ) { ++learned; }
			}
			else
			{
				SAM_ERROR(MOD, "class [" + def->id + "] references unknown spell '" + sp + "' — skipping.");
			}
			continue;
		}
		addSpell(id, player, true);
		++learned;
		SAM_DEBUG(MOD, "  learned " + sp);
	}
	if ( learned > 0 )
	{
		SAM_INFO(MOD, "Applied " + std::to_string(learned) + " starting spell(s) for [" + def->id + "].");
	}
}

#endif // !EDITOR
