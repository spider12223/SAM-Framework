/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_loader.cpp
	Desc: implementation of the S.A.M core loader (scanner + summary skeleton).
	      Class and item registration are intentionally NOT implemented here yet.

-------------------------------------------------------------------------------*/

#include "sam_loader.hpp"
#include "sam_workshop.hpp"
#include "sam_classes.hpp"
#include "sam_items.hpp"
#ifndef EDITOR
#include "sam_sync.hpp"    // multiplayer sync — game build only (not in EDITOR_SOURCES)
#include "sam_patcher.hpp" // layered data patches — game build only (needs PhysFS + outputdir)
#include "sam_backup.hpp"  // daily save backup — game build only (needs outputdir + fs)
#endif
#include "sam_logger.hpp"

#include <string>

bool SAMLoader::loaded = false;

void SAMLoader::load(const std::vector<std::pair<std::string, std::string>>& mountedPaths,
	const std::string& baronyVersion)
{
	SAM_INFO("CORE", "S.A.M initializing..." + (baronyVersion.empty() ? std::string() : (" (Barony " + baronyVersion + ")")));
	SAM_INFO("CORE", "Scanning " + std::to_string(mountedPaths.size()) + " mounted mod path(s) for mod.json...");

	const std::vector<SAMModManifest> mods = SAMWorkshop::scan(mountedPaths, baronyVersion);

	// Fully rebuild the class + item registries every load (loadMods fires on
	// every Play, so appending would double-register).
	SAMClasses::clear();
	SAMItems::clear();
#ifndef EDITOR
	SAMSync::clear(); // drop any stale fingerprint state from a previous lobby

	// Apply layered data patches now — after scanning manifests, before we
	// register classes/items, and (crucially) before Barony's initGameDatafiles
	// re-reads the data files. The patcher writes merged copies to a private
	// prepend-mounted overlay, so that re-read transparently picks them up.
	SAMPatcher::applyAll(mods);
#endif

	int totalClasses = 0;
	int totalItems = 0;
	int totalPlugins = 0;
	for ( const auto& m : mods )
	{
		totalClasses += static_cast<int>(m.classes.size());
		totalItems += static_cast<int>(m.items.size());
		totalPlugins += static_cast<int>(m.plugins.size());

		SAM_INFO("WORKSHOP", "Found mod: " + m.name + " [" + m.ns + "] v" + m.version
			+ " (" + std::to_string(m.classes.size()) + " classes, "
			+ std::to_string(m.items.size()) + " items, "
			+ std::to_string(m.plugins.size()) + " plugins)");
		if ( !m.author.empty() )
		{
			SAM_DEBUG("WORKSHOP", "  author: " + m.author + " | path: " + m.modPath);
		}
		if ( !m.description.empty() )
		{
			SAM_DEBUG("WORKSHOP", "  " + m.description);
		}

		// Register the mod's classes and items into the runtime registries.
		SAMClasses::loadFromManifest(m);
		SAMItems::loadFromManifest(m);
	}

	loaded = true;

	if ( mods.empty() )
	{
		SAM_INFO("CORE", "S.A.M load complete. No S.A.M mods found (no mod.json in any mounted path).");
	}
	else
	{
		std::string summary = "S.A.M load complete. " + std::to_string(mods.size()) + " mod(s) loaded, "
			+ std::to_string(SAMClasses::count()) + " class(es) registered ("
			+ std::to_string(totalClasses) + " declared), "
			+ std::to_string(SAMItems::count()) + " item(s) registered ("
			+ std::to_string(totalItems) + " declared), ";
#ifndef EDITOR
		summary += std::to_string(SAMPatcher::operationsApplied()) + " patch op(s) across "
			+ std::to_string(SAMPatcher::filesPatched()) + " file(s), ";
#endif
		summary += std::to_string(totalPlugins) + " plugins declared.";
		SAM_INFO("CORE", summary);

#ifndef EDITOR
		// Mods are active — take a once-per-day snapshot of the player's saves
		// before the session, so a broken mod can't cost them progress. The mod
		// fingerprint tags the backup; failure is logged, never blocks loading.
		SAMBackup::backupIfNeeded(SAMSync::generateFingerprint());
#endif
	}
	// Classes and items are registered above. Plugin (.dll) loading is not
	// implemented yet — the plugin count is the declared count from each mod.json.
}

void SAMLoader::unload()
{
	SAM_INFO("CORE", "S.A.M unloading — clearing mod + class + item + sync registries and patch overlay.");
	SAMWorkshop::clear();
	SAMClasses::clear();
	SAMItems::clear();
#ifndef EDITOR
	SAMSync::clear();
	SAMPatcher::clear(); // unmount + wipe the generated patch overlay
#endif
	loaded = false;
}

bool SAMLoader::isLoaded()
{
	return loaded;
}
