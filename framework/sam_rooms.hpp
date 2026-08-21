/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_rooms.hpp — inject prefab rooms into Barony's existing levelsets.

	Barony builds a floor by loading an open arena (<set>.lmp) and stamping the prefab
	rooms <set>00.lmp, 01, ... into it until it is full. A mod can already ship a whole
	NEW levelset that way. This is the other half: adding rooms to a levelset that already
	exists, so a mod's content shows up in the ordinary mines/swamp/labyrinth run rather
	than only in its own branch. It is the closest thing Barony has to a Minecraft
	structure pack.

	THE ONE INVARIANT THAT MATTERS: room order IS the RNG index space.

	generateDungeon picks a room with `map_rng.rand() % numlevels` and then walks the room
	list linearly to that index, and in multiplayer EVERY CLIENT REGENERATES THE MAP ITSELF
	from a shared seed. So if the injected list is not byte-identical and identically
	ORDERED on every machine, players silently end up in different dungeons -- not a
	desync the engine detects, just two people describing different rooms to each other.

	PhysFS enumeration order is not guaranteed, and S.A.M's own mod load order follows the
	player's click order in the Mods menu, so NEITHER is safe to inherit. Every list this
	registry hands out is therefore sorted by a stable key ("namespace/relative path")
	before it leaves. That is the whole reason this is a registry rather than three lines
	inlined into the generator.

	Game build only.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

struct SAMModManifest;

class SAMRooms
{
public:
	// Read every mod's "rooms" block. Fully self-cleaning: drops any previous registration.
	static void applyAll(const std::vector<SAMModManifest>& mods);

	// Forget everything (mod unload / new game).
	static void clear();

	// True if ANY mod injected a room. The generator's fast path checks this so a vanilla
	// game never touches this registry while building a floor.
	static bool any();

	// Absolute .lmp paths to append to `levelset`'s room pool, ALREADY SORTED into the
	// canonical order every machine agrees on. Empty for a levelset nobody added to.
	static const std::vector<std::string>& roomsFor(const std::string& levelset);

	// Totals for the load summary.
	static int count();        // rooms registered
	static int levelsets();    // distinct levelsets added to
};
