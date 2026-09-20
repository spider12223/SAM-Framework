/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_workshop.cpp
	Desc: implementation of the S.A.M mod-manifest reader.

-------------------------------------------------------------------------------*/

#include "sam_workshop.hpp"
#include <algorithm>
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include <cstdio>
#include <cstdlib>  // strtol
#include <fstream>
#include <iterator>   // istreambuf_iterator (contentDigestFor)
#include <sstream>
#include <set>
#include <filesystem>  // the sounds/ and music/ folders a mod may drop files into

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

/*-------------------------------------------------------------------------------
	Sounds and music

	A modder should be able to add a sound by dropping a file in a folder. Two routes, and
	the object form in mod.json wins where both name the same thing:

	  sounds/<name>.ogg          -> "<ns>:<name>"
	  sounds/replace/<name>.ogg  -> replaces the vanilla sound or group <name>
	  music/<name>.ogg           -> music track "<ns>:<name>"
	  music/replace/<name>.ogg   -> replaces the vanilla track <name>
-------------------------------------------------------------------------------*/

static bool samIsAudioFile(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	if ( dot == std::string::npos ) { return false; }
	std::string ext = name.substr(dot + 1);
	for ( char& c : ext ) { c = (char)std::tolower((unsigned char)c); }
	// Whatever FMOD decodes. .ogg is what Barony itself ships; the rest are what modders have.
	return ext == "ogg" || ext == "wav" || ext == "mp3" || ext == "flac";
}

// A file name as an id: lower case, anything outside a-z 0-9 _ becomes _. "Big Boom!.ogg" is
// "big_boom_" -- predictable, and printed in the log so nobody has to guess.
static std::string samAudioSlug(const std::string& stem)
{
	std::string s = stem;
	for ( char& c : s )
	{
		const unsigned char u = (unsigned char)c;
		if ( std::isalnum(u) ) { c = (char)std::tolower(u); }
		else { c = '_'; }
	}
	return s;
}

// One object from "sounds" or "music". `where` names it in any error ("sounds[3]").
static bool samParseAudioDecl(const json& el, bool isMusic, const std::string& ns,
	const std::string& fileLabel, const std::string& where, SAMAudioDecl& d)
{
	const char* what = isMusic ? "music" : "sounds";
	auto fail = [&](const std::string& field, const std::string& problem, const std::string& expected,
		const std::string& fix) {
		SAMErrors::reportSemantic(MOD, fileLabel, "/" + where + field, "", problem, expected, fix,
			"that entry ignored.", true);
		return false;
	};

	if ( auto it = el.find("id"); it != el.end() )
	{
		if ( !it->is_string() || it->get<std::string>().empty() ) { return fail("/id", "not a name", "a name, e.g. \"boom\"", "put the name in quotes"); }
		d.id = it->get<std::string>();
		if ( d.id.find(':') == std::string::npos ) { d.id = ns + ":" + d.id; }
		else if ( d.id.compare(0, ns.size() + 1, ns + ":") != 0 )
		{
			SAM_WARN(MOD, std::string("[") + ns + "] " + what + " id '" + d.id + "' uses another mod's"
				" namespace. It still loads, but two mods can now fight over one name; drop the"
				" prefix and it becomes '" + ns + ":...' automatically.");
		}
	}
	if ( auto it = el.find("replace"); it != el.end() )
	{
		if ( it->is_string() ) { d.replace = it->get<std::string>(); }
		else if ( it->is_number_integer() ) { d.replace = std::to_string(it->get<long long>()); }
		else { return fail("/replace", "not a name", "the vanilla name, e.g. \"SwingWeapon\"", "put the name in quotes"); }
	}
	if ( d.id.empty() == d.replace.empty() )
	{
		return fail("", d.id.empty() ? "has neither \"id\" nor \"replace\"" : "has both \"id\" and \"replace\"",
			"\"id\" to add a new one, or \"replace\" to swap a vanilla one", "keep exactly one of the two");
	}

	auto fileOk = [&](const std::string& f) {
		if ( f.empty() ) { return false; }
		if ( SAMErrors::relPathEscapes(f) )
		{
			SAM_WARN(MOD, std::string("[") + ns + "] " + what + " file '" + f + "' escapes the mod folder -- ignored.");
			return false;
		}
		return true;
	};
	if ( auto it = el.find("file"); it != el.end() )
	{
		if ( it->is_string() ) { if ( fileOk(it->get<std::string>()) ) { d.files.push_back(it->get<std::string>()); } }
		else if ( it->is_array() )
		{
			for ( const auto& f : *it )
			{
				if ( f.is_string() && fileOk(f.get<std::string>()) ) { d.files.push_back(f.get<std::string>()); }
			}
		}
	}
	if ( d.files.empty() )
	{
		return fail("/file", "missing", std::string("a mod-relative audio file, e.g. \"") + (isMusic ? "music" : "sounds")
			+ "/boom.ogg\", or a list of them to pick from at random", "add a \"file\" field");
	}

	if ( auto it = el.find("volume"); it != el.end() && it->is_number() )
	{
		double v = it->get<double>();
		if ( !(v >= 0.0) ) { v = 0.0; }
		if ( v > 4.0 ) { v = 4.0; }
		d.volume = v;
	}
	if ( auto it = el.find("loop"); it != el.end() && it->is_boolean() ) { d.loop = it->get<bool>(); d.loopSet = true; }

	if ( isMusic )
	{
		if ( auto it = el.find("floors"); it != el.end() )
		{
			if ( it->is_number_integer() ) { d.floors.push_back(it->get<int>()); }
			else if ( it->is_array() ) { for ( const auto& f : *it ) { if ( f.is_number_integer() ) { d.floors.push_back(f.get<int>()); } } }
		}
		if ( auto it = el.find("maps"); it != el.end() )
		{
			if ( it->is_string() ) { d.maps.push_back(it->get<std::string>()); }
			else if ( it->is_array() ) { for ( const auto& m : *it ) { if ( m.is_string() ) { d.maps.push_back(m.get<std::string>()); } } }
		}
		if ( auto it = el.find("combat"); it != el.end() && it->is_string() && fileOk(it->get<std::string>()) )
		{
			d.combat = it->get<std::string>();
		}
		if ( !d.replace.empty() && (!d.floors.empty() || !d.maps.empty()) )
		{
			SAM_WARN(MOD, std::string("[") + ns + "] music '" + d.replace + "': \"floors\" and \"maps\""
				" are for a new track; a replacement plays wherever the vanilla one would. Ignored.");
			d.floors.clear(); d.maps.clear();
		}
	}
	d.origin = std::string("mod.json ") + where;
	return true;
}

