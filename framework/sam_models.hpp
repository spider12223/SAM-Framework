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
		// Who asked for it -- an item id, a class id, a namespace. Only used to make the
		// log line name the thing the modder has to go and edit, instead of naming the path
		// twice and leaving them to guess which JSON file mentions it. Optional: older call
		// sites brace-init two members and this stays empty.
		std::string owner;
	};

	// One registered model, for /sam_models and anything else that needs to see the table.
	struct Entry
	{
		std::string id;
		std::string path;
		int index = -1;
		// True when this resolved to a base-game file rather than the mod's own. Decided by
		// the loader and carried here, so a caller cannot re-derive it and disagree.
		bool baseGame = false;
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

	// ---- naming a VANILLA model by path instead of by index --------------------------
	//
	// Called once per line while init.cpp loads models.txt, before anything else in the
	// framework runs. Keeps the path the engine was about to discard so a mod can say
	// what it means instead of a number.
	void noteVanillaModelPath(int index, const std::string& path);

	// Resolve a vanilla model path to its engine index, or -1.
	//
	// Accepts the full models.txt path, the same path without its leading "models/", or
	// a bare filename when that filename appears exactly once in the table. Case and
	// slash direction are normalised, because a path copied off Windows arrives with
	// backslashes and a path typed from memory arrives in the wrong case.
	//
	// `ambiguous` is set when a bare filename matched more than one model -- the caller
	// needs that to tell the author to be more specific rather than silently picking one.
	int vanillaModelIndexForPath(const std::string& path, bool* ambiguous = nullptr);

	// The file a registered id was loaded from, or "" if that id is unknown.
	std::string pathForId(const std::string& id);

	// Everything registered, id-sorted. Powers /sam_models: the model registry is the one a
	// mod is most likely to get wrong, and it was the only one with no way to inspect it.
	std::vector<Entry> list();

	// Number of models S.A.M has appended this session.
	int count();

	// Forget the id->index map. Called when mods unload. NOTE: this does NOT shrink the
	// engine's tables — Barony rebuilds those wholesale on the next load, and shrinking
	// them here would invalidate indices the renderer may still be holding this frame.
	void clear();
}
