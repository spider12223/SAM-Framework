/*-------------------------------------------------------------------------------
	S.A.M Framework — Custom playable-race registry + application.
	See sam_races.hpp for the design + the graceful-degrade rule.

	IRON no-op rule: with no race mod loaded the registry is empty, so any() is
	false and every hook returns before touching game state — vanilla behaviour is
	byte-identical. A vanilla/mismatched client reading a 200-255 race id maps to
	HUMAN via getMonsterFromPlayerRace's own default, so it degrades to Human.
-------------------------------------------------------------------------------*/

#include "sam_races.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include "main.hpp"     // umbrella — provides decls stat.hpp needs
#include "stat.hpp"     // Stat members (STR..CHR, HP/MP, playerRace)
#include "monster.hpp"  // Monster enum, monstertypename[], NUMMONSTERS
#include "player.hpp"   // players[], MAXPLAYERS (applySpells)
#include "net.hpp"      // multiplayer, CLIENT, intro
#include "mod_tools.hpp" // ItemTooltips.spellItems (vanilla spell-name resolve)
#include "magic/magic.hpp" // addSpell
#include "sam_spells.hpp"  // grant custom "ns:spell" innate race spells
#include "sam_models.hpp"  // SAMModels::modelIndexForId (resolve a race's limb models)

#include <algorithm>
#include <set>
#include <fstream>
#include <sstream>
#include <map>
#include <cctype>

using nlohmann::json;
static const char* MOD = "RACES";

namespace
{
	std::string joinPath(const std::string& dir, const std::string& file)
	{
		if ( dir.empty() ) { return file; }
		if ( file.empty() ) { return dir; }
		const char last = dir[dir.size() - 1];
		if ( last == '/' || last == '\\' ) { return dir + file; }
		return dir + "/" + file;
	}
	bool readWholeFile(const std::string& path, std::string& out)
	{
		std::ifstream f(path.c_str(), std::ios::binary);
		if ( !f.good() ) { return false; }
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}

	// The 18 monster bodies that back the existing playable races — each has a
	// dedicated first-person arm, so a SAM race riding one is correct in BOTH the
	// 3rd-person body and the 1st-person view.
	bool isSupportedHostBody(int monster)
	{
		switch ( monster )
		{
			case HUMAN: case SKELETON: case VAMPIRE: case SUCCUBUS: case INCUBUS:
			case GOBLIN: case AUTOMATON: case INSECTOID: case GOATMAN: case GNOME:
			case GREMLIN: case DRYAD: case MYCONID: case SALAMANDER: case TROLL:
			case SPIDER: case CREATURE_IMP: case RAT:
				return true;
			default:
				return false;
		}
	}
	// Map a monster-type name (case-insensitive) to its Monster enum value, or -1.
	int monsterFromName(const std::string& nameIn)
	{
		std::string want = nameIn;
		for ( char& c : want ) { c = (char)std::tolower((unsigned char)c); }
		for ( int i = 0; i < NUMMONSTERS; ++i ) { if ( want == monstertypename[i] ) { return i; } }
		return -1;
	}

	// The five limbs the player draw path can be told to replace, and the JSON name for
	// each. "head" is deliberately absent: the engine sets the head on the player entity
	// itself, not through setDefaultPlayerModel, so it is carried separately.
	struct LimbSlot { const char* key; int limbType; };
	const LimbSlot kLimbSlots[] = {
		{ "torso",     LIMB_HUMANOID_TORSO    },
		{ "leg_right", LIMB_HUMANOID_RIGHTLEG },
		{ "leg_left",  LIMB_HUMANOID_LEFTLEG  },
		{ "arm_right", LIMB_HUMANOID_RIGHTARM },
		{ "arm_left",  LIMB_HUMANOID_LEFTARM  },
	};

	// "head" is not in kLimbSlots (the engine sets it outside the limb path), so the
	// accepted-name check has to include it explicitly or a valid file would be rejected.
	bool isKnownLimbSlot(const std::string& key)
	{
		if ( key == "head" ) { return true; }
		for ( const LimbSlot& s : kLimbSlots ) { if ( key == s.key ) { return true; } }
		return false;
	}

	// Every model index any registered race uses as a head. Rebuilt by resolveLimbModels
	// and consulted by isRaceHeadSprite; empty in vanilla.
	std::set<int> s_raceHeadSprites;

