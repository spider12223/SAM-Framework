/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_bodies.cpp — see sam_bodies.hpp for why the override lives at the draw site.

-------------------------------------------------------------------------------*/

#include "sam_bodies.hpp"

#include "main.hpp"
#include "entity.hpp"
#include "stat.hpp"        // Stat::name (the variant name we resolve from)
#include "monster.hpp"     // actMonster (the gate for the lazy resolve)
#include "sam_monsters.hpp" // bodyModelForName / anyBodyDeclared
#include "sam_models.hpp"   // modelIndexForId

#include <unordered_map>

namespace
{
	// uid -> engine model index (-1 = "resolved, has none", cached so we never re-resolve).
	// EMPTY in vanilla: that is the whole no-op guarantee, and it is why modelForEntity()
	// can bail before touching the entity at all.
	std::unordered_map<uint32_t, int> s_bodies;
}

void SAMBodies::setBody(uint32_t uid, int modelIndex)
{
	if ( modelIndex < 0 )
	{
		s_bodies.erase(uid);
		return;
	}
	s_bodies[uid] = modelIndex;
}


void SAMBodies::forget(uint32_t uid)
{
	// Called from ~Entity, which is hot. Empty check first so vanilla pays one compare.
	if ( s_bodies.empty() ) { return; }
	s_bodies.erase(uid);
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
	if ( it != s_bodies.end() ) { return it->second; }

	// Cache miss. A monster spawned from ANY path (dungeon generation, /summon, a follower
	// restored from a save) reaches the renderer without anyone having tagged it, and the
	// variant-application sites mostly only hold a Stat, not the Entity. So resolve it here,
	// once, from the variant NAME the engine stamped onto the Stat -- then cache the answer,
	// including the negative, so this costs one map hit on every later frame. ~Entity erases
	// the entry, so the map stays bounded by live entities.
	if ( !anyDeclared || entity->behavior != &actMonster ) { return -1; }
	int resolved = -1;
	if ( Stat* st = const_cast<Entity*>(entity)->getStats() )
	{
		if ( const char* modelId = SAMMonsters::bodyModelForName(st->name) )
		{
			resolved = SAMModels::modelIndexForId(modelId);
		}
	}
	s_bodies[entity->getUID()] = resolved;   // negative-cached too: never re-resolve this entity
	return resolved;
}

void SAMBodies::clear()
{
	s_bodies.clear();
}

int SAMBodies::count()
{
	return static_cast<int>(s_bodies.size());
}
