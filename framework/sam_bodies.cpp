/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_bodies.cpp — see sam_bodies.hpp for why the override lives at the draw site.

-------------------------------------------------------------------------------*/

#include "sam_bodies.hpp"

#include "main.hpp"
#include "game.hpp"        // the engine's act* behaviour symbols and the game globals
#include "entity.hpp"
#include "stat.hpp"        // Stat::name (the variant name we resolve from)
#include "monster.hpp"     // actMonster + MONSTER_ATTACK's slot
#include "net.hpp"      // net_packet / net_clients / sendPacketSafe (the SAMB announce)
#include "player.hpp"   // players[]->isLocalPlayer()
#include "sam_monsters.hpp" // bodyForName / anyBodyDeclared
#include "sam_models.hpp"   // modelIndexForId
#include "sam_logger.hpp"  // SAM_WARN: a truncated body name must never be silent
#include "sam_net.hpp"     // the ordered BODY op, for a client that said HELLO

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

	// The model a framework SPAWNER gave an entity (sam_spawn_entity and friends), by id. It is
	// what sam_clear_model falls back to instead of dropping the entity to entity->sprite: sprite
	// crosses the wire as a raw appended model index and indices follow each machine's own mod
	// load order, so a client would draw whatever its own table holds at that number -- or
	// nothing at all, if its table is shorter.
	std::unordered_map<uint32_t, std::string> s_spawnIds;

	// Entities a SCRIPT deliberately hid (sam_set_visible, sam_set_entity_flag INVISIBLE). The
	// draw pass asks hiddenByEffect whether a custom model must honour INVISIBLE, and INVISIBLE
	// means two different things in Barony: a creature's invisibility effect, and "this equipment
	// slot is empty" on a limb, which the engine rewrites every frame. Only a hide a script asked
	// for is a real hide; see hiddenByEffect.
	std::unordered_set<uint32_t> s_scriptHidden;

	// WHICH LAYER a name belongs to on the client. A JSON body name (what hostAnnounce sends)
	// is the client's stand-in for a Stat it cannot read, and only a monster has one; a model id
	// a SCRIPT set -- or a spawner announced -- belongs to the script layer, which draws on any
	// entity at all. The client used to file both as body names, so a model set on anything but
	// a monster (a prop, a spawned entity, a companion, a player) was drawn by the host alone,
	// and sam_get_model answered nil on every client.
	constexpr std::uint8_t SAM_BODY_KIND_NAME = 0;
	constexpr std::uint8_t SAM_BODY_KIND_SCRIPT = 1;

	// The ordered form of 'SAMB' (sam_mp_entities.cpp documents the EntityFirst block):
	// [u32 uid][u8 kind][str8 name]. 'SAMB' is reliable but NOT ordered, so a model set and
	// cleared a moment later could land cleared-then-set and stay wrong for good.
	constexpr std::uint8_t SAM_OP_BODY = SAMNet::Op::EntityFirst + 3;

	// Announce a model id or body name to every client. Shared by hostAnnounce and by the
	// runtime setter so there is one wire format, not two.
	void samSendBody(uint32_t uid, const std::string& payload, std::uint8_t kind)
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
			if ( SAMNet::peerHasSam(c) )
			{
				SAMNet::Writer w;
				w.u32((std::uint32_t)uid);
				w.u8(kind);
				w.str8(p);
				SAMNet::sendToClient(c, SAM_OP_BODY, w.buf);
				continue;
			}
			// A peer SAMNet has already declared stock gets NOTHING, which is the rule sam_net.hpp
			// states for all three legacy packets: a stock 5.0.2 client has no 'SAMB' handler, so
			// it logged one "mystery packet" line per announcement -- one per spawned entity --
			// for the whole game, and now it logs them for at most the HELLO window.
			// (A mod set with JSON bodies and no scripts never declares anybody stock: SAMNet::tick
			// returns before the grace window runs, so peerMayHaveSam stays true and those machines
			// still get the announcement below.)
			if ( !SAMNet::peerMayHaveSam(c) ) { continue; }
			// Anybody else -- a game still inside its HELLO grace window, or a mod set with no
			// scripts (a JSON body needs none) -- gets 'SAMB'.
			// The kind rides AFTER the name: an older S.A.M reader stops at the name length and
			// never sees it, and treats every name as a body name, exactly as before.
			strcpy((char*)net_packet->data, "SAMB");
			SDLNet_Write32(uid, &net_packet->data[4]);
			net_packet->data[8] = (Uint8)p.size();
			if ( !p.empty() ) { memcpy(&net_packet->data[9], p.data(), p.size()); }
			net_packet->data[9 + p.size()] = kind;
			net_packet->address.host = net_clients[c - 1].host;
			net_packet->address.port = net_clients[c - 1].port;
			net_packet->len = 10 + (int)p.size();
			sendPacketSafe(net_sock, -1, net_packet, c - 1);
		}
	}

	// Script-layer announcements waiting for the tick.
	//
	// setBodyById and clearBodyById are reached from inside a SCRIPT CALL -- sam_set_model, and
	// now every sam_spawn_entity / sam_spawn_projectile / companion with a mod model, through
	// SAMMpEntities::adopt -- and a script call can be running inside an engine packet handler
	// (player.on_before_hit fires from Entity::attack, which the host calls from its 'ATAK'
	// handler). net_packet is ONE global buffer and several handlers read their own packet again
	// after calling out, so writing it there hands the handler somebody else's bytes to finish
	// reading. That is the rule sam_net.hpp and sam_mp_entities.cpp both state: nothing goes out
	// from inside a script call. reannounceAll has the same problem from the other end -- it is
	// called from the middle of the 'JOIN' handler, which goes on to build its reply.
	//
	// hostAnnounce (the JSON body name) is deliberately NOT queued: it is called from actMonster,
	// on the tick, and a mod set with bodies but no scripts never runs a tick hook at all.
	struct PendingBody
	{
		uint32_t uid = 0;
		std::string payload;
		std::uint8_t kind = 0;
	};
	std::vector<PendingBody> s_bodyPending;
	constexpr std::size_t BODY_PENDING_MAX = 4096;

	void samQueueBody(uint32_t uid, const std::string& payload, std::uint8_t kind)
	{
		if ( multiplayer != SERVER ) { return; }   // singleplayer and clients send nothing at all
		if ( s_bodyPending.size() >= BODY_PENDING_MAX )
		{
			SAMNet::warnOnce("bodies:pending", "More than 4096 model changes are waiting to be sent in one tick;"
				" the rest were not sent to the other players.");
			return;
		}
		PendingBody p;
		p.uid = uid;
		p.payload = payload;
		p.kind = kind;
		s_bodyPending.push_back(p);
	}

	// Every game tick, on every machine, while scripts are loaded (SAMNet runs the hooks). Only a
	// script fills the list, so this is one empty() test in every other game.
	void samBodiesTick()
	{
		if ( s_bodyPending.empty() ) { return; }
		for ( const PendingBody& p : s_bodyPending )
		{
			// The entity can have died between the script call and this tick, and uids are recycled
			// within a level: announcing a model for a uid nothing holds any more would hand it to
			// whatever is created next. A clear still goes out -- it can only ever remove something.
			if ( !p.payload.empty() && !uidToEntity((Sint32)p.uid) ) { continue; }
			samSendBody(p.uid, p.payload, p.kind);
		}
		s_bodyPending.clear();
	}

	// Client: the ordered form arrived.
	void samOnBodyOp(const std::string& body)
	{
		SAMNet::Reader r(body);
		const std::uint32_t uid = r.u32();
		const std::uint8_t kind = r.u8();
		const std::string name = r.str8();
		if ( !r.ok ) { return; }
		SAMBodies::applyRemote((uint32_t)uid, name, kind == SAM_BODY_KIND_SCRIPT);
	}

	struct SamBodyOpRegistrar
	{
		SamBodyOpRegistrar() { SAMNet::onClientOp(SAM_OP_BODY, &samOnBodyOp); }
	};
	SamBodyOpRegistrar s_samBodyOpRegistrar;
	SAMNet::TickHook s_samBodyTickHook(&samBodiesTick);
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
	if ( !s_bodies.empty() )       { s_bodies.erase(uid); }
	if ( !s_announced.empty() )    { s_announced.erase(uid); }
	if ( !s_remoteNames.empty() )  { s_remoteNames.erase(uid); }
	if ( !s_scriptIds.empty() )    { s_scriptIds.erase(uid); }
	if ( !s_spawnIds.empty() )     { s_spawnIds.erase(uid); }
	if ( !s_scriptHidden.empty() ) { s_scriptHidden.erase(uid); }
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
	samSendBody(uid, name, SAM_BODY_KIND_NAME);
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
	samQueueBody(uid, modelId, SAM_BODY_KIND_SCRIPT);
	return true;
}

