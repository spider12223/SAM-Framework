/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_items.cpp
	Desc: implementation of the custom item/weapon registry.

	Registers each custom item into Barony's `items[]` at a reserved slot (>= 5000).
	No `#ifndef EDITOR` needed: everything here touches only `items[]` + the list
	helpers, which exist in both the game and editor builds.

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros that
// would break nlohmann/std. Must precede every include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_items.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "sam_models.hpp" // custom .vox registration (appendModels / modelIndexForId)
#include "nlohmann/json.hpp"

#include "main.hpp"    // list_t/string_t, stringCopy, stringDeconstructor, list_* helpers
#include "items.hpp"   // items[], ItemGeneric, ItemType, Category, ItemEquippableSlot, NUM_ITEM_SLOTS
#ifndef EDITOR
#include "mod_tools.hpp" // ItemTooltips.itemNameStringToItemID — resolves "model_from_item" names
#endif

#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <set>

using nlohmann::json;

static const char* MOD = "ITEMS";

static std::map<int, SAMItemDef> s_registry;
static int s_nextItemId = SAM_ITEM_ID_BASE;

// v0.7.0 Feature 5: sam_patch_item snapshots. items[id] is a live, persistent table
// (unlike class/monster stats, which are recomputed each construction), so a patch
// must capture the slot's originals and restore them on unload. Keyed by slot id;
// captured insert-if-absent so repeated patches never overwrite the true original.
struct SAMItemSaved
{
	int weight = 0, gold_value = 0, level = 0;
	Category category = WEAPON;
	ItemEquippableSlot item_slot = NO_EQUIP;
	std::string tooltip, nameId, nameUnid;
	std::map<std::string, Sint32> attributes;
};
static std::map<int, SAMItemSaved> s_itemPatches;

