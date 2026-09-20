/*-------------------------------------------------------------------------------
	S.A.M Framework — custom music. See sam_music.hpp.
-------------------------------------------------------------------------------*/

#include "sam_music.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "sam_net.hpp"   // the ordered channel a forced track rides to a client, and its hooks

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "main.hpp"
#include "game.hpp"
#include "engine/audio/sound.hpp"
#include "entity.hpp"
#include "stat.hpp"
#include "player.hpp"
#include "net.hpp"
#include "fmod_errors.h"

static const char* MOD = "MUSIC";

namespace
{
	std::string lower(std::string s)
	{
		for ( char& c : s ) { c = (char)std::tolower((unsigned char)c); }
		return s;
	}
	std::string joinPath(const std::string& dir, const std::string& file)
	{
		if ( dir.empty() ) { return file; }
		const char last = dir[dir.size() - 1];
		if ( last == '/' || last == '\\' ) { return dir + file; }
		return dir + "/" + file;
	}

	// ---- the game's own tracks, by name ------------------------------------------------------
	//
	// Every slot playMusic() can be handed. `file` is the track's file name; the aliases are the
	// names a modder reaches for first ("mines" for the level tracks, "mines_combat" for the fight).
	struct Slot
	{
		std::string file, alias1, alias2;
		FMOD::Sound** single = nullptr;    // &shopmusic
		FMOD::Sound*** array = nullptr;    // &minesmusic
		int idx = 0;
		FMOD::Sound* get() const
		{
			if ( single ) { return *single; }
			if ( array && *array ) { return (*array)[idx]; }
			return nullptr;
		}
	};

	const std::vector<Slot>& slots()
	{
		static std::vector<Slot> v;
		if ( !v.empty() ) { return v; }
		auto area = [&](const std::string& name, FMOD::Sound*** arr, int count) {
			for ( int i = 0; i < count; ++i )
			{
				char buf[64];
				snprintf(buf, sizeof(buf), "%s%02d", name.c_str(), i);
				Slot s; s.file = buf; s.array = arr; s.idx = i;
				s.alias1 = (i == 0) ? name + "_combat" : name;   // index 0 is the area's fight track
				v.push_back(s);
			}
		};
		area("mines", &minesmusic, NUMMINESMUSIC);
		area("swamp", &swampmusic, NUMSWAMPMUSIC);
		area("labyrinth", &labyrinthmusic, NUMLABYRINTHMUSIC);
		area("ruins", &ruinsmusic, NUMRUINSMUSIC);
		area("underworld", &underworldmusic, NUMUNDERWORLDMUSIC);
		area("hell", &hellmusic, NUMHELLMUSIC);
		area("caves", &cavesmusic, NUMCAVESMUSIC);
		area("citadel", &citadelmusic, NUMCITADELMUSIC);
		area("fortress", &fortressmusic, NUMFORTRESSMUSIC);
		// The minotaur's two are not a level/fight pair: 00 plays while one hunts you, 01 is the maze.
		{ Slot s; s.file = "minotaur00"; s.alias1 = "minotaur"; s.alias2 = "minotaur_chase"; s.array = &minotaurmusic; s.idx = 0; v.push_back(s); }
		{ Slot s; s.file = "minotaur01"; s.alias1 = "minotaur_maze"; s.array = &minotaurmusic; s.idx = 1; v.push_back(s); }
		// The menu picks among these; the first is also the victory credits.
		for ( int i = 0; i < NUMINTROMUSIC; ++i )
		{
			char buf[32];
			if ( i == 0 ) { snprintf(buf, sizeof(buf), "intro"); } else { snprintf(buf, sizeof(buf), "intro%02d", i); }
			Slot s; s.file = buf; s.alias1 = "mainmenu"; s.array = &intromusic; s.idx = i; v.push_back(s);
		}
		auto one = [&](const char* file, FMOD::Sound** p, const char* alias = "") {
			Slot s; s.file = file; s.alias1 = alias; s.single = p; v.push_back(s);
		};
		one("introduction", &introductionmusic);
		one("intermission", &intermissionmusic);
		one("minetown", &minetownmusic);
		one("splash", &splashmusic);
		one("library", &librarymusic);
		one("shop", &shopmusic);
		one("herxboss", &herxmusic, "herx");
		one("temple", &templemusic);
		one("endgame", &endgamemusic);
		one("escape", &escapemusic);
		one("devil", &devilmusic);
		one("sanctum", &sanctummusic);
		one("gnomishmines", &gnomishminesmusic);
		one("greatcastle", &greatcastlemusic);
		one("sokoban", &sokobanmusic);
		one("caveslair", &caveslairmusic);
		one("bramscastle", &bramscastlemusic);
		one("hamlet", &hamletmusic);
		one("tutorial", &tutorialmusic);
		one("gameover", &gameovermusic, "death");
		one("story", &introstorymusic, "introstory");
		return v;
	}

