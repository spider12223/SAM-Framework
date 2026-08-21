/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_world.hpp — world queries, terrain and mechanisms.

	WHY THIS EXISTS. Until now a script could see creatures and nothing else:
	sam_get_nearby_entities skips any entity whose behavior is not actMonster or actPlayer,
	so doors, chests, levers, gold and dropped items were invisible. And the only spatial
	query available was raw distance, which sees straight through solid rock -- so a mod
	could not check line of sight, could not tell whether a tile was walkable before
	spawning something on it, and could not tell whether it had just walled off the exit.

	The framework also fires world.on_door_opened and world.on_switch_toggled while giving
	a script no verb to open a door or throw a lever. This closes that.

	Everything here is a thin wrapper over machinery Barony already has (lineTrace,
	pathMapGrounded, mechanismPowerOn, the map.tiles array). Nothing invents a system.

	Coordinates are TILE coordinates throughout, matching sam_get_position.

	Game build only.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Entity;

class SAMWorld
{
public:
	// ---- terrain -------------------------------------------------------------------

	struct TileInfo
	{
		int wall = 0;         // OBSTACLELAYER tile id; 0 means open
		int floor = 0;
		int ceiling = 0;
		bool solid = false;   // a wall is in the way
		bool water = false;
		bool lava = false;
		bool walkable = false;// open, in bounds, and not lava
		bool valid = false;   // false = out of bounds
	};

	static TileInfo tile(int x, int y);

	// Write a tile. layer 0 = floor, 1 = wall, 2 = ceiling. Host only.
	// Refuses out of bounds rather than corrupting the map array.
	static bool setTile(int x, int y, int layer, int tileId);

	// Is this a sane place to put something: in bounds, not inside a wall, not lava.
	static bool spawnable(int x, int y);

	// ---- spatial reasoning ---------------------------------------------------------

	// Can a straight line get from A to B? Returns the blocking tile when it cannot.
	static bool lineOfSight(double x1, double y1, double x2, double y2,
		bool blockedByEntities, int& blockedX, int& blockedY);

	// Can something WALK (or fly) from A to B at all? This is the softlock check: after a
	// mod edits terrain it can ask whether the exit is still reachable before committing.
	static bool connected(int x1, int y1, int x2, int y2, bool flying);

	// Light level at a tile, 0..255, computed exactly the way Entity::entityLight does --
	// so it is literally the number monster vision thresholds on, not an approximation of it.
	// That is the whole point: a stealth mod can agree with the AI instead of guessing.
	//
	// `player` picks the lightmap. The default of -1 means the SHARED map (slot 0), which is
	// what the AI reads. Passing 0..MAXPLAYERS-1 gives that camera's rendered light instead,
	// which includes the player's own glow and answers a different question.
	static int lightAt(int x, int y, int player = -1);

	// ---- finding things ------------------------------------------------------------

	// Entities of a KIND near a tile. kind: "door" "chest" "fountain" "sink" "switch"
	// "gate" "ladder" "portal" "item" "gold" "boulder" "monster" "player" "any".
	// This is the gap sam_get_nearby_entities leaves: it returns creatures only.
	static std::vector<uint32_t> findEntities(int x, int y, double radiusTiles,
		const std::string& kind);

	// What is inside a chest, or what a creature is carrying.
	struct ItemInfo
	{
		int type = 0;
		std::string name;
		int count = 1;
		int status = 0;
		int beatitude = 0;
		bool identified = false;
	};
	static bool containerItems(uint32_t uid, std::vector<ItemInfo>& out);

	// ---- mechanisms ----------------------------------------------------------------
	// All host only: these change world state and the result replicates normally.

	static bool setDoor(uint32_t uid, bool open);
	static bool setDoorLocked(uint32_t uid, bool locked);
	static bool powerEntity(uint32_t uid, bool on);
	static bool toggleSwitch(uint32_t uid);

	// ---- level context -------------------------------------------------------------

	struct LevelInfo
	{
		int floor = 0;
		std::string name;
		std::string author;
		int width = 0, height = 0;
		bool secret = false;
		int skybox = 0;
		bool noDigging = false, noTeleport = false, noLevitation = false;
	};
	static LevelInfo level();
};
