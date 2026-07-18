/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_sounds.hpp
	Desc: runtime custom sound-effect registry (append onto the engine sound table).

	Mods ship .ogg/.wav files and declare them in mod.json's "sounds" array. Each
	sound JSON is { id, file, loop }. At load the files are STAGED (parsed, no engine
	touch); the engine's global sounds[] array is grown once at the single safe point
	in Mods::loadMods() (after the vanilla sound-reload pass), FMOD-loading each file
	and publishing numsounds LAST so appended indices become playable atomically.

	The registry is empty in vanilla → any() is false and nothing is appended, so
	the engine sound table is byte-identical. Custom sounds route to the default SFX
	channel group automatically (their index is far above the special-cased vanilla
	indices), so no engine play-path edit is needed. sam_play_sound accepts either a
	numeric vanilla index or a registered "namespace:sound" id.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

struct SAMModManifest;  // from sam_workshop.hpp (full type only needed in the .cpp)

namespace SAMSounds
{
	// Parse + stage every sound JSON declared in a mod manifest. Does NOT touch the
	// engine sound array yet — appendSounds() does that at the safe point. Additive
	// across manifests within one load cycle; call clear() first each cycle.
	void loadFromManifest(const SAMModManifest& manifest);

	// Drop the staged list + the id->index map (called on mod (un)load). Does not
	// shrink the engine array — appendSounds() resets it back to the vanilla base
	// itself on the next run, so repeated reloads don't accumulate.
	void clear();

	// Grow the engine sounds[] table with every staged sound and build the id->index
	// map. Call ONCE from Mods::loadMods(), AFTER the vanilla sound-reload block.
	// Returns how many sounds were appended. No-op / stub in a non-Barony build.
	int appendSounds();

	// Engine sound index for a registered "namespace:sound", or -1 if unknown.
	int soundIndexForId(const std::string& id);

	// Number of custom sounds registered (appended) this session.
	int count();
	// True iff at least one custom sound is registered.
	bool any();
	// The "namespace:sound" id at registry index 0..count()-1 (ascending id order), or
	// "" if out of range. For the /sam_sounds listing.
	std::string idAtIndex(int index);
	// The engine sound index for registry index 0..count()-1, or -1.
	int engineIndexAtIndex(int index);
}
