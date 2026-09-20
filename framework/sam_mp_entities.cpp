/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_mp_entities.cpp
	Desc: see sam_mp_entities.hpp.

	OPS -- the Op::EntityFirst block (56..71). All host -> client; the host accepts none, so
	no client can move, resize or re-tile anything on anybody else's machine.

	  EntityFirst+0  TILES  [u32 mapseed][u8 floor][u8 secret][u16 n], then n x
	                        ([u16 x][u16 y][u8 layer][u16 tile]). Terrain the host edited, in
	                        the order it edited it. Applied only on the floor it was made on: a
	                        client still loading that floor holds it, and one that has already
	                        left drops it.
	  EntityFirst+1  PIN    [u32 uid][u8 mask], then in bit order: POS f64 x, f64 y | FACING
	                        f64 yaw | HEIGHT f64 z | SCALE f64 x3 | SIZE u32 sizex, u32 sizey.
	                        Absolute state for an entity ENTU cannot update on this client: one
	                        the client pins with NOUPDATE, or this client's own player (size).
	  EntityFirst+2  NUDGE  [u32 uid][f64 dx][f64 dy], world units. Move THIS client's own
	                        player by that much, through its own collision (sam_move_entity).
	  EntityFirst+3  BODY   [u32 uid][u8 kind][str8 name]. Registered and sent by sam_bodies.cpp:
	                        the ordered form of 'SAMB' for a client that said HELLO. kind 1 = a
	                        model id a script set (or a spawner announced), 0 = a JSON body name;
	                        an empty name drops the script layer. Ordered, so a model set and then
	                        cleared a moment later can never arrive cleared-then-set.
	  EntityFirst+4..15     free.

	THE ENTU TAIL. net.cpp already appends one byte to ENTU at ENTITY_PACKET_LENGTH (the
	race-head marker). A S.A.M-owned entity gets two more after it: [kind][param]. A stock
	5.0.2 client reads fixed offsets below ENTITY_PACKET_LENGTH and never sees them; a S.A.M
	client from before this change tests only the race-head byte, which stays 0 here.

-------------------------------------------------------------------------------*/

#include "sam_mp_entities.hpp"
#include "sam_net.hpp"
#include "sam_event.hpp"      // SamEvent::anyScripts: the invisibility hook tracks nothing without a mod
#include "sam_logger.hpp"
#include "sam_world.hpp"      // applyRemoteTile: a client writes terrain through the same checks as the host
#include "sam_bodies.hpp"     // setBodyById: a spawned mod model crosses by NAME; resetSession
#include "sam_models.hpp"     // idForModelIndex

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "main.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "entity.hpp"
#include "items.hpp"
#include "monster.hpp"
#include "interface/interface.hpp"   // FollowerMenu: a removed follower must leave the local leader's menu
#include "net.hpp"
#include "collision.hpp"             // clipMove, for a nudge applied on the owner's machine
#include "player.hpp"

namespace
{
	const char* MOD = "SAM";

	constexpr std::uint8_t OP_TILES = SAMNet::Op::EntityFirst + 0;
	constexpr std::uint8_t OP_PIN   = SAMNet::Op::EntityFirst + 1;
	constexpr std::uint8_t OP_NUDGE = SAMNet::Op::EntityFirst + 2;

	// What a PIN carries. The other four bits are the public SAMMpEntities::Changed ones, so a
	// transformed() call hands its bits straight through.
	constexpr unsigned PIN_POS = 1u << 0;
	constexpr unsigned PIN_TRANSFORM = SAMMpEntities::Changed::FACING | SAMMpEntities::Changed::HEIGHT
		| SAMMpEntities::Changed::SCALE | SAMMpEntities::Changed::SIZE;

	// The kind byte on the ENTU tail. 0 is "not ours", which is also what an older host sends.
	constexpr std::uint8_t WIRE_LERP = 1;        // bind actEmpty: interpolate, nothing else
	constexpr std::uint8_t WIRE_COMPANION = 2;   // run the companion's motion here; param = owner
	constexpr std::uint8_t WIRE_PORTAL = 3;      // a decorative portal: actPortal's inert path
	constexpr int WIRE_AT = ENTITY_PACKET_LENGTH + 1;   // after the race-head byte

	// Tiles per TILES op. 7 bytes each keeps one op near 28 KB, far under the 256 KB a receiver
	// will reassemble, and a whole-map rewrite still goes out in a handful of ops.
	constexpr std::size_t TILES_PER_OP = 4096;
	// The per-floor log replayed to a client that says HELLO late. Bounded so a mod rewriting
	// every tile of a large map every floor cannot grow it without limit.
	constexpr std::size_t TILE_LOG_MAX = 65536;

	// ---------------------------------------------------------------- host state

	// Entities a framework spawner made, by uid, with the pointer they were made at. Both are
	// compared on every lookup: the engine hands a uid back out only to replace one a
	// throwaway particle gave up, never one a live entity held, so uid AND pointer matching is
	// exact -- and a stale row can only ever fail to match, never mislabel another entity.
	struct Owned
	{
		Entity* e = nullptr;
		std::uint8_t kind = 0;
		std::uint8_t param = 0;
	};
	std::unordered_map<Uint32, Owned> s_owned;

	// PINs to send at the end of this tick, coalesced: a script moving a prop every frame costs
	// one op per frame, and ten writes in one frame still cost one.
	std::unordered_map<Uint32, unsigned> s_pinPending;
	// Everything ever pinned on this floor, re-sent with current values to a late HELLO.
	std::unordered_map<Uint32, unsigned> s_pinned;

	// Ground items and gold a script moved or shoved, until the host copy comes to rest and
	// clients have been sent where it stopped. Their own physics runs on every machine, but
	// the client has no TileEntityList and NOUPDATE blocks every later correction, so without
	// the last word the resting spots drift apart.
	//
	// Every raw packet this module sends goes out from the tick, never from inside the script
	// call that asked for it: net_packet is ONE global buffer, and a script can be running
	// inside an engine packet handler that reads its own packet again after the event returns.
	struct Pushed
	{
		Entity* e = nullptr;
		Uint32 since = 0;
		bool announce = false;   // the first GHOI (and a gold bag's wake-up) has not gone out yet
	};
	std::unordered_map<Uint32, Pushed> s_pushed;

	// Stock machines owed the engine's own position correction after a script nudged their
	// player (sam_move_entity). Sent from the tick with the host's position as it then stands.
	bool s_pmovPending[MAXPLAYERS] = { false };

	// A size a script set on a remote player's entity while that player's game had not said HELLO
	// yet. The correction is held by SAMNet until HELLO -- and THROWN AWAY if the grace window
	// expires and the peer turns out to be stock -- so the host would be left computing that
	// player's collisions with a box their own machine, the one that moves them, never heard
	// about: they stick in doorways and rubber-band, which is the very failure the refusal in
	// sizeReachesOwner exists to prevent. Refusing every call inside the window instead would
	// refuse S.A.M players too (HELLO costs a round trip), so the call is accepted and PUT BACK
	// the moment the peer is declared stock.
	struct HeldSize
	{
		Uint32 uid = 0;
		Sint32 sizex = 0;
		Sint32 sizey = 0;
		bool active = false;
	};
	HeldSize s_heldSize[MAXPLAYERS];

	// Companions whose punch started this tick: ENTS 18 from the tick.
	std::unordered_set<Uint32> s_punchPending;

	// Gibs a script threw this tick, copied at the call: a gib holds the shared uid -3, so it
	// cannot be found again by the time the tick sends the engine's 'SPGB' for it.
	struct PendingGib
	{
		Sint16 x = 0, y = 0, z = 0, sprite = 0;
		Uint8 flags = 0;   // SPGB byte 12: bit 0 a sprite gib, bit 1 a poof
	};
	std::vector<PendingGib> s_gibsPending;
	constexpr std::size_t GIBS_PENDING_MAX = 256;   // a script throwing gibs every frame, bounded

	// Monsters invisible because of their EFFECT, so the moment it ends can be told to clients.
	std::unordered_set<Uint32> s_invisible;

	// ---------------------------------------------------------------- terrain

	struct FloorKey
	{
		Uint32 seed = 0;
		std::uint8_t level = 0;
		std::uint8_t secret = 0;
		bool operator==(const FloorKey& o) const { return seed == o.seed && level == o.level && secret == o.secret; }
		bool operator!=(const FloorKey& o) const { return !(*this == o); }
	};

