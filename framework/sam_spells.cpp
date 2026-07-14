/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_spells.cpp
	Desc: implementation of the custom-spell registry (Session 1 — loader only).

	Parses + validates spell JSON (spell.schema.json) and assigns each a stable
	runtime id (>= SAM_SPELL_ID_BASE = 2000) + a "namespace:spell" lookup. This half
	is engine-decoupled — it touches no Barony types — but is compiled into the game
	build only (a later session adds the in-engine spell_t construction that needs
	magic.hpp). Nothing here builds a castable spell yet.

-------------------------------------------------------------------------------*/

// Barony headers (pulled in transitively via the shared logger) can drag in
// <windows.h>; stop it defining min()/max() macros that would break nlohmann/std.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_spells.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include <fstream>
#include <sstream>
#include <vector>

using nlohmann::json;

static const char* MOD = "SPELLS";

/*-------------------------------------------------------------------------------
	Registry storage (engine-decoupled)
-------------------------------------------------------------------------------*/
static std::map<int, SAMSpellDef> s_registry;
static int s_nextSpellId = SAM_SPELL_ID_BASE;

static bool readWholeFile(const std::string& path, std::string& out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if ( !f.is_open() ) { return false; }
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

static std::string joinPath(const std::string& dir, const std::string& file)
{
	if ( dir.empty() ) { return file; }
	const char back = dir.back();
	return ( back == '/' || back == '\\' ) ? (dir + file) : (dir + "/" + file);
}

// The spell.schema.json "payload" enum — used only to warn on an unknown value
// (Session 1 still registers it; the payload isn't consumed until casting lands).
static const std::vector<std::string>& payloadNames()
{
	static const std::vector<std::string> v = {
		"force", "fire", "lightning", "cold", "magic_missile", "poison", "slow",
		"confuse", "sleep", "dig", "dominate", "charm_monster", "acid_spray", "bleed",
		"ghost_bolt", "stoneblood", "locking", "opening", "tele_pull", "steal_weapon", "drain_soul"
	};
	return v;
}

static bool listHas(const std::vector<std::string>& v, const std::string& s)
{
	for ( const std::string& x : v ) { if ( x == s ) { return true; } }
	return false;
}

/*-------------------------------------------------------------------------------
	SAMSpells
-------------------------------------------------------------------------------*/
void SAMSpells::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const std::string& relPath : manifest.spells )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "Spell file not found: " + path + " (declared by [" + manifest.ns + "])");
			continue;
		}

		const std::string fileLabel = SAMErrors::displayFile(manifest.ns, relPath);
		json j;
		try
		{
			j = json::parse(text);
		}
		catch ( const json::parse_error& e )
		{
			SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "spell not loaded.");
			continue;
		}
		if ( !j.is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
				"a JSON object: { ... }", "wrap the file contents in { }", "spell not loaded.");
			continue;
		}

		auto getStr = [&](const char* k) -> std::string {
			auto it = j.find(k);
			return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
		};
		auto getInt = [&](const char* k, int dv) -> int {
			auto it = j.find(k);
			return (it != j.end() && it->is_number()) ? it->get<int>() : dv;
		};
		auto getBool = [&](const char* k, bool dv) -> bool {
			auto it = j.find(k);
			return (it != j.end() && it->is_boolean()) ? it->get<bool>() : dv;
		};

		SAMSpellDef def;
		def.id = getStr("id");
		def.name = getStr("name");
		def.modNamespace = manifest.ns;
		def.modPath = manifest.modPath;

		if ( def.id.empty() || def.id.find(':') == std::string::npos )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/id", def.id, "missing or malformed (required)",
				"an id in \"namespace:spell\" form, e.g. \"" + manifest.ns + ":shadow_bolt\"",
				"add an \"id\" field", "spell not loaded.");
			continue;
		}
		if ( def.name.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/name", "", "missing (required)",
				"the display name, e.g. \"Shadow Bolt\"", "add a \"name\" field",
				"spell [" + def.id + "] not loaded.");
			continue;
		}

		def.description   = getStr("description");
		def.manaCost      = getInt("mana_cost", 1);
		def.projectileType = getStr("projectile_type"); if ( def.projectileType.empty() ) { def.projectileType = "missile"; }
		def.payload       = getStr("payload");
		def.damageMin     = getInt("damage_min", 0);
		def.damageMax     = getInt("damage_max", 0);
		def.range         = getInt("range", 0);
		def.speed         = getInt("speed", 0);
		def.onHitEffect   = getStr("on_hit_effect");
		def.onHitDuration = getInt("on_hit_duration", 0);
		def.onHitChance   = getInt("on_hit_chance", 0);
		def.icon          = getStr("icon");
		def.startingSpell = getBool("starting_spell", false);

		if ( def.payload.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/payload", "", "missing (required)",
				"a payload like \"force\" or \"fire\"", "add a \"payload\" field",
				"spell [" + def.id + "] not loaded.");
			continue;
		}
		if ( !listHas(payloadNames(), def.payload) )
		{
			const std::string sug = SAMErrors::suggest(def.payload, payloadNames());
			SAMErrors::reportSemantic(MOD, fileLabel, "/payload", def.payload, "not a known payload",
				"one of force/fire/lightning/cold/... (see spell.schema.json)",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
				"registered anyway; it will do nothing until casting is implemented.", /*warn=*/true);
		}

		const int id = s_nextSpellId++;
		def.numericId = id;
		s_registry[id] = def;

		SAM_INFO(MOD, "Registering spell: " + def.name + " [" + def.id + "] -> runtime id " + std::to_string(id));
		SAM_DEBUG(MOD, "  payload " + def.payload + ", projectile " + def.projectileType
			+ ", mana " + std::to_string(def.manaCost) + ", dmg " + std::to_string(def.damageMin)
			+ "-" + std::to_string(def.damageMax) + (def.startingSpell ? ", starting_spell" : ""));
	}
}

void SAMSpells::clear()
{
	s_registry.clear();
	s_nextSpellId = SAM_SPELL_ID_BASE;
}

int SAMSpells::count()
{
	return static_cast<int>(s_registry.size());
}

const SAMSpellDef* SAMSpells::getSpell(int spellId)
{
	auto it = s_registry.find(spellId);
	return (it != s_registry.end()) ? &it->second : nullptr;
}

const SAMSpellDef* SAMSpells::getSpellByName(const std::string& namespacedId)
{
	for ( const auto& kv : s_registry )
	{
		if ( kv.second.id == namespacedId )
		{
			return &kv.second;
		}
	}
	return nullptr;
}
