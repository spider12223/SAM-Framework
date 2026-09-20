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

	   Nothing in those two touches an engine type. The engine passes uids and a double;
	   this module owns the accumulation and the clamp, and the runtimes own the dispatch.

	3. THE MULTIPLAYER HALF OF THREE COMBAT FUNCTIONS, kept here so both runtimes share
	   one body: the species-resist table reaching a joining client, sam_revive_player for
	   every player slot, and the one-frame sam_set_defending override on a player whose
	   shield input lives on another machine. These do touch the engine, and say why.

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

	// MULTIPLAYER. The table is kept on every machine: the host resolves the damage, and each
	// client's character sheet reads its own copy. The two script functions are `all` calls,
	// so a host call is carried to every client as it happens. What that cannot cover is a
	// table that already differed before the game -- an override left from an earlier
	// singleplayer game on either machine -- so a client's catch-up (SAMRules) sends the host's
	// whole table here and the client takes it wholesale. Host side; one client.
	void sendResistSnapshot(int peer);

	// ---- reviving a player (sam_revive_player) -------------------------------------------
	//
	// The shared body of both runtimes' binding. Returns true when the player is standing again.
	//
	// THE GEAR THE DEATH ALREADY PAID OUT. Dying copies the backpack out -- to the floor in
	// singleplayer, to the loot bag in co-op without keep-inventory -- and leaves the originals
	// where they were, because the engine only ever revives on the next floor, where it purges
	// them. A revive mid-floor has to do that purge itself or every item exists twice.
	//
	// A REMOTE PLAYER is stood up by the host building the body exactly as the engine's own
	// 'REZZ' handler does, and telling that player's machine (S.A.M only) to take down its ghost
	// and drop what the death copied. A stock client cannot be told, and would keep its ghost
	// and a second copy of its gear, so it is refused rather than half-revived.
	bool revivePlayer(int player, int wantX, int wantY);

	// ---- sam_set_defending ------------------------------------------------------------------
	//
	// The flag is rewritten by its owner: every frame by actHudShield for a player on this
	// machine, but for a REMOTE player only by their next 'SHLD' -- sent on a change or every
	// 120 ticks. So the documented one-frame override lasted up to 2.4 seconds on a remote
	// player. The binding reports each write here, and the host puts the player's own value
	// back once one full pass of game logic has seen the override.
	void noteDefendingOverride(int player, bool before, bool forced);

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
