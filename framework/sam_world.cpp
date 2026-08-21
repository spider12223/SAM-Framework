/*-------------------------------------------------------------------------------
	S.A.M Framework — world queries, terrain and mechanisms. See sam_world.hpp.
-------------------------------------------------------------------------------*/

#include "sam_world.hpp"
#include "sam_logger.hpp"

#include <algorithm>   // std::min/std::max on the lightmap channels
#include <cstring>     // strncmp (the fortress ambient check, copied from entityLight)

#ifndef EDITOR
#	include "main.hpp"        // map, MAPLAYERS, OBSTACLELAYER, swimmingtiles, lavatiles
#	include "game.hpp"
#	include "entity.hpp"
#	include "collision.hpp"   // lineTrace, checkObstacle, hit
#	include "paths.hpp"       // pathMapGrounded / pathMapFlying
#	include "items.hpp"
#	include "stat.hpp"
#	include "net.hpp"
#	include "player.hpp"     // players[] / isLocalPlayer -- the tile broadcast skips local players
#	define SAM_WORLD_HAVE_BARONY 1
#endif

namespace
{
	const char* MOD = "WORLD";

#ifdef SAM_WORLD_HAVE_BARONY
	bool inBounds(int x, int y)
	{
		return ( x >= 0 && y >= 0 && x < (int)map.width && y < (int)map.height );
	}

	// The engine's own indexing, copied from maps.cpp:613/621 rather than re-derived:
	//   layer + y*MAPLAYERS + x*MAPLAYERS*height
	int tileAt(int layer, int x, int y)
	{
		if ( !map.tiles || !inBounds(x, y) ) { return 0; }
		return map.tiles[layer + y * MAPLAYERS + x * MAPLAYERS * map.height];
	}

	// A tile id indexes the shared tile tables, so guard against a map referencing a tile
	// beyond what is loaded -- those arrays are sized by numtiles.
	bool tileFlag(const bool* table, int id)
	{
		if ( !table || id < 0 || id >= (int)numtiles ) { return false; }
		return table[id];
	}

	Entity* resolve(uint32_t uid)
	{
		return uidToEntity((Sint32)uid);
	}

	bool hostOnly(const char* fn)
	{
		if ( multiplayer == CLIENT )
		{
			SAM_WARN(MOD, std::string(fn) + " refused: host only.");
			return false;
		}
		return true;
	}

	// Set when a script edits the OBSTACLELAYER. The path maps are what connected() reads,
	// and every place the ENGINE knocks a wall down (actwallbuster.cpp:65, the dig spell at
	// castSpell.cpp:5414, actmagic.cpp:16003) calls generatePathMaps() straight afterwards.
	// setTile did not, so sam_tiles_connected -- advertised as the softlock check -- was
	// answering from the map as it stood before the edit, which is precisely backwards.
	//
	// Deferred rather than immediate: a script digging a corridor writes many tiles in a
	// loop, and regenerating per tile would make that quadratic for no benefit. Nothing reads
	// the path maps until connected() asks, so rebuild there, once.
	bool s_pathMapsDirty = false;

	// Tell every client about a tile the host just changed.
	//
	// Barony has no general tile-sync packet -- 'MAPT' carries tile ATTRIBUTES, not ids --
	// so this mirrors what the wall buster does for its own hole ('WACD', actwallbuster.cpp:56).
	// Without it a script that digs a passage opens it on the host alone: the host walks
	// through, every client still sees and collides with solid rock.
	void broadcastTile(int x, int y, int layer, int tileId)
	{
		if ( multiplayer != SERVER ) { return; }
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() ) { continue; }
			strcpy((char*)net_packet->data, "SAMT");
			SDLNet_Write16((Uint16)x, &net_packet->data[4]);
			SDLNet_Write16((Uint16)y, &net_packet->data[6]);
			net_packet->data[8] = (Uint8)layer;
			// Tile ids run to numtiles, which is well past 255 with mod tiles loaded.
			SDLNet_Write16((Uint16)tileId, &net_packet->data[9]);
			net_packet->address.host = net_clients[c - 1].host;
			net_packet->address.port = net_clients[c - 1].port;
			net_packet->len = 11;
			sendPacketSafe(net_sock, -1, net_packet, c - 1);
		}
	}
#endif
}

// ---- terrain -----------------------------------------------------------------------

