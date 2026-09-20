/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_spells.hpp
	Desc: runtime custom-spell registry (Session 1 — loader/metadata only).

	Custom spells are registered into S.A.M's own registry under integer ids
	starting at SAM_SPELL_ID_BASE (2000), chosen to clear the vanilla SPELL_* enum
	(max 224, NUM_SPELLS 225), the spell-ELEMENT id space (>= 10000), and the
	500-999 range other mod toolkits use.

	SCOPE (Session 1): this half only PARSES + VALIDATES spell JSON and hands out a
	stable runtime id + namespaced-name lookup, so mods and scripts can REFERENCE a
	custom spell (e.g. a class's starting_spells, or sam_grant_spell). It does NOT
	yet build the in-engine spell_t / element tree, so a custom spell cannot be cast
	or granted for real yet — that (the casting behavior) is a later session. The
	registry itself is engine-decoupled: it touches no Barony types.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <map>

struct SAMModManifest;  // from sam_workshop.hpp (full type only needed in the .cpp)

// Custom spell ids occupy [2000, ...). Well clear of vanilla (<225), elements
// (>=10000) and the 500-999 range used by other toolkits.
static const int SAM_SPELL_ID_BASE = 2000;

// One parsed spell JSON (mirrors spell.schema.json). Session 1 stores the full
// metadata; the casting-relevant fields are carried now and consumed later.
struct SAMSpellDef
{
	std::string id;                 // "namespace:spell"
	int numericId = 0;              // assigned runtime id (>= SAM_SPELL_ID_BASE)
	std::string modNamespace;       // owning mod namespace
	std::string modPath;            // absolute mod folder (to resolve the icon PNG later)

	std::string name;               // display name
	std::string description;

	int manaCost = 1;
	// Magic skill (Spellcasting proficiency + INT) needed to LEARN it from a book. 0 = the
	// default = anyone can learn it, even a low-INT melee class (the learn path passes
	// ignoreSkill for a difficulty-0 custom spell, matching "learnable by any caster").
	// Set it above 0 to gate the spell behind magic skill like a vanilla spellbook.
	int difficulty = 0;
	std::string projectileType = "missile"; // "missile" | "missile_trio" | "none"
	std::string payload;            // e.g. "force", "fire", "drain_soul" (schema enum)

	int damageMin = 0;
	int damageMax = 0;
	int range = 0;
	int speed = 0;

	std::string onHitEffect;        // optional EFF_ name or custom "ns:effect"
	int onHitDuration = 0;          // ticks
	int onHitChance = 0;            // percent 0-100
	mutable int onHitEffectSlot = -1; // v1.5.0: resolved engine effect slot for onHitEffect (-1 = none/unknown)

	std::string icon;               // mod-relative PNG path
	bool startingSpell = false;
};

class SAMSpells
{
public:
	// Read + register every spell JSON declared in a mod manifest, into the spell
	// registry starting at SAM_SPELL_ID_BASE. Additive across manifests within one
	// load cycle; call clear() first each cycle.
	static void loadFromManifest(const SAMModManifest& manifest);

	// Wipe the registry and reset the id counter to SAM_SPELL_ID_BASE.
	static void clear();

	// Number of custom spells currently registered.
	static int count();

	// Look up a registered spell by its runtime id (>= 2000). null if none.
	static const SAMSpellDef* getSpell(int spellId);

	// Look up a registered spell by its "namespace:spell" id string. null if none.
	static const SAMSpellDef* getSpellByName(const std::string& namespacedId);

	// --- Session 2: build the real in-engine spell (needs Barony magic.hpp) --------
	// Construct the Barony spell_t (element tree) for a registered SAMSpellDef and
	// register it into allGameSpells[def.numericId], plus populate ItemTooltips.spellItems
	// so the spell is grantable, castable and shows in the spell UI with its name.
	// Idempotent: a no-op if that id is already present in allGameSpells.
	static void buildEngineSpell(const SAMSpellDef& def);
	// Build the engine spell for every registered SAMSpellDef (idempotent). Re-invoked
	// from setupSpells(), which wipes allGameSpells whenever it re-runs.
	static void buildAllEngineSpells();
	// Remove + free the SAM engine spells from allGameSpells + spellItems (revert on unload).
	static void removeEngineSpells();

	// Grant a registered custom spell ("namespace:spell") to a player's known spells (uses
	// the engine addSpell now that the spell_t exists). Returns false if unknown/unbuilt or
	// the grant was refused (already known / non-local player).
	static bool grantCustomSpell(int player, const std::string& namespacedId);

	// ---- the script API's spell operations (sam_grant_spell / sam_remove_spell / readers) ----
	// Shared by the Lua and JS bindings so the two cannot drift, and by the multiplayer
	// inventory mirror.

	// A script's spell reference -> engine spell id, or -1. Accepts a numeric id as text (what
	// sam_get_tome_spell hands back), a mod's "namespace:spell", or a vanilla internal name
	// ("spell_fireball", any case).
	static int resolveSpellRef(const std::string& spell);

	// The name scripts see for a spell id: the "namespace:spell" a mod declared, else the
	// engine's internal name, else "" for an id nothing defines.
	static std::string scriptName(int spellId);

	// Teach spell `spellId` to `player` on THIS machine, through the engine's own addSpell (the
	// spell list plus its spell item). The player must play here; in multiplayer the
	// trampoline carries a remote player's grant to their machine before this runs. False
	// (logged) when the spell is not built, the player is not on this machine, or they already
	// know it. A removal of the same spell that is still queued (see queueRemoveSpell) is
	// cancelled instead and the grant succeeds, so "remove it, then grant my own version of
	// it" in one handler leaves the player with the spell rather than with neither.
	static bool grantSpell(int player, int spellId);

	// Forget spell `spellId` for `player` on this machine. Queued, never immediate: the engine
	// frame that fired the event a script is answering may be casting that very spell_t (the
	// player's selected spell IS the list element) or using its spell item. Returns false when
	// the player does not know it or is not on this machine. drainRemoveQueue does the work at
	// the same safe point as SAMItems::drainDestroyQueue, which calls it. Until it does, the
	// player still knows the spell: a reader asked in the same frame says so, and a grant of
	// the same spell cancels the removal (grantSpell).
	static bool queueRemoveSpell(int player, int spellId);
	static void drainRemoveQueue();
	static void clearRemoveQueue();

	// Absolute, Image::get-ready path to a custom spell's icon PNG (from the mod folder),
	// or "" if the runtime id isn't a registered custom spell / has no icon / the file is
	// missing. The spell-icon UI calls this for id >= SAM_SPELL_ID_BASE.
	static std::string getIconPath(int spellId);
};
