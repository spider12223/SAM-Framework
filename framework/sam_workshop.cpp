/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_workshop.cpp
	Desc: implementation of the S.A.M mod-manifest reader.

-------------------------------------------------------------------------------*/

#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include <cstdio>
#include <cstdlib>  // strtol
#include <fstream>
#include <sstream>
#include <set>

using nlohmann::json;

static const char* MOD = "WORKSHOP";

std::vector<SAMModManifest> SAMWorkshop::registry;

/*-------------------------------------------------------------------------------
	Local helpers
-------------------------------------------------------------------------------*/

// Read an entire file into `out`. Returns false if it cannot be opened.
// mountedFilepaths entries are real OS directories, so we read the manifest
// straight off disk. We deliberately do NOT use PHYSFS_openRead("mod.json"):
// every mod is mounted at the same virtual root, so all their mod.json files
// would collide at "/mod.json" and PHYSFS would only ever surface one of them.
static bool readWholeFile(const std::string& path, std::string& out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if ( !f.is_open() )
	{
		return false;
	}
	// Cap: mod.json is untrusted Workshop content read on every game start. Refuse
	// an absurdly large manifest before slurping it into memory (bad_alloc / OOM).
	static const std::streamoff MAX_MANIFEST_BYTES = 4 * 1024 * 1024; // 4 MB
	f.seekg(0, std::ios::end);
	const std::streamoff size = f.tellg();
	if ( size > MAX_MANIFEST_BYTES )
	{
		SAM_WARN(MOD, "Manifest " + path + " is too large (" + std::to_string((long long)size)
			+ " bytes, cap " + std::to_string((long long)MAX_MANIFEST_BYTES) + ") — mod skipped.");
		return false;
	}
	f.seekg(0, std::ios::beg);
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

// Join a directory and a filename, tolerating a trailing separator on dir.
static std::string joinPath(const std::string& dir, const std::string& file)
{
	if ( dir.empty() )
	{
		return file;
	}
	const char back = dir.back();
	if ( back == '/' || back == '\\' )
	{
		return dir + file;
	}
	return dir + "/" + file;
}

// Parse "MAJOR.MINOR.PATCH" leniently; missing parts default to 0. Uses strtol
// with clamping instead of sscanf("%d"): a %d conversion that overflows int is
// UB, and the version strings come from untrusted mod.json. Each component is
// clamped to [0, 1000000] so nonsense/overflowing values compare sanely.
static void parseVersion(const std::string& v, int out[3])
{
	out[0] = out[1] = out[2] = 0;
	const char* p = v.c_str();
	for ( int i = 0; i < 3 && *p; ++i )
	{
		char* end = nullptr;
		long val = std::strtol(p, &end, 10);
		if ( end == p ) { break; }             // no digits consumed
		if ( val < 0 ) { val = 0; }
		if ( val > 1000000 ) { val = 1000000; } // clamp absurd / strtol-saturated values
		out[i] = static_cast<int>(val);
		p = end;
		if ( *p == '.' ) { ++p; } else { break; }
	}
}

// Returns true if `have` >= `need` (semantic version comparison).
static bool versionAtLeast(const std::string& have, const std::string& need)
{
	int h[3], n[3];
	parseVersion(have, h);
	parseVersion(need, n);
	for ( int i = 0; i < 3; ++i )
	{
		if ( h[i] != n[i] )
		{
			return h[i] > n[i];
		}
	}
	return true; // equal
}

// A dependency declaration: optional prefix ('?' = optional, '!' = incompatible;
// none = required), a namespace, and an optional "@x.y.z" minimum-version pin.
//   "darkblade_core"        -> required
//   "?enchanting_system"    -> optional (load if present; adjust order; fine if not)
//   "!bad_weapons_mod"      -> incompatible (skip THIS mod if that one is present)
//   "core@1.2.0"            -> required, at least v1.2.0
enum class DepKind { Required, Optional, Incompatible };
struct ParsedDep
{
	DepKind kind = DepKind::Required;
	std::string ns;
	std::string minVersion; // "" if unpinned
};
static ParsedDep parseDep(const std::string& dep)
{
	ParsedDep d;
	std::string s = dep;
	if ( !s.empty() && s[0] == '?' ) { d.kind = DepKind::Optional; s = s.substr(1); }
	else if ( !s.empty() && s[0] == '!' ) { d.kind = DepKind::Incompatible; s = s.substr(1); }
	const size_t at = s.find('@');
	if ( at != std::string::npos ) { d.ns = s.substr(0, at); d.minVersion = s.substr(at + 1); }
	else { d.ns = s; }
	return d;
}

// Strip a leading 'v'/'V' from a version string ("v5.0.2" -> "5.0.2").
static std::string stripVersionPrefix(const std::string& v)
{
	if ( !v.empty() && (v[0] == 'v' || v[0] == 'V') )
	{
		return v.substr(1);
	}
	return v;
}

// Validate a mod's declared version windows against the current S.A.M + Barony
// versions. Philosophy (P3): NEVER hard-block on a version mismatch — warn and
// let the player decide. The ONE exception is an explicit
// `incompatible_with_barony_version` that matches the running game, which returns
// false (skip). Missing/unknown versions are simply not checked.
static bool checkVersions(const SAMModManifest& m, const std::string& baronyVersionRaw)
{
	const std::string label = "[" + m.ns + " v" + m.version + "]";

	// --- S.A.M framework version window ---
	if ( !m.frameworkMinVersion.empty() && !versionAtLeast(SAM_FRAMEWORK_VERSION, m.frameworkMinVersion) )
	{
		SAM_WARN(MOD, "Mod " + label + " requires S.A.M >= " + m.frameworkMinVersion
			+ ", you have " + SAM_FRAMEWORK_VERSION + ". Loaded anyway (may be unstable) — check for a mod update.");
	}
	if ( !m.frameworkMaxVersion.empty() && !versionAtLeast(m.frameworkMaxVersion, SAM_FRAMEWORK_VERSION) )
	{
		SAM_WARN(MOD, "Mod " + label + " was built for S.A.M <= " + m.frameworkMaxVersion
			+ ", you have " + SAM_FRAMEWORK_VERSION + ". Loaded anyway (may be unstable) — check for a mod update.");
	}

	// --- Barony version window (only if we know the running game version) ---
	const std::string bv = stripVersionPrefix(baronyVersionRaw);
	if ( !bv.empty() )
	{
		if ( !m.baronyMinVersion.empty() && !versionAtLeast(bv, m.baronyMinVersion) )
		{
			SAM_WARN(MOD, "Mod " + label + " requires Barony " + m.baronyMinVersion
				+ "+, you have " + bv + ". Loaded anyway (may be unstable) — check for a mod update.");
		}
		if ( !m.baronyMaxVersion.empty() && !versionAtLeast(m.baronyMaxVersion, bv) )
		{
			SAM_WARN(MOD, "Mod " + label + " was built for Barony <= " + m.baronyMaxVersion
				+ ", you have " + bv + ". Loaded anyway (may be unstable) — check for a mod update.");
		}
		if ( !m.incompatibleWithBaronyVersion.empty()
			&& stripVersionPrefix(m.incompatibleWithBaronyVersion) == bv )
		{
			SAM_ERROR(MOD, "Mod " + label + " declares itself INCOMPATIBLE with Barony " + bv
				+ " — NOT loaded (explicit incompatibility).");
			return false;
		}
	}
	return true;
}

// Parse one mod.json document into a manifest. Returns false (and logs) if the
// JSON is malformed or a required field is missing.
static bool parseManifest(const std::string& jsonText, const std::string& modPath,
	const std::string& displayName, SAMModManifest& out)
{
	const std::string fileLabel = SAMErrors::displayFile(displayName, "mod.json");
	json j;
	try
	{
		j = json::parse(jsonText);
	}
	catch ( const json::parse_error& e )
	{
		SAMErrors::reportSyntax(MOD, fileLabel, jsonText, e.what(), e.byte, "mod not loaded.");
		return false;
	}
	if ( !j.is_object() )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
			"a JSON object: { ... }", "wrap the file contents in { }", "mod not loaded.");
		return false;
	}

	auto getString = [&](const char* key) -> std::string {
		auto it = j.find(key);
		if ( it != j.end() && it->is_string() )
		{
			return it->get<std::string>();
		}
		return "";
	};
	auto getStringArray = [&](const char* key, bool arePaths) -> std::vector<std::string> {
		std::vector<std::string> result;
		auto it = j.find(key);
		if ( it != j.end() && it->is_array() )
		{
			for ( const auto& element : *it )
			{
				if ( !element.is_string() ) { continue; }
				const std::string s = element.get<std::string>();
				// Path-traversal guard: these array entries are RELATIVE file paths
				// that the loaders join onto the mod folder and then open/execute.
				// Reject any that escape the mod dir so a crafted "../.." entry can't
				// reach files outside it. (Centralized here so classes/items/patches/
				// monsters/spells/plugins are all covered.)
				if ( arePaths && SAMErrors::relPathEscapes(s) )
				{
					SAM_WARN(MOD, std::string("Manifest '") + key + "' entry '" + s
						+ "' escapes the mod folder — ignored.");
					continue;
				}
				result.push_back(s);
			}
		}
		return result;
	};

	out.ns = getString("namespace");
	out.name = getString("name");
	out.author = getString("author");
	out.version = getString("version");
	out.frameworkMinVersion = getString("framework_min_version");
	out.frameworkMaxVersion = getString("framework_max_version");
	out.baronyMinVersion = getString("barony_min_version");
	out.baronyMaxVersion = getString("barony_max_version");
	out.incompatibleWithBaronyVersion = getString("incompatible_with_barony_version");
	out.description = getString("description");
	out.dependencies = getStringArray("dependencies", false); // namespaces, not paths
	out.classes = getStringArray("classes", true);
	out.items = getStringArray("items", true);
	out.patches = getStringArray("patches", true);
	out.monsters = getStringArray("monsters", true);
	out.spells = getStringArray("spells", true);
	out.effects = getStringArray("effects", true);
	out.races = getStringArray("races", true);
	out.sounds = getStringArray("sounds", true);
	out.plugins = getStringArray("plugins", true);
	out.modPath = modPath;
	out.displayName = displayName;

	// Required fields (mirrors mod.schema.json "required").
	if ( out.ns.empty() )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/namespace", "", "missing (required)",
			"a lowercase id, e.g. \"darkblade\"", "add a \"namespace\" field", "mod not loaded.");
		return false;
	}
	if ( out.name.empty() )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/name", "", "missing (required)",
			"a display name, e.g. \"Darkblade Pack\"", "add a \"name\" field", "mod not loaded.");
		return false;
	}
	if ( out.version.empty() )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/version", "", "missing (required)",
			"a MAJOR.MINOR.PATCH string, e.g. \"1.0.0\"", "add a \"version\" field", "mod not loaded.");
		return false;
	}
	if ( out.frameworkMinVersion.empty() )
	{
		SAMErrors::reportSemantic(MOD, fileLabel, "/framework_min_version", "", "missing (required)",
			"the minimum S.A.M version, e.g. \"0.1.0\"", "add a \"framework_min_version\" field", "mod not loaded.");
		return false;
	}
	return true;
}