	// The floor this machine is on. mapseed changes with every floor (a client takes it from
	// LVLC before it loads), so it tells two floors apart where the floor number alone cannot.
	FloorKey currentFloor()
	{
		FloorKey k;
		k.seed = mapseed;
		k.level = (std::uint8_t)currentlevel;   // LVLC itself carries the floor in one byte
		k.secret = secretlevel ? 1 : 0;
		return k;
	}

	struct TileEdit
	{
		std::uint16_t x = 0;
		std::uint16_t y = 0;
		std::uint8_t layer = 0;
		std::uint16_t tile = 0;
	};
	std::vector<TileEdit> s_tilesPending;   // host: edits this tick, in order (the ordered op)
	FloorKey s_tilesPendingKey;
	// Host: the same edits, for machines that have not said HELLO yet. Drained at a fixed budget
	// per tick, because each one costs a RELIABLE packet per such machine and a mod re-tiles a
	// room with a loop -- a few thousand edits in one frame would otherwise be a few thousand
	// reliable sends in that frame, a host hitch and a retransmit storm. The ordered channel
	// throttles itself for exactly this reason (sam_net.cpp's FRAMES_PER_TICK).
	std::deque<TileEdit> s_legacyPending;
	constexpr std::size_t LEGACY_PER_TICK = 64;      // per tick, matching FRAMES_PER_TICK
	constexpr std::size_t LEGACY_PENDING_MAX = 16384;
	std::map<std::uint64_t, std::uint16_t> s_tileLog;   // host: this floor's edits, last one per tile
	FloorKey s_tileLogKey;

	std::uint64_t tileKey(int x, int y, int layer)
	{
		return ((std::uint64_t)(std::uint16_t)x << 24) | ((std::uint64_t)(std::uint16_t)y << 8)
			| (std::uint64_t)(std::uint8_t)layer;
	}

	// ---------------------------------------------------------------- client state

	// TILES bodies not yet applied, in arrival order. One that is not for this floor holds
	// everything behind it, so edits can never apply out of order.
	std::deque<std::string> s_tilesWaiting;
	Uint32 s_tilesWaitingSince = 0;
	// Floors this client has been on and left, so a late TILES op for one is dropped at once
	// instead of holding up the current floor's edits until it times out.
	std::vector<FloorKey> s_leftFloors;
	FloorKey s_lastFloor;
	bool s_haveLastFloor = false;

	// PINs for an entity this client does not know yet (the ENTU that creates it can arrive
	// after the op). Kept in order; a newer PIN for a uid with an older one waiting queues
	// behind it rather than applying first and then being overwritten.
	struct WaitingPin
	{
		Uint32 uid = 0;
		std::string body;
		Uint32 until = 0;
	};
	std::deque<WaitingPin> s_pinsWaiting;
	constexpr std::size_t PINS_WAITING_MAX = 256;

	// ---------------------------------------------------------------- helpers

	bool validUid(Uint32 uid)
	{
		// 0 and the negatives (-2 client-local, -3 gibs and particles) are shared engine
		// markers that many entities hold at once; written unsigned they are above 2^31.
		return uid != 0 && uid <= 0x7FFFFFFFu;
	}