// Files dropped in <mod>/<sub>/, sorted so the order -- and every index handed out from it -- is
// the same on every machine.
static void samScanAudioFolder(const std::string& modPath, const std::string& sub, bool replaces,
	const std::string& ns, std::vector<SAMAudioDecl>& out)
{
	std::error_code ec;
	const std::filesystem::path dir = std::filesystem::path(modPath) / sub;
	if ( !std::filesystem::is_directory(dir, ec) ) { return; }
	std::vector<std::string> names;
	for ( const auto& entry : std::filesystem::directory_iterator(dir, ec) )
	{
		if ( !entry.is_regular_file(ec) ) { continue; }
		const std::string name = entry.path().filename().string();
		if ( samIsAudioFile(name) ) { names.push_back(name); }
	}
	std::sort(names.begin(), names.end());
	for ( const auto& name : names )
	{
		const std::string stem = name.substr(0, name.find_last_of('.'));
		SAMAudioDecl d;
		d.files.push_back(sub + "/" + name);
		if ( replaces ) { d.replace = stem; }
		else { d.id = ns + ":" + samAudioSlug(stem); }
		d.origin = sub + "/" + name;
		d.fromFolder = true;
		out.push_back(d);
	}
}

// Folder files join the declared list unless mod.json already names the same id or target.
static void samMergeFolder(std::vector<SAMAudioDecl>& declared, const std::vector<SAMAudioDecl>& found)
{
	for ( const auto& f : found )
	{
		bool taken = false;
		for ( const auto& d : declared )
		{
			std::string a = f.id.empty() ? f.replace : f.id, b = d.id.empty() ? d.replace : d.id;
			for ( char& c : a ) { c = (char)std::tolower((unsigned char)c); }
			for ( char& c : b ) { c = (char)std::tolower((unsigned char)c); }
			if ( a == b && f.id.empty() == d.id.empty() ) { taken = true; break; }
		}
		if ( !taken ) { declared.push_back(f); }
	}
}