	bool isVanillaName(const std::string& n)
	{
		const std::string l = lower(n);
		for ( const Slot& s : slots() )
		{
			if ( s.file == l || s.alias1 == l || s.alias2 == l ) { return true; }
		}
		return false;
	}

	// ---- staged and registered ----------------------------------------------------------------

	struct Staged
	{
		std::string id;                     // "ns:name", or "" for a replacement
		std::string replace;                // lower-case vanilla name
		std::vector<std::string> paths;
		bool loop = true;
		std::vector<int> floors;
		std::vector<std::string> maps;      // lower case
		std::string combatPath;
		std::string ns;
		std::string origin;
	};
	struct StagedTheme { std::string monster, ns, track, where; int range = 0; };

	struct Track
	{
		std::string id;                     // "" for a replacement or a rule's fight track
		std::string label;
		std::vector<FMOD::Sound*> sounds;   // the variants that opened
		bool loop = true;
	};
	struct Rule { std::vector<int> floors; std::vector<std::string> maps; int track = -1; int combat = -1; };
	struct Theme { int track = -1; int range = 0; };

	std::vector<Staged> s_staged;
	std::vector<StagedTheme> s_stagedThemes;
	std::vector<Track> s_tracks;
	std::map<std::string, int> s_idToTrack;        // lower id -> track
	std::map<std::string, int> s_replace;          // lower vanilla name -> track
	std::vector<Rule> s_rules;
	std::map<std::string, Theme> s_themes;         // monster display name -> theme
	bool s_active = false;

	// Forcing. The HOST decides (a script's track, else a monster's theme) and tells clients by name.
	int s_scriptTrack = -1;
	bool s_scriptLoop = true;
	double s_scriptFade = 1.5;
	bool s_scriptPersist = false;
	// The floor a non-persistent script track belongs to. The floor NUMBER and the secret flag,
	// not map.name: every floor of one area is called the same ("The Mines"), so "until the floor
	// changes" really meant "until the area changes".
	int s_scriptFloor = -1;
	bool s_scriptSecret = false;
	int s_monsterTrack = -1;
	int s_netTrack = -1;                           // client: what the host last said
	bool s_netLoop = true;
	double s_netFade = 1.5;
	int s_forcedPlaying = -1;                      // the track handleForced last started
	int s_sentTrack = -1;                          // host: what clients were last told
	// What substitute() last handed out, so nowPlaying() can name a replacement by what the game asked for.
	FMOD::Sound* s_subSound = nullptr;
	std::string s_subName;

	unsigned s_rng = 0x2545F491u;
	FMOD::Sound* pick(const Track& t)
	{
		if ( t.sounds.empty() ) { return nullptr; }
		if ( t.sounds.size() == 1 ) { return t.sounds[0]; }
		s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
		return t.sounds[s_rng % t.sounds.size()];
	}

	int trackForId(const std::string& id)
	{
		auto it = s_idToTrack.find(lower(id));
		return (it != s_idToTrack.end()) ? it->second : -1;
	}

