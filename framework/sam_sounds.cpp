/*-------------------------------------------------------------------------------
	S.A.M Framework — custom sound effects, and replacing the game's own.
	See sam_sounds.hpp for what a modder does and the IRON no-op rule.
-------------------------------------------------------------------------------*/

#include "sam_sounds.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

// Engine build: the FMOD sound globals, entities and items. In a standalone build these headers
// are absent and everything that touches the engine compiles to a stub.
#if defined(__has_include) && __has_include("main.hpp")
#	define SAM_SOUNDS_HAVE_BARONY 1
#	include "main.hpp"                  // Config (USE_FMOD), physfs.h
#	include "engine/audio/sound.hpp"    // sounds, numsounds, fmod_system, the channel groups
#	include "entity.hpp"
#	include "stat.hpp"
#	include "items.hpp"
#	include "sam_items.hpp"
#	include "game.hpp"
#	include "net.hpp"
#	include "player.hpp"                // players[]->isLocalPlayer(): who hears a sound here
#	include "sam_net.hpp"               // the ordered channel a mod sound's name rides to a client
#	include "fmod_errors.h"
#endif

using nlohmann::json;
static const char* MOD = "SOUNDS";

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
	std::string lower(std::string s)
	{
		for ( char& c : s ) { c = (char)std::tolower((unsigned char)c); }
		return s;
	}
	std::string trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while ( a < b && std::isspace((unsigned char)s[a]) ) { ++a; }
		while ( b > a && std::isspace((unsigned char)s[b - 1]) ) { --b; }
		return s.substr(a, b - a);
	}
	bool isSep(char c) { return c == '-' || c == '_' || c == ' '; }

	// A sound's GROUP: its file name minus the variant suffix. "SwingWeapon3V1" -> "SwingWeapon",
	// "LeatherSteps-04" -> "LeatherSteps", "DoorOpen1V2wav" -> "DoorOpen". The same rule builds
	// docs/vanilla-sounds.md and the Mod Builder's picker, so a name printed there is one this
	// accepts.
	std::string soundGroup(const std::string& stem)
	{
		std::string g = stem;
		if ( g.size() >= 3 && lower(g.substr(g.size() - 3)) == "wav" ) { g.resize(g.size() - 3); }
		size_t i = g.size();
		while ( i > 0 && std::isdigit((unsigned char)g[i - 1]) ) { --i; }
		if ( i < g.size() && i > 0 && (g[i - 1] == 'V' || g[i - 1] == 'v') )
		{
			size_t j = i - 1;
			if ( j > 0 && isSep(g[j - 1]) ) { --j; }
			g.resize(j);
		}
		i = g.size();
		while ( i > 0 && std::isdigit((unsigned char)g[i - 1]) ) { --i; }
		if ( i < g.size() )
		{
			size_t j = i;
			if ( j > 0 && isSep(g[j - 1]) ) { --j; }
			g.resize(j);
		}
		while ( !g.empty() && isSep(g.back()) ) { g.pop_back(); }
		return g.empty() ? stem : g;
	}

	// Picks among variants. Cosmetic, so a tiny local generator rather than one of the engine's
	// streams, whose sequence other code may depend on.
	unsigned s_rng = 0x9E3779B9u;
	int pickFrom(const std::vector<int>& v)
	{
		if ( v.empty() ) { return -1; }
		if ( v.size() == 1 ) { return v[0]; }
		s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
		return v[s_rng % v.size()];
	}

	struct StagedSound
	{
		std::string id;                      // "ns:name", or empty for a replacement
		std::string replace;                 // the vanilla target, when replacing
		std::vector<std::string> absPaths;   // one per variant
		bool loop = false;
		double volume = 1.0;
		std::string ns;
		std::string origin;
	};

	struct Entry
	{
		std::string id;                      // "" for a replacement
		std::string label;                   // for log lines
		std::vector<int> indices;            // engine indices that LOADED; variants
		double volume = 1.0;
	};

	struct StagedMap
	{
		std::string key;                     // monster display name, or item "ns:item"
		bool isItem = false;
		std::string ns;
		std::vector<std::pair<std::string, std::string>> map;
		std::string where;
	};

	// Registry -- EMPTY in vanilla, which is the whole no-op guarantee.
	std::vector<StagedSound> s_staged;
	std::vector<StagedMap> s_stagedMaps;
	std::vector<Entry> s_entries;
	std::map<std::string, int> s_idToEntry;          // lower-case id -> entry
	std::map<std::string, int> s_index;               // id -> first slot, for the /sam_sounds listing
	std::vector<int> s_replaceByVanilla;              // vanilla index -> entry, -1 none
	std::vector<float> s_volumeForSlot;               // appended slot -> volume factor
	std::vector<int> s_entryForSlot;                  // appended slot -> entry, -1 none (a name for the wire)
	std::map<std::string, std::map<int, int>> s_monsterMaps;   // display name -> (vanilla index -> entry)
	std::map<int, std::map<int, int>> s_itemMaps;              // item type -> (vanilla index -> entry)
	bool s_anyRouting = false;
	bool s_anyEntityRouting = false;

	std::vector<std::string> s_vanillaNames;          // index -> file name without extension
	std::map<std::string, std::vector<int>> s_byStem; // lower name  -> indices
	std::map<std::string, std::vector<int>> s_byGroup;// lower group -> indices
