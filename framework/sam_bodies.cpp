/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_bodies.cpp — see sam_bodies.hpp for why the override lives at the draw site.

-------------------------------------------------------------------------------*/

#include "sam_bodies.hpp"

#include "main.hpp"
#include "entity.hpp"
#include "stat.hpp"        // Stat::name (the variant name we resolve from)
#include "monster.hpp"     // actMonster + MONSTER_ATTACK's slot
#include "net.hpp"      // net_packet / net_clients / sendPacketSafe (the SAMB announce)
#include "player.hpp"   // players[]->isLocalPlayer()
#include "sam_monsters.hpp" // bodyForName / anyBodyDeclared
#include "sam_models.hpp"   // modelIndexForId
#include "sam_logger.hpp"  // SAM_WARN: a truncated body name must never be silent

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	// A body resolved to real engine model indices, cached per entity so the id->index
	// lookups happen once rather than every frame.
	struct Anim
	{
		int base = -1;              // idle / fallback. -1 = this entity has no custom body.
		std::vector<int> fly;       // movement cycle (may be empty)
		int attack = -1;            // -1 = fall back to base
		int castFrame = -1;         // -1 = fall back to attack, then base
		int deathFrame = -1;        // -1 = whatever the body would normally draw
		int frameTicks = 10;
		double yawOffsetDeg = 0.0;
		double offFwd = 0.0, offSide = 0.0, offUp = 0.0;
	};

	// uid -> resolved body. EMPTY in vanilla: that is the whole no-op guarantee, and it is
	// why modelForEntity() can bail before touching the entity at all.
	std::unordered_map<uint32_t, Anim> s_bodies;

	// Host: uids already announced to the clients, so the announce is one lookup per tick
	// rather than a packet per tick. Emptied by reannounceAll (a join) and by forget (the
	// entity died or the floor changed), so it can never outlive the uids it holds.
	std::unordered_set<uint32_t> s_announced;

	// Client: uid -> the body name the host sent. This is the client's stand-in for the Stat
	// name it does not have.
	std::unordered_map<uint32_t, std::string> s_remoteNames;

	// Script-set models, by id. Kept apart from the JSON body so clearing one falls back to
	// the other rather than to nothing, and so bodyIdFor answers about the script's choice.
	std::unordered_map<uint32_t, std::string> s_scriptIds;

	// Announce a model id or body name to every client. Shared by hostAnnounce and by the
	// runtime setter so there is one wire format, not two.
	void samSendBody(uint32_t uid, const std::string& payload)
	{
		if ( multiplayer != SERVER ) { return; }
		std::string p = payload;
		if ( p.size() > SAMBodies::SAM_BODY_MAX_NAME )
		{
			// A truncated name resolves to nothing on the client and is then negative-cached
			// for the entity's life, so this must never happen quietly.
			static bool toldOnce = false;
			if ( !toldOnce )
			{
				toldOnce = true;
				SAM_WARN("BODIES", "A body name or model id is longer than "
					+ std::to_string((int)SAMBodies::SAM_BODY_MAX_NAME)
					+ " characters and had to be shortened to fit one packet; clients will not"
					" resolve it. Shorten the monster's name or the model id.");
			}
			p.resize(SAMBodies::SAM_BODY_MAX_NAME);
		}
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( client_disconnected[c] ) { continue; }
			if ( !players[c] || players[c]->isLocalPlayer() ) { continue; }
			strcpy((char*)net_packet->data, "SAMB");
			SDLNet_Write32(uid, &net_packet->data[4]);
			net_packet->data[8] = (Uint8)p.size();
			if ( !p.empty() ) { memcpy(&net_packet->data[9], p.data(), p.size()); }
			net_packet->address.host = net_clients[c - 1].host;
			net_packet->address.port = net_clients[c - 1].port;
			net_packet->len = 9 + (int)p.size();
			sendPacketSafe(net_sock, -1, net_packet, c - 1);
		}
	}
}

void SAMBodies::setBody(uint32_t uid, int modelIndex)
{
	if ( modelIndex < 0 )
	{
		s_bodies.erase(uid);
		return;
	}
	Anim a; a.base = modelIndex;
	s_bodies[uid] = a;
}

