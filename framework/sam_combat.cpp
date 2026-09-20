/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_combat.cpp
	Desc: batch 4 — species damage resistance + the on_damage_multiplier hook.
	      See sam_combat.hpp for why neither of these can be a plain binding.

-------------------------------------------------------------------------------*/

#include "sam_combat.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>      // free: an equipped item that has no inventory node (see samPurgeDeadGear)
#include <map>
#include <string>
#include <vector>

#include "sam_logger.hpp"
#include "sam_event.hpp"
#include "sam_net.hpp"   // the ordered channel: the resist snapshot, a remote revive
#include "sam_rules.hpp" // SAMRules::NetOp, this package's op table

// The same order as sam_lua_runtime.cpp, the largest unit that includes all of these.
#include "main.hpp"     // umbrella -- establishes the engine context monster.hpp needs
#include "game.hpp"     // TICKS_PER_SECOND, clientnum
#include "items.hpp"    // Item, itemCategory, SPELL_CAT
#include "player.hpp"   // players[], the ghost, paperDoll
#include "net.hpp"      // keepInventoryGlobal, serverUpdatePlayerStats
#include "stat.hpp"     // Stat, the ten equipment slots
#include "entity.hpp"   // Entity, actPlayer
#include "monster.hpp"  // NUMMONSTERS, NOTHING, DamageTableType, limbs

namespace SAMCombat
{
	// ---- species damage resistance ------------------------------------------------------

	// Keyed by species*8 + damageType. A map rather than a NUMMONSTERS*7 array because the
	// common case is a handful of entries and the read has to be cheap when it is EMPTY,
	// which anySpeciesResist() answers without touching this at all.
	static std::map<int, double> s_speciesResist;

	// The number of DamageTableType members (monster.hpp:554) and the width of every
	// damagetables row. Named rather than repeated so the two stay one fact.
	static const int SAM_DAMAGE_TYPE_COUNT = 7;

	static bool samSpeciesInRange(int species)
	{
		// NOTHING is the 0 sentinel and is not a creature anyone fights.
		return species > 0 && species < NUMMONSTERS;
	}

	bool setSpeciesResist(int species, int damageType, double multiplier)
	{
		if ( !samSpeciesInRange(species) )
		{
			SAM_WARN("COMBAT", "sam_set_species_damage_resist: " + std::to_string(species)
				+ " is not a creature this game has.");
			return false;
		}
		if ( damageType < 0 || damageType >= SAM_DAMAGE_TYPE_COUNT )
		{
			SAM_WARN("COMBAT", "sam_set_species_damage_resist: damage type " + std::to_string(damageType)
				+ " is not one of the seven the engine has (sword, mace, axe, polearm, ranged,"
				" magic, unarmed).");
			return false;
		}
		// Not finite, and negative, are two different mistakes with the same result: the
		// damage the engine computes stops being a number. Refuse both loudly rather than
		// clamping, because a NaN here would not show up until something died wrong.
		if ( !std::isfinite(multiplier) || multiplier < 0.0 )
		{
			SAM_WARN("COMBAT", "sam_set_species_damage_resist: the multiplier must be a finite"
				" number and cannot be negative. 0 means immune, 1 is normal, 2 is double damage taken.");
			return false;
		}
		// 20 is arbitrary but not decoration: this number multiplies a damage figure that is
		// then narrowed to int at several call sites, and a mod that typed 1e9 would wrap it.
		if ( multiplier > 20.0 )
		{
			SAM_WARN("COMBAT", "sam_set_species_damage_resist: clamped to 20. Past that the"
				" damage figure this multiplies overflows the int the engine stores it in.");
			multiplier = 20.0;
		}
		s_speciesResist[species * 8 + damageType] = multiplier;
		return true;
	}

	int clearSpeciesResist(int species, int damageType)
	{
		if ( !samSpeciesInRange(species) ) { return 0; }
		if ( damageType < 0 || damageType >= SAM_DAMAGE_TYPE_COUNT )
		{
			// Every type for this species.
			int removed = 0;
			for ( int t = 0; t < SAM_DAMAGE_TYPE_COUNT; ++t )
			{
				removed += (int)s_speciesResist.erase(species * 8 + t);
			}
			return removed;
		}
		return (int)s_speciesResist.erase(species * 8 + damageType);
	}

	int clearAllSpeciesResist()
	{
		const int n = (int)s_speciesResist.size();
		s_speciesResist.clear();
		return n;
	}

	bool anySpeciesResist()
	{
		return !s_speciesResist.empty();
	}

