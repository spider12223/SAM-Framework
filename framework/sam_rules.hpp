/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_rules.hpp
	Desc: the engine's own numbers, and what a mod is allowed to contribute to them.

	One module rather than three, because these are one idea at three sizes: a
	value Barony computes for itself, which a mod should be able to influence
	WITHOUT overwriting what another mod did. That is the difference between mods
	that stack and mods that fight, and it is the pattern on_damage_multiplier
	proved out in 2.8.

	  * STAT MODIFIERS -- contributions to STR/DEX/CON/INT/PER/CHR, AC, attack and
	    move speed. Barony caches none of these: statGetSTR and friends recompute
	    from scratch at every call site, and each already carries a SAM seam for
	    custom effects. This adds a second, general one beside it.
	  * THE EFFECT GATE -- one hook inside Entity::setEffect that serves both a
	    cancellable before-hook and a real immunity table, because a veto IS an
	    immunity and there is no sense having two mechanisms.
	  * THE XP CURVE -- the level threshold is a hardcoded 100 with the vanilla
	    scaling commented out beside it.

	WHY PLAYERS ARE KEYED BY SLOT AND MONSTERS BY UID.

	A player's entity is destroyed and rebuilt on every floor, so its uid changes
	under you: a modifier keyed by uid would silently evaporate on the stairs and
	the mod author would have no idea why. A player slot is stable for the run. A
	monster has no such handle, so it keys by uid -- and its table is cleared on
	every floor for the same reason the damage-immune set is, since the engine
	rolls uids back for particles and a remembered one can come to mean something
	else.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

class Stat;
class Entity;

namespace SAMRules
{
	// The values a mod may contribute to. Order is load-bearing: it indexes the tables.
	enum StatKind
	{
		SAM_ST_STR = 0, SAM_ST_DEX, SAM_ST_CON, SAM_ST_INT, SAM_ST_PER, SAM_ST_CHR,
		SAM_ST_AC, SAM_ST_ATTACK, SAM_ST_SPEED,
		SAM_ST_COUNT
	};

	// A name for a stat, and back again. -1 for a word that is not one of them.
	int statKindFromName(const char* name);
	const char* statKindName(int kind);

	// ---- stat modifiers -------------------------------------------------------------------
	//
	// Every modifier carries an id so a mod can take back its OWN contribution without
	// disturbing anyone else's. Adding with an id that already exists replaces it, which is
	// what makes a per-tick "recalculate my buff" loop safe.
	//
	// `owner` is the calling mod's namespace, passed by the runtime. Ids are scoped by it, so
	// two mods that pick the same word get two entries, and neither can replace or remove the
	// other's. A null owner in a clear means every mod's -- the loader's teardown, never a script.
	//
	// add is applied first and multiply second, across all mods: (base + sum of adds) * product
	// of multipliers. That ordering is the one Barony uses for its own bonuses, so two mods
	// each giving +2 STR give +4, and two each halving give a quarter.
	bool addPlayerModifier(int player, int kind, const char* owner, const char* id, double add, double mult);
	bool addMonsterModifier(long long uid, int kind, const char* owner, const char* id, double add, double mult);
	int  removePlayerModifier(int player, const char* owner, const char* id);    // every stat carrying that id
	int  removeMonsterModifier(long long uid, const char* owner, const char* id);
	int  clearPlayerModifiers(int player, const char* owner);     // player < 0: every player
	int  clearMonsterModifiers(long long uid, const char* owner); // uid <= 0: every monster

	// What a modifier id currently contributes, for a script that wants to read back rather
	// than remember. Returns false when that id is not present on that stat.
	bool getPlayerModifier(int player, int kind, const char* owner, const char* id, double* add, double* mult);

	// ---- the engine side of the modifiers ---------------------------------------------------
	//
	// anyStatMod() is one bool test, so an unmodded game pays nothing at nine call sites that
	// run constantly. apply() takes the base the engine computed and hands back the modified
	// value; `my` may be null, in which case the target is worked out from the Stat pointer,
	// which is how AC() -- the one that has no Entity -- is served.
	bool anyStatMod();
	double apply(const Stat* s, const Entity* my, int kind, double base);

	// Everything keyed by a creature's uid -- modifiers and per-creature immunities -- is dropped
	// on every floor. Players survive the floor, but not the run:
	void clearMonsterModifiersForNewFloor();
	// A new character, a loaded save included: player modifiers and immunities go. Species
	// immunities and the XP curve stay, being rules a mod declares rather than character state.
	// Every machine runs this for itself; it sends nothing. A multiplayer CLIENT also notes
	// that it wants the host's rules again, and asks for them on its next tick (see below).
	void clearPlayersForNewRun();

	// ---- reaching the owning client ---------------------------------------------------------
	//
	// statGetSTR and the movement code run on the CLIENT too, for its own character sheet and
	// its own walking speed. A host-only table would have the host resolving damage with one
	// number while the client draws and moves with another, so the totals are sent on.
	//
	// A SUMMARY crosses, not the named modifiers: a client never edits the table, it only needs
	// what the totals came to. Sent on every change, and again on each floor. Only when there
	// is something to say: an unmodded game sends nothing. syncAllPlayersToClients re-sends the
	// XP curve and every move-speed multiplier that is not 1 as well.
	//
	// ALL OF IT TRAVELS ON SAMNet's ORDERED CHANNEL (sam_net.hpp), and only to a client that
	// said HELLO. It used to be three raw packets ('SAMM', 'SAMX', 'SAMS') sent to every client,
	// stock ones included, and Barony's reliable packets are unordered: a speed set to 1.5 and
	// back to 1.0 in quick succession could land the other way round and stay wrong for the
	// run. net.cpp keeps the three receive handlers, so this build still understands an older
	// host.
	//
	// WHEN A CLIENT IS CAUGHT UP. A client asks, once per run, after its own new-run reset has
	// emptied what it held (clearPlayersForNewRun). Asking rather than being told is what makes
	// the timing safe: whatever the host sends in answer is applied after that reset, never
	// before it. A curve declared at load time or a buff given in game.on_game_start therefore
	// reaches a client on the first floor, not at the first stairs.
	void syncPlayerToClients(int player);
	void syncAllPlayersToClients();