	int openTrack(const std::string& id, const std::string& label, const std::vector<std::string>& paths, bool loop)
	{
		Track t;
		t.id = id;
		t.label = label;
		t.loop = loop;
		for ( const std::string& p : paths )
		{
			FMOD::Sound* snd = nullptr;
			// Streamed from disk, as vanilla streams its own: a track is minutes long.
			const FMOD_RESULT r = fmod_system->createStream(p.c_str(), FMOD_2D, nullptr, &snd);
			if ( r != FMOD_OK || !snd )
			{
				SAM_ERROR(MOD, "Could not open '" + p + "' for " + label + ": " + FMOD_ErrorString(r)
					+ ". Barony plays .ogg, .wav, .mp3 and .flac; re-export the file if it is one of those.");
				continue;
			}
			t.sounds.push_back(snd);
		}
		s_tracks.push_back(t);
		return (int)s_tracks.size() - 1;
	}

	void setFade(double seconds)
	{
		// sound_update() moves a channel by increment*2 once per frame that had a tick: about 100
		// steps a second. 0 means cut.
		if ( seconds <= 0.0 ) { return; }
		float inc = (float)(1.0 / (100.0 * seconds));
		inc = std::max(0.0005f, std::min(1.0f, inc));
		fadein_increment = inc;
		fadeout_increment = inc;
	}

	// Host -> client op on the ordered S.A.M channel (SAMNet, SoundFirst block; sam_sounds.cpp has
	// the two before it):
	//   SoundFirst+2  the forced track  [u8 on][u8 loop][u16 fade centiseconds][str8 track id]
	constexpr std::uint8_t OP_MUSIC = SAMNet::Op::SoundFirst + 2;

	// One client's copy of the forced track, by NAME either way, so it means the same track whatever
	// order the mods loaded in. The ordered S.A.M channel when that client can take it: an "on" and
	// the "off" right after it can never arrive swapped, and a client still loading gets it when it
	// says hello. 'SAMU' otherwise: a stock client ignores it, and a mod set whose monster themes
	// need no script at all has no S.A.M channel (it only runs while a script is loaded).
	void sendForcedTo(int c, int track, bool loop, double fade)
	{
		// A splitscreen player shares this machine's speakers and has no address of their own:
		// net_clients[c-1] is 0.0.0.0:0, so both routes below would send into nothing.
		if ( c < 1 || c >= MAXPLAYERS || !players[c] || players[c]->isLocalPlayer() ) { return; }
		const std::string id = (track >= 0 && track < (int)s_tracks.size()) ? s_tracks[track].id : std::string();
		if ( id.size() > 200 ) { return; }
		const int fadeCs = (int)std::max(0.0, std::min(600.0, fade) * 100.0);
		SAMNet::Writer w;
		w.u8((std::uint8_t)(track >= 0 ? 1 : 0));
		w.u8((std::uint8_t)(loop ? 1 : 0));
		w.u16((std::uint16_t)fadeCs);
		w.str8(id);
		// Only a peer we KNOW runs S.A.M goes on the channel. sendToClient also answers true for
		// "held until that machine says hello", and reading that as delivered meant a stock 5.0.2
		// joiner got no mod music at all for the whole twenty second grace window: the held copy
		// was never sent, because it never says hello. Every other caller in the tree asks first,
		// and this one now does too.
		if ( SAMNet::peerHasSam(c) && SAMNet::sendToClient(c, OP_MUSIC, w.buf) ) { return; }

		// A peer already declared stock has no 'SAMU' handler: it would log one "mystery packet"
		// line and acknowledge a packet it cannot use. Same rule as 'SAMB' in sam_bodies.cpp.
		// (A mod set with no scripts never declares anybody stock, so monster themes that need no
		// script still reach everyone by 'SAMU'.)
		if ( !SAMNet::peerMayHaveSam(c) ) { return; }

		memcpy(net_packet->data, "SAMU", 4);
		net_packet->data[4] = (Uint8)(track >= 0 ? 1 : 0);
		net_packet->data[5] = (Uint8)(loop ? 1 : 0);
		SDLNet_Write16((Uint16)fadeCs, &net_packet->data[6]);
		memcpy(&net_packet->data[8], id.c_str(), id.size() + 1);
		net_packet->len = 8 + (int)id.size() + 1;
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}

