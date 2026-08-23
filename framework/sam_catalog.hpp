/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	sam_catalog.hpp - read the game's own content registries.

	WHY THIS EXISTS. A script could ask about the item in your hand and nothing else. There
	was no way to ask "what items exist", so the entire class of mod that INDEXES the game -
	a recipe browser, a bestiary, a drop-table reference, a boss checklist - was impossible
	to write, no matter how good the UI was.

	That class is not a niche. A searchable item index is the most-downloaded Minecraft mod
	ever made, and Terraria's equivalent has 85% of the install base of the biggest content
	mod for that game. See SAM_MODDING_FREEDOM_STUDY.md.

	WHAT IT READS. The engine's live tables, not a copy: items[] (ItemGeneric), the monster
	type list, and the spell registry. Anything a mod added through S.A.M is in those tables
	by the time this runs, so a catalogue built from it includes modded content automatically -
	which is the whole reason the Minecraft equivalent became a dependency for everyone.

	READ ONLY. Nothing here mutates game state, so none of it is host-gated: a client can
	browse a catalogue perfectly safely.

	Game build only.

-------------------------------------------------------------------------------*/

#pragma once

#include <map>
#include <string>
#include <vector>

namespace SAMCatalog
{
	struct ItemEntry
	{
		int type = -1;
		std::string name;        // identified name
		std::string unidName;    // what it looks like before appraisal
		std::string category;    // WEAPON, ARMOR, POTION, ...
		int level = -1;          // generation depth; -1 = never generated as loot
		int weight = 0;
		int value = 0;
		bool custom = false;     // added by a mod rather than the base game
	};

	// Every item the game knows about, base and modded. Pass a category to filter
	// ("WEAPON", "POTION", ...), or "" for all of them.
	//
	// This walks the whole item table, so a script should call it once and keep the result
	// rather than calling it per frame.
	std::vector<ItemEntry> items(const std::string& categoryFilter);

	// One item in full, including its attribute map (the numbers a tooltip is built from).
	bool itemInfo(int type, ItemEntry& out, std::map<std::string, int>& attributes);

	// Resolve a name or a "ns:id" to an item type, so a browser can look up what the player
	// typed. Returns -1 if nothing matches.
	int itemTypeFor(const std::string& nameOrId);

	struct MonsterEntry
	{
		int type = -1;
		std::string name;
		bool custom = false;
	};
	std::vector<MonsterEntry> monsters();

	struct SpellEntry
	{
		int id = -1;
		std::string name;
		int cost = 0;
		bool custom = false;
	};
	std::vector<SpellEntry> spells();
}
