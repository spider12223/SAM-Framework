/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_monsters.cpp
	Desc: implementation of custom monster/enemy support.

	Translates a friendly, namespaced monster JSON (monster.schema.json) into
	Barony's raw data/custom-monsters/<name>.json format, and generates a merged
	data/monstercurve.json spawn table from each monster's `spawn` block. Both are
	written into a private prepend-mounted overlay (mirroring sam_patcher), so
	Barony's own lazy MonsterStatCustomManager / MonsterCurveCustomManager reads
	pick them up. Game build only; the caller is #ifndef EDITOR-guarded.

	Crash-safety contract: MonsterStatCustomManager::readFromFile accesses
	d["stats"], d["misc_stats"], d["proficiencies"], d["equipped_items"] and
	d["inventory_items"] with NO HasMember guard — a missing section is UB. So
	every file we emit ALWAYS contains those five sections (empty {}/[] is fine),
	and `followers` (if emitted) always carries both num_followers and
	follower_variants.

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros that
// would break nlohmann/std. Must come before any include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_monsters.hpp"
#include "sam_workshop.hpp"   // SAMModManifest
#include "sam_logger.hpp"
#include "sam_models.hpp" // modelIndexForId — body resolution report
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include "main.hpp"           // physfs.h + core
#include "game.hpp"
#include "stat.hpp"
#include "items.hpp"          // ItemType
#include "player.hpp"         // (mod_tools.hpp prerequisite — matches sam_classes.cpp include order)
#include "net.hpp"            // (mod_tools.hpp prerequisite)
#include "mod_tools.hpp"      // ItemTooltips (itemNameStringToItemID)
#include "monster.hpp"        // monstertypename[], NUMMONSTERS
#include "files.hpp"          // outputdir

#include <fstream>
#include <sstream>
#include <filesystem>
#include <map>
#include <set>
#include <vector>
#include <cctype>
#include <system_error>

namespace
{
	// variant display name -> trait bitmask. Empty in vanilla.
	std::map<std::string, unsigned long long> s_monsterTraits;
	// variant display name -> mod-declared body model id ("ns:model"). Empty in vanilla.
	std::map<std::string, SAMMonsters::BodyDef> s_monsterBodies;
	// "ns:slug" -> what it takes to summon it. Keyed by ID, unlike the two maps above.
	std::map<std::string, SAMMonsters::VariantRef> s_variantsById;
	bool s_anyBodyDeclared = false;

	unsigned long long samMonsterTraitBit(const std::string& name)
	{
		return SAMMonsters::traitBitForName(name.c_str());
	}
}

// The single trait-name table. Public so the script binding resolves names through the
// exact same list the JSON parser uses.
unsigned long long SAMMonsters::traitBitForName(const char* nameC)
{
	const std::string name = nameC ? nameC : "";
	if ( name == "boss" )            { return 1ULL << 0; }
	if ( name == "trader" )          { return 1ULL << 1; }
	if ( name == "untargetable" )    { return 1ULL << 2; }
	if ( name == "immobile_turret" ) { return 1ULL << 3; }
	if ( name == "never_retreat" )   { return 1ULL << 4; }
	if ( name == "water_walking" )   { return 1ULL << 5; }
	if ( name == "undead" )          { return 1ULL << 6; }
	if ( name == "ally_recolour" )   { return 1ULL << 7; }
	if ( name == "tinker_construct" ) { return 1ULL << 8; }
	if ( name == "no_digestion" )    { return 1ULL << 9; }
	if ( name == "pass_through" )    { return 1ULL << 10; }
	return 0;
}


using nlohmann::json;
namespace fs = std::filesystem;

static const char* MOD = "MONSTERS";

// Private overlay folder, OUTSIDE ./mods/ (so it never shows in the Local Mods
// browser) and separate from sam_patch_overlay (independent lifecycle). Written
// with std::ofstream, prepend-mounted so its files win over the base.
static const char* OVERLAY_NAME = "sam_monster_overlay";
static bool s_mounted = false;
static int s_filesWritten = 0;
static int s_declared = 0;
static int s_curveLevels = 0;

// Barony reads each custom-monster file AND monstercurve.json into a fixed
// 65536-byte stack buffer; stay safely under it or the read is truncated.
static const size_t SAFE_MAX = 64000;

/*-------------------------------------------------------------------------------
	Local helpers
-------------------------------------------------------------------------------*/
static std::string joinPath(const std::string& dir, const std::string& file)
{
	if ( dir.empty() ) { return file; }
	const char back = dir.back();
	return (back == '/' || back == '\\') ? (dir + file) : (dir + "/" + file);
}

static std::string overlayRealDir()
{
	return joinPath(std::string(outputdir), OVERLAY_NAME);
}