void SAMBodies::forget(uint32_t uid)
{
	// Called from ~Entity, which is hot, so every map is emptiness-checked before it is
	// touched and vanilla pays four compares and nothing else. All four are keyed by uid and
	// uids are recycled within a level, so any one of them left holding a dead entity would
	// hand its model to whatever is created next.
	if ( !s_bodies.empty() )      { s_bodies.erase(uid); }
	if ( !s_announced.empty() )   { s_announced.erase(uid); }
	if ( !s_remoteNames.empty() ) { s_remoteNames.erase(uid); }
	if ( !s_scriptIds.empty() )   { s_scriptIds.erase(uid); }
}

void SAMBodies::hostAnnounce(const Entity* entity)
{
	if ( multiplayer != SERVER || !entity ) { return; }
	if ( !SAMMonsters::anyBodyDeclared() ) { return; }
	const uint32_t uid = entity->getUID();
	if ( s_announced.count(uid) ) { return; }

	// A script-set model is the more recent statement of intent, so never overwrite it with
	// the JSON body name -- doing so made every sam_set_model revert on the next tick for
	// every client while the host kept showing the new model.
	if ( !s_scriptIds.empty() && s_scriptIds.count(uid) > 0 ) { s_announced.insert(uid); return; }

	Stat* st = const_cast<Entity*>(entity)->getStats();
	if ( !st || st->name[0] == '\0' ) { return; }   // nothing to say yet; try again next tick
	if ( !SAMMonsters::bodyForName(st->name) ) { s_announced.insert(uid); return; } // no body: never ask again

	std::string name = st->name;
	if ( name.size() > SAM_BODY_MAX_NAME ) { name.resize(SAM_BODY_MAX_NAME); }

	// Host -> client only, exactly like 'SAMI' and 'SAMS'. The host never accepts one, so a
	// client cannot tell anybody else what a monster looks like.
	samSendBody(uid, name);
	// Mark it announced either way: with nobody to tell, re-checking every tick is pure
	// cost, and a later joiner is handled by reannounceAll rather than by re-testing here.
	s_announced.insert(uid);
}

bool SAMBodies::setBodyById(uint32_t uid, const std::string& modelId)
{
	if ( modelId.empty() ) { clearBodyById(uid); return true; }
	if ( SAMModels::modelIndexForId(modelId) < 0 ) { return false; }
	// Refuse a uid with nothing behind it. uids are recycled within a level, so storing a
	// model against a dead one would hand it to whatever entity is created next.
	if ( !uidToEntity((Uint32)uid) ) { return false; }
	s_scriptIds[uid] = modelId;
	s_bodies.erase(uid);          // force a re-resolve with the new id
	samSendBody(uid, modelId);
	return true;
}

void SAMBodies::clearBodyById(uint32_t uid)
{
	s_scriptIds.erase(uid);
	s_bodies.erase(uid);
	// Forget that we announced it, so the next tick re-announces the creature's JSON body.
	// Without this a client was told "drop it" and never told what to draw instead, leaving
	// the host on the JSON body and every client on the base creature permanently.
	s_announced.erase(uid);
	// An empty payload tells a client to drop the SCRIPT layer only; its own announced body
	// name survives, and the re-announce above refreshes it either way.
	samSendBody(uid, std::string());
}

std::string SAMBodies::bodyIdFor(uint32_t uid)
{
	auto it = s_scriptIds.find(uid);
	return ( it == s_scriptIds.end() ) ? std::string() : it->second;
}

void SAMBodies::reannounceAll()
{
	s_announced.clear();
	// Clearing the set re-announces every JSON body on the following ticks, but a script-set
	// model is announced ONCE at the moment the script sets it -- a player who joins later
	// would never hear about it. Re-send those now.
	for ( const auto& kv : s_scriptIds )
	{
		if ( !kv.second.empty() ) { samSendBody(kv.first, kv.second); }
	}
}