	void sendForced(int track, bool loop, double fade)
	{
		if ( multiplayer != SERVER ) { return; }
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( client_disconnected[c] ) { continue; }
			sendForcedTo(c, track, loop, fade);
		}
	}

	// Client: the host's answer arrived on the ordered channel. The same thing 'SAMU' carries.
	void onMusicOp(const std::string& body)
	{
		SAMNet::Reader r(body);
		const bool on = r.u8() != 0;
		const bool loop = r.u8() != 0;
		const double fade = (double)r.u16() / 100.0;
		const std::string id = r.str8();
		if ( !r.ok ) { return; }
		SAMMusic::applyNet(on, id, fade, loop);
	}

	// Host: a client just said hello. What was queued for it while it loaded has been handed over
	// already; this is the answer as it stands NOW, "nothing forced" included, so a client that
	// arrives mid-fight hears the boss track and one that arrives after it ended hears nothing.
	void onHello(int p)
	{
		if ( !s_active ) { return; }
		const bool script = s_scriptTrack >= 0 && s_sentTrack == s_scriptTrack;
		sendForcedTo(p, s_sentTrack, script ? s_scriptLoop : true, script ? s_scriptFade : 1.5);
	}

	// Registered at static initialisation, like every SAMNet op and hook.
	struct MusicNet
	{
		MusicNet() { SAMNet::onClientOp(OP_MUSIC, &onMusicOp); }
	};
	MusicNet s_musicNet;
	SAMNet::HelloHook s_musicHello(&onHello);
	SAMNet::ClearHook s_musicClear(&SAMMusic::resetForNewGame);   // a game ended: see resetForNewGame

	// Host, once a frame while any mod music exists: expire a script's track at the floor's end,
	// find a living monster with a theme, and tell clients when the answer changes.
	void hostUpdate()
	{
		if ( s_scriptTrack >= 0 && !s_scriptPersist
			&& (s_scriptFloor != currentlevel || s_scriptSecret != secretlevel) )
		{
			s_scriptTrack = -1;
		}

		s_monsterTrack = -1;
		if ( !s_themes.empty() && map.creatures )
		{
			for ( node_t* node = map.creatures->first; node && s_monsterTrack < 0; node = node->next )
			{
				Entity* e = (Entity*)node->element;
				if ( !e || e->behavior != &actMonster ) { continue; }
				Stat* st = e->getStats();
				if ( !st || st->HP <= 0 ) { continue; }
				auto it = s_themes.find(st->name);
				if ( it == s_themes.end() ) { continue; }
				if ( it->second.range > 0 )
				{
					const double reach = it->second.range * 16.0;
					bool near = false;
					for ( int i = 0; i < MAXPLAYERS && !near; ++i )
					{
						if ( !players[i] || !players[i]->entity ) { continue; }
						const double dx = players[i]->entity->x - e->x, dy = players[i]->entity->y - e->y;
						near = dx * dx + dy * dy <= reach * reach;
					}
					if ( !near ) { continue; }
				}
				s_monsterTrack = it->second.track;
			}
		}

		const int want = (s_scriptTrack >= 0) ? s_scriptTrack : s_monsterTrack;
		if ( want != s_sentTrack )
		{
			// Silent when nothing was ever forced: an unmodded game never sends this.
			const bool loop = (s_scriptTrack >= 0) ? s_scriptLoop : true;
			const double fade = (s_scriptTrack >= 0) ? s_scriptFade : 1.5;
			sendForced(want, loop, fade);
			s_sentTrack = want;
		}
	}
}