/*-------------------------------------------------------------------------------
	Local helpers
-------------------------------------------------------------------------------*/
static bool readWholeFile(const std::string& path, std::string& out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if ( !f.is_open() )
	{
		return false;
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

static std::string joinPath(const std::string& dir, const std::string& file)
{
	if ( dir.empty() )
	{
		return file;
	}
	const char back = dir.back();
	return (back == '/' || back == '\\') ? (dir + file) : (dir + "/" + file);
}

static std::string toForwardSlashes(std::string s)
{
	for ( char& c : s ) { if ( c == '\\' ) { c = '/'; } }
	return s;
}

static bool fileExists(const std::string& p)
{
	std::ifstream f(p.c_str(), std::ios::binary);
	return f.good();
}

// Valid enum-name lists (for validation + "did you mean?" suggestions). Kept in
// step with categoryFromName/slotFromName below and with item.schema.json.
static const std::vector<std::string>& validCategoryNames()
{
	static const std::vector<std::string> v = {
		"WEAPON", "ARMOR", "AMULET", "POTION", "SCROLL", "MAGICSTAFF", "RING",
		"SPELLBOOK", "GEM", "THROWN", "TOOL", "FOOD", "BOOK", "SPELL_CAT", "TOME_SPELL"
	};
	return v;
}
static const std::vector<std::string>& validSlotNames()
{
	static const std::vector<std::string> v = {
		"EQUIPPABLE_IN_SLOT_WEAPON", "EQUIPPABLE_IN_SLOT_SHIELD", "EQUIPPABLE_IN_SLOT_MASK",
		"EQUIPPABLE_IN_SLOT_HELM", "EQUIPPABLE_IN_SLOT_GLOVES", "EQUIPPABLE_IN_SLOT_BOOTS",
		"EQUIPPABLE_IN_SLOT_BREASTPLATE", "EQUIPPABLE_IN_SLOT_CLOAK", "EQUIPPABLE_IN_SLOT_AMULET",
		"EQUIPPABLE_IN_SLOT_RING", "NO_EQUIP"
	};
	return v;
}
static bool listContains(const std::vector<std::string>& v, const std::string& s)
{
	for ( const std::string& x : v ) { if ( x == s ) { return true; } }
	return false;
}

static Category categoryFromName(const std::string& n)
{
	if ( n == "WEAPON" ) { return WEAPON; }
	if ( n == "ARMOR" ) { return ARMOR; }
	if ( n == "AMULET" ) { return AMULET; }
	if ( n == "POTION" ) { return POTION; }
	if ( n == "SCROLL" ) { return SCROLL; }
	if ( n == "MAGICSTAFF" ) { return MAGICSTAFF; }
	if ( n == "RING" ) { return RING; }
	if ( n == "SPELLBOOK" ) { return SPELLBOOK; }
	if ( n == "GEM" ) { return GEM; }
	if ( n == "THROWN" ) { return THROWN; }
	if ( n == "TOOL" ) { return TOOL; }
	if ( n == "FOOD" ) { return FOOD; }
	if ( n == "BOOK" ) { return BOOK; }
	if ( n == "SPELL_CAT" ) { return SPELL_CAT; }
	if ( n == "TOME_SPELL" ) { return TOME_SPELL; }
	return WEAPON; // fallback
}

// Reverse of categoryFromName: Category enum value -> name string. Exposed via SAMItems so
// scripts (sam_get_item_category) can read an item's category, e.g. "reward identifying a GEM".
std::string SAMItems::categoryName(int category)
{
	switch ( (Category)category )
	{
		case WEAPON:     return "WEAPON";
		case ARMOR:      return "ARMOR";
		case AMULET:     return "AMULET";
		case POTION:     return "POTION";
		case SCROLL:     return "SCROLL";
		case MAGICSTAFF: return "MAGICSTAFF";
		case RING:       return "RING";
		case SPELLBOOK:  return "SPELLBOOK";
		case GEM:        return "GEM";
		case THROWN:     return "THROWN";
		case TOOL:       return "TOOL";
		case FOOD:       return "FOOD";
		case BOOK:       return "BOOK";
		case SPELL_CAT:  return "SPELL_CAT";
		case TOME_SPELL: return "TOME_SPELL";
		default:         return "";
	}
}

static ItemEquippableSlot slotFromName(const std::string& n)
{
	if ( n == "EQUIPPABLE_IN_SLOT_WEAPON" ) { return EQUIPPABLE_IN_SLOT_WEAPON; }
	if ( n == "EQUIPPABLE_IN_SLOT_SHIELD" ) { return EQUIPPABLE_IN_SLOT_SHIELD; }
	if ( n == "EQUIPPABLE_IN_SLOT_MASK" ) { return EQUIPPABLE_IN_SLOT_MASK; }
	if ( n == "EQUIPPABLE_IN_SLOT_HELM" ) { return EQUIPPABLE_IN_SLOT_HELM; }
	if ( n == "EQUIPPABLE_IN_SLOT_GLOVES" ) { return EQUIPPABLE_IN_SLOT_GLOVES; }
	if ( n == "EQUIPPABLE_IN_SLOT_BOOTS" ) { return EQUIPPABLE_IN_SLOT_BOOTS; }
	if ( n == "EQUIPPABLE_IN_SLOT_BREASTPLATE" ) { return EQUIPPABLE_IN_SLOT_BREASTPLATE; }
	if ( n == "EQUIPPABLE_IN_SLOT_CLOAK" ) { return EQUIPPABLE_IN_SLOT_CLOAK; }
	if ( n == "EQUIPPABLE_IN_SLOT_AMULET" ) { return EQUIPPABLE_IN_SLOT_AMULET; }
	if ( n == "EQUIPPABLE_IN_SLOT_RING" ) { return EQUIPPABLE_IN_SLOT_RING; }
	return NO_EQUIP;
}

// A vanilla item whose model + inventory icon we clone as a placeholder, so a
// custom item renders safely if ever spawned (real custom models are post-launch).
static ItemType templateForCategory(Category cat)
{
	switch ( cat )
	{
		case WEAPON:     return IRON_SWORD;
		case ARMOR:      return LEATHER_BREASTPIECE;
		case AMULET:     return AMULET_POISONRESISTANCE;
		case POTION:     return POTION_WATER;
		case SCROLL:     return SCROLL_BLANK;
		case MAGICSTAFF: return MAGICSTAFF_LIGHT;
		case RING:       return RING_ADORNMENT;
		case SPELLBOOK:  return SPELLBOOK_LIGHT;
		case GEM:        return GEM_ROCK;
		case THROWN:     return BRONZE_TOMAHAWK;
		case TOOL:       return TOOL_TORCH;
		case FOOD:       return FOOD_BREAD;
		case BOOK:       return READABLE_BOOK;
		default:         return IRON_SWORD;
	}
}

// A vanilla item whose model + inventory icon match an EQUIP SLOT, so a custom
// equippable renders as the right KIND of gear. This takes priority over
// templateForCategory for equippable items: the category "ARMOR" covers shields,
// helms, breastplates, boots, gloves and cloaks, so a category-only placeholder
// makes e.g. a shield clone a breastplate model — which renders as a misshapen
// blob ("rock") in the shield hand. Selecting by slot fixes that.
static ItemType templateForSlot(ItemEquippableSlot eslot)
{
	switch ( eslot )
	{
		case EQUIPPABLE_IN_SLOT_WEAPON:      return IRON_SWORD;
		case EQUIPPABLE_IN_SLOT_SHIELD:      return WOODEN_SHIELD;
		case EQUIPPABLE_IN_SLOT_HELM:        return LEATHER_HELM;
		case EQUIPPABLE_IN_SLOT_BREASTPLATE: return LEATHER_BREASTPIECE;
		case EQUIPPABLE_IN_SLOT_GLOVES:      return GLOVES;
		case EQUIPPABLE_IN_SLOT_BOOTS:       return LEATHER_BOOTS;
		case EQUIPPABLE_IN_SLOT_CLOAK:       return CLOAK;
		case EQUIPPABLE_IN_SLOT_MASK:        return TOOL_BLINDFOLD;
		case EQUIPPABLE_IN_SLOT_AMULET:      return AMULET_POISONRESISTANCE;
		case EQUIPPABLE_IN_SLOT_RING:        return RING_ADORNMENT;
		default:                             return IRON_SWORD; // NO_EQUIP: caller uses category
	}
}

#ifndef EDITOR
// Resolve a vanilla ItemType NAME (e.g. "SILVER_SHIELD", case-insensitive) to its
// numeric type via Barony's item-name map, or -1 if unknown. Lets a modder pick the
// exact vanilla 3D model their custom item clones ("model_from_item"). Game-only
// (ItemTooltips is not populated in the editor build).
static int vanillaItemTypeFromName(const std::string& n)
{
	if ( n.empty() ) { return -1; }
	std::string lower = n;
	for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
	auto it = ItemTooltips.itemNameStringToItemID.find(lower);
	if ( it != ItemTooltips.itemNameStringToItemID.end() ) { return it->second; }
	return -1;
}
#endif

// Deep-copy a source item's inventory-image list into dest, allocating fresh
// nodes so the two lists never alias (mirrors ItemTooltips_t::readItemsFromFile).
static void deepCopyImages(list_t& dest, const list_t& src)
{
	list_FreeAll(&dest);
	dest.first = nullptr;
	dest.last = nullptr;
	for ( node_t* node = src.first; node != nullptr; node = node->next )
	{
		string_t* srcStr = static_cast<string_t*>(node->element);
		string_t* s = static_cast<string_t*>(malloc(sizeof(string_t)));
		if ( !s ) { continue; }
		const size_t len = 64;
		s->data = static_cast<char*>(malloc(sizeof(char) * len));
		if ( !s->data ) { free(s); continue; }
		memset(s->data, 0, sizeof(char) * len);
		s->lines = 1;
		node_t* n = list_AddNodeLast(&dest);
		n->element = s;
		n->deconstructor = &stringDeconstructor;
		n->size = sizeof(string_t);
		s->node = n;
		if ( srcStr && srcStr->data )
		{
			stringCopy(s->data, srcStr->data, len - 1, strlen(srcStr->data));
		}
	}
}

// The sanitized "tooltip_sam_<id>" key for a custom item (same derivation everywhere).
static std::string samTooltipKeyFor(const std::string& itemStrId)
{
	std::string ttKey = "tooltip_sam_";
	for ( char c : itemStrId )
	{
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		ttKey += ok ? c : '_';
	}
	return ttKey;
}

#ifndef EDITOR
// Give a custom item a REAL hover tooltip. The key: clone the vanilla CATEGORY template
// (tooltip_sword, tooltip_armor_breastpiece, tooltip_potion_water, ...) — NOT the bare
// "tooltip_default". Only the category template carries the ATK / stat-bonus / durability
// rows and the weight+value footer, so cloning tooltip_default (as we used to) left the
// item with none of its details. We pick the same vanilla item we already clone visuals
// from, so the tooltip layout matches the item's kind. This is called at registration
// AND re-called after any vanilla tooltip reload (readTooltipsFromFile) clears the map.
static void injectCustomTooltip(int id, const SAMItemDef& def)
{
	const Category cat = categoryFromName(def.category);
	const ItemEquippableSlot eslot = slotFromName(def.slot);
	const ItemType tmpl = (eslot != NO_EQUIP) ? templateForSlot(eslot) : templateForCategory(cat);

	std::string srcKey = (tmpl >= 0 && tmpl < NUMITEMS) ? items[tmpl].tooltip : std::string();
	if ( srcKey.empty() || !ItemTooltips.tooltips.count(srcKey) )
	{
		srcKey = "tooltip_default";
	}
	if ( !ItemTooltips.tooltips.count(srcKey) )
	{
		items[id].tooltip = "tooltip_default"; // tooltips not loaded yet; nothing to clone
		return;
	}

	// Copy the fully-resolved category template (its templates are already expanded into
	// concrete lines at parse time), then swap in the item's own description if it has one.
	auto ttCopy = ItemTooltips.tooltips[srcKey];
	if ( !def.description.empty() )
	{
		ttCopy.descriptionText.clear();
		std::istringstream bs(def.description);
		std::string bl;
		while ( std::getline(bs, bl) ) { ttCopy.descriptionText.push_back(bl); }
		if ( ttCopy.descriptionText.empty() ) { ttCopy.descriptionText.push_back(def.description); }
	}
	const std::string ttKey = samTooltipKeyFor(def.id);
	ItemTooltips.tooltips[ttKey] = std::move(ttCopy);
	items[id].tooltip = ttKey;
}
#endif

// Write one parsed def into its reserved items[] slot. Returns false if full.
static bool registerItem(SAMItemDef def)
{
	const int id = s_nextItemId;
	if ( id >= NUM_ITEM_SLOTS )
	{
		SAM_ERROR(MOD, "Item registry full (next id " + std::to_string(id) + " >= NUM_ITEM_SLOTS "
			+ std::to_string(NUM_ITEM_SLOTS) + ") — skipping '" + def.id + "'. Raise NUM_ITEM_SLOTS in items.hpp.");
		return false;
	}
	s_nextItemId = id + 1;
	def.numericId = id;

	const Category cat = categoryFromName(def.category);
	const ItemEquippableSlot eslot = slotFromName(def.slot);
	ItemGeneric& slot = items[id];

	// Placeholder visuals cloned from a vanilla item. For an EQUIPPABLE item pick
	// the template by its equip slot (a shield clones a shield, boots clone boots,
	// ...) so it renders as the right kind of gear; otherwise fall back to a
	// category-appropriate template.
	const ItemType tmpl = (eslot != NO_EQUIP) ? templateForSlot(eslot) : templateForCategory(cat);
	slot.index = items[tmpl].index;
	slot.fpindex = items[tmpl].fpindex;
	slot.indexShort = items[tmpl].indexShort;
	slot.variations = 1;
	deepCopyImages(slot.images, items[tmpl].images);

#ifndef EDITOR
	// Explicit 3D-model source: clone the model (world + first-person + inventory icon)
	// from a NAMED vanilla item, so a modder can pick exactly which vanilla model their
	// custom item wears — e.g. "model_from_item": "SILVER_SHIELD". This overrides the
	// slot/category placeholder above, and is in turn overridden by an explicit "model"
	// .vox (registerModModels runs later and wins). A per-item "icon" PNG (below) still
	// overrides the 2D icon if present.
	if ( !def.modelFromItem.empty() )
	{
		const int src = vanillaItemTypeFromName(def.modelFromItem);
		if ( src >= 0 && src < NUMITEMS )
		{
			slot.index      = items[src].index;
			slot.fpindex    = items[src].fpindex;
			slot.indexShort = items[src].indexShort;
			deepCopyImages(slot.images, items[src].images);
			SAM_INFO(MOD, "Item [" + def.id + "] model_from_item '" + def.modelFromItem
				+ "' -> vanilla type " + std::to_string(src) + " (model index " + std::to_string(items[src].index) + ")");
		}
		else
		{
			SAM_WARN(MOD, "Item [" + def.id + "] model_from_item '" + def.modelFromItem
				+ "' is not a known vanilla ItemType — using the placeholder model.");
		}
	}
#endif

	// Custom inventory icon. The modern inventory UI draws items[type].images[0]'s
	// PATH STRING via Image::get (which resolves an absolute path directly, exactly
	// like the class-select portrait) — it does NOT read items[].surfaces. So point
	// image node 0 at the mod's PNG. deepCopyImages allocates a 64-byte buffer that
	// is too small for an absolute path, so free + re-malloc it to fit. If the file
	// is missing we keep the placeholder (cloned above), never crash.
	// Path-traversal guard: def.icon is untrusted mod JSON, joined onto modPath
	// here AND again in getIconPath(). Drop it if it escapes the mod folder so
	// neither path can open a file outside the mod's own directory.
	if ( !def.icon.empty() && SAMErrors::relPathEscapes(def.icon) )
	{
		SAM_WARN(MOD, "Item [" + def.id + "] icon path '" + def.icon + "' escapes the mod folder — ignoring it.");
		def.icon.clear();
	}
	if ( !def.icon.empty() )
	{
		const std::string abs = toForwardSlashes(joinPath(def.modPath, def.icon));
		if ( fileExists(abs) && slot.images.first )
		{
			string_t* s = static_cast<string_t*>(slot.images.first->element);
			// Allocate BEFORE freeing the old buffer so an OOM keeps the placeholder
			// intact instead of leaving a dangling/null s->data.
			char* buf = static_cast<char*>(malloc(abs.size() + 1));
			if ( buf )
			{
				free(s->data);
				memset(buf, 0, abs.size() + 1);
				// dest_size = full capacity (abs.size()+1); passing abs.size() would
				// make stringCopy overwrite the LAST real char with the terminator.
				stringCopy(buf, abs.c_str(), abs.size() + 1, abs.size());
				s->data = buf;
				SAM_INFO(MOD, "Item [" + def.id + "] custom inventory icon -> " + abs);
			}
			else
			{
				SAM_ERROR(MOD, "Item [" + def.id + "] icon: out of memory — keeping placeholder.");
			}
		}
		else
		{
			SAM_WARN(MOD, "Item [" + def.id + "] icon not found (using placeholder): " + abs);
		}
	}

	// Metadata from the JSON.
	slot.setIdentifiedName(def.nameIdentified);
	slot.setUnidentifiedName(def.nameUnidentified.empty() ? def.nameIdentified : def.nameUnidentified);
	slot.category = cat;
	slot.item_slot = eslot;
	slot.weight = def.weight;
	slot.gold_value = def.goldValue;
	slot.level = def.level;
#ifndef EDITOR
	injectCustomTooltip(id, def);
#else
	slot.tooltip = "tooltip_default";
#endif
	slot.attributes.clear();
	for ( const auto& kv : def.attributes )
	{
		slot.attributes[kv.first] = kv.second;
	}

	s_registry[id] = def;

	SAM_INFO(MOD, "Registering item: " + def.nameIdentified + " [" + def.id + "] -> slot "
		+ std::to_string(id) + " (" + def.category + ")");
	SAM_DEBUG(MOD, "  weight " + std::to_string(def.weight) + ", value " + std::to_string(def.goldValue)
		+ ", level " + std::to_string(def.level) + ", slot " + def.slot + ", "
		+ std::to_string(def.attributes.size()) + " attribute(s); placeholder model cloned from a vanilla item");
	return true;
}

/*-------------------------------------------------------------------------------
	SAMItems
-------------------------------------------------------------------------------*/
void SAMItems::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const std::string& relPath : manifest.items )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "Item file not found: " + path + " (declared by [" + manifest.ns + "])");
			continue;
		}

		const std::string fileLabel = SAMErrors::displayFile(manifest.ns, relPath);
		json j;
		try
		{
			j = json::parse(text);
		}
		catch ( const json::parse_error& e )
		{
			SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "item not loaded.");
			continue;
		}
		if ( !j.is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
				"a JSON object: { ... }", "wrap the file contents in { }", "item not loaded.");
			continue;
		}

		auto getStr = [&](const char* k) -> std::string {
			auto it = j.find(k);
			return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
		};
		auto getInt = [&](const char* k, int dv) -> int {
			auto it = j.find(k);
			return (it != j.end() && it->is_number()) ? it->get<int>() : dv;
		};
		auto getBool = [&](const char* k, bool dv) -> bool {
			auto it = j.find(k);
			return (it != j.end() && it->is_boolean()) ? it->get<bool>() : dv;
		};
		auto getNum = [&](const char* k, double dv) -> double {
			auto it = j.find(k);
			return (it != j.end() && it->is_number()) ? it->get<double>() : dv;
		};

		SAMItemDef def;
		def.id = getStr("id");
		def.nameIdentified = getStr("name_identified");
		def.category = getStr("category");
		def.modNamespace = manifest.ns;
		def.modPath = manifest.modPath;

		if ( def.id.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/id", "", "missing (required)",
				"an id in \"namespace:item\" form, e.g. \"" + manifest.ns + ":shadowblade\"",
				"add an \"id\" field", "item not loaded.");
			continue;
		}
		if ( def.nameIdentified.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/name_identified", "", "missing (required)",
				"the identified name, e.g. \"Shadowblade\"", "add a \"name_identified\" field",
				"item [" + def.id + "] not loaded.");
			continue;
		}
		if ( def.category.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/category", "", "missing (required)",
				"one of the 15 Category names (WEAPON, ARMOR, POTION, ...)", "add a \"category\" field",
				"item [" + def.id + "] not loaded.");
			continue;
		}
		// Category present but not a known enum name → warn + suggest, then fall back.
		if ( !listContains(validCategoryNames(), def.category) )
		{
			const std::string sug = SAMErrors::suggest(def.category, validCategoryNames());
			SAMErrors::reportSemantic(MOD, fileLabel, "/category", def.category, "not a known category",
				"one of the 15 Category names (see item.schema.json)",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
				"item [" + def.id + "] treated as WEAPON.", /*warn=*/true);
		}

		def.nameUnidentified = getStr("name_unidentified");
		def.description = getStr("description");
		def.slot = getStr("slot");
		if ( def.slot.empty() ) { def.slot = "NO_EQUIP"; }
		// Slot present but not a known enum name → warn + suggest, then fall back.
		if ( !listContains(validSlotNames(), def.slot) )
		{
			const std::string sug = SAMErrors::suggest(def.slot, validSlotNames());
			SAMErrors::reportSemantic(MOD, fileLabel, "/slot", def.slot, "not a known equip slot",
				"one of the 11 ItemEquippableSlot names, or omit for NO_EQUIP",
				sug.empty() ? "" : ("did you mean \"" + sug + "\"?"),
				"item [" + def.id + "] slot treated as NO_EQUIP.", /*warn=*/true);
		}
		def.weight = getInt("weight", 0);
		def.goldValue = getInt("gold_value", 0);
		def.level = getInt("level", -1);
		def.model = getStr("model");
		def.modelFp = getStr("model_fp");
		def.modelFromItem = getStr("model_from_item");
		def.icon = getStr("icon");
		def.onHitEffect = getStr("on_hit_effect");
		def.onHitChance = getNum("on_hit_chance", 0.0);
		def.stackable = getBool("stackable", false);
		def.magicLevel = getInt("magic_level", 0);

		if ( j.contains("attributes") && j["attributes"].is_object() )
		{
			for ( auto it = j["attributes"].begin(); it != j["attributes"].end(); ++it )
			{
				if ( it.value().is_number() )
				{
					def.attributes[it.key()] = it.value().get<int>();
					// The engine reads exactly two of these: ATK (items.cpp attackGetAttack)
					// and AC (armorGetAC). Anything else is stored here and never looked at,
					// so an invented key like "EFF_UNBREAKABLE" silently does nothing — and
					// the JSON schema can't catch every case for older mods. Say so.
					if ( it.key() != "ATK" && it.key() != "AC" )
					{
						SAM_WARN(MOD, "Item '" + def.id + "': attribute '" + it.key() + "' is not read by the engine "
							"and will have no effect. Only ATK and AC are used. (Item effects come from a script, "
							"not from an attribute key.)");
					}
				}
			}
		}

		registerItem(def);
	}
}