void SAMBodies::setSpawnModel(uint32_t uid, const std::string& modelId)
{
	if ( modelId.empty() ) { return; }
	// Same announcement as sam_set_model, but remembered as the entity's OWN model so that
	// clearBodyById has something to fall back to (see there).
	if ( setBodyById(uid, modelId) ) { s_spawnIds[uid] = modelId; }
}

void SAMBodies::clearBodyById(uint32_t uid)
{
	s_scriptIds.erase(uid);
	s_bodies.erase(uid);
	// Forget that we announced it, so the next tick re-announces the creature's JSON body.
	// Without this a client was told "drop it" and never told what to draw instead, leaving
	// the host on the JSON body and every client on the base creature permanently.
	s_announced.erase(uid);
	// An entity a framework spawner made has neither a JSON body nor a vanilla model behind it:
	// dropping the layer sends the renderer to entity->sprite, which is an APPENDED model index
	// and crosses the wire raw, so a client whose mods sorted differently draws a different model
	// -- or nothing, if that index is past its own nummodels. Put the spawn model back instead.
	// It is the same model every machine was already drawing, so this behaves identically in
	// singleplayer, on the host and on a client.
	auto sit = s_spawnIds.find(uid);
	if ( sit != s_spawnIds.end() && !sit->second.empty() )
	{
		s_scriptIds[uid] = sit->second;
		samQueueBody(uid, sit->second, SAM_BODY_KIND_SCRIPT);
		return;
	}
	// An empty payload tells a client to drop the SCRIPT layer only; its own announced body
	// name survives, and the re-announce above refreshes it either way.
	samQueueBody(uid, std::string(), SAM_BODY_KIND_SCRIPT);
}

