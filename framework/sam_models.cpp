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
#	include "draw.hpp"   // GL_CHECK_ERR + glDelete*: we free the buffers we asked for
#endif

namespace
{
	const char* MOD = "MODELS";

	// Where the VANILLA model table ends, captured the first time we ever append.
	//
	// This is what makes a mod model's index the same on two different machines. It is not
	// "nummodels right now", which depends on what this session has already loaded and
	// unloaded; it is the fixed size of the base game's table, which is the line count of
	// models/models.txt (init.cpp:717). Nothing in the mod system moves it: Barony's own mod
	// models REPLACE entries inside [1, nummodels) through physfsModelIndexUpdate, and
	// files.cpp:4038 warns if a model index ever lands outside that range.
	//
	// -1 until a mod actually ships a model, so a vanilla session never touches any of this.
	int s_baseIndex = -1;

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

	// ---- MagicaVoxel -> Barony slab -------------------------------------------------
	//
	// Barony's loader reads only the SLAB format and asserts on anything else, so a
	// MagicaVoxel export used to be refused with an explanation and nothing more. This
	// converts it in memory at load time instead. Nothing is written to disk and every
	// machine converts the same bytes to the same voxels, so it changes nothing about model
	// indices or multiplayer.
	//
	// MagicaVoxel is a RIFF-ish chunk format:
	//   "VOX " int32 version
	//   then chunks: char[4] id, int32 contentBytes, int32 childBytes, content, children
	// The chunks that matter are SIZE (three int32) and XYZI (int32 count, then count
	// records of x,y,z,colourIndex as bytes), plus an optional RGBA of 256 x (r,g,b,a).
	//
	// ORIENTATION, and this is the part that is easy to get wrong: MagicaVoxel is Z-UP, and
	// Barony's slab is Z-DOWN -- array index z=0 is the TOP of the model. Verified against a
	// shipped asset: models/decorations/Chair.vox is 10x8x20 with its seat plane at z=13, its
	// backrest at z=0..12 and its legs at z=14..19. So Z has to be flipped.
	//
	// Flipping Z ALONE would be a reflection and would mirror every model (a left boot would
	// arrive as a right one). Flipping a second axis makes the pair a 180-degree rotation
	// about X, which preserves handedness. The model ends up facing the opposite way in the
	// horizontal plane, which yaw_offset already exists to correct.
	bool samReadWholeFile(const std::string& physfsPath, std::vector<unsigned char>& out)
	{
		PHYSFS_File* fh = PHYSFS_openRead(physfsPath.c_str());
		if ( !fh ) { return false; }
		const PHYSFS_sint64 len = PHYSFS_fileLength(fh);
		if ( len <= 0 || len > (PHYSFS_sint64)(64 * 1024 * 1024) ) { PHYSFS_close(fh); return false; }
		out.resize((size_t)len);
		const PHYSFS_sint64 got = PHYSFS_readBytes(fh, out.data(), (PHYSFS_uint64)len);
		PHYSFS_close(fh);
		return got == len;
	}

	bool samIsMagicaVoxel(const std::string& physfsPath)
	{
		PHYSFS_File* fh = PHYSFS_openRead(physfsPath.c_str());
		if ( !fh ) { return false; }
		unsigned char magic[4] = { 0 };
		const PHYSFS_sint64 got = PHYSFS_readBytes(fh, magic, 4);
		PHYSFS_close(fh);
		return got == 4 && magic[0] == 'V' && magic[1] == 'O' && magic[2] == 'X' && magic[3] == ' ';
	}

	Sint32 samRd32(const std::vector<unsigned char>& b, size_t off)
	{
		return (Sint32)((Uint32)b[off] | ((Uint32)b[off + 1] << 8)
			| ((Uint32)b[off + 2] << 16) | ((Uint32)b[off + 3] << 24));
	}