void SAMItems::clear()
{
	// v0.7.0 Feature 5: revert every sam_patch_item override to its captured original
	// FIRST (restores vanilla items[] fields before any custom-slot teardown below).
	for ( const auto& kv : s_itemPatches )
	{
		const int id = kv.first;
		if ( id >= 0 && id < NUM_ITEM_SLOTS )
		{
			const SAMItemSaved& s = kv.second;
			items[id].weight = s.weight;
			items[id].gold_value = s.gold_value;
			items[id].level = s.level;
			items[id].category = s.category;
			items[id].item_slot = s.item_slot;
			items[id].tooltip = s.tooltip;
			items[id].setIdentifiedName(s.nameId);
			items[id].setUnidentifiedName(s.nameUnid);
			items[id].attributes = s.attributes;
		}
	}
	s_itemPatches.clear();

	// Free the image lists we allocated for custom slots, and reset those slots so
	// nothing lingers or gets picked up by a later lookup.
	for ( const auto& kv : s_registry )
	{
		const int id = kv.first;
		if ( id >= 0 && id < NUM_ITEM_SLOTS )
		{
			list_FreeAll(&items[id].images);
			items[id].images.first = nullptr;
			items[id].images.last = nullptr;
			items[id].level = -1;
			items[id].setIdentifiedName("");
			items[id].setUnidentifiedName("");
			items[id].attributes.clear();
		}
	}
	s_registry.clear();
	s_nextItemId = SAM_ITEM_ID_BASE;
}

