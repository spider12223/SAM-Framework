/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_bodies.hpp — custom BODY models for living creatures.

	A custom monster in S.A.M is a VARIANT of a vanilla Monster type, so it inherits
	that type's body: initMonster(<hardcoded model index>) inside the vanilla per-type
	init. There is no monster model override in vanilla, which is why a modded monster
	always looked like the creature underneath it.

	THE KEY DECISION: the override lives at the DRAW SITE (glDrawVoxel), not in
	entity->sprite.

	entity->sprite is read by ~70 GAMEPLAY predicates — getMonsterTypeFromSprite,
	targeting, tooltips, the client's behavior reconstruction in clientActions — and it
	is on the wire. Writing a custom index there would mean fighting the engine in
	seventy places and putting a load-order-dependent number into network packets.
	glDrawVoxel is the ONLY place the engine turns an entity into a model, so overriding
	there leaves the creature a completely normal monster to every piece of game logic
	and changes only how it LOOKS.

	Lookup is by entity uid into a side map that is EMPTY in vanilla, so the renderer
	pays one .empty() check per voxel draw when no mod is loaded.

	Game build only (the editor links opengl.cpp too, so the call site is #ifndef EDITOR).

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>

class Entity;

class SAMBodies
{
public:
	// Give this entity a custom body model (an engine model index, normally from
	// SAMModels::modelIndexForId). Pass modelIndex < 0 to clear it.
	static void setBody(uint32_t uid, int modelIndex);


	// Forget one entity (called from ~Entity so gib tags cannot accumulate).
	static void forget(uint32_t uid);

	// The custom body model index for this entity, or -1 if it has none. Returns -1
	// immediately when nothing is registered, which is the vanilla no-op path.
	static int modelForEntity(const Entity* entity);

	// Drop every registration (mod unload / new game).
	static void clear();

	// How many entities currently carry a custom body.
	static int count();
};
