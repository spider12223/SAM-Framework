/*-------------------------------------------------------------------------------
	S.A.M Framework — Custom sound-effect registry.
	See sam_sounds.hpp for the design + the IRON no-op rule.
-------------------------------------------------------------------------------*/

#include "sam_sounds.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include <fstream>
#include <sstream>
#include <vector>
#include <map>

// Engine build: pull in the FMOD sound globals. In the standalone/editor build these
// headers are absent, so appendSounds() compiles to a no-op stub.
#if defined(__has_include) && __has_include("main.hpp")
#	define SAM_SOUNDS_HAVE_BARONY 1
#	include "main.hpp"                  // Config (USE_FMOD), umbrella decls
#	include "engine/audio/sound.hpp"    // FMOD::Sound** sounds, Uint32 numsounds, fmod_system
#endif

using nlohmann::json;
static const char* MOD = "SOUNDS";

namespace
{
	std::string joinPath(const std::string& dir, const std::string& file)
	{
		if ( dir.empty() ) { return file; }
		if ( file.empty() ) { return dir; }
		const char last = dir[dir.size() - 1];
		if ( last == '/' || last == '\\' ) { return dir + file; }
		return dir + "/" + file;
	}
	bool readWholeFile(const std::string& path, std::string& out)
	{
		std::ifstream f(path.c_str(), std::ios::binary);
		if ( !f.good() ) { return false; }
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}

	struct StagedSound
	{
		std::string id;       // "ns:sound"
		std::string absPath;  // mod-relative path to the .ogg/.wav (cwd-resolvable)
		bool loop = false;
	};

	// Registry — EMPTY in vanilla (the whole no-op guarantee).
	std::vector<StagedSound> s_staged;        // pending, filled by loadFromManifest
	std::map<std::string, int> s_index;        // "ns:sound" -> engine index (after append)
#ifdef SAM_SOUNDS_HAVE_BARONY
	Uint32 s_baseNumsounds = 0;                // the vanilla numsounds, captured once
	bool s_haveBase = false;
#endif
}

void SAMSounds::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const std::string& relPath : manifest.sounds )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "Sound file not found: " + path + " (declared by [" + manifest.ns + "])");
			continue;
		}
		const std::string fileLabel = SAMErrors::displayFile(manifest.ns, relPath);
		json j;
		try { j = json::parse(text); }
		catch ( const json::parse_error& e )
		{
			SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "sound not loaded.");
			continue;
		}
		if ( !j.is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
				"a JSON object: { ... }", "wrap the file contents in { }", "sound not loaded.");
			continue;
		}

		auto getStr = [&](const char* k) -> std::string {
			auto it = j.find(k);
			return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
		};
		auto getBool = [&](const char* k, bool dv) -> bool {
			auto it = j.find(k);
			return (it != j.end() && it->is_boolean()) ? it->get<bool>() : dv;
		};

		const std::string id = getStr("id");
		if ( id.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/id", "", "missing (required)",
				"an id in \"namespace:sound\" form, e.g. \"" + manifest.ns + ":boom\"",
				"add an \"id\" field", "sound not loaded.");
			continue;
		}
		bool dup = (s_index.find(id) != s_index.end());
		for ( const StagedSound& s : s_staged ) { if ( s.id == id ) { dup = true; break; } }
		if ( dup )
		{
			SAM_WARN(MOD, "Duplicate sound id '" + id + "' — keeping the first, skipping this one.");
			continue;
		}

		const std::string file = getStr("file");
		if ( file.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/file", "", "missing (required)",
				"a mod-relative path to a .ogg/.wav, e.g. \"sounds/boom.ogg\"",
				"add a \"file\" field", "sound not loaded.");
			continue;
		}
		if ( SAMErrors::relPathEscapes(file) )
		{
			SAM_WARN(MOD, "Sound [" + id + "] file path '" + file + "' escapes the mod folder — ignoring it.");
			continue;
		}

		StagedSound s;
		s.id = id;
		s.absPath = joinPath(manifest.modPath, file);
		s.loop = getBool("loop", false);
		s_staged.push_back(s);
		SAM_INFO(MOD, "Staged sound [" + id + "] <- " + s.absPath + (s.loop ? " (loop)" : ""));
	}
}