#ifdef SAM_SOUNDS_HAVE_BARONY
	Uint32 s_baseNumsounds = 0;                       // the vanilla numsounds, captured once
	bool s_haveBase = false;
#endif

	// The game's sound list, read the way the engine reads it, so an index here is the index
	// the engine plays.
	void loadVanillaNames()
	{
		s_vanillaNames.clear(); s_byStem.clear(); s_byGroup.clear();
#ifdef SAM_SOUNDS_HAVE_BARONY
		const char* realDir = PHYSFS_getRealDir("sound/sounds.txt");
		if ( !realDir )
		{
			SAM_WARN(MOD, "sound/sounds.txt was not found, so vanilla sounds can only be named by number.");
			return;
		}
		const std::string path = std::string(realDir) + PHYSFS_getDirSeparator() + "sound/sounds.txt";
		std::ifstream f(path.c_str(), std::ios::binary);
		std::string line;
		int idx = 0;
		while ( idx < (int)s_baseNumsounds && std::getline(f, line) )
		{
			line = trim(line);
			const size_t slash = line.find_last_of("/\\");
			std::string stem = (slash == std::string::npos) ? line : line.substr(slash + 1);
			const size_t dot = stem.find_last_of('.');
			if ( dot != std::string::npos ) { stem = stem.substr(0, dot); }
			s_vanillaNames.push_back(stem);
			// "null" lines are placeholders the game never plays; naming them would only mislead.
			if ( !stem.empty() && lower(stem) != "null" )
			{
				s_byStem[lower(stem)].push_back(idx);
				s_byGroup[lower(soundGroup(stem))].push_back(idx);
			}
			++idx;
		}
#endif
	}

	std::string labelFor(const StagedSound& s)
	{
		return s.id.empty() ? ("the replacement for '" + s.replace + "'") : ("[" + s.id + "]");
	}

	// A sound id as a mod wrote it -> an entry, a bare name meaning one of `ns`'s own.
	int entryForId(const std::string& id, const std::string& ns)
	{
		auto it = s_idToEntry.find(lower(id));
		if ( it != s_idToEntry.end() ) { return it->second; }
		if ( id.find(':') == std::string::npos && !ns.empty() )
		{
			it = s_idToEntry.find(lower(ns + ":" + id));
			if ( it != s_idToEntry.end() ) { return it->second; }
		}
		return -1;
	}
}

std::vector<int> SAMSounds::vanillaIndicesFor(const std::string& nameIn)
{
	const std::string name = trim(nameIn);
	if ( name.empty() ) { return {}; }
	bool digits = true;
	for ( char c : name ) { if ( !std::isdigit((unsigned char)c) ) { digits = false; break; } }
	if ( digits )
	{
		const long v = std::strtol(name.c_str(), nullptr, 10);
		if ( v >= 0 && v < vanillaCount() ) { return { (int)v }; }
		return {};
	}
	// Seven names are both a group and one sound in it ("Casting" is Casting + Casting1V1 + ...).
	// Such a name means everything that answers to it, so "Casting" is the whole group; the one
	// base sound alone is reached by its number.
	const auto st = s_byStem.find(lower(name));
	const auto gr = s_byGroup.find(lower(name));
	if ( st == s_byStem.end() && gr == s_byGroup.end() ) { return {}; }
	if ( gr == s_byGroup.end() ) { return st->second; }
	if ( st == s_byStem.end() ) { return gr->second; }
	std::vector<int> all = gr->second;
	all.insert(all.end(), st->second.begin(), st->second.end());
	std::sort(all.begin(), all.end());
	all.erase(std::unique(all.begin(), all.end()), all.end());
	return all;
}

std::string SAMSounds::vanillaNameAt(int index)
{
	if ( index < 0 || index >= (int)s_vanillaNames.size() ) { return std::string(); }
	return s_vanillaNames[index];
}

int SAMSounds::vanillaCount()
{
#ifdef SAM_SOUNDS_HAVE_BARONY
	if ( !s_vanillaNames.empty() ) { return (int)s_vanillaNames.size(); }
	return s_haveBase ? (int)s_baseNumsounds : (int)numsounds;
#else
	return (int)s_vanillaNames.size();
#endif
}

namespace
{
	// A path as a key: one separator, one case, no leading "./". Two spellings of the same file
	// must compare equal, or a variant is registered twice.
	std::string pathKey(std::string p)
	{
		for ( char& c : p ) { if ( c == '\\' ) { c = '/'; } c = (char)std::tolower((unsigned char)c); }
		std::string out;
		size_t i = 0;
		while ( i < p.size() )
		{
			if ( p.compare(i, 2, "./") == 0 ) { i += 2; continue; }
			const size_t slash = p.find('/', i);
			const std::string part = p.substr(i, slash == std::string::npos ? std::string::npos : slash - i + 1);
			if ( part != "./" ) { out += part; }
			if ( slash == std::string::npos ) { break; }
			i = slash + 1;
			while ( i < p.size() && p[i] == '/' ) { ++i; }
		}
		return out;
	}
}