static bool readWholeFile(const std::string& path, std::string& out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if ( !f.is_open() ) { return false; }
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

static std::string toLower(std::string s)
{
	for ( char& c : s ) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
	return s;
}

static bool listHas(const std::vector<std::string>& v, const std::string& s)
{
	for ( const std::string& x : v ) { if ( x == s ) { return true; } }
	return false;
}

static bool startsWithUnused(const std::string& s)
{
	return s.rfind("monster_unused", 0) == 0;
}

// The custom-monster JSON skill names (distinct from the class JSON's PRO_ names).
// These are the ENGINE's proficiency display names (getSkillLangEntry ->
// lang/en.txt ids 236-249, 3204, 3340) — NOT the class JSON's PRO_ names, and
// NOT the (buggy) names in Barony's own custom-monster samples, which use
// Appraise/Swimming/Casting/Magic for slots 3/4/6/7 where the engine actually
// matches Lore/Thaumaturgy/Mysticism/Sorcery. A key must equal one of these
// exactly or the engine silently leaves that proficiency at 0.
static const std::vector<std::string> kSkillNames = {
	"Tinkering", "Stealth", "Trading", "Lore", "Thaumaturgy", "Leader",
	"Mysticism", "Sorcery", "Ranged", "Sword", "Mace", "Axe", "Polearm",
	"Shield", "Unarmed", "Alchemy"
};
static const std::vector<std::string> kSlotNames = {
	"weapon", "shield", "helmet", "breastplate", "gloves", "shoes",
	"cloak", "ring", "amulet", "mask"
};
static const std::vector<std::string> kStatusNames = {
	"broken", "decrepit", "worn", "serviceable", "excellent"
};

// Index of a species name in monstertypename[], or -1. 0 == "nothing" (the
// engine's failure sentinel), which we treat as invalid for a base_type.
static int monsterTypeIndex(const std::string& name)
{
	for ( int i = 0; i < NUMMONSTERS; ++i )
	{
		if ( name == monstertypename[i] ) { return i; }
	}
	return -1;
}

// Usable base creature names (index >= 1, excluding the reserved unused slots).
static const std::vector<std::string>& validMonsterTypeNames()
{
	static std::vector<std::string> v;
	if ( v.empty() )
	{
		for ( int i = 1; i < NUMMONSTERS; ++i )
		{
			const std::string s = monstertypename[i];
			if ( !startsWithUnused(s) ) { v.push_back(s); }
		}
	}
	return v;
}

// Lowercase vanilla item names, for item-type validation + suggestions.
static const std::vector<std::string>& validItemNames()
{
	static std::vector<std::string> v;
	if ( v.empty() )
	{
		for ( const auto& kv : ItemTooltips.itemNameStringToItemID ) { v.push_back(kv.first); }
	}
	return v;
}

// Friendly property key -> Barony raw property key (most pass through unchanged).
static std::string mapPropertyKey(const std::string& k)
{
	if ( k == "display_as_generic_species" ) { return "monster_name_always_display_as_generic_species"; }
	if ( k == "fill_empty_equipment_with_defaults" ) { return "populate_empty_equipped_items_with_default"; }
	return k;
}

/*-------------------------------------------------------------------------------
	Item-entry validation (non-blocking: warn + keep, since Barony degrades
	gracefully to a default item rather than crashing on an unknown name)
-------------------------------------------------------------------------------*/
static void validateItemType(const json& v, const std::string& fileLabel, const std::string& field)
{
	auto checkOne = [&](const json& t) {
		if ( !t.is_string() ) { return; }
		const std::string s = toLower(t.get<std::string>());
		if ( s == "empty" ) { return; }
		if ( !listHas(validItemNames(), s) )
		{
			const std::string sug = SAMErrors::suggest(s, validItemNames());
			SAMErrors::reportSemantic(MOD, fileLabel, field, s, "not a known item",
				"a vanilla item name (lowercase, e.g. \"iron_sword\")",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
				"a fallback item spawns in this slot instead.", /*warn=*/true);
		}
	};
	if ( v.is_string() ) { checkOne(v); }
	else if ( v.is_array() ) { for ( const json& e : v ) { checkOne(e); } }
}

static void validateStatus(const json& v, const std::string& fileLabel, const std::string& field)
{
	auto checkOne = [&](const json& t) {
		if ( !t.is_string() ) { return; }
		const std::string s = t.get<std::string>();
		if ( !listHas(kStatusNames, s) )
		{
			const std::string sug = SAMErrors::suggest(s, kStatusNames);
			SAMErrors::reportSemantic(MOD, fileLabel, field, s, "not a known status",
				"one of broken, decrepit, worn, serviceable, excellent",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
				"treated as decrepit.", /*warn=*/true);
		}
	};
	if ( v.is_string() ) { checkOne(v); }
	else if ( v.is_array() ) { for ( const json& e : v ) { checkOne(e); } }
}

static void validateItemEntry(const json& entry, const std::string& fileLabel, const std::string& field)
{
	if ( !entry.is_object() ) { return; }
	auto tIt = entry.find("type");
	if ( tIt != entry.end() ) { validateItemType(*tIt, fileLabel, field + "/type"); }
	auto sIt = entry.find("status");
	if ( sIt != entry.end() ) { validateStatus(*sIt, fileLabel, field + "/status"); }
}

// A slot/inventory value is a single entry OR an array of entries (weighted pick).
static void validateItemSlot(const json& v, const std::string& fileLabel, const std::string& field)
{
	if ( v.is_object() ) { validateItemEntry(v, fileLabel, field); }
	else if ( v.is_array() )
	{
		for ( size_t i = 0; i < v.size(); ++i )
		{
			validateItemEntry(v[i], fileLabel, field + "/" + std::to_string(i));
		}
	}
}

// Copy only the numeric members of an object. Barony reads every stat / misc_stat
// / proficiency leaf with an UNGUARDED GetInt(), so a non-numeric value that
// reached the emitted file would be UB in release (garbage) or an abort in a
// debug/asserted build. Drop-and-warn keeps the file crash-safe.
static json numericOnly(const json& obj, const std::string& fileLabel, const std::string& section)
{
	json out = json::object();
	if ( !obj.is_object() ) { return out; }
	for ( auto& kv : obj.items() )
	{
		if ( kv.value().is_number() ) { out[kv.key()] = kv.value(); }
		else
		{
			SAM_WARN(MOD, "Non-numeric value for '" + kv.key() + "' in " + section + " ("
				+ fileLabel + ") — dropped.");
		}
	}
	return out;
}

/*-------------------------------------------------------------------------------
	Friendly monster JSON -> Barony raw custom-monster document
-------------------------------------------------------------------------------*/
struct Translated
{
	bool ok = false;
	std::string slug;         // local part of the id ("goblin_captain")
	std::string variantFile;  // on-disk name "<ns>_<slug>" (no .json)
	std::string baseType;     // validated base species
	std::string name;         // display name
	json raw;                 // raw custom-monster document to write
	json spawn = json::array();
};

static Translated translateMonster(const json& in, const std::string& modNs, const std::string& fileLabel,
	std::vector<std::pair<std::string, std::string>>& followerRefs)
{
	Translated tr;

	// --- id (namespace:monster) ---
	if ( !in.contains("id") || !in["id"].is_string() )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/id", "", "missing or not a string",
			"a \"namespace:monster\" id", "", "monster NOT loaded.");
		return tr;
	}
	const std::string id = in["id"].get<std::string>();
	const size_t colon = id.find(':');
	if ( colon == std::string::npos || colon == 0 || colon + 1 >= id.size() )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/id", id, "not in namespace:monster form",
			"e.g. \"" + modNs + ":goblin_captain\"", "", "monster NOT loaded.");
		return tr;
	}
	const std::string idNs = id.substr(0, colon);
	tr.slug = id.substr(colon + 1);

	// Path-traversal guard: tr.slug and modNs both become part of the on-disk
	// overlay filename (variantFile), written under the overlay dir via a raw
	// std::ofstream that does NOT normalize '..'. Reject anything that isn't a
	// plain identifier so a crafted id like "ns:../../evil" (or a "../"
	// namespace) can't escape the sam_monster_overlay directory.
	auto safeIdent = [](const std::string& s) -> bool {
		if ( s.empty() ) { return false; }
		for ( char c : s )
		{
			const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
				|| (c >= '0' && c <= '9') || c == '_' || c == '-';
			if ( !ok ) { return false; }
		}
		return true;
	};
	if ( !safeIdent(tr.slug) )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/id", id, "monster id has illegal characters after ':'",
			"only letters, digits, '_' or '-' (e.g. \"" + modNs + ":goblin_captain\")", "", "monster NOT loaded.");
		return tr;
	}
	if ( !safeIdent(modNs) )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/namespace", modNs, "mod namespace has illegal characters",
			"only letters, digits, '_' or '-'", "", "monster NOT loaded.");
		return tr;
	}
	if ( idNs != modNs )
	{
		SAM_WARN(MOD, "Monster id '" + id + "' namespace does not match mod namespace '" + modNs
			+ "' — using '" + modNs + "' for the on-disk file.");
	}
	tr.variantFile = modNs + "_" + tr.slug;

	// --- name ---
	if ( !in.contains("name") || !in["name"].is_string() || in["name"].get<std::string>().empty() )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/name", "", "missing or empty",
			"a non-empty display name", "", "monster [" + id + "] NOT loaded.");
		return tr;
	}
	tr.name = in["name"].get<std::string>();
	// Stat::name is char[128] and the engine copies into it with strcpy, twice (the
	// StatEntry, then the live Stat). Clamp here, with a word, rather than let a long
	// display name overrun two engine buffers at spawn time.
	if ( tr.name.size() > 127 )
	{
		SAM_WARN(MOD, "Monster '" + id + "' name is " + std::to_string(tr.name.size())
			+ " characters; the engine holds 127. Truncated.");
		tr.name.resize(127);
	}

	// S.A.M: mod-declared engine traits for this monster. Recorded by NAME, because a custom
	// monster is a variant of a vanilla Monster type and shares that type -- the name is what
	// identifies it when the engine stamps the variant onto a live Stat.
	if ( in.contains("traits") && in["traits"].is_array() )
	{
		unsigned long long bits = 0;
		for ( const auto& t : in["traits"] )
		{
			if ( !t.is_string() ) { continue; }
			std::string tn = t.get<std::string>();
			for ( char& c : tn ) { c = (char)std::tolower((unsigned char)c); }
			const unsigned long long b = samMonsterTraitBit(tn);
			if ( b ) { bits |= b; }
			else
			{
				SAM_WARN(MOD, "Monster '" + tr.name + "' declares unknown trait '" + tn + "' — ignored.");
			}
		}
		if ( bits )
		{
			// Keyed by DISPLAY NAME, which is not guaranteed unique: two mods, or a mod and a
			// vanilla named NPC, can collide. Traits change GAMEPLAY (untargetable, pass_through,
			// boss), so a silent collision could make an unrelated creature unhittable. First
			// declaration wins and the clash is reported.
			auto prev = s_monsterTraits.find(tr.name);
			if ( prev != s_monsterTraits.end() && prev->second != bits )
			{
				SAM_WARN(MOD, "Two monsters are both named '" + tr.name + "' with different traits. "
					"Traits are matched by name, so the first one loaded wins. Rename one of them.");
			}
			else { s_monsterTraits[tr.name] = bits; }
		}
	}

	// S.A.M: an optional custom BODY model, recorded by name for the same reason as traits.
	// The value is the mod's "namespace:model" id; it is resolved to an engine model index
	// lazily (at draw time), because models are appended to the engine table AFTER monster
	// JSON is parsed, and because an index is load-order dependent while the id is stable.
	if ( in.contains("body") && in["body"].is_object() )
	{
		const auto& b = in["body"];
		SAMMonsters::BodyDef bd;
		if ( b.contains("model") && b["model"].is_string() ) { bd.model = b["model"].get<std::string>(); }
		if ( b.contains("attack") && b["attack"].is_string() ) { bd.attack = b["attack"].get<std::string>(); }
		if ( b.contains("cast") && b["cast"].is_string() )     { bd.cast = b["cast"].get<std::string>(); }
		if ( b.contains("death") && b["death"].is_string() )   { bd.death = b["death"].get<std::string>(); }
		if ( b.contains("hitbox") && b["hitbox"].is_number_integer() )
		{
			// Hard ceiling of 127, because sizex/sizey go out in the entity packet as a Sint8
			// (net.cpp writes (Sint8)entity->sizex). A hitbox of 200 would arrive on a
			// multiplayer client as -56 and silently break collision for everyone but the host,
			// so clamp here and say so rather than shipping a box that only works solo.
			const int hb = b["hitbox"].get<int>();
			bd.hitbox = (hb < 0) ? 0 : ((hb > 127) ? 127 : hb);
			if ( hb > 127 )
			{
				SAM_WARN(MOD, "Monster '" + tr.name + "' sets body.hitbox to "
					+ std::to_string(hb) + ", above the maximum of 127. The hitbox is sent to "
					"multiplayer clients as a signed byte, so a larger value would arrive "
					"negative and break collision for them. Clamped to 127.");
			}
		}
		if ( b.contains("offset") && b["offset"].is_object() )
		{
			const auto& o = b["offset"];
			if ( o.contains("forward") && o["forward"].is_number() ) { bd.offsetForward = o["forward"].get<double>(); }
			if ( o.contains("side") && o["side"].is_number() )       { bd.offsetSide = o["side"].get<double>(); }
			if ( o.contains("up") && o["up"].is_number() )           { bd.offsetUp = o["up"].get<double>(); }
		}
		if ( b.contains("yaw_offset") && b["yaw_offset"].is_number() )
		{
			bd.yawOffsetDeg = b["yaw_offset"].get<double>();
		}
		if ( b.contains("frame_ticks") && b["frame_ticks"].is_number_integer() )
		{
			const int ft = b["frame_ticks"].get<int>();
			bd.frameTicks = (ft < 1) ? 1 : ft;   // 0 would divide by zero at draw time
		}
		if ( b.contains("fly") && b["fly"].is_array() )
		{
			for ( const auto& fr : b["fly"] )
			{
				if ( fr.is_string() && !fr.get<std::string>().empty() ) { bd.fly.push_back(fr.get<std::string>()); }
			}
		}
		if ( !bd.model.empty() )
		{
			auto prevBody = s_monsterBodies.find(tr.name);
			if ( prevBody != s_monsterBodies.end() && prevBody->second.model != bd.model )
			{
				SAM_WARN(MOD, "Two monsters are both named '" + tr.name + "' with different body "
					"models. Bodies are matched by name, so the first one loaded wins. Rename one.");
			}
			else
			{
				s_monsterBodies[tr.name] = bd;
				s_anyBodyDeclared = true;
			}
		}
	}

	// --- base_type (validated against the engine's monstertypename[] whitelist) ---
	if ( !in.contains("base_type") || !in["base_type"].is_string() )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/base_type", "", "missing or not a string",
			"one of the " + std::to_string(validMonsterTypeNames().size()) + " base creature types (see monster.schema.json)", "",
			"monster [" + id + "] NOT loaded.");
		return tr;
	}
	tr.baseType = in["base_type"].get<std::string>();
	const int baseIdx = monsterTypeIndex(tr.baseType);
	if ( baseIdx <= 0 || startsWithUnused(tr.baseType) )
	{
		const std::string sug = SAMErrors::suggest(tr.baseType, validMonsterTypeNames());
		SAMErrors::reportSemantic(MOD, fileLabel, "/base_type", tr.baseType, "not a known creature type",
			"one of the " + std::to_string(validMonsterTypeNames().size()) + " base creature types (see monster.schema.json)",
			sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
			"monster [" + id + "] NOT loaded (Barony would silently spawn a 'nothing' monster).");
		return tr;
	}

	// --- build the raw document ---
	json& raw = tr.raw;
	raw["version"] = 1;

	// stats: friendly numeric stats (numeric-validated) + injected identity.
	json stats = json::object();
	if ( in.contains("stats") && in["stats"].is_object() ) { stats = numericOnly(in["stats"], fileLabel, "stats"); }
	stats["name"] = tr.name;
	stats["type"] = tr.baseType;
	if ( in.contains("sex") && in["sex"].is_number_integer() ) { stats["sex"] = in["sex"]; }
	if ( in.contains("appearance") && in["appearance"].is_number_integer() ) { stats["appearance"] = in["appearance"]; }
	raw["stats"] = stats;

	// misc_stats <- friendly "random_stats" (numeric-validated; always emitted).
	raw["misc_stats"] = (in.contains("random_stats") && in["random_stats"].is_object())
		? numericOnly(in["random_stats"], fileLabel, "random_stats") : json::object();

	// proficiencies: validate keys, drop unknown (always emitted).
	json prof = json::object();
	if ( in.contains("proficiencies") && in["proficiencies"].is_object() )
	{
		for ( auto& kv : in["proficiencies"].items() )
		{
			if ( !listHas(kSkillNames, kv.key()) )
			{
				const std::string sug = SAMErrors::suggest(kv.key(), kSkillNames);
				SAMErrors::reportSemantic(MOD, fileLabel, "/proficiencies/" + kv.key(), kv.key(),
					"not a known skill", "a Barony skill name (16 total; see monster.schema.json)",
					sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
					"this skill is skipped.", /*warn=*/true);
			}
			else if ( !kv.value().is_number_integer() )
			{
				SAM_WARN(MOD, "Non-integer value for skill '" + kv.key() + "' in " + fileLabel + " — skill skipped.");
			}
			else { prof[kv.key()] = kv.value(); }
		}
	}
	raw["proficiencies"] = prof;

	// equipped_items: validate slot keys (drop unknown) + item names (warn) (always emitted).
	json equip = json::object();
	if ( in.contains("equipped_items") && in["equipped_items"].is_object() )
	{
		for ( auto& kv : in["equipped_items"].items() )
		{
			if ( !listHas(kSlotNames, kv.key()) )
			{
				const std::string sug = SAMErrors::suggest(kv.key(), kSlotNames);
				SAMErrors::reportSemantic(MOD, fileLabel, "/equipped_items/" + kv.key(), kv.key(),
					"not a known equipment slot",
					"one of weapon, shield, helmet, breastplate, gloves, shoes, cloak, ring, amulet, mask",
					sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
					"this slot is skipped.", /*warn=*/true);
				continue;
			}
			validateItemSlot(kv.value(), fileLabel, "/equipped_items/" + kv.key());
			equip[kv.key()] = kv.value();
		}
	}
	raw["equipped_items"] = equip;

	// inventory_items: validate item names (warn) (always emitted).
	json inv = json::array();
	if ( in.contains("inventory_items") && in["inventory_items"].is_array() )
	{
		inv = in["inventory_items"];
		for ( size_t i = 0; i < inv.size(); ++i )
		{
			validateItemSlot(inv[i], fileLabel, "/inventory_items/" + std::to_string(i));
		}
	}
	raw["inventory_items"] = inv;

	// properties: remap the two renamed keys, pass the rest through (optional).
	if ( in.contains("properties") && in["properties"].is_object() && !in["properties"].empty() )
	{
		json props = json::object();
		for ( auto& kv : in["properties"].items() ) { props[mapPropertyKey(kv.key())] = kv.value(); }
		raw["properties"] = props;
	}

	// followers: always emit both sub-keys; rewrite "namespace:monster" -> filename.
	{
		json fol = json::object();
		int numFollowers = 0;
		json fv = json::object();
		if ( in.contains("followers") && in["followers"].is_object() )
		{
			const json& F = in["followers"];
			if ( F.contains("num_followers") && F["num_followers"].is_number_integer() )
			{
				numFollowers = F["num_followers"].get<int>();
				// The engine runs one spawn per follower per leader at map generation, each
				// re-reading the variant file from disk. No clamp existed anywhere.
				if ( numFollowers < 0 || numFollowers > 8 )
				{
					SAM_WARN(MOD, "Monster '" + tr.name + "' num_followers " + std::to_string(numFollowers)
						+ " is outside 0..8 — clamped.");
					numFollowers = ( numFollowers < 0 ) ? 0 : 8;
				}
			}
			if ( F.contains("follower_variants") && F["follower_variants"].is_object() )
			{
				for ( auto& kv : F["follower_variants"].items() )
				{
					std::string key = kv.key();
					std::string outKey = key;
					if ( key.find(':') != std::string::npos )
					{
						for ( char& c : outKey ) { if ( c == ':' ) { c = '_'; } }
					}
					else if ( key != "default" && key != "none" && !key.empty() )
					{
						SAM_WARN(MOD, "Follower key '" + key + "' in " + fileLabel
							+ " is not namespaced ('namespace:monster') — using it as a raw filename.");
					}
					if ( !kv.value().is_number_integer() )
					{
						SAM_WARN(MOD, "Non-integer weight for follower '" + key + "' in " + fileLabel
							+ " — follower entry skipped.");
						continue;
					}
					fv[outKey] = kv.value();
					followerRefs.push_back(std::make_pair(fileLabel, outKey));
				}
			}
		}
		fol["num_followers"] = numFollowers;
		fol["follower_variants"] = fv;
		raw["followers"] = fol;
	}

	// shopkeeper_properties: pass through (raw keys already match) (optional).
	if ( in.contains("shopkeeper_properties") && in["shopkeeper_properties"].is_object()
		&& !in["shopkeeper_properties"].empty() )
	{
		raw["shopkeeper_properties"] = in["shopkeeper_properties"];
	}

	// SAM-only spawn block (drives monstercurve generation; stripped from raw).
	if ( in.contains("spawn") && in["spawn"].is_array() ) { tr.spawn = in["spawn"]; }

	tr.ok = true;
	return tr;
}

