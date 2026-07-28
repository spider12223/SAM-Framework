/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_bodies.cpp — see sam_bodies.hpp for why the override lives at the draw site.

-------------------------------------------------------------------------------*/

#include "sam_bodies.hpp"

#include "main.hpp"
#include "entity.hpp"
#include "stat.hpp"        // Stat::name (the variant name we resolve from)
#include "monster.hpp"     // actMonster + MONSTER_ATTACK's slot
#include "sam_monsters.hpp" // bodyForName / anyBodyDeclared
#include "sam_models.hpp"   // modelIndexForId

#include <unordered_map>
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
		int frameTicks = 10;
		double yawOffsetDeg = 0.0;
		double offFwd = 0.0, offSide = 0.0, offUp = 0.0;
	};

	// uid -> resolved body. EMPTY in vanilla: that is the whole no-op guarantee, and it is
	// why modelForEntity() can bail before touching the entity at all.
	std::unordered_map<uint32_t, Anim> s_bodies;
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
	// Called from ~Entity, which is hot. Empty check first so vanilla pays one compare.
	if ( s_bodies.empty() ) { return; }
	s_bodies.erase(uid);
}

void SAMBodies::clear()
{
	s_bodies.clear();
}

int SAMBodies::count()
{
	return static_cast<int>(s_bodies.size());
}

// Pick the frame to draw for an already-resolved body.
//
// Barony has no skeletal animation. Vanilla creatures animate by swapping whole models --
// the rat alternates between two every 10 ticks -- and this does the same thing, driven by
// the entity's own tick counter so every creature is on its own beat.
static int samPickFrame(const Anim& a, const Entity* entity)
{
	// MONSTER_ATTACK is skill[8] and is non-zero for the duration of a swing.
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
	const bool anyDeclared = SAMMonsters::anyBodyDeclared();
	if ( !anyDeclared && s_bodies.empty() ) { return -1; }
	if ( !entity ) { return -1; }

	// A death gib carries its body tag on the entity itself (see spawnGib): gibs all share
	// uid -3, so they cannot be keyed by uid. skill[57] = model index + 1, skill[58] = the
	// sprite the parent had + 1. Only the gib the *Die function stamped with the parent's
	// sprite matches; the blood gibs spawned alongside it keep sprite 5/211 and fall through.
	if ( entity->skill[57] > 0 )
	{
		return ( entity->sprite == entity->skill[58] - 1 ) ? (entity->skill[57] - 1) : -1;
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
	if ( !anyDeclared || entity->behavior != &actMonster ) { return -1; }
	Anim resolved;
	if ( Stat* st = const_cast<Entity*>(entity)->getStats() )
	{
		if ( const SAMMonsters::BodyDef* def = SAMMonsters::bodyForName(st->name) )
		{
			resolved.base = SAMModels::modelIndexForId(def->model);
			resolved.frameTicks = (def->frameTicks < 1) ? 1 : def->frameTicks;
			resolved.yawOffsetDeg = def->yawOffsetDeg;
			resolved.offFwd = def->offsetForward;
			resolved.offSide = def->offsetSide;
			resolved.offUp = def->offsetUp;
			if ( !def->attack.empty() ) { resolved.attack = SAMModels::modelIndexForId(def->attack); }
			for ( const std::string& f : def->fly )
			{
				const int fi = SAMModels::modelIndexForId(f);
				if ( fi >= 0 ) { resolved.fly.push_back(fi); }
			}
		}
	}
	s_bodies[entity->getUID()] = resolved;   // negative-cached too: never re-resolve this entity
	return ( resolved.base < 0 ) ? -1 : samPickFrame(resolved, entity);
}

double SAMBodies::yawOffsetForEntity(const Entity* entity)
{
	if ( s_bodies.empty() || !entity ) { return 0.0; }
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