	// A client that has not said HELLO YET: it cannot be sent an ordered op, so anything it is to
	// see has to go in the vanilla form as well. True during the HELLO grace window for a S.A.M
	// player too, which is why this decides only what to SEND, never what to warn about.
	bool anyPeerWithoutSam()
	{
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( SAMNet::isRemotePlayer(c) && !SAMNet::peerHasSam(c) ) { return true; }
		}
		return false;
	}

	// A client SAMNet has DECIDED is stock: the grace window passed and it never said hello. Only
	// this deserves a "a player without S.A.M keeps seeing it where it was" warning -- warnOnce
	// keys are never cleared, so one such line in the first seconds of an all-S.A.M game, before
	// anybody has said hello, would be wrong for the rest of the session and never corrected.
	bool anyStockPeer()
	{
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( SAMNet::isRemotePlayer(c) && !SAMNet::peerMayHaveSam(c) ) { return true; }
		}
		return false;
	}

	// A cosmetic bodypart -- a weapon, a shield, a helmet, an arm. Two different shapes: every
	// MONSTER limb is built with `entity->skill[2] = my->getUID();` right beside
	// my->bodyparts.push_back (the pairing is 1:1 across all 41 monster_*.cpp files, and launched
	// entities like arrows and fireballs never set skill[2], so it separates them exactly), while
	// a PLAYER's limb carries skill[2] = the player NUMBER and its own behaviour instead
	// (actplayer.cpp) -- so the monster test alone answers false for every player's limb.
	// The engine rebuilds a limb from what its creature is wearing every frame, INVISIBLE
	// included: there it means "this slot is empty", not "hidden".
	bool isLimb(const Entity* e)
	{
		if ( !e ) { return false; }
		if ( e->behavior == &actPlayerLimb ) { return true; }
		return e->parent != 0 && e->behavior != &actMonster && e->skill[2] == (Sint32)e->parent;
	}

	// Kinds a client pins with NOUPDATE, so ENTU never updates them there (actitem.cpp and
	// actgold.cpp re-assert it every tick; clientActions sets it for these behaviours).
	bool clientPins(const Entity* e)
	{
		return e->flags[NOUPDATE] || e->behavior == &actItem || e->behavior == &actGoldBag
			|| e->behavior == &actGate || e->behavior == &actTorch || e->behavior == &actCampfire
			|| e->behavior == &actColumn || e->behavior == &actPistonCam || e->behavior == &actCeilingTile;
	}

	// Every remote client that runs S.A.M scripts. sendToClient already refuses a stock client
	// and holds the op for one that has not said HELLO yet.
	void sendToSamPeers(std::uint8_t op, const std::string& body)
	{
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( SAMNet::isRemotePlayer(c) ) { SAMNet::sendToClient(c, op, body); }
		}
	}

	// The engine's own item push packet (actitem.cpp's ghost push sends exactly this), handled
	// by a stock client: absolute position and velocity, and it wakes the item's physics.
	void sendGhoi(const Entity* e, bool stopped)
	{
		if ( multiplayer != SERVER || !e || !net_packet ) { return; }
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() ) { continue; }
			strcpy((char*)net_packet->data, "GHOI");
			SDLNet_Write32((Uint32)e->getUID(), &net_packet->data[4]);
			SDLNet_Write16((Sint16)(e->x * 32), &net_packet->data[8]);
			SDLNet_Write16((Sint16)(e->y * 32), &net_packet->data[10]);
			SDLNet_Write16((Sint16)(e->z * 32), &net_packet->data[12]);
			SDLNet_Write16((Sint16)(stopped ? 0.0 : e->vel_x * 32), &net_packet->data[14]);
			SDLNet_Write16((Sint16)(stopped ? 0.0 : e->vel_y * 32), &net_packet->data[16]);
			SDLNet_Write16((Sint16)(stopped ? 0.0 : e->vel_z * 32), &net_packet->data[18]);
			net_packet->address.host = net_clients[c - 1].host;
			net_packet->address.port = net_clients[c - 1].port;
			net_packet->len = 20;
			sendPacketSafe(net_sock, -1, net_packet, c - 1);
		}
	}

	// The tick sends the first GHOI with the item's position and velocity as they then stand
	// (a placement carries whatever velocity it had, a shove the new one), then watches it rest.
	void trackPushed(Entity* e)
	{
		const Uint32 uid = e->getUID();
		if ( !validUid(uid) ) { return; }
		Pushed p;
		p.e = e;
		p.since = ticks;
		p.announce = true;
		s_pushed[uid] = p;
	}

	std::string pinBody(const Entity* e, unsigned mask)
	{
		SAMNet::Writer w;
		w.u32((std::uint32_t)e->getUID());
		w.u8((std::uint8_t)(mask & (PIN_POS | PIN_TRANSFORM)));
		if ( mask & PIN_POS ) { w.f64(e->x); w.f64(e->y); }
		if ( mask & SAMMpEntities::Changed::FACING ) { w.f64(e->yaw); }
		if ( mask & SAMMpEntities::Changed::HEIGHT ) { w.f64(e->z); }
		if ( mask & SAMMpEntities::Changed::SCALE ) { w.f64(e->scalex); w.f64(e->scaley); w.f64(e->scalez); }
		if ( mask & SAMMpEntities::Changed::SIZE ) { w.u32((std::uint32_t)e->sizex); w.u32((std::uint32_t)e->sizey); }
		return w.buf;
	}

	void queuePin(const Entity* e, unsigned mask)
	{
		if ( !e || !mask ) { return; }
		const Uint32 uid = e->getUID();
		if ( !validUid(uid) ) { return; }
		s_pinPending[uid] |= mask;
		s_pinned[uid] |= mask;
	}

	std::string tilesBody(const FloorKey& k, const TileEdit* edits, std::size_t n)
	{
		SAMNet::Writer w;
		w.u32(k.seed);
		w.u8(k.level);
		w.u8(k.secret);
		w.u16((std::uint16_t)n);
		for ( std::size_t i = 0; i < n; ++i )
		{
			w.u16(edits[i].x);
			w.u16(edits[i].y);
			w.u8(edits[i].layer);
			w.u16(edits[i].tile);
		}
		return w.buf;
	}

	// toPlayer < 0: every remote S.A.M client.
	void sendTiles(int toPlayer, const FloorKey& k, const std::vector<TileEdit>& edits)
	{
		for ( std::size_t at = 0; at < edits.size(); at += TILES_PER_OP )
		{
			const std::size_t n = std::min<std::size_t>(TILES_PER_OP, edits.size() - at);
			const std::string body = tilesBody(k, &edits[at], n);
			if ( toPlayer < 0 ) { sendToSamPeers(OP_TILES, body); }
			else { SAMNet::sendToClient(toPlayer, OP_TILES, body); }
		}
	}

	// A machine that has not said HELLO. A stock client understands 'WALD' (the engine's own
	// pickaxe dig sends it) and only that, so a dug wall is the one edit it can see; it zeroes
	// the obstacle layer and nothing else. Everything else keeps the legacy 'SAMT', which a
	// S.A.M client still in its HELLO grace window understands -- the ordered op that machine
	// also gets once it says HELLO carries the same edits in order.
	// WALD is sent INSTEAD of SAMT, never as well: two reliable packets are not ordered on a
	// direct connection, and a stray late WALD would reopen a wall a later SAMT rebuilt.
	// 'SAMT' is NOT sent to a machine SAMNet has already declared stock: it has no handler for it
	// and would print "Got a mystery packet: SAMT" once per edited tile, which is thousands of
	// lines for one re-tiled room and breaks the promise sam_net.hpp makes about a stock client.
	// A dug wall still goes, because that is the one edit such a machine can actually see.
	void sendLegacyTile(int x, int y, int layer, int tileId)
	{
		if ( !net_packet ) { return; }
		const bool dugWall = ( layer == OBSTACLELAYER && tileId == 0 );
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() ) { continue; }
			if ( SAMNet::peerHasSam(c) ) { continue; }
			if ( !dugWall && !SAMNet::peerMayHaveSam(c) ) { continue; }
			if ( dugWall )
			{
				strcpy((char*)net_packet->data, "WALD");
				SDLNet_Write16((Uint16)x, &net_packet->data[4]);
				SDLNet_Write16((Uint16)y, &net_packet->data[6]);
				net_packet->len = 8;
			}
			else
			{
				strcpy((char*)net_packet->data, "SAMT");
				SDLNet_Write16((Uint16)x, &net_packet->data[4]);
				SDLNet_Write16((Uint16)y, &net_packet->data[6]);
				net_packet->data[8] = (Uint8)layer;
				SDLNet_Write16((Uint16)tileId, &net_packet->data[9]);
				net_packet->len = 11;
			}
			net_packet->address.host = net_clients[c - 1].host;
			net_packet->address.port = net_clients[c - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, c - 1);
		}
	}

	// ---------------------------------------------------------------- client: applying ops

	void noteFloor()
	{
		const FloorKey k = currentFloor();
		if ( s_haveLastFloor && k == s_lastFloor ) { return; }
		if ( s_haveLastFloor )
		{
			s_leftFloors.push_back(s_lastFloor);
			if ( s_leftFloors.size() > 8 ) { s_leftFloors.erase(s_leftFloors.begin()); }
		}
		s_lastFloor = k;
		s_haveLastFloor = true;
	}

	bool leftFloor(const FloorKey& k)
	{
		if ( k == currentFloor() ) { return false; }
		for ( const FloorKey& f : s_leftFloors ) { if ( f == k ) { return true; } }
		return false;
	}

	// 1 = applied (or unreadable: dropped), 0 = for a floor this machine has not reached yet,
	// -1 = for a floor this machine has already left.
	int applyTiles(const std::string& body)
	{
		SAMNet::Reader r(body);
		FloorKey k;
		k.seed = r.u32();
		k.level = r.u8();
		k.secret = r.u8();
		const int n = r.u16();
		if ( !r.ok ) { return 1; }
		if ( k != currentFloor() ) { return leftFloor(k) ? -1 : 0; }
		for ( int i = 0; i < n; ++i )
		{
			const int x = r.u16();
			const int y = r.u16();
			const int layer = r.u8();
			const int tile = r.u16();
			if ( !r.ok ) { break; }
			SAMWorld::applyRemoteTile(x, y, layer, tile);   // bounds re-checked there; marks path maps dirty
		}
		return 1;
	}

	void drainTiles()
	{
		while ( !s_tilesWaiting.empty() )
		{
			const int got = applyTiles(s_tilesWaiting.front());
			if ( got == 0 ) { break; }   // not on that floor yet: everything behind it waits with it
			s_tilesWaiting.pop_front();
			s_tilesWaitingSince = ticks;
		}
	}

	void onTilesOp(const std::string& body)
	{
		noteFloor();
		if ( s_tilesWaiting.size() >= 64 )
		{
			s_tilesWaiting.pop_front();
			SAMNet::warnOnce("mpent:tiles-backlog", "Terrain edits from the host piled up while this machine was not on"
				" the floor they belong to; the oldest were dropped.");
		}
		if ( s_tilesWaiting.empty() ) { s_tilesWaitingSince = ticks; }
		s_tilesWaiting.push_back(body);
		drainTiles();
	}

	// true = applied (or unreadable: dropped), false = no such entity here yet.
	bool applyPin(const std::string& body)
	{
		SAMNet::Reader r(body);
		const std::uint32_t uid = r.u32();
		const std::uint8_t mask = r.u8();
		if ( !r.ok ) { return true; }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e ) { return false; }
		if ( mask & PIN_POS )
		{
			const double x = r.f64();
			const double y = r.f64();
			// Never a player: a player's position belongs to the machine that owns it, and the
			// host moves one with TELE or a nudge instead.
			if ( r.ok && std::isfinite(x) && std::isfinite(y) && e->behavior != &actPlayer )
			{
				e->x = x; e->y = y;
				// The interpolation target too, or the client loop would pull it back toward the
				// last ENTU it had.
				e->new_x = x; e->new_y = y;
			}
		}
		if ( mask & SAMMpEntities::Changed::FACING )
		{
			const double v = r.f64();
			if ( r.ok && std::isfinite(v) ) { e->yaw = v; e->new_yaw = v; }
		}
		if ( mask & SAMMpEntities::Changed::HEIGHT )
		{
			const double v = r.f64();
			if ( r.ok && std::isfinite(v) ) { e->z = v; e->new_z = v; }
		}
		if ( mask & SAMMpEntities::Changed::SCALE )
		{
			const double sx = r.f64();
			const double sy = r.f64();
			const double sz = r.f64();
			// Clamped for the same reason as the size below: these came off the wire. The
			// ceiling is the 1.99 the companion spawner already documents as the most the
			// network carries, and nothing may be negative or the model turns inside out.
			if ( r.ok && std::isfinite(sx) && std::isfinite(sy) && std::isfinite(sz) )
			{
				e->scalex = std::max(0.0, std::min(1.99, sx));
				e->scaley = std::max(0.0, std::min(1.99, sy));
				e->scalez = std::max(0.0, std::min(1.99, sz));
			}
		}
		if ( mask & SAMMpEntities::Changed::SIZE )
		{
			Sint32 sx = (Sint32)r.u32();
			Sint32 sy = (Sint32)r.u32();
			// The same 0..127 the host-side setter enforces (sam_set_entity_size). It is not
			// decoration there -- a size crosses the vanilla wire as one SIGNED BYTE -- and a
			// value that arrived here instead of being set here has had no such check, so it
			// gets one. A collision box read from a wire value is not something to take on trust.
			if ( r.ok )
			{
				if ( sx < 0 ) { sx = 0; } if ( sx > 127 ) { sx = 127; }
				if ( sy < 0 ) { sy = 0; } if ( sy > 127 ) { sy = 127; }
				e->sizex = sx; e->sizey = sy;
			}
		}
		return true;
	}

	void onPinOp(const std::string& body)
	{
		SAMNet::Reader r(body);
		const std::uint32_t uid = r.u32();
		if ( !r.ok ) { return; }
		bool behind = false;
		for ( const WaitingPin& w : s_pinsWaiting ) { if ( w.uid == uid ) { behind = true; break; } }
		if ( !behind && applyPin(body) ) { return; }
		if ( s_pinsWaiting.size() >= PINS_WAITING_MAX ) { s_pinsWaiting.pop_front(); }
		WaitingPin w;
		w.uid = uid;
		w.body = body;
		w.until = ticks + TICKS_PER_SECOND * 5;
		s_pinsWaiting.push_back(std::move(w));
	}

	void onNudgeOp(const std::string& body)
	{
		SAMNet::Reader r(body);
		const std::uint32_t uid = r.u32();
		real_t dx = (real_t)r.f64();
		real_t dy = (real_t)r.f64();
		if ( !r.ok || !std::isfinite(dx) || !std::isfinite(dy) ) { return; }
		if ( clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum] || !players[clientnum]->entity ) { return; }
		Entity* me = players[clientnum]->entity;
		// A nudge meant for a previous life or a previous floor names an entity that is gone.
		if ( (std::uint32_t)me->getUID() != uid ) { return; }
		const real_t limit = 16.0 * 1024.0;
		dx = std::max(-limit, std::min(limit, dx));
		dy = std::max(-limit, std::min(limit, dy));
		// Sub-stepped exactly as sam_move_entity does on the host: clipMove tests only the
		// destination, so one long step would hop a wall.
		const int steps = std::max(1, (int)std::ceil(std::sqrt(dx * dx + dy * dy) / 7.0));
		dx /= (real_t)steps;
		dy /= (real_t)steps;
		const hit_t savedHit = hit;   // clipMove writes the engine's global
		for ( int i = 0; i < steps; ++i )
		{
			if ( clipMove(&me->x, &me->y, dx, dy, me) <= 0.0 ) { break; }
		}
		hit = savedHit;
	}

	// ---------------------------------------------------------------- the client's companion

	// A S.A.M client runs the companion's motion itself from its owner's entity here, which
	// is the smooth one on the owner's own machine, and animates the punch from skill[18]
	// (the host sends ENTS 18 when a punch starts). An 8-tick lunge is shorter than one ENTU
	// interval plus the 3-tick interpolation window, so from ENTU alone it was a twitch at best.
	void actSamCompanionClient(Entity* my)
	{
		if ( !my ) { return; }
		my->flags[PASSABLE] = true;
		my->flags[BRIGHT] = true;
		const int owner = my->skill[17];
		if ( owner < 0 || owner >= MAXPLAYERS || !players[owner] || !players[owner]->entity ) { return; }
		SAMMpEntities::companionStep(my, players[owner]->entity);
		// ENTU keeps writing the host's copy into new_* and the client loop drags an entity toward
		// it for three ticks after every packet. This behaviour IS the motion here, so aim that
		// pull at where the companion already is.
		my->new_x = my->x;
		my->new_y = my->y;
		my->new_z = my->z;
		my->new_yaw = my->yaw;
		my->vel_x = 0.0;
		my->vel_y = 0.0;
		my->vel_z = 0.0;
	}

	// ---------------------------------------------------------------- hooks

	void hostTick()
	{
		// Transforms scripts made this tick: one op per entity, with the values as they stand.
		if ( !s_pinPending.empty() )
		{
			for ( const auto& kv : s_pinPending )
			{
				Entity* e = uidToEntity((Sint32)kv.first);
				if ( !e ) { continue; }
				sendToSamPeers(OP_PIN, pinBody(e, kv.second));
			}
			s_pinPending.clear();
		}

		// Terrain edits this tick, batched. The legacy packets first, for any machine that has not
		// said HELLO (a stock one understands only the dug wall); both lists hold the same edits
		// and both belong to s_tilesPendingKey's floor.
		if ( !s_tilesPending.empty() || !s_legacyPending.empty() )
		{
			if ( s_tilesPendingKey == currentFloor() )
			{
				// A fixed number of legacy packets per tick; the rest waits for the next one, in
				// order. The ordered op below carries the whole batch in one go -- SAMNet splits
				// and paces it itself.
				for ( std::size_t n = 0; n < LEGACY_PER_TICK && !s_legacyPending.empty(); ++n )
				{
					const TileEdit t = s_legacyPending.front();
					s_legacyPending.pop_front();
					sendLegacyTile(t.x, t.y, t.layer, t.tile);
				}
				if ( !s_tilesPending.empty() ) { sendTiles(-1, s_tilesPendingKey, s_tilesPending); }
			}
			else
			{
				// Whatever is left belongs to a floor this game has already left, and applying it
				// on the one we are on now would edit the wrong map.
				s_legacyPending.clear();
			}
			s_tilesPending.clear();
		}

		// The engine's own correction for a stock machine whose player a script nudged. new_x is
		// where actPlayer will put the host copy, which is where the move left it -- unless the
		// owner reported again in between, and then that report is the truth anyway.
		for ( int pn = 1; pn < MAXPLAYERS; ++pn )
		{
			if ( !s_pmovPending[pn] ) { continue; }
			s_pmovPending[pn] = false;
			if ( !SAMNet::isRemotePlayer(pn) || !players[pn] || !players[pn]->entity || !net_packet ) { continue; }
			const Entity* pe = players[pn]->entity;
			// Sent UNRELIABLY, as the engine sends it: the client's handler checks neither floor nor
			// sequence, so a late resend could drag the player back to a stale spot or onto the next
			// floor, while a lost one only loses the nudge -- the owner's next report overwrites
			// new_x either way.
			strcpy((char*)net_packet->data, "PMOV");
			SDLNet_Write16((Sint16)(pe->new_x * 32), &net_packet->data[4]);
			SDLNet_Write16((Sint16)(pe->new_y * 32), &net_packet->data[6]);
			net_packet->address.host = net_clients[pn - 1].host;
			net_packet->address.port = net_clients[pn - 1].port;
			net_packet->len = 8;
			sendPacket(net_sock, -1, net_packet, pn - 1);
		}

		// A size set on a player whose game had not said HELLO yet: either it has now (the held op
		// went with it and the owner has the new box), or SAMNet has declared that machine stock
		// and dropped the op -- and then the host must not keep a collision box the machine that
		// moves that player knows nothing about.
		for ( int pn = 1; pn < MAXPLAYERS; ++pn )
		{
			if ( !s_heldSize[pn].active ) { continue; }
			if ( SAMNet::peerHasSam(pn) ) { s_heldSize[pn].active = false; continue; }
			if ( !SAMNet::isRemotePlayer(pn) ) { s_heldSize[pn].active = false; continue; }
			if ( SAMNet::peerMayHaveSam(pn) ) { continue; }   // still inside the grace window
			s_heldSize[pn].active = false;
			Entity* pe = uidToEntity((Sint32)s_heldSize[pn].uid);
			if ( !pe || pe->behavior != &actPlayer ) { continue; }   // they died or left: nothing to put back
			if ( pe->sizex == s_heldSize[pn].sizex && pe->sizey == s_heldSize[pn].sizey ) { continue; }
			pe->sizex = s_heldSize[pn].sizex;
			pe->sizey = s_heldSize[pn].sizey;
			pe->flags[UPDATENEEDED] = true;
			queuePin(pe, SAMMpEntities::Changed::SIZE);   // the other S.A.M clients were told the new one
			SAMNet::warnOnce("mpent:size-stock:" + std::to_string(pn), "sam_set_entity_size was used on player "
				+ std::to_string(pn) + " before their game had said whether it runs S.A.M. It does not, so their"
				" own machine -- the one that moves them -- could never be told, and the size has been put back."
				" Set a player's size after player.on_player_joined has had a moment, or not at all for a player"
				" whose game is stock.");
		}

		// Gibs a script threw: the same packet serverSpawnGibForClient sends (actgib.cpp).
		if ( !s_gibsPending.empty() )
		{
			if ( net_packet )
			{
				for ( const PendingGib& g : s_gibsPending )
				{
					for ( int c = 1; c < MAXPLAYERS; ++c )
					{
						if ( client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() ) { continue; }
						strcpy((char*)net_packet->data, "SPGB");
						SDLNet_Write16((Uint16)g.x, &net_packet->data[4]);
						SDLNet_Write16((Uint16)g.y, &net_packet->data[6]);
						SDLNet_Write16((Uint16)g.z, &net_packet->data[8]);
						SDLNet_Write16((Uint16)g.sprite, &net_packet->data[10]);
						net_packet->data[12] = g.flags;
						net_packet->address.host = net_clients[c - 1].host;
						net_packet->address.port = net_clients[c - 1].port;
						net_packet->len = 13;
						sendPacketSafe(net_sock, -1, net_packet, c - 1);
					}
				}
			}
			s_gibsPending.clear();
		}

		// A companion's punch: S.A.M clients animate the lunge from skill[18].
		if ( !s_punchPending.empty() )
		{
			for ( Uint32 uid : s_punchPending )
			{
				Entity* e = uidToEntity((Sint32)uid);
				if ( e && SAMMpEntities::owns(e) ) { serverUpdateEntitySkill(e, 18); }
			}
			s_punchPending.clear();
		}

		// Moved or shoved items and gold: the first word now, and once the host copy rests, where.
		for ( auto it = s_pushed.begin(); it != s_pushed.end(); )
		{
			Entity* e = uidToEntity((Sint32)it->first);
			if ( !e || e != it->second.e ) { it = s_pushed.erase(it); continue; }
			if ( it->second.announce )
			{
				it->second.announce = false;
				it->second.since = ticks;
				sendGhoi(e, false);
				// actGoldBag runs its physics only while goldBouncing is 0, on the client too.
				if ( e->behavior == &actGoldBag ) { serverUpdateEntitySkill(e, 3); }
				++it;
				continue;
			}
			bool resting = true;
			if ( e->behavior == &actItem ) { resting = ( e->itemNotMoving != 0 ); }
			else if ( e->behavior == &actGoldBag ) { resting = ( e->goldBouncing != 0 ); }
			if ( resting )
			{
				sendGhoi(e, true);
				it = s_pushed.erase(it);
				continue;
			}
			if ( (Uint32)(ticks - it->second.since) > (Uint32)(TICKS_PER_SECOND * 30) )
			{
				it = s_pushed.erase(it);   // still sliding after half a minute: stop watching it
				continue;
			}
			++it;
		}

		// Forget entities that no longer exist, once a second.
		if ( ticks % TICKS_PER_SECOND == 0 )
		{
			for ( auto it = s_owned.begin(); it != s_owned.end(); )
			{
				if ( uidToEntity((Sint32)it->first) != it->second.e ) { it = s_owned.erase(it); }
				else { ++it; }
			}
			for ( auto it = s_pinned.begin(); it != s_pinned.end(); )
			{
				if ( !uidToEntity((Sint32)it->first) ) { it = s_pinned.erase(it); }
				else { ++it; }
			}
		}
	}

	void clientTick()
	{
		noteFloor();
		if ( !s_tilesWaiting.empty() )
		{
			drainTiles();
			while ( !s_tilesWaiting.empty() )
			{
				// The head is for a floor this machine has left, or has waited long enough that
				// its floor is not coming: drop it and let the rest through.
				const int got = applyTiles(s_tilesWaiting.front());
				if ( got == 0 && (Uint32)(ticks - s_tilesWaitingSince) < (Uint32)(TICKS_PER_SECOND * 20) ) { break; }
				if ( got == 0 )
				{
					SAMNet::warnOnce("mpent:tiles-lost", "Terrain edits from the host were for a floor this machine never"
						" reached; they were dropped.");
				}
				s_tilesWaiting.pop_front();
				s_tilesWaitingSince = ticks;
				drainTiles();
			}
		}
		if ( !s_pinsWaiting.empty() )
		{
			std::deque<WaitingPin> work;
			work.swap(s_pinsWaiting);
			for ( WaitingPin& w : work )
			{
				bool behind = false;
				for ( const WaitingPin& q : s_pinsWaiting ) { if ( q.uid == w.uid ) { behind = true; break; } }
				if ( !behind && applyPin(w.body) ) { continue; }
				if ( (Sint32)(w.until - ticks) <= 0 ) { continue; }   // that entity never arrived here
				s_pinsWaiting.push_back(std::move(w));
			}
		}
	}

	void tick()
	{
		if ( multiplayer == SERVER ) { hostTick(); }
		else if ( multiplayer == CLIENT ) { clientTick(); }
	}

	void clearHost()
	{
		s_owned.clear();
		s_pinPending.clear();
		s_pinned.clear();
		s_pushed.clear();
		s_punchPending.clear();
		s_gibsPending.clear();
		for ( int pn = 0; pn < MAXPLAYERS; ++pn ) { s_pmovPending[pn] = false; s_heldSize[pn] = HeldSize(); }
		s_invisible.clear();
		s_tilesPending.clear();
		s_legacyPending.clear();
		s_tileLog.clear();
		s_tilesPendingKey = FloorKey();
		s_tileLogKey = FloorKey();
	}

	void clearAll()
	{
		clearHost();
		s_tilesWaiting.clear();
		s_leftFloors.clear();
		s_haveLastFloor = false;
		s_pinsWaiting.clear();
		// Body and model tags are keyed by uid, and uids start again from 1 in the next game.
		SAMBodies::resetSession();
	}

	// Host: a client just said HELLO. Held ops already carry what happened before this; this
	// covers what fell off that queue (it is bounded) with the state as it stands now.
	void hello(int player)
	{
		if ( multiplayer != SERVER ) { return; }
		if ( !s_tileLog.empty() && s_tileLogKey == currentFloor() )
		{
			std::vector<TileEdit> all;
			all.reserve(s_tileLog.size());
			for ( const auto& kv : s_tileLog )
			{
				TileEdit t;
				t.x = (std::uint16_t)((kv.first >> 24) & 0xFFFF);
				t.y = (std::uint16_t)((kv.first >> 8) & 0xFFFF);
				t.layer = (std::uint8_t)(kv.first & 0xFF);
				t.tile = kv.second;
				all.push_back(t);
			}
			sendTiles(player, s_tileLogKey, all);
		}
		for ( const auto& kv : s_pinned )
		{
			Entity* e = uidToEntity((Sint32)kv.first);
			if ( e ) { SAMNet::sendToClient(player, OP_PIN, pinBody(e, kv.second)); }
		}
	}

	struct Registrar
	{
		Registrar()
		{
			SAMNet::onClientOp(OP_TILES, &onTilesOp);
			SAMNet::onClientOp(OP_PIN, &onPinOp);
			SAMNet::onClientOp(OP_NUDGE, &onNudgeOp);
		}
	};
	Registrar s_registrar;
	SAMNet::TickHook s_tickHook(&tick);
	SAMNet::ClearHook s_clearHook(&clearAll);
	SAMNet::HelloHook s_helloHook(&hello);

	const char* flagName(int flag)
	{
		switch ( flag )
		{
			case BLOCKSIGHT:       return "BLOCKSIGHT";
			case INVISIBLE:        return "INVISIBLE";
			case INVISIBLE_DITHER: return "INVISIBLE_DITHER";
			case BURNABLE:         return "BURNABLE";
			default:               return "that flag";
		}
	}
}

