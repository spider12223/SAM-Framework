/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_sounds.hpp
	Desc: custom sound effects, and replacing the game's own.

	WHAT A MODDER DOES

	  * Add a sound: drop sounds/zap.ogg in the mod. It is "<namespace>:zap". No JSON.
	  * Replace a vanilla sound: drop sounds/replace/SwingWeapon.ogg. Every weapon swing now
	    plays it. Names come from the game's own sound list (docs/vanilla-sounds.md); a name can
	    be one sound ("SwingWeapon3V1") or its whole group ("SwingWeapon" = all five swings).
	  * More control: an object in mod.json's "sounds" -- several files to pick from at random,
	    a volume, a loop. A monster or an item can also carry its own "sounds" map, so a custom
	    rat squeaks its own squeak and a pair of boots has its own footsteps.

	HOW IT REACHES THE ENGINE

	Custom sounds are appended to the engine's sound table once, at the one safe point in
	Mods::loadMods (after the vanilla reload). Replacements are NOT written into the vanilla
	slots: route() swaps the index at the moment a sound is played, inside the engine's LOCAL
	play functions. Two consequences worth keeping:
	  * the network still carries the vanilla index, so a player without the mod hears the
	    original and a player with it hears the replacement -- nobody hears the wrong sound;
	  * unloading the mod needs nothing undone in the vanilla table.
	A mod's OWN sound, and a monster's or item's sound map, keep the same promise in multiplayer
	by crossing the network by name (see "multiplayer" below), never by slot number.

	THE IRON RULE: with no mod loaded every table here is empty, anyRouting() is one bool, and
	the engine's sound table is byte-identical to vanilla.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SAMModManifest;  // from sam_workshop.hpp (full type only needed in the .cpp)
class Entity;

namespace SAMSounds
{
	// Stage every sound a mod declares -- sound JSON files, mod.json objects, and the files in
	// its sounds/ and sounds/replace/ folders. Touches nothing in the engine yet.
	void loadFromManifest(const SAMModManifest& manifest);

	// Drop everything staged and registered (mod load/unload). appendSounds() resets the engine
	// table back to the vanilla size itself, so repeated reloads never accumulate.
	void clear();

	// Grow the engine sound table with every staged file, resolve every replacement and every
	// monster/item sound map. Call ONCE from Mods::loadMods(), after the vanilla sound reload.
	int appendSounds();

	// A registered sound by "ns:name", case-insensitive. With variants, each call picks one at
	// random. -1 when unknown, or when none of its files loaded.
	int soundIndexForId(const std::string& id);
	// Whether the id is registered at all -- even if every one of its files failed to load, which
	// soundIndexForId cannot tell apart from "never declared".
	bool isRegistered(const std::string& id);
	// Every added (not replacing) sound id, for sam_list_sounds and /sam_sounds.
	std::vector<std::string> listIds();

	int count();
	bool any();
	std::string idAtIndex(int index);       // for the /sam_sounds listing
	int engineIndexAtIndex(int index);

	// ---- the playback seam ------------------------------------------------------------------
	//
	// Called by the engine's local play functions. route() turns a vanilla index into its
	// replacement (a random variant) and applies a sound's volume setting; routeForEntity()
	// applies a monster's or an equipped item's own sound map first. Both return `snd` untouched
	// when nothing applies, and anyRouting()/anyEntityRouting() are single bool tests.
	bool anyRouting();
	std::uint16_t route(std::uint16_t snd, std::uint8_t* vol);
	bool anyEntityRouting();
	std::uint16_t routeForEntity(const Entity* e, std::uint16_t snd);

	// ---- the game's own sounds, by name -----------------------------------------------------
	//
	// Names are the file names in sound/sounds.txt without the extension. A name answers to:
	//   an exact file name  "SwingWeapon3V1"  -> that sound (every line that lists that file)
	//   a group            "SwingWeapon"      -> all its variants (the name minus -01 / 3V1 / ...)
	//   a number           "23"              -> that index
	// A name that is both a group and one sound in it ("Casting") means the whole group; the one
	// sound alone is reached by its number. Empty when it is none of them.
	std::vector<int> vanillaIndicesFor(const std::string& name);
	std::string vanillaNameAt(int index);
	int vanillaCount();

	// ---- sound maps on content, staged while monster / item JSON is read --------------------
	//
	// { "RatDie": "mymod:squeak" }: when THIS monster (matched by display name, like its traits
	// and body) or the holder of THIS item plays a vanilla sound on the left, it plays the sound
	// on the right instead. Keys take any vanilla name form above; values are sound ids, a bare
	// name meaning one of the declaring mod's own.
	void stageMonsterSounds(const std::string& monsterName, const std::string& ns,
		const std::vector<std::pair<std::string, std::string>>& map, const std::string& where);
	void stageItemSounds(const std::string& itemId, const std::string& ns,
		const std::vector<std::pair<std::string, std::string>>& map, const std::string& where);

	// ---- stopping -----------------------------------------------------------------------------
	//
	// Stop every channel on THIS machine that is playing one of the id's files. A looping sound
	// otherwise ran until the floor changed. Returns how many channels were stopped.
	int stopById(const std::string& id);
	// Host: tell every client to do the same. Sent only when a script asks: on the ordered S.A.M
	// channel to a client that runs S.A.M scripts (so a stop can never overtake the play it ends),
	// the 'SAMN' packet to any other.
	void broadcastStop(const std::string& id);

	// ---- multiplayer --------------------------------------------------------------------------
	//
	// A custom sound is an appended slot, and slots follow the order the mods loaded in, so a slot
	// number means the same sound on two machines only when both loaded exactly the same mods. A
	// sound map also swaps a vanilla sound for a custom one before the network ever sees it. So
	// what crosses to another machine is the sound's NAME, plus the vanilla sound it stands in for:
	//   * a client that runs S.A.M scripts (it said hello) gets it on the ordered S.A.M channel;
	//   * any other client gets the vanilla packet (SNDP / SNDG) carrying the vanilla sound -- so a
	//     stock client hears the game's own sound instead of nothing -- with the name appended after
	//     the packet's fixed bytes, which a stock client never reads and playNetTail() resolves.
	// A plain vanilla sound is not touched: every function below returns false for it and the
	// engine's own packet goes out exactly as before.

	// The id of the custom sound in engine slot `index`, or "" for a vanilla slot (and for every
	// slot when no mod adds a sound).
	std::string idForEngineIndex(int index);

	// Host: tell every remote player about a sound this machine just played at (x, y) in world
	// pixels. `played` is the slot played here; `vanilla` is the vanilla sound it stands in for, or
	// -1. False when there is nothing S.A.M has to carry (a vanilla sound with no stand-in): the
	// caller's own SNDP loop then runs unchanged.
	bool broadcastPos(double x, double y, int played, int vanilla, std::uint8_t vol);

	// Host: play `snd` once for everyone in the game (sam_play_sound). Once on THIS machine however
	// many local players share its speakers -- a loop over players played it once per splitscreen
	// player, stacked -- and to each remote player by the vanilla packet or by name, as above.
	void playForEveryone(int snd, std::uint8_t vol);

	// Client, from the engine's SNDP / SNDG handlers (net.cpp): when the packet carries a S.A.M
	// sound name after its fixed bytes, play that sound (by name, else the vanilla sound the packet
	// carries, else nothing) and return true. False means an ordinary vanilla packet.
	bool playNetTail(bool positional);
}