// A vanilla data reload (initGameDatafiles) rewrites the built-in item range and, worse,
// readTooltipsFromFile CLEARS the entire tooltip map — wiping every "tooltip_sam_*" we
// injected, which is exactly why custom items lost their ATK / weight / value rows and
// showed "This item does not have a tooltip yet!". Re-stamp our reserved slots and
// re-inject their tooltips from the registry. Idempotent; runs right after the vanilla
// item/tooltip reads in initGameDatafiles.
void SAMItems::reapplyAfterDataReload()
{
#ifndef EDITOR
	for ( const auto& kv : s_registry )
	{
		const int id = kv.first;
		const SAMItemDef& def = kv.second;
		if ( id < 0 || id >= NUM_ITEM_SLOTS ) { continue; }
		ItemGeneric& slot = items[id];
		slot.weight = def.weight;
		slot.gold_value = def.goldValue;
		slot.level = def.level;
		slot.category = categoryFromName(def.category);
		slot.item_slot = slotFromName(def.slot);
		slot.attributes.clear();
		for ( const auto& a : def.attributes ) { slot.attributes[a.first] = a.second; }
		injectCustomTooltip(id, def);
	}
	if ( !s_registry.empty() )
	{
		SAM_INFO(MOD, "Re-applied " + std::to_string(s_registry.size())
			+ " custom item(s) after a vanilla data reload (tooltips restored).");
	}
#endif
}