// -------------------------------------------------------------------- S.A.M-owned entities

void SAMMpEntities::adopt(Entity* e, Kind kind, int owner)
{
	if ( !e ) { return; }
	std::uint8_t wire = WIRE_LERP;
	std::uint8_t param = 0;
	switch ( kind )
	{
		case Kind::Scripted:
			// -7 is actEmpty on every client, stock included (net.cpp clientActions): the engine's
			// own "used on clients to permit dead reckoning" behaviour (actgeneral.cpp). Without a
			// behaviour a client never moves an entity past its first ENTU.
			e->skill[2] = -7;
			break;
		case Kind::Projectile:
			e->skill[2] = -7;
			// The host moves a projectile by fskill[2]/[3] itself; ENTU carries vel_x/vel_y, and a
			// client with a velocity dead-reckons between the 8 Hz updates instead of stuttering
			// through three ticks of interpolation and three of standing still. Nothing on the host
			// reads vel_x for it, so the bound below only keeps the wire honest: ENTU packs velocity
			// as (Sint16)(v * 32), and past 1023 a client would reckon the shot backwards.
			e->vel_x = std::max<real_t>(-1023.0, std::min<real_t>(1023.0, e->fskill[2]));
			e->vel_y = std::max<real_t>(-1023.0, std::min<real_t>(1023.0, e->fskill[3]));
			break;
		case Kind::Companion:
			e->skill[2] = -7;
			wire = WIRE_COMPANION;
			param = (std::uint8_t)( ( owner >= 0 && owner < MAXPLAYERS ) ? owner : 0 );
			// ENTU carries scale as (Uint8)(scale * 128), so past 1.99 every other player would see
			// a wrapped size. Clamped in every mode, like sam_set_scale: a mod is written once and
			// played in both.
			if ( e->scalex > 1.99 || e->scaley > 1.99 || e->scalez > 1.99 )
			{
				SAM_WARN(MOD, "sam_spawn_companion: scale is clamped to 1.99, the most the network can carry;"
					" other players would otherwise see it at the wrong size.");
				e->scalex = std::min<real_t>(e->scalex, 1.99);
				e->scaley = std::min<real_t>(e->scaley, 1.99);
				e->scalez = std::min<real_t>(e->scalez, 1.99);
			}
			break;
		case Kind::Portal:
			// skill[2] is left alone: sprite 254 already binds actPortal on every client, and the
			// portal skill aliases start at skill[0].
			wire = WIRE_PORTAL;
			break;
	}
	e->flags[UPDATENEEDED] = true;   // without it the ENTU sweep never sends the entity at all

	// A mod model is an APPENDED model index, and indices follow each machine's own mod load.
	// The name re-resolves on every machine, exactly as sam_set_model's does -- so this also
	// makes sam_get_model answer with that id, the same on the host and on every client.
	// Recorded as the entity's SPAWN model, not as an override, so sam_clear_model on it puts this
	// model back instead of dropping the entity to entity->sprite -- which is the host's own
	// appended index and means a different model, or none, on a machine whose mods sorted
	// differently. It is announced from the tick, never from inside this script call.
	if ( kind != Kind::Portal )
	{
		const std::string id = SAMModels::idForModelIndex((int)e->sprite);
		if ( !id.empty() ) { SAMBodies::setSpawnModel((uint32_t)e->getUID(), id); }
	}

	if ( multiplayer == SERVER && validUid(e->getUID()) )
	{
		Owned o;
		o.e = e;
		o.kind = wire;
		o.param = param;
		s_owned[e->getUID()] = o;
	}
}

