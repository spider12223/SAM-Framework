// sam_world_state.hpp -- S.A.M Framework: state that rides inside the player's savegame.
//
// S.A.M already had sam_save_data, which writes a JSON file under savegames/sam_mod_data/.
// That file is GLOBAL: it is shared by every character, it survives deleting the save that
// created it, and it has no idea which run it belongs to. That is the right home for a
// mod's settings, and the wrong home for anything about a particular character's run --
// a bank balance, a hub's unlock flags, how far a quest got. Put those in the global file
// and a new character inherits the last one's progress.
//
// This module is the other half: per-character state that is born, saved, loaded and
// deleted with one savegame. It does not invent a save format. Barony's SaveGameInfo
// already carries `additional_data`, an untyped vector<pair<string,string>> that is
// written with the rest of the save, covered by the save's integrity hash, and read back
// through loops that ignore keys they do not recognise (vanilla stores exactly one key,
// "game_scenario"). Adding namespaced keys to it inherits the entire round-trip for free
// and cannot desynchronise from the save it belongs to.
//
// Keys are stored as  "sam:" <namespace> ":" <key>  so vanilla's own reads skip them and
// two mods cannot collide.
//
// SIZE IS CAPPED ON PURPOSE. `additional_data` has no ceiling of its own, and the save
// reader allocates from the file's own numbers with only an assert() for a guard -- which
// is compiled out in release. A mod that dumped a few megabytes in here would produce a
// savegame that crashes on load rather than one that fails to load. The caps below keep
// the section small enough that this is unreachable, and a refused write is reported to
// the mod instead of being silently dropped.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace SAMWorldState
{
	// Deliberately modest. This is a place for flags, counters and small blobs, not an
	// item store -- items belong in a chest, which the engine already persists properly.
	constexpr size_t kMaxKeyBytes   = 256;
	constexpr size_t kMaxValueBytes = 8 * 1024;
	constexpr size_t kMaxTotalBytes = 64 * 1024;

	// Returns false (and logs why) if the namespace/key is unusable or a cap was hit.
	bool set(const std::string& ns, const std::string& key, const std::string& value);
	bool get(const std::string& ns, const std::string& key, std::string& out);
	bool erase(const std::string& ns, const std::string& key);
	std::vector<std::string> keys(const std::string& ns);

	// ---- savegame bridge -------------------------------------------------------------
	// collect(): called while a save is being written, appends our keys to additional_data.
	// beginLoad(): called before a save's additional_data is walked, drops what we hold.
	// absorb(): offered every key from a save being read; returns true if it was ours.
	void collect(std::vector<std::pair<std::string, std::string>>& out);
	void beginLoad();
	bool absorb(const std::string& key, const std::string& value);

	// Called when a run ends and we return to the main menu, so the next character does
	// not start life holding the last one's state. This must NOT be called at game start:
	// loadGame() has already populated us by then.
	void clearAll();

	size_t totalBytes();
}
