/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_loader.hpp
	Desc: the core S.A.M entry point. Barony calls SAMLoader::load() from inside
	      Mods::loadMods() (after mod paths are mounted) and SAMLoader::unload()
	      from Mods::unloadMods(). load() drives the workshop scan, then the
	      class/item/monster loaders and the data patcher.

	Takes the mounted-paths list as a parameter (pass Mods::mountedFilepaths) so
	this framework stays decoupled from Barony's internals.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>
#include <utility>

struct SAMModManifest; // from sam_workshop.hpp (full type only needed in the .cpp)

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

	// --- Cross-mod integration queries (for other mods' plugin DLLs, etc.) ---
	// True if a mod with the given namespace is currently loaded (post dependency
	// resolution). Lets an optional integration check "is mod X here?".
	static bool isModLoaded(const std::string& namespaceId);

	// The loaded manifest for a namespace, or nullptr. The pointer is valid until
	// the next load/unload (it points into the workshop registry).
	static const SAMModManifest* getManifest(const std::string& namespaceId);

private:
	static bool loaded;
};
