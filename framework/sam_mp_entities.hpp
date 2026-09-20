/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_mp_entities.hpp
	Desc: multiplayer replication for script-made entities and world edits.

	WHY THIS EXISTS. Barony replicates an entity through one generic path, the ENTU sweep
	(host game.cpp, every 6 ticks), and that path has three blind spots a script walks
	straight into:

	  * An entity with no client BEHAVIOUR never moves on a client. ENTU writes the host's
	    position into new_x/new_y, and only the client loop of an entity that HAS a behaviour
	    moves x toward it (game.cpp, "interpolate to new position"). A client picks the
	    behaviour from the sprite first and then from the skill[2] code (net.cpp
	    clientActions), and the framework's own spawners left skill[2] at values no client
	    binds -- so every sam_spawn_entity, sam_spawn_projectile and companion stood frozen
	    wherever its first ENTU put it, and the companion and decorative portal were never
	    sent at all.
	  * An entity the CLIENT pins with NOUPDATE (every ground item and gold bag, torches and a
	    list of props) answers ENTU with NOUP, and the host then stops sending it. Moving,
	    turning, lifting or scaling one changed the host alone.
	  * ENTU skips a client's OWN player, so a size set on player 2 never reached player 2's
	    machine -- the machine that moves player 2.

	What this module does about each, preferring what a stock 5.0.2 client already
	understands:

	  * S.A.M-owned entities get skill[2] = -7, which EVERY client (stock included) binds to
	    actEmpty, the engine's own "just interpolate" behaviour. A byte appended to their ENTU
	    (after the race-head byte, so older readers never look at it) tells a S.A.M client
	    exactly what they are, so it binds them even when the model is one the sprite switch
	    would claim first (a door, a torch), animates a companion itself, and knows a
	    decorative portal is only decoration.
	  * A ground item or gold bag that is IN MOTION -- shoved, or moved while it was still
	    falling -- goes to clients as the engine's own 'GHOI' packet, which a stock client
	    already handles. One that has come to rest is pinned instead (below): 'GHOI' wakes the
	    item on the receiving machine, and an item a script placed on a ledge or over a pit must
	    stay exactly where it was put, on every machine, as it does in singleplayer.
	  * Everything else a client cannot learn from ENTU -- a pinned entity's position, facing,
	    height, scale or size, a size on a player's own machine, a nudge to a remote player --
	    goes to S.A.M clients on the ordered channel (the Op::EntityFirst block below).
	  * Terrain edits ride the same channel, in order, batched per tick, keyed to the floor
	    they belong to (a client still loading the previous floor holds them until it
	    arrives), and replayed to a client that says HELLO later.

	Pure no-op without a mod: every table here is filled only by a script call, and every
	engine hook tests one empty container before it does anything.

	Game build only.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>

class Entity;

namespace SAMMpEntities
{
	// ---------------------------------------------------------------- S.A.M-owned entities

	// What a framework spawner made. Decides how clients are told to treat it.
	enum class Kind : std::uint8_t
	{
		Scripted,     // sam_spawn_entity: a script is its brain, clients only interpolate it
		Projectile,   // sam_spawn_projectile: also given a velocity so clients dead-reckon it
		Companion,    // sam_spawn_companion: a S.A.M client runs the follow/punch motion itself
		Portal,       // sam_spawn_portal: purely decorative, never interactive
	};

	// Host (and singleplayer): call once, after the spawner has set the entity up and before it
	// returns the uid. Makes it replicate (UPDATENEEDED), gives it a client behaviour code
	// (skill[2] = -7, except a portal, which clients bind by its sprite), gives a projectile
	// its velocity from fskill[2]/[3], clamps a companion's scale to what ENTU can carry, and
	// announces a mod model by NAME (SAMBodies::setBodyById) so a client whose model table is
	// ordered differently still draws the right thing. `owner` is the companion's player.
	// Nothing here uses skill[2] afterwards; a spawner must keep its own data elsewhere.
	void adopt(Entity* e, Kind kind, int owner = -1);

	// True for an entity adopt() registered (host). Its position, facing, height, scale and
	// size reach every client through ENTU, so nothing below sends anything for it.
	bool owns(const Entity* e);

	// net.cpp sendEntityUDP (host): the kind byte to append to this entity's ENTU, 0 for none.
	// `param` receives the kind's one-byte parameter (a companion's owner).
	std::uint8_t wireKind(const Entity* e, std::uint8_t& param);

	// net.cpp clientActions (client), called first: reads the byte wireKind wrote from the ENTU
	// in net_packet and binds the entity. True when it did, and clientActions must then stop.
	bool clientBind(Entity* e);

	// ---------------------------------------------------------------- script writes (host)

	// What a script changed. Bits for transformed().
	namespace Changed
	{
		constexpr unsigned FACING = 1u << 1;   // yaw
		constexpr unsigned HEIGHT = 1u << 2;   // z
		constexpr unsigned SCALE  = 1u << 3;
		constexpr unsigned SIZE   = 1u << 4;   // sizex/sizey
	}

	// A script is about to move `e` relatively (sam_move_entity). For a REMOTE player the host
	// copy is reset to the freshest position the owner reported (new_x/new_y), because
	// actPlayer does exactly that on the next tick and a move computed from the stale x would
	// be thrown away. Records where the move starts.
	void beginMove(Entity* e, double& startX, double& startY);
	// ...and has moved it. A remote player's owner is told: a S.A.M machine gets the DELTA
	// (applied there with its own collision, so it never snaps back by a round trip), a stock
	// machine gets the engine's own PMOV correction. Anything else goes through moved().
	void endMove(Entity* e, double startX, double startY);

