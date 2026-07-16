/*
 * sam_models — append mod-supplied .vox models to Barony's model table.
 * See sam_models.hpp for why this works and where it must run.
 */
#include "sam_models.hpp"
#include "sam_logger.hpp"

#include <map>
#include <cstdlib>
#include <cstring>

#if !defined(SAM_MODELS_NO_BARONY) && (defined(BARONY_SUPER_META) || __has_include("main.hpp"))
#	define SAM_MODELS_HAVE_BARONY 1
#	include "main.hpp"   // models, polymodels, nummodels, voxel_t, polymodel_t
#	include "files.hpp"  // loadVoxel
#	include "init.hpp"   // generatePolyModels, generateVBOs
#endif

namespace
{
	const char* MOD = "MODELS";

	// "namespace:name" -> engine model index. Only ever grows within a load; cleared
	// when mods unload.
	std::map<std::string, int> s_index;
}

int SAMModels::appendModels(const std::vector<Request>& requests)
{
#ifndef SAM_MODELS_HAVE_BARONY
	(void)requests;
	return 0;
#else
	if ( requests.empty() ) { return 0; }

	const int oldCount = (int)nummodels;
	if ( oldCount <= 0 || !models )
	{
		SAM_ERROR(MOD, "Model table not initialised yet — refusing to append "
			+ std::to_string(requests.size()) + " model(s). This must run after the engine has loaded models.txt.");
		return 0;
	}

	// Load every voxel FIRST, into a staging list. A .vox that fails to load must not
	// consume an index: the table is positional, so a hole would silently shift every
	// later model and mis-render other mods' content. Skip it instead.
	struct Staged { std::string id; voxel_t* vox; };
	std::vector<Staged> staged;
	staged.reserve(requests.size());

	for ( const Request& r : requests )
	{
		if ( r.id.empty() || r.physfsPath.empty() ) { continue; }
		if ( s_index.find(r.id) != s_index.end() )
		{
			SAM_WARN(MOD, "Model id '" + r.id + "' is already registered — skipping the duplicate.");
			continue;
		}
		// loadVoxel resolves the path through PhysFS itself, so any mounted mod folder
		// works with no path juggling on our side. It takes char* (not const).
		std::vector<char> path(r.physfsPath.begin(), r.physfsPath.end());
		path.push_back('\0');
		voxel_t* vox = loadVoxel(path.data());
		if ( !vox )
		{
			SAM_ERROR(MOD, "Could not load model '" + r.physfsPath + "' for [" + r.id
				+ "] — is the .vox actually in the mod folder? Skipping it; the rest still load.");
			continue;
		}
		staged.push_back({ r.id, vox });
	}

	if ( staged.empty() ) { return 0; }

	const int addCount = (int)staged.size();
	const int newCount = oldCount + addCount;

	// Grow the model table. realloc can MOVE these — safe here only because Barony
	// itself frees and rebuilds polymodels at this same point for its own mods, so
	// nothing is holding a pointer across the call.
	voxel_t** grownModels = (voxel_t**)realloc(models, sizeof(voxel_t*) * (size_t)newCount);
	if ( !grownModels )
	{
		SAM_ERROR(MOD, "Out of memory growing the model table to " + std::to_string(newCount)
			+ " — leaving the table untouched.");
		for ( Staged& s : staged ) { if ( s.vox ) { if (s.vox->data) { free(s.vox->data); } free(s.vox); } }
		return 0;
	}
	models = grownModels;
	for ( int i = 0; i < addCount; ++i ) { models[oldCount + i] = staged[i].vox; }

	// generatePolyModels only reallocs when asked for the FULL range (start==0 &&
	// end==nummodels); for a partial range it writes straight into the existing array.
	// So the polymodel table has to be grown here, before we call it, or it writes off
	// the end.
	polymodel_t* grownPoly = (polymodel_t*)realloc(polymodels, sizeof(polymodel_t) * (size_t)newCount);
	if ( !grownPoly )
	{
		SAM_ERROR(MOD, "Out of memory growing the polymodel table to " + std::to_string(newCount)
			+ " — leaving the table untouched.");
		// models[] is already grown, but nummodels is still oldCount, so the extra
		// slots are simply never read. Free the voxels we staged and bail.
		for ( Staged& s : staged ) { if ( s.vox ) { if (s.vox->data) { free(s.vox->data); } free(s.vox); } }
		return 0;
	}
	polymodels = grownPoly;
	memset(&polymodels[oldCount], 0, sizeof(polymodel_t) * (size_t)addCount);

	// Publish the new size only once BOTH tables are grown — generatePolyModels and
	// generateVBOs both read nummodels, and an inconsistent pair here reads off the end.
	nummodels = (Uint32)newCount;

	// Build geometry + GPU buffers for the appended range only. Cache rebuild is forced:
	// models.cache is written for the boot-time set, so it has nothing for these indices.
	const bool oldUseCache = useModelCache;
	useModelCache = false;
	generatePolyModels(oldCount, newCount, true);
	generateVBOs(oldCount, newCount);
	useModelCache = oldUseCache;

	for ( int i = 0; i < addCount; ++i )
	{
		s_index[staged[i].id] = oldCount + i;
		SAM_DEBUG(MOD, "  [" + staged[i].id + "] -> model index " + std::to_string(oldCount + i));
	}

	SAM_INFO(MOD, "Registered " + std::to_string(addCount) + " custom model(s); model table "
		+ std::to_string(oldCount) + " -> " + std::to_string(newCount) + ".");
	return addCount;
#endif
}

int SAMModels::modelIndexForId(const std::string& id)
{
	auto it = s_index.find(id);
	return ( it != s_index.end() ) ? it->second : -1;
}

int SAMModels::count()
{
	return (int)s_index.size();
}

void SAMModels::clear()
{
	// Only the id map is dropped. The engine's tables are rebuilt wholesale on the next
	// mod load, and shrinking them here would invalidate indices the renderer may still
	// be holding for the rest of this frame.
	s_index.clear();
}