void SAMMusic::loadFromManifest(const SAMModManifest& manifest)
{
	// Files a mod.json entry already uses (its variants, its combat track) are not ALSO tracks of
	// their own when the folder scan finds them. Entries written in mod.json come first in the list.
	auto key = [](std::string p) {
		for ( char& c : p ) { if ( c == '\\' ) { c = '/'; } c = (char)std::tolower((unsigned char)c); }
		while ( p.rfind("./", 0) == 0 ) { p = p.substr(2); }
		return p;
	};
	std::vector<std::string> used;
	for ( const SAMAudioDecl& d : manifest.musicDecls )
	{
		if ( d.fromFolder ) { continue; }
		for ( const std::string& f : d.files ) { used.push_back(key(f)); }
		if ( !d.combat.empty() ) { used.push_back(key(d.combat)); }
	}
	for ( const SAMAudioDecl& d : manifest.musicDecls )
	{
		if ( d.fromFolder && !d.files.empty()
			&& std::find(used.begin(), used.end(), key(d.files.front())) != used.end() )
		{
			SAM_DEBUG(MOD, "[" + manifest.ns + "] " + d.origin + " is part of a track mod.json declares; not a track of its own.");
			continue;
		}
		Staged s;
		s.id = d.id;
		s.replace = lower(d.replace);
		s.ns = manifest.ns;
		s.origin = d.origin;
		s.floors = d.floors;
		for ( const std::string& m : d.maps ) { s.maps.push_back(lower(m)); }
		const std::string label = s.id.empty() ? ("the replacement for '" + d.replace + "'") : ("[" + s.id + "]");
		if ( !s.replace.empty() && !isVanillaName(s.replace) )
		{
			SAM_ERROR(MOD, "[" + manifest.ns + "] '" + d.replace + "' (" + d.origin + ") is not a vanilla track name."
				" Try \"mines\", \"mines_combat\", \"shop\", \"minetown\", \"mainmenu\" or \"gameover\" --"
				" /sam_music vanilla lists every one.");
			continue;
		}
		for ( const std::string& f : d.files )
		{
			const std::string p = joinPath(manifest.modPath, f);
			std::ifstream probe(p.c_str(), std::ios::binary);
			if ( probe.good() ) { s.paths.push_back(p); }
			else { SAM_ERROR(MOD, "[" + manifest.ns + "] " + label + ": the audio file '" + p + "' is not there."); }
		}
		if ( s.paths.empty() ) { continue; }
		// Unset loop: one file loops; several take turns, the way the game rotates an area's tracks.
		s.loop = d.loopSet ? d.loop : (s.paths.size() == 1);
		if ( !d.combat.empty() )
		{
			const std::string p = joinPath(manifest.modPath, d.combat);
			std::ifstream probe(p.c_str(), std::ios::binary);
			if ( probe.good() ) { s.combatPath = p; }
			else { SAM_ERROR(MOD, "[" + manifest.ns + "] " + label + ": the combat file '" + p + "' is not there."); }
		}
		if ( !s.id.empty() )
		{
			bool dup = false;
			for ( const Staged& o : s_staged ) { if ( lower(o.id) == lower(s.id) ) { dup = true; break; } }
			if ( dup ) { SAM_WARN(MOD, "Two tracks are both called '" + s.id + "' -- keeping the first."); continue; }
		}
		SAM_INFO(MOD, "Staged " + label + " <- " + std::to_string(s.paths.size()) + " file(s) from " + s.origin
			+ (s.floors.empty() ? "" : ", on " + std::to_string(s.floors.size()) + " floor(s)")
			+ (s.maps.empty() ? "" : ", on " + std::to_string(s.maps.size()) + " map(s)")
			+ (s.combatPath.empty() ? "" : ", with a combat track"));
		s_staged.push_back(s);
	}
}

void SAMMusic::stageMonsterMusic(const std::string& monsterName, const std::string& ns,
	const std::string& track, int rangeTiles, const std::string& where)
{
	if ( track.empty() ) { return; }
	StagedTheme t; t.monster = monsterName; t.ns = ns; t.track = track; t.range = std::max(0, rangeTiles); t.where = where;
	s_stagedThemes.push_back(t);
}