void SAMSounds::loadFromManifest(const SAMModManifest& manifest)
{
	// Every file a declared sound (a sound JSON or a mod.json object) already uses. A file in the
	// sounds/ folder that is one of these is part of that sound -- a variant, or a replacement's
	// audio -- and is not also registered as a sound of its own.
	std::vector<std::string> usedPaths;
	auto isUsed = [&](const std::string& p) {
		const std::string k = pathKey(p);
		return std::find(usedPaths.begin(), usedPaths.end(), k) != usedPaths.end();
	};

	auto stage = [&](StagedSound s) {
		// Every file checked HERE, where the log can still say which mod and which line. It used
		// to be found out at append time, as a slot that resolved and played silence.
		std::vector<std::string> present;
		for ( const std::string& p : s.absPaths )
		{
			std::ifstream f(p.c_str(), std::ios::binary);
			if ( f.good() ) { present.push_back(p); }
			else
			{
				SAM_ERROR(MOD, "[" + manifest.ns + "] " + labelFor(s) + ": the audio file '" + p + "' is not"
					" there. Paths are relative to the mod folder, e.g. \"sounds/zap.ogg\".");
			}
		}
		if ( present.empty() ) { return; }
		s.absPaths = present;
		if ( !s.id.empty() )
		{
			for ( const StagedSound& o : s_staged )
			{
				if ( !o.id.empty() && lower(o.id) == lower(s.id) )
				{
					SAM_WARN(MOD, "Two sounds are both called '" + s.id + "' -- keeping the first (" + o.origin
						+ "), skipping " + s.origin + ".");
					return;
				}
			}
		}
		char vol[32] = "";
		if ( s.volume != 1.0 ) { snprintf(vol, sizeof(vol), ", volume %g", s.volume); }
		SAM_INFO(MOD, "Staged " + labelFor(s) + " <- " + std::to_string(s.absPaths.size()) + " file(s) from "
			+ s.origin + (s.loop ? " (loop)" : "") + vol);
		for ( const std::string& p : s.absPaths ) { usedPaths.push_back(pathKey(p)); }
		s_staged.push_back(s);
	};

	// The older form: a path to a sound JSON file. Read with the same fields as the inline form.
	for ( const std::string& relPath : manifest.sounds )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "[" + manifest.ns + "] the sound definition '" + relPath + "' listed in mod.json"
				" is not there. (To add an audio file directly, list it as \"sounds/zap.ogg\" or just"
				" put it in the sounds/ folder.)");
			continue;
		}
		const std::string fileLabel = SAMErrors::displayFile(manifest.ns, relPath);
		json j;
		try { j = json::parse(text); }
		catch ( const json::parse_error& e )
		{
			SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "sound not loaded.");
			continue;
		}
		if ( !j.is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
				"a JSON object: { ... }", "wrap the file contents in { }", "sound not loaded.");
			continue;
		}
		StagedSound s;
		s.ns = manifest.ns;
		s.origin = relPath;
		if ( auto it = j.find("id"); it != j.end() && it->is_string() ) { s.id = it->get<std::string>(); }
		if ( auto it = j.find("replace"); it != j.end() && it->is_string() ) { s.replace = it->get<std::string>(); }
		if ( !s.id.empty() && s.id.find(':') == std::string::npos ) { s.id = manifest.ns + ":" + s.id; }
		if ( s.id.empty() == s.replace.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/id", "", s.id.empty() ? "missing" : "given with \"replace\"",
				"\"id\" to add a sound, or \"replace\" to swap a vanilla one -- one of the two",
				"keep exactly one", "sound not loaded.");
			continue;
		}
		if ( auto it = j.find("file"); it != j.end() )
		{
			auto add = [&](const std::string& f) {
				if ( f.empty() ) { return; }
				if ( SAMErrors::relPathEscapes(f) ) { SAM_WARN(MOD, labelFor(s) + " file '" + f + "' escapes the mod folder -- ignored."); return; }
				s.absPaths.push_back(joinPath(manifest.modPath, f));
			};
			if ( it->is_string() ) { add(it->get<std::string>()); }
			else if ( it->is_array() ) { for ( const auto& f : *it ) { if ( f.is_string() ) { add(f.get<std::string>()); } } }
		}
		if ( s.absPaths.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/file", "", "missing (required)",
				"a mod-relative audio file, e.g. \"sounds/boom.ogg\", or a list of them",
				"add a \"file\" field", "sound not loaded.");
			continue;
		}
		if ( auto it = j.find("loop"); it != j.end() && it->is_boolean() ) { s.loop = it->get<bool>(); }
		if ( auto it = j.find("volume"); it != j.end() && it->is_number() )
		{
			double v = it->get<double>();
			s.volume = (v >= 0.0) ? std::min(v, 4.0) : 0.0;
		}
		stage(s);
	}

	// Objects in mod.json and files in sounds/ and sounds/replace/ (parsed by SAMWorkshop). The
	// objects come first in the list, so their files are known before any folder file is looked at.
	for ( const SAMAudioDecl& d : manifest.soundDecls )
	{
		if ( d.fromFolder && !d.files.empty() && isUsed(joinPath(manifest.modPath, d.files.front())) )
		{
			SAM_DEBUG(MOD, "[" + manifest.ns + "] " + d.origin + " is part of a sound mod.json declares; not a sound of its own.");
			continue;
		}
		StagedSound s;
		s.id = d.id;
		s.replace = d.replace;
		for ( const std::string& f : d.files ) { s.absPaths.push_back(joinPath(manifest.modPath, f)); }
		s.loop = d.loop;
		s.volume = d.volume;
		s.ns = manifest.ns;
		s.origin = d.origin;
		stage(s);
	}
}