	// Registry — EMPTY in vanilla (the whole no-op guarantee).
	std::map<int, SAMRaceDef> s_byId;            // runtime id 200..255 -> def
	std::map<std::string, int> s_byIdString;      // "ns:race" -> id
	int s_nextId = SAM_RACE_ID_BASE;
}

void SAMRaces::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const std::string& relPath : manifest.races )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "Race file not found: " + path + " (declared by [" + manifest.ns + "])");
			continue;
		}
		const std::string fileLabel = SAMErrors::displayFile(manifest.ns, relPath);
		json j;
		try { j = json::parse(text); }
		catch ( const json::parse_error& e )
		{
			SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "race not loaded.");
			continue;
		}
		if ( !j.is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
				"a JSON object: { ... }", "wrap the file contents in { }", "race not loaded.");
			continue;
		}

		auto getStr = [&](const char* k) -> std::string {
			auto it = j.find(k);
			return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
		};
		auto getBool = [&](const char* k, bool dv) -> bool {
			auto it = j.find(k);
			return (it != j.end() && it->is_boolean()) ? it->get<bool>() : dv;
		};

		SAMRaceDef def;
		def.id = getStr("id");
		if ( def.id.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/id", "", "missing (required)",
				"an id in \"namespace:race\" form, e.g. \"" + manifest.ns + ":frostborn\"",
				"add an \"id\" field", "race not loaded.");
			continue;
		}
		if ( s_byIdString.count(def.id) )
		{
			SAM_WARN(MOD, "Duplicate race id '" + def.id + "' — keeping the first, skipping this one.");
			continue;
		}
		if ( s_nextId > 255 )
		{
			SAM_ERROR(MOD, "Custom-race slots full (" + std::to_string(256 - SAM_RACE_ID_BASE)
				+ " max) — skipping '" + def.id + "'.");
			continue;
		}

		def.name = getStr("name").empty() ? def.id : getStr("name");
		def.description = getStr("description");
		def.modNamespace = manifest.ns;

		// host_body: the existing monster whose model this race wears. Restricted to
		// the 18 bodies with a proper first-person arm; anything else falls to HUMAN.
		const std::string hostName = getStr("host_body");
		int host = hostName.empty() ? (int)HUMAN : monsterFromName(hostName);
		if ( host < 0 || !isSupportedHostBody(host) )
		{
			SAM_WARN(MOD, "Race '" + def.id + "' host_body '" + hostName
				+ "' is not a supported player body — falling back to HUMAN.");
			host = (int)HUMAN;
		}
		def.hostMonster = host;
		def.hostBodyName = (host >= 0 && host < NUMMONSTERS) ? monstertypename[host] : "human";

		// stat_modifiers: { "STR": 2, "DEX": -1, "HP": 10, ... } — deltas.
		auto sm = j.find("stat_modifiers");
		if ( sm != j.end() && sm->is_object() )
		{
			auto rd = [&](const char* k, int& out) {
				auto v = sm->find(k);
				if ( v != sm->end() && v->is_number() ) { out = v->get<int>(); }
			};
			rd("STR", def.str); rd("DEX", def.dex); rd("CON", def.con);
			rd("INT", def.intel); rd("PER", def.per); rd("CHR", def.chr);
			rd("HP", def.hp); rd("MP", def.mp);
		}
		def.bloodDiet = getBool("blood_diet", false);

		// allies / enemies: monster-type names, resolved to Monster enum values now so the
		// hot path (checkEnemy, once per entity pair) never touches a string. An unknown
		// name is reported and dropped rather than silently ignored -- a typo'd "goatmen"
		// would otherwise look exactly like a race whose allegiance quietly does nothing.
		auto readMonsterList = [&](const char* key, std::vector<int>& out) {
			auto arr = j.find(key);
			if ( arr == j.end() || !arr->is_array() ) { return; }
			for ( const json& e : *arr )
			{
				if ( !e.is_string() )
				{
					SAMErrors::reportSemantic(MOD, fileLabel, std::string("/") + key, e.dump(),
						"not a string", "a quoted monster name, e.g. \"goatman\"",
						"put quotes around it",
						"that entry ignored; the rest of the race loaded.", true);
					continue;
				}
				const std::string nm = e.get<std::string>();
				const int m = monsterFromName(nm);
				if ( m <= 0 )   // 0 is "nothing", which is not a creature
				{
					SAMErrors::reportSemantic(MOD, fileLabel, std::string("/") + key, nm,
						"not a monster type",
						"a monster name such as \"goatman\", \"gnome\" or \"shopkeeper\"",
						"check the spelling against the monster list in the docs",
						"that entry ignored; the rest of the race loaded.", true);
					continue;
				}
				out.push_back(m);
			}
		};
		// limb_models: kept as written and resolved later — the model table does not exist
		// during mod load, so an index cannot be validated or looked up here.
		auto lm = j.find("limb_models");
		if ( lm != j.end() && lm->is_object() )
		{
			for ( auto it = lm->begin(); it != lm->end(); ++it )
			{
				// A slot name the draw path never asks for would sit in the map doing
				// nothing, which is indistinguishable from a race that declared no body at
				// all. Say so instead: "arm-right" and "rightArm" are the obvious typos and
				// neither is a JSON error, so nothing else would ever catch them.
				if ( !isKnownLimbSlot(it.key()) )
				{
					SAMErrors::reportSemantic(MOD, fileLabel, "/limb_models/" + it.key(), "",
						"not a limb this race can replace",
						"one of: head, torso, arm_right, arm_left, leg_right, leg_left",
						"check the spelling", "that entry ignored; the rest of the race loaded.", true);
					continue;
				}
				if ( !it.value().is_string() )
				{
					// A bare 1025 rather than "1025" is the easy mistake, and the schema
					// only catches it for people using the builder.
					SAMErrors::reportSemantic(MOD, fileLabel, "/limb_models/" + it.key(),
						it.value().dump(), "not a string",
						"a quoted model reference, e.g. \"1025\" or \"" + manifest.ns + ":body\"",
						"put quotes around it",
						"that limb keeps the host body's model.", true);
					continue;
				}
				if ( it.value().get<std::string>().empty() ) { continue; }
				def.limbModels[it.key()] = it.value().get<std::string>();
			}
		}

		readMonsterList("allies", def.allies);
		readMonsterList("enemies", def.enemies);

		// Declaring both is a contradiction the author needs to resolve, not something to
		// resolve silently. enemies wins (hostility is the more consequential reading of
		// an ambiguous file), and we say so.
		for ( int m : def.enemies )
		{
			if ( std::find(def.allies.begin(), def.allies.end(), m) != def.allies.end() )
			{
				SAM_WARN(MOD, "Race '" + def.id + "' lists '" + std::string(monstertypename[m])
					+ "' as BOTH an ally and an enemy — treating it as an enemy.");
			}
		}
		if ( j.contains("starting_spells") && j["starting_spells"].is_array() )
		{
			for ( const json& e : j["starting_spells"] )
			{
				if ( e.is_string() ) { def.startingSpells.push_back(e.get<std::string>()); }
			}
		}

		def.numericId = s_nextId++;
		s_byId[def.numericId] = def;
		s_byIdString[def.id] = def.numericId;
		SAM_INFO(MOD, "Registered race: " + def.name + " [" + def.id + "] -> id "
			+ std::to_string(def.numericId) + " on body " + def.hostBodyName
			+ " (STR " + std::to_string(def.str) + " DEX " + std::to_string(def.dex)
			+ " CON " + std::to_string(def.con) + " INT " + std::to_string(def.intel)
			+ " PER " + std::to_string(def.per) + " CHR " + std::to_string(def.chr)
			+ " HP " + std::to_string(def.hp) + " MP " + std::to_string(def.mp) + ")");
		if ( !def.allies.empty() || !def.enemies.empty() )
		{
			std::string line = "  " + def.name + " allegiance:";
			if ( !def.allies.empty() )
			{
				line += " allied with";
				for ( int m : def.allies ) { line += " " + std::string(monstertypename[m]); }
			}
			if ( !def.enemies.empty() )
			{
				line += " hostile to";
				for ( int m : def.enemies ) { line += " " + std::string(monstertypename[m]); }
			}
			SAM_INFO(MOD, line);
		}
	}
}