bool SAMMpEntities::owns(const Entity* e)
{
	if ( !e || s_owned.empty() ) { return false; }
	auto it = s_owned.find(e->getUID());
	return it != s_owned.end() && it->second.e == e;
}

std::uint8_t SAMMpEntities::wireKind(const Entity* e, std::uint8_t& param)
{
	param = 0;
	if ( !e || s_owned.empty() ) { return 0; }   // the vanilla path: one empty() test
	auto it = s_owned.find(e->getUID());
	if ( it == s_owned.end() || it->second.e != e ) { return 0; }
	param = it->second.param;
	return it->second.kind;
}

bool SAMMpEntities::clientBind(Entity* e)
{
	// len is checked, not assumed: a vanilla host, or an older S.A.M one, sends at most one byte
	// past ENTITY_PACKET_LENGTH, and anything beyond would be whatever the previous packet left.
	if ( !e || !net_packet || net_packet->len < WIRE_AT + 2 ) { return false; }
	const std::uint8_t kind = net_packet->data[WIRE_AT];
	const std::uint8_t param = net_packet->data[WIRE_AT + 1];
	switch ( kind )
	{
		case WIRE_LERP:
			// Bound here, BEFORE the sprite switch, because that switch claims a whole list of
			// vanilla models first (3 is a torch and pins itself with NOUPDATE, 2 a door, 185/186
			// a switch and a gate...) and a script may well spawn its entity with one of them.
			e->behavior = &actEmpty;
			return true;
		case WIRE_COMPANION:
			e->skill[17] = ( param < MAXPLAYERS ) ? (Sint32)param : -1;
			e->behavior = &actSamCompanionClient;
			return true;
		case WIRE_PORTAL:
			// actPortal's decorative path (skill[19] == 1): it glows, turns and animates, and
			// skips the vanilla portal's tooltip, ambience and interaction -- none of which the
			// host's copy has, because skill[19] never crosses the wire.
			e->skill[19] = 1;
			e->behavior = &actPortal;
			return true;
		default:
			return false;
	}
}

