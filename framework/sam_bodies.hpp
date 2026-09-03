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
#include <string>

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

	// True when this entity is invisible because of an EFFECT (a potion or a spell) rather
	// than because its species structurally hides its main entity. The draw pass un-skips a
	// custom body so the six "AI bodypart" monsters stay visible; without this it also
	// un-skipped a monster that had drunk invisibility, leaving it fully visible AND
	// clickable. Answers false for everything in vanilla.
	static bool hiddenByEffect(const Entity* entity);

	// Extra rotation (DEGREES about the vertical axis) for this entity's custom body, or 0.
	// A .vox authored facing a different way than Barony expects would otherwise render
	// sideways, and re-exporting every frame is a worse fix than one number in the JSON.
	static double yawOffsetForEntity(const Entity* entity);

	// Model offset for this entity's custom body, in the model's OWN facing (voxels).
	// Returns false and leaves the outputs untouched when there is nothing to offset.
	// Lets a long creature put its HEAD on the entity origin, which is where the engine
	// spawns attacks from -- otherwise a dragon appears to bite you with its belly.
	static bool offsetForEntity(const Entity* entity, double& fwd, double& side, double& up);

	// ---- multiplayer: carrying a body to the clients ("SAMB") ----------------------
	//
	// A client cannot work out a monster's body for itself: it holds no Stat for an ordinary
	// monster, so the variant name the body is keyed on is simply not there. The host tells
	// it. What crosses the wire is the NAME, never a model index -- a name re-resolves
	// against each machine's own registry, so two players whose mod lists sort differently
	// still see the same creature, which an index could never guarantee.
	// 127 covers Stat::name (char[128]) in full. 9 + 127 is still far inside NET_PACKET_SIZE,
	// and truncating a name would have made the client resolve nothing and negative-cache the
	// creature permanently -- a silent, unrecoverable wrong model.
	static const size_t SAM_BODY_MAX_NAME = 127;

	// Host: announce this monster's body to every connected client, once. Cheap to call
	// every tick for every monster -- it is one hash-set lookup once a monster is known, and
	// it returns immediately when no mod declares a body or the game is not a server.
	static void hostAnnounce(const Entity* entity);

	// Host: forget who has been told what, so everything is announced again. Called when a
	// player joins, because a late joiner has heard none of the earlier announcements.
	static void reannounceAll();

	// Client: record the body name the host sent for this uid.
	static void applyRemote(uint32_t uid, const std::string& bodyName);

	// ---- runtime model control (sam_set_model) -------------------------------------
	// Give this entity a model by ID at runtime and tell every client. Returns false when
	// the id is not registered. Pass an empty id to clear it and let the entity go back to
	// whatever it would otherwise draw.
	static bool setBodyById(uint32_t uid, const std::string& modelId);
	static void clearBodyById(uint32_t uid);
	// The id a script set on this entity, or "" if none. Not the JSON body: only what a
	// script put there, because that is the only thing a script can meaningfully read back.
	static std::string bodyIdFor(uint32_t uid);

	// The model a death gib should carry: the body's `death` state if declared, else whatever
	// it was already drawing. Called by spawnGib, because a monster is gone the tick it dies.
	static int deathModelForEntity(const Entity* entity);

	// Diagnosis for /sam_bodies: how many bodies this machine has resolved, how many the
	// host has announced, and how many names a client has been told. A co-op body problem
	// is otherwise invisible -- you cannot tell "the host never sent it" from "the client
	// could not resolve it" by looking at the screen.
	static int announcedCount();
	static int remoteCount();

	// Drop every registration (mod unload / new game).
	static void clear();

	// How many entities currently carry a custom body.
	static int count();
};
