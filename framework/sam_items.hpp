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

// Framework-owned items live ABOVE every mod id, at fixed numbers that never move.
// See the NUM_ITEM_SLOTS comment in items.hpp for why load-order ids would corrupt saves.
static const int SAM_BUILTIN_ITEM_ID_BASE = 6000;
static const int SAM_ITEM_HUNTERS_WORKBENCH = 6000;

// One parsed item JSON (mirrors item.schema.json).
struct SAMItemDef
{
	std::string id;                 // "namespace:item"
	int numericId = 0;              // assigned runtime slot (>= SAM_ITEM_ID_BASE)
	std::string modNamespace;
	std::string modPath;            // absolute mod folder (used to resolve the icon PNG)

	std::string nameIdentified;
	std::string nameUnidentified;
	std::string description;        // hover-tooltip body (injected into ItemTooltips)
	std::string category;           // "WEAPON" etc. (Barony Category enum name)
	std::string slot = "NO_EQUIP";  // ItemEquippableSlot enum name
	int weight = 0;
	int goldValue = 0;
	int level = -1;                 // -1 = excluded from random generation

	// Mod-supplied .vox models. The path is the mod-relative one the modder wrote, which
	// IS its PhysFS logical path (a mod folder is mounted at the root), e.g.
	// "models/mymod/sword.vox". Registered + resolved to an engine model index by
	// registerModModels(); overrides modelFromItem when both are set.
	std::string model;              // world/held model
	std::string modelFp;            // optional separate first-person model
	// v2.5 state models. Keys: broken, cursed, blessed, unidentified. Each optional; an
	// undeclared state falls through to `model` / `modelFp`. Resolved to engine indices by
	// registerModModels, like every other model reference.
	std::map<std::string, std::string> modelStates;
	std::map<std::string, std::string> modelFpStates;
	std::map<std::string, int> modelStateIdx;
	std::map<std::string, int> modelFpStateIdx;
	std::string modelFromItem;      // vanilla ItemType name (e.g. "SILVER_SHIELD") to clone the 3D model from
	std::string icon;               // mod-relative PNG path — loaded into the inventory icon

	// Which weapon skill this item trains and scales off: "sword", "axe", "mace",
	// "polearm", "ranged", "thrown" or "unarmed". Empty = "sword" for an equippable
	// weapon, which is what every custom weapon silently behaved as before this existed.
	// Barony decides a weapon's skill by comparing the item TYPE against a hardcoded list
	// of vanilla types, so a custom id matches nothing and the engine answers -1: no skill
	// XP, the UNARMED damage table, no damage variance, no durability scaling. Declaring
	// it here is what makes a custom axe actually an axe.
	std::string weaponSkill;

	// Engine traits this item opts into: "ranged", "quiver", "foci", "instrument",
	// "thrown_ball", "shield_slot", "potion_bad", "automaton_food", "tinker_throwable".
	// Barony decides each of those by checking the item against a hardcoded list of vanilla
	// types, which a custom item can never be on. Naming one here puts the item on the list.
	std::vector<std::string> traits;

	// Per-kit crafting-panel skin. Only meaningful on an item used as a custom tinkering
	// kit. Maps a panel ROLE ("base", "drawer", "cost_backing", ...) to a mod-relative PNG.
	// Every role is optional; a role the mod does not supply keeps the vanilla art, so a
	// partial skin is legal and a kit with no entry at all looks exactly like vanilla.
	std::map<std::string, std::string> kitUi;

	std::map<std::string, int> attributes;
	// Spell-payload attributes (spellbook_spell / foci_spell / tome_spell / magicstaff_spell)
	// whose JSON value was a STRING -- a vanilla spell name ("SPELL_FIREBALL") or a custom
	// "namespace:spell". They cannot be resolved at parse time because custom spells load
	// AFTER items, so they are stashed here and turned into numeric ids by
	// SAMItems::resolveSpellAttributes() once every spell is registered.
	std::map<std::string, std::string> spellAttrPending;
	std::string onHitEffect;
	double onHitChance = 0.0;
	bool stackable = false;
	int magicLevel = 0;
};

// v0.7.0 Feature 5: a runtime override of an existing item slot's base fields. Only
// the has-flagged fields are written; `attributes` are MERGED (existing vanilla keys
// like ATK/AC survive). Originals are snapshotted on first patch and restored on unload.
struct SAMItemPatch
{
	bool hasWeight = false;   int weight = 0;
	bool hasValue = false;    int value = 0;
	bool hasLevel = false;    int level = 0;
	bool hasCategory = false; std::string category;   // Category enum name
	bool hasSlot = false;     std::string slot;       // ItemEquippableSlot enum name
	bool hasTooltip = false;  std::string tooltip;
	bool hasNameId = false;   std::string nameIdentified;
	bool hasNameUnid = false; std::string nameUnidentified;
	std::map<std::string, int> attributes;            // merged into items[id].attributes
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