	// A script placed a non-player entity somewhere new (sam_set_position, sam_move_entity).
	// A ground item or gold bag keeps whatever rest state it had: one still falling is sent as
	// 'GHOI' and lands where it lands, one at rest is pinned and stays asleep exactly where the
	// script put it -- which is what singleplayer has always done, and 'GHOI' would undo.
	void moved(Entity* e);

	// A script teleported a player (sam_set_position -> Entity::teleport, which already sent
	// TELE). A remote player's host copy is pointed at the new spot at once, instead of being
	// dragged back to new_x until the owner's echo arrives.
	void teleported(Entity* e);

	// A script gave a ground item or gold bag a velocity (sam_apply_force). A resting gold bag
	// is woken (it ignores velocity otherwise, on every machine); clients get GHOI, and once
	// the host copy comes to rest they get one more with its final spot.
	void pushed(Entity* e);

	// A script changed some of Changed:: on `e` (sam_set_scale, sam_set_entity_size,
	// sam_set_elevation, sam_set_entity_facing / sam_look_at).
	void transformed(Entity* e, unsigned what);

	// A script threw a gib (sam_gib). spawnGib makes a local-only entity (uid -3, NOUPDATE) and
	// sends nothing; every engine caller follows it with the vanilla 'SPGB', which a stock client
	// handles. This sends that same packet from the tick, with the gib as it is now.
	void gibForClients(const Entity* gib);

	// ---------------------------------------------------------------- refusals (both runtimes)
	// Each returns false AND says why when the write would not last. `who` is the function name.

	// actPlayer rewrites a player's scale every frame on every machine, and a slime animates
	// its own scale every frame on every machine.
	bool scaleSticks(const Entity* e, const char* who);
	// A size on a remote player must reach the owner's machine (it computes that player's
	// movement), and only a S.A.M machine can be told. While that player's game has not said
	// HELLO yet nobody knows which it is, so the call is allowed and the old size remembered:
	// if the peer is then declared stock, the tick puts it back and says so.
	bool sizeReachesOwner(const Entity* e, const char* who);
	// A creature's INVISIBLE is rewritten from its invisibility effect every frame on the host,
	// a ground item is made visible again every frame, and a limb's INVISIBLE is rewritten from
	// what its creature is wearing (there it means "this slot is empty").
	bool visibilitySticks(const Entity* e, const char* who);
	// Flags the engine rewrites every frame: BLOCKSIGHT on a creature, INVISIBLE_DITHER on a
	// player, BURNABLE on a ground item, INVISIBLE and its dither on a limb. `flag` is the
	// entity.hpp index.
	bool flagSticks(const Entity* e, int flag, const char* who);
	// sam_spawn_particle "poof": the host draws the raw scale while clients receive
	// Uint16(scale * 100). Clamped to what that can carry, with a warning.
	double wirePoofScale(double scale, const char* who);

	// ---------------------------------------------------------------- engine hooks

	// Host, sam_remove_entity's drain, right before the entity is freed: a removed follower
	// leaves its leader's FOLLOWERS list, a remote leader is told (LDEL, before ENTD, as the
	// death path does), and a local leader's follower menu forgets it.
	void beforeRemove(Entity* e);

	// Host, net.cpp 'NOUP': false when a client's NOUP must NOT stop the host sending this
	// entity. A stock client pins a S.A.M-owned entity whose model it binds to a pinned prop;
	// S.A.M clients never do and still need its updates.
	bool keepsUpdatingDespiteNoup(const Entity* e);

	// Host, actmonster.cpp's generic invisibility block. Note that the effect is active, and
	// ask whether it just ended -- the host clears INVISIBLE then and sends nothing, so every
	// client kept the monster hidden for good. Only tracks anything while scripts are loaded.
	void noteInvisibleByEffect(const Entity* e);
	bool invisibleEffectEnded(const Entity* e);

	// Host, game.cpp, when a floor is about to load: every per-floor table is dropped.
	void onFloorChange();

	// sam_world.cpp setTile (host): tell the clients. S.A.M clients get the ordered, floor-keyed
	// op; a machine that has not said HELLO yet gets the vanilla 'WALD' for a dug wall (which a
	// stock client understands) and the legacy 'SAMT' for anything else. Once SAMNet has decided
	// a machine is stock it gets 'WALD' only -- 'SAMT' is a packet it has no handler for, and one
	// per edited tile would fill its log with "mystery packet" lines. Those legacy packets are
	// reliable and cost one send per tile per machine, so they go out at a fixed budget per tick.
	void tileChanged(int x, int y, int layer, int tileId);

	// ---------------------------------------------------------------- the companion's motion

	// The follow / hover / punch step, shared by the host behaviour and the client animator so
	// the two can never drift. `owner` is the owning player's entity.
	void companionStep(Entity* my, const Entity* owner);
	// Host: a punch started (skill[18] was just set). S.A.M clients animate it from skill[18].
	void companionPunched(Entity* e);
	constexpr int COMPANION_PUNCH_TICKS = 8;   // one forward-thrust window
	constexpr double COMPANION_RISE = 6.0;     // float height above the owner (z is negative-up)
}