SAMWorld::TileInfo SAMWorld::tile(int x, int y)
{
	TileInfo t;
#ifdef SAM_WORLD_HAVE_BARONY
	if ( !inBounds(x, y) ) { return t; }
	t.valid = true;
	t.wall    = tileAt(OBSTACLELAYER, x, y);
	t.floor   = tileAt(0, x, y);
	t.ceiling = tileAt(2, x, y);
	t.solid   = ( t.wall != 0 );
	// Liquid comes from the FLOOR tile, and the engine decides which tiles are liquid from
	// their image FILENAME (init.cpp) -- so a mod's own tile named "..lava.." lands here too.
	t.water = tileFlag(swimmingtiles, t.floor);
	t.lava  = tileFlag(lavatiles, t.floor);
	t.walkable = ( !t.solid && t.floor != 0 && !t.lava );
#else
	(void)x; (void)y;
#endif
	return t;
}

bool SAMWorld::setTile(int x, int y, int layer, int tileId)
{
#ifndef SAM_WORLD_HAVE_BARONY
	(void)x; (void)y; (void)layer; (void)tileId;
	return false;
#else
	if ( !hostOnly("sam_set_tile") ) { return false; }
	if ( !map.tiles || !inBounds(x, y) ) { return false; }
	if ( layer < 0 || layer >= MAPLAYERS ) { return false; }
	if ( tileId < 0 || tileId >= (int)numtiles ) { return false; }
	map.tiles[layer + y * MAPLAYERS + x * MAPLAYERS * map.height] = tileId;
	// A wall appearing or disappearing changes what can be walked to. Mark it; connected()
	// rebuilds before it answers.
	if ( layer == OBSTACLELAYER ) { s_pathMapsDirty = true; }
	broadcastTile(x, y, layer, tileId);
	return true;
#endif
}

bool SAMWorld::spawnable(int x, int y)
{
	const TileInfo t = tile(x, y);
	return t.valid && t.walkable;
}

// ---- spatial reasoning --------------------------------------------------------------

bool SAMWorld::lineOfSight(double x1, double y1, double x2, double y2,
	bool blockedByEntities, int& blockedX, int& blockedY)
{
	blockedX = -1; blockedY = -1;
#ifndef SAM_WORLD_HAVE_BARONY
	(void)x1; (void)y1; (void)x2; (void)y2; (void)blockedByEntities;
	return false;
#else
	// lineTrace works in world pixels and takes an angle + a maximum range, so convert.
	const real_t px1 = x1 * 16.0 + 8.0, py1 = y1 * 16.0 + 8.0;
	const real_t px2 = x2 * 16.0 + 8.0, py2 = y2 * 16.0 + 8.0;
	const real_t dx = px2 - px1, dy = py2 - py1;
	const real_t want = sqrt(dx * dx + dy * dy);
	if ( want < 0.001 ) { return true; }          // same tile: trivially visible
	const real_t angle = atan2(dy, dx);

	// A null `my` means "nothing is doing the looking", which is what we want for a pure
	// geometry question. lineTrace fills the GLOBAL `hit` with what stopped it -- and that
	// global is engine state: vanilla code does `lineTrace(...)` and then reads hit.entity /
	// hit.mapx on the following lines. A script can call this from inside a hook that fires
	// mid-engine-logic, so save and restore it. Three lines to make the hazard impossible
	// instead of auditing forty call sites and hoping.
	const hit_t savedHit = hit;
	const real_t got = lineTrace(nullptr, px1, py1, angle, want, 0, blockedByEntities);
	const bool clear = ( got >= want - 0.5 );
	if ( !clear )
	{
		blockedX = (int)(hit.x / 16.0);
		blockedY = (int)(hit.y / 16.0);
	}
	hit = savedHit;
	return clear;
#endif
}

bool SAMWorld::connected(int x1, int y1, int x2, int y2, bool flying)
{
#ifndef SAM_WORLD_HAVE_BARONY
	(void)x1; (void)y1; (void)x2; (void)y2; (void)flying;
	return false;
#else
	if ( !inBounds(x1, y1) || !inBounds(x2, y2) ) { return false; }
	// Answer against the map as it is NOW, not as it was before the script edited it.
	if ( s_pathMapsDirty ) { generatePathMaps(); s_pathMapsDirty = false; }
	const int* pmap = flying ? pathMapFlying : pathMapGrounded;
	if ( !pmap ) { return false; }
	// The path maps label every tile with a region id; two tiles are reachable from each
	// other exactly when they carry the same non-zero id. This is the engine's own answer,
	// and far cheaper than generating a path just to throw it away.
	const int a = pmap[y1 + x1 * map.height];
	const int b = pmap[y2 + x2 * map.height];
	return ( a != 0 && a == b );
#endif
}

