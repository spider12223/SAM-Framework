/*
 * sam_models — append mod-supplied .vox models to Barony's model table.
 * See sam_models.hpp for why this works and where it must run.
 */
#include "sam_models.hpp"
#include "sam_logger.hpp"

#include <map>
#include <vector>
#include <string>
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

	// id -> where it landed, and what file it came from.
	//
	// The path is kept because the id alone cannot tell a harmless RE-registration (Barony
	// calls loadMods on every Play, so every model is offered again with the same id AND the
	// same path) from a real collision (two mods claiming one id with different files). The
	// first version warned on both, which meant every model produced a WARN from the second
	// game onward -- a log full of "already registered" that reads exactly like an authoring
	// error and buries the one line that matters.
	struct Registration
	{
		int index = -1;
		std::string path;
		// Recorded at append time so /sam_models reports exactly what the loader decided.
		// The command used to re-derive this itself and got a different answer, flagging the
		// framework's own sam_builtin/ models as the modder's mistake.
		bool baseGame = false;
	};
	std::map<std::string, Registration> s_index;

	// Vanilla models by path, built once at boot from models.txt. Two maps because a bare
	// filename is not unique in general ("head.vox" appears under several creatures), so
	// it needs a count before it can be trusted as an answer.
	std::map<std::string, int> s_vanillaByPath;   // normalised full path -> index
	std::map<std::string, std::vector<int>> s_vanillaByFile;  // bare filename -> indices

	// Lower case, forward slashes, no leading slash, no leading "models/". Everything
	// that varies between two ways of writing the same path, removed.
	std::string normalisePath(const std::string& in)
	{
		std::string out;
		out.reserve(in.size());
		for ( char c : in )
		{
			if ( c == '\\' ) { c = '/'; }
			if ( c >= 'A' && c <= 'Z' ) { c = (char)(c - 'A' + 'a'); }
			if ( c == '\r' || c == '\n' || c == '\t' ) { continue; }
			out += c;
		}
		while ( !out.empty() && (out.back() == ' ') ) { out.pop_back(); }
		size_t b = out.find_first_not_of(" /");
		if ( b == std::string::npos ) { return std::string(); }
		out = out.substr(b);
		if ( out.compare(0, 7, "models/") == 0 ) { out = out.substr(7); }
		return out;
	}

	std::string fileNameOf(const std::string& normalised)
	{
		const size_t slash = normalised.find_last_of('/');
		return ( slash == std::string::npos ) ? normalised : normalised.substr(slash + 1);
	}
}

namespace
{
#ifdef SAM_MODELS_HAVE_BARONY
	// Barony's .vox is a "slab": three little-endian int32 dimensions, then w*h*d palette
	// index bytes, then a 768-byte RGB palette, so the file is exactly 12 + w*h*d + 768 bytes.
	//
	// Measured across every .vox the game ships: 2410 of 2411 match. The single exception,
	// models/decorations/ceiling_bbrick.vox, is a 12-byte header-only stub that claims 32x32x1
	// and carries no data at all -- and it is not listed in models.txt, so the engine never
	// loads it either. Rejecting it costs nothing.
	//
	// WHY WE CHECK BEFORE CALLING loadVoxel. loadVoxel (files.cpp:2280) reads the first int32
	// straight into sizex and then does
	//     malloc(sizeof(Uint8) * sizex * sizey * sizez);
	//     memset(model->data, 0, ...);
	// with no validation of the dimensions and NO null check on the malloc. A file that is
	// neither slab nor MagicaVoxel -- a corrupt export, a truncated copy, a renamed PNG --
	// gives absurd dimensions, the signed multiply overflows, malloc returns nullptr and the
	// memset segfaults. The game dies at the loading screen with nothing in any log.
	//
	// So sam_models' documented "a model that fails to load is skipped with a warning" simply
	// could not hold: it only covers files loadVoxel manages to REJECT, never ones it crashes
	// on. Validating here is what makes that promise true.
	//
	// Returns true when the file is a slab we can safely hand over. On false, `why` explains
	// it in terms the modder can act on.
	bool samValidateSlab(const std::string& physfsPath, std::string& why)
	{
		why.clear();
		if ( !PHYSFS_getRealDir(physfsPath.c_str()) )
		{
			why = "No file at that path. Check the spelling and that it is inside your mod folder.";
			return false;
		}
		PHYSFS_File* fh = PHYSFS_openRead(physfsPath.c_str());
		if ( !fh )
		{
			why = "The file exists but could not be opened for reading.";
			return false;
		}
		const PHYSFS_sint64 len = PHYSFS_fileLength(fh);
		unsigned char head[12] = { 0 };
		const PHYSFS_sint64 got = PHYSFS_readBytes(fh, head, 12);
		PHYSFS_close(fh);

		if ( got < 12 )
		{
			why = "The file is only " + std::to_string((long long)len)
				+ " bytes, too short to be a .vox at all. Did the copy finish?";
			return false;
		}
		// MagicaVoxel's own format is RIFF-ish and starts with the ASCII magic "VOX ". It
		// shares an extension with Barony's slab and nothing else, and exporting from
		// MagicaVoxel without converting is by far the most common way a model fails.
		if ( head[0] == 'V' && head[1] == 'O' && head[2] == 'X' && head[3] == ' ' )
		{
			why = "This is a MagicaVoxel .vox, which Barony cannot read. The two formats share"
				" an extension and nothing else -- convert it to Barony slab format.";
			return false;
		}

		auto rd32 = [&](int off) -> long long {
			return (long long)((unsigned)head[off] | ((unsigned)head[off + 1] << 8)
				| ((unsigned)head[off + 2] << 16) | ((unsigned)head[off + 3] << 24));
		};
		const long long w = rd32(0), h = rd32(4), d = rd32(8);
		// A signed int32 read as unsigned above stays positive here; anything enormous or
		// zero is not a model. 1024 on a side is already far past anything Barony ships.
		if ( w <= 0 || h <= 0 || d <= 0 || w > 1024 || h > 1024 || d > 1024 )
		{
			why = "The header says this model is " + std::to_string(w) + "x" + std::to_string(h)
				+ "x" + std::to_string(d) + ", which is not a real size. This is not a Barony"
				" slab .vox.";
			return false;
		}
		const long long expect = 12 + (w * h * d) + 768;
		if ( (long long)len != expect )
		{
			why = "The header says " + std::to_string(w) + "x" + std::to_string(h) + "x"
				+ std::to_string(d) + ", so a Barony slab would be exactly "
				+ std::to_string(expect) + " bytes, but the file is "
				+ std::to_string((long long)len) + ". It is either truncated or not a slab .vox.";
			return false;
		}
		return true;
	}

