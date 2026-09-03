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
class Entity;           // Barony entity.hpp — same reason

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

	// Optional "blood_diet": true — this race sustains on blood, not food.
	bool bloodDiet = false;

	// Optional "starting_spells": innate spells this race knows from creation (vanilla
	// "SPELL_X" names or custom "namespace:spell"). Granted by SAMRaces::applySpells.
	std::vector<std::string> startingSpells;

	// Optional "allies" / "enemies": monster types (Monster enum values) this race is at
	// peace with, or always hostile to, REGARDLESS of what its host body says.
	//
	// A race already inherits its host body's relations for free -- a goatman-bodied race
	// is ignored by goatmen because the engine sees stats->type == GOATMAN. These two
	// lists are the part that could not be expressed before: a difference from the host.
	// Both are empty by default, and an empty list is not "no allies", it is "no opinion"
	// -- the host body's own relations stand untouched.
	std::vector<int> allies;
	std::vector<int> enemies;

	// Optional "limb_models": this race's OWN body, one model per limb, instead of the
	// host body's. host_body still decides the skeleton -- the limb offsets, the
	// animation, which slots exist -- and these decide what is drawn in each slot.
	//
	// limbModels holds the reference exactly as the JSON wrote it; limbModelIdx and
	// headModelIdx hold it resolved to an engine model index, which cannot happen at
	// parse time because the model table does not exist yet. -1 means "not set", which
	// is different from 0: index 0 is models/system/null.vox and draws nothing.
	std::map<std::string, std::string> limbModels;

	// v2.5 "first_person": what YOU see of your own body. Separate from limb_models because
	// these are different models in every vanilla race too -- the third-person arm and the
	// first-person arm are not the same .vox -- and because they never leave this machine.
	// v2.5 per-limb transform. A race hosted on a body of different proportions would
	// otherwise have to re-author its .vox to fit somebody else's skeleton.
	struct LimbXform
	{
		double scale = 1.0;
		double offX = 0.0, offY = 0.0, offZ = 0.0;   // in the limb's own frame (focal)
		double pitch = 0.0, roll = 0.0;              // added to whatever the animation set
		bool any = false;                            // false = nothing to apply, skip it
	};
	// keyed by LIMB_HUMANOID_*, like limbModelIdx
	std::map<int, LimbXform> limbXform;

	// v2.5 "extra_limbs": models hung on the limb slots Barony allocates and never uses.
	struct SAMExtraLimb
	{
		std::string model;        // as written; resolved after the model table exists
		int modelIdx = -1;
		std::string attach = "body";   // body | head | torso
		double offFwd = 0.0, offSide = 0.0, offUp = 0.0;
		double focalX = 0.0, focalY = 0.0, focalZ = 0.0;
		double pitch = 0.0, roll = 0.0, yawOffsetDeg = 0.0;
		double scale = 1.0;
		bool sway = false;        // the SALAMANDER-style ping-pong used by tails
	};
	// At most 13: children indices 16..28 are the slots the engine leaves unused.
	std::vector<SAMExtraLimb> extraLimbs;

	std::string fpArm;         // the bare forearm/hand that holds your weapon
	std::string fpHandLeft;    // the left casting hand
	int fpArmIdx = -1;         // resolved by resolveLimbModels; -1 = use the host body's
	int fpHandLeftIdx = -1;
	std::map<int, int> limbModelIdx;   // LIMB_HUMANOID_* -> engine model index
	int headModelIdx = -1;
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

	// True iff this (custom) race opted into a blood diet. False for a vanilla race, an
	// unregistered id, or a race without the flag. Read by playerRequiresBloodToSustain.
	static bool requiresBloodDiet(int raceId);

	// Grant this player's custom-race innate spells (startingSpells). No-op for a vanilla
	// race or an unregistered id. Called from initClass, mirroring SAMClasses::applySpells.
	static void applySpells(int player);

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

	// What this race has DECLARED about a monster type, as a tri-state:
	//
	//    1  ally     -- will not attack it, and it will not attack back
	//    0  enemy    -- hostile on sight, whatever the host body thinks
	//   -1  silent   -- no declaration; the host body's own relations stand
	//
	// -1 is the answer for every vanilla race, every unregistered id, and every race
	// that declared nothing, which is what keeps the engine sites a true no-op. Callers
	// must treat -1 as "leave the verdict alone", never as a boolean.
	static int declaredAllegiance(int raceId, int monsterType);

	// --- custom limb models ---------------------------------------------------------
	// Turn every declared limb_models reference into an engine model index. Must run
	// AFTER the model table is built and after a mod's own .vox files are appended to
	// it, which is why it is a separate pass and not part of loadFromManifest.
	static void resolveLimbModels();

	// The model this race draws for one limb (a LIMB_HUMANOID_* constant), or -1 to
	// leave the host body's own model alone. -1 for every vanilla race.
	static int limbModelFor(int raceId, int limbType);

	// True when this race supplies its own model for that limb. The engine adds +2 to an arm
	// sprite to reach the bent / weapon-holding variant, which only works because vanilla
	// authors those variants at consecutive indices. A mod's limb is a single appended index
	// with nothing reserved after it, so the caller must skip that arithmetic. See the
	// guarded += 2 sites in actplayer.cpp.
	static bool usesLimbOverride(int raceId, int limbType);

	// The transform for one limb, or nullptr when this race declares none. Read every frame
	// by the player limb loop.
	static const SAMRaceDef::LimbXform* limbXformFor(int raceId, int limbType);

	// First-person models for this race, or -1 to leave the host body's alone. Local only:
	// the HUD arm and casting hands are never networked.
	// The extra limbs this race declares, or nullptr. Read every frame by the player limb
	// loop, so it hands back the vector rather than copying.
	static const std::vector<SAMRaceDef::SAMExtraLimb>* extraLimbsFor(int raceId);

	static int fpArmModelFor(int raceId);
	static int fpHandLeftModelFor(int raceId);

	// The model this race draws as its head, or -1. Kept apart from limbModelFor
	// because the engine sets the head on the player entity itself rather than through
	// the limb path, and never gates it behind an equipment check.
	static int headModelFor(int raceId);

	// True iff this model index is some registered race's head. The engine's
	// isPlayerHeadSprite is a hardcoded list of vanilla PLAYER heads; a race head is
	// not in it (Gharbad's head is a monster limb, not a player head), and answering
	// false there breaks the client's player-entity binding in multiplayer.
	static bool isRaceHeadSprite(int sprite);

	// The host body a head sprite belongs to (a Monster enum value), or 0 for a sprite no
	// race or class uses as a head. Entity::getMonsterTypeFromSprite consults this so the
	// engine can still tell WHAT a player wearing a custom head is: weapon focal points,
	// camera height and a dozen other things derive the body from the head model, and a
	// head that is in no monsterSprites[] row used to answer NOTHING -- limbs[0], all zeros.
	static int hostMonsterForHeadSprite(int sprite);

	// A class head override has the same problem; classes register theirs here, keyed by
	// the race they were authored for. Cleared at the start of every resolveAppearance.
	static void noteClassHeadSprite(int sprite, int hostMonster);
	static void clearClassHeadNotes();

	// True when a MONSTER (not a player) is wearing a sprite some race or class uses as a
	// head -- Gharbad the boss wears gharbad_head.vox, and so does a race built on him.
	// Client-side code that read isPlayerHeadSprite as "this is a player" needs this to
	// keep treating the monster as a monster.
	static bool isRaceHeadOnMonster(const Entity* e);

	// Mod-relative .vox paths named in limb_models, for SAMItems::registerModModels to append
	// to the model table in the same batch as item and class models. Without this a path in
	// limb_models resolved nowhere: the resolver's comment promised a registration that
	// nothing performed.
	static std::vector<std::string> limbModelPaths();

	// Does this player SEE that monster type as hostile?
	//
	// For the client-side sites that read the allegiance table directly because
	// checkEnemy is host-only there: the callout markers and the aim-assist cone. Pass
	// what the table said as `vanilla` and it is handed straight back for every vanilla
	// race, every unregistered id, and every relation nobody declared -- so those sites
	// keep their exact behaviour and only a declared relation changes the answer.
	//
	// Shopkeepers are excluded on purpose: their disposition belongs to the wanted level.
	static bool clientEnemyView(int player, int monsterType, bool vanilla);
};
