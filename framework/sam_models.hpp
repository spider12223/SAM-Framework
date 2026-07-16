/*
 * sam_models — let mods ship their own .vox models.
 *
 * WHY THIS IS POSSIBLE (it was long assumed it wasn't):
 * Barony's model table is not fixed at compile time. At boot, init.cpp counts the
 * LINES of models/models.txt into `nummodels`, then mallocs models[] and polymodels[]
 * from that count and builds geometry + VBOs for the whole range. Nothing about it is
 * static — the table is exactly as big as the list says. So a mod's model is just one
 * more entry, and the engine already has the machinery to build it:
 *   generatePolyModels(start, end, forceCacheRebuild)   // both take a RANGE
 *   generateVBOs(start, end)
 * We append our models at the END, so every vanilla index (0..nummodels-1 at boot)
 * keeps its meaning and nothing that references a vanilla model by index moves.
 *
 * WHERE THIS RUNS: from Mods::loadMods(), AFTER Barony's own model-replacement pass.
 * SAMLoader::load() has already parsed every mod by then, and running after the
 * vanilla pass means its [1, nummodels) range is computed against the OLD count, so
 * our appended models are invisible to it. No interference in either direction.
 *
 * THE ONE REAL HAZARD: growing the tables reallocs them, which can MOVE them. Anything
 * holding a raw pointer into models[]/polymodels[] across the call is left dangling.
 * We only grow at the one point in the frame where Barony already frees and rebuilds
 * polymodels wholesale for its own mods, so nothing is mid-flight there — but do not
 * move this call somewhere "more convenient" without re-checking that.
 */
#pragma once

#include <string>
#include <vector>

namespace SAMModels
{
	// A mod-supplied model awaiting registration.
	//   physfsPath — the model's logical path as Barony reads it (forward slashes,
	//                no leading slash), e.g. "mods/mymod/models/sword.vox". It must be
	//                resolvable through PhysFS, which it is for any mounted mod folder.
	//   id         — "namespace:name", how a mod refers to it from JSON.
	struct Request
	{
		std::string id;
		std::string physfsPath;
	};

	// Append every requested model to the engine's model table, building geometry and
	// VBOs for the new entries only. Safe to call with an empty list (no-op). Requests
	// whose .vox fails to load are skipped with a warning rather than shifting every
	// later index — an index table with holes would silently mis-render other mods.
	// Returns how many models were actually added.
	int appendModels(const std::vector<Request>& requests);

	// Engine model index for a registered "namespace:name", or -1 if unknown.
	// This is what makes an item's `model` field resolvable.
	int modelIndexForId(const std::string& id);

	// Number of models S.A.M has appended this session.
	int count();

	// Forget the id->index map. Called when mods unload. NOTE: this does NOT shrink the
	// engine's tables — Barony rebuilds those wholesale on the next load, and shrinking
	// them here would invalidate indices the renderer may still be holding this frame.
	void clear();
}