	// Did this path resolve to the BASE GAME rather than to a mod?
	//
	// loadVoxel goes through PhysFS, and the base game is mounted in PhysFS, so a vanilla
	// path like models/creatures/goatman/goatman_named/gharbad_head.vox loads perfectly and
	// the framework happily registers a SECOND COPY of a stock model as if the mod shipped it.
	// The modder gets a clean log and a floating limb on screen, which is exactly the report
	// that prompted this check. The engine already does the same test in files.cpp:3957 --
	// PHYSFS_getRealDir returns "./" for base-game content.
	bool samIsBaseGameFile(const std::string& physfsPath)
	{
		// The framework's own bundled assets live in the game directory by design -- the
		// built-in Hunter's Workbench writes its models to sam_builtin/ (sam_workbench.cpp).
		// They resolve to "./" like base-game content, but they are not a modder's mistake,
		// and warning about them would put two spurious lines in every single startup log.
		if ( physfsPath.compare(0, 12, "sam_builtin/") == 0 ) { return false; }
		const char* real = PHYSFS_getRealDir(physfsPath.c_str());
		if ( !real ) { return false; }
		return ( strcmp(real, "./") == 0 );
	}

	// Vanilla creature models are single LIMBS, not whole bodies, so a body_model pointed at
	// one draws a floating arm. Worth saying out loud when we can see it coming.
	bool samLooksLikeCreatureLimb(const std::string& physfsPath)
	{
		return ( physfsPath.find("models/creatures/") != std::string::npos );
	}
#endif
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
	struct Staged { std::string id; std::string path; voxel_t* vox; };
	std::vector<Staged> staged;
	staged.reserve(requests.size());