	bool speciesResist(int species, int damageType, double* out)
	{
		if ( s_speciesResist.empty() || !out ) { return false; }
		// The engine indexes damagetables with myStats.type, which for a CUSTOM RACE player is
		// the host monster body it was built on -- always a real species. It is still bounded
		// here, because a species out of range would be a lookup we invented, not one the
		// engine made.
		if ( !samSpeciesInRange(species) ) { return false; }
		if ( damageType < 0 || damageType >= SAM_DAMAGE_TYPE_COUNT ) { return false; }
		auto it = s_speciesResist.find(species * 8 + damageType);
		if ( it == s_speciesResist.end() ) { return false; }
		*out = it->second;
		return true;
	}

	// ---- the on_damage_multiplier hook --------------------------------------------------

	static bool                s_multActive = false;
	static std::vector<double> s_multContributions;

	bool multiplierHookActive() { return s_multActive; }

	void addMultiplier(double fraction)
	{
		if ( !s_multActive ) { return; }
		if ( !std::isfinite(fraction) ) { return; }
		// One contribution is bounded so that the SUM cannot be driven anywhere silly by a
		// single typo; the combined result is clamped again below.
		if ( fraction >  20.0 ) { fraction =  20.0; }
		if ( fraction < -20.0 ) { fraction = -20.0; }
		s_multContributions.push_back(fraction);
	}

	bool fireDamageMultiplierHook(long long targetUid, long long attackerUid,
		double& mult, int damageType, long long projectileUid, int spellID)
	{
		// Nothing loaded: one bool test and out. This runs inside damage resolution, several
		// times per swing in the worst case, so it has to cost nothing in a vanilla game.
		if ( !SamEvent::anyScripts() ) { return false; }

		// NESTING IS REFUSED. A handler is free to deal damage, which re-enters the engine's
		// damage path and arrives back here. Without this the inner dispatch would take over
		// the contribution list and the outer hit would apply somebody else's numbers.
		if ( s_multActive ) { return false; }
		if ( !std::isfinite(mult) ) { return false; }

		const double before = mult;

		s_multActive = true;
		s_multContributions.clear();

		SamEvent e("on_damage_multiplier");
		e.i("target_uid",   targetUid);
		e.i("attacker_uid", attackerUid);
		e.i("damage_type",  (long long)damageType);
		// The event bus carries long long and std::string only, so a fractional multiplier
		// crosses as thousandths: 1000 is normal damage, 1500 is +50%, 0 is immune. A handler
		// may assign this field directly, which overrides every contribution.
		e.i("multiplier_x1000", (long long)std::lround(before * 1000.0));
		e.i("projectile_uid", projectileUid);
		e.i("spell_id",       (long long)spellID);

		const bool allowed = e.fire();

		s_multActive = false;

		// Positives SUM, negatives MULTIPLY -- the engine's own rule for its allBonuses pool
		// (entity.cpp:32010-32022). Two mods each adding +0.2 give +40%, not +44%; two each
		// halving give a quarter, not nothing. Copying the rule means a script author who has
		// read Barony's numbers is not surprised by ours.
		double summed = 0.0;
		double multiplied = 1.0;
		for ( double v : s_multContributions )
		{
			if ( v > 0.0 )       { summed += v; }
			else if ( v < 0.0 )  { multiplied *= (1.0 + v); }
		}
		s_multContributions.clear();
		if ( multiplied < 0.0 ) { multiplied = 0.0; }   // a contribution of exactly -1 zeroes it
		double after = before * multiplied + summed;

		// An assignment to the field itself is ABSOLUTE and wins over the contributions.
		// Compared against what we PUT IN, not against `after`: the write-back store always
		// holds this key because we seeded it, so a fallback comparison could never tell
		// "untouched" from "set to the same value".
		const long long seeded  = (long long)std::lround(before * 1000.0);
		const long long written = e.get("multiplier_x1000", seeded);
		if ( written != seeded ) { after = (double)written / 1000.0; }

		// A handler returning false means this hit does nothing, the same meaning `return
		// false` already has in on_before_damage.
		if ( !allowed ) { after = 0.0; }

		if ( !std::isfinite(after) || after < 0.0 ) { after = 0.0; }
		if ( after > 20.0 ) { after = 20.0; }

		if ( after == before ) { return false; }
		mult = after;
		return true;
	}

	// ---- multiplayer: the species-resist table -------------------------------------------

	static std::uint8_t samCombatOp(int offset) { return (std::uint8_t)(SAMNet::Op::RulesFirst + offset); }

