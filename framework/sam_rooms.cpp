/*-------------------------------------------------------------------------------
	S.A.M Framework — room injection. See sam_rooms.hpp for the ordering invariant.
-------------------------------------------------------------------------------*/

#include "sam_rooms.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"

#include <algorithm>
#include <fstream>
#include <map>

namespace
{
	const char* MOD = "ROOMS";

	// One injected room. sortKey is what makes multiplayer safe: it is derived only from
	// the mod namespace and the declared path, never from load order or directory listing
	// order, so two machines with the same mods always produce the same sequence.
	struct Room
	{
		std::string sortKey;   // "<namespace>/<relative path>"
		std::string absPath;   // resolved absolute .lmp
	};

	std::map<std::string, std::vector<std::string>> s_byLevelset; // levelset -> sorted abs paths
	int s_count = 0;
	const std::vector<std::string> s_empty;

	std::string joinPath(const std::string& dir, const std::string& file)
	{
		if ( dir.empty() ) { return file; }
		if ( file.empty() ) { return dir; }
		const char last = dir[dir.size() - 1];
		if ( last == '/' || last == '\\' ) { return dir + file; }
		return dir + "/" + file;
	}

	bool fileExists(const std::string& p)
	{
		std::ifstream f(p.c_str(), std::ios::binary);
		return f.good();
	}

	// Barony's own level names are lowercase; normalise so "Mine" and "mine" mean the same
	// levelset rather than silently creating a second bucket nothing ever reads.
	std::string lower(std::string s)
	{
		for ( char& c : s ) { c = (char)std::tolower((unsigned char)c); }
		return s;
	}
}

void SAMRooms::applyAll(const std::vector<SAMModManifest>& mods)
{
	clear();

	std::map<std::string, std::vector<Room>> gathered;

	for ( const SAMModManifest& m : mods )
	{
		for ( const auto& kv : m.rooms )
		{
			const std::string levelset = lower(kv.first);
			if ( levelset.empty() ) { continue; }
			for ( const std::string& rel : kv.second )
			{
				if ( rel.empty() ) { continue; }
				if ( SAMErrors::relPathEscapes(rel) )
				{
					SAM_WARN(MOD, "Mod [" + m.ns + "] room path '" + rel
						+ "' escapes the mod folder - ignoring it.");
					continue;
				}
				const std::string abs = joinPath(m.modPath, rel);
				if ( !fileExists(abs) )
				{
					SAM_WARN(MOD, "Mod [" + m.ns + "] declares room '" + rel + "' for levelset '"
						+ levelset + "', but the file is not there (looked for " + abs + ").");
					continue;
				}
				Room r;
				r.sortKey = m.ns + "/" + rel;
				r.absPath = abs;
				gathered[levelset].push_back(r);
			}
		}
	}

	for ( auto& kv : gathered )
	{
		// THE invariant. Sorting by (namespace, path) rather than trusting mod load order
		// is what stops two players in the same game generating different dungeons: the
		// generator indexes this list with a shared random seed, so the order has to be a
		// property of the CONTENT, not of the order somebody happened to click their mods.
		std::sort(kv.second.begin(), kv.second.end(),
			[](const Room& a, const Room& b) { return a.sortKey < b.sortKey; });

		std::vector<std::string>& out = s_byLevelset[kv.first];
		out.reserve(kv.second.size());
		for ( const Room& r : kv.second ) { out.push_back(r.absPath); }
		s_count += (int)out.size();

		SAM_INFO(MOD, "Levelset '" + kv.first + "' gains " + std::to_string(out.size())
			+ " room(s) from mods.");
	}

	if ( s_count > 0 )
	{
		SAM_INFO(MOD, "Injected " + std::to_string(s_count) + " room(s) across "
			+ std::to_string((int)s_byLevelset.size()) + " levelset(s).");
	}
}

void SAMRooms::clear()
{
	s_byLevelset.clear();
	s_count = 0;
}

bool SAMRooms::any() { return s_count > 0; }

const std::vector<std::string>& SAMRooms::roomsFor(const std::string& levelset)
{
	if ( s_byLevelset.empty() ) { return s_empty; }
	auto it = s_byLevelset.find(lower(levelset));
	return ( it != s_byLevelset.end() ) ? it->second : s_empty;
}

int SAMRooms::count() { return s_count; }
int SAMRooms::levelsets() { return (int)s_byLevelset.size(); }
