/*-------------------------------------------------------------------------------
	S.A.M Framework - reading the game's content registries. See sam_catalog.hpp.
-------------------------------------------------------------------------------*/

#include "sam_catalog.hpp"
#include <cstring>
#include "sam_items.hpp"
#include "sam_spells.hpp"  // a custom spell reports its DECLARED "ns:spell" id
#include "sam_logger.hpp"

#include <cctype>

#ifndef EDITOR
#	include "main.hpp"
#	include "items.hpp"      // items[], ItemGeneric, NUM_ITEM_SLOTS, ItemTooltips
#	include "monster.hpp"    // monstertypename, NUMMONSTERS
#	include "mod_tools.hpp"   // ItemTooltips: the engine name -> item id table
#	include "magic/magic.hpp"
#	define SAM_CATALOG_HAVE_BARONY 1
#endif

namespace
{
#ifdef SAM_CATALOG_HAVE_BARONY
	// Anything at or above this id was registered by a mod through S.A.M, not shipped with
	// the game. A browser wants to say which is which.
	bool isCustomItem(int type)
	{
		return ( type >= SAM_ITEM_ID_BASE );
	}

	std::string lower(std::string s)
	{
		for ( char& c : s ) { c = (char)std::tolower((unsigned char)c); }
		return s;
	}
#endif
}

std::vector<SAMCatalog::ItemEntry> SAMCatalog::items(const std::string& categoryFilter)
{
	std::vector<ItemEntry> out;
#ifdef SAM_CATALOG_HAVE_BARONY
	const std::string want = lower(categoryFilter);
	out.reserve(256);
	for ( int t = 0; t < NUM_ITEM_SLOTS; ++t )
	{
		const ItemGeneric& g = ::items[t];
		const char* nm = g.getIdentifiedName();
		// The table is sized to the highest reserved id, so most slots are empty. An entry
		// with no name is a hole, not an item.
		if ( !nm || nm[0] == '\0' ) { continue; }

		const std::string cat = SAMItems::categoryName((int)g.category);
		if ( !want.empty() && lower(cat) != want ) { continue; }

		ItemEntry e;
		e.type = t;
		e.name = nm;
		e.unidName = g.getUnidentifiedName() ? g.getUnidentifiedName() : "";
		e.category = cat;
		e.level = g.level;
		e.weight = g.weight;
		e.value = g.gold_value;
		e.custom = isCustomItem(t);
		out.push_back(e);
	}
#else
	(void)categoryFilter;
#endif
	return out;
}

bool SAMCatalog::itemInfo(int type, ItemEntry& out, std::map<std::string, int>& attributes)
{
	attributes.clear();
#ifndef SAM_CATALOG_HAVE_BARONY
	(void)type; (void)out;
	return false;
#else
	if ( type < 0 || type >= NUM_ITEM_SLOTS ) { return false; }
	const ItemGeneric& g = ::items[type];
	const char* nm = g.getIdentifiedName();
	if ( !nm || nm[0] == '\0' ) { return false; }

	out = ItemEntry{};
	out.type = type;
	out.name = nm;
	out.unidName = g.getUnidentifiedName() ? g.getUnidentifiedName() : "";
	out.category = SAMItems::categoryName((int)g.category);
	out.level = g.level;
	out.weight = g.weight;
	out.value = g.gold_value;
	out.custom = isCustomItem(type);
	for ( const auto& kv : g.attributes ) { attributes[kv.first] = (int)kv.second; }
	return true;
#endif
}