int SAMMusic::appendTracks()
{
	// Streams from a previous load are released by clear(), which the loader runs first.
	if ( s_staged.empty() )
	{
		if ( !s_stagedThemes.empty() )
		{
			SAM_WARN(MOD, "A monster names a music track, but no mod declares any music.");
			s_stagedThemes.clear();
		}
		return 0;
	}
	if ( !fmod_system )
	{
		SAM_ERROR(MOD, "Sound engine not initialised -- cannot open " + std::to_string(s_staged.size()) + " track(s).");
		return 0;
	}
	int opened = 0;
	for ( const Staged& s : s_staged )
	{
		const std::string label = s.id.empty() ? ("the replacement for '" + s.replace + "' from [" + s.ns + "]") : ("[" + s.id + "]");
		const int t = openTrack(s.id, label, s.paths, s.loop);
		opened += (int)s_tracks[t].sounds.size();
		if ( s_tracks[t].sounds.empty() ) { continue; }
		if ( !s.id.empty() )
		{
			s_idToTrack[lower(s.id)] = t;
			if ( !s.floors.empty() || !s.maps.empty() )
			{
				Rule r; r.floors = s.floors; r.maps = s.maps; r.track = t;
				if ( !s.combatPath.empty() )
				{
					const int c = openTrack("", label + "'s combat track", { s.combatPath }, true);
					if ( !s_tracks[c].sounds.empty() ) { r.combat = c; }
				}
				s_rules.push_back(r);
			}
			SAM_INFO(MOD, "Registered track " + label + " (" + std::to_string(s_tracks[t].sounds.size()) + " file(s))");
		}
		else
		{
			auto prev = s_replace.find(s.replace);
			if ( prev != s_replace.end() )
			{
				SAM_WARN(MOD, "Two mods replace the vanilla track '" + s.replace + "'; the one loaded later wins.");
			}
			s_replace[s.replace] = t;
			SAM_INFO(MOD, "Replaced vanilla track '" + s.replace + "' with " + std::to_string(s_tracks[t].sounds.size()) + " file(s) from [" + s.ns + "].");
		}
	}
	for ( const StagedTheme& th : s_stagedThemes )
	{
		int t = trackForId(th.track);
		if ( t < 0 && th.track.find(':') == std::string::npos ) { t = trackForId(th.ns + ":" + th.track); }
		if ( t < 0 )
		{
			SAM_WARN(MOD, th.where + ": \"" + th.track + "\" is not a track any loaded mod declares. Ignored.");
			continue;
		}
		Theme m; m.track = t; m.range = th.range;
		s_themes[th.monster] = m;
		SAM_INFO(MOD, "Monster '" + th.monster + "' plays " + s_tracks[t].label
			+ (th.range > 0 ? " within " + std::to_string(th.range) + " tiles" : " while alive on the floor"));
	}
	s_staged.clear();
	s_stagedThemes.clear();
	s_active = !s_tracks.empty();
	SAM_INFO(MOD, "Custom music: " + std::to_string(opened) + " file(s) open, " + std::to_string(s_replace.size())
		+ " vanilla replacement(s), " + std::to_string(s_rules.size()) + " floor/map rule(s), "
		+ std::to_string(s_themes.size()) + " monster theme(s).");
	return opened;
}

void SAMMusic::clear()
{
	// A track still on a channel is stopped by its release; the game then picks its own again.
	for ( Track& t : s_tracks )
	{
		for ( FMOD::Sound* s : t.sounds ) { if ( s ) { s->release(); } }
	}
	s_tracks.clear();
	s_idToTrack.clear();
	s_replace.clear();
	s_rules.clear();
	s_themes.clear();
	s_staged.clear();
	s_stagedThemes.clear();
	s_active = false;
	s_scriptTrack = -1; s_monsterTrack = -1; s_netTrack = -1; s_forcedPlaying = -1;
	s_subSound = nullptr; s_subName.clear();
	// A client told of a track must be told it is over; an unmodded game never gets here with one.
	if ( s_sentTrack >= 0 ) { sendForced(-1, true, 0.0); }
	s_sentTrack = -1;
}

bool SAMMusic::active() { return s_active; }

FMOD::Sound* SAMMusic::substitute(FMOD::Sound* vanilla)
{
	if ( !s_active || !vanilla || s_replace.empty() ) { return vanilla; }
	for ( const Slot& sl : slots() )
	{
		if ( sl.get() != vanilla ) { continue; }
		for ( const std::string* k : { &sl.file, &sl.alias1, &sl.alias2 } )
		{
			if ( k->empty() ) { continue; }
			auto it = s_replace.find(*k);
			if ( it == s_replace.end() ) { continue; }
			FMOD::Sound* r = pick(s_tracks[it->second]);
			if ( r ) { s_subSound = r; s_subName = sl.file; return r; }
		}
		return vanilla;
	}
	return vanilla;
}

