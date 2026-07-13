/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_loader.cpp
	Desc: implementation of the S.A.M core loader. Drives the workshop scan, then
	      the class / item / monster loaders and the data patcher, and prints a
	      per-mod + final summary.

-------------------------------------------------------------------------------*/

#include "sam_loader.hpp"
#include "sam_workshop.hpp"
#include "sam_classes.hpp"
#include "sam_items.hpp"
#ifndef EDITOR
#include "sam_sync.hpp"    // multiplayer sync — game build only (not in EDITOR_SOURCES)
#include "sam_patcher.hpp" // layered data patches — game build only (needs PhysFS + outputdir)
#include "sam_monsters.hpp"// custom monster overlay — game build only (needs PhysFS + outputdir)
#include "sam_backup.hpp"  // daily save backup — game build only (needs outputdir + fs)
#include "sam_lua_runtime.hpp" // Lua behavior scripting — game build only
#endif
#include "sam_logger.hpp"

#include <string>
#include <fstream>

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

	// Fresh sandboxed Lua VM for this load cycle — drops any behavior scripts
	// registered on a previous Play, mirroring the class/item registry rebuild.
	SAMLua::shutdown();
	SAMLua::init();

	// Apply layered data patches now — after scanning manifests, before we
	// register classes/items, and (crucially) before Barony's initGameDatafiles
	// re-reads the data files. The patcher writes merged copies to a private
	// prepend-mounted overlay, so that re-read transparently picks them up.
	SAMPatcher::applyAll(mods);

	// Write custom-monster variant files + a merged monstercurve.json into a
	// private prepend-mounted overlay. Barony reads these lazily at map
	// generation (long after this hook), so the mount alone suffices.
	SAMMonsters::applyAll(mods);
#endif

	int totalClasses = 0;
	int totalItems = 0;
	int totalMonsters = 0;
	int totalPlugins = 0;
	for ( const auto& m : mods )
	{
		totalClasses += static_cast<int>(m.classes.size());
		totalItems += static_cast<int>(m.items.size());
		totalMonsters += static_cast<int>(m.monsters.size());
		totalPlugins += static_cast<int>(m.plugins.size());

		SAM_INFO("WORKSHOP", "Found mod: " + m.name + " [" + m.ns + "] v" + m.version
			+ " (" + std::to_string(m.classes.size()) + " classes, "
			+ std::to_string(m.items.size()) + " items, "
			+ std::to_string(m.monsters.size()) + " monsters, "
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

#ifndef EDITOR
		// S.A.M Lua: auto-load a behavior script sitting next to a class JSON.
		// "classes/assassin.json" -> "classes/assassin.lua" in the same mod folder.
		for ( const std::string& classRel : m.classes )
		{
			std::string luaRel = classRel;
			const std::string::size_type ext = luaRel.rfind(".json");
			if ( ext != std::string::npos ) { luaRel = luaRel.substr(0, ext) + ".lua"; }
			else { luaRel += ".lua"; }

			const std::string luaPath = m.modPath + "/" + luaRel;
			std::ifstream probe(luaPath.c_str());
			if ( !probe.good() ) { continue; } // no sibling .lua — perfectly fine
			probe.close();

			if ( SAMLua::loadScript(luaPath) )
			{
				// Build a readable "<namespace>:<basename>" id + bare filename for the log.
				std::string base = classRel;
				std::string::size_type slash = base.find_last_of("/\\");
				if ( slash != std::string::npos ) { base = base.substr(slash + 1); }
				const std::string::size_type bdot = base.rfind('.');
				if ( bdot != std::string::npos ) { base = base.substr(0, bdot); }

				std::string luaFile = luaRel;
				slash = luaFile.find_last_of("/\\");
				if ( slash != std::string::npos ) { luaFile = luaFile.substr(slash + 1); }

				SAM_INFO("LUA", "Loaded script: " + luaFile + " for [" + m.ns + ":" + base + "]");
			}
		}
#endif
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
			+ std::to_string(SAMPatcher::filesPatched()) + " file(s), "
			+ std::to_string(SAMMonsters::count()) + " monster(s) registered ("
			+ std::to_string(totalMonsters) + " declared) across "
			+ std::to_string(SAMMonsters::curveLevels()) + " spawn level(s), ";
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
	SAM_INFO("CORE", "S.A.M unloading — clearing mod + class + item + sync registries and patch/monster overlays.");
	SAMWorkshop::clear();
	SAMClasses::clear();
	SAMItems::clear();
#ifndef EDITOR
	SAMSync::clear();
	SAMPatcher::clear();  // unmount + wipe the generated patch overlay
	SAMMonsters::clear(); // unmount + wipe the generated monster overlay
#endif
	loaded = false;
}

bool SAMLoader::isLoaded()
{
	return loaded;
}

bool SAMLoader::isModLoaded(const std::string& namespaceId)
{
	for ( const SAMModManifest& m : SAMWorkshop::manifests() )
	{
		if ( m.ns == namespaceId ) { return true; }
	}
	return false;
}

const SAMModManifest* SAMLoader::getManifest(const std::string& namespaceId)
{
	for ( const SAMModManifest& m : SAMWorkshop::manifests() )
	{
		if ( m.ns == namespaceId ) { return &m; }
	}
	return nullptr;
}