int SAMCatalog::itemTypeFor(const std::string& nameOrId)
{
#ifndef SAM_CATALOG_HAVE_BARONY
	(void)nameOrId;
	return -1;
#else
	if ( nameOrId.empty() ) { return -1; }
	// An all-digits string is an item TYPE that arrived as text. Lua accepted this already,
	// because lua_isnumber treats "42" as a number, while QuickJS's JS_IsNumber does not --
	// so the same script returned an item in Lua and null in JS. Resolve it here, in the
	// shared helper, so both runtimes agree.
	{
		bool digits = true;
		for ( char c : nameOrId ) { if ( c < '0' || c > '9' ) { digits = false; break; } }
		if ( digits )
		{
			const long t = strtol(nameOrId.c_str(), nullptr, 10);
			// In range is not the same as real: the item table has empty slots, and both
			// items() and itemInfo() reject them. Returning one here would hand back a
			// "valid" type that every other call then refuses.
			if ( t >= 0 && t < NUM_ITEM_SLOTS )
			{
				const char* nm = ::items[t].getIdentifiedName();
				if ( nm && nm[0] ) { return (int)t; }
			}
			return -1;
		}
	}
	// A "ns:id" is a mod's own item; ask the registry first.
	if ( nameOrId.find(':') != std::string::npos )
	{
		const int t = SAMItems::itemIdForIdString(nameOrId);
		if ( t >= 0 ) { return t; }
	}
	// Then the engine's own name table, which is keyed lowercase.
	auto it = ItemTooltips.itemNameStringToItemID.find(lower(nameOrId));
	if ( it != ItemTooltips.itemNameStringToItemID.end() ) { return it->second; }
	// Last resort: match a display name exactly, so a browser can round-trip what it showed.
	//
	// ALL matches, not the first one. "tablet" is the identified name of three different tomes
	// (sorcery, mysticism, thaumaturgy), and returning the first meant sam_get_item_info("tablet")
	// quietly answered sorcery. Picking one silently is the failure that hides for months; naming
	// the candidates costs the author a minute and tells them exactly what to write instead.
	//
	// Safe to have this tier at all, checked against the shipped data rather than assumed: no
	// display name collides with a DIFFERENT item's internal key, so placing it last can never
	// hijack a resolution that already worked. It must never be widened to the UNIDENTIFIED name,
	// where "spellbook" alone is shared by 127 items.
	{
		std::vector<int> hits;
		const std::string want = lower(nameOrId);
		for ( int t = 0; t < NUM_ITEM_SLOTS; ++t )
		{
			const char* nm = ::items[t].getIdentifiedName();
			if ( nm && nm[0] && lower(nm) == want ) { hits.push_back(t); }
		}
		if ( hits.size() == 1 ) { return hits[0]; }
		if ( hits.size() > 1 )
		{
			std::string names;
			for ( size_t i = 0; i < hits.size(); ++i )
			{
				if ( !names.empty() ) { names += ", "; }
				const char* internal = ( hits[i] >= 0 && hits[i] < NUMITEMS )
					? itemNameStrings[hits[i] + 2] : "?";
				names += internal;
			}
			SAM_ERROR("CATALOG", "'" + nameOrId + "' is a displayed name shared by "
				+ std::to_string((int)hits.size()) + " items: " + names
				+ ". Refusing to guess -- pass one of those names, or the numeric type.");
		}
	}
	return -1;
#endif
}

std::vector<SAMCatalog::MonsterEntry> SAMCatalog::monsters()
{
	std::vector<MonsterEntry> out;
#ifdef SAM_CATALOG_HAVE_BARONY
	for ( int t = 0; t < NUMMONSTERS; ++t )
	{
		const char* nm = monstertypename[t];
		if ( !nm || nm[0] == '\0' ) { continue; }
		// Index 0 is the NOTHING sentinel and the tail of the table is reserved padding
		// ("monster_unused_6/7/8"). Neither is a creature, and handing them to a script
		// puts "nothing" at the top of every bestiary a mod builds. items() already filters
		// its own placeholders; this is the same courtesy.
		if ( t == 0 ) { continue; }
		if ( strncmp(nm, "monster_unused", 14) == 0 ) { continue; }
		MonsterEntry e;
		e.type = t;
		e.name = nm;
		// Custom monsters are variants of a base species rather than new entries in this
		// table, so nothing here is "custom"; the flag is reserved so the shape does not
		// change if that ever becomes true.
		e.custom = false;
		out.push_back(e);
	}
#endif
	return out;
}

std::vector<SAMCatalog::SpellEntry> SAMCatalog::spells()
{
	std::vector<SpellEntry> out;
#ifdef SAM_CATALOG_HAVE_BARONY
	for ( const auto& kv : allGameSpells )
	{
		spell_t* s = kv.second;
		if ( !s ) { continue; }
		// hide_from_ui marks the engine's internal spells (spawner/foci plumbing). A browser
		// wants the ones a player can actually see and cast.
		if ( s->hide_from_ui ) { continue; }
		SpellEntry e;
		e.id = s->ID;
		// A CUSTOM spell reports the id its mod declared ("mymod:frostlance"), not the mangled
		// internal name S.A.M generated for the engine ("spell_sam_mymod_frostlance"). Both
		// resolve on the way back in, but only one of them is a string the mod ever wrote, so
		// comparing an event against your own declaration used to be impossible.
		{
			const SAMSpellDef* samDef = SAMSpells::getSpell(s->ID);
			e.name = ( samDef && !samDef->id.empty() ) ? samDef->id : std::string(s->spell_internal_name);
		}
		e.cost = getCostOfSpell(s);
		e.custom = false;
		out.push_back(e);
	}
#endif
	return out;
}