// -------------------------------------------------------------------- script writes (host)

void SAMMpEntities::beginMove(Entity* e, double& startX, double& startY)
{
	startX = e ? (double)e->x : 0.0;
	startY = e ? (double)e->y : 0.0;
	if ( !e || multiplayer != SERVER || e->behavior != &actPlayer ) { return; }
	if ( !SAMNet::isRemotePlayer(e->skill[2]) ) { return; }
	// The same two lines actPlayer runs for a remote player every tick (actplayer.cpp):
	// new_x/new_y are what the owner last reported, and x is about to be reset to them anyway.
	if ( e->new_x > 0.001 ) { e->x = e->new_x; }
	if ( e->new_y > 0.001 ) { e->y = e->new_y; }
	startX = (double)e->x;
	startY = (double)e->y;
}

void SAMMpEntities::endMove(Entity* e, double startX, double startY)
{
	if ( !e ) { return; }
	// Anything that is not a player goes through moved(), so a relative move reaches the other
	// machines -- and leaves a resting item resting -- exactly as sam_set_position does.
	if ( e->behavior != &actPlayer ) { moved(e); return; }
	if ( multiplayer != SERVER ) { return; }
	const int pn = e->skill[2];
	if ( !SAMNet::isRemotePlayer(pn) ) { return; }
	// Keep the host copy where the move put it; actPlayer resets x to new_x next tick.
	e->new_x = e->x;
	e->new_y = e->y;
	const double dx = (double)e->x - startX;
	const double dy = (double)e->y - startY;
	if ( dx == 0.0 && dy == 0.0 ) { return; }
	if ( SAMNet::peerHasSam(pn) )
	{
		// The DELTA, on the ordered channel. The owner applies it to where it is NOW, through
		// its own collision. An absolute spot computed here is half a round trip old by the time
		// it lands and would yank a walking player back by that much.
		SAMNet::Writer w;
		w.u32((std::uint32_t)e->getUID());
		w.f64(dx);
		w.f64(dy);
		SAMNet::sendToClient(pn, OP_NUDGE, w.buf);
		return;
	}
	// A machine without S.A.M: the engine's own position correction (net.cpp's PMOV handler,
	// which a stock client runs), sent from the tick -- see s_pmovPending.
	if ( pn > 0 && pn < MAXPLAYERS ) { s_pmovPending[pn] = true; }
}

void SAMMpEntities::moved(Entity* e)
{
	if ( !e ) { return; }
	const bool item = ( e->behavior == &actItem );
	const bool gold = ( e->behavior == &actGoldBag );
	// An item or a bag that has COME TO REST stays at rest, in every mode. Putting a decoration in
	// a wall alcove, on a ledge, or anywhere over a pit is a use sam_set_position supports on
	// purpose (its own warning says so), and waking it hands it straight back to the engine's
	// physics -- which over a pit or in lava DELETES the item (actitem.cpp, "falling out of the
	// map"). Waking used to be here so that every machine settled such an item the same way, but
	// the only machine that would have settled it differently is a stock one, and a resting item
	// cannot be moved on a stock client at all (see below): all the wake really bought was
	// destroying placed items in singleplayer, where nearly every mod is written and tested.
	// An item still in flight is simply moved mid-fall and lands where it lands. And the call that
	// means "make this move", sam_push_entity, wakes the item itself before it sets a velocity.
	const bool atRest = ( item && e->itemNotMoving != 0 ) || ( gold && e->goldBouncing != 0 );
	if ( multiplayer != SERVER ) { return; }
	// Players move by TELE or a nudge; monsters and S.A.M-owned entities through ENTU, which
	// every client interpolates.
	if ( e->behavior == &actPlayer || e->behavior == &actMonster || owns(e) ) { return; }
	if ( ( item || gold ) && !atRest )
	{
		// The tick sends the engine's own 'GHOI' with where it now is (a stock client handles it
		// and wakes the item), a gold bag's goldBouncing = 0, and once the host copy rests, the
		// spot it rested on.
		trackPushed(e);
		return;
	}
	// ...and a resting one is pinned instead: the ordered PIN moves a S.A.M client's copy without
	// touching its physics, so it stays exactly as asleep as the host's. 'GHOI' cannot be used for
	// it -- the engine's own handler clears itemNotMoving (net.cpp) -- so a machine without S.A.M
	// keeps seeing it where it was, which is what the warning below says and the same limit every
	// other client-pinned prop has.
	queuePin(e, PIN_POS);
	if ( clientPins(e) && anyStockPeer() )
	{
		SAMNet::warnOnce("mpent:pinned-move", "A script moved something (a gate, a torch, a resting ground item or a"
			" similar prop) that Barony never updates on a client. Players whose game runs S.A.M see the move; a"
			" player without it keeps seeing it where it was.");
	}
}

