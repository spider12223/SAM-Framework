/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_combat.hpp
	Desc: batch 4 — the two pieces of the combat API that cannot live in a script
	      binding, because the engine has to be able to READ them from inside
	      entity.cpp while the damage is being resolved.

	1. SPECIES DAMAGE RESISTANCE.

	   The obvious implementation is to write `damagetables[species][type]` from the
	   binding. It cannot work, and it fails silently. monster.hpp:497 declares

	       static double damagetables[NUMMONSTERS][7] = { ... };

	   at namespace scope INSIDE A HEADER, so every translation unit that includes it
	   gets its OWN internal-linkage copy. A write performed in sam_lua_runtime.cpp
	   lands in that file's copy; Entity::getDamageTableMultiplier reads entity.cpp's
	   copy and never sees it. Everything would report success and nothing would change.

	   So the override is a table the ENGINE reads, consulted at the one line in
	   getDamageTableMultiplier that seeds the multiplier. `any()` is there so that with
	   no mod loaded the added code is a single bool test against false.

	2. THE DAMAGE-MULTIPLIER HOOK.

	   Entity::modifyDamageMultipliersFromEffects is the choke point: damageMultiplier is
	   passed BY REFERENCE, the attacker and the target are both in scope, and its six
	   call sites cover melee (entity.cpp), arrows (actarrow.cpp) and every magic path.
	   getDamageTableMultiplier looks like the better site and is not — 24 call sites,
	   several of them the character sheet rendering on a CLIENT every frame.

	   Nothing here touches an engine type. The engine passes uids and a double; this
	   module owns the accumulation and the clamp, and the runtimes own the dispatch.

-------------------------------------------------------------------------------*/

#pragma once

namespace SAMCombat
{
	// ---- species damage resistance ------------------------------------------------------
	//
	// multiplier is the same scale as damagetables: 1.0 = normal, 0.5 = half damage taken,
	// 2.0 = double. Refuses a species outside (NOTHING, NUMMONSTERS) or a damage type
	// outside [0, 6], and refuses a non-finite or negative multiplier.
	//
	// THERE IS NO IMMUNITY HERE, whatever number you pass. This value only SEEDS the multiplier;
	// getDamageTableMultiplier then runs its bonus pool over it and ends
	// `return std::max(0.1, 1.0 + bonus)`. Trace an override of 0 through it and the answer is
	// 0.1, not 0 -- a tenth of the damage, every time. A mod that wrote 0 expecting a boss to be
	// untouchable would have shipped one taking 10%. For real immunity use sam_set_damage_immune
	// (per creature) or veto on_before_damage / on_damage_multiplier (per hit).
	bool setSpeciesResist(int species, int damageType, double multiplier);

	// Drop one species+type override, or every override. Returns how many were removed.
	int clearSpeciesResist(int species, int damageType);
	int clearAllSpeciesResist();

	// Fast bail for the engine read site. False whenever no mod has set anything.
	bool anySpeciesResist();

	// The engine read. Returns true and writes *out only when this species+type is
	// overridden; leaves *out alone otherwise.
	bool speciesResist(int species, int damageType, double* out);

	// ---- the on_damage_multiplier hook --------------------------------------------------
	//
	// Called by the engine from Entity::modifyDamageMultipliersFromEffects. Returns true if
	// a script changed the multiplier. `mult` is in/out. Does nothing (and returns false)
	// when no script has registered interest, so the engine site costs one call and one
	// bool test with no mod loaded.
	//
	// spellID is -1 when the damage is not from a spell; projectileUid is 0 when there is
	// no projectile. damageType is the DamageTableType int (0..6).
	bool fireDamageMultiplierHook(long long targetUid, long long attackerUid,
		double& mult, int damageType, long long projectileUid, int spellID);

	// True while a fireDamageMultiplierHook dispatch is open. sam_add_damage_multiplier
	// refuses outside one, so a mod calling it from the wrong place is told rather than
	// having its contribution vanish into a window nobody will close.
	bool multiplierHookActive();

	// A script's contribution. Positive adds, negative multiplies -- the engine's own rule
	// for its allBonuses pool (entity.cpp:32010-32022), so two mods contributing +0.2 give
	// +40% rather than +44%, and two contributing -0.5 give a quarter rather than nothing.
	void addMultiplier(double fraction);

	// Drop everything. Called from both loader paths, like every other registry.
	void clear();
}