namespace
{
	// Resolve one model reference three ways, in the order a modder expects: a model this
	// mod declared, then a mod-relative .vox path (which registration turned into an id),
	// then a raw vanilla index.
	//
	// A bad reference is REPORTED, never silently dropped. Both failure modes here are
	// invisible in game -- opengl.cpp draws index 0 and out-of-range indices as nothing at
	// all, with no log line and nothing in /sam_models, because no model was ever
	// registered -- so the message is the only thing standing between a modder and an
	// afternoon of wondering why their arm vanished.
	int resolveModelRef(const std::string& ref, const std::string& raceId, const std::string& slot)
	{
		int idx = SAMModels::modelIndexForId(ref);
		if ( idx >= 0 ) { return idx; }

		char* end = nullptr;
		const long n = std::strtol(ref.c_str(), &end, 10);
		if ( end && *end == '\0' )
		{
			if ( n > 0 && n < (long)nummodels ) { return (int)n; }
			SAM_ERROR(MOD, "Race [" + raceId + "] limb_models." + slot + " is model index "
				+ std::to_string(n) + ", which is "
				+ ( n == 0 ? std::string("models/system/null.vox, the engine's EMPTY model")
				           : std::string("past the end of the table (this game has ")
				             + std::to_string((long long)nummodels) + " models, 1.."
				             + std::to_string((long long)nummodels - 1) + ")" )
				+ ". That limb would draw as nothing, so it is ignored."
				" Remember models.txt line N is index N-1.");
			return -1;
		}

		SAM_WARN(MOD, "Race [" + raceId + "] limb_models." + slot + " '" + ref
			+ "' is not a registered model — ignoring it (that limb keeps the host body's"
			" own model). Declare it in mod.json \"models\", or use a raw vanilla index.");
		return -1;
	}
}