void SAMMpEntities::teleported(Entity* e)
{
	if ( !e || multiplayer != SERVER || e->behavior != &actPlayer ) { return; }
	if ( !SAMNet::isRemotePlayer(e->skill[2]) ) { return; }
	// Entity::teleport moved x and sent TELE; actPlayer would reset x to new_x -- the old spot
	// -- until the owner's first report from the new one arrived.
	e->new_x = e->x;
	e->new_y = e->y;
}

void SAMMpEntities::pushed(Entity* e)
{
	if ( !e ) { return; }
	const bool gold = ( e->behavior == &actGoldBag );
	if ( !gold && e->behavior != &actItem ) { return; }
	// actGoldBag runs its physics only while goldBouncing is 0 and sets it to 1 once the bag
	// settles, which is where nearly every bag on the floor is -- so a shove wrote a velocity
	// nothing read, on every machine, singleplayer included.
	if ( gold ) { e->goldBouncing = 0; }
	if ( multiplayer != SERVER ) { return; }
	// The velocity never reached a client: ENTU is refused for a pinned item. GHOI carries it,
	// from the tick (trackPushed), with the velocity as it then stands.
	trackPushed(e);
}

void SAMMpEntities::transformed(Entity* e, unsigned what)
{
	if ( !e || multiplayer != SERVER ) { return; }
	what &= PIN_TRANSFORM;
	if ( !what ) { return; }
	if ( e->behavior == &actPlayer )
	{
		// ENTU skips a client's own player. Size is the one of these a player accepts (the others
		// are refused), and the owner computes that player's movement with it.
		if ( what & Changed::SIZE ) { queuePin(e, Changed::SIZE); }
		return;
	}
	if ( e->behavior == &actMonster || owns(e) ) { return; }
	queuePin(e, what);
	if ( clientPins(e) && anyStockPeer() )
	{
		SAMNet::warnOnce("mpent:pinned-transform", "A script turned, lifted, scaled or resized a ground item or a"
			" prop that Barony never updates on a client. Players whose game runs S.A.M see it; a player"
			" without it keeps seeing the old one.");
	}
}

// -------------------------------------------------------------------- refusals

bool SAMMpEntities::scaleSticks(const Entity* e, const char* who)
{
	if ( !e ) { return false; }
	if ( e->behavior == &actPlayer )
	{
		SAMNet::warnOnce(std::string(who) + "|scale:player", std::string(who) + " refused: the game resets a player's"
			" scale every frame on every machine, so this would not last a single frame.");
		return false;
	}
	if ( e->behavior == &actMonster && e->getMonsterTypeFromSprite() == SLIME )
	{
		SAMNet::warnOnce(std::string(who) + "|scale:slime", std::string(who) + " refused: a slime animates its own"
			" scale every frame on every machine, so this would not last a single frame.");
		return false;
	}
	return true;
}

bool SAMMpEntities::sizeReachesOwner(const Entity* e, const char* who)
{
	if ( !e || e->behavior != &actPlayer || multiplayer != SERVER ) { return true; }
	const int pn = e->skill[2];
	if ( !SAMNet::isRemotePlayer(pn) ) { return true; }
	if ( !SAMNet::peerMayHaveSam(pn) )
	{
		SAMNet::warnOnce(std::string(who) + "|size:stock:" + std::to_string(pn),
			std::string(who) + " refused: player " + std::to_string(pn) + "'s game is not running"
			" S.A.M, so their own machine -- the one that moves them -- would keep the old size while"
			" the host used the new one, and they would stick in doorways and rubber-band.");
		return false;
	}
	if ( !SAMNet::peerHasSam(pn) && pn > 0 && pn < MAXPLAYERS && !s_heldSize[pn].active )
	{
		// That player's game has not said HELLO yet, so we do not know whether it runs S.A.M.
		// Accept the call -- a mod sizing players in player.on_player_joined or on the first ticks
		// is doing a normal thing -- but remember the size it had, so the tick can put it back if
		// the peer is declared stock and the correction meant for them is dropped.
		s_heldSize[pn].uid = (Uint32)e->getUID();
		s_heldSize[pn].sizex = e->sizex;
		s_heldSize[pn].sizey = e->sizey;
		s_heldSize[pn].active = true;
	}
	return true;
}

bool SAMMpEntities::visibilitySticks(const Entity* e, const char* who)
{
	if ( !e ) { return false; }
	if ( isLimb(e) )
	{
		SAMNet::warnOnce(std::string(who) + "|vis:limb",
			std::string(who) + " refused: a creature's limb (its weapon, shield, helmet or an arm) is"
			" rebuilt from what that creature is wearing every frame, and INVISIBLE on a limb is how the game"
			" says the slot is EMPTY -- so this would be undone on the next tick. Hide the whole creature with"
			" sam_apply_effect(uid, \"INVISIBLE\", ticks), or take the item off it.");
		return false;
	}
	if ( e->behavior == &actMonster || e->behavior == &actPlayer )
	{
		SAMNet::warnOnce(std::string(who) + "|vis:creature",
			std::string(who) + " refused: a creature's visibility is recomputed from its invisibility"
			" effect every frame, so this is undone on the next tick"
			+ ( multiplayer == SERVER ? std::string(" -- and other players would be left holding the value set here") : std::string() )
			+ ". Use sam_apply_effect(uid, \"INVISIBLE\", ticks) instead.");
		return false;
	}
	if ( e->behavior == &actItem )
	{
		SAMNet::warnOnce(std::string(who) + "|vis:item",
			std::string(who) + " refused: the game makes a ground item visible again every frame,"
			" so this is undone on the next tick"
			+ ( multiplayer == SERVER ? std::string(" while other players keep it hidden") : std::string() )
			+ ". Remove it (sam_remove_entity) or move it out of sight.");
		return false;
	}
	return true;
}

bool SAMMpEntities::flagSticks(const Entity* e, int flag, const char* who)
{
	if ( !e ) { return false; }
	const bool creature = ( e->behavior == &actMonster || e->behavior == &actPlayer );
	// BLOCKSIGHT: actPlayer and nearly every monster's own bodypart code (monster_human.cpp and 31
	// more) rewrite it from the creature's invisibility every frame, on the host, and no ENTF is
	// ever sent for it -- so the host undid the call while every client kept the value for good.
	// INVISIBLE_DITHER is rewritten only for a PLAYER (actplayer.cpp); on a monster's main entity
	// nothing touches it, so it sticks there and is not refused.
	if ( ( creature && flag == BLOCKSIGHT ) || ( e->behavior == &actPlayer && flag == INVISIBLE_DITHER ) )
	{
		SAMNet::warnOnce(std::string(who) + "|flag:rewritten:" + flagName(flag),
			std::string(who) + ": " + flagName(flag) + " on a " + ( e->behavior == &actPlayer
			? "player" : "creature" ) + " is rewritten by the game every frame from its invisibility, so this"
			" would not last -- and in a connected game other players could be left holding the value set here.");
		return false;
	}
	// The same rewrite on a limb: actplayer.cpp and every monster_*.cpp set a bodypart's INVISIBLE
	// (and its dither) from the equipment every frame, so a script's value lasts one tick. The
	// other flags on a limb are left alone and still allowed.
	if ( ( flag == INVISIBLE || flag == INVISIBLE_DITHER ) && isLimb(e) )
	{
		SAMNet::warnOnce(std::string(who) + "|flag:limb:" + flagName(flag),
			std::string(who) + ": " + flagName(flag) + " on a creature's limb (its weapon, shield,"
			" helmet or an arm) is written by the game every frame from what that creature is wearing -- on a"
			" limb INVISIBLE is how the game says the slot is EMPTY -- so this would not last a single frame.");
		return false;
	}
	if ( e->behavior == &actItem && flag == BURNABLE )
	{
		SAMNet::warnOnce(std::string(who) + "|flag:burnable",
			std::string(who) + ": BURNABLE on a ground item is set again by the game every frame"
			" on every machine, so this would not last.");
		return false;
	}
	return true;
}