// Stable topological sort so that a mod's dependencies load before it does.
// Missing dependencies (namespaces not present in the set) are ignored here —
// scan() warns about them separately. A dependency cycle falls back to declared
// order (with a warning) so loading never hangs.
static std::vector<SAMModManifest> sortByDependencies(const std::vector<SAMModManifest>& mods)
{
	std::set<std::string> present;
	for ( const auto& m : mods )
	{
		present.insert(m.ns);
	}

	std::vector<SAMModManifest> result;
	std::set<std::string> emitted;
	std::vector<const SAMModManifest*> remaining;
	for ( const auto& m : mods )
	{
		remaining.push_back(&m);
	}

	while ( !remaining.empty() )
	{
		bool progress = false;
		std::vector<const SAMModManifest*> next;
		for ( const SAMModManifest* m : remaining )
		{
			bool ready = true;
			for ( const auto& dep : m->dependencies )
			{
				const ParsedDep pd = parseDep(dep);
				if ( pd.kind == DepKind::Incompatible ) { continue; } // not an ordering edge
				// Only block on required/optional deps that ARE present but not yet emitted.
				if ( present.count(pd.ns) && !emitted.count(pd.ns) )
				{
					ready = false;
					break;
				}
			}
			if ( ready )
			{
				result.push_back(*m);
				emitted.insert(m->ns);
				progress = true;
			}
			else
			{
				next.push_back(m);
			}
		}
		if ( !progress )
		{
			SAM_WARN(MOD, "Dependency cycle among " + std::to_string(next.size())
				+ " mod(s); loading them in declared order.");
			for ( const SAMModManifest* m : next )
			{
				result.push_back(*m);
			}
			break;
		}
		remaining.swap(next);
	}
	return result;
}