void SAMRaces::resolveLimbModels()
{
	s_raceHeadSprites.clear();
	for ( auto& kv : s_byId )
	{
		SAMRaceDef& def = kv.second;
		def.limbModelIdx.clear();
		def.headModelIdx = -1;
		if ( def.limbModels.empty() ) { continue; }

		for ( const LimbSlot& slot : kLimbSlots )
		{
			auto it = def.limbModels.find(slot.key);
			if ( it == def.limbModels.end() ) { continue; }
			const int idx = resolveModelRef(it->second, def.id, slot.key);
			if ( idx >= 0 ) { def.limbModelIdx[slot.limbType] = idx; }
		}

		auto head = def.limbModels.find("head");
		if ( head != def.limbModels.end() )
		{
			const int idx = resolveModelRef(head->second, def.id, "head");
			if ( idx >= 0 )
			{
				def.headModelIdx = idx;
				// Whatever the head resolved to -- our .vox or a vanilla monster limb --
				// the engine has to agree it is a player head, or multiplayer breaks.
				s_raceHeadSprites.insert(idx);
			}
		}

		if ( !def.limbModelIdx.empty() || def.headModelIdx >= 0 )
		{
			SAM_INFO(MOD, "  " + def.name + " body: "
				+ std::to_string((int)def.limbModelIdx.size() + (def.headModelIdx >= 0 ? 1 : 0))
				+ " custom limb model(s) on the " + def.hostBodyName + " frame");
		}
	}
}

int SAMRaces::limbModelFor(int raceId, int limbType)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return -1; }
	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() ) { return -1; }
	auto lit = it->second.limbModelIdx.find(limbType);
	return ( lit == it->second.limbModelIdx.end() ) ? -1 : lit->second;
}

int SAMRaces::headModelFor(int raceId)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return -1; }
	auto it = s_byId.find(raceId);
	return ( it == s_byId.end() ) ? -1 : it->second.headModelIdx;
}

bool SAMRaces::clientEnemyView(int player, int monsterType, bool vanilla)
{
	if ( s_byId.empty() ) { return vanilla; }
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return vanilla; }
	if ( stats[player]->stat_appearance != 0 ) { return vanilla; }
	// A shopkeeper's opinion of you is the wanted level's to give, and it already accounts
	// for a race that cannot shop. Answering here as well would let the two disagree.
	if ( monsterType == SHOPKEEPER ) { return vanilla; }

	const int rel = declaredAllegiance(stats[player]->playerRace, monsterType);
	return ( rel < 0 ) ? vanilla : (rel == 0);
}

bool SAMRaces::isRaceHeadSprite(int sprite)
{
	return !s_raceHeadSprites.empty() && s_raceHeadSprites.count(sprite) > 0;
}

