/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_items.hpp
	Desc: runtime custom item/weapon registry.

	Custom items are registered into Barony's global `items[]` table at reserved
	slots starting at SAM_ITEM_ID_BASE (5000). `items[]` is sized to NUM_ITEM_SLOTS
	(see items.hpp) so these slots exist; `NUMITEMS` (the built-in item count) is
	left unchanged, so vanilla loops and random item generation never touch the
	custom range. A custom item's `type` is simply its 5000+ slot, so every vanilla
	`items[item->type]` access resolves correctly once it's registered.

	Custom inventory ICONS are loaded from the mod folder at runtime: the icon PNG's
	absolute path is written into items[id].images, which the inventory UI draws via
	Image::get — the same path class portraits use. Custom 3D MODELS are not loaded
	yet; a custom item borrows a category-appropriate vanilla item's model as a
	placeholder, so it renders safely if ever spawned.

	Unlike the class loader, this whole file compiles into both the game and the
	editor — it only touches `items[]` and list helpers, which exist in both.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>
#include <map>

struct SAMModManifest;  // from sam_workshop.hpp (full type only needed in the .cpp)

// Custom item ids occupy [5000, NUM_ITEM_SLOTS). Chosen well above NUMITEMS.
static const int SAM_ITEM_ID_BASE = 5000;

// One parsed item JSON (mirrors item.schema.json).
struct SAMItemDef
{
	std::string id;                 // "namespace:item"
	int numericId = 0;              // assigned runtime slot (>= SAM_ITEM_ID_BASE)
	std::string modNamespace;
	std::string modPath;            // absolute mod folder (used to resolve the icon PNG)

	std::string nameIdentified;
	std::string nameUnidentified;
	std::string category;           // "WEAPON" etc. (Barony Category enum name)
	std::string slot = "NO_EQUIP";  // ItemEquippableSlot enum name
	int weight = 0;
	int goldValue = 0;
	int level = -1;                 // -1 = excluded from random generation

	std::string model;              // path (PLANNED — not loaded yet, placeholder used)
	std::string modelFp;            // path (PLANNED — not loaded yet)
	std::string icon;               // mod-relative PNG path — loaded into the inventory icon

	std::map<std::string, int> attributes;
	std::string onHitEffect;
	double onHitChance = 0.0;
	bool stackable = false;
	int magicLevel = 0;
};

class SAMItems
{
public:
	// Read + register every item JSON declared in a mod manifest, into items[]
	// starting at SAM_ITEM_ID_BASE. Additive across manifests within one load
	// cycle; call clear() first each cycle.
	static void loadFromManifest(const SAMModManifest& manifest);

	// Free the image lists we allocated for custom slots, wipe the registry, and
	// reset the id counter to SAM_ITEM_ID_BASE.
	static void clear();

	// Number of custom items currently registered.
	static int count();

	// Look up a registered item by its runtime slot id (>= 5000). null if none.
	static const SAMItemDef* getItem(int itemId);

	// Reverse lookup: runtime slot id for a "namespace:item" id string, or -1.
	static int itemIdForIdString(const std::string& idString);
};
