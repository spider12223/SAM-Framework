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

	// ---- custom crafting materials ----------------------------------------------
	// A recipe may cost the modder's OWN items instead of metal/magic scrap. The
	// crafting panel has room for exactly two material columns, so a recipe may name
	// up to two. Returns false when this item uses the ordinary scrap costs, in which
	// case the caller keeps its existing metal/magic behaviour untouched.
	//   typeA/typeB: resolved ItemTypes (typeB is -1 when only one material is used)
	//   cntA/cntB:   how many of each a craft consumes
	bool materialsFor(int itemType, int& typeA, int& cntA, int& typeB, int& cntB);

	// ---- custom crafting kits ---------------------------------------------------
	// A recipe may name a "kit": an item that opens its OWN crafting grid instead of
	// adding to the vanilla tinkering kit. When a custom kit is open the vanilla
	// recipes are hidden entirely, so that kit gets all 20 cells to itself and never
	// collides with vanilla crafting.
	//
	// True if this item type is used as a kit by at least one registered recipe. The
	// engine asks this to decide whether "use item" should open the crafting screen.
	bool isCustomKit(int itemType);
	// Register a kit that exists whether or not any recipe names it. The framework's own
	// built-in bench uses this: it must open (and wear its skin) from the moment a mod puts
	// it in a class loadout, even before that mod adds a single recipe to it.
	void registerBuiltinKit(int itemType);
	// Tell the registry which kit the player has open, so cost / skill / material
	// lookups answer for THAT bench. The same item may be craftable at several kits
	// for different prices, so these are keyed by (kit, item), not item alone.
	// -1 = the ordinary tinkering kit.
	void setActiveKit(int kitItemType);
	// The kit currently open, or -1 for the ordinary tinkering kit. The panel skin reads
	// this every frame to decide whose art to draw.
	int activeKit();
	// The resolved kit ItemType recipe `index` is bound to, or -1 when it belongs to
	// the ordinary tinkering kit.
	int kitForIndex(int index);

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
