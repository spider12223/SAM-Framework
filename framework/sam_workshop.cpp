/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_workshop.cpp
	Desc: implementation of the S.A.M mod-manifest reader.

-------------------------------------------------------------------------------*/

#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "nlohmann/json.hpp"

#include <cstdio>   // sscanf
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

// Parse "MAJOR.MINOR.PATCH" leniently; missing parts default to 0.
static void parseVersion(const std::string& v, int out[3])
{
	out[0] = out[1] = out[2] = 0;
	sscanf(v.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]);
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

// Strip an optional "@x.y.z" version pin from a dependency string.
static std::string dependencyNamespace(const std::string& dep)
{
	const size_t at = dep.find('@');
	return (at == std::string::npos) ? dep : dep.substr(0, at);
}

// Parse one mod.json document into a manifest. Returns false (and logs) if the
// JSON is malformed or a required field is missing.
static bool parseManifest(const std::string& jsonText, const std::string& modPath,
	const std::string& displayName, SAMModManifest& out)
{
	// allow_exceptions = false: json::parse returns a discarded value on error
	// instead of throwing, so this is safe regardless of the build's exception
	// settings.
	json j = json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
	if ( j.is_discarded() || !j.is_object() )
	{
		SAM_ERROR(MOD, "Invalid mod.json (not valid JSON object) in: " + modPath);
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
	auto getStringArray = [&](const char* key) -> std::vector<std::string> {
		std::vector<std::string> result;
		auto it = j.find(key);
		if ( it != j.end() && it->is_array() )
		{
			for ( const auto& element : *it )
			{
				if ( element.is_string() )
				{
					result.push_back(element.get<std::string>());
				}
			}
		}
		return result;
	};

	out.ns = getString("namespace");
	out.name = getString("name");
	out.author = getString("author");
	out.version = getString("version");
	out.frameworkMinVersion = getString("framework_min_version");
	out.description = getString("description");
	out.dependencies = getStringArray("dependencies");
	out.classes = getStringArray("classes");
	out.items = getStringArray("items");
	out.plugins = getStringArray("plugins");
	out.modPath = modPath;
	out.displayName = displayName;

	// Required fields (mirrors mod.schema.json "required").
	if ( out.ns.empty() )
	{
		SAM_ERROR(MOD, "mod.json missing required 'namespace' in: " + modPath);
		return false;
	}
	if ( out.name.empty() )
	{
		SAM_ERROR(MOD, "mod.json missing required 'name' [" + out.ns + "] in: " + modPath);
		return false;
	}
	if ( out.version.empty() )
	{
		SAM_ERROR(MOD, "mod.json missing required 'version' [" + out.ns + "] in: " + modPath);
		return false;
	}
	if ( out.frameworkMinVersion.empty() )
	{
		SAM_ERROR(MOD, "mod.json missing required 'framework_min_version' [" + out.ns + "] in: " + modPath);
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
				const std::string dns = dependencyNamespace(dep);
				// Only block on dependencies that ARE present but not yet emitted.
				if ( present.count(dns) && !emitted.count(dns) )
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
	const std::vector<std::pair<std::string, std::string>>& mountedPaths)
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

		if ( !versionAtLeast(SAM_FRAMEWORK_VERSION, manifest.frameworkMinVersion) )
		{
			SAM_ERROR(MOD, "Skipping '" + manifest.name + "' [" + manifest.ns + "]: requires S.A.M >= "
				+ manifest.frameworkMinVersion + " but this build is " + SAM_FRAMEWORK_VERSION + ".");
			continue;
		}

		found.push_back(manifest);
	}

	// Warn about declared dependencies that are not installed.
	std::set<std::string> presentNs;
	for ( const auto& m : found )
	{
		presentNs.insert(m.ns);
	}
	for ( const auto& m : found )
	{
		for ( const auto& dep : m.dependencies )
		{
			const std::string dns = dependencyNamespace(dep);
			if ( !presentNs.count(dns) )
			{
				SAM_WARN(MOD, "'" + m.name + "' [" + m.ns + "] depends on '" + dns
					+ "', which is not installed.");
			}
		}
	}

	registry = sortByDependencies(found);
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
