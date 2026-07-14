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
#include "sam_js_runtime.hpp"  // JavaScript + TypeScript scripting — game build only
#include "files.hpp"           // outputdir — TS cache / compiler location
#endif
#include "sam_logger.hpp"

#include <string>
#include <fstream>
#ifndef EDITOR
#include <filesystem>
#include <system_error>
#endif

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

	// Fresh sandboxed JavaScript/TypeScript runtime for this load cycle too.
	SAMJs::shutdown();
	SAMJs::init();

	// TypeScript transpile cache + the vendored compiler live next to the game
	// output dir (where sam_log.txt / sam_patch_overlay already are), NEVER in the
	// read-only mod folders. Cache = <outputdir>/sam_ts_cache, compiler =
	// <outputdir>/typescript.js (shipped alongside barony.exe).
	auto samJoinOut = [](const std::string& sub) -> std::string {
		std::string d = outputdir;
		if ( d.empty() ) { return sub; }
		const char b = d.back();
		return (b == '/' || b == '\\') ? (d + sub) : (d + "/" + sub);
	};
	const std::string tsCacheDir = samJoinOut("sam_ts_cache");
	const std::string tsCompilerPath = samJoinOut("typescript.js");
	std::error_code samTsEc;
	std::filesystem::create_directories(tsCacheDir, samTsEc);

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
		// S.A.M scripting: auto-load behavior scripts sitting next to a class JSON.
		// For "classes/assassin.json" we look for classes/assassin.{ts,js,lua} in the
		// same mod folder and load ALL that exist (detection order ts -> js -> lua;
		// each loaded script receives every event). A .ts is transpiled to JS once
		// (cached), then run through the same sandboxed QuickJS as a .js script.
		for ( const std::string& classRel : m.classes )
		{
			// Shared base name: strip the ".json" extension.
			std::string base = classRel;
			const std::string::size_type ext = base.rfind(".json");
			if ( ext != std::string::npos ) { base = base.substr(0, ext); }

			// Readable "<namespace>:<basename>" id for the log lines.
			std::string idBase = base;
			const std::string::size_type idSlash = idBase.find_last_of("/\\");
			if ( idSlash != std::string::npos ) { idBase = idBase.substr(idSlash + 1); }
			const std::string classId = m.ns + ":" + idBase;

			auto samExists = [](const std::string& p) { std::ifstream f(p.c_str()); return f.good(); };
			auto samFileName = [](const std::string& p) -> std::string {
				const std::string::size_type s = p.find_last_of("/\\");
				return (s != std::string::npos) ? p.substr(s + 1) : p;
			};

			// TypeScript (transpiled to JS, cached under <outputdir>/sam_ts_cache).
			const std::string tsPath = m.modPath + "/" + base + ".ts";
			if ( samExists(tsPath) && SAMJs::loadScriptTS(tsPath, tsCacheDir, tsCompilerPath, m.ns) )
			{
				SAM_INFO("JS", "Loaded script: " + samFileName(tsPath) + " (TypeScript) for [" + classId + "]");
			}
			// JavaScript.
			const std::string jsPath = m.modPath + "/" + base + ".js";
			if ( samExists(jsPath) && SAMJs::loadScriptJS(jsPath, m.ns) )
			{
				SAM_INFO("JS", "Loaded script: " + samFileName(jsPath) + " (JavaScript) for [" + classId + "]");
			}
			// Lua.
			const std::string luaPath = m.modPath + "/" + base + ".lua";
			if ( samExists(luaPath) && SAMLua::loadScript(luaPath, m.ns) )
			{
				SAM_INFO("LUA", "Loaded script: " + samFileName(luaPath) + " (Lua) for [" + classId + "]");
			}
		}
#endif
	}

#ifndef EDITOR
	// All scripts loaded — free the ~9MB TypeScript compiler + its runtime so it
	// is not resident during gameplay (lazily recreated if a later load needs it).
	SAMJs::releaseTranspiler();
#endif

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
	SAMLua::shutdown();   // drop the Lua behavior VM
	SAMJs::shutdown();    // drop the JS/TS behavior VM (+ any transpile runtime)
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
