/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_races.hpp
	Desc: runtime custom playable-race registry + application.

	A custom race is a "modifier race riding an existing monster body". It is
	stored under an integer id in [SAM_RACE_ID_BASE, 255] — chosen so the id
	survives Barony's single-byte race wire format (playerRace == MISC_FLAGS[4]).
	A vanilla or mismatched client that reads such an id falls through
	getMonsterFromPlayerRace's HUMAN default, so it degrades gracefully to Human.

	The registry/parsing half (loadFromManifest/clear/get/count) is decoupled from
	Barony. The application half (hostMonsterForRace/applyStats) touches Barony
	internals and is compiled only into the game (not the editor). Racial abilities
	are delivered as attribute deltas here and, later, through the status-effect
	system — never as new hardcoded "== RACE_X" branches.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>
#include <map>

struct SAMModManifest;  // from sam_workshop.hpp (full type only needed in the .cpp)
class Stat;             // Barony stat.hpp — forward declared so this header stays light

// Custom race ids occupy [200, 255]. Barony networks playerRace as one byte and
// never range-checks it, so values in this window round-trip save + net intact,
// while vanilla getMonsterFromPlayerRace maps anything unknown to HUMAN.
static const int SAM_RACE_ID_BASE = 200;

// One parsed race JSON (mirrors race.schema.json).
struct SAMRaceDef
{
	std::string id;             // "namespace:race"
	int numericId = -1;         // assigned runtime id (>= SAM_RACE_ID_BASE)
	std::string modNamespace;   // owning mod namespace
	std::string name;           // display name (shown in char-select)
	std::string description;

	// The existing monster body this race renders as. Stored as a Monster enum
	// value; restricted at load time to the 18 bodies that have a proper
	// first-person arm (so both views are correct). Defaults to HUMAN (1).
	int hostMonster = 1;
	std::string hostBodyName;   // the resolved monster name, for diagnostics/UI

	// Attribute / HP / MP deltas added on top of the class base (vanilla races add
	// no attributes, so these are the race's whole stat identity). intel == INT.
	int str = 0, dex = 0, con = 0, intel = 0, per = 0, chr = 0, hp = 0, mp = 0;
};

class SAMRaces
{
public:
	// Read + register every race JSON declared in a mod manifest. Additive across
	// manifests within one load cycle; call clear() first each cycle.
	static void loadFromManifest(const SAMModManifest& manifest);

	// Wipe the registry and reset the id counter to SAM_RACE_ID_BASE.
	static void clear();

	// True iff at least one custom race is registered (fast no-op guard).
	static bool any();

	// Number of custom races currently registered.
	static int count();

	// Look up a registered race by its runtime id (>= 200). null if none.
	static const SAMRaceDef* get(int raceId);

	// Enumerate registered races by index (0..count()-1), ascending id order.
	// Returns the runtime race id, or -1 if out of range. Used by char-select
	// to append custom races to the picker.
	static int raceIdAtIndex(int index);

	// Reverse lookup: runtime race id for a "namespace:race" id string, or -1.
	static int raceIdForIdString(const std::string& idString);

	// --- application into the running game (defined only in the game build) ---
	// The Monster body a race renders as. For a registered SAM race -> its host
	// monster; for anything else -> HUMAN. This is the single mapping the engine's
	// getMonsterFromPlayerRace default arm calls, which fixes BOTH the 3rd-person
	// body and the 1st-person arms (they resolve through the same function).
	static int hostMonsterForRace(int raceId);

	// Display name for a race id (>= 200), or "" if unregistered.
	static std::string displayName(int raceId);
	// Description for a race id, or "".
	static std::string description(int raceId);

	// Apply the race's attribute/HP/MP deltas to a Stat. Called from the tail of
	// initClassStats, before the unconditional HP/MP clamp. No-op for a non-SAM
	// race id or an unregistered id.
	static void applyStats(int raceId, Stat* myStats);
};
