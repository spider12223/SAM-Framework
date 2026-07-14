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
				if ( si.equip ) { useItem(item2, player); }
				if ( si.hotbarSlot >= 0 && si.hotbarSlot < static_cast<int>(NUM_HOTBAR_SLOTS) )
				{
					hotbar[si.hotbarSlot].item = item2->uid;
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
