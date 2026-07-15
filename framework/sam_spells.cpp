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

// Session 2: the engine-spell builder needs Barony's magic system. sam_spells.cpp is
// GAME_SOURCES only, so these are always available.
#include "main.hpp"          // list_t/node_t, list_AddNodeLast/FreeAll/RemoveNode
#include "magic/magic.hpp"   // spell_t, spellConstructor, copySpellElement, allGameSpells, spellElement_* globals
#include "mod_tools.hpp"     // ItemTooltips.spellItems / spellNameStringToSpellID

#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cctype>

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

static std::string toLowerStr(std::string s)
{
	for ( char& c : s ) { c = (char)std::tolower((unsigned char)c); }
	return s;
}

// Map a schema payload string to the global spellElement_t template Barony copies into a
// spell's payload node (the engine matches on element_internal_name at cast time). Returns
// nullptr for an unknown payload (spell is still built, just with no payload = a dud bolt).
static spellElement_t* samPayloadElement(const std::string& payload)
{
	if ( payload == "force" )         { return &spellElement_force; }
	if ( payload == "fire" )          { return &spellElement_fire; }
	if ( payload == "lightning" )     { return &spellElement_lightning; }
	if ( payload == "cold" )          { return &spellElement_cold; }
	if ( payload == "magic_missile" ) { return &spellElement_magicmissile; }
	if ( payload == "poison" )        { return &spellElement_poison; }
	if ( payload == "slow" )          { return &spellElement_slow; }
	if ( payload == "confuse" )       { return &spellElement_confuse; }
	if ( payload == "sleep" )         { return &spellElement_sleep; }
	if ( payload == "dig" )           { return &spellElement_dig; }
	if ( payload == "dominate" )      { return &spellElement_dominate; }
	if ( payload == "charm_monster" ) { return &spellElement_charmMonster; }
	if ( payload == "acid_spray" )    { return &spellElement_acidSpray; }
	if ( payload == "bleed" )         { return &spellElement_bleed; }
	if ( payload == "ghost_bolt" )    { return &spellElement_ghostBolt; }
	if ( payload == "stoneblood" )    { return &spellElement_stoneblood; }
	if ( payload == "locking" )       { return &spellElement_locking; }
	if ( payload == "opening" )       { return &spellElement_opening; }
	if ( payload == "tele_pull" )     { return &spellElement_telePull; }
	if ( payload == "steal_weapon" )  { return &spellElement_stealWeapon; }
	if ( payload == "drain_soul" )    { return &spellElement_drainSoul; }
	return nullptr;
}

// "sam_test:shadow_bolt" -> "spell_sam_sam_test_shadow_bolt" (a unique <64-char internal
// name; the engine + tooltip templates key off spell_internal_name / spellItems internalName).
static std::string samSpellInternalName(const std::string& id)
{
	std::string s = "spell_sam_" + id;
	for ( char& c : s ) { if ( c == ':' || c == '/' || c == '\\' || c == '.' || c == ' ' ) { c = '_'; } }
	if ( s.size() > 63 ) { s.resize(63); }
	return s;
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

		// Session 2: build the real in-engine spell so it's grantable + castable + in the UI.
		buildEngineSpell(s_registry[id]);
	}
}

void SAMSpells::clear()
{
	removeEngineSpells(); // free + unregister the engine spell_t's first (before we lose the ids)
	s_registry.clear();
	s_nextSpellId = SAM_SPELL_ID_BASE;
}