/*-------------------------------------------------------------------------------
	Spawn -> monstercurve.json merge
-------------------------------------------------------------------------------*/
static void mergeSpawn(json& curve, const std::string& variantFile, const std::string& baseType,
	const json& spawnArr, const std::string& fileLabel,
	std::set<std::string>& touchedLevels, bool& curveModified)
{
	for ( const json& e : spawnArr )
	{
		if ( !e.is_object() ) { continue; }

		// Type-checked reads: a mod could give any field the wrong JSON type, and an
		// unguarded nlohmann .value()/get would THROW (type_error) — which, uncaught,
		// unwinds out of Mods::loadMods and terminates the game. Never trust the type.
		const std::string level = (e.contains("level_name") && e["level_name"].is_string())
			? e["level_name"].get<std::string>() : std::string();
		if ( level.empty() )
		{
			SAM_WARN(MOD, "A spawn entry with a missing/empty 'level_name' in " + fileLabel + " was skipped.");
			continue;
		}
		const std::string mode = (e.contains("mode") && e["mode"].is_string())
			? e["mode"].get<std::string>() : std::string("random");
		const std::string species = (e.contains("base_species") && e["base_species"].is_string())
			? e["base_species"].get<std::string>() : baseType;
		const int spIdx = monsterTypeIndex(species);
		if ( spIdx <= 0 || startsWithUnused(species) )
		{
			const std::string sug = SAMErrors::suggest(species, validMonsterTypeNames());
			SAMErrors::reportSemantic(MOD, fileLabel, "/spawn/base_species", species,
				"not a known creature type", "a base creature type",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
				"this spawn entry is skipped.", /*warn=*/true);
			continue;
		}

		auto intField = [&](const char* k, int def) -> int {
			auto it = e.find(k);
			return (it != e.end() && it->is_number_integer()) ? it->get<int>() : def;
		};
		const int wchance = intField("weighted_chance", 1);
		const int dmin = intField("dungeon_depth_minimum", 0);
		const int dmax = intField("dungeon_depth_maximum", 99);
		const int vweight = intField("variant_weight", 1);
		const int dweight = intField("default_weight", 1);
		const std::string arrayKey = (mode == "fixed") ? "fixed_monsters" : "random_generation_monsters";

		json& levels = curve["levels"];
		if ( !levels.contains(level) || !levels[level].is_object() ) { levels[level] = json::object(); }
		json& lvObj = levels[level];
		if ( !lvObj.contains(arrayKey) || !lvObj[arrayKey].is_array() ) { lvObj[arrayKey] = json::array(); }
		json& arr = lvObj[arrayKey];

		json* entry = nullptr;
		for ( json& it : arr )
		{
			if ( it.is_object() && it.contains("name") && it["name"].is_string()
				&& it["name"].get<std::string>() == species ) { entry = &it; break; }
		}
		if ( !entry )
		{
			json ne = json::object();
			ne["name"] = species;
			ne["variants"] = json::object();
			ne["variants"]["default"] = dweight;
			if ( arrayKey == "random_generation_monsters" )
			{
				ne["weighted_chance"] = wchance;
				ne["dungeon_depth_minimum"] = dmin;
				ne["dungeon_depth_maximum"] = dmax;
			}
			arr.push_back(ne);
			entry = &arr.back();
		}
		else
		{
			if ( !entry->contains("variants") || !(*entry)["variants"].is_object() )
			{
				(*entry)["variants"] = json::object();
			}
			if ( !(*entry)["variants"].contains("default") ) { (*entry)["variants"]["default"] = dweight; }
		}
		(*entry)["variants"][variantFile] = vweight;
		touchedLevels.insert(level);
		curveModified = true;
	}
}