	// The move-speed multiplier the runtime keeps for `player` (SAMLua::getMoveSpeedMult), to
	// every client. Movement is computed on the machine that owns the player, so a host-set
	// multiplier that stays on the host does nothing at all. The runtime calls this whenever a
	// script changes a multiplier; it sends only when the value differs from what the clients
	// were last told, so a script that re-applies the same speed every tick costs nothing.
	void syncMoveSpeed(int player);

	// The op numbers this package uses on SAMNet: offsets from SAMNet::Op::RulesFirst (72..87).
	// One table, so two files of the package cannot pick the same number. The host->client and
	// client->host spaces are separate, as sam_net.hpp explains.
	namespace NetOp
	{
		// host -> client
		constexpr int Totals       = 0;   // one player's stat-modifier totals            sam_rules.cpp
		constexpr int XpCurve      = 1;   // the XP curve in force                        sam_rules.cpp
		constexpr int MoveSpeed    = 2;   // one player's move-speed multiplier           sam_rules.cpp
		constexpr int FullExp      = 3;   // the receiving player's own EXP, full width   sam_rules.cpp
		constexpr int SpeciesResist = 4;  // every species damage-resist override         sam_combat.cpp
		constexpr int ClassOverlay = 5;   // every class patch and class passive          sam_rules.cpp
		constexpr int Revive       = 6;   // "you have been revived": ghost down, gear    sam_combat.cpp
		constexpr int BindBody     = 7;   // "player n's new body is uid u"               sam_combat.cpp
		constexpr int FollowerName = 8;   // a follower of the receiving player renamed   sam_monsters.cpp
		constexpr int ItemPatches  = 9;   // every item patch (sam_items.cpp, by spec)    sam_items.cpp
		// client -> host
		constexpr int Resync       = 0;   // "I have started a run: send me the rules"    sam_rules.cpp
	}

	// Client side: take the totals off the wire. Bounds-checked by the caller in net.cpp.
	void applyNetSummary(int player, const double* add, const double* mult, int count);

	// ---- effect immunity, and the gate that enforces it -------------------------------------
	//
	// Barony's own immunities are a hardcoded species switch inside Entity::setEffect with no
	// table and no way to ask. This adds one that mods own: each entry remembers which mods
	// asked for it, and a creature stays immune until every one of them has let go.
	bool setPlayerImmunity(int player, int effect, bool on, const char* owner);
	bool setMonsterImmunity(long long uid, int effect, bool on, const char* owner);
	bool setSpeciesImmunity(int species, int effect, bool on, const char* owner);
	bool isImmune(const Stat* s, const Entity* my, int effect);
	int  clearImmunities(const char* owner);   // null: every mod's
	bool anyImmunity();

	// The gate itself, called from Entity::setEffect before its own immunity switch. Returns
	// false when the effect must not be applied at all. May rewrite strength and duration, so
	// a mod can halve a poison rather than only refusing it.
	//
	// Serves the cancellable event AND the immunity table from one site, because a veto and an
	// immunity are the same answer arrived at two ways.
	// effectName is passed in rather than looked up: the engine's EFF_ name table is file-local
	// to entity.cpp, and a second copy of a 135-entry map here is a drift waiting to happen.
	bool effectGate(Entity* e, int effect, const char* effectName,
		int* strength, int* duration, bool wasActive);

	// ---- the XP curve -------------------------------------------------------------------------
	//
	// threshold(level) is what the engine needs to see to grant the next level. Unset levels
	// answer 100, which is vanilla. Setting level -1 sets the flat value for every level.
	//
	// Entries keep their owner. The newest entry for a level is the one in force, and a mod's
	// clear takes back only its own, letting any other mod's curve show through.
	bool setXpThreshold(int level, int threshold, const char* owner);
	int  clearXpCurve(const char* owner);   // null: every mod's
	bool anyXpCurve();
	int  xpThreshold(int level);              // on a client, answers from the curve the host sent

	// The curve reaches clients for their XP bar, which is drawn on their own machine.
	//
	// So does the EXP itself, when it outgrows a byte. Every vanilla 'ATTR' carries EXP as ONE
	// BYTE, which was always enough under vanilla's threshold of 100 -- but under a curve of 500
	// a host EXP of 300 arrived as 44, and the client's bar showed 8% instead of 60%. While the
	// owner's EXP is above 255 the host sends it whole, and the client puts it back each time a
	// vanilla ATTR truncates it again. A stock client keeps the truncated bar; nothing else on it
	// reads EXP, since levelling up is decided on the host.
	void syncXpToClients();
	void applyNetXpCurve(const int* levels, const int* thresholds, int count);
	void clearClientXpCurve();

	// Drop everything. Both loader paths, like every other registry.
	void clear();
}