void SAMSounds::stageMonsterSounds(const std::string& monsterName, const std::string& ns,
	const std::vector<std::pair<std::string, std::string>>& map, const std::string& where)
{
	if ( map.empty() ) { return; }
	StagedMap m; m.key = monsterName; m.isItem = false; m.ns = ns; m.map = map; m.where = where;
	s_stagedMaps.push_back(m);
}

void SAMSounds::stageItemSounds(const std::string& itemId, const std::string& ns,
	const std::vector<std::pair<std::string, std::string>>& map, const std::string& where)
{
	if ( map.empty() ) { return; }
	StagedMap m; m.key = itemId; m.isItem = true; m.ns = ns; m.map = map; m.where = where;
	s_stagedMaps.push_back(m);
}

void SAMSounds::clear()
{
	s_staged.clear();
	s_stagedMaps.clear();
	s_entries.clear();
	s_idToEntry.clear();
	s_index.clear();
	s_replaceByVanilla.clear();
	s_volumeForSlot.clear();
	s_entryForSlot.clear();
	s_monsterMaps.clear();
	s_itemMaps.clear();
	s_anyRouting = false;
	s_anyEntityRouting = false;
}

int SAMSounds::soundIndexForId(const std::string& id)
{
	const int e = entryForId(id, std::string());
	if ( e < 0 ) { return -1; }
	return pickFrom(s_entries[e].indices);
}

bool SAMSounds::isRegistered(const std::string& id) { return entryForId(id, std::string()) >= 0; }

std::vector<std::string> SAMSounds::listIds()
{
	std::vector<std::string> out;
	for ( const Entry& e : s_entries ) { if ( !e.id.empty() ) { out.push_back(e.id); } }
	std::sort(out.begin(), out.end());
	return out;
}

int SAMSounds::count() { return static_cast<int>(s_index.size()); }
bool SAMSounds::any() { return !s_index.empty(); }

std::string SAMSounds::idAtIndex(int index)
{
	if ( index < 0 || index >= (int)s_index.size() ) { return std::string(); }
	auto it = s_index.begin();
	std::advance(it, index);
	return it->first;
}

int SAMSounds::engineIndexAtIndex(int index)
{
	if ( index < 0 || index >= (int)s_index.size() ) { return -1; }
	auto it = s_index.begin();
	std::advance(it, index);
	return it->second;
}

bool SAMSounds::anyRouting() { return s_anyRouting; }
bool SAMSounds::anyEntityRouting() { return s_anyEntityRouting; }

std::uint16_t SAMSounds::route(std::uint16_t snd, std::uint8_t* vol)
{
	if ( !s_anyRouting ) { return snd; }
	int play = (int)snd;
	if ( (size_t)snd < s_replaceByVanilla.size() )
	{
		const int e = s_replaceByVanilla[snd];
		if ( e >= 0 )
		{
			const int pick = pickFrom(s_entries[e].indices);
			if ( pick >= 0 ) { play = pick; }
		}
	}
#ifdef SAM_SOUNDS_HAVE_BARONY
	if ( vol && play >= (int)s_baseNumsounds )
	{
		const size_t k = (size_t)(play - (int)s_baseNumsounds);
		if ( k < s_volumeForSlot.size() && s_volumeForSlot[k] != 1.0f )
		{
			// The engine plays at vol/255 and never above, so a volume over 1 raises a quiet call
			// toward the game's maximum rather than past it.
			const long v = std::lround((double)*vol * s_volumeForSlot[k]);
			*vol = (std::uint8_t)std::max(0L, std::min(255L, v));
		}
	}
#else
	(void)vol;
#endif
	return (std::uint16_t)play;
}

std::uint16_t SAMSounds::routeForEntity(const Entity* e, std::uint16_t snd)
{
#ifdef SAM_SOUNDS_HAVE_BARONY
	if ( !s_anyEntityRouting || !e ) { return snd; }
	Entity* ent = const_cast<Entity*>(e);
	Stat* st = ent->getStats();
	if ( !st ) { return snd; }
	if ( !s_monsterMaps.empty() && ent->behavior == &actMonster )
	{
		auto m = s_monsterMaps.find(st->name);
		if ( m != s_monsterMaps.end() )
		{
			auto hit = m->second.find((int)snd);
			if ( hit != m->second.end() )
			{
				const int pick = pickFrom(s_entries[hit->second].indices);
				if ( pick >= 0 ) { return (std::uint16_t)pick; }
			}
		}
	}
	if ( !s_itemMaps.empty() )
	{
		Item* const worn[] = { st->weapon, st->shield, st->helmet, st->breastplate, st->gloves,
			st->shoes, st->cloak, st->ring, st->amulet, st->mask };
		for ( Item* item : worn )
		{
			if ( !item ) { continue; }
			auto m = s_itemMaps.find((int)item->type);
			if ( m == s_itemMaps.end() ) { continue; }
			auto hit = m->second.find((int)snd);
			if ( hit == m->second.end() ) { continue; }
			const int pick = pickFrom(s_entries[hit->second].indices);
			if ( pick >= 0 ) { return (std::uint16_t)pick; }
		}
	}
#else
	(void)e;
#endif
	return snd;
}

