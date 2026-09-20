/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_rules.cpp
	Desc: see sam_rules.hpp.

-------------------------------------------------------------------------------*/

#include "sam_rules.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sam_logger.hpp"
#include "sam_event.hpp"
#include "sam_net.hpp"          // the ordered channel every rule below reaches clients on
#include "sam_lua_runtime.hpp"  // SAMLua::getMoveSpeedMult / applyMoveSpeedMult (the table lives there)
#include "sam_combat.hpp"       // the species-resist snapshot in a client's catch-up
#include "sam_classes.hpp"      // the class-patch snapshot in a client's catch-up
#include "sam_effects.hpp"      // a custom class passive crosses by its "ns:effect" name

#include <cstdint>
#include <cstdlib>   // atoi: a vanilla class or effect number, sent as text beside mods' names

#include "main.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "entity.hpp"
#include "player.hpp"
#include "monster.hpp"
#include "net.hpp"

namespace SAMRules
{
	// ---- names ------------------------------------------------------------------------------

	static const char* const kStatNames[SAM_ST_COUNT] = {
		"STR", "DEX", "CON", "INT", "PER", "CHR", "AC", "ATTACK", "SPEED"
	};

	const char* statKindName(int kind)
	{
		if ( kind < 0 || kind >= SAM_ST_COUNT ) { return "?"; }
		return kStatNames[kind];
	}

	int statKindFromName(const char* name)
	{
		if ( !name ) { return -1; }
		std::string want(name);
		for ( char& c : want ) { c = (char)std::toupper((unsigned char)c); }
		// LVL and the rest are deliberately absent: those are stored numbers rather than
		// computed ones, so a modifier on them would be overwritten by the next write. Use
		// sam_set_stat for those, and say so rather than accepting the word and doing nothing.
		for ( int i = 0; i < SAM_ST_COUNT; ++i )
		{
			if ( want == kStatNames[i] ) { return i; }
		}
		return -1;
	}

	// ---- ownership ------------------------------------------------------------------------------
	//
	// Everything a mod contributes belongs to the mod that made it. Two mods that both call their
	// buff "haste" must not replace each other, and one mod's teardown must not take back
	// another's: that is the whole difference between mods that stack and mods that fight. The
	// owner is the calling script's namespace, filled in by the runtime, so no mod has to spell
	// it or can forge it.

	static const char kOwnerSep = '\x1F';
	static std::string samKey(const char* owner, const char* id)
	{
		std::string k = owner ? owner : "";
		k += kOwnerSep;
		k += id ? id : "";
		return k;
	}
	static std::string samOwnerPrefix(const char* owner)
	{
		std::string k = owner ? owner : "";
		k += kOwnerSep;
		return k;
	}
	static bool samOwnedBy(const std::string& key, const std::string& prefix)
	{
		return key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
	}

	// ---- stat modifiers -----------------------------------------------------------------------

	struct Mod
	{
		double add = 0.0;
		double mult = 1.0;
	};
	// stat -> (owner + id -> contribution). A map per stat so apply() touches one small container.
	typedef std::map<std::string, Mod> ModsById;

	struct Table
	{
		ModsById byStat[SAM_ST_COUNT];
		bool any = false;
		void recount()
		{
			any = false;
			for ( int i = 0; i < SAM_ST_COUNT; ++i )
			{
				if ( !byStat[i].empty() ) { any = true; return; }
			}
		}
	};

	static Table s_player[MAXPLAYERS];
	static std::map<long long, Table> s_monster;
	static bool s_anyStat = false;