void SAMSpells::buildEngineSpell(const SAMSpellDef& def)
{
	const int id = def.numericId;
	if ( id < SAM_SPELL_ID_BASE ) { return; }
	if ( allGameSpells.find(id) != allGameSpells.end() ) { return; } // already built this cycle

	const std::string internalName = samSpellInternalName(def.id);

	// 1) Build the spell_t: a propulsion root element with one payload child (the shape
	//    castSpell/actmagic expect for a projectile spell).
	spell_t* spell = (spell_t*)malloc(sizeof(spell_t));
	if ( !spell ) { SAM_ERROR(MOD, "buildEngineSpell: malloc failed for " + def.id); return; }
	spellConstructor(spell, id); // zero-init + allGameSpells[id] = spell
	spell->needsDataFreed = 1;
	strncpy(spell->spell_internal_name, internalName.c_str(), sizeof(spell->spell_internal_name) - 1);
	spell->spell_internal_name[sizeof(spell->spell_internal_name) - 1] = '\0';
	spell->difficulty = 0; // learnable by any caster (grants use ignoreSkill anyway)
	spell->mana = (def.manaCost < 0 ? 0 : def.manaCost);
	spell->skillID = PRO_SORCERY;

	const bool trio = (def.projectileType == "missile_trio");
	const bool selfCast = (def.projectileType == "none");

	// Root (propulsion). Even a "none"/self spell gets a missile root for now so it casts.
	spellElement_t* rootGlobal = trio ? &spellElement_missile_trio : &spellElement_missile;
	node_t* node = list_AddNodeLast(&spell->elements);
	node->element = copySpellElement(rootGlobal);
	node->size = sizeof(spellElement_t);
	node->deconstructor = &spellElementDeconstructor;
	spellElement_t* root = (spellElement_t*)node->element;
	root->node = node;

	// Payload child (drives on-hit behavior + bolt sprite; damage overridden from the def).
	spellElement_t* payloadGlobal = samPayloadElement(def.payload);
	if ( payloadGlobal )
	{
		node = list_AddNodeLast(&root->elements);
		node->element = copySpellElement(payloadGlobal);
		node->size = sizeof(spellElement_t);
		node->deconstructor = &spellElementDeconstructor;
		spellElement_t* payload = (spellElement_t*)node->element;
		payload->node = node;
		if ( def.damageMax > 0 ) { payload->setDamage(def.damageMax); }
	}
	else
	{
		SAM_WARN(MOD, "buildEngineSpell: unknown payload '" + def.payload + "' for " + def.id + " (bolt will be inert).");
	}
	(void)selfCast;

	// 2) Tooltip/UI metadata so the spell lists with its name + a sane tooltip. spellItem_t
	//    is a private nested type, so we edit the map's default-inserted value in place.
	auto& si = ItemTooltips.spellItems[id];
	si.id = id;
	si.internalName = internalName;
	si.name = def.name;
	si.name_lowercase = toLowerStr(def.name);
	si.mana = spell->mana;
	si.skillID = PRO_SORCERY;
	si.difficulty = 0;
	si.damage = def.damageMax;
	si.spellType = trio ? ItemTooltips_t::SPELL_TYPE_PROJECTILE_SHORT_X3
	                    : (selfCast ? ItemTooltips_t::SPELL_TYPE_SELF : ItemTooltips_t::SPELL_TYPE_PROJECTILE);
	si.spellTags.clear();
	if ( def.damageMax > 0 || def.damageMin > 0 ) { si.spellTags.insert(ItemTooltips_t::SPELL_TAG_DAMAGE); }
	if ( !def.onHitEffect.empty() ) { si.spellTags.insert(ItemTooltips_t::SPELL_TAG_STATUS_EFFECT); }
	ItemTooltips.spellNameStringToSpellID[internalName] = id;

	SAM_INFO(MOD, "Built spell_t for " + def.name + " -> allGameSpells[" + std::to_string(id) + "]");
}

void SAMSpells::buildAllEngineSpells()
{
	for ( const auto& kv : s_registry ) { buildEngineSpell(kv.second); }
}

bool SAMSpells::grantCustomSpell(int player, const std::string& namespacedId)
{
	const SAMSpellDef* def = getSpellByName(namespacedId);
	if ( !def )
	{
		SAM_ERROR(MOD, "grant: unknown custom spell '" + namespacedId + "'.");
		return false;
	}
	if ( allGameSpells.find(def->numericId) == allGameSpells.end() )
	{
		SAM_ERROR(MOD, "grant: engine spell not built for '" + namespacedId + "' (id "
			+ std::to_string(def->numericId) + ") — cannot grant.");
		return false;
	}
	const bool ok = addSpell(def->numericId, player, true);
	if ( ok )
	{
		SAM_INFO(MOD, "Granted custom spell " + namespacedId + " to player " + std::to_string(player));
	}
	else
	{
		SAM_INFO(MOD, "Custom spell " + namespacedId + " not granted to player " + std::to_string(player)
			+ " (already known or non-local player).");
	}
	return ok;
}

void SAMSpells::removeEngineSpells()
{
	for ( const auto& kv : s_registry )
	{
		const int id = kv.first;
		auto it = allGameSpells.find(id);
		if ( it != allGameSpells.end() )
		{
			spell_t* spell = it->second;
			if ( spell )
			{
				if ( spell->sustain_node ) { list_RemoveNode(spell->sustain_node); }
				list_FreeAll(&spell->elements);
				if ( spell->needsDataFreed ) { free(spell); }
			}
			allGameSpells.erase(it);
		}
		ItemTooltips.spellItems.erase(id);
		ItemTooltips.spellNameStringToSpellID.erase(samSpellInternalName(kv.second.id));
	}
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

std::string SAMSpells::getIconPath(int spellId)
{
	const SAMSpellDef* def = getSpell(spellId);
	if ( !def || def->icon.empty() ) { return std::string(); }
	// Path-traversal guard: def->icon is untrusted mod JSON; refuse to resolve a
	// path that escapes the mod folder (fall back to no custom icon).
	if ( SAMErrors::relPathEscapes(def->icon) ) { return std::string(); }
	const std::string abs = joinPath(def->modPath, def->icon);
	std::ifstream f(abs.c_str(), std::ios::binary);
	if ( !f.good() ) { return std::string(); }
	return abs;
}
