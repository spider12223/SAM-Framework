/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_patcher.cpp
	Desc: implementation of layered / additive JSON patches.

	Operation engine works on a parsed nlohmann::json document; the overlay
	lifecycle (wipe → write → prepend-mount) makes Barony read the merged result.
	Game build only (GAME_SOURCES); the caller is #ifndef EDITOR-guarded.

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_patcher.hpp"
#include "sam_workshop.hpp"   // SAMModManifest
#include "sam_logger.hpp"
#include "nlohmann/json.hpp"

#include "main.hpp"           // physfs.h + core
#include "files.hpp"          // outputdir

#include <fstream>
#include <sstream>
#include <filesystem>
#include <map>
#include <vector>
#include <cmath>
#include <system_error>

using nlohmann::json;
namespace fs = std::filesystem;

static const char* MOD = "PATCHER";

// Our private overlay folder. Deliberately OUTSIDE ./mods/ so Barony's Local
// Mods browser never lists it as a fake mod (it scans ./mods/ subfolders). We
// write to it with std::ofstream (not PhysFS), so it isn't tied to the write
// dir, and PHYSFS_mount it with prepend priority so patched files still win.
static const char* OVERLAY_NAME = "sam_patch_overlay";
static bool s_mounted = false;
static int s_opsApplied = 0;
static int s_filesPatched = 0;

/*-------------------------------------------------------------------------------
	Local helpers
-------------------------------------------------------------------------------*/
static std::string joinPath(const std::string& dir, const std::string& file)
{
	if ( dir.empty() ) { return file; }
	const char back = dir.back();
	return (back == '/' || back == '\\') ? (dir + file) : (dir + "/" + file);
}

// Real filesystem dir of our overlay: <outputdir>/sam_patch_overlay (e.g.
// ./sam_patch_overlay on Windows). Outside ./mods/, so it never shows in the
// Local Mods browser, but still prepend-mounted so its files win over the base.
static std::string overlayRealDir()
{
	return joinPath(std::string(outputdir), OVERLAY_NAME);
}