int SAMWorld::lightAt(int x, int y, int player)
{
#ifndef SAM_WORLD_HAVE_BARONY
	(void)x; (void)y; (void)player;
	return 0;
#else
	// WHICH LIGHTMAP. There are MAXPLAYERS + 1 of them and slot 0 is not a spare -- it is the
	// SHARED one. light.cpp:117 fans an unowned light (a wall torch, a lit room) into every
	// slot including 0, while a light that belongs to a player goes only into that player's.
	// So slot 0 is "light that is there for everyone", and slots 1..MAXPLAYERS are what each
	// camera actually renders, including that player's own glow.
	//
	// The one a script wants is slot 0, because that is the one the GAME asks: Entity::entityLight
	// reads lightmaps[0] (entity.cpp:574), entityLightAfterReductions builds on it, and
	// actmonster.cpp calls that to decide whether a monster can see you. Reading a camera's
	// lightmap instead would answer a different question -- how bright the tile LOOKS to that
	// player -- and a stealth mod built on it would disagree with the AI it is trying to beat.
	//
	// player < 0 (the default) = the shared/AI lightmap. 0..MAXPLAYERS-1 = that camera's, for
	// the rarer case where a mod really does want what the screen shows.
	const int slot = ( player >= 0 && player < MAXPLAYERS ) ? player + 1 : 0;
	const auto& lm = lightmaps[slot];
	if ( !inBounds(x, y) || lm.empty() ) { return 0; }
	const size_t idx = (size_t)(y + x * map.height);
	if ( idx >= lm.size() ) { return 0; }

	// Same arithmetic as Entity::entityLight (entity.cpp:568), deliberately duplicated rather
	// than approximated: the whole value of this function is that the number it returns is the
	// number the monster AI thresholds on. Averaging the channels or taking the brightest would
	// both be defensible and both would be wrong here.
	const auto& v = lm[idx];
	float level = (v.x + v.y + v.z) / 3.f;
	if ( strncmp(map.filename, "fortress", 8) == 0 )
	{
		// The engine dims the ambient in the fortress before it judges vision; match it.
		level = (std::max(0.f, v.x - 32.f)
		       + std::max(0.f, v.y - 32.f)
		       + std::max(0.f, v.z - 40.f)) / 3.f;
	}
	if ( v.w > 0.f )
	{
		// w is the shadow term: a lit tile under cover reads darker than its colour suggests.
		const float shade = std::min(std::max(0.f, v.w), 255.f) / 255.f;
		level -= (level * shade * 0.8f);
	}
	int out = (int)level;
	if ( out < 0 ) { out = 0; }
	if ( out > 255 ) { out = 255; }
	return out;
#endif
}

// ---- finding things -----------------------------------------------------------------

std::vector<uint32_t> SAMWorld::findEntities(int x, int y, double radiusTiles,
	const std::string& kind)
{
	std::vector<uint32_t> out;
#ifdef SAM_WORLD_HAVE_BARONY
	if ( !map.entities ) { return out; }
	const real_t cx = x * 16.0 + 8.0, cy = y * 16.0 + 8.0;
	const real_t maxDist = radiusTiles * 16.0;

	for ( node_t* node = map.entities->first; node != nullptr; node = node->next )
	{
		Entity* e = (Entity*)node->element;
		if ( !e ) { continue; }

		bool match = false;
		if      ( kind == "any" )      { match = true; }
		else if ( kind == "door" )     { match = ( e->behavior == &actDoor ); }
		else if ( kind == "chest" )    { match = ( e->behavior == &actChest ); }
		else if ( kind == "fountain" ) { match = ( e->behavior == &actFountain ); }
		else if ( kind == "sink" )     { match = ( e->behavior == &actSink ); }
		else if ( kind == "switch" )   { match = ( e->behavior == &actSwitch ); }
		else if ( kind == "gate" )     { match = ( e->behavior == &actGate ); }
		else if ( kind == "ladder" )   { match = ( e->behavior == &actLadder ); }
		else if ( kind == "portal" )   { match = ( e->behavior == &actPortal ); }
		else if ( kind == "item" )     { match = ( e->behavior == &actItem ); }
		else if ( kind == "gold" )     { match = ( e->behavior == &actGoldBag ); }
		else if ( kind == "boulder" )  { match = ( e->behavior == &actBoulder ); }
		else if ( kind == "monster" )  { match = ( e->behavior == &actMonster ); }
		else if ( kind == "player" )   { match = ( e->behavior == &actPlayer ); }
		if ( !match ) { continue; }

		const real_t ddx = e->x - cx, ddy = e->y - cy;
		if ( (ddx * ddx + ddy * ddy) > (maxDist * maxDist) ) { continue; }
		out.push_back((uint32_t)e->getUID());
		if ( out.size() >= 64 ) { break; }   // same spirit as the 32 cap on nearby_entities
	}
#else
	(void)x; (void)y; (void)radiusTiles; (void)kind;
#endif
	return out;
}