void SAMSounds::clear()
{
	s_staged.clear();
	s_index.clear();
}

int SAMSounds::soundIndexForId(const std::string& id)
{
	auto it = s_index.find(id);
	return (it != s_index.end()) ? it->second : -1;
}

int SAMSounds::count() { return static_cast<int>(s_index.size()); }
bool SAMSounds::any() { return !s_index.empty(); }

std::string SAMSounds::idAtIndex(int index)
{
	if ( index < 0 || index >= (int)s_index.size() ) { return std::string(); }
	auto it = s_index.begin();
	std::advance(it, index);
	return it->first;
}

int SAMSounds::engineIndexAtIndex(int index)
{
	if ( index < 0 || index >= (int)s_index.size() ) { return -1; }
	auto it = s_index.begin();
	std::advance(it, index);
	return it->second;
}

int SAMSounds::appendSounds()
{
#ifndef SAM_SOUNDS_HAVE_BARONY
	return 0;
#else
	// Capture the vanilla base the first time we ever append, so a mod reload
	// (which re-stages) resets the engine table to base instead of accumulating.
	if ( !s_haveBase ) { s_baseNumsounds = numsounds; s_haveBase = true; }

	// Drop any previously-appended sounds back to the vanilla base. Safe because a
	// mod reload happens at the menu, not mid-play.
	if ( sounds && numsounds > s_baseNumsounds )
	{
		for ( Uint32 i = s_baseNumsounds; i < numsounds; ++i )
		{
			if ( sounds[i] ) { sounds[i]->release(); sounds[i] = nullptr; }
		}
		numsounds = s_baseNumsounds;
	}
	s_index.clear();

	if ( s_staged.empty() ) { return 0; }
	if ( !fmod_system || !sounds || numsounds == 0 )
	{
		SAM_ERROR(MOD, "Sound engine not initialised — cannot append " + std::to_string(s_staged.size()) + " custom sound(s).");
		return 0;
	}

	const Uint32 oldCount = numsounds;
	const int addCount = (int)s_staged.size();
	FMOD::Sound** grown = (FMOD::Sound**)realloc(sounds, sizeof(FMOD::Sound*) * (size_t)(oldCount + (Uint32)addCount));
	if ( !grown )
	{
		SAM_ERROR(MOD, "Out of memory growing the sound table to " + std::to_string(oldCount + addCount) + " — leaving it untouched.");
		return 0;
	}
	sounds = grown;

	for ( int i = 0; i < addCount; ++i )
	{
		const StagedSound& s = s_staged[i];
		const Uint32 idx = oldCount + (Uint32)i;
		FMOD_MODE flags = FMOD_DEFAULT | FMOD_3D | FMOD_LOWMEM;
		if ( s.loop ) { flags |= FMOD_LOOP_NORMAL; }
		FMOD::Sound* snd = nullptr;
		FMOD_RESULT r = fmod_system->createSound(s.absPath.c_str(), flags, nullptr, &snd);
		sounds[idx] = snd; // may be null on failure — play paths guard sounds[snd]==nullptr
		if ( r != FMOD_OK || !snd )
		{
			SAM_ERROR(MOD, "Failed to load sound '" + s.absPath + "' for [" + s.id
				+ "] — the id resolves but plays silent. Is the file really in the mod folder?");
		}
		else
		{
			SAM_INFO(MOD, "Registered sound [" + s.id + "] -> index " + std::to_string(idx) + (s.loop ? " (loop)" : ""));
		}
		s_index[s.id] = (int)idx; // occupy the slot regardless, so ids never shift
	}

	numsounds = oldCount + (Uint32)addCount; // publish LAST — appended indices now playable
	s_staged.clear();
	SAM_INFO(MOD, "Custom sounds appended: " + std::to_string(addCount) + "; numsounds "
		+ std::to_string(oldCount) + " -> " + std::to_string(numsounds) + ".");
	return addCount;
#endif
}