int SAMItems::count()
{
	return static_cast<int>(s_registry.size());
}

bool SAMItems::patchItem(int id, const SAMItemPatch& p)
{
	if ( id < 0 || id >= NUM_ITEM_SLOTS )
	{
		SAM_ERROR(MOD, "sam_patch_item: item slot out of range: " + std::to_string(id));
		return false;
	}
	ItemGeneric& slot = items[id];

	// Snapshot the originals the FIRST time this slot is patched (insert-if-absent),
	// so a second patch to the same slot never captures an already-patched value.
	if ( s_itemPatches.find(id) == s_itemPatches.end() )
	{
		SAMItemSaved s;
		s.weight = slot.weight;
		s.gold_value = slot.gold_value;
		s.level = slot.level;
		s.category = slot.category;
		s.item_slot = slot.item_slot;
		s.tooltip = slot.tooltip;
		s.nameId = slot.getIdentifiedName();
		s.nameUnid = slot.getUnidentifiedName();
		s.attributes = slot.attributes;
		s_itemPatches[id] = s;
	}

	if ( p.hasWeight )   { slot.weight = p.weight; }
	if ( p.hasValue )    { slot.gold_value = p.value; }
	if ( p.hasLevel )    { slot.level = p.level; }
	if ( p.hasCategory ) { slot.category = categoryFromName(p.category); }
	if ( p.hasSlot )     { slot.item_slot = slotFromName(p.slot); }
	if ( p.hasTooltip )  { slot.tooltip = p.tooltip; }
	if ( p.hasNameId )   { slot.setIdentifiedName(p.nameIdentified); }
	if ( p.hasNameUnid ) { slot.setUnidentifiedName(p.nameUnidentified); }
	for ( const auto& kv : p.attributes ) { slot.attributes[kv.first] = (Sint32)kv.second; } // MERGE

	SAM_INFO(MOD, "Patched item slot " + std::to_string(id) + " ("
		+ std::to_string(p.attributes.size()) + " attribute override(s))");
	return true;
}