double SAMMpEntities::wirePoofScale(double scale, const char* who)
{
	// The host draws the scale it was given; spawnPoof sends every client Uint16(scale * 100),
	// which cannot hold a negative, a NaN, or anything past 655.35.
	if ( !std::isfinite(scale) )
	{
		SAMNet::warnOnce(std::string(who) + "|poofnan", std::string(who) + ": scale must be a finite number; used 1.");
		return 1.0;
	}
	if ( scale < 0.01 || scale > 655.0 )
	{
		SAMNet::warnOnce(std::string(who) + "|poofclamp", std::string(who) + ": a poof's scale is clamped to 0.01..655,"
			" which is what the network can carry; other players would otherwise see a different size.");
		return scale < 0.01 ? 0.01 : 655.0;
	}
	return scale;
}

// -------------------------------------------------------------------- engine hooks

void SAMMpEntities::beforeRemove(Entity* e)
{
	// The same steps the engine's own death path takes for a player's follower (actmonster.cpp,
	// "broadcast my player allies about my death"), in the same order: the uid leaves the
	// leader's list, a remote leader gets LDEL (before ~Entity sends ENTD, as in vanilla), and a
	// local leader's follower menu lets go of a pointer that is about to be freed.
	if ( !e || e->behavior != &actMonster ) { return; }
	Stat* st = e->getStats();
	if ( !st || st->leader_uid == 0 ) { return; }
	const Uint32 uid = e->getUID();
	for ( int c = 0; c < MAXPLAYERS; ++c )
	{
		if ( !players[c] || !players[c]->entity || !stats[c] ) { continue; }
		if ( st->leader_uid != players[c]->entity->getUID() ) { continue; }
		for ( node_t* n = stats[c]->FOLLOWERS.first; n != nullptr; n = n->next )
		{
			if ( !n->element || *((Uint32*)n->element) != uid ) { continue; }
			list_RemoveNode(n);
			if ( !players[c]->isLocalPlayer() )
			{
				serverRemoveClientFollower(c, uid);
			}
			else
			{
				if ( FollowerMenu[c].recentEntity && (FollowerMenu[c].recentEntity->getUID() == 0
					|| FollowerMenu[c].recentEntity->getUID() == uid) )
				{
					FollowerMenu[c].recentEntity = nullptr;
				}
				if ( FollowerMenu[c].followerToCommand == e )
				{
					FollowerMenu[c].closeFollowerMenuGUI();
				}
			}
			break;
		}
		break;
	}
}

bool SAMMpEntities::keepsUpdatingDespiteNoup(const Entity* e)
{
	return owns(e);
}

void SAMMpEntities::noteInvisibleByEffect(const Entity* e)
{
	if ( !e || multiplayer != SERVER || !SamEvent::anyScripts() ) { return; }
	s_invisible.insert(e->getUID());
}

bool SAMMpEntities::invisibleEffectEnded(const Entity* e)
{
	if ( s_invisible.empty() || !e ) { return false; }
	return s_invisible.erase(e->getUID()) > 0;
}

void SAMMpEntities::onFloorChange()
{
	// Every entity is freed with the old map, and every edit belonged to it.
	clearHost();
}

void SAMMpEntities::tileChanged(int x, int y, int layer, int tileId)
{
	if ( multiplayer != SERVER ) { return; }

	const FloorKey k = currentFloor();
	TileEdit t;
	t.x = (std::uint16_t)x;
	t.y = (std::uint16_t)y;
	t.layer = (std::uint8_t)layer;
	t.tile = (std::uint16_t)tileId;
	if ( s_tilesPendingKey != k ) { s_tilesPending.clear(); s_legacyPending.clear(); s_tilesPendingKey = k; }
	s_tilesPending.push_back(t);
	// Only when some machine still needs the vanilla form; the tick sends both, in edit order.
	if ( anyPeerWithoutSam() )
	{
		// Bounded, because the legacy list is drained at a budget and a script can fill it faster
		// than that. The ordered op above is unaffected: it carries every edit, in order.
		if ( s_legacyPending.size() < LEGACY_PENDING_MAX ) { s_legacyPending.push_back(t); }
		else
		{
			SAMNet::warnOnce("mpent:legacy-backlog", "A script is editing terrain faster than it can be sent to a"
				" player whose game does not run S.A.M; some of those edits will not reach them.");
		}
	}

	if ( s_tileLogKey != k ) { s_tileLog.clear(); s_tileLogKey = k; }
	const std::uint64_t key = tileKey(x, y, layer);
	if ( s_tileLog.size() < TILE_LOG_MAX || s_tileLog.count(key) ) { s_tileLog[key] = t.tile; }
	else
	{
		SAMNet::warnOnce("mpent:tilelog", "More than 65536 tiles were edited on this floor; a player whose game"
			" says hello late will not be sent the rest.");
	}
}

// -------------------------------------------------------------------- the companion's motion

void SAMMpEntities::companionStep(Entity* my, const Entity* p)
{
	if ( !my || !p ) { return; }
	// The same numbers the companion has always used (sam_lua_runtime.cpp's samCompanionBehavior
	// now calls this): 18 px behind the owner at rest, a 30 px lunge over the punch window.
	const double back = 18.0;
	const double reach = 30.0;
	double tx, ty, ease;
	if ( my->skill[18] > 0 )
	{
		// Thrust forward (in front of the owner) then back -- reach traces 0 -> 30 -> 0 across the
		// punch window, so it lunges out and returns.
		const double phase = (double)(COMPANION_PUNCH_TICKS - my->skill[18]) / (double)COMPANION_PUNCH_TICKS;
		const double r = reach * std::sin(phase * PI);
		tx = p->x + r * std::cos(p->yaw);
		ty = p->y + r * std::sin(p->yaw);
		ease = 0.6;                 // snap out fast for a punchy jab
		my->skill[18]--;
	}
	else
	{
		// Idle: float a set distance behind the owner (yaw + PI is directly behind).
		tx = p->x + back * std::cos(p->yaw + PI);
		ty = p->y + back * std::sin(p->yaw + PI);
		ease = 0.25;                // smooth trail
	}
	my->x += (tx - my->x) * ease;
	my->y += (ty - my->y) * ease;
	my->yaw = p->yaw;               // face where the owner faces
	my->fskill[0] += 0.05;          // gentle vertical bob
	my->z = p->z - COMPANION_RISE + 1.5 * std::sin(my->fskill[0]);
}

void SAMMpEntities::gibForClients(const Entity* gib)
{
	if ( !gib || multiplayer != SERVER ) { return; }
	if ( s_gibsPending.size() >= GIBS_PENDING_MAX )
	{
		SAMNet::warnOnce("mpent:gibs", "sam_gib: more than 256 gibs in one frame; the other players see only"
			" the first 256.");
		return;
	}
	// Exactly the fields serverSpawnGibForClient reads, as the gib stands right after spawnGib.
	PendingGib g;
	g.x = (Sint16)gib->x;
	g.y = (Sint16)gib->y;
	g.z = (Sint16)gib->z;
	g.sprite = (Sint16)gib->sprite;
	g.flags = (Uint8)( ( gib->flags[SPRITE] ? 1 << 0 : 0 ) | ( gib->skill[5] == 1 ? 1 << 1 : 0 ) );
	s_gibsPending.push_back(g);
}

void SAMMpEntities::companionPunched(Entity* e)
{
	// ENTS 18, from the tick: a S.A.M client's companion animator starts the same lunge from it.
	// A stock client cannot draw the companion's mod model at all, so it loses nothing.
	if ( !e || multiplayer != SERVER ) { return; }
	const Uint32 uid = e->getUID();
	if ( validUid(uid) ) { s_punchPending.insert(uid); }
}