/*-------------------------------------------------------------------------------
	SAMWorkshop
-------------------------------------------------------------------------------*/

std::vector<SAMModManifest> SAMWorkshop::scan(
	const std::vector<std::pair<std::string, std::string>>& mountedPaths,
	const std::string& baronyVersion)
{
	// Fully clear and rebuild — never append (loadMods() fires on every Play).
	clear();

	std::vector<SAMModManifest> found;
	for ( const auto& entry : mountedPaths )
	{
		const std::string& path = entry.first;
		const std::string& displayName = entry.second;

		const std::string manifestPath = joinPath(path, "mod.json");
		std::string text;
		if ( !readWholeFile(manifestPath, text) )
		{
			// Not a S.A.M mod — just a regular asset-overlay mod. Not an error.
			SAM_DEBUG(MOD, "No mod.json in '" + displayName + "' (" + path + ") — skipping.");
			continue;
		}

		SAMModManifest manifest;
		if ( !parseManifest(text, path, displayName, manifest) )
		{
			continue; // parseManifest already logged the reason
		}

		// Version compatibility — warns on a mismatch (loads anyway), and only
		// skips a mod that declares an explicit incompatibility with this Barony.
		if ( !checkVersions(manifest, baronyVersion) )
		{
			continue;
		}

		found.push_back(manifest);
	}

	// --- Dependency resolution ---
	// `present` = namespaces that survived parsing + version checks.
	std::set<std::string> present;
	for ( const auto& m : found ) { present.insert(m.ns); }

	// Drop mods whose REQUIRED dep is missing, or which are INCOMPATIBLE with a
	// present mod. Removing a mod can cascade (a dependent's required dep goes
	// missing), so iterate to a fixpoint.
	std::set<std::string> removed;
	bool changed = true;
	while ( changed )
	{
		changed = false;
		for ( const auto& m : found )
		{
			if ( removed.count(m.ns) ) { continue; }
			for ( const auto& dep : m.dependencies )
			{
				const ParsedDep pd = parseDep(dep);
				if ( pd.kind == DepKind::Required && !present.count(pd.ns) )
				{
					SAM_ERROR(MOD, "'" + m.name + "' [" + m.ns + "] requires mod [" + pd.ns
						+ "] which is not loaded — NOT loaded (missing required dependency).");
					removed.insert(m.ns); present.erase(m.ns); changed = true; break;
				}
				if ( pd.kind == DepKind::Incompatible && present.count(pd.ns) )
				{
					SAM_ERROR(MOD, "'" + m.name + "' [" + m.ns + "] is incompatible with loaded mod ["
						+ pd.ns + "] — NOT loaded (declared incompatibility).");
					removed.insert(m.ns); present.erase(m.ns); changed = true; break;
				}
			}
		}
	}

	// Survivors, with a clear per-mod resolution summary for any mod with deps.
	std::vector<SAMModManifest> survivors;
	for ( const auto& m : found )
	{
		if ( removed.count(m.ns) ) { continue; }
		survivors.push_back(m);
		if ( m.dependencies.empty() ) { continue; }
		SAM_INFO(MOD, "Resolving dependencies for [" + m.ns + " v" + m.version + "]...");
		for ( const auto& dep : m.dependencies )
		{
			const ParsedDep pd = parseDep(dep);
			const bool here = present.count(pd.ns) > 0;
			if ( pd.kind == DepKind::Required )
			{
				SAM_INFO(MOD, "  Required: [" + pd.ns + "] - present.");
			}
			else if ( pd.kind == DepKind::Optional )
			{
				SAM_INFO(MOD, here ? ("  Optional: [" + pd.ns + "] - present, loading before.")
					: ("  Optional: [" + pd.ns + "] - not present, skipping integration."));
			}
			else // Incompatible (survived, so it's absent)
			{
				SAM_INFO(MOD, "  Incompatible: [" + pd.ns + "] - not present, no conflict.");
			}
			// Present dep with a version pin that's too old: warn (informational).
			if ( here && !pd.minVersion.empty() )
			{
				for ( const auto& x : found )
				{
					if ( x.ns == pd.ns && !versionAtLeast(x.version, pd.minVersion) )
					{
						SAM_WARN(MOD, "  [" + pd.ns + "] is v" + x.version + " but [" + m.ns
							+ "] wants >= " + pd.minVersion + " - loaded anyway.");
					}
				}
			}
		}
	}

	registry = sortByDependencies(survivors);
	return registry;
}

void SAMWorkshop::clear()
{
	registry.clear();
}

const std::vector<SAMModManifest>& SAMWorkshop::manifests()
{
	return registry;
}