void SAMBodies::noteScriptVisibility(uint32_t uid, bool visible)
{
	// Called by sam_set_visible / sam_set_entity_flag(INVISIBLE) on every machine that runs the
	// call. hiddenByEffect honours INVISIBLE on a custom model only for what is recorded here.
	if ( visible ) { if ( !s_scriptHidden.empty() ) { s_scriptHidden.erase(uid); } }
	else { s_scriptHidden.insert(uid); }
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
		if ( !kv.second.empty() ) { samQueueBody(kv.first, kv.second, SAM_BODY_KIND_SCRIPT); }
	}
}

void SAMBodies::applyRemote(uint32_t uid, const std::string& bodyName, bool scriptModel)
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
	// Both tables are keyed by a uid the HOST chose, and ~Entity only forgets uids this machine
	// actually created, so a tag for a uid that never exists here is never collected until the
	// game ends. A floor holds a few hundred entities; anything past this is not a real game.
	constexpr std::size_t MAX_REMOTE_TAGS = 8192;
	if ( scriptModel )
	{
		if ( s_scriptIds.size() >= MAX_REMOTE_TAGS && s_scriptIds.find(uid) == s_scriptIds.end() )
		{
			SAMNet::warnOnce("bodies:tags", "The host has named models for more entities than this game can hold;"
				" the newest are being ignored.");
			return;
		}
		// The script layer, exactly as the host keeps it: it draws on any entity (modelForEntity
		// lets a scripted uid through whatever it is), sam_get_model reads it, and the empty
		// payload above is what clears it -- so a cleared model can no longer linger here the
		// way it did when it sat in the body-name layer.
		s_scriptIds[uid] = bodyName;
	}
	else
	{
		if ( s_remoteNames.size() >= MAX_REMOTE_TAGS && s_remoteNames.find(uid) == s_remoteNames.end() )
		{
			SAMNet::warnOnce("bodies:tags", "The host has named models for more entities than this game can hold;"
				" the newest are being ignored.");
			return;
		}
		// Announced body names belong to the remote layer, never the script layer -- mixing
		// them made bodyIdFor answer differently on the host and on a client for one entity.
		s_remoteNames[uid] = bodyName;
	}
	// The entity may already have been drawn and negative-cached before this arrived, so
	// drop that answer and let it resolve again with the name in hand.
	s_bodies.erase(uid);
}

void SAMBodies::resetSession()
{
	// Every table here is keyed by uid, and the next game hands the same numbers out again.
	// ~Entity already forgets each entity it frees; this is the net for anything that outlived
	// its entity (a tag announced for a uid this machine never created).
	clear();
}

void SAMBodies::clear()
{
	s_announced.clear();
	s_remoteNames.clear();
	s_scriptIds.clear();
	s_spawnIds.clear();
	s_scriptHidden.clear();
	s_bodies.clear();
	// Anything still waiting for the tick names uids of a game that has ended.
	s_bodyPending.clear();
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
	// The draw pass asks this about an entity the engine has already marked INVISIBLE, and the
	// answer decides whether its custom model honours that or is drawn anyway.
	//
	// INVISIBLE means two different things in Barony. On a creature it is the invisibility EFFECT
	// and a custom body must vanish with it. On an equipment limb it is STRUCTURAL -- "this hand
	// is empty" -- and the engine rewrites it from the equipment every frame (monster_human.cpp
	// LIMB_HUMANOID_WEAPON and friends, actplayer.cpp for a player's limbs). Answering "hidden"
	// for everything that is not a monster made sam_set_model on an empty weapon or shield limb
	// draw nothing at all, on every machine and in singleplayer too -- and floating a cosmetic in
	// an empty hand is a use this file's own comment blesses.
	//
	// So the only other entity we hide is one a SCRIPT hid on purpose, which it tells us about
	// (noteScriptVisibility, from sam_set_visible -- the only call that may write this flag; the
	// flag table refuses INVISIBLE to sam_set_entity_flag). That keeps the reason
	// this test was added -- a spawned entity or companion with a mod model can still be hidden --
	// without guessing about a flag the engine owns.
	if ( !s_scriptHidden.empty() && s_scriptHidden.count(entity->getUID()) > 0 ) { return true; }
	if ( entity->behavior != &actMonster ) { return false; }
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