int SAMSounds::stopById(const std::string& id)
{
#ifdef SAM_SOUNDS_HAVE_BARONY
	const int e = entryForId(id, std::string());
	if ( e < 0 || !sounds ) { return 0; }
	std::vector<FMOD::Sound*> mine;
	for ( int idx : s_entries[e].indices )
	{
		if ( idx >= 0 && (Uint32)idx < numsounds && sounds[idx] ) { mine.push_back(sounds[idx]); }
	}
	if ( mine.empty() ) { return 0; }
	int stopped = 0;
	FMOD::ChannelGroup* const groups[] = { sound_group, soundAmbient_group, soundEnvironment_group,
		soundNotification_group, music_notification_group };
	for ( FMOD::ChannelGroup* g : groups )
	{
		if ( !g ) { continue; }
		int count = 0;
		g->getNumChannels(&count);
		// Collected first: stopping a channel can reorder the group mid-walk.
		std::vector<FMOD::Channel*> hits;
		for ( int i = 0; i < count; ++i )
		{
			FMOD::Channel* c = nullptr;
			if ( g->getChannel(i, &c) != FMOD_OK || !c ) { continue; }
			FMOD::Sound* cur = nullptr;
			c->getCurrentSound(&cur);
			if ( cur && std::find(mine.begin(), mine.end(), cur) != mine.end() ) { hits.push_back(c); }
		}
		for ( FMOD::Channel* c : hits ) { c->stop(); ++stopped; }
	}
	return stopped;
#else
	(void)id;
	return 0;
#endif
}

// ---- multiplayer ------------------------------------------------------------------------------
//
// Host -> client ops on the ordered S.A.M channel (SAMNet), in the SoundFirst block:
//   SoundFirst+0  play a sound by name  [u8 flags: bit0 positional][f64 x][f64 y] (x/y only when
//                                       positional)[u8 volume][u16 vanilla stand-in, 0xFFFF none]
//                                       [str8 id]
//   SoundFirst+1  stop a sound by name  [str8 id]
//   (SoundFirst+2 is sam_music's forced track.)
// And, for a client that has not said hello, the vanilla SNDP / SNDG packet with a tail after its
// fixed bytes: 'S' 'A' 'M', one length byte, then the id (at most TAIL_ID_MAX bytes).
#ifdef SAM_SOUNDS_HAVE_BARONY
namespace
{
	constexpr std::uint8_t OP_PLAY = SAMNet::Op::SoundFirst + 0;
	constexpr std::uint8_t OP_STOP = SAMNet::Op::SoundFirst + 1;
	constexpr std::uint16_t NO_STAND_IN = 0xFFFF;
	constexpr size_t TAIL_ID_MAX = 200;
	constexpr int SNDP_FIXED = 15;   // the bytes a stock SNDP handler reads
	constexpr int SNDG_FIXED = 7;    // the bytes a stock SNDG handler reads

	// What THIS machine plays for a name the host sent: the name if a mod here declares it, else the
	// vanilla sound it stood in for, else nothing. Never a slot number from another machine -- that
	// is exactly the number that means a different sound under a different mod list.
	int resolveNetSound(const std::string& id, int vanilla)
	{
		const int idx = SAMSounds::soundIndexForId(id);
		if ( idx >= 0 ) { return idx; }
		if ( vanilla >= 0 && vanilla < SAMSounds::vanillaCount() ) { return vanilla; }
		SAMNet::warnOnce("sound:missing:" + id, "The host played the sound '" + id + "', which no mod on this"
			" machine declares, so it was not played here. Is the same mod installed?");
		return -1;
	}

	// The vanilla packet for one client: SNDP (positional) or SNDG (global), carrying `wire`, with the
	// name appended when there is one. A stock client reads only the fixed bytes.
	void sendVanillaPacket(int c, bool positional, double x, double y, int wire, Uint8 vol, const std::string& id)
	{
		int at = 0;
		if ( positional )
		{
			memcpy(net_packet->data, "SNDP", 4);
			SDLNet_Write32((Uint32)(Sint32)x, &net_packet->data[4]);
			SDLNet_Write32((Uint32)(Sint32)y, &net_packet->data[8]);
			SDLNet_Write16((Uint16)wire, &net_packet->data[12]);
			net_packet->data[14] = vol;
			at = SNDP_FIXED;
		}
		else
		{
			memcpy(net_packet->data, "SNDG", 4);
			SDLNet_Write16((Uint16)wire, &net_packet->data[4]);
			net_packet->data[6] = vol;
			at = SNDG_FIXED;
		}
		if ( !id.empty() && id.size() <= TAIL_ID_MAX )
		{
			net_packet->data[at + 0] = 'S';
			net_packet->data[at + 1] = 'A';
			net_packet->data[at + 2] = 'M';
			net_packet->data[at + 3] = (Uint8)id.size();
			memcpy(&net_packet->data[at + 4], id.data(), id.size());
			at += 4 + (int)id.size();
		}
		net_packet->len = at;
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}

	// One remote player: by name on the ordered channel when they run S.A.M scripts, else the vanilla
	// packet. `id` is the played sound's name ("" for a vanilla one), `vanilla` the stand-in (-1 none).
	void sendSoundTo(int c, bool positional, double x, double y, int played, int vanilla, Uint8 vol, const std::string& id)
	{
		// Not while a level loads: the channel is flushed from the game tick, which does not run
		// then, and a sound that arrives a level late is worse than the vanilla stand-in.
		if ( !id.empty() && !loading && SAMNet::peerHasSam(c) )
		{
			SAMNet::Writer w;
			w.u8(positional ? 1 : 0);
			if ( positional ) { w.f64(x); w.f64(y); }
			w.u8(vol);
			w.u16(vanilla >= 0 ? (std::uint16_t)vanilla : NO_STAND_IN);
			w.str8(id);
			if ( SAMNet::sendToClient(c, OP_PLAY, w.buf) ) { return; }
		}
		// A stock client plays the fixed index: the game's own sound when this one stands in for it,
		// else the mod slot, which a stock client drops as out of range.
		sendVanillaPacket(c, positional, x, y, vanilla >= 0 ? vanilla : played, vol, id);
	}