bool SAMWorld::containerItems(uint32_t uid, std::vector<ItemInfo>& out)
{
	out.clear();
#ifndef SAM_WORLD_HAVE_BARONY
	(void)uid;
	return false;
#else
	// A client has no authoritative inventory for anything but itself, so without this it
	// returned SUCCESS with an empty list -- indistinguishable from a genuinely empty chest,
	// which is the worst possible answer.
	if ( !hostOnly("sam_get_container_items") ) { return false; }
	Entity* e = resolve(uid);
	if ( !e ) { return false; }

	list_t* inv = nullptr;
	if ( e->behavior == &actChest )
	{
		inv = e->getChestInventoryList();
	}
	else if ( e->behavior == &actMonster )
	{
		if ( Stat* s = e->getStats() ) { inv = &s->inventory; }
	}
	if ( !inv ) { return false; }

	for ( node_t* n = inv->first; n != nullptr; n = n->next )
	{
		Item* it = (Item*)n->element;
		if ( !it ) { continue; }
		ItemInfo i;
		i.type = (int)it->type;
		i.count = (int)it->count;
		i.status = (int)it->status;
		i.beatitude = (int)it->beatitude;
		i.identified = it->identified;
		// The identified name, so a script can match on something readable. Unidentified
		// items deliberately still report their real type: this is a host-side query for
		// mod logic, not something shown to a player.
		if ( it->type >= 0 && it->type < NUM_ITEM_SLOTS ) { i.name = items[it->type].getIdentifiedName(); }
		out.push_back(i);
	}
	return true;
#endif
}

// ---- mechanisms ---------------------------------------------------------------------

bool SAMWorld::setDoor(uint32_t uid, bool open)
{
#ifndef SAM_WORLD_HAVE_BARONY
	(void)uid; (void)open;
	return false;
#else
	if ( !hostOnly("sam_set_door") ) { return false; }
	Entity* e = resolve(uid);
	if ( !e || e->behavior != &actDoor ) { return false; }
	e->doorStatus = open ? 1 : 0;
	// Doors are replicated by their own act function reading doorStatus, so nothing extra
	// to send; but wake the timer so the swing animation plays instead of snapping.
	e->doorTimer = 0;
	return true;
#endif
}

bool SAMWorld::setDoorLocked(uint32_t uid, bool locked)
{
#ifndef SAM_WORLD_HAVE_BARONY
	(void)uid; (void)locked;
	return false;
#else
	if ( !hostOnly("sam_set_door_locked") ) { return false; }
	Entity* e = resolve(uid);
	if ( !e || e->behavior != &actDoor ) { return false; }
	e->doorLocked = locked ? 1 : 0;
	return true;
#endif
}

bool SAMWorld::powerEntity(uint32_t uid, bool on)
{
#ifndef SAM_WORLD_HAVE_BARONY
	(void)uid; (void)on;
	return false;
#else
	if ( !hostOnly("sam_power_entity") ) { return false; }
	Entity* e = resolve(uid);
	if ( !e ) { return false; }
	if ( on ) { e->mechanismPowerOn(); }
	else      { e->mechanismPowerOff(); }
	return true;
#endif
}

bool SAMWorld::toggleSwitch(uint32_t uid)
{
#ifndef SAM_WORLD_HAVE_BARONY
	(void)uid;
	return false;
#else
	if ( !hostOnly("sam_toggle_switch") ) { return false; }
	Entity* e = resolve(uid);
	if ( !e ) { return false; }
	// Entity::toggleSwitch defaults to flipping skill[0] and replicating it. skill[0] means
	// something completely different on a monster or an item, so accepting any uid would
	// corrupt that entity and broadcast the corruption to every client. Only real switches.
	if ( e->behavior != &actSwitch )
	{
		SAM_WARN(MOD, "sam_toggle_switch: uid " + std::to_string((unsigned long long)uid)
			+ " is not a switch. Find one with sam_find_entities(x, y, r, \"switch\").");
		return false;
	}
	e->toggleSwitch();
	return true;
#endif
}

// ---- level context ------------------------------------------------------------------

SAMWorld::LevelInfo SAMWorld::level()
{
	LevelInfo l;
#ifdef SAM_WORLD_HAVE_BARONY
	l.floor = currentlevel;
	l.name = map.name;
	l.author = map.author;
	l.width = (int)map.width;
	l.height = (int)map.height;
	l.secret = secretlevel;
	l.skybox = (int)map.skybox;
	l.noDigging    = ( MFLAG_DISABLEDIGGING != 0 );
	l.noTeleport   = ( MFLAG_DISABLETELEPORT != 0 );
	l.noLevitation = ( MFLAG_DISABLELEVITATION != 0 );
#endif
	return l;
}