void SAMBodies::applyRemote(uint32_t uid, const std::string& bodyName)
{
	if ( bodyName.empty() )
	{
		// The host cleared the SCRIPT model. Drop only that layer: s_remoteNames is this
		// client's stand-in for the Stat name it cannot read, so clearing it too would leave
		// the entity with nothing to fall back to and it would show the base creature.
		s_scriptIds.erase(uid);
		s_bodies.erase(uid);
		return;
	}
	// Announced names belong to the remote layer, never the script layer -- mixing them made
	// bodyIdFor answer differently on the host and on a client for the same entity.
	s_remoteNames[uid] = bodyName;
	s_bodies.erase(uid);
	// The entity may already have been drawn and negative-cached before this arrived, so
	// drop that answer and let it resolve again with the name in hand.
	s_bodies.erase(uid);
}
void SAMBodies::clear()
{
	s_announced.clear();
	s_remoteNames.clear();
	s_scriptIds.clear();
	s_bodies.clear();
}

int SAMBodies::count()
{
	return static_cast<int>(s_bodies.size());
}

int SAMBodies::deathModelForEntity(const Entity* entity)
{
	// The corpse poof. A monster entity is destroyed the same tick it dies, so the death look
	// has to be handed to the gib rather than drawn on the creature. Falls back to whatever
	// the body was already drawing, so a body with no `death` is unchanged.
	if ( !entity ) { return -1; }
	// Resolve FIRST. A monster that died without ever being drawn on this machine has no cache
	// entry yet, and reading the cache before resolving would skip its `death` model entirely.
	const int live = modelForEntity(entity);
	auto it = s_bodies.find(entity->getUID());
	if ( it != s_bodies.end() && it->second.deathFrame >= 0 ) { return it->second.deathFrame; }
	return live;
}

int SAMBodies::announcedCount()
{
	return static_cast<int>(s_announced.size());
}

int SAMBodies::remoteCount()
{
	return static_cast<int>(s_remoteNames.size());
}

// Pick the frame to draw for an already-resolved body.
//
// Barony has no skeletal animation. Vanilla creatures animate by swapping whole models --
// the rat alternates between two every 10 ticks -- and this does the same thing, driven by
// the entity's own tick counter so every creature is on its own beat.
static int samPickFrame(const Anim& a, const Entity* entity)
{
	// MONSTER_ATTACK is skill[8] and is non-zero for the duration of a swing. It also carries
	// the magic wind-up and release poses, so a `cast` frame comes off the same value -- and
	// because skill[8] is already pushed to clients, every player sees a cast without a new
	// packet. Tested before attack: a spell is the more specific statement of the two.
	if ( a.castFrame >= 0
		&& (entity->skill[8] == MONSTER_POSE_MAGIC_WINDUP1
			|| entity->skill[8] == MONSTER_POSE_MAGIC_WINDUP2
			|| entity->skill[8] == MONSTER_POSE_MAGIC_WINDUP3
			|| entity->skill[8] == MONSTER_POSE_MAGIC_CAST1
			|| entity->skill[8] == MONSTER_POSE_MAGIC_CAST2
			|| entity->skill[8] == MONSTER_POSE_MAGIC_CAST3) )
	{
		return a.castFrame;
	}
	if ( a.attack >= 0 && entity->skill[8] != 0 ) { return a.attack; }

	if ( !a.fly.empty() )
	{
		// Only animate while actually moving, exactly like the vanilla rat's walk cycle;
		// a creature standing still holds its idle frame.
		const real_t vx = entity->vel_x, vy = entity->vel_y;
		if ( (vx * vx + vy * vy) > 0.0001 )
		{
			const int step = (int)((entity->ticks / (Uint32)a.frameTicks) % (Uint32)a.fly.size());
			const int idx = a.fly[step];
			if ( idx >= 0 ) { return idx; }
		}
	}
	return a.base;
}

