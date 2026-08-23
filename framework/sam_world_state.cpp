#include "sam_world_state.hpp"
#include "sam_logger.hpp"

#include <cstring>
#include <map>

namespace SAMWorldState
{
	namespace
	{
		// namespace -> key -> value
		std::map<std::string, std::map<std::string, std::string>> s_data;

		constexpr const char* kPrefix = "sam:";

		size_t entryBytes(const std::string& ns, const std::string& key, const std::string& v)
		{
			// what this pair will actually occupy in additional_data
			return 4 + ns.size() + 1 + key.size() + v.size();
		}

		// A namespace becomes part of the stored key and ':' is the separator, so a
		// namespace containing one would make the key ambiguous on the way back in.
		// Mod namespaces are already plain identifiers, so this only ever rejects a
		// genuinely malformed one. Keys may contain ':' freely -- absorb() splits on
		// the first separator only.
		bool namespaceUsable(const std::string& ns)
		{
			return !ns.empty() && ns.find(':') == std::string::npos;
		}
	}

	size_t totalBytes()
	{
		size_t n = 0;
		for ( const auto& nsEntry : s_data )
		{
			for ( const auto& kv : nsEntry.second )
			{
				n += entryBytes(nsEntry.first, kv.first, kv.second);
			}
		}
		return n;
	}

	bool set(const std::string& ns, const std::string& key, const std::string& value)
	{
		if ( !namespaceUsable(ns) )
		{
			SAM_ERROR("WORLD", "sam_world_save: mod namespace '" + ns + "' cannot be used"
				" for saved state (it must not be empty or contain ':').");
			return false;
		}
		if ( key.empty() || key.size() > kMaxKeyBytes )
		{
			SAM_ERROR("WORLD", "sam_world_save: key must be 1-" + std::to_string(kMaxKeyBytes)
				+ " bytes.");
			return false;
		}
		if ( value.size() > kMaxValueBytes )
		{
			SAM_ERROR("WORLD", "sam_world_save: value for '" + key + "' is "
				+ std::to_string(value.size()) + " bytes, over the "
				+ std::to_string(kMaxValueBytes) + " byte limit. Saved state is meant for"
				" flags and counters; store items in a stash chest instead.");
			return false;
		}

		// Measure the total as it would be AFTER this write, so replacing a big value
		// with a small one is never refused.
		size_t projected = totalBytes() + entryBytes(ns, key, value);
		auto nsIt = s_data.find(ns);
		if ( nsIt != s_data.end() )
		{
			auto kIt = nsIt->second.find(key);
			if ( kIt != nsIt->second.end() )
			{
				projected -= entryBytes(ns, key, kIt->second);
			}
		}
		if ( projected > kMaxTotalBytes )
		{
			SAM_ERROR("WORLD", "sam_world_save: refused '" + key + "' -- saved state would"
				" reach " + std::to_string(projected) + " bytes, over the "
				+ std::to_string(kMaxTotalBytes) + " byte budget shared by all mods."
				" Oversized save data can produce a savegame that fails to load.");
			return false;
		}

		s_data[ns][key] = value;
		return true;
	}

	bool get(const std::string& ns, const std::string& key, std::string& out)
	{
		auto nsIt = s_data.find(ns);
		if ( nsIt == s_data.end() ) { return false; }
		auto kIt = nsIt->second.find(key);
		if ( kIt == nsIt->second.end() ) { return false; }
		out = kIt->second;
		return true;
	}

	bool erase(const std::string& ns, const std::string& key)
	{
		auto nsIt = s_data.find(ns);
		if ( nsIt == s_data.end() ) { return false; }
		return nsIt->second.erase(key) > 0;
	}

	std::vector<std::string> keys(const std::string& ns)
	{
		std::vector<std::string> out;
		auto nsIt = s_data.find(ns);
		if ( nsIt == s_data.end() ) { return out; }
		for ( const auto& kv : nsIt->second ) { out.push_back(kv.first); }
		return out;
	}

	void collect(std::vector<std::pair<std::string, std::string>>& out)
	{
		for ( const auto& nsEntry : s_data )
		{
			for ( const auto& kv : nsEntry.second )
			{
				out.push_back(std::make_pair(
					std::string(kPrefix) + nsEntry.first + ":" + kv.first, kv.second));
			}
		}
	}

	void beginLoad()
	{
		// A save is authoritative for its own character: whatever we were holding from a
		// previous run must not survive into this one.
		s_data.clear();
	}

	bool absorb(const std::string& key, const std::string& value)
	{
		const size_t plen = strlen(kPrefix);
		if ( key.compare(0, plen, kPrefix) != 0 ) { return false; }
		const size_t sep = key.find(':', plen);
		if ( sep == std::string::npos || sep == plen ) { return false; }
		const std::string ns = key.substr(plen, sep - plen);
		const std::string k = key.substr(sep + 1);
		if ( k.empty() ) { return false; }
		s_data[ns][k] = value;
		return true;
	}

	void clearAll()
	{
		s_data.clear();
	}
}
