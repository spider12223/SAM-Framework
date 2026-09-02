#include "sam_world_state.hpp"
#include "sam_logger.hpp"
#include "sam_sync.hpp"   // the mod-set fingerprint this save was made with
#include "sam_items.hpp"  // saveIdTable: what every custom item id MEANT when this save was written

#include <cstring>
#include <map>

namespace SAMWorldState
{
	namespace
	{
		// namespace -> key -> value
		std::map<std::string, std::map<std::string, std::string>> s_data;

		constexpr const char* kPrefix = "sam:";
		// Deliberately NOT under the "sam:<ns>:" prefix, so absorb() never mistakes it for a
		// mod's own key and no mod can write to it.
		constexpr const char* kModSetKey = "sam_modset";
		// "5000=ns:a;5001=ns:b;..." -- read back by the engine (scores.cpp getSaveGameInfo)
		// the moment the save file is parsed, before any item is created from it.
		constexpr const char* kItemIdsKey = "sam_itemids";
		std::string s_savedModSet;   // what the save being loaded was made with
		bool        s_haveSavedModSet = false;

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
		// Record the mod set even when no mod stored anything: the point is to know what this
		// save's ids were allocated against, and that is true whether or not a script used
		// the world store. Costs one short string, and nothing at all with no mods loaded.
		const std::string fp = SAMSync::generateFingerprint();
		if ( !fp.empty() ) { out.push_back(std::make_pair(std::string(kModSetKey), fp)); }
		// And what each custom item id MEANT, so the ids can move (a mod added, removed or
		// renamed) and the items in this save still come back as themselves.
		const std::string ids = SAMItems::saveIdTable();
		if ( !ids.empty() ) { out.push_back(std::make_pair(std::string(kItemIdsKey), ids)); }

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
		s_savedModSet.clear();
		s_haveSavedModSet = false;

		// A save is authoritative for its own character: whatever we were holding from a
		// previous run must not survive into this one.
		s_data.clear();
	}

	bool absorb(const std::string& key, const std::string& value)
	{
		if ( key == kModSetKey )
		{
			s_savedModSet = value;
			s_haveSavedModSet = true;
			return true;
		}
		if ( key == kItemIdsKey ) { return true; } // consumed earlier, at file-read time
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

	void warnIfModSetChanged()
	{
		const std::string now = SAMSync::generateFingerprint();
		if ( !s_haveSavedModSet )
		{
			// A save from before this existed, or a vanilla one. If mods are loaded now, the
			// ids in it were allocated under the old load-order scheme and we cannot know
			// what they were -- say so once rather than pretend.
			if ( !now.empty() )
			{
				SAM_WARN("WORLD", "This save does not record which mods it was made with"
					" (it predates that being written down). If any custom items in it look"
					" wrong, that is why -- re-saving now will record the current set.");
			}
			return;
		}
		if ( s_savedModSet == now ) { return; }

		// Same mods at the same versions, only the FILES differ (or the save predates
		// digests altogether, in which case there is nothing to compare). That is not a
		// different mod set, so do not call it one.
		if ( SAMSync::stripDigests(s_savedModSet) == SAMSync::stripDigests(now) )
		{
			if ( s_savedModSet.find('+') == std::string::npos ) { return; }
			SAM_WARN("WORLD", "Same mods as this save, but some of their files have changed since"
				" it was written. Usually harmless; if a custom class, race, spell or effect"
				" looks different, that is why.");
			return;
		}

		// A different set means different ids for classes, races, spells and effects (custom
		// ITEMS are re-matched by name on load, see the item lines above this one). Name both
		// sides: the player can usually fix it by re-enabling something.
		SAM_WARN("WORLD", "This save was made with a different set of mods.");
		SAM_WARN("WORLD", "  saved with: " + (s_savedModSet.empty() ? std::string("(none)") : SAMSync::stripDigests(s_savedModSet)));
		SAM_WARN("WORLD", "  loaded now: " + (now.empty() ? std::string("(none)") : SAMSync::stripDigests(now)));
		SAM_WARN("WORLD", "  Custom classes, races, spells and effects in this save may not be"
			" the ones they were. Re-enable the missing mods to restore them.");
	}

	void clearAll()
	{
		s_data.clear();
		s_savedModSet.clear();
		s_haveSavedModSet = false;
	}
}