/*-------------------------------------------------------------------------------
	SAMMonsters
-------------------------------------------------------------------------------*/
void SAMMonsters::clear()
{
	s_monsterBodies.clear();
	s_variantsById.clear();
	s_anyBodyDeclared = false;
	s_monsterTraits.clear();
	if ( s_mounted )
	{
		PHYSFS_unmount(overlayRealDir().c_str());
		s_mounted = false;
	}
	std::error_code ec;
	fs::remove_all(fs::path(overlayRealDir()), ec); // wipe stale overlay; ignore errors
	s_filesWritten = 0;
	s_declared = 0;
	s_curveLevels = 0;
}

const SAMMonsters::VariantRef* SAMMonsters::variantForId(const std::string& nsColonSlug)
{
	auto it = s_variantsById.find(nsColonSlug);
	return ( it != s_variantsById.end() ) ? &it->second : nullptr;
}

unsigned long long SAMMonsters::traitsForName(const char* variantName)
{
	if ( !variantName || !*variantName || s_monsterTraits.empty() ) { return 0; }
	auto it = s_monsterTraits.find(variantName);
	return ( it != s_monsterTraits.end() ) ? it->second : 0ULL;
}

const SAMMonsters::BodyDef* SAMMonsters::bodyForName(const char* variantName)
{
	if ( !variantName || !*variantName || s_monsterBodies.empty() ) { return nullptr; }
	auto it = s_monsterBodies.find(variantName);
	return ( it != s_monsterBodies.end() ) ? &it->second : nullptr;
}