bool SAMMusic::levelTrack(int floor, const char* mapName, FMOD::Sound** out, bool* loop)
{
	if ( !s_active || s_rules.empty() || !out ) { return false; }
	const std::string m = lower(mapName ? mapName : "");
	// Newest first, so a mod loaded later (a dependent one) can take a floor over.
	for ( auto it = s_rules.rbegin(); it != s_rules.rend(); ++it )
	{
		const bool byFloor = std::find(it->floors.begin(), it->floors.end(), floor) != it->floors.end();
		const bool byMap = std::find(it->maps.begin(), it->maps.end(), m) != it->maps.end();
		if ( !byFloor && !byMap ) { continue; }
		FMOD::Sound* s = pick(s_tracks[it->track]);
		if ( !s ) { continue; }
		*out = s;
		if ( loop ) { *loop = s_tracks[it->track].loop; }
		return true;
	}
	return false;
}

bool SAMMusic::combatTrack(int floor, const char* mapName, FMOD::Sound** out)
{
	if ( !s_active || s_rules.empty() || !out ) { return false; }
	const std::string m = lower(mapName ? mapName : "");
	for ( auto it = s_rules.rbegin(); it != s_rules.rend(); ++it )
	{
		if ( it->combat < 0 ) { continue; }
		const bool byFloor = std::find(it->floors.begin(), it->floors.end(), floor) != it->floors.end();
		const bool byMap = std::find(it->maps.begin(), it->maps.end(), m) != it->maps.end();
		if ( !byFloor && !byMap ) { continue; }
		FMOD::Sound* s = pick(s_tracks[it->combat]);
		if ( s ) { *out = s; return true; }
	}
	return false;
}

bool SAMMusic::handleForced(bool playing, bool* released)
{
	if ( !s_active ) { return false; }
	int want = -1;
	bool loop = true;
	double fade = 1.5;
	if ( multiplayer == CLIENT )
	{
		want = s_netTrack; loop = s_netLoop; fade = s_netFade;
	}
	else
	{
		hostUpdate();
		if ( s_scriptTrack >= 0 ) { want = s_scriptTrack; loop = s_scriptLoop; fade = s_scriptFade; }
		else if ( s_monsterTrack >= 0 ) { want = s_monsterTrack; }
	}
	if ( want < 0 || want >= (int)s_tracks.size() )
	{
		if ( s_forcedPlaying >= 0 )
		{
			s_forcedPlaying = -1;
			if ( released ) { *released = true; }
		}
		return false;
	}
	const Track& t = s_tracks[want];
	FMOD::Sound* cur = nullptr;
	if ( music_channel ) { music_channel->getCurrentSound(&cur); }
	const bool mine = cur && std::find(t.sounds.begin(), t.sounds.end(), cur) != t.sounds.end();
	if ( s_forcedPlaying == want && !playing && !loop )
	{
		// A track asked to play once has finished: hand the music back. Not gated on `mine`: once
		// a one-shot channel ends FMOD invalidates it, getCurrentSound answers nothing, and a test
		// for "still mine" failed -- so the sting restarted forever instead of letting go.
		if ( multiplayer == CLIENT ) { s_netTrack = -1; } else if ( s_scriptTrack == want ) { s_scriptTrack = -1; }
		s_forcedPlaying = -1;
		if ( released ) { *released = true; }
		return false;
	}
	if ( s_forcedPlaying != want || !mine || !playing )
	{
		FMOD::Sound* s = pick(t);
		if ( !s ) { return false; }
		playMusic(s, loop, fade > 0.0, false);
		setFade(fade);
		s_forcedPlaying = want;
	}
	return true;
}

bool SAMMusic::play(const std::string& id, double fadeSeconds, bool loop, bool persist, std::string* err)
{
	const int t = trackForId(id);
	if ( t < 0 || s_tracks[t].sounds.empty() )
	{
		if ( err ) { *err = (t < 0) ? "no mod declares a track called '" + id + "'" : "none of '" + id + "''s files opened"; }
		return false;
	}
	s_scriptTrack = t;
	s_scriptLoop = loop;
	s_scriptFade = fadeSeconds;
	s_scriptPersist = persist;
	s_scriptFloor = currentlevel;
	s_scriptSecret = secretlevel;
	return true;
}