	// Save-file identity. Numeric ids are handed out per mod SET (sorted by namespace,
	// then declaration order), so a save has to remember what each id MEANT.
	//   saveIdTable()       -> "5000=ns:a;5001=ns:b;..." for every registered custom item
	//                          (written into the save's additional_data as "sam_itemids").
	//   remapSavedItemIds() -> for that table, savedId -> the id the same NAME has now
	//                          (oldToNew), and savedId -> name for the ones whose mod is
	//                          not loaded (unresolved). Returns false when the table is
	//                          empty (a save from before this existed): touch nothing.
	static std::string saveIdTable();
	static bool remapSavedItemIds(const std::string& savedTable,
		std::map<int, int>& oldToNew, std::map<int, std::string>& unresolved);

	// Custom item ids eligible for RANDOM GENERATION in `category` at a dungeon depth
	// between minLevel and maxLevel, appended to `out`. This is how modded items reach
	// chests, shops, floor drops and monster inventories.
	//
	// The engine cannot simply widen its own loop to cover the custom id band: those slots
	// are zero-initialised, so an UNREGISTERED one reads back as category WEAPON at level 0
	// and would pass the filter -- flooding every weapon roll with phantom items even with
	// no mod loaded. Asking the registry means only real, declared items are ever offered,
	// and with no mod the answer is always "none", so the vanilla RNG stream is untouched.
	//
	// An item opts OUT with "level": -1, which is exactly what that field has always meant.
	static void lootCandidates(int category, int minLevel, int maxLevel, std::vector<int>& out);

	// v0.7.0 Feature 5: override an existing item slot's base fields (vanilla or custom).
	// Snapshots the slot's originals on the first patch; reverted by clear() on unload.
	// Returns false if id is out of range [0, NUM_ITEM_SLOTS).
	static bool patchItem(int id, const SAMItemPatch& patch);

	// Absolute, Image::get-ready path to a custom item's inventory icon PNG, or ""
	// if the slot isn't a registered custom item / has no icon. The inventory
	// renderer calls this for type >= SAM_ITEM_ID_BASE so a custom slot serves its
	// own icon directly, independent of the vanilla images[]/appearance indexing.
	static std::string getIconPath(int itemId);

	// The model a custom item should draw given its state, or -1 to use its ordinary one.
	// Called from itemModel / itemModelFirstperson, which both hold the Item, so this needs
	// no new plumbing and nothing new on the wire: status, beatitude and identified are all
	// already saved and networked.
	static int stateModelFor(int itemType, int status, int beatitude, bool identified, bool firstPerson);

	// Absolute path to this kit's art for one panel role, or "" when the item declares no
	// skin, omits that role, or the file is missing. The caller falls back to vanilla art.
	static std::string getKitUiPath(int itemId, const std::string& role);

	// The proficiency (PRO_SWORD / PRO_AXE / ...) this custom weapon uses, or -1 when the
	// id is not a registered custom item. The engine asks this from getWeaponSkill.
	static int weaponSkillFor(int itemId);

	// Write a def at a FIXED id, outside the load-order allocator. Only the framework's
	// own built-ins use this; a mod can never reach the reserved band.
	static bool registerBuiltinAt(int id, SAMItemDef def);

	// Load every custom .vox a registered item asked for via its "model"/"model_fp"
	// field, and point the item at it. Must be called from Mods::loadMods AFTER the
	// engine's own model-replacement pass — see sam_models.hpp for why the ordering
	// matters and why growing the model tables is only safe at that exact point.
	// No-op when no item ships a model.
	static void registerModModels();

	// Re-stamp custom item slots and re-inject their tooltips after a vanilla data
	// reload. initGameDatafiles' readTooltipsFromFile clears the whole tooltip map,
	// wiping our "tooltip_sam_*" entries; call this right after those reads so custom
	// items keep their ATK / weight / value rows and real hover tooltip. Idempotent.
	static void reapplyAfterDataReload();

	// Resolve string-valued spell-payload attributes (spellbook_spell / foci_spell /
	// tome_spell / magicstaff_spell) into numeric spell ids, now that every custom spell
	// is registered. A value with ':' is a custom "namespace:spell"; anything else is a
	// vanilla spell name ("SPELL_FIREBALL"). Writes the id into BOTH the registry def and
	// the live items[] slot, so it survives a later data reload. Call from the loader after
	// SAMSpells::loadFromManifest has run for every mod. No-op when nothing is pending.
	static void resolveSpellAttributes();

	// Name of a Category enum value ("WEAPON", "ARMOR", "GEM", ...), or "" if unknown.
	// Reverse of the internal categoryFromName; lets scripts read an item's category.
	static std::string categoryName(int category);
};