int SAMBodies::modelForEntity(const Entity* entity)
{
	// Ordered cheapest-first. With no mod loaded (and no mod declaring a body) the map is
	// empty and this is one bool + one integer compare per voxel draw -- the renderer never
	// dereferences the entity, never touches a Stat.
	// A script-set model must survive both fast-outs, or sam_set_model would do nothing at
	// all unless some mod ALSO happened to declare a JSON monster body -- which is how it
	// shipped in the first draft and is the whole feature being dead.
	const bool anyDeclared = SAMMonsters::anyBodyDeclared();
	if ( !anyDeclared && s_bodies.empty() && s_scriptIds.empty() && s_remoteNames.empty() ) { return -1; }
	if ( !entity ) { return -1; }

	// A death gib carries its body tag on the entity itself (see spawnGib): gibs all share
	// uid -3, so they cannot be keyed by uid. skill[57] = model index + 1, skill[58] = the
	// sprite the parent had + 1. Only the gib the *Die function stamped with the parent's
	// sprite matches; the blood gibs spawned alongside it keep sprite 5/211 and fall through.
	if ( entity->skill[57] > 0 )
	{
		// The low 16 bits are the model index + 1; the high 16 carry the yaw (see spawnGib).
		const int samGibModel = (entity->skill[57] & 0xFFFF) - 1;
		return ( entity->sprite == entity->skill[58] - 1 ) ? samGibModel : -1;
	}

	auto it = s_bodies.find(entity->getUID());
	if ( it != s_bodies.end() )
	{
		return ( it->second.base < 0 ) ? -1 : samPickFrame(it->second, entity);
	}

	// A limb is a cosmetic bodypart of a monster, and we hide the vanilla ones because the
	// custom body already draws the whole creature.
	//
	// "Has a parent" is NOT enough to identify one. A monster also stamps its own uid onto the
	// parent field of everything it launches -- castSpell sets missileEntity->parent (see
	// magic/castSpell.cpp), and so do arrows, thrown weapons and the devil's boulders. Hiding
	// on `parent` alone would make a custom-bodied dragon breathe INVISIBLE fireballs.
	//
	// The real marker is skill[2]: every limb in the engine is built with
	//     entity->skill[2] = my->getUID();
	// right next to my->bodyparts.push_back(entity). That pairing is 1:1 across all 41
	// monster_*.cpp files, and projectiles never set it (a magic missile uses skill[4]/[5] and
	// leaves skill[2] at 0), so it separates limbs from launched entities exactly.
	//
	// Deliberately NO recursion and no uidToEntity: if the parent has not been resolved yet
	// we simply do not hide this frame. The parent resolves on its own first draw, so the
	// limb is hidden from the next frame onward. One frame of a stray limb at spawn is a far
	// better trade than reaching into the entity list from inside the renderer.
	//
	// Returning 0 is the engine's own "do not draw" path: glDrawVoxel bails on models[0].
	if ( anyDeclared && entity->parent != 0 && entity->behavior != &actMonster
		&& entity->skill[2] == static_cast<Sint32>(entity->parent) )
	{
		auto pit = s_bodies.find(entity->parent);
		if ( pit != s_bodies.end() && pit->second.base >= 0 ) { return 0; }
	}

	// Cache miss. A monster spawned from ANY path (dungeon generation, /summon, a follower
	// restored from a save) reaches the renderer without anyone having tagged it, and the
	// variant-application sites mostly only hold a Stat, not the Entity. So resolve it here,
	// once, from the variant NAME the engine stamped onto the Stat -- then cache the answer,
	// including the negative, so this costs one map hit on every later frame. ~Entity erases
	// the entry, so the map stays bounded by live entities.
	// The actMonster gate is right for a JSON body (they are declared on monsters), but a
	// script may point sam_set_model at anything -- a limb, a spawned prop, a player. Let a
	// uid the script named through regardless of what it is.
	const bool samScripted = ( !s_scriptIds.empty() && s_scriptIds.count(entity->getUID()) > 0 );
	if ( !samScripted && (!anyDeclared || entity->behavior != &actMonster) ) { return -1; }
	// Where the body NAME comes from differs by machine, and that is the whole multiplayer
	// story: the host reads it off the monster's Stat, a client has no Stat for an ordinary
	// monster and uses what the host sent it instead.
	std::string bodyName;
	bool haveName = false;
	// A model a script set explicitly outranks everything: it is the most recent statement
	// of intent about what this entity looks like.
	{
		auto sit = s_scriptIds.find(entity->getUID());
		if ( sit != s_scriptIds.end() && !sit->second.empty() )
		{
			bodyName = sit->second;
			haveName = true;
		}
	}
	if ( !haveName )
	if ( multiplayer == CLIENT )
	{
		auto rit = s_remoteNames.find(entity->getUID());
		if ( rit != s_remoteNames.end() ) { bodyName = rit->second; haveName = true; }
	}
	else
	if ( Stat* st = const_cast<Entity*>(entity)->getStats() )
	{
		bodyName = st->name;
		haveName = true;
	}

	Anim resolved;
	if ( haveName )
	{
		// Resolve, do not guess. The first draft decided by looking for a ':' in the payload,
		// which is wrong in both directions: a mod.json model id is whatever the author typed
		// and need not contain one, and a monster's display name may. Try the model registry
		// first (an exact lookup, so a miss is free) and fall back to a body name.
		const int samDirect = SAMModels::modelIndexForId(bodyName);
		if ( samDirect >= 0 )
		{
			resolved.base = samDirect;
		}
		else if ( const SAMMonsters::BodyDef* def = SAMMonsters::bodyForName(bodyName.c_str()) )
		{
			resolved.base = SAMModels::modelIndexForId(def->model);
			resolved.frameTicks = (def->frameTicks < 1) ? 1 : def->frameTicks;
			resolved.yawOffsetDeg = def->yawOffsetDeg;
			resolved.offFwd = def->offsetForward;
			resolved.offSide = def->offsetSide;
			resolved.offUp = def->offsetUp;
			if ( !def->attack.empty() ) { resolved.attack = SAMModels::modelIndexForId(def->attack); }
			if ( !def->cast.empty() )   { resolved.castFrame = SAMModels::modelIndexForId(def->cast); }
			if ( !def->death.empty() )  { resolved.deathFrame = SAMModels::modelIndexForId(def->death); }
			for ( const std::string& f : def->fly )
			{
				const int fi = SAMModels::modelIndexForId(f);
				if ( fi >= 0 ) { resolved.fly.push_back(fi); }
			}
		}
	}
	// Do not negative-cache an entity that has no name YET. On a client the Stat arrives as a
	// placeholder and the name follows later; caching "no body" on the first frame would pin
	// the base creature model for the entity's whole life.
	// Never negative-cache an entity whose name has not ARRIVED yet, or the base creature is
	// pinned for its whole life. On the host that means a Stat with an empty name; on a
	// client it means the announcement has not landed, which is routine because the entity
	// and the packet race each other.
	if ( resolved.base < 0 )
	{
		if ( multiplayer == CLIENT ) { if ( !haveName ) { return -1; } }
		else
		if ( Stat* st2 = const_cast<Entity*>(entity)->getStats() ) { if ( st2->name[0] == '\0' ) { return -1; } }
	}
	s_bodies[entity->getUID()] = resolved;   // negative-cached too: never re-resolve this entity
	return ( resolved.base < 0 ) ? -1 : samPickFrame(resolved, entity);
}