	// Returns a fresh voxel_t on success (caller owns it), or nullptr with `why` filled.
	voxel_t* samConvertMagicaVoxel(const std::string& physfsPath, std::string& why)
	{
		why.clear();
		std::vector<unsigned char> b;
		if ( !samReadWholeFile(physfsPath, b) || b.size() < 8 )
		{
			why = "The file could not be read, or is too small to be a MagicaVoxel model.";
			return nullptr;
		}

		Sint32 sx = 0, sy = 0, sz = 0;
		bool haveSize = false, haveVoxels = false, haveRgba = false;
		std::vector<unsigned char> xyzi;      // raw voxel records, 4 bytes each
		unsigned char rgba[256][4];
		memset(rgba, 0, sizeof(rgba));
		int modelsSeen = 0;

		// Walk the chunk tree flatly: every chunk header is the same shape, and the ones we
		// care about are all direct children of MAIN.
		size_t p = 8;   // past "VOX " + version
		while ( p + 12 <= b.size() )
		{
			const char id[5] = { (char)b[p], (char)b[p + 1], (char)b[p + 2], (char)b[p + 3], 0 };
			const Sint32 contentBytes = samRd32(b, p + 4);
			const Sint32 childBytes   = samRd32(b, p + 8);
			if ( contentBytes < 0 || childBytes < 0 ) { break; }
			const size_t content = p + 12;
			if ( content + (size_t)contentBytes > b.size() ) { break; }

			if ( !strcmp(id, "SIZE") && contentBytes >= 12 )
			{
				++modelsSeen;
				if ( !haveSize )
				{
					sx = samRd32(b, content);
					sy = samRd32(b, content + 4);
					sz = samRd32(b, content + 8);
					haveSize = true;
				}
			}
			else if ( !strcmp(id, "XYZI") && contentBytes >= 4 )
			{
				// Only the FIRST model's voxels, and only once its SIZE has been seen. Without
				// the modelsSeen test a first XYZI with a zero or lying count would be skipped
				// and the SECOND model's voxels would be pasted into the first model's
				// dimensions -- a garbled model rather than an honest failure.
				if ( !haveVoxels && haveSize && modelsSeen == 1 )
				{
					const Sint32 n = samRd32(b, content);
					if ( n > 0 && (size_t)contentBytes >= 4 + (size_t)n * 4 )
					{
						xyzi.assign(b.begin() + (content + 4), b.begin() + (content + 4 + (size_t)n * 4));
						haveVoxels = true;
					}
				}
			}
			else if ( !strcmp(id, "RGBA") && contentBytes >= 256 * 4 )
			{
				memcpy(rgba, &b[content], 256 * 4);
				haveRgba = true;
			}

			// MAIN has no content of its own and everything else lives in its children, so
			// descend into children rather than skipping them.
			if ( !strcmp(id, "MAIN") ) { p = content; }
			else { p = content + (size_t)contentBytes; }
		}

		if ( !haveSize || !haveVoxels )
		{
			why = "This MagicaVoxel file has no model in it (no SIZE/XYZI chunk).";
			return nullptr;
		}
		// MagicaVoxel's own hard limit is 256 per axis, so anything larger is a corrupt or
		// hostile header rather than a real model -- and 512^3 would have committed a 128MB
		// allocation off one lying int.
		if ( sx <= 0 || sy <= 0 || sz <= 0 || sx > 256 || sy > 256 || sz > 256 )
		{
			why = "The model is " + std::to_string(sx) + "x" + std::to_string(sy) + "x"
				+ std::to_string(sz) + ", which is not a size Barony can use.";
			return nullptr;
		}
		if ( modelsSeen > 1 )
		{
			SAM_WARN(MOD, "'" + physfsPath + "' contains " + std::to_string(modelsSeen)
				+ " models; Barony has one model per file, so only the first was converted.");
		}
		if ( !haveRgba )
		{
			// MagicaVoxel omits RGBA when the model uses its built-in default palette. We do
			// not carry a copy of that table, and inventing colours would be worse than
			// saying so.
			why = "This MagicaVoxel file carries no palette (no RGBA chunk), which happens when"
				" the model uses the default palette untouched. Recolour any voxel in"
				" MagicaVoxel and save again so the palette is written out.";
			return nullptr;
		}

		voxel_t* model = (voxel_t*)malloc(sizeof(voxel_t));
		if ( !model ) { why = "Out of memory."; return nullptr; }
		model->sizex = sx; model->sizey = sy; model->sizez = sz;
		const size_t cells = (size_t)sx * (size_t)sy * (size_t)sz;
		model->data = (Uint8*)malloc(cells);
		if ( !model->data ) { free(model); why = "Out of memory."; return nullptr; }
		memset(model->data, 255, cells);   // 255 is AIR in Barony's slab

		// MagicaVoxel colour index i (1..255) is palette entry i-1; Barony keeps 255 for air
		// and uses 0..254, so the index maps straight down by one.
		for ( size_t i = 0; i + 3 < xyzi.size(); i += 4 )
		{
			const int vx = xyzi[i], vy = xyzi[i + 1], vz = xyzi[i + 2];
			const int ci = xyzi[i + 3];
			if ( ci <= 0 ) { continue; }
			if ( vx >= sx || vy >= sy || vz >= sz ) { continue; }
			// Same ordering files.cpp walks: index = z + y*sizez + x*sizey*sizez, with Z and Y
			// flipped for the orientation reason above.
			const size_t fz = (size_t)(sz - 1 - vz);
			const size_t fy = (size_t)(sy - 1 - vy);
			const size_t idx = fz + fy * (size_t)sz + (size_t)vx * (size_t)sy * (size_t)sz;
			model->data[idx] = (Uint8)(ci - 1);
		}

		// A slab file stores 6-bit channels and loadVoxel expands them with << 2 after
		// reading. We are building the voxel_t ourselves and never go through loadVoxel, so
		// store MagicaVoxel's 8-bit values as they are -- shifting down and back up here
		// would only throw away the low two bits of every colour. Alpha is dropped: the
		// slab format has none.
		for ( int c = 0; c < 256; ++c )
		{
			model->palette[c][0] = rgba[c][0];
			model->palette[c][1] = rgba[c][1];
			model->palette[c][2] = rgba[c][2];
		}
		return model;
	}

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

#ifdef SAM_MODELS_HAVE_BARONY
namespace
{
	// Give back everything S.A.M appended, so the next load can rebuild the range from a
	// known base instead of stacking on top of it.
	//
	// The geometry and GPU half mirrors what the engine does for its own model reloads
	// (mod_tools.cpp:11900-11917). The voxel itself is freed here as well, which that loop
	// does NOT do -- physfsModelIndexUpdate replaces vanilla voxels in place, whereas these
	// are ours: loadVoxel (or the MagicaVoxel converter) allocated them for us.
	void samReleaseAppendedRange()
	{
		if ( s_baseIndex < 0 || (int)nummodels <= s_baseIndex ) { return; }

		for ( int c = s_baseIndex; c < (int)nummodels; ++c )
		{
			if ( polymodels )
			{
					// numfaces goes with faces. faces==nullptr while numfaces>0 is the one
				// combination generateVBOs cannot survive: it allocates 9*numfaces floats and
				// then dereferences faces[i] with no null check (files.cpp:5273-5281).
				if ( polymodels[c].faces ) { free(polymodels[c].faces); polymodels[c].faces = nullptr; }
				polymodels[c].numfaces = 0;
				if ( polymodels[c].vao )
				{
					GL_CHECK_ERR(glDeleteVertexArrays(1, &polymodels[c].vao));
					polymodels[c].vao = 0;
				}
				if ( polymodels[c].positions )
				{
					GL_CHECK_ERR(glDeleteBuffers(1, &polymodels[c].positions));
					polymodels[c].positions = 0;
				}
				if ( polymodels[c].colors )
				{
					GL_CHECK_ERR(glDeleteBuffers(1, &polymodels[c].colors));
					polymodels[c].colors = 0;
				}
				if ( polymodels[c].normals )
				{
					GL_CHECK_ERR(glDeleteBuffers(1, &polymodels[c].normals));
					polymodels[c].normals = 0;
				}
			}
			if ( models && models[c] )
			{
				if ( models[c]->data ) { free(models[c]->data); }
				free(models[c]);
				models[c] = nullptr;
			}
		}

		// Published last, and only downwards to a base the renderer was never drawing past
		// before we grew it.
		//
		// There are TWO callers. The normal one is Mods::loadMods, behind a loading screen, at
		// the same point the engine already tears its own polymodels down. The other is
		// /sam_reload (consolecommand.cpp), a debug command documented for main-menu use --
		// that path was already growing these tables and calling generatePolyModels, so it is
		// no more exposed than it was, but it is not covered by the loading-screen argument.
		nummodels = (Uint32)s_baseIndex;
	}
}
#endif

int SAMModels::appendModels(const std::vector<Request>& requests)
{
#ifndef SAM_MODELS_HAVE_BARONY
	(void)requests;
	return 0;
#else
	if ( (int)nummodels <= 0 || !models )
	{
		SAM_ERROR(MOD, "Model table not initialised yet — refusing to append "
			+ std::to_string(requests.size()) + " model(s). This must run after the engine has loaded models.txt.");
		return 0;
	}

	// Pin the end of the vanilla table the first time through, then REBUILD our range on every
	// load rather than appending to whatever is already there. This is the whole point: an id's
	// index now depends only on which mods are loaded, because the request list below was built
	// from a topologically sorted mod set. It no longer depends on what this session loaded and
	// unloaded first, which two players cannot see and cannot match.
	if ( s_baseIndex < 0 ) { s_baseIndex = (int)nummodels; }
	samReleaseAppendedRange();
	s_index.clear();

	if ( requests.empty() ) { return 0; }

	const int oldCount = s_baseIndex;

	// Load every voxel FIRST, into a staging list. A .vox that fails to load must not
	// consume an index: the table is positional, so a hole would silently shift every
	// later model and mis-render other mods' content. Skip it instead.
	// A placeholder for a request whose FILE could not be loaded on this machine.
	//
	// Skipping such a request would compact the list, and then an id's index would depend on
	// how many earlier files happened to load HERE -- which is the same divergence this whole
	// rebuild exists to remove, just relocated from session history to machine state. So the
	// slot is reserved instead, exactly as the engine does for a missing model at init.cpp:755
	// (which allocates a fresh voxel rather than renumbering).
	//
	// 1x1x1 with the single cell set to 255, which is the empty value (files.cpp:4312), so it
	// builds zero faces and draws nothing. Owned like any other voxel, so the release frees it
	// without a special case.
	auto samMakeReservedVoxel = []() -> voxel_t*
	{
		voxel_t* v = (voxel_t*)malloc(sizeof(voxel_t));
		if ( !v ) { return nullptr; }
		v->sizex = 1; v->sizey = 1; v->sizez = 1;
		v->data = (Uint8*)malloc(1);
		if ( !v->data ) { free(v); return nullptr; }
		v->data[0] = 255;
		memset(v->palette, 0, sizeof(v->palette));
		return v;
	};

	struct Staged { std::string id; std::string path; voxel_t* vox; };
	std::vector<Staged> staged;
	staged.reserve(requests.size());
	std::map<std::string, std::string> seenInBatch;   // id -> path, for the clash warning below

	for ( const Request& r : requests )
	{
		if ( r.id.empty() || r.physfsPath.empty() ) { continue; }

		// `owner` is only for the message; it is empty for older call sites.
		const std::string who = r.owner.empty() ? ("[" + r.id + "]") : ("[" + r.owner + "]");

		// Duplicates are now checked WITHIN this batch rather than against the surviving map
		// from last load. The map is cleared above because the range is rebuilt, so a repeat
		// of the same id here is a genuine clash between two mods -- never the harmless
		// re-registration that happened when Barony called loadMods a second time.
		auto known = seenInBatch.find(r.id);
		if ( known != seenInBatch.end() )
		{
			if ( known->second == r.physfsPath ) { continue; }   // same id, same file: nothing to say
			SAM_WARN(MOD, "Two different models claim the id '" + r.id + "': already loaded from '"
				+ known->second + "', now also requested from '" + r.physfsPath
				+ "' by " + who + ". Keeping the first. Namespace your model folder"
				" (models/<yourmod>/...) so ids cannot collide.");
			continue;
		}

		// A MagicaVoxel export is converted in memory rather than refused. It is the format
		// every voxel tool produces and Barony reads none of it, so this used to be exactly
		// where a modder's first model died.
		voxel_t* samConverted = nullptr;
		if ( samIsMagicaVoxel(r.physfsPath) )
		{
			std::string convWhy;
			samConverted = samConvertMagicaVoxel(r.physfsPath, convWhy);
			if ( !samConverted )
			{
				SAM_ERROR(MOD, "Model '" + r.physfsPath + "' for " + who
					+ " is a MagicaVoxel file that could not be converted. " + convWhy
					+ " Its slot is reserved so the other models keep their numbers.");
				staged.push_back({ r.id, r.physfsPath, samMakeReservedVoxel() });
				continue;
			}
			SAM_INFO(MOD, "Converted MagicaVoxel model '" + r.physfsPath + "' ("
				+ std::to_string(samConverted->sizex) + "x" + std::to_string(samConverted->sizey)
				+ "x" + std::to_string(samConverted->sizez) + ") for " + who + ".");
		}

		// Refuse a file we cannot prove is a slab, BEFORE loadVoxel gets a chance to
		// segfault on it. See samValidateSlab.
		std::string why;
		if ( !samConverted && !samValidateSlab(r.physfsPath, why) )
		{
			SAM_ERROR(MOD, "Model '" + r.physfsPath + "' for " + who + " was not loaded. "
				+ why + " Its slot is reserved so the other models keep their numbers.");
			staged.push_back({ r.id, r.physfsPath, samMakeReservedVoxel() });
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
		// A converted MagicaVoxel model is already in hand; only a slab goes through loadVoxel.
		voxel_t* vox = samConverted ? samConverted : loadVoxel(path.data());
		if ( !vox )
		{
			// Validation passed, so this is something else -- out of memory, or a format
			// quirk we did not model. Say so honestly rather than guessing.
			SAM_ERROR(MOD, "Could not load model '" + r.physfsPath + "' for " + who
				+ " even though it looks like a valid slab .vox. Its slot is reserved so the"
				" other models keep their numbers.");
			staged.push_back({ r.id, r.physfsPath, samMakeReservedVoxel() });
			continue;
		}
		// Recorded only now that it really loaded. Claiming it earlier meant a failed request
		// poisoned the id, and a later good one for the same id was refused with a collision
		// warning describing a clash that never happened.
		seenInBatch[r.id] = r.physfsPath;
		staged.push_back({ r.id, r.physfsPath, vox });
	}

	// A reserved slot whose own tiny allocation failed would be a null in the table, and
	// generatePolyModels dereferences models[c]. Out of memory here is not survivable in a way
	// that preserves numbering, so say so rather than renumbering silently.
	for ( const Staged& s : staged )
	{
		if ( !s.vox )
		{
			SAM_ERROR(MOD, "Out of memory reserving a model slot; the remaining models would be"
				" renumbered, so none are registered this load.");
			for ( Staged& t : staged ) { if ( t.vox ) { if ( t.vox->data ) { free(t.vox->data); } free(t.vox); } }
			return 0;
		}
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
		// NOT "leaving the table untouched": the release above already freed the previous set,
		// so this returns with no custom models at all rather than with the old ones.
		SAM_ERROR(MOD, "Out of memory growing the model table to " + std::to_string(newCount)
			+ " — no custom models are registered this load.");
		for ( Staged& s : staged ) { if ( s.vox ) { if (s.vox->data) { free(s.vox->data); } free(s.vox); } }
		return 0;
	}
	models = grownModels;
	for ( int i = 0; i < addCount; ++i ) { models[oldCount + i] = staged[i].vox; }
	// From here the table owns the staged voxels. The polymodel bail-out below frees them, so
	// it must also clear these slots or it leaves dangling pointers above nummodels for the
	// next release to walk.

	// generatePolyModels only reallocs when asked for the FULL range (start==0 &&
	// end==nummodels); for a partial range it writes straight into the existing array.
	// So the polymodel table has to be grown here, before we call it, or it writes off
	// the end.
	polymodel_t* grownPoly = (polymodel_t*)realloc(polymodels, sizeof(polymodel_t) * (size_t)newCount);
	if ( !grownPoly )
	{
		// Same as above: the previous set is already gone, so this is not a no-op.
		SAM_ERROR(MOD, "Out of memory growing the polymodel table to " + std::to_string(newCount)
			+ " — no custom models are registered this load.");
		// models[] is already grown and already points at the staged voxels, so clear those
		// slots as well as freeing them -- nummodels is still oldCount so nothing reads them
		// now, but the next release walks [base, nummodels) and must not find stale pointers.
		for ( Staged& s : staged ) { if ( s.vox ) { if (s.vox->data) { free(s.vox->data); } free(s.vox); } }
		for ( int i = 0; i < addCount; ++i ) { models[oldCount + i] = nullptr; }
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
	// Only the id map is dropped, and the engine's tables are deliberately left alone:
	// shrinking them here would invalidate indices the renderer may still be holding for the
	// rest of this frame. appendModels reclaims the range itself, at the one point in the
	// frame where that is safe, so nothing accumulates across mods-off/mods-on cycles.
	s_index.clear();
}
