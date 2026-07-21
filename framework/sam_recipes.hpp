/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_recipes.hpp
	Desc: runtime tinkering-recipe registry (add / re-cost / hide craftable items).

	Mods declare recipes in mod.json's "recipes" array. Each recipe JSON is either
	  { item, metal_cost, magic_cost, skill_level, status?, slot? }   -- add or re-cost
	  { remove: "<ITEMTYPE or ns:item>" }                             -- hide a vanilla entry
	`item` is a vanilla ItemType name ("TOOL_FIRE_BOMB") or a custom "namespace:item".

	The engine reads this registry from exactly three places, all of which are plain
	per-ItemType switches with a default: fallthrough, so an EMPTY registry is a
	bit-for-bit no-op and the vanilla craftable grid is byte-identical:
	  - GenericGUIMenu::tinkeringCreateCraftableItemList  (append + suppress)
	  - GenericGUIMenu::tinkeringGetCraftingCost          (scrap cost)
	  - GenericGUIMenu::tinkeringPlayerHasSkillLVLToCraft (skill requirement)

	IMPORTANT — LAZY ITEM RESOLUTION. A recipe stores its `item` as the raw STRING and
	only resolves it to a runtime ItemType when the craftable list is built (i.e. when
	the kit is opened). Resolving at manifest-load time would return -1 for any recipe
	pointing at an item registered by a mod later in the same load loop.

	Tinkering is entirely client-local in Barony (no crafting packets), so nothing here
	crosses the wire. Custom-item OUTPUT still requires mod parity across a lobby, the
	same as any other S.A.M custom item.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

struct SAMModManifest;  // from sam_workshop.hpp (full type only needed in the .cpp)

namespace SAMRecipes
{
	// Parse + register every recipe JSON declared in a mod manifest. Additive across
	// manifests within one load cycle; call clear() first each cycle.
	void loadFromManifest(const SAMModManifest& manifest);

	// Drop every registered recipe + suppression (called on mod (un)load).
	void clear();

	// True iff at least one recipe or suppression is registered. Every engine hook
	// early-outs on this, so vanilla stays on its exact original code path.
	bool any();

	// ---- engine read side -------------------------------------------------------
	// All of these answer false (and leave their out-params untouched) when nothing
	// is registered for that item, so the caller falls through to vanilla behaviour.

	// A mod asked to hide the vanilla CRAFT entry for this item type. (Salvage and
	// repair are separate systems and are deliberately left alone.)
	bool isVanillaSuppressed(int itemType);

	// Scrap cost for this item. Covers both a brand-new custom recipe and a re-costed
	// vanilla one. metal/magic are only written when this returns true.
	bool costFor(int itemType, int& metal, int& magic);

	// Required tinkering skill for this item, as a raw 0..100 value (vanilla buckets
	// its own recipes into 6 tiers of 20; a mod may use any number in between).
	bool skillFor(int itemType, int& required);

	// ---- craftable-list build side ----------------------------------------------
	// Number of ADD recipes registered (suppressions are not counted here).
	int count();

	// Resolve the add-recipe at index 0..count()-1 into a concrete craftable entry.
	// Returns false if its item cannot be resolved (unknown vanilla name, or a custom
	// id whose mod is not loaded) — the caller should skip it. `status` is a Status
	// enum value. `hasSlot` is true when the mod pinned an explicit grid cell.
	bool entryAtIndex(int index, int& itemType, int& status, int& slotX, int& slotY, bool& hasSlot);

	// The "namespace:item" (or vanilla name) string at index, for logging. "" if OOR.
	std::string itemIdAtIndex(int index);
}