// Content digest: FNV-1a 64 over every file the manifest DECLARES, each as its relative
// path followed by its bytes, in sorted order so mount order cannot change the result.
// '\r' bytes are dropped so a Windows (CRLF) checkout of the same mod digests like a Linux
// one. Only declared files are walked: a .vox or .ogg that a JSON merely refers to is not
// (the JSON naming it is). A declared file that is missing digests as "<missing>", so the
// U13 case -- a room one side does not have -- shows up as a digest difference.
static std::string contentDigestFor(const SAMModManifest& m)
{
	std::vector<std::string> rels;
	auto addAll = [&](const std::vector<std::string>& v) { rels.insert(rels.end(), v.begin(), v.end()); };
	addAll(m.classes); addAll(m.items); addAll(m.patches); addAll(m.monsters); addAll(m.spells);
	addAll(m.effects); addAll(m.races); addAll(m.sounds); addAll(m.recipes);
	for ( const auto& kv : m.models ) { rels.push_back(kv.second); }
	for ( const auto& kv : m.images ) { rels.push_back(kv.second); }
	for ( const auto& set : m.rooms ) { addAll(set.second); }
	std::vector<std::string> audio;
	for ( const auto& d : m.soundDecls ) { audio.insert(audio.end(), d.files.begin(), d.files.end()); }
	for ( const auto& d : m.musicDecls ) { audio.insert(audio.end(), d.files.begin(), d.files.end()); if ( !d.combat.empty() ) { audio.push_back(d.combat); } }
	if ( rels.empty() && audio.empty() ) { return std::string(); }
	std::sort(rels.begin(), rels.end());
	rels.erase(std::unique(rels.begin(), rels.end()), rels.end());

	unsigned long long h = 14695981039346656037ULL;
	auto mix = [&h](const char* data, size_t n)
	{
		for ( size_t i = 0; i < n; ++i )
		{
			const unsigned char c = static_cast<unsigned char>(data[i]);
			if ( c == '\r' ) { continue; }
			h ^= c;
			h *= 1099511628211ULL;
		}
		h ^= 0xFFu; // field separator so "ab"+"c" and "a"+"bc" differ
		h *= 1099511628211ULL;
	};
	for ( const auto& rel : rels )
	{
		mix(rel.data(), rel.size());
		std::ifstream f(joinPath(m.modPath, rel), std::ios::binary);
		if ( f.is_open() )
		{
			std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			mix(bytes.data(), bytes.size());
		}
		else
		{
			static const char kMissing[] = "<missing>";
			mix(kMissing, sizeof(kMissing) - 1);
		}
	}
	// Audio: path and size only. Two machines with the same file set get the same sound
	// indices, and that is what this has to catch; hashing the bytes of every music track on
	// every game start would cost seconds for nothing more.
	std::sort(audio.begin(), audio.end());
	audio.erase(std::unique(audio.begin(), audio.end()), audio.end());
	for ( const auto& rel : audio )
	{
		std::error_code ec;
		const auto size = std::filesystem::file_size(std::filesystem::path(joinPath(m.modPath, rel)), ec);
		const std::string tag = rel + "#" + (ec ? std::string("<missing>") : std::to_string((unsigned long long)size));
		mix(tag.data(), tag.size());
	}
	char buf[24];
	snprintf(buf, sizeof(buf), "%016llx", h);
	return std::string(buf);
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
				if ( !element.is_string() )
				{
					SAMErrors::reportSemantic(MOD, fileLabel, std::string("/") + key, element.dump().substr(0, 40),
						"not a string", "a quoted path, e.g. \"classes/knight.json\"", "put quotes around it",
						"that entry ignored.", true);
					continue;
				}
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
	out.frameworkMinVersion = stripVersionPrefix(getString("framework_min_version"));
	out.frameworkMaxVersion = stripVersionPrefix(getString("framework_max_version"));
	out.baronyMinVersion = stripVersionPrefix(getString("barony_min_version"));
	out.baronyMaxVersion = stripVersionPrefix(getString("barony_max_version"));
	out.incompatibleWithBaronyVersion = stripVersionPrefix(getString("incompatible_with_barony_version"));
	out.description = getString("description");

	// Report what the schema would have caught. A mod.json is usually hand-written, and a
	// misspelled or wrong-typed key used to load without a word and register nothing --
	// "my classes don't show up" with an empty log. The builder validates against the
	// schema; the loader is the last line for everyone else.
	{
		static const char* const kKnown[] = { "$schema", "namespace", "name", "author", "version",
			"framework_min_version", "framework_max_version", "barony_min_version", "barony_max_version",
			"incompatible_with_barony_version", "dependencies", "classes", "items", "patches", "monsters",
			"spells", "effects", "races", "sounds", "music", "recipes", "plugins", "models", "images", "rooms",
			"description", nullptr };
		static const char* const kArrays[] = { "dependencies", "classes", "items", "patches", "monsters",
			"spells", "effects", "races", "sounds", "music", "recipes", "plugins", "models", "images", nullptr };
		for ( auto it = j.begin(); it != j.end(); ++it )
		{
			bool known = false;
			for ( const char* const* k = kKnown; *k; ++k ) { if ( it.key() == *k ) { known = true; break; } }
			if ( !known )
			{
				SAMErrors::reportSemantic(MOD, fileLabel, "/" + it.key(), "", "not a mod.json key",
					"one of the keys in mod.schema.json (check the spelling and the case)",
					"fix or remove it", "that key ignored.", true);
			}
		}
		for ( const char* const* k = kArrays; *k; ++k )
		{
			auto it = j.find(*k);
			if ( it != j.end() && !it->is_array() )
			{
				SAMErrors::reportSemantic(MOD, fileLabel, std::string("/") + *k, it->dump().substr(0, 40),
					"not an array", "a JSON array: [ ... ]", "wrap the value in [ ]",
					"nothing under that key loaded.", true);
			}
		}
	}
	// "rooms": { "<levelset>": ["rooms/foo.lmp", ...] } -- prefab rooms added to an
	// existing levelset's pool. Parsed here; ordering/validation happens in SAMRooms.
	{
		auto it = j.find("rooms");
		if ( it != j.end() && it->is_object() )
		{
			for ( auto r = it->begin(); r != it->end(); ++r )
			{
				if ( !r.value().is_array() ) { continue; }
				std::vector<std::string> paths;
				for ( const auto& el : r.value() )
				{
					if ( el.is_string() ) { paths.push_back(el.get<std::string>()); }
				}
				if ( !paths.empty() ) { out.rooms.emplace_back(r.key(), paths); }
			}
		}
	}
	out.dependencies = getStringArray("dependencies", false); // namespaces, not paths
	out.classes = getStringArray("classes", true);
	out.items = getStringArray("items", true);
	out.patches = getStringArray("patches", true);
	out.monsters = getStringArray("monsters", true);
	out.spells = getStringArray("spells", true);
	out.effects = getStringArray("effects", true);
	out.races = getStringArray("races", true);
	// "sounds": each entry is either a path to a sound JSON file (the older form, still read) or
	// the sound itself as an object -- { "id": "boom", "file": "sounds/boom.ogg" } or
	// { "replace": "SwingWeapon", "file": "sounds/whoosh.ogg" }. "music" takes the objects.
	for ( const char* key : { "sounds", "music" } )
	{
		const bool isMusic = std::string(key) == "music";
		auto it = j.find(key);
		if ( it == j.end() || !it->is_array() ) { continue; }
		int index = 0;
		for ( const auto& el : *it )
		{
			const std::string where = std::string(key) + "[" + std::to_string(index++) + "]";
			if ( el.is_string() && !isMusic )
			{
				const std::string s = el.get<std::string>();
				// A bare audio path here is the most common first attempt, and it used to be read
				// as JSON and reported as a syntax error in the sound file. Take it as meant.
				if ( samIsAudioFile(s) )
				{
					if ( SAMErrors::relPathEscapes(s) ) { SAM_WARN(MOD, "Manifest 'sounds' entry '" + s + "' escapes the mod folder -- ignored."); continue; }
					SAMAudioDecl d;
					d.files.push_back(s);
					const size_t slash = s.find_last_of("/\\");
					const std::string base = (slash == std::string::npos) ? s : s.substr(slash + 1);
					d.id = getString("namespace") + ":" + samAudioSlug(base.substr(0, base.find_last_of('.')));
					d.origin = std::string("mod.json ") + where;
					out.soundDecls.push_back(d);
					continue;
				}
				if ( SAMErrors::relPathEscapes(s) ) { SAM_WARN(MOD, "Manifest 'sounds' entry '" + s + "' escapes the mod folder -- ignored."); continue; }
				out.sounds.push_back(s);
				continue;
			}
			if ( !el.is_object() )
			{
				SAMErrors::reportSemantic(MOD, fileLabel, "/" + where, el.dump().substr(0, 40), "not an entry",
					isMusic ? "an object: { \"id\": \"boss\", \"file\": \"music/boss.ogg\" }"
					        : "an object: { \"id\": \"boom\", \"file\": \"sounds/boom.ogg\" }, or a path",
					"write it as an object", "that entry ignored.", true);
				continue;
			}
			SAMAudioDecl d;
			if ( samParseAudioDecl(el, isMusic, getString("namespace"), fileLabel, where, d) )
			{
				(isMusic ? out.musicDecls : out.soundDecls).push_back(d);
			}
		}
	}
	out.recipes = getStringArray("recipes", true);
	out.plugins = getStringArray("plugins", true);
	// v1.4.0 — standalone .vox models for sam_spawn_companion / decorative entities. An
	// array of { "id": "ns:name", "file": "models/.../x.vox" }. The id is how a script
	// names the model (sam_spawn_companion); file is the mod-relative .vox path.
	{
		auto it = j.find("models");
		if ( it != j.end() && it->is_array() )
		{
			for ( const auto& el : *it )
			{
				if ( !el.is_object() ) { continue; }
				std::string id, file;
				if ( auto idIt = el.find("id"); idIt != el.end() && idIt->is_string() ) { id = idIt->get<std::string>(); }
				if ( auto fIt = el.find("file"); fIt != el.end() && fIt->is_string() ) { file = fIt->get<std::string>(); }
				if ( id.empty() || file.empty() ) { continue; }
				if ( SAMErrors::relPathEscapes(file) )
				{
					SAM_WARN(MOD, std::string("Manifest 'models' file '") + file + "' escapes the mod folder — ignored.");
					continue;
				}
				out.models.push_back({ id, file });
			}
		}
	}

	// v1.10.3 -- pictures the mod draws itself. Same shape as "models": an array of
	// { "id": "ns:name", "file": "art/x.png" }.
	{
		auto it = j.find("images");
		if ( it != j.end() && it->is_array() )
		{
			for ( const auto& el : *it )
			{
				if ( !el.is_object() ) { continue; }
				std::string id, file;
				if ( auto idIt = el.find("id"); idIt != el.end() && idIt->is_string() ) { id = idIt->get<std::string>(); }
				if ( auto fIt = el.find("file"); fIt != el.end() && fIt->is_string() ) { file = fIt->get<std::string>(); }
				if ( id.empty() || file.empty() ) { continue; }
				if ( SAMErrors::relPathEscapes(file) )
				{
					SAM_WARN(MOD, std::string("Manifest 'images' file '") + file + "' escapes the mod folder - ignored.");
					continue;
				}
				out.images.push_back({ id, file });
			}
		}
	}
	out.modPath = modPath;
	out.displayName = displayName;

	// The folders. After mod.json, so an object there for the same id or target wins.
	if ( !out.ns.empty() )
	{
		std::vector<SAMAudioDecl> found;
		samScanAudioFolder(modPath, "sounds", false, out.ns, found);
		samScanAudioFolder(modPath, "sounds/replace", true, out.ns, found);
		samMergeFolder(out.soundDecls, found);
		found.clear();
		samScanAudioFolder(modPath, "music", false, out.ns, found);
		samScanAudioFolder(modPath, "music/replace", true, out.ns, found);
		samMergeFolder(out.musicDecls, found);
	}

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

	// DETERMINISTIC TIE-BREAK. Dependencies still come first -- that is what the loop below
	// does -- but among mods that do not depend on each other the order was whatever order
	// they happened to be mounted in, i.e. the arrangement of the player's mod list.
	//
	// That mattered far more than it looks. Custom content ids (items, classes, races,
	// spells, effects) are handed out in load order and then written RAW into savegames and
	// RAW onto the wire, with nothing anywhere mapping them back to a name. So reordering
	// your mod list silently turned every saved custom item into a different mod's item, and
	// two players with the SAME mods in a different order got different ids for the same
	// content -- which the multiplayer fingerprint could not detect, because it compares a
	// sorted list of names.
	//
	// Sorting by namespace makes the result depend only on WHICH mods are loaded, never on
	// how they are arranged. Same set, same ids, every machine, every launch.
	std::sort(remaining.begin(), remaining.end(),
		[](const SAMModManifest* a, const SAMModManifest* b) { return a->ns < b->ns; });

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

		manifest.contentDigest = contentDigestFor(manifest);

		// Two mods sharing a namespace break the one thing the namespace sort exists to
		// guarantee: their sort key is equal, so their relative order -- and every content id
		// after them -- is whatever the mount order happened to be, and the multiplayer
		// fingerprint cannot see it because both machines produce the same sorted NAME list.
		// Keep the first, refuse the rest, loudly.
		bool dupNs = false;
		for ( const auto& prior : found )
		{
			if ( prior.ns == manifest.ns )
			{
				SAM_ERROR(MOD, "Mod at '" + manifest.modPath + "' uses namespace [" + manifest.ns
					+ "], already taken by the mod at '" + prior.modPath
					+ "' -- NOT loaded. Every mod needs a namespace of its own.");
				dupNs = true;
				break;
			}
		}
		if ( dupNs ) { continue; }
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
