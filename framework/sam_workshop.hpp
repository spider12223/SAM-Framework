/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_workshop.hpp
	Desc: reads S.A.M mod manifests (mod.json) from the mod folders that Barony
	      has already mounted, validates them, and returns them in a
	      dependency-safe load order.

	S.A.M does NOT scan Steam Workshop folders itself. Barony's own mod system
	(Mods::mountedFilepaths in mod_tools.hpp) already resolves and mounts every
	subscribed Workshop item and every local mod. SAMWorkshop just consumes that
	list — one <path, displayName> pair per mounted mod folder.

	Note: nlohmann/json is intentionally kept OUT of this header so translation
	units that only need the manifest data (sam_loader.cpp, the mod_tools.cpp
	hook) don't have to pull in the ~25k-line single header.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>
#include <utility>

// One parsed mod.json. Field names mirror mod.schema.json (except `ns`, since
// `namespace` is a C++ keyword).
struct SAMModManifest
{
	std::string ns;                   // "namespace" — unique lowercase mod id prefix
	std::string name;                 // human-readable name
	std::string author;               // author/handle
	std::string version;              // MAJOR.MINOR.PATCH
	std::string frameworkMinVersion;  // minimum S.A.M version required
	std::string frameworkMaxVersion;  // maximum S.A.M version targeted (optional)
	std::string baronyMinVersion;     // minimum Barony version targeted (optional)
	std::string baronyMaxVersion;     // maximum Barony version targeted (optional)
	std::string incompatibleWithBaronyVersion; // exact Barony version to hard-block on (optional)
	std::string description;          // short description

	std::vector<std::string> dependencies; // other namespaces ("ns" or "ns@x.y.z")
	std::vector<std::string> classes;      // relative paths to class JSON files
	std::vector<std::string> items;        // relative paths to item JSON files
	std::vector<std::string> patches;      // relative paths to patch JSON files
	std::vector<std::string> monsters;     // relative paths to monster JSON files
	std::vector<std::string> plugins;      // relative paths to plugin .dll files

	std::string modPath;      // absolute directory this mod was loaded from
	std::string displayName;  // Workshop/local display name (from mountedFilepaths)
};

class SAMWorkshop
{
public:
	// Rebuilds the manifest registry from the given mounted mod paths (pass
	// Mods::mountedFilepaths). Reads each <path>/mod.json, skips folders without
	// one (they are plain asset-overlay mods, not S.A.M mods), validates
	// framework_min_version, and returns the manifests in dependency-safe order.
	//
	// This FULLY CLEARS and rebuilds the registry on every call — it never
	// appends — because Barony calls Mods::loadMods() every time the player
	// starts a modded game, not once per session.
	// `baronyVersion` is the running Barony version string (e.g. "v5.0.2", passed
	// from the game hook); used to warn on barony_min/max_version mismatches. Pass
	// "" (the default) when the game version is unknown/irrelevant (e.g. tests).
	static std::vector<SAMModManifest> scan(
		const std::vector<std::pair<std::string, std::string>>& mountedPaths,
		const std::string& baronyVersion = "");

	// Wipes the manifest registry (used by the unload hook).
	static void clear();

	// The manifests from the most recent scan().
	static const std::vector<SAMModManifest>& manifests();

private:
	static std::vector<SAMModManifest> registry;
};