static bool readWholeFile(const std::string& path, std::string& out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if ( !f.is_open() ) { return false; }
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

// "items.iron_sword.gold_value" -> json_pointer("/items/iron_sword/gold_value").
// RFC6901-escapes '~' and '/' inside each key (Barony keys don't use '.').
static json::json_pointer dotToPointer(const std::string& dotted)
{
	std::string ptr;
	std::string token;
	auto flush = [&]() {
		std::string esc;
		for ( char c : token )
		{
			if ( c == '~' ) { esc += "~0"; }
			else if ( c == '/' ) { esc += "~1"; }
			else { esc += c; }
		}
		ptr += "/";
		ptr += esc;
		token.clear();
	};
	for ( char c : dotted )
	{
		if ( c == '.' ) { flush(); }
		else { token += c; }
	}
	flush();
	return json::json_pointer(ptr);
}

// Apply one operation to `doc`. Returns true if it changed anything. Never
// throws — every access is guarded so a bad path is a logged warning, not a crash.
static bool applyOne(json& doc, const json& op, const std::string& target,
	const std::string& modNs, std::map<std::string, std::string>& lastWriter)
{
	auto opIt = op.find("op");
	auto pathIt = op.find("path");
	if ( opIt == op.end() || !opIt->is_string() || pathIt == op.end() || !pathIt->is_string() )
	{
		SAM_WARN(MOD, "Malformed operation (missing 'op'/'path') in a patch from [" + modNs
			+ "] targeting " + target + " — skipped.");
		return false;
	}
	const std::string opName = opIt->get<std::string>();
	const std::string path = pathIt->get<std::string>();
	if ( path.empty() )
	{
		SAM_WARN(MOD, "Empty 'path' in a " + opName + " from [" + modNs + "] on " + target + " — skipped.");
		return false;
	}
	const json::json_pointer ptr = dotToPointer(path);

	auto warnIfContested = [&]() {
		auto it = lastWriter.find(path);
		if ( it != lastWriter.end() && it->second != modNs )
		{
			SAM_WARN(MOD, "'" + path + "' in " + target + " edited by [" + modNs
				+ "] overrides an earlier edit by [" + it->second + "] (last-loaded wins).");
		}
		lastWriter[path] = modNs;
	};

	// Safety net: nlohmann json_pointer mutations can throw on a malformed path
	// (e.g. an intermediate token that lands on a pre-existing scalar or array).
	// We MUST degrade to a logged warning, never crash the game (stated contract).
	try
	{
	if ( opName == "edit_field" )
	{
		if ( op.find("value") == op.end() )
		{
			SAM_WARN(MOD, "edit_field on '" + path + "' (" + target + ") from [" + modNs + "] has no 'value' — skipped.");
			return false;
		}
		if ( !doc.contains(ptr) )
		{
			SAM_WARN(MOD, "edit_field: path '" + path + "' not found in " + target + " (from [" + modNs
				+ "]) — skipped. Check the field name/spelling.");
			return false;
		}
		warnIfContested();
		doc[ptr] = op["value"];
		return true;
	}
	else if ( opName == "multiply_field" )
	{
		auto vIt = op.find("value");
		if ( vIt == op.end() || !vIt->is_number() )
		{
			SAM_WARN(MOD, "multiply_field on '" + path + "' (" + target + ") from [" + modNs + "] needs a numeric 'value' — skipped.");
			return false;
		}
		if ( !doc.contains(ptr) || !doc[ptr].is_number() )
		{
			SAM_WARN(MOD, "multiply_field: '" + path + "' not found or not numeric in " + target
				+ " (from [" + modNs + "]) — skipped.");
			return false;
		}
		warnIfContested();
		const double factor = vIt->get<double>();
		if ( doc[ptr].is_number_integer() )
		{
			doc[ptr] = static_cast<long long>(std::llround(doc[ptr].get<double>() * factor));
		}
		else
		{
			doc[ptr] = doc[ptr].get<double>() * factor;
		}
		return true;
	}
	else if ( opName == "remove_field" )
	{
		if ( !doc.contains(ptr) )
		{
			SAM_WARN(MOD, "remove_field: path '" + path + "' not found in " + target + " (from [" + modNs + "]) — skipped.");
			return false;
		}
		const json::json_pointer parent = ptr.parent_pointer();
		const std::string key = ptr.back();
		if ( doc.contains(parent) && doc[parent].is_object() )
		{
			doc[parent].erase(key);
			return true;
		}
		SAM_WARN(MOD, "remove_field: parent of '" + path + "' in " + target + " is not an object — skipped.");
		return false;
	}
	else if ( opName == "add_entry" )
	{
		if ( op.find("value") == op.end() )
		{
			SAM_WARN(MOD, "add_entry on '" + path + "' (" + target + ") from [" + modNs + "] has no 'value' — skipped.");
			return false;
		}
		// If the parent path already exists but is NOT an object (a scalar/array),
		// bail with a clean message. (A scalar higher up the chain is still caught
		// by the try/catch below.)
		const json::json_pointer parent = ptr.parent_pointer();
		if ( doc.contains(parent) && !doc[parent].is_object() )
		{
			SAM_WARN(MOD, "add_entry: parent of '" + path + "' in " + target
				+ " is not an object (from [" + modNs + "]) — skipped.");
			return false;
		}
		warnIfContested();
		doc[ptr] = op["value"]; // creates missing intermediate objects / new key
		return true;
	}

	SAM_WARN(MOD, "Unknown patch op '" + opName + "' from [" + modNs + "] on " + target + " — skipped.");
	return false;
	}
	catch ( const json::exception& e )
	{
		SAM_WARN(MOD, "Operation '" + opName + "' on path '" + path + "' (" + target + ") from ["
			+ modNs + "] failed (" + e.what() + ") — skipped. Check the path against the file's structure.");
		return false;
	}
}

/*-------------------------------------------------------------------------------
	SAMPatcher
-------------------------------------------------------------------------------*/
void SAMPatcher::clear()
{
	if ( s_mounted )
	{
		PHYSFS_unmount(overlayRealDir().c_str());
		s_mounted = false;
	}
	std::error_code ec;
	fs::remove_all(fs::path(overlayRealDir()), ec); // wipe stale overlay; ignore errors
	s_opsApplied = 0;
	s_filesPatched = 0;
}

void SAMPatcher::applyAll(const std::vector<SAMModManifest>& mods)
{
	// Always start from a clean slate — never let a previous overlay linger or
	// compound (we must read the TRUE current base, not our own last output).
	clear();

	// Collect operations per target file, in mod load order.
	struct Item { std::string modNs; json op; };
	std::map<std::string, std::vector<Item>> byTarget;

	for ( const SAMModManifest& m : mods )
	{
		for ( const std::string& rel : m.patches )
		{
			const std::string path = joinPath(m.modPath, rel);
			std::string text;
			if ( !readWholeFile(path, text) )
			{
				SAM_ERROR(MOD, "Patch file not found: " + path + " (declared by [" + m.ns + "]) — skipped.");
				continue;
			}
			json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
			if ( j.is_discarded() || !j.is_object() )
			{
				SAM_ERROR(MOD, "Invalid patch JSON (not an object): " + path + " (from [" + m.ns + "]).");
				continue;
			}
			auto tIt = j.find("target");
			auto oIt = j.find("operations");
			if ( tIt == j.end() || !tIt->is_string() || oIt == j.end() || !oIt->is_array() )
			{
				SAM_ERROR(MOD, "Patch " + path + " (from [" + m.ns + "]) needs a string 'target' and an array 'operations' — skipped.");
				continue;
			}
			const std::string target = tIt->get<std::string>();
			for ( const json& op : *oIt )
			{
				if ( op.is_object() )
				{
					byTarget[target].push_back(Item{ m.ns, op });
				}
			}
		}
	}

	if ( byTarget.empty() )
	{
		return; // no patches declared
	}

	for ( const auto& kv : byTarget )
	{
		const std::string& target = kv.first;
		const std::vector<Item>& ops = kv.second;

		// Read the CURRENTLY-RESOLVED target (base data, or a mod's full replacement
		// — patches stack on top of whatever is mounted). Overlay is unmounted (via
		// clear() above), so getRealDir returns the true underlying file.
		const char* realDir = PHYSFS_getRealDir(target.c_str());
		if ( !realDir )
		{
			SAM_ERROR(MOD, "Patch target '" + target + "' does not exist (no such file mounted) — "
				+ std::to_string(ops.size()) + " operation(s) skipped.");
			continue;
		}
		std::string baseText;
		if ( !readWholeFile(joinPath(realDir, target), baseText) )
		{
			SAM_ERROR(MOD, "Could not read patch target '" + target + "' from " + realDir + " — skipped.");
			continue;
		}
		json doc = json::parse(baseText, nullptr, /*allow_exceptions=*/false);
		if ( doc.is_discarded() )
		{
			SAM_ERROR(MOD, "Patch target '" + target + "' is not valid JSON — skipped.");
			continue;
		}

		int appliedHere = 0;
		std::map<std::string, std::string> lastWriter;   // path -> mod ns (contest warnings)
		std::map<std::string, int> perMod;               // mod ns -> ops applied (for the log line)
		for ( const Item& it : ops )
		{
			if ( applyOne(doc, it.op, target, it.modNs, lastWriter) )
			{
				++appliedHere;
				++perMod[it.modNs];
			}
		}

		if ( appliedHere == 0 )
		{
			continue; // nothing landed; don't emit an overlay file
		}

		// Serialize compact (strips the base file's whitespace, so it is smaller
		// than the pretty-printed original) so we stay under Barony's fixed read
		// buffers — items/items.json is read into a ~599999-byte buffer and would
		// be TRUNCATED (breaking every entry, not just added ones) if exceeded.
		// If the merge is still too big, skip the overlay so the base loads intact.
		const std::string merged = doc.dump();
		static const size_t SAFE_MAX = 590000;
		if ( merged.size() >= SAFE_MAX )
		{
			SAM_ERROR(MOD, "Merged '" + target + "' is " + std::to_string(merged.size())
				+ " bytes — too large for Barony's read buffer; skipping this overlay so the base file loads intact. Reduce add_entry patches.");
			continue;
		}
		const std::string outPath = joinPath(overlayRealDir(), target);
		std::error_code ec;
		fs::create_directories(fs::path(outPath).parent_path(), ec);
		std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
		if ( !out.is_open() )
		{
			SAM_ERROR(MOD, "Could not write overlay for '" + target + "' at " + outPath + " — patches for this file skipped.");
			continue;
		}
		out << merged;
		out.close();

		++s_filesPatched;
		s_opsApplied += appliedHere;
		for ( const auto& pm : perMod )
		{
			SAM_INFO(MOD, "Applied " + std::to_string(pm.second) + " operation(s) from ["
				+ pm.first + "] to " + target);
		}
	}

	// Prepend-mount the overlay so Barony's next data-file read (initGameDatafiles)
	// resolves these files to the merged versions instead of the base.
	if ( s_filesPatched > 0 )
	{
		if ( PHYSFS_mount(overlayRealDir().c_str(), NULL, 0) )
		{
			s_mounted = true;
			SAM_INFO(MOD, "Mounted patch overlay (" + std::to_string(s_opsApplied) + " op(s) across "
				+ std::to_string(s_filesPatched) + " file(s)).");
		}
		else
		{
			SAM_ERROR(MOD, std::string("Failed to mount patch overlay: ")
				+ PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()) + " — patches will NOT take effect this load.");
		}
	}
}

int SAMPatcher::operationsApplied() { return s_opsApplied; }
int SAMPatcher::filesPatched() { return s_filesPatched; }