	for ( const Request& r : requests )
	{
		if ( r.id.empty() || r.physfsPath.empty() ) { continue; }

		// `owner` is only for the message; it is empty for older call sites.
		const std::string who = r.owner.empty() ? ("[" + r.id + "]") : ("[" + r.owner + "]");

		auto known = s_index.find(r.id);
		if ( known != s_index.end() )
		{
			// Same id, same file: this is just Barony calling loadMods again (it does so on
			// every Play). Nothing is wrong and nothing needs saying.
			if ( known->second.path == r.physfsPath ) { continue; }
			// Same id, DIFFERENT file: a real clash, and the second mod silently loses.
			SAM_WARN(MOD, "Two different models claim the id '" + r.id + "': already loaded from '"
				+ known->second.path + "', now also requested from '" + r.physfsPath
				+ "' by " + who + ". Keeping the first. Namespace your model folder"
				" (models/<yourmod>/...) so ids cannot collide.");
			continue;
		}

		// Refuse a file we cannot prove is a slab, BEFORE loadVoxel gets a chance to
		// segfault on it. See samValidateSlab.
		std::string why;
		if ( !samValidateSlab(r.physfsPath, why) )
		{
			SAM_ERROR(MOD, "Model '" + r.physfsPath + "' for " + who + " was not loaded. "
				+ why + " Skipping it; the rest still load.");
			continue;
		}

		// It loads -- but did it come from the mod, or from the base game?
		if ( samIsBaseGameFile(r.physfsPath) )
		{
			std::string extra;
			if ( samLooksLikeCreatureLimb(r.physfsPath) )
			{
				extra = " Note that vanilla creature models are single LIMBS (a head, an arm),"
					" not whole bodies — using one as a body model draws a floating limb.";
			}
			SAM_WARN(MOD, "Model '" + r.physfsPath + "' for " + who + " is a BASE GAME file,"
				" not one your mod ships. It will load, but you are registering a second copy"
				" of a stock model." + extra
				+ " If you meant to ship your own .vox, put it under your mod folder.");
		}

		// loadVoxel resolves the path through PhysFS itself, so any mounted mod folder
		// works with no path juggling on our side. It takes char* (not const).
		std::vector<char> path(r.physfsPath.begin(), r.physfsPath.end());
		path.push_back('\0');
		voxel_t* vox = loadVoxel(path.data());
		if ( !vox )
		{
			// Validation passed, so this is something else -- out of memory, or a format
			// quirk we did not model. Say so honestly rather than guessing.
			SAM_ERROR(MOD, "Could not load model '" + r.physfsPath + "' for " + who
				+ " even though it looks like a valid slab .vox. Skipping it; the rest still load.");
			continue;
		}
		staged.push_back({ r.id, r.physfsPath, vox });
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
		s_index[staged[i].id] = Registration{ oldCount + i, staged[i].path,
			samIsBaseGameFile(staged[i].path) };
		// INFO, not DEBUG. This mapping is the single most useful line in the log when a
		// model does not show up, and DEBUG is off unless an environment variable nobody
		// outside this repo knows about is set -- so in practice it was never seen.
		SAM_INFO(MOD, "  model [" + staged[i].id + "] -> index "
			+ std::to_string(oldCount + i) + "  (" + staged[i].path + ")");
	}

	SAM_INFO(MOD, "Registered " + std::to_string(addCount) + " custom model(s); model table "
		+ std::to_string(oldCount) + " -> " + std::to_string(newCount) + ".");
	return addCount;
#endif
}

void SAMModels::noteVanillaModelPath(int index, const std::string& path)
{
	const std::string norm = normalisePath(path);
	if ( norm.empty() ) { return; }
	// First writer wins: if models.txt lists the same file twice, the earlier index is
	// the one every existing mod's number already refers to.
	if ( s_vanillaByPath.find(norm) == s_vanillaByPath.end() ) { s_vanillaByPath[norm] = index; }
	s_vanillaByFile[fileNameOf(norm)].push_back(index);
}

int SAMModels::vanillaModelIndexForPath(const std::string& path, bool* ambiguous)
{
	if ( ambiguous ) { *ambiguous = false; }
	const std::string norm = normalisePath(path);
	if ( norm.empty() || s_vanillaByPath.empty() ) { return -1; }

	auto exact = s_vanillaByPath.find(norm);
	if ( exact != s_vanillaByPath.end() ) { return exact->second; }

	// Only try the bare-filename form when the author actually gave a bare filename.
	// Falling back to it for a full path that simply did not match would answer a
	// question they did not ask, using a model from some other creature's folder.
	if ( norm.find('/') != std::string::npos ) { return -1; }

	auto byFile = s_vanillaByFile.find(norm);
	if ( byFile == s_vanillaByFile.end() || byFile->second.empty() ) { return -1; }
	if ( byFile->second.size() > 1 )
	{
		if ( ambiguous ) { *ambiguous = true; }
		return -1;
	}
	return byFile->second.front();
}

int SAMModels::modelIndexForId(const std::string& id)
{
	auto it = s_index.find(id);
	return ( it != s_index.end() ) ? it->second.index : -1;
}

std::string SAMModels::pathForId(const std::string& id)
{
	auto it = s_index.find(id);
	return ( it != s_index.end() ) ? it->second.path : std::string();
}

std::vector<SAMModels::Entry> SAMModels::list()
{
	std::vector<Entry> out;
	out.reserve(s_index.size());
	for ( const auto& kv : s_index )
	{
		out.push_back(Entry{ kv.first, kv.second.path, kv.second.index, kv.second.baseGame });
	}
	return out;
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
