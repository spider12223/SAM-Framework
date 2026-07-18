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

		def.numericId = s_nextId++;
		s_byId[def.numericId] = def;
		s_byIdString[def.id] = def.numericId;
		SAM_INFO(MOD, "Registered race: " + def.name + " [" + def.id + "] -> id "
			+ std::to_string(def.numericId) + " on body " + def.hostBodyName
			+ " (STR " + std::to_string(def.str) + " DEX " + std::to_string(def.dex)
			+ " CON " + std::to_string(def.con) + " INT " + std::to_string(def.intel)
			+ " PER " + std::to_string(def.per) + " CHR " + std::to_string(def.chr)
			+ " HP " + std::to_string(def.hp) + " MP " + std::to_string(def.mp) + ")");
	}
}

void SAMRaces::clear()
{
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
