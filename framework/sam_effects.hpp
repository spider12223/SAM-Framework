/*-------------------------------------------------------------------------------

	S.A.M Framework — Custom status effects (real mechanics)

	Vanilla effects occupy EFF_ASLEEP(0)..EFF_DUCKED(134) in the fixed Uint8
	EFFECTS[NUMEFFECTS=160] array; slots 135..159 are allocated, ticked, saved and
	networked by the engine but UNUSED. SAM registers mod-defined effects into those
	25 slots and feeds their properties (flat attribute modifiers, move-speed multiplier)
	into the existing engine getters through a single guarded arm each.

	IRON RULE: with no mod loaded the registry is empty, SAMEffects::any() is false, and
	every engine hook is dead — vanilla is byte-identical. Do NOT widen NUMEFFECTS (it is
	baked into the wire + save format); reuse the reserved 135..159 range only.

-------------------------------------------------------------------------------*/
#pragma once

#include <string>

struct SAMModManifest; // from sam_workshop.hpp
class Stat;            // Barony stat.hpp

// The reserved custom-effect slot range. MUST match samEffectNameToId in the Lua runtime,
// which already accepts 135..NUMEFFECTS as custom pseudo-effect slots.
static const int SAM_EFFECT_SLOT_BASE = 135;

// Attribute indices for SAMEffects::sumAttrMod, in the class-JSON order STR..CHR.
enum SAMEffectAttr { SAM_EFF_STR = 0, SAM_EFF_DEX, SAM_EFF_CON, SAM_EFF_INT, SAM_EFF_PER, SAM_EFF_CHR };

struct SAMEffectDef
{
	int slot = -1;                    // assigned engine slot 135..159
	std::string id;                   // "mymod:frostbite"
	std::string name;                 // display name (for logs / future HUD)
	std::string tooltip;              // short description
	std::string icon;                 // absolute icon path, or "" (future HUD)
	int attr[6] = { 0, 0, 0, 0, 0, 0 }; // flat STR,DEX,CON,INT,PER,CHR modifiers while active
	int hpPerSecond = 0;              // native per-second HP delta (slice 2)
	int mpPerSecond = 0;              // native per-second MP delta (slice 2)
	double moveSpeedMult = 1.0;       // 1.0 == no change
	int defaultDurationTicks = 0;     // 0 == caller supplies the duration
	bool hudHidden = false;
	std::string modNamespace;
	std::string modPath;
};

namespace SAMEffects
{
	// Load every "effects/*.json" a mod declared. Assigns each a free slot 135..159.
	void loadFromManifest(const SAMModManifest& manifest);
	// Empty the registry (called on mod teardown/reload) -> back to vanilla.
	void clear();
	int  count();

	// Fast guard: is any custom effect registered? Every engine hook checks this first so
	// the whole feature is a compile-time-cheap, runtime-dead no-op in a modless game.
	bool any();

	// Resolve a "namespace:effect" id to its engine slot, or -1 if unknown.
	int  idForName(const std::string& id);
	// The registered def for an engine slot, or nullptr if that slot is not a SAM effect.
	const SAMEffectDef* get(int slot);
	// Display id ("ns:effect") for a slot, or "" if none.
	std::string nameForSlot(int slot);
	int  defaultDurationForSlot(int slot); // 0 if none / unregistered

	// ---- engine hooks (each caller wraps in `if (SAMEffects::any())`) ----------
	// Sum the flat modifier for attribute `attrIndex` (SAMEffectAttr) over every active
	// custom-effect slot on this stat block. 0 when nothing active. Works for any entity.
	int    sumAttrMod(const Stat* s, int attrIndex);
	// Product of move_speed_mult over active custom slots. 1.0 when none active.
	double speedMult(const Stat* s);

	// (Re)insert a HUD display entry for every registered custom effect into the engine's
	// status-effect definition map. Call right after the engine's loadStatusEffectsJSON()
	// (which clears that map on each data reload). No-op when the registry is empty.
	void reapplyDisplayEntries();
}