	// Body: [u16 count] then per override (u16 species, u8 damage type, f64 multiplier). Species
	// and damage types are the engine's own enums, fixed numbers on every machine.
	void sendResistSnapshot(int peer)
	{
		if ( multiplayer != SERVER ) { return; }
		SAMNet::Writer w;
		w.u16((std::uint16_t)s_speciesResist.size());
		for ( const auto& kv : s_speciesResist )
		{
			w.u16((std::uint16_t)(kv.first / 8));
			w.u8((std::uint8_t)(kv.first % 8));
			w.f64(kv.second);
		}
		SAMNet::sendToClient(peer, samCombatOp(SAMRules::NetOp::SpeciesResist), w.buf);
	}

	static void samOnResistSnapshot(const std::string& body)
	{
		SAMNet::Reader r(body);
		std::map<int, double> table;
		const int n = r.u16();
		for ( int i = 0; i < n && r.ok; ++i )
		{
			const int species = r.u16();
			const int type = r.u8();
			const double mult = r.f64();
			// The same bounds setSpeciesResist enforces, so the wire cannot put a value here that
			// a script on this machine could not have.
			if ( !samSpeciesInRange(species) || type < 0 || type >= SAM_DAMAGE_TYPE_COUNT ) { continue; }
			if ( !std::isfinite(mult) || mult < 0.0 || mult > 20.0 ) { continue; }
			table[species * 8 + type] = mult;
		}
		// Half a table is worse than this machine's own until the next catch-up.
		if ( !r.ok ) { return; }
		s_speciesResist.swap(table);
	}

	// ---- reviving a player ----------------------------------------------------------------

	// What the engine does to a dead player's gear before it stands them up on the next floor
	// (maps.cpp, the co-op revive): free all ten equipment slots, and remove every backpack
	// item except spells, which death never takes. Mirrored field for field. An equipped item
	// usually also sits in the backpack list and goes with its node; one that does not (the
	// host's copy of a remote player's gear is made with no list) is freed on its own.
	static void samPurgeDeadGear(Stat* s)
	{
		if ( !s ) { return; }
		Item** slots[] = {
			&s->helmet, &s->breastplate, &s->gloves, &s->shoes, &s->shield,
			&s->weapon, &s->cloak, &s->amulet, &s->ring, &s->mask,
		};
		for ( Item** slot : slots )
		{
			if ( *slot )
			{
				if ( (*slot)->node ) { list_RemoveNode((*slot)->node); }
				else { free(*slot); }
			}
			*slot = nullptr;
		}
		node_t* next = nullptr;
		for ( node_t* node = s->inventory.first; node != nullptr; node = next )
		{
			next = node->next;
			Item* item = (Item*)node->element;
			if ( item && itemCategory(item) == SPELL_CAT ) { continue; }
			list_RemoveNode(node);
		}
	}

	// Bind a player's slot to their new body once it has arrived here. On a machine that is not
	// the host, a revived player's body comes in by the host's entity updates -- and the engine
	// only re-points players[n]->entity at a player that still HAS one, which a dead player does
	// not. So without this every other machine lost track of a revived player for the rest of
	// the floor: the same gap vanilla's own 'REZZ' has.
	struct PendingBind
	{
		Uint32 uid = 0;
		Uint32 until = 0;
	};
	static PendingBind s_bind[MAXPLAYERS];

	static void samQueueBind(int player, Uint32 uid)
	{
		if ( player < 0 || player >= MAXPLAYERS || uid == 0 ) { return; }
		s_bind[player].uid = uid;
		s_bind[player].until = ticks + 10 * TICKS_PER_SECOND;
	}

