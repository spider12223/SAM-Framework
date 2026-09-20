/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_music.hpp
	Desc: custom music -- a mod's own tracks, and replacing the game's.

	WHAT A MODDER DOES

	  * Replace a vanilla track: drop music/replace/mines.ogg. Every Mines level track is now
	    that file. Names: "mines" (the level tracks), "mines_combat" (the fight track), an exact
	    file ("mines03"), "shop", "minetown", "mainmenu", "gameover", ... (docs/sounds-and-music.md).
	  * Add a track: drop music/boss.ogg. It is "<namespace>:boss", for a script or a monster.
	  * Give floors or maps their own music: an object in mod.json's "music" with "floors" or
	    "maps", and an optional "combat" file for fights there.
	  * Boss music: a monster's JSON "music": "mymod:boss" plays while it is alive on the floor.
	  * From a script: sam_play_music("mymod:boss") takes over for every player,
	    sam_stop_music() hands it back and the game crossfades to what it would have played.

	HOW IT REACHES THE ENGINE

	Every track the game plays goes through playMusic(), so a replacement is one substitution
	there and the game's own choice of WHICH track to play is untouched. Floor rules and forced
	tracks are three guards in handleLevelMusic(). Music is chosen on each machine separately, so
	the one thing that has to cross the network -- a forced track -- is sent by NAME (the S.A.M
	channel, or 'SAMU'; see "multiplayer" below).

	THE IRON RULE: with no mod declaring music, active() is false, every seam is one bool test,
	and nothing is ever sent.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

struct SAMModManifest;
namespace FMOD { class Sound; }

namespace SAMMusic
{
	// Stage a mod's tracks, replacements and floor/map rules (mod.json "music" + the music/
	// and music/replace/ folders). Touches nothing in the engine yet.
	void loadFromManifest(const SAMModManifest& manifest);
	// A monster's theme, by display name like its traits. range 0 = anywhere on the floor.
	void stageMonsterMusic(const std::string& monsterName, const std::string& ns,
		const std::string& track, int rangeTiles, const std::string& where);

	// Open every staged file. Call from Mods::loadMods() after the vanilla music reload.
	int appendTracks();
	// Release every stream and forget everything (mod load start / unload).
	void clear();
	bool active();

	// ---- the engine's seams (engine/audio/music.cpp) ---------------------------------------
	FMOD::Sound* substitute(FMOD::Sound* vanilla);
	bool levelTrack(int floor, const char* mapName, FMOD::Sound** out, bool* loop);
	bool combatTrack(int floor, const char* mapName, FMOD::Sound** out);
	// Top of handleLevelMusic. True when a forced track (a script's, or a monster's theme) owns
	// the music this frame. *released goes true on the frame one lets go, so the caller can
	// reset its "already playing" flags and the game picks its own track again.
	bool handleForced(bool playing, bool* released);

	// ---- scripts ----------------------------------------------------------------------------
	// Host: force a track for everyone until stop() -- or until the floor changes, unless persist.
	bool play(const std::string& id, double fadeSeconds, bool loop, bool persist, std::string* err);
	bool stop();
	// The track actually playing on THIS machine: "ns:name", a vanilla name ("mines02"), or "".
	std::string nowPlaying();
	std::vector<std::string> listIds();
	// The vanilla names a replacement can target, for the console and the docs.
	std::vector<std::string> vanillaNames();

	// ---- multiplayer --------------------------------------------------------------------------
	//
	// The host tells each client what is forced, by NAME: on the ordered S.A.M channel to a client
	// that runs S.A.M scripts (so an "on" and the "off" after it can never swap, and a client that
	// says hello late is told what is playing), the 'SAMU' packet to any other -- a mod set with a
	// monster theme and no scripts at all still needs that one, since the channel only runs while a
	// script is loaded.
	void syncToClients();   // floor entry: settle this floor's answer, then re-send it
	void applyNet(bool on, const std::string& id, double fadeSeconds, bool loop);

	// Forget every forced track, on THIS machine: the host's script track and monster theme, the
	// track a client was told to play, what clients were last told. For a new game on every
	// machine (doNewGame) and for a game that ended (a SAMNet clear hook runs it). A client that
	// left a game while a boss track played otherwise played it, looping, through the whole of its
	// next game under a host that forced nothing -- a host only ever sends a CHANGE. A track still
	// on this machine's channel is let go by the next in-game music update, the normal way, so the
	// game's own track comes back.
	void resetForNewGame();
}