bool SAMMonsters::anyBodyDeclared()
{
	return s_anyBodyDeclared;
}

void SAMMonsters::reportBodyResolution()
{
	if ( s_monsterBodies.empty() ) { return; }
	for ( const auto& kv : s_monsterBodies )
	{
		const int idx = SAMModels::modelIndexForId(kv.second.model);
		if ( idx >= 0 )
		{
			SAM_INFO(MOD, "Monster '" + kv.first + "' body model '" + kv.second.model
				+ "' -> model index " + std::to_string(idx) + ".");
		}
		else
		{
			SAM_WARN(MOD, "Monster '" + kv.first + "' declares body model '" + kv.second.model
				+ "', which is not a registered model. It will render as its base creature. "
				  "Declare the .vox in this mod's \"models\" list with that exact id.");
		}
	}
}

void SAMMonsters::applyAll(const std::vector<SAMModManifest>& mods)
{
	// Start clean — never compound our own previous overlay, and read the TRUE
	// current base monstercurve (unmounted) below.
	clear();

	// Load any currently-resolved base monstercurve.json to merge on top of
	// (vanilla ships none; a mod may). Overlay is unmounted, so this is the real base.
	json curve = json::object();
	{
		const char* realDir = PHYSFS_getRealDir("data/monstercurve.json");
		if ( realDir )
		{
			std::string text;
			if ( readWholeFile(joinPath(realDir, "data/monstercurve.json"), text) )
			{
				json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
				if ( !parsed.is_discarded() && parsed.is_object() ) { curve = parsed; }
			}
		}
	}
	if ( !curve.contains("version") ) { curve["version"] = 1; }
	if ( !curve.contains("levels") || !curve["levels"].is_object() ) { curve["levels"] = json::object(); }

	std::set<std::string> baseLevels;
	for ( auto& lv : curve["levels"].items() ) { baseLevels.insert(lv.key()); }

	std::set<std::string> writtenVariants;
	std::set<std::string> touchedLevels;
	std::vector<std::pair<std::string, std::string>> followerRefs;
	bool curveModified = false;

	for ( const SAMModManifest& m : mods )
	{
		for ( const std::string& rel : m.monsters )
		{
			const std::string path = joinPath(m.modPath, rel);
			const std::string fileLabel = SAMErrors::displayFile(m.ns, rel);
			try
			{
			std::string text;
			if ( !readWholeFile(path, text) )
			{
				SAM_ERROR(MOD, "Monster file not found: " + path + " (declared by [" + m.ns + "]) — skipped.");
				continue;
			}
			++s_declared;

			json j;
			try
			{
				j = json::parse(text);
			}
			catch ( const json::parse_error& e )
			{
				SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "monster not loaded.");
				continue;
			}
			if ( !j.is_object() )
			{
				SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
					"a JSON object: { ... }", "wrap the file contents in { }", "monster not loaded.");
				continue;
			}

			Translated tr = translateMonster(j, m.ns, fileLabel, followerRefs);
			if ( !tr.ok ) { continue; }

			const std::string merged = tr.raw.dump();
			if ( merged.size() >= SAFE_MAX )
			{
				SAM_ERROR(MOD, "Monster [" + m.ns + ":" + tr.slug + "] is " + std::to_string(merged.size())
					+ " bytes — too large for Barony's 64KB read buffer; skipped. Trim its equipment/inventory.");
				continue;
			}
			const std::string outPath = joinPath(overlayRealDir(), "data/custom-monsters/" + tr.variantFile + ".json");
			std::error_code ec;
			fs::create_directories(fs::path(outPath).parent_path(), ec);
			std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
			if ( !out.is_open() )
			{
				SAM_ERROR(MOD, "Could not write monster overlay file at " + outPath + " — [" + m.ns + ":" + tr.slug + "] skipped.");
				continue;
			}
			out << merged;
			out.close();

			++s_filesWritten;
			writtenVariants.insert(tr.variantFile);
			// Record it by ID as well, so sam_spawn_monster("ns:slug") can find it. Done
			// here rather than at parse time so an id only ever resolves to a file that
			// actually reached disk.
			{
				SAMMonsters::VariantRef ref;
				ref.variantFile = tr.variantFile;
				ref.baseType    = tr.baseType;
				ref.displayName = tr.name;
				s_variantsById[m.ns + ":" + tr.slug] = ref;
			}
			SAM_INFO(MOD, "Registered monster: " + tr.name + " [" + m.ns + ":" + tr.slug + "] — variant of "
				+ tr.baseType + " -> data/custom-monsters/" + tr.variantFile + ".json");

			if ( tr.spawn.is_array() && !tr.spawn.empty() )
			{
				mergeSpawn(curve, tr.variantFile, tr.baseType, tr.spawn, fileLabel, touchedLevels, curveModified);
			}
			else
			{
				SAM_WARN(MOD, "Monster [" + m.ns + ":" + tr.slug + "] declares no 'spawn' — it will only appear "
					"via followers, map-editor placement, or the console (not random dungeon generation).");
			}
			}
			catch ( const json::exception& e )
			{
				SAM_ERROR(MOD, std::string("Unexpected JSON error processing ") + fileLabel + " ("
					+ e.what() + ") — monster skipped.");
			}
		}
	}

	// Warn on follower references that resolve to no loaded monster.
	for ( const auto& fr : followerRefs )
	{
		const std::string& key = fr.second;
		if ( key == "default" || key == "none" || key.empty() ) { continue; }
		if ( !writtenVariants.count(key) )
		{
			SAM_WARN(MOD, "Follower reference '" + key + "' (in " + fr.first
				+ ") does not match any loaded S.A.M monster — that follower will be skipped in-game.");
		}
	}

	s_curveLevels = static_cast<int>(touchedLevels.size());

	// Write the merged curve (if any spawns landed) and warn about SAM-managed levels.
	if ( curveModified )
	{
		const std::string mergedCurve = curve.dump();
		if ( mergedCurve.size() >= SAFE_MAX )
		{
			SAM_ERROR(MOD, "Merged monstercurve.json is " + std::to_string(mergedCurve.size())
				+ " bytes — too large for Barony's 64KB read buffer; custom monsters will NOT spawn via the "
				"curve this load. Reduce spawn entries.");
		}
		else
		{
			const std::string outPath = joinPath(overlayRealDir(), "data/monstercurve.json");
			std::error_code ec;
			fs::create_directories(fs::path(outPath).parent_path(), ec);
			std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
			if ( out.is_open() )
			{
				out << mergedCurve;
				out.close();
				for ( const std::string& level : touchedLevels )
				{
					if ( !baseLevels.count(level) )
					{
						SAM_WARN(MOD, "Level '" + level + "' now uses a S.A.M-managed random spawn table — only "
							"species listed by loaded mods spawn there (vanilla versions still appear via each "
							"species' 'default' weight). Levels you don't target are unaffected.");
					}
				}
			}
			else
			{
				SAM_ERROR(MOD, "Could not write monstercurve overlay at " + outPath
					+ " — custom monsters will NOT spawn via the curve this load.");
			}
		}
	}

	// Prepend-mount the overlay so Barony's lazy custom-monster / monstercurve
	// reads (at map generation, long after this hook) resolve our files.
	if ( s_filesWritten > 0 || curveModified )
	{
		if ( PHYSFS_mount(overlayRealDir().c_str(), NULL, 0) )
		{
			s_mounted = true;
			SAM_INFO(MOD, "Mounted monster overlay (" + std::to_string(s_filesWritten) + " monster file(s) across "
				+ std::to_string(s_curveLevels) + " spawn level(s)).");
		}
		else
		{
			SAM_ERROR(MOD, std::string("Failed to mount monster overlay: ")
				+ PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()) + " — custom monsters will NOT take effect this load.");
		}
	}
}

int SAMMonsters::count() { return s_filesWritten; }
int SAMMonsters::declared() { return s_declared; }
int SAMMonsters::curveLevels() { return s_curveLevels; }