	static void samClientBindTick()
	{
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			PendingBind& b = s_bind[p];
			if ( b.uid == 0 ) { continue; }
			if ( !players[p] || players[p]->entity )
			{
				b = PendingBind();   // bound already, by actPlayer or by the engine
				continue;
			}
			Entity* e = uidToEntity(b.uid);
			if ( e && e->behavior == &actPlayer && e->skill[2] == p )
			{
				players[p]->entity = e;
				b = PendingBind();
				continue;
			}
			if ( (Sint32)(ticks - b.until) > 0 ) { b = PendingBind(); }   // it never came; do not guess
		}
	}

	bool revivePlayer(int player, int wantX, int wantY)
	{
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("COMBAT", "sam_revive_player refused: host only.");
			return false;
		}
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] ) { return false; }
		if ( players[player]->entity )
		{
			SAM_WARN("COMBAT", "sam_revive_player: player " + std::to_string(player) + " is not dead.");
			return false;
		}
		const bool remote = SAMNet::isRemotePlayer(player);
		if ( multiplayer == SERVER && player != clientnum && !remote )
		{
			SAM_WARN("COMBAT", "sam_revive_player: player " + std::to_string(player) + " is not in this game.");
			return false;
		}
		if ( remote && !SAMNet::peerHasSam(player) )
		{
			SAM_WARN("COMBAT", "sam_revive_player: player " + std::to_string(player) + "'s game is not"
				" running S.A.M scripts. Only their own machine can take down their ghost and drop the"
				" gear it still holds from the death, so they were not revived.");
			return false;
		}

		// Where. The ghost's tile unless the caller named one, and the map's bounds decide
		// whether either is usable -- a body outside the map is not a revive.
		int tx = wantX, ty = wantY;
		if ( tx < 0 || ty < 0 )
		{
			if ( players[player]->ghost.my )
			{
				tx = (int)(players[player]->ghost.my->x / 16);
				ty = (int)(players[player]->ghost.my->y / 16);
			}
			else
			{
				SAM_WARN("COMBAT", "sam_revive_player: player " + std::to_string(player) + " has no"
					" ghost to revive at, so name a tile: sam_revive_player(n, x, y).");
				return false;
			}
		}
		if ( tx < 0 || ty < 0 || tx >= (int)map.width || ty >= (int)map.height )
		{
			SAM_ERROR("COMBAT", "sam_revive_player: tile " + std::to_string(tx) + "," + std::to_string(ty)
				+ " is outside this map.");
			return false;
		}

		// What the death copied out, and so what has to go now. actplayer.cpp's death drops a
		// player's items to the floor in singleplayer and copies them to the loot bag in co-op
		// without keep-inventory; a remote player's own machine did the same through 'DIEI',
		// and the host bagged its copies of their equipment. Each machine purges its own copy.
		const bool purge = remote ? !keepInventoryGlobal
			: ( (multiplayer == SINGLE && !splitscreen) || !keepInventoryGlobal );
		if ( purge )
		{
			samPurgeDeadGear(stats[player]);
			if ( players[player]->isLocalPlayer() ) { players[player]->paperDoll.updateSlots(); }
		}

		// The body, field for field from the engine's own 'REZZ' handler. NOT
		// Player::Ghost_t::respawn(), which returns nullptr immediately for any non-local
		// player and would silently do nothing for three of the four slots.
		if ( players[player]->ghost.my )
		{
			list_RemoveNode(players[player]->ghost.my->mynode);
			players[player]->ghost.my = nullptr;
		}
		players[player]->ghost.reset();

		Entity* entity = newEntity(113, 1, map.entities, nullptr);
		entity->x = (tx * 16) + 8;
		entity->y = (ty * 16) + 8;
		entity->new_x = entity->x;
		entity->new_y = entity->y;
		entity->z = -1;
		entity->flags[INVISIBLE] = false;
		entity->flags[GENIUS] = true;
		entity->behavior = &actPlayer;
		entity->skill[2] = player;
		entity->yaw = 0.0;
		entity->sizex = 4;
		entity->sizey = 4;
		entity->focalx = limbs[HUMAN][0][0];
		entity->focaly = limbs[HUMAN][0][1];
		entity->focalz = limbs[HUMAN][0][2];
		entity->flags[UPDATENEEDED] = true;
		entity->flags[BLOCKSIGHT] = true;
		entity->addToCreatureList(map.creatures);
		players[player]->entity = entity;
		stats[player]->HP = stats[player]->MAXHP / 2;

		if ( multiplayer == SERVER )
		{
			// Every machine's copy of the HP now, rather than on the next three-second 'STAT'.
			serverUpdatePlayerStats();
			const Uint32 uid = entity->getUID();
			if ( remote )
			{
				// Body: [u8 purge][u32 HP as a signed value][u32 body uid].
				SAMNet::Writer w;
				w.u8(purge ? 1 : 0);
				w.u32((std::uint32_t)(std::int32_t)stats[player]->HP);
				w.u32((std::uint32_t)uid);
				SAMNet::sendToClient(player, samCombatOp(SAMRules::NetOp::Revive), w.buf);
			}
			// Body: [u8 player][u32 body uid].
			SAMNet::Writer b;
			b.u8((std::uint8_t)player);
			b.u32((std::uint32_t)uid);
			for ( int c = 1; c < MAXPLAYERS; ++c )
			{
				if ( c == player || !SAMNet::peerHasSam(c) ) { continue; }
				SAMNet::sendToClient(c, samCombatOp(SAMRules::NetOp::BindBody), b.buf);
			}
		}
		SAM_INFO("COMBAT", "sam_revive_player: player " + std::to_string(player) + " revived at tile "
			+ std::to_string(tx) + "," + std::to_string(ty) + (purge ? " (the gear the death paid out is gone)." : "."));
		return true;
	}

	// The revived player's own machine: what Player::Ghost_t::respawn does locally before it
	// sends 'REZZ', and the purge the next floor would have run.
	static void samOnRevive(const std::string& body)
	{
		SAMNet::Reader r(body);
		const bool purge = r.u8() != 0;
		const Sint32 hp = (Sint32)r.u32();
		const Uint32 uid = r.u32();
		if ( !r.ok || multiplayer != CLIENT ) { return; }
		if ( clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum] || !stats[clientnum] ) { return; }
		Player& me = *players[clientnum];
		if ( me.ghost.my )
		{
			me.ghost.setActive(false);
			list_RemoveNode(me.ghost.my->mynode);
			me.ghost.my = nullptr;
		}
		me.ghost.reset();
		if ( purge )
		{
			samPurgeDeadGear(stats[clientnum]);
			me.paperDoll.updateSlots();
		}
		if ( hp > 0 ) { stats[clientnum]->HP = hp; }
		// actPlayer binds our own body the first time it runs on it; this is the same bind in
		// case that has not happened yet, and does nothing once it has.
		samQueueBind(clientnum, uid);
		SAM_INFO("COMBAT", "The host revived this machine's player.");
	}

	static void samOnBindBody(const std::string& body)
	{
		SAMNet::Reader r(body);
		const int player = r.u8();
		const Uint32 uid = r.u32();
		if ( !r.ok || multiplayer != CLIENT ) { return; }
		samQueueBind(player, uid);
	}

	// ---- sam_set_defending on a remote player ---------------------------------------------

	struct DefendOverride
	{
		bool active = false;
		bool prev = false;      // what the player's own machine last reported
		bool forced = false;    // what the script wrote
		Uint32 setTick = 0;
	};
	static DefendOverride s_defend[MAXPLAYERS];

	void noteDefendingOverride(int player, bool before, bool forced)
	{
		if ( multiplayer != SERVER || player < 0 || player >= MAXPLAYERS ) { return; }
		// A player on this machine has actHudShield rewriting the flag every frame already.
		if ( !SAMNet::isRemotePlayer(player) ) { return; }
		DefendOverride& d = s_defend[player];
		if ( !d.active ) { d.prev = before; }   // two writes in a row still restore the reported value
		d.active = true;
		d.forced = forced;
		d.setTick = ticks;
	}

	// Runs at the end of every host frame, after the scripts. `ticks` advances between
	// gameLogic() and the scripts, so a write made by a script this frame carries this frame's
	// tick, and is left alone until the NEXT frame's gameLogic() -- where hits are resolved --
	// has run with it. Restoring at the end of the same frame would have undone every write
	// from on_tick or a timer before any hit could see it.
	static void samHostDefendTick()
	{
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			DefendOverride& d = s_defend[p];
			if ( !d.active || ticks == d.setTick ) { continue; }
			// If the player's own 'SHLD' has changed the flag since, that is newer than both the
			// script's value and ours, and it stays.
			if ( stats[p] && stats[p]->defending == d.forced ) { stats[p]->defending = d.prev; }
			d = DefendOverride();
		}
	}

	static void samCombatTick()
	{
		if ( multiplayer == SERVER ) { samHostDefendTick(); }
		else if ( multiplayer == CLIENT ) { samClientBindTick(); }
	}

	static void samCombatNetClear()
	{
		for ( int p = 0; p < MAXPLAYERS; ++p ) { s_bind[p] = PendingBind(); s_defend[p] = DefendOverride(); }
	}

	struct SamCombatNet
	{
		SamCombatNet()
		{
			SAMNet::onClientOp(samCombatOp(SAMRules::NetOp::SpeciesResist), &samOnResistSnapshot);
			SAMNet::onClientOp(samCombatOp(SAMRules::NetOp::Revive), &samOnRevive);
			SAMNet::onClientOp(samCombatOp(SAMRules::NetOp::BindBody), &samOnBindBody);
			SAMNet::addTickHook(&samCombatTick);
			SAMNet::addClearHook(&samCombatNetClear);
		}
	};
	static SamCombatNet s_combatNet;

	void clear()
	{
		s_speciesResist.clear();
		s_multActive = false;
		s_multContributions.clear();
		samCombatNetClear();
	}
}