	void onPlayOp(const std::string& body)
	{
		SAMNet::Reader r(body);
		const std::uint8_t flags = r.u8();
		const bool positional = (flags & 1) != 0;
		double x = 0.0, y = 0.0;
		if ( positional ) { x = r.f64(); y = r.f64(); }
		const std::uint8_t vol = r.u8();
		const std::uint16_t standIn = r.u16();
		const std::string id = r.str8();
		if ( !r.ok || id.empty() ) { return; }
		const int idx = resolveNetSound(id, standIn == NO_STAND_IN ? -1 : (int)standIn);
		if ( idx < 0 ) { return; }
		if ( positional ) { playSoundPosLocal((real_t)x, (real_t)y, (Uint16)idx, (Uint8)vol); }
		else { playSound((Uint16)idx, (Uint8)vol); }
	}

	void onStopOp(const std::string& body)
	{
		SAMNet::Reader r(body);
		const std::string id = r.str8();
		if ( !r.ok || id.empty() ) { return; }
		SAMSounds::stopById(id);
	}

	// Registered at static initialisation, like every SAMNet op. The handlers run from the game tick.
	struct SoundOps
	{
		SoundOps()
		{
			SAMNet::onClientOp(OP_PLAY, &onPlayOp);
			SAMNet::onClientOp(OP_STOP, &onStopOp);
		}
	};
	SoundOps s_soundOps;
}
#endif

std::string SAMSounds::idForEngineIndex(int index)
{
#ifdef SAM_SOUNDS_HAVE_BARONY
	if ( s_entryForSlot.empty() || !s_haveBase || index < (int)s_baseNumsounds ) { return std::string(); }
	const size_t k = (size_t)(index - (int)s_baseNumsounds);
	if ( k >= s_entryForSlot.size() ) { return std::string(); }
	const int e = s_entryForSlot[k];
	if ( e < 0 || e >= (int)s_entries.size() ) { return std::string(); }
	return s_entries[e].id;
#else
	(void)index;
	return std::string();
#endif
}

bool SAMSounds::broadcastPos(double x, double y, int played, int vanilla, std::uint8_t vol)
{
#ifdef SAM_SOUNDS_HAVE_BARONY
	if ( multiplayer != SERVER || vol == 0 ) { return false; }
	const std::string id = idForEngineIndex(played);
	if ( id.empty() && vanilla < 0 ) { return false; }   // a plain vanilla sound: the engine's packet
	for ( int c = 1; c < MAXPLAYERS; ++c )
	{
		if ( client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() ) { continue; }
		sendSoundTo(c, true, x, y, played, vanilla, (Uint8)vol, id);
	}
	return true;
#else
	(void)x; (void)y; (void)played; (void)vanilla; (void)vol;
	return false;
#endif
}

void SAMSounds::playForEveryone(int snd, std::uint8_t vol)
{
#ifdef SAM_SOUNDS_HAVE_BARONY
	if ( snd < 0 || (Uint32)snd >= numsounds ) { return; }
	// Once here if anybody here is playing. Every local player shares this machine's speakers, so
	// playing it per local player (what a loop over playSoundPlayer did) stacked it in splitscreen.
	bool anyLocal = false;
	for ( int i = 0; i < MAXPLAYERS && !anyLocal; ++i )
	{
		anyLocal = players[i] && !client_disconnected[i] && players[i]->isLocalPlayer();
	}
	if ( anyLocal ) { playSound((Uint16)snd, (Uint8)vol); }
	if ( multiplayer != SERVER || vol == 0 ) { return; }
	const std::string id = idForEngineIndex(snd);
	for ( int c = 1; c < MAXPLAYERS; ++c )
	{
		if ( client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() ) { continue; }
		// A vanilla sound goes as exactly the SNDG playSoundPlayer sends; a mod's by name.
		sendSoundTo(c, false, 0.0, 0.0, snd, -1, (Uint8)vol, id);
	}
#else
	(void)snd; (void)vol;
#endif
}

bool SAMSounds::playNetTail(bool positional)
{
#ifdef SAM_SOUNDS_HAVE_BARONY
	if ( !net_packet ) { return false; }
	const int at = positional ? SNDP_FIXED : SNDG_FIXED;
	if ( net_packet->len < at + 5 ) { return false; }
	const Uint8* d = net_packet->data;
	if ( d[at] != 'S' || d[at + 1] != 'A' || d[at + 2] != 'M' ) { return false; }
	const int n = (int)d[at + 3];
	// Bounded against the real packet: a lying length byte would read past the buffer.
	if ( n <= 0 || n > (int)TAIL_ID_MAX || net_packet->len < at + 4 + n ) { return false; }
	const std::string id((const char*)&d[at + 4], (size_t)n);
	const int fixed = (int)SDLNet_Read16(&d[positional ? 12 : 4]);
	const Uint8 vol = d[positional ? 14 : 6];
	// The fixed index is only trusted as a VANILLA sound. A mod slot from the host's table means
	// whatever that slot holds here, which is the wrong-sound bug this tail exists to prevent.
	const int idx = resolveNetSound(id, fixed < vanillaCount() ? fixed : -1);
	if ( idx >= 0 )
	{
		if ( positional )
		{
			playSoundPosLocal((real_t)SDLNet_Read32(&d[4]), (real_t)SDLNet_Read32(&d[8]), (Uint16)idx, vol);
		}
		else { playSound((Uint16)idx, vol); }
	}
	return true;
#else
	(void)positional;
	return false;
#endif
}