bool SAMMusic::stop()
{
	const bool had = s_scriptTrack >= 0;
	s_scriptTrack = -1;
	return had;
}

std::string SAMMusic::nowPlaying()
{
	if ( !music_channel ) { return std::string(); }
	bool isPlaying = false;
	music_channel->isPlaying(&isPlaying);
	if ( !isPlaying ) { return std::string(); }
	FMOD::Sound* cur = nullptr;
	music_channel->getCurrentSound(&cur);
	if ( !cur ) { return std::string(); }
	if ( cur == s_subSound && !s_subName.empty() ) { return s_subName; }   // a replacement: the name the game asked for
	for ( const Track& t : s_tracks )
	{
		if ( std::find(t.sounds.begin(), t.sounds.end(), cur) != t.sounds.end() )
		{
			return t.id.empty() ? t.label : t.id;
		}
	}
	for ( const Slot& sl : slots() ) { if ( sl.get() == cur ) { return sl.file; } }
	return std::string();
}

std::vector<std::string> SAMMusic::listIds()
{
	std::vector<std::string> out;
	for ( const auto& kv : s_idToTrack ) { out.push_back(s_tracks[kv.second].id); }
	std::sort(out.begin(), out.end());
	return out;
}

std::vector<std::string> SAMMusic::vanillaNames()
{
	std::vector<std::string> out;
	for ( const Slot& s : slots() )
	{
		for ( const std::string* k : { &s.file, &s.alias1, &s.alias2 } )
		{
			if ( !k->empty() && std::find(out.begin(), out.end(), *k) == out.end() ) { out.push_back(*k); }
		}
	}
	return out;
}

void SAMMusic::syncToClients()
{
	if ( multiplayer != SERVER || !s_active ) { return; }
	// Settle this floor's answer FIRST. At floor entry a track started on the floor just left has
	// expired, and re-sending it here only for the next frame to send "off" is a pair that a
	// direct-connect game (reliable, but not ordered) could apply the wrong way round.
	const int before = s_sentTrack;
	hostUpdate();
	if ( s_sentTrack < 0 || s_sentTrack != before ) { return; }   // nothing forced, or just sent
	const bool loop = (s_scriptTrack >= 0) ? s_scriptLoop : true;
	const double fade = (s_scriptTrack >= 0) ? s_scriptFade : 1.5;
	sendForced(s_sentTrack, loop, fade);
}

void SAMMusic::resetForNewGame()
{
	// Nothing is sent: every machine runs this for itself, at the same point of the same game.
	s_scriptTrack = -1;
	s_scriptFloor = -1;
	s_scriptSecret = false;
	s_monsterTrack = -1;
	s_netTrack = -1;
	s_sentTrack = -1;
	// s_forcedPlaying is left alone on purpose. It is what THIS machine's music channel is still
	// playing, and leaving it lets the next handleForced() see "nothing is forced now" and RELEASE
	// it -- which drops the engine's "already playing" flags so the game picks its own track.
	// Clearing it here skipped that release: the engine only re-picks level music when map.name
	// changes, so a new game whose first floor had the same name as the old one kept the old
	// looping boss track, the very bug this reset is for.
}

void SAMMusic::applyNet(bool on, const std::string& id, double fadeSeconds, bool loop)
{
	if ( !on ) { s_netTrack = -1; return; }
	const int t = trackForId(id);
	if ( t < 0 )
	{
		// Once a session, not once per floor: an ordinary mod mismatch fires this every time the
		// host changes music. The key is fixed rather than the track name, because the name came
		// off the wire and the warned set is never pruned; the first mismatch is the diagnostic
		// and the rest are the same problem.
		SAMNet::warnOnce("music:unknown",
			"The host is playing '" + id.substr(0, 64)
			+ "', which no mod on this machine declares. Is the same mod installed?");
		s_netTrack = -1;
		return;
	}
	s_netTrack = t;
	s_netLoop = loop;
	s_netFade = fadeSeconds;
}