const SAMItemDef* SAMItems::getItem(int itemId)
{
	auto it = s_registry.find(itemId);
	return (it != s_registry.end()) ? &it->second : nullptr;
}

int SAMItems::itemIdForIdString(const std::string& idString)
{
	for ( const auto& kv : s_registry )
	{
		if ( kv.second.id == idString )
		{
			return kv.first;
		}
	}
	return -1;
}

std::string SAMItems::getIconPath(int itemId)
{
	auto it = s_registry.find(itemId);
	if ( it == s_registry.end() || it->second.icon.empty() )
	{
		return std::string();
	}
	// Same absolute path we resolved at registration, but collapse any accidental
	// "//" into "/" so Image::get (PhysFS + raw fallback) resolves it cleanly.
	const std::string raw = toForwardSlashes(joinPath(it->second.modPath, it->second.icon));
	std::string out;
	out.reserve(raw.size());
	for ( char c : raw )
	{
		if ( c == '/' && !out.empty() && out.back() == '/' ) { continue; }
		out.push_back(c);
	}
	if ( !fileExists(out) )
	{
		return std::string(); // fall back to the placeholder rather than a broken path
	}
	return out;
}

void SAMItems::registerModModels()
{
#ifndef EDITOR
	// A mod's folder is PhysFS-mounted at the root, so a file the mod ships at
	// <moddir>/models/sword.vox is logically just "models/sword.vox" — which is exactly
	// what loadVoxel wants, and exactly what the modder wrote in the JSON. No joining
	// against modPath here (unlike `icon`, which needs a real absolute path for the
	// image loader). The model path IS the logical path.
	//
	// Consequence worth knowing: model paths share one namespace with vanilla and with
	// every other mod. Two mods shipping "models/sword.vox" resolve to whichever mounted
	// first. Modders should namespace their folder (models/<yourmod>/sword.vox).
	std::vector<SAMModels::Request> reqs;
	std::set<std::string> seen;

	auto want = [&](SAMItemDef& def, std::string& path, const char* field) {
		if ( path.empty() ) { return; }
		// Untrusted mod JSON — the same guard `icon` gets. A model path that climbs out
		// of the mod folder could point loadVoxel at any file on disk.
		if ( SAMErrors::relPathEscapes(path) )
		{
			SAM_WARN(MOD, "Item [" + def.id + "] " + field + " path '" + path
				+ "' escapes the mod folder — ignoring it.");
			path.clear();
			return;
		}
		if ( seen.insert(path).second )
		{
			// The path is its own id: unique, stable, and it saves inventing a naming
			// scheme. Two items sharing one .vox therefore cost one model entry.
			reqs.push_back({ path, path });
		}
	};

	for ( auto& kv : s_registry )
	{
		want(kv.second, kv.second.model, "model");
		want(kv.second, kv.second.modelFp, "model_fp");
	}
	if ( reqs.empty() ) { return; }

	SAMModels::appendModels(reqs);

	// Point each item at what it asked for. This runs after model_from_item has already
	// set an index, so an explicit "model" deliberately wins over a cloned vanilla one.
	for ( auto& kv : s_registry )
	{
		SAMItemDef& def = kv.second;
		const int id = kv.first;
		if ( id < 0 || id >= NUM_ITEM_SLOTS ) { continue; }

		if ( !def.model.empty() )
		{
			const int idx = SAMModels::modelIndexForId(def.model);
			if ( idx >= 0 )
			{
				items[id].index = idx;
				// No separate short-race variant for a custom model; reuse the same one
				// so short races don't silently fall back to the placeholder.
				items[id].indexShort = idx;
				// Held/first-person view defaults to the same model unless the mod gave
				// a dedicated one below.
				if ( def.modelFp.empty() ) { items[id].fpindex = idx; }
				SAM_INFO(MOD, "Item [" + def.id + "] model '" + def.model
					+ "' -> model index " + std::to_string(idx));
			}
		}
		if ( !def.modelFp.empty() )
		{
			const int idx = SAMModels::modelIndexForId(def.modelFp);
			if ( idx >= 0 )
			{
				items[id].fpindex = idx;
				SAM_INFO(MOD, "Item [" + def.id + "] model_fp '" + def.modelFp
					+ "' -> model index " + std::to_string(idx));
			}
		}
	}
#endif
}