void SAMSounds::broadcastStop(const std::string& id)
{
#ifdef SAM_SOUNDS_HAVE_BARONY
	if ( multiplayer != SERVER || id.empty() || id.size() > 200 ) { return; }
	for ( int c = 1; c < MAXPLAYERS; ++c )
	{
		if ( client_disconnected[c] ) { continue; }
		// A splitscreen player on this very machine has no address to send to: net_clients[c-1]
		// is 0.0.0.0:0 and every send burns the transport's retries on nothing.
		if ( !players[c] || players[c]->isLocalPlayer() ) { continue; }
		// The ordered channel to a client that runs S.A.M, the same one its plays came on, so a
		// stop sent right after a play can never arrive first and leave a loop running.
		if ( SAMNet::peerHasSam(c) )
		{
			SAMNet::Writer w;
			w.str8(id);
			if ( SAMNet::sendToClient(c, OP_STOP, w.buf) ) { continue; }
		}
		// A peer already declared stock has no 'SAMN' handler, so the raw fallback below only
		// earns it a "mystery packet" log line and an ACK round trip -- once per stop, which a
		// script calling this from on_tick turns into a flood. Same rule as 'SAMB' in
		// sam_bodies.cpp: nothing at all once we know.
		if ( !SAMNet::peerMayHaveSam(c) ) { continue; }
		memcpy(net_packet->data, "SAMN", 4);
		memcpy(&net_packet->data[4], id.c_str(), id.size() + 1);
		net_packet->len = 4 + (int)id.size() + 1;
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
#else
	(void)id;
#endif
}

int SAMSounds::appendSounds()
{
#ifndef SAM_SOUNDS_HAVE_BARONY
	return 0;
#else
	// Capture the vanilla base the first time we ever append, so a mod reload (which re-stages)
	// resets the engine table to base instead of accumulating.
	if ( !s_haveBase ) { s_baseNumsounds = numsounds; s_haveBase = true; }

	// Drop anything appended last time back to the vanilla base. Safe because a mod reload
	// happens at the menu, not mid-play.
	if ( sounds && numsounds > s_baseNumsounds )
	{
		for ( Uint32 i = s_baseNumsounds; i < numsounds; ++i )
		{
			if ( sounds[i] ) { sounds[i]->release(); sounds[i] = nullptr; }
		}
		numsounds = s_baseNumsounds;
	}
	s_entries.clear(); s_idToEntry.clear(); s_index.clear(); s_replaceByVanilla.clear();
	s_volumeForSlot.clear(); s_entryForSlot.clear(); s_monsterMaps.clear(); s_itemMaps.clear();
	s_anyRouting = false; s_anyEntityRouting = false;

	if ( s_staged.empty() )
	{
		if ( !s_stagedMaps.empty() )
		{
			SAM_WARN(MOD, std::to_string(s_stagedMaps.size()) + " monster/item sound map(s) name sounds, but no mod"
				" declared any sounds for them to play.");
			s_stagedMaps.clear();
		}
		return 0;
	}
	if ( !fmod_system || !sounds || numsounds == 0 )
	{
		SAM_ERROR(MOD, "Sound engine not initialised -- cannot load " + std::to_string(s_staged.size()) + " custom sound(s).");
		return 0;
	}

	loadVanillaNames();

	size_t total = 0;
	for ( const StagedSound& s : s_staged ) { total += s.absPaths.size(); }
	const Uint32 oldCount = numsounds;
	FMOD::Sound** grown = (FMOD::Sound**)realloc(sounds, sizeof(FMOD::Sound*) * (size_t)(oldCount + (Uint32)total));
	if ( !grown )
	{
		SAM_ERROR(MOD, "Out of memory growing the sound table to " + std::to_string(oldCount + total) + " -- leaving it untouched.");
		return 0;
	}
	sounds = grown;
	s_volumeForSlot.assign(total, 1.0f);
	s_entryForSlot.assign(total, -1);
	s_replaceByVanilla.assign((size_t)s_baseNumsounds, -1);
	std::vector<std::string> replacedBy((size_t)s_baseNumsounds);

	Uint32 next = oldCount;
	int loaded = 0, failed = 0, replacements = 0;
	for ( const StagedSound& s : s_staged )
	{
		Entry e;
		e.id = s.id;
		e.label = labelFor(s);
		e.volume = s.volume;
		for ( const std::string& path : s.absPaths )
		{
			const Uint32 idx = next++;
			FMOD_MODE flags = FMOD_DEFAULT | FMOD_3D | FMOD_LOWMEM;
			if ( s.loop ) { flags |= FMOD_LOOP_NORMAL; }
			FMOD::Sound* snd = nullptr;
			const FMOD_RESULT r = fmod_system->createSound(path.c_str(), flags, nullptr, &snd);
			// The slot is taken either way, so every later index is the same on every machine
			// whether or not one file failed on one of them. A failed slot simply never joins
			// the entry's variants, so the id never resolves to silence.
			sounds[idx] = (r == FMOD_OK) ? snd : nullptr;
			s_volumeForSlot[idx - oldCount] = (float)s.volume;
			if ( r != FMOD_OK || !snd )
			{
				++failed;
				SAM_ERROR(MOD, "Could not load '" + path + "' for " + e.label + ": " + FMOD_ErrorString(r)
					+ ". Barony plays .ogg, .wav, .mp3 and .flac; re-export the file if it is one of those.");
			}
			else
			{
				++loaded;
				e.indices.push_back((int)idx);
				// Which entry this slot belongs to, so the host can name it on the wire. Taken
				// before the push below, so it is the index this entry is about to get.
				s_entryForSlot[idx - oldCount] = (int)s_entries.size();
			}
		}
		const int entryIdx = (int)s_entries.size();
		s_entries.push_back(e);
		if ( !s.id.empty() )
		{
			s_idToEntry[lower(s.id)] = entryIdx;
			s_index[s.id] = e.indices.empty() ? -1 : e.indices.front();
			SAM_INFO(MOD, "Registered sound [" + s.id + "] -> " + std::to_string(e.indices.size()) + " variant(s)"
				+ (e.indices.empty() ? " -- none loaded, so it will not play" : ""));
			continue;
		}

		const std::vector<int> targets = vanillaIndicesFor(s.replace);
		if ( targets.empty() )
		{
			SAM_ERROR(MOD, "[" + s.ns + "] '" + s.replace + "' (" + s.origin + ") is not a vanilla sound name."
				" Names are the files in the game's sound/sounds.txt without the extension -- one sound"
				" (\"SwingWeapon3V1\") or a whole group (\"SwingWeapon\"). Type /sam_sounds vanilla <part of a"
				" name> in the console to search, or see docs/vanilla-sounds.md.");
			continue;
		}
		if ( e.indices.empty() )
		{
			SAM_WARN(MOD, "The replacement for '" + s.replace + "' has no file that loaded, so the vanilla sound keeps playing.");
			continue;
		}
		for ( int t : targets )
		{
			if ( s_replaceByVanilla[t] >= 0 && replacedBy[t] != s.ns )
			{
				SAM_WARN(MOD, "Vanilla sound '" + vanillaNameAt(t) + "' is replaced by both [" + replacedBy[t]
					+ "] and [" + s.ns + "]; the one loaded later, [" + s.ns + "], wins.");
			}
			s_replaceByVanilla[t] = entryIdx;
			replacedBy[t] = s.ns;
		}
		++replacements;
		SAM_INFO(MOD, "Replaced vanilla '" + s.replace + "' (" + std::to_string(targets.size()) + " sound"
			+ (targets.size() == 1 ? "" : "s") + ") with " + std::to_string(e.indices.size()) + " file(s) from [" + s.ns + "].");
	}
	numsounds = oldCount + (Uint32)total; // publish LAST: the appended indices are now playable

	// Sound maps on monsters and items, now that every sound has an entry.
	int maps = 0;
	for ( const StagedMap& m : s_stagedMaps )
	{
		std::map<int, int> resolved;
		for ( const auto& kv : m.map )
		{
			const std::vector<int> targets = vanillaIndicesFor(kv.first);
			if ( targets.empty() )
			{
				SAM_WARN(MOD, m.where + ": \"" + kv.first + "\" is not a vanilla sound name (see docs/vanilla-sounds.md). Ignored.");
				continue;
			}
			const int entry = entryForId(kv.second, m.ns);
			if ( entry < 0 || s_entries[entry].id.empty() )
			{
				SAM_WARN(MOD, m.where + ": \"" + kv.second + "\" is not a sound any loaded mod declares. Ignored.");
				continue;
			}
			if ( s_entries[entry].indices.empty() ) { continue; } // its load failure is already logged
			for ( int t : targets ) { resolved[t] = entry; }
		}
		if ( resolved.empty() ) { continue; }
		if ( m.isItem )
		{
			const int type = SAMItems::itemIdForIdString(m.key);
			if ( type < 0 )
			{
				SAM_WARN(MOD, m.where + ": the item '" + m.key + "' is not registered, so its sounds are not used.");
				continue;
			}
			for ( const auto& kv : resolved ) { s_itemMaps[type][kv.first] = kv.second; }
		}
		else
		{
			for ( const auto& kv : resolved ) { s_monsterMaps[m.key][kv.first] = kv.second; }
		}
		++maps;
	}

	bool anyVolume = false;
	for ( float v : s_volumeForSlot ) { if ( v != 1.0f ) { anyVolume = true; break; } }
	s_anyRouting = replacements > 0 || anyVolume;
	s_anyEntityRouting = !s_monsterMaps.empty() || !s_itemMaps.empty();
	if ( !s_anyRouting ) { s_replaceByVanilla.clear(); }

	s_staged.clear();
	s_stagedMaps.clear();
	SAM_INFO(MOD, "Custom sounds: " + std::to_string(loaded) + " file(s) loaded"
		+ (failed ? ", " + std::to_string(failed) + " FAILED" : std::string()) + ", " + std::to_string(replacements)
		+ " vanilla replacement(s), " + std::to_string(maps) + " monster/item sound map(s); numsounds "
		+ std::to_string(oldCount) + " -> " + std::to_string(numsounds) + ".");
	return (int)total;
#endif
}