bool SAMBodies::hiddenByEffect(const Entity* entity)
{
	if ( !entity ) { return false; }
	Stat* st = const_cast<Entity*>(entity)->getStats();
	return st && st->getEffectActive(EFF_INVISIBLE) != 0;
}

double SAMBodies::yawOffsetForEntity(const Entity* entity)
{
	if ( !entity ) { return 0.0; }
	// A death gib carries the yaw correction its parent had, because gibs all share uid -3
	// and cannot be found in s_bodies. Without this a body authored with yaw_offset visibly
	// snaps at the instant it dies -- the one frame the player is looking straight at it.
	if ( entity->skill[57] > 0 )
	{
		const int samYawPacked = (entity->skill[57] >> 16) & 0xFFFF;
		if ( samYawPacked != 0 ) { return (double)(samYawPacked - 3600) / 10.0; }
	}
	if ( s_bodies.empty() ) { return 0.0; }
	auto it = s_bodies.find(entity->getUID());
	return ( it != s_bodies.end() && it->second.base >= 0 ) ? it->second.yawOffsetDeg : 0.0;
}

bool SAMBodies::offsetForEntity(const Entity* entity, double& fwd, double& side, double& up)
{
	if ( s_bodies.empty() || !entity ) { return false; }
	auto it = s_bodies.find(entity->getUID());
	if ( it == s_bodies.end() || it->second.base < 0 ) { return false; }
	const Anim& a = it->second;
	if ( a.offFwd == 0.0 && a.offSide == 0.0 && a.offUp == 0.0 ) { return false; }
	fwd = a.offFwd; side = a.offSide; up = a.offUp;
	return true;
}
