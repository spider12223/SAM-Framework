/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_loader.hpp
	Desc: the core S.A.M entry point. Barony calls SAMLoader::load() from inside
	      Mods::loadMods() (after mod paths are mounted) and SAMLoader::unload()
	      from Mods::unloadMods(). load() drives the workshop scan and, later,
	      the class/item loaders; for now it is the scanner + summary skeleton.

	Takes the mounted-paths list as a parameter (pass Mods::mountedFilepaths) so
	this framework stays decoupled from Barony's internals.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>
#include <utility>

class SAMLoader
{
public:
	// Scan + register all S.A.M mods from the mounted mod paths, logging each
	// mod found and a summary. Fully rebuilds state on every call (Barony calls
	// this every time the player starts a modded game).
	static void load(const std::vector<std::pair<std::string, std::string>>& mountedPaths,
		const std::string& baronyVersion = "");

	// Tear down all S.A.M state back to vanilla (called from Mods::unloadMods()).
	static void unload();

	// Global "is S.A.M active" flag other systems can query.
	static bool isLoaded();

private:
	static bool loaded;
};
