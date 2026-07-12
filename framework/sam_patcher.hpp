/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_patcher.hpp
	Desc: layered / additive JSON patches.

	The single biggest compatibility win: instead of two mods each shipping their
	own copy of a vanilla data file (e.g. items/items.json) — where the last one
	mounted silently wins and the other's changes vanish — mods contribute
	*patches* that STACK. Each patch declares a target file and a list of
	operations (edit_field / add_entry / remove_field / multiply_field). S.A.M
	reads the currently-resolved target, applies every mod's operations in load
	order, and writes the merged result into a private overlay folder that it
	mounts at high priority — so Barony's own next read of that file (during
	initGameDatafiles) transparently picks up the combined result.

	Why an overlay folder and not the write dir directly: Barony's PhysFS write
	dir (<outputdir>/mods) is NOT mounted at the logical root, and the base data
	dir is mounted at LOWER priority than a prepend-mounted folder. So a naive
	write wouldn't win (or would clobber the real vanilla file, since on Windows
	outputdir == datadir == "./"). Instead S.A.M writes (via std::ofstream) to a
	dedicated `sam_patch_overlay/` folder — deliberately OUTSIDE `./mods/` so
	Barony's Local Mods browser never lists it as a fake mod — and PHYSFS_mounts
	it with prepend priority. Nothing vanilla is ever modified; the folder is
	fully owned by S.A.M and wiped on every load/unload.

	Game build only (needs PhysFS mounting + Barony's outputdir). The call in
	sam_loader.cpp is #ifndef EDITOR-guarded, and this .cpp is in GAME_SOURCES only.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>

struct SAMModManifest;  // from sam_workshop.hpp (full type only needed in the .cpp)

class SAMPatcher
{
public:
	// Read every patch file declared by every mod (already in dependency load
	// order), group operations by target file, apply them, write the merged
	// results to the overlay, and mount it with prepend priority. Call from
	// SAMLoader::load() BEFORE initGameDatafiles re-reads the data files.
	// Fully self-cleaning: unmounts + wipes any previous overlay first.
	static void applyAll(const std::vector<SAMModManifest>& mods);

	// Unmount + delete the overlay folder (call from SAMLoader::unload()).
	static void clear();

	// Stats from the most recent applyAll(), for the load summary.
	static int operationsApplied();
	static int filesPatched();
};