int SAMRaces::declaredAllegiance(int raceId, int monsterType)
{
	// Ordered cheapest-first. For a vanilla race raceId is 0, so this returns on the
	// first comparison without touching the map -- which is every call in a game with
	// no race mod loaded, and every call about a vanilla-race player in one that has.
	if ( raceId < SAM_RACE_ID_BASE ) { return -1; }
	if ( s_byId.empty() ) { return -1; }
	if ( monsterType <= 0 || monsterType >= NUMMONSTERS ) { return -1; }

	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() ) { return -1; }
	const SAMRaceDef& def = it->second;

	// enemies first: a type in both lists is hostile, matching the load-time warning.
	for ( int m : def.enemies ) { if ( m == monsterType ) { return 0; } }
	for ( int m : def.allies )  { if ( m == monsterType ) { return 1; } }
	return -1;
}

void SAMRaces::clear()
{
	// The head set outlives the defs otherwise, and a stale entry makes
	// isPlayerHeadSprite answer true for a model no race uses any more.
	s_raceHeadSprites.clear();
	s_byId.clear();
	s_byIdString.clear();
	s_nextId = SAM_RACE_ID_BASE;
}

bool SAMRaces::any() { return !s_byId.empty(); }
int  SAMRaces::count() { return static_cast<int>(s_byId.size()); }

const SAMRaceDef* SAMRaces::get(int raceId)
{
	auto it = s_byId.find(raceId);
	return (it != s_byId.end()) ? &it->second : nullptr;
}

bool SAMRaces::requiresBloodDiet(int raceId)
{
	const SAMRaceDef* def = get(raceId);
	return def && def->bloodDiet;
}

void SAMRaces::applySpells(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] ) { return; }
	const SAMRaceDef* def = get(stats[player]->playerRace);
	if ( !def ) { return; }
	const bool isLocalPlayer = players[player]->isLocalPlayer();
	if ( !isLocalPlayer && multiplayer == CLIENT && intro == false ) { return; }

	int learned = 0;
	for ( const std::string& sp : def->startingSpells )
	{
		// Vanilla spell name first, resolved through the same map SAMClasses uses.
		std::string lower = sp;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		int id = -1;
		for ( const auto& kv : ItemTooltips.spellItems )
		{
			if ( kv.second.internalName == lower ) { id = kv.first; break; }
		}
		if ( id < 0 )
		{
			// A custom "namespace:spell": grant via the engine spell_t SAMSpells built.
			if ( SAMSpells::getSpellByName(sp) )
			{
				if ( SAMSpells::grantCustomSpell(player, sp) ) { ++learned; }
			}
			else
			{
				SAM_ERROR(MOD, "race [" + def->id + "] references unknown spell '" + sp + "' — skipping.");
			}
			continue;
		}
		addSpell(id, player, true); // ignoreSkill: innate spells are never gated
		++learned;
	}
	if ( learned > 0 )
	{
		SAM_INFO(MOD, "Applied " + std::to_string(learned) + " innate spell(s) for race [" + def->id + "].");
	}
}

int SAMRaces::raceIdAtIndex(int index)
{
	if ( index < 0 || index >= (int)s_byId.size() ) { return -1; }
	auto it = s_byId.begin();
	std::advance(it, index);
	return it->first;
}

int SAMRaces::raceIdForIdString(const std::string& idString)
{
	auto it = s_byIdString.find(idString);
	return (it != s_byIdString.end()) ? it->second : -1;
}

int SAMRaces::hostMonsterForRace(int raceId)
{
	auto it = s_byId.find(raceId);
	return (it != s_byId.end()) ? it->second.hostMonster : (int)HUMAN;
}

std::string SAMRaces::displayName(int raceId)
{
	auto it = s_byId.find(raceId);
	return (it != s_byId.end()) ? it->second.name : std::string();
}

std::string SAMRaces::description(int raceId)
{
	auto it = s_byId.find(raceId);
	return (it != s_byId.end()) ? it->second.description : std::string();
}

void SAMRaces::applyStats(int raceId, Stat* myStats)
{
	if ( !myStats || raceId < SAM_RACE_ID_BASE ) { return; }
	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() ) { return; }
	const SAMRaceDef& d = it->second;
	myStats->STR += d.str;
	myStats->DEX += d.dex;
	myStats->CON += d.con;
	myStats->INT += d.intel;
	myStats->PER += d.per;
	myStats->CHR += d.chr;
	myStats->HP += d.hp;   myStats->MAXHP += d.hp;
	myStats->MP += d.mp;   myStats->MAXMP += d.mp;
	// The caller's unconditional std::max(1, ...) clamp runs right after this.
}