	static void refreshAnyStat()
	{
		s_anyStat = false;
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( s_player[i].any ) { s_anyStat = true; return; }
		}
		for ( auto& kv : s_monster )
		{
			if ( kv.second.any ) { s_anyStat = true; return; }
		}
	}

	// What a client was told a player's totals are. A client holds no named modifiers -- it never
	// edits them -- so this is the whole of its table.
	//
	// Multipliers start at 1, not 0. They used to start at 0 and apply() read 0 as "never set",
	// which also swallowed a REAL multiplier of 0: a host freezing a player with SPEED x0 had that
	// player walking normally on their own screen and snapping back.
	struct NetTotals
	{
		double add[SAM_ST_COUNT];
		double mult[SAM_ST_COUNT];
		NetTotals() { reset(); }
		void reset() { for ( int i = 0; i < SAM_ST_COUNT; ++i ) { add[i] = 0.0; mult[i] = 1.0; } }
	};
	static NetTotals s_clientTotals[MAXPLAYERS];
	static bool s_netAny = false;
	// Whether this player's clients were last told something other than "no modifiers". A totals
	// packet goes out only when there is something to say, so a game in which no mod touches a
	// stat sends nothing at all and a stock client never sees the tag.
	static bool s_netSent[MAXPLAYERS] = { false };

	// Every seam asks this first. A CLIENT answers from the totals the host sent: it never holds
	// named modifiers of its own, so asking s_anyStat there made every seam on every client skip,
	// and the whole client path -- the reason the packet exists -- was dead code. A remote player
	// walked at vanilla speed through any SPEED modifier the host set.
	bool anyStatMod() { return ( multiplayer == CLIENT ) ? s_netAny : s_anyStat; }

	// Totals are rounded to what the wire can carry and clamped to what it can hold, on the host
	// as well as in the packet. So the host and a client compute the SAME number from the same
	// modifiers -- the packet used to clamp a total the host went on applying raw.
	static const double kMaxTotalAdd = 1000000.0;
	static const double kMaxTotalMult = 1000000.0;
	static void samSettleTotals(double& add, double& mult)
	{
		if ( add > kMaxTotalAdd ) { add = kMaxTotalAdd; }
		if ( add < -kMaxTotalAdd ) { add = -kMaxTotalAdd; }
		if ( mult > kMaxTotalMult ) { mult = kMaxTotalMult; }
		if ( mult < 0.0 ) { mult = 0.0; }
		add = std::round(add * 100.0) / 100.0;
		mult = std::round(mult * 1000.0) / 1000.0;
	}

	// A contribution that is not a number stops being one for everything downstream, and a NaN in
	// a stat propagates into damage, AC and movement before anything reports it.
	static bool samSaneMod(int kind, double add, double mult, const char* who)
	{
		if ( !std::isfinite(add) || !std::isfinite(mult) )
		{
			SAM_ERROR("RULES", std::string(who) + ": add and multiply both have to be finite numbers.");
			return false;
		}
		if ( mult < 0.0 )
		{
			SAM_ERROR("RULES", std::string(who) + ": a negative multiplier would invert the stat"
				" rather than reduce it. Use 0 for none of it, 0.5 for half.");
			return false;
		}
		if ( std::fabs(add) > 10000.0 || mult > 100.0 )
		{
			SAM_ERROR("RULES", std::string(who) + ": that is far outside any stat the game has (add"
				" is limited to +-10000 and multiply to 100). A number that size is almost always a"
				" unit mistake, and it would overflow the engine's integers downstream.");
			return false;
		}
		// SPEED is scaled from two different bases: a player's speed factor is around 10 to 20, a
		// monster's movement factor is 0 to 1. The same +1 would be +6% on one and +100% on the
		// other, so SPEED takes a multiplier and nothing else.
		if ( kind == SAM_ST_SPEED && add != 0.0 )
		{
			SAM_ERROR("RULES", std::string(who) + ": SPEED takes a multiplier only -- pass 0 for add"
				" and, say, 1.25 for multiply. An add would mean different things on a player and on"
				" a monster.");
			return false;
		}
		return true;
	}

	static bool samStatKindOk(int kind, const char* who)
	{
		if ( kind >= 0 && kind < SAM_ST_COUNT ) { return true; }
		SAM_ERROR("RULES", std::string(who) + ": that is not a stat this can modify. The nine are"
			" STR, DEX, CON, INT, PER, CHR, AC, ATTACK and SPEED.");
		return false;
	}

	static bool samIdOk(const char* id, const char* who)
	{
		if ( id && *id ) { return true; }
		SAM_ERROR("RULES", std::string(who) + ": every modifier needs an id, so your mod can take"
			" back its own contribution later without disturbing anybody else's.");
		return false;
	}

	bool addPlayerModifier(int player, int kind, const char* owner, const char* id, double add, double mult)
	{
		if ( player < 0 || player >= MAXPLAYERS )
		{
			SAM_ERROR("RULES", "sam_add_stat_modifier: " + std::to_string(player) + " is not a player slot.");
			return false;
		}
		if ( !samStatKindOk(kind, "sam_add_stat_modifier") ) { return false; }
		if ( !samIdOk(id, "sam_add_stat_modifier") ) { return false; }
		if ( !samSaneMod(kind, add, mult, "sam_add_stat_modifier") ) { return false; }
		Mod m; m.add = add; m.mult = mult;
		s_player[player].byStat[kind][samKey(owner, id)] = m;
		s_player[player].recount();
		refreshAnyStat();
		syncPlayerToClients(player);
		return true;
	}

	bool addMonsterModifier(long long uid, int kind, const char* owner, const char* id, double add, double mult)
	{
		if ( uid <= 0 || uid > 0x7FFFFFFFLL )
		{
			SAM_ERROR("RULES", "sam_add_monster_stat_modifier: uid " + std::to_string(uid)
				+ " is not one entity.");
			return false;
		}
		if ( !samStatKindOk(kind, "sam_add_monster_stat_modifier") ) { return false; }
		if ( !samIdOk(id, "sam_add_monster_stat_modifier") ) { return false; }
		if ( !samSaneMod(kind, add, mult, "sam_add_monster_stat_modifier") ) { return false; }
		Mod m; m.add = add; m.mult = mult;
		Table& t = s_monster[uid];
		t.byStat[kind][samKey(owner, id)] = m;
		t.recount();
		refreshAnyStat();
		return true;
	}

	bool getPlayerModifier(int player, int kind, const char* owner, const char* id, double* add, double* mult)
	{
		if ( player < 0 || player >= MAXPLAYERS ) { return false; }
		if ( kind < 0 || kind >= SAM_ST_COUNT || !id || !*id ) { return false; }
		auto& m = s_player[player].byStat[kind];
		auto it = m.find(samKey(owner, id));
		if ( it == m.end() ) { return false; }
		if ( add ) { *add = it->second.add; }
		if ( mult ) { *mult = it->second.mult; }
		return true;
	}

	static int samRemoveFrom(Table& t, const std::string& key)
	{
		int n = 0;
		for ( int i = 0; i < SAM_ST_COUNT; ++i ) { n += (int)t.byStat[i].erase(key); }
		t.recount();
		return n;
	}

	// Every entry one mod owns, across every stat. What a scoped clear takes back.
	static int samEraseOwned(Table& t, const std::string& prefix)
	{
		int n = 0;
		for ( int i = 0; i < SAM_ST_COUNT; ++i )
		{
			for ( auto it = t.byStat[i].begin(); it != t.byStat[i].end(); )
			{
				if ( samOwnedBy(it->first, prefix) ) { it = t.byStat[i].erase(it); ++n; }
				else { ++it; }
			}
		}
		t.recount();
		return n;
	}

	int removePlayerModifier(int player, const char* owner, const char* id)
	{
		if ( player < 0 || player >= MAXPLAYERS || !id || !*id ) { return 0; }
		const int n = samRemoveFrom(s_player[player], samKey(owner, id));
		refreshAnyStat();
		syncPlayerToClients(player);
		return n;
	}

	int removeMonsterModifier(long long uid, const char* owner, const char* id)
	{
		if ( !id || !*id ) { return 0; }
		auto it = s_monster.find(uid);
		if ( it == s_monster.end() ) { return 0; }
		const int n = samRemoveFrom(it->second, samKey(owner, id));
		if ( !it->second.any ) { s_monster.erase(it); }
		refreshAnyStat();
		return n;
	}

	static int samCountTable(const Table& t)
	{
		int n = 0;
		for ( int i = 0; i < SAM_ST_COUNT; ++i ) { n += (int)t.byStat[i].size(); }
		return n;
	}

	int clearPlayerModifiers(int player, const char* owner)
	{
		int n = 0;
		const std::string prefix = samOwnerPrefix(owner);
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( player >= 0 && i != player ) { continue; }
			if ( owner ) { n += samEraseOwned(s_player[i], prefix); }
			else { n += samCountTable(s_player[i]); s_player[i] = Table(); }
			syncPlayerToClients(i);
		}
		refreshAnyStat();
		return n;
	}

	int clearMonsterModifiers(long long uid, const char* owner)
	{
		int n = 0;
		const std::string prefix = samOwnerPrefix(owner);
		for ( auto it = s_monster.begin(); it != s_monster.end(); )
		{
			if ( uid > 0 && it->first != uid ) { ++it; continue; }
			if ( owner ) { n += samEraseOwned(it->second, prefix); }
			else { n += samCountTable(it->second); it->second = Table(); }
			if ( !it->second.any ) { it = s_monster.erase(it); }
			else { ++it; }
		}
		refreshAnyStat();
		return n;
	}

	// Which table is this? A player is found by SLOT, because a player's entity is rebuilt on
	// every floor and its uid changes underneath any modifier keyed by it.
	static const Table* samTableFor(const Stat* s, const Entity* my)
	{
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( stats[i] == s ) { return s_player[i].any ? &s_player[i] : nullptr; }
			if ( my && players[i] && players[i]->entity == my )
			{
				return s_player[i].any ? &s_player[i] : nullptr;
			}
		}
		if ( s_monster.empty() ) { return nullptr; }
		if ( my )
		{
			auto it = s_monster.find((long long)const_cast<Entity*>(my)->getUID());
			if ( it != s_monster.end() && it->second.any ) { return &it->second; }
			return nullptr;
		}
		// No entity, only a stat block: AC() is the caller that does this. The table is keyed by
		// uid, so ask each modded monster whether that block is its own. Only monsters a mod has
		// actually modified are in here, so it is a short walk, and it is never taken at all in a
		// game where no mod has touched a monster.
		if ( s )
		{
			for ( auto& kv : s_monster )
			{
				if ( !kv.second.any ) { continue; }
				Entity* owner = uidToEntity((Sint32)kv.first);
				if ( owner && owner->getStats() == s ) { return &kv.second; }
			}
		}
		return nullptr;
	}

	static int samPlayerSlotFor(const Stat* s, const Entity* my)
	{
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( stats[i] == s ) { return i; }
			if ( my && players[i] && players[i]->entity == my ) { return i; }
		}
		return -1;
	}

	double apply(const Stat* s, const Entity* my, int kind, double base)
	{
		if ( kind < 0 || kind >= SAM_ST_COUNT ) { return base; }

		double add = 0.0, mult = 1.0;

		if ( multiplayer == CLIENT )
		{
			// A client has totals rather than modifiers, and only for players -- it holds no
			// Stat for an ordinary monster, so there is nothing of a monster's to modify here.
			// The totals arrive already settled, so this matches the host's number exactly.
			if ( !s_netAny ) { return base; }
			const int slot = samPlayerSlotFor(s, my);
			if ( slot < 0 ) { return base; }
			add = s_clientTotals[slot].add[kind];
			mult = s_clientTotals[slot].mult[kind];
			if ( add == 0.0 && mult == 1.0 ) { return base; }
		}
		else
		{
			if ( !s_anyStat ) { return base; }
			const Table* t = samTableFor(s, my);
			if ( !t ) { return base; }
			const ModsById& m = t->byStat[kind];
			if ( m.empty() ) { return base; }
			// Adds first, then multiplies, across every mod at once. Doing it per-mod instead
			// would make the result depend on which mod happened to register first, which is
			// the exact thing this exists to avoid.
			for ( const auto& kv : m ) { add += kv.second.add; mult *= kv.second.mult; }
			samSettleTotals(add, mult);
		}

		double out = (base + add) * mult;
		if ( !std::isfinite(out) ) { return base; }
		// Every caller narrows this to a 32-bit integer.
		if ( out > 2000000000.0 ) { out = 2000000000.0; }
		if ( out < -2000000000.0 ) { out = -2000000000.0; }
		return out;
	}

	// ---- the wire: stat totals ------------------------------------------------------------------

	void applyNetSummary(int player, const double* add, const double* mult, int count)
	{
		if ( player < 0 || player >= MAXPLAYERS || !add || !mult ) { return; }
		for ( int i = 0; i < SAM_ST_COUNT && i < count; ++i )
		{
			s_clientTotals[player].add[i] = add[i];
			s_clientTotals[player].mult[i] = mult[i];
		}
		s_netAny = false;
		for ( int p = 0; p < MAXPLAYERS && !s_netAny; ++p )
		{
			for ( int i = 0; i < SAM_ST_COUNT; ++i )
			{
				if ( s_clientTotals[p].add[i] != 0.0 || s_clientTotals[p].mult[i] != 1.0 ) { s_netAny = true; break; }
			}
		}
	}

	// ---- the channel ------------------------------------------------------------------------

	static std::uint8_t samRulesOp(int offset) { return (std::uint8_t)(SAMNet::Op::RulesFirst + offset); }

	// One op to every client that runs S.A.M. A client that has not said HELLO yet is skipped
	// rather than queued: it asks for the whole picture itself once it is in its run (see
	// samCatchUp), and a script that sets something every tick would otherwise fill the queue
	// SAMNet holds for it. A stock client is never sent anything.
	static void samRulesToEveryPeer(int offset, const std::string& body)
	{
		if ( multiplayer != SERVER ) { return; }
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( SAMNet::peerHasSam(c) ) { SAMNet::sendToClient(c, samRulesOp(offset), body); }
		}
	}

	// A player's settled totals. Returns true when they are the identity (no modifier at all).
	static bool samPlayerTotals(int player, double* add, double* mult)
	{
		bool identity = true;
		for ( int i = 0; i < SAM_ST_COUNT; ++i )
		{
			add[i] = 0.0; mult[i] = 1.0;
			for ( const auto& kv : s_player[player].byStat[i] )
			{
				add[i] += kv.second.add;
				mult[i] *= kv.second.mult;
			}
			samSettleTotals(add[i], mult[i]);
			if ( add[i] != 0.0 || mult[i] != 1.0 ) { identity = false; }
		}
		return identity;
	}

	// Body: [u8 player] then per stat (u32 add x100 as a signed value, u32 multiply x1000). The
	// same numbers the old 'SAMM' packet carried. Settled totals, so these multiply out to whole
	// numbers and fit: add x100 within +-1e8 and multiply x1000 within 1e9, both inside 32 bits.
	static std::string samTotalsBody(int player, const double* add, const double* mult)
	{
		SAMNet::Writer w;
		w.u8((std::uint8_t)player);
		for ( int i = 0; i < SAM_ST_COUNT; ++i )
		{
			w.u32((std::uint32_t)(std::int32_t)std::lround(add[i] * 100.0));
			w.u32((std::uint32_t)std::lround(mult[i] * 1000.0));
		}
		return w.buf;
	}

	void syncPlayerToClients(int player)
	{
		if ( multiplayer != SERVER ) { return; }
		if ( player < 0 || player >= MAXPLAYERS ) { return; }

		double add[SAM_ST_COUNT], mult[SAM_ST_COUNT];
		const bool identity = samPlayerTotals(player, add, mult);

		// Nothing to say, and nothing said earlier that needs taking back: stay silent. This is
		// what keeps an unmodded game off the wire. The floor-change resend used to send nine
		// identity pairs to every client on every floor, stock clients included.
		if ( identity && !s_netSent[player] ) { return; }
		s_netSent[player] = !identity;

		samRulesToEveryPeer(NetOp::Totals, samTotalsBody(player, add, mult));
	}

	// ---- the wire: move speed ---------------------------------------------------------------------
	//
	// The multiplier table itself lives in the runtime (it is read from the movement inner loop).
	// This only remembers what the clients were last told, so an unchanged value is not re-sent.

	struct SpeedSent
	{
		double v[MAXPLAYERS];
		SpeedSent() { reset(); }
		void reset() { for ( int i = 0; i < MAXPLAYERS; ++i ) { v[i] = 1.0; } }   // what resetMoveSpeed sets
	};
	static SpeedSent s_speedSent;

	// Body: [u8 player][u32 multiplier x1000 as a signed value], the old 'SAMS' encoding.
	static std::string samSpeedBody(int player, double mult)
	{
		SAMNet::Writer w;
		w.u8((std::uint8_t)player);
		w.u32((std::uint32_t)(std::int32_t)std::lround(mult * 1000.0));
		return w.buf;
	}

	void syncMoveSpeed(int player)
	{
		if ( multiplayer != SERVER ) { return; }
		if ( player < 0 || player >= MAXPLAYERS ) { return; }
		const double v = SAMLua::getMoveSpeedMult(player);
		if ( v == s_speedSent.v[player] ) { return; }
		s_speedSent.v[player] = v;
		// To EVERY client, not only the owner's. The owner is the one whose movement uses it;
		// the others hold it so that no machine in the game carries a number the host does not.
		samRulesToEveryPeer(NetOp::MoveSpeed, samSpeedBody(player, v));
	}

	void syncAllPlayersToClients()
	{
		if ( multiplayer != SERVER ) { return; }
		for ( int i = 0; i < MAXPLAYERS; ++i ) { syncPlayerToClients(i); }
		syncXpToClients();
		// And every multiplier that is not 1, whether or not it changed: the channel is ordered
		// and reliable, so this is belt and braces, and it costs nothing when no mod set a speed.
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			const double v = SAMLua::getMoveSpeedMult(i);
			if ( v != 1.0 ) { s_speedSent.v[i] = v; samRulesToEveryPeer(NetOp::MoveSpeed, samSpeedBody(i, v)); }
		}
	}

	// ---- effect immunity ------------------------------------------------------------------------

	// effect id -> the mods that asked for it. Kept per player slot, per monster uid and per
	// species. A creature is immune while ANY mod says so, and one mod switching its immunity
	// off leaves another mod's in place -- a plain flag let the second mod silently cancel the
	// first, which is the fight this module exists to prevent.
	typedef std::set<std::string> Owners;
	typedef std::map<int, Owners> ImmTable;
	static ImmTable s_immPlayer[MAXPLAYERS];
	static std::map<long long, ImmTable> s_immMonster;
	static std::map<int, ImmTable> s_immSpecies;
	static bool s_anyImm = false;

	static void samImmSet(ImmTable& t, int effect, bool on, const char* owner)
	{
		const std::string o = owner ? owner : "";
		if ( on ) { t[effect].insert(o); return; }
		auto it = t.find(effect);
		if ( it == t.end() ) { return; }
		it->second.erase(o);
		if ( it->second.empty() ) { t.erase(it); }
	}

	static bool samImmHas(const ImmTable& t, int effect)
	{
		auto it = t.find(effect);
		return it != t.end() && !it->second.empty();
	}

	// Removes one owner everywhere in a table (or everyone, owner null). Counts what went.
	static int samImmClear(ImmTable& t, const char* owner)
	{
		int n = 0;
		for ( auto it = t.begin(); it != t.end(); )
		{
			if ( owner ) { n += (int)it->second.erase(owner); }
			else { n += (int)it->second.size(); it->second.clear(); }
			if ( it->second.empty() ) { it = t.erase(it); }
			else { ++it; }
		}
		return n;
	}

	static void refreshAnyImm()
	{
		s_anyImm = false;
		for ( int i = 0; i < MAXPLAYERS; ++i ) { if ( !s_immPlayer[i].empty() ) { s_anyImm = true; return; } }
		if ( !s_immMonster.empty() || !s_immSpecies.empty() ) { s_anyImm = true; }
	}

	bool anyImmunity() { return s_anyImm; }

	static bool samEffectOk(int effect, const char* who)
	{
		if ( effect >= 0 && effect < NUMEFFECTS ) { return true; }
		SAM_ERROR("RULES", std::string(who) + ": " + std::to_string(effect)
			+ " is not an effect this game has.");
		return false;
	}

	bool setPlayerImmunity(int player, int effect, bool on, const char* owner)
	{
		if ( player < 0 || player >= MAXPLAYERS )
		{
			SAM_ERROR("RULES", "sam_set_immunity: " + std::to_string(player) + " is not a player slot.");
			return false;
		}
		if ( !samEffectOk(effect, "sam_set_immunity") ) { return false; }
		samImmSet(s_immPlayer[player], effect, on, owner);
		refreshAnyImm();
		return true;
	}

	bool setMonsterImmunity(long long uid, int effect, bool on, const char* owner)
	{
		if ( uid <= 0 || uid > 0x7FFFFFFFLL )
		{
			SAM_ERROR("RULES", "sam_set_monster_immunity: uid " + std::to_string(uid) + " is not one entity.");
			return false;
		}
		if ( !samEffectOk(effect, "sam_set_monster_immunity") ) { return false; }
		if ( on ) { samImmSet(s_immMonster[uid], effect, true, owner); }
		else
		{
			auto it = s_immMonster.find(uid);
			if ( it != s_immMonster.end() )
			{
				samImmSet(it->second, effect, false, owner);
				if ( it->second.empty() ) { s_immMonster.erase(it); }
			}
		}
		refreshAnyImm();
		return true;
	}

	bool setSpeciesImmunity(int species, int effect, bool on, const char* owner)
	{
		if ( species <= 0 || species >= NUMMONSTERS )
		{
			SAM_ERROR("RULES", "sam_set_species_immunity: " + std::to_string(species)
				+ " is not a creature this game has.");
			return false;
		}
		if ( !samEffectOk(effect, "sam_set_species_immunity") ) { return false; }
		if ( on ) { samImmSet(s_immSpecies[species], effect, true, owner); }
		else
		{
			auto it = s_immSpecies.find(species);
			if ( it != s_immSpecies.end() )
			{
				samImmSet(it->second, effect, false, owner);
				if ( it->second.empty() ) { s_immSpecies.erase(it); }
			}
		}
		refreshAnyImm();
		return true;
	}

	bool isImmune(const Stat* s, const Entity* my, int effect)
	{
		if ( !s_anyImm || effect < 0 ) { return false; }
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			const bool isThisPlayer = ( stats[i] == s )
				|| ( my && players[i] && players[i]->entity == my );
			if ( isThisPlayer )
			{
				if ( samImmHas(s_immPlayer[i], effect) ) { return true; }
				break;
			}
		}
		if ( my && !s_immMonster.empty() )
		{
			auto it = s_immMonster.find((long long)const_cast<Entity*>(my)->getUID());
			if ( it != s_immMonster.end() && samImmHas(it->second, effect) ) { return true; }
		}
		if ( s && !s_immSpecies.empty() )
		{
			auto it = s_immSpecies.find((int)s->type);
			if ( it != s_immSpecies.end() && samImmHas(it->second, effect) ) { return true; }
		}
		return false;
	}

	int clearImmunities(const char* owner)
	{
		int n = 0;
		for ( int i = 0; i < MAXPLAYERS; ++i ) { n += samImmClear(s_immPlayer[i], owner); }
		for ( auto it = s_immMonster.begin(); it != s_immMonster.end(); )
		{
			n += samImmClear(it->second, owner);
			if ( it->second.empty() ) { it = s_immMonster.erase(it); } else { ++it; }
		}
		for ( auto it = s_immSpecies.begin(); it != s_immSpecies.end(); )
		{
			n += samImmClear(it->second, owner);
			if ( it->second.empty() ) { it = s_immSpecies.erase(it); } else { ++it; }
		}
		refreshAnyImm();
		return n;
	}

	// ---- lifetimes ----------------------------------------------------------------------------------

	// Everything keyed by a creature's uid, dropped together: the modifiers AND the per-creature
	// immunities, which share the reason and so the lifetime. The immunities used to be left
	// behind -- documented as dying with the floor and never cleared -- so a uid the next floor
	// handed to something else arrived immune to whatever its previous owner was.
	//
	// Called BEFORE the next floor loads, not after: followers are re-summoned during the load
	// and fire world.on_monster_spawned, and a clear after that erased what a handler had just
	// given them.
	void clearMonsterModifiersForNewFloor()
	{
		if ( !s_immMonster.empty() ) { s_immMonster.clear(); refreshAnyImm(); }
		if ( s_monster.empty() ) { return; }
		s_monster.clear();
		refreshAnyStat();
	}

	// A new character. Player modifiers and immunities are keyed by SLOT so that they survive
	// the stairs, which also meant they survived into the next run: the last character's buffs,
	// curses and immunities landed on whoever took that slot next. None of it is saved, so it
	// resets on every new run, a loaded save included, exactly like the per-player script data
	// cleared beside it. game.on_game_start fires right after, which is where a mod re-applies
	// what it means to keep.
	//
	// Species immunities and the XP curve are left alone. Those are rules a mod may declare once
	// when it loads, and clearing them here would silently undo that mod from the second run on.
	//
	// Nothing is SENT from here. Each machine runs this for itself at the start of its own run.
	// Sending from here reached the next game's clients -- stock ones among them -- with the
	// previous game's leftovers. Instead a CLIENT asks the host for its rules once this run is
	// under way (s_wantResync, sent from the tick), and the host answers with what is live.
	// That order is the point: the answer is applied after this reset, never wiped by it --
	// which a packet sent from the host's own doNewGame could not promise, since a client may
	// still be finishing the previous run when the host starts the next one.
	static bool s_wantResync = false;          // client: ask the host for its rules on the next tick
	static long long s_fullExp = -1;           // client: our own EXP as the host last sent it whole
	struct ExpSent
	{
		long long v[MAXPLAYERS];
		ExpSent() { reset(); }
		void reset() { for ( int i = 0; i < MAXPLAYERS; ++i ) { v[i] = -1; } }
	};
	static ExpSent s_expSent;                  // host: each remote player's EXP as last reported to them

	void clearPlayersForNewRun()
	{
		for ( int i = 0; i < MAXPLAYERS; ++i ) { s_player[i] = Table(); s_immPlayer[i].clear(); }
		refreshAnyStat();
		refreshAnyImm();
		for ( int p = 0; p < MAXPLAYERS; ++p ) { s_clientTotals[p].reset(); s_netSent[p] = false; }
		s_netAny = false;
		clearClientXpCurve();
		// doNewGame puts every multiplier back to 1 on every machine (SAMLua::resetMoveSpeed),
		// so "what the clients were told" is 1 again too. Without this a buff re-applied at the
		// same value in the next run's game.on_game_start was taken for "unchanged" and never sent.
		s_speedSent.reset();
		s_expSent.reset();
		s_fullExp = -1;
		s_wantResync = ( multiplayer == CLIENT );
	}

	// ---- the gate ---------------------------------------------------------------------------------

	static bool s_gateActive = false;   // re-entrancy: a handler may apply effects of its own

	bool effectGate(Entity* e, int effect, const char* effectName,
		int* strength, int* duration, bool wasActive)
	{
		if ( !e || !strength || !duration ) { return true; }
		Stat* s = e->getStats();
		if ( !s ) { return true; }

		bool immune = isImmune(s, e, effect);

		// Nothing loaded and nothing immune: one bool test each and out. This sits inside
		// Entity::setEffect, which runs for every buff, poison tick and shrine in the game.
		if ( !immune && !SamEvent::anyScripts() ) { return true; }

		// A handler is free to apply effects of its own, which re-enters setEffect and arrives
		// back here. Without this the inner dispatch would decide the outer effect's fate.
		if ( s_gateActive ) { return !immune; }

		if ( !SamEvent::anyScripts() ) { return !immune; }

		s_gateActive = true;

		const long long origStrength = (long long)*strength;
		const long long origDuration = (long long)*duration;
		const int player = ( e->behavior == &actPlayer ) ? e->skill[2] : -1;

		SamEvent ev("on_before_effect_applied");
		ev.i("uid", (long long)e->getUID());
		ev.i("player", (long long)player);
		ev.i("effect", (long long)effect);
		ev.s("effect_name", effectName ? effectName : "");
		ev.i("strength", origStrength);
		ev.i("duration_ticks", origDuration);
		ev.i("was_active", wasActive ? 1 : 0);
		// Pre-filled from the immunity tables so a handler can SEE that something is already
		// immune -- and clear it by writing 0, which is the only way to make a mod's immunity
		// conditional on anything.
		ev.i("immune", immune ? 1 : 0);
		const bool allowed = ev.fire();

		const long long wantStrength = ev.get("strength", origStrength);
		const long long wantDuration = ev.get("duration_ticks", origDuration);
		const long long wantImmune = ev.get("immune", immune ? 1 : 0);

		s_gateActive = false;

		if ( !allowed ) { return false; }

		if ( wantStrength != origStrength )
		{
			long long v = wantStrength;
			if ( v < 0 ) { v = 0; }
			if ( v > 255 ) { v = 255; }   // the engine stores strength in a Uint8
			*strength = (int)v;
		}
		if ( wantDuration != origDuration )
		{
			// The engine only counts DOWN a timer that is above zero, so 0 and every negative
			// number leave an effect on forever. A handler writing 0 means "no time at all", and
			// used to get a permanent effect instead: 0 now refuses it, like strength 0 does.
			// A negative number is the explicit way to ask for permanent, and stores -1, the
			// value the engine and sam_get_effect_duration already use for that.
			if ( wantDuration == 0 ) { return false; }
			long long v = wantDuration;
			if ( v < 0 ) { v = -1; }
			if ( v > 0x7FFFFFFFLL ) { v = 0x7FFFFFFFLL; }
			*duration = (int)v;
		}

		immune = ( wantImmune != 0 );
		return !immune;
	}

	// ---- the XP curve --------------------------------------------------------------------------
	//
	// One threshold per level, but several mods may each have set one. Every entry keeps its
	// owner, and the most recent entry for a level is the one in force -- so a mod's clear takes
	// back its own curve and lets the one underneath show through, instead of wiping everyone's.

	struct XpEntry { std::string owner; int threshold; };
	static std::map<int, std::vector<XpEntry> > s_xpByLevel;   // level -> entries, newest last
	static std::map<int, int> s_xpEffective;                   // derived: level -> threshold in force
	static std::map<int, int> s_clientXp;                      // what a client was told
	static bool s_xpSent = false;

	static void samXpRebuild()
	{
		s_xpEffective.clear();
		for ( const auto& kv : s_xpByLevel )
		{
			if ( !kv.second.empty() ) { s_xpEffective[kv.first] = kv.second.back().threshold; }
		}
	}

	static const std::map<int, int>& samXpInForce()
	{
		return ( multiplayer == CLIENT ) ? s_clientXp : s_xpEffective;
	}

	bool anyXpCurve() { return !samXpInForce().empty(); }

	bool setXpThreshold(int level, int threshold, const char* owner)
	{
		if ( threshold < 1 )
		{
			SAM_ERROR("RULES", "sam_set_xp_curve: a threshold under 1 would level the player up on"
				" every tick, forever -- the engine grants a level whenever EXP reaches the threshold,"
				" and EXP never drops below 0.");
			return false;
		}
		// EXP is a Sint32 and the level-up drains it by the threshold, so an enormous threshold
		// is harmless -- but a level index outside what a run can reach is a mistake worth naming
		// rather than storing forever.
		if ( level < -1 || level > 1000 )
		{
			SAM_ERROR("RULES", "sam_set_xp_curve: level " + std::to_string(level)
				+ " is outside 0..1000. Use -1 to set the flat threshold for every level.");
			return false;
		}
		const std::string o = owner ? owner : "";
		auto& v = s_xpByLevel[level];
		for ( auto it = v.begin(); it != v.end(); )
		{
			if ( it->owner == o ) { it = v.erase(it); } else { ++it; }
		}
		v.push_back(XpEntry{ o, threshold });
		samXpRebuild();
		syncXpToClients();
		return true;
	}

	int clearXpCurve(const char* owner)
	{
		int n = 0;
		for ( auto it = s_xpByLevel.begin(); it != s_xpByLevel.end(); )
		{
			auto& v = it->second;
			if ( !owner ) { n += (int)v.size(); v.clear(); }
			else
			{
				for ( auto e = v.begin(); e != v.end(); )
				{
					if ( e->owner == owner ) { e = v.erase(e); ++n; } else { ++e; }
				}
			}
			if ( v.empty() ) { it = s_xpByLevel.erase(it); } else { ++it; }
		}
		samXpRebuild();
		syncXpToClients();
		return n;
	}

	int xpThreshold(int level)
	{
		const std::map<int, int>& m = samXpInForce();
		if ( m.empty() ) { return 100; }
		auto it = m.find(level);
		if ( it != m.end() ) { return it->second; }
		it = m.find(-1);
		if ( it != m.end() ) { return it->second; }
		return 100;   // vanilla
	}

	// The curve crosses to clients for one reason: their XP bar is drawn on their own machine,
	// against EXP / threshold. Without it a client's bar read as vanilla's 100 whatever the curve
	// said. The level-up itself only ever runs on the host.
	//
	// Body: [u16 entry count], then per entry (u16 level as a signed value, u32 threshold). A
	// body of any size crosses the channel, so the old one-packet limit of 84 levels is gone.
	static std::string samXpBody()
	{
		SAMNet::Writer w;
		// Levels run -1..1000 (setXpThreshold refuses the rest), so this always fits a u16.
		w.u16((std::uint16_t)s_xpEffective.size());
		for ( const auto& kv : s_xpEffective )
		{
			w.u16((std::uint16_t)(std::int16_t)kv.first);
			w.u32((std::uint32_t)kv.second);
		}
		return w.buf;
	}

	void syncXpToClients()
	{
		if ( multiplayer != SERVER ) { return; }
		if ( s_xpEffective.empty() && !s_xpSent ) { return; }   // nothing to say, nothing to take back
		s_xpSent = !s_xpEffective.empty();
		samRulesToEveryPeer(NetOp::XpCurve, samXpBody());
	}

	void applyNetXpCurve(const int* levels, const int* thresholds, int count)
	{
		s_clientXp.clear();
		if ( !levels || !thresholds ) { return; }
		for ( int i = 0; i < count; ++i )
		{
			if ( thresholds[i] >= 1 ) { s_clientXp[levels[i]] = thresholds[i]; }
		}
	}

	void clearClientXpCurve() { s_clientXp.clear(); }

	// ---- multiplayer: the full EXP -------------------------------------------------------------
	//
	// See syncXpToClients in the header. The host watches each S.A.M client's EXP and sends it
	// whole whenever it changes while it is -- or has just been -- too big for the ATTR byte.

	static std::string samU32Body(std::uint32_t v)
	{
		SAMNet::Writer w;
		w.u32(v);
		return w.buf;
	}

	static void samHostSendFullExp()
	{
		for ( int p = 1; p < MAXPLAYERS; ++p )
		{
			if ( !SAMNet::peerHasSam(p) || !stats[p] ) { continue; }
			const long long exp = (long long)stats[p]->EXP;
			const long long last = s_expSent.v[p];
			if ( exp == last ) { continue; }
			s_expSent.v[p] = exp;
			// ATTR carries 0..255 exactly, so only a value past that needs sending -- and the
			// first one back inside it, which tells the client to stop restoring the big number.
			if ( exp <= 255 && last <= 255 ) { continue; }
			SAMNet::sendToClient(p, samRulesOp(NetOp::FullExp), samU32Body((std::uint32_t)(std::int32_t)exp));
		}
	}

	// ---- multiplayer: hunger ------------------------------------------------------------------
	//
	// Clients never tick hunger; the host does, and sends a client its own HUNGER ('HNGR') only
	// when it crosses a tier edge -- every 100 points, every 25 between 300 and 600 -- so a
	// script on that client could read a value a hundred points stale. Every five seconds a
	// changed value goes out by the engine's own sender. HNGR is absolute, so it cannot apply
	// twice, and only S.A.M clients are sent the extra copies: nothing else there reads it.
	struct HungerSent
	{
		Sint32 v[MAXPLAYERS];
		HungerSent() { reset(); }
		void reset() { for ( int i = 0; i < MAXPLAYERS; ++i ) { v[i] = -1; } }
	};
	static HungerSent s_hungerSent;

	static void samHostRefreshHunger()
	{
		if ( ticks % (Uint32)(5 * TICKS_PER_SECOND) != 0 ) { return; }
		for ( int p = 1; p < MAXPLAYERS; ++p )
		{
			if ( !SAMNet::peerHasSam(p) || !stats[p] ) { continue; }
			if ( stats[p]->HUNGER == s_hungerSent.v[p] ) { continue; }
			s_hungerSent.v[p] = stats[p]->HUNGER;
			serverUpdateHunger(p);
		}
	}

	// ---- multiplayer: the class overlay (sam_patch_class, class passives) ---------------------
	//
	// Class patches are an `all` table: every machine keeps its own copy, and the host's calls
	// are carried to every client. That keeps copies equal for calls made DURING a game. It
	// cannot help with one made before: patches last until mods reload, so a runtime patch from
	// an earlier singleplayer game stays on the machine that made it. So a client's catch-up
	// carries the host's whole overlay and the client takes it wholesale.
	//
	// A mod's class and a mod's effect cross by NAME. Their numbers are handed out in load
	// order, so the host's number need not be this machine's; vanilla numbers are fixed.

	static bool samAllDigits(const std::string& s)
	{
		if ( s.empty() || s.size() > 6 ) { return false; }
		for ( char c : s ) { if ( c < '0' || c > '9' ) { return false; } }
		return true;
	}

	static std::string samClassKey(int classnum)
	{
		if ( classnum >= SAM_CLASS_ID_BASE )
		{
			const SAMClassDef* def = SAMClasses::getClass(classnum);
			return def ? def->id : std::string();
		}
		return std::to_string(classnum);
	}

	static int samClassFromKey(const std::string& key)
	{
		if ( samAllDigits(key) )
		{
			const int n = std::atoi(key.c_str());
			return ( n >= 0 && n < NUMCLASSES ) ? n : -1;
		}
		return key.empty() ? -1 : SAMClasses::classIdForIdString(key);
	}

	static std::string samEffectKey(int effect)
	{
		if ( effect >= SAM_EFFECT_SLOT_BASE )
		{
			const std::string n = SAMEffects::nameForSlot(effect);
			if ( !n.empty() ) { return n; }
		}
		return std::to_string(effect);
	}

	static int samEffectFromKey(const std::string& key)
	{
		if ( samAllDigits(key) )
		{
			const int n = std::atoi(key.c_str());
			return ( n >= 0 && n < NUMEFFECTS ) ? n : -1;
		}
		return key.empty() ? -1 : SAMEffects::idForName(key);
	}

	// Body: [u16 patches] per patch (str8 class, u16 stats {str8 key, u32 value}, u16 skills
	// {str8 key, u32 value}), then [u16 classes with passives] per class (str8 class, u16 n
	// {str8 effect}). Values are signed and cross as their 32-bit pattern.
	static std::string samClassOverlayBody()
	{
		std::map<int, SAMClassStatPatch> patches;
		std::map<int, std::vector<int> > passives;
		SAMClasses::overlaySnapshot(patches, passives);

		SAMNet::Writer w;
		std::vector<std::pair<std::string, const SAMClassStatPatch*> > p;
		for ( const auto& kv : patches )
		{
			const std::string key = samClassKey(kv.first);
			if ( !key.empty() ) { p.emplace_back(key, &kv.second); }
		}
		w.u16((std::uint16_t)p.size());
		for ( const auto& e : p )
		{
			w.str8(e.first);
			w.u16((std::uint16_t)e.second->stats.size());
			for ( const auto& s : e.second->stats ) { w.str8(s.first); w.u32((std::uint32_t)(std::int32_t)s.second); }
			w.u16((std::uint16_t)e.second->skills.size());
			for ( const auto& s : e.second->skills ) { w.str8(s.first); w.u32((std::uint32_t)(std::int32_t)s.second); }
		}
		std::vector<std::pair<std::string, const std::vector<int>*> > q;
		for ( const auto& kv : passives )
		{
			const std::string key = samClassKey(kv.first);
			if ( !key.empty() ) { q.emplace_back(key, &kv.second); }
		}
		w.u16((std::uint16_t)q.size());
		for ( const auto& e : q )
		{
			w.str8(e.first);
			w.u16((std::uint16_t)e.second->size());
			for ( int eff : *e.second ) { w.str8(samEffectKey(eff)); }
		}
		return w.buf;
	}

	static void samOnClassOverlay(const std::string& body)
	{
		SAMNet::Reader r(body);
		std::map<int, SAMClassStatPatch> patches;
		std::map<int, std::vector<int> > passives;
		const int nPatches = r.u16();
		for ( int i = 0; i < nPatches && r.ok; ++i )
		{
			const std::string key = r.str8();
			SAMClassStatPatch patch;
			const int nStats = r.u16();
			for ( int k = 0; k < nStats && r.ok; ++k )
			{
				std::string sk = r.str8();
				patch.stats[sk] = (int)(std::int32_t)r.u32();
			}
			const int nSkills = r.u16();
			for ( int k = 0; k < nSkills && r.ok; ++k )
			{
				std::string sk = r.str8();
				patch.skills[sk] = (int)(std::int32_t)r.u32();
			}
			const int cls = samClassFromKey(key);
			if ( cls < 0 )
			{
				SAMNet::warnOnce("rules:class:" + key, "The host patched class '" + key
					+ "', which this machine does not have; that patch was not copied here.");
				continue;
			}
			patches[cls] = patch;
		}
		const int nPassive = r.u16();
		for ( int i = 0; i < nPassive && r.ok; ++i )
		{
			const std::string key = r.str8();
			const int n = r.u16();
			std::vector<int> effects;
			for ( int k = 0; k < n && r.ok; ++k )
			{
				const std::string ek = r.str8();
				const int eff = samEffectFromKey(ek);
				if ( eff >= 0 ) { effects.push_back(eff); }
				else
				{
					SAMNet::warnOnce("rules:passive:" + ek, "The host gives a class the effect '" + ek
						+ "', which this machine does not have; that passive was not copied here.");
				}
			}
			const int cls = samClassFromKey(key);
			if ( cls >= 0 && !effects.empty() ) { passives[cls] = effects; }
		}
		// A body cut short is dropped whole: replacing the table with half of the host's would
		// be worse than keeping this machine's own until the next catch-up.
		if ( !r.ok ) { return; }
		SAMClasses::overlayReplace(patches, passives);
	}

	// ---- multiplayer: a client's catch-up -------------------------------------------------------
	//
	// Everything a client should hold, for one client that has just asked (NetOp::Resync). Only
	// what differs from a fresh run is sent -- the client emptied its copies before asking -- apart
	// from the two tables every machine keeps (species resists, class overlay), which always go,
	// because a client's copy of those can carry leftovers from an earlier game of its own.
	static void samCatchUp(int peer)
	{
		if ( multiplayer != SERVER || !SAMNet::peerHasSam(peer) ) { return; }
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			double add[SAM_ST_COUNT], mult[SAM_ST_COUNT];
			if ( !samPlayerTotals(p, add, mult) )
			{
				SAMNet::sendToClient(peer, samRulesOp(NetOp::Totals), samTotalsBody(p, add, mult));
			}
		}
		if ( !s_xpEffective.empty() )
		{
			SAMNet::sendToClient(peer, samRulesOp(NetOp::XpCurve), samXpBody());
		}
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			const double v = SAMLua::getMoveSpeedMult(p);
			if ( v != 1.0 ) { SAMNet::sendToClient(peer, samRulesOp(NetOp::MoveSpeed), samSpeedBody(p, v)); }
		}
		if ( peer < MAXPLAYERS && stats[peer] )
		{
			const long long exp = (long long)stats[peer]->EXP;
			s_expSent.v[peer] = exp;
			if ( exp > 255 )
			{
				SAMNet::sendToClient(peer, samRulesOp(NetOp::FullExp), samU32Body((std::uint32_t)(std::int32_t)exp));
			}
		}
		SAMCombat::sendResistSnapshot(peer);
		SAMNet::sendToClient(peer, samRulesOp(NetOp::ClassOverlay), samClassOverlayBody());
	}

	// ---- multiplayer: receiving ------------------------------------------------------------------

	static void samOnTotals(const std::string& body)
	{
		SAMNet::Reader r(body);
		const int player = (int)r.u8();
		double add[SAM_ST_COUNT], mult[SAM_ST_COUNT];
		for ( int i = 0; i < SAM_ST_COUNT; ++i )
		{
			add[i] = (double)(std::int32_t)r.u32() / 100.0;
			mult[i] = (double)r.u32() / 1000.0;
		}
		if ( !r.ok || player >= MAXPLAYERS ) { return; }
		applyNetSummary(player, add, mult, SAM_ST_COUNT);
	}

	static void samOnXpCurve(const std::string& body)
	{
		SAMNet::Reader r(body);
		const int count = (int)r.u16();
		std::vector<int> levels, thresholds;
		for ( int i = 0; i < count && r.ok; ++i )
		{
			const int level = (int)(std::int16_t)r.u16();
			const int threshold = (int)r.u32();
			levels.push_back(level);
			thresholds.push_back(threshold);
		}
		if ( !r.ok ) { return; }
		// An empty curve is the host taking its curve back, and clears ours.
		if ( levels.empty() ) { clearClientXpCurve(); return; }
		applyNetXpCurve(levels.data(), thresholds.data(), (int)levels.size());
	}

	static void samOnMoveSpeed(const std::string& body)
	{
		SAMNet::Reader r(body);
		const int player = (int)r.u8();
		const double mult = (double)(std::int32_t)r.u32() / 1000.0;
		if ( !r.ok || player >= MAXPLAYERS ) { return; }
		SAMLua::applyMoveSpeedMult(player, mult);
	}

	static void samOnFullExp(const std::string& body)
	{
		SAMNet::Reader r(body);
		const long long exp = (long long)(std::int32_t)r.u32();
		if ( !r.ok || clientnum < 0 || clientnum >= MAXPLAYERS || !stats[clientnum] ) { return; }
		s_fullExp = exp;
		stats[clientnum]->EXP = (Sint32)exp;
	}

	static void samOnResync(int from, const std::string& /*body*/)
	{
		samCatchUp(from);
	}

	// Every tick, on every machine, while scripts are loaded and a multiplayer game is running.
	static void samRulesTick()
	{
		if ( multiplayer == SERVER )
		{
			samHostSendFullExp();
			samHostRefreshHunger();
		}
		else if ( multiplayer == CLIENT )
		{
			if ( s_wantResync && SAMNet::sendToHost(samRulesOp(NetOp::Resync), std::string()) )
			{
				s_wantResync = false;
			}
			// A vanilla 'ATTR' (a kill, a skill-up) writes our EXP back as its low byte. When that
			// byte is the low byte of the full value the host sent, it is that value truncated,
			// not a new one -- put the whole number back. A NEW value has a new low byte, and the
			// host sends it whole on the same tick.
			if ( s_fullExp > 255 && clientnum >= 0 && clientnum < MAXPLAYERS && stats[clientnum] )
			{
				Sint32& exp = stats[clientnum]->EXP;
				if ( (long long)exp != s_fullExp && (exp & 0xFF) == (Sint32)(s_fullExp & 0xFF) )
				{
					exp = (Sint32)s_fullExp;
				}
			}
		}
	}

	// A game ended: forget what any peer was told.
	static void samRulesNetClear()
	{
		s_wantResync = false;
		s_fullExp = -1;
		s_expSent.reset();
		s_hungerSent.reset();
		s_speedSent.reset();
	}

	struct SamRulesNet
	{
		SamRulesNet()
		{
			SAMNet::onClientOp(samRulesOp(NetOp::Totals), &samOnTotals);
			SAMNet::onClientOp(samRulesOp(NetOp::XpCurve), &samOnXpCurve);
			SAMNet::onClientOp(samRulesOp(NetOp::MoveSpeed), &samOnMoveSpeed);
			SAMNet::onClientOp(samRulesOp(NetOp::FullExp), &samOnFullExp);
			SAMNet::onClientOp(samRulesOp(NetOp::ClassOverlay), &samOnClassOverlay);
			SAMNet::onHostOp(samRulesOp(NetOp::Resync), &samOnResync);
			SAMNet::addTickHook(&samRulesTick);
			SAMNet::addClearHook(&samRulesNetClear);
		}
	};
	static SamRulesNet s_rulesNet;

	// ---- teardown --------------------------------------------------------------------------------

	void clear()
	{
		for ( int i = 0; i < MAXPLAYERS; ++i ) { s_player[i] = Table(); }
		s_monster.clear();
		s_anyStat = false;
		s_xpByLevel.clear();
		samXpRebuild();
		// Tell the clients too, if a game is running and the channel is still open. Without this
		// an unloaded mod leaves its buffs on every other machine's character sheet and movement.
		// SAMNet closes once the last script is gone, so an unload that has already dropped them
		// sends nothing; the clients' copies then last until their own next run resets them.
		syncAllPlayersToClients();
		// ...and forget what was sent either way. syncPlayerToClients only runs as a SERVER, and
		// a game that has ended is SINGLE again, so the flags used to survive into the next game
		// and send it a take-back packet it never needed -- stock clients included.
		for ( int p = 0; p < MAXPLAYERS; ++p ) { s_clientTotals[p].reset(); s_netSent[p] = false; }
		s_netAny = false;
		s_xpSent = false;
		s_clientXp.clear();
		clearImmunities(nullptr);
		s_gateActive = false;
		samRulesNetClear();
	}
}
