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
#include "sam_classes.hpp" // class appearance model paths (whole-body + heads) to append
#include "nlohmann/json.hpp"

#include <map>
#include <string>
#include <utility>

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

// Resolved kit_ui paths, cached because the crafting panel asks for every role on EVERY
// frame: without this each open panel cost ~17 real file opens per frame per player, just
// to re-derive a string that cannot change until the next mod load. Dropped by clear().
static std::map<std::pair<int, std::string>, std::string> s_kitUiPathCache;
// Same story for icons: getIconPath is called per inventory/grid slot per frame, and each
// call was doing a real file open just to re-derive an unchanging string.
static std::map<int, std::string> s_iconPathCache;

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

static std::string samLower(const std::string& in)
{
	std::string out = in;
	for ( char& c : out ) { c = (char)std::tolower((unsigned char)c); }
	return out;
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

// A vanilla item matching a DECLARED weapon skill. This drives both the placeholder model
// and — more importantly — the cloned TOOLTIP, because Barony's tooltip renderer works out
// which skill to name by substring-matching the tooltip KEY ("tooltip_axe" -> Axe). Cloning
// iron_sword for every weapon is what made every custom weapon claim "Sword Damage" and
// sword-skill scaling regardless of what it actually was.
// Returns -1 when the mod declared nothing, so the caller keeps its slot/category choice.
static int templateForWeaponSkill(const std::string& skill)
{
	if ( skill == "axe" )     { return IRON_AXE; }
	if ( skill == "mace" )    { return IRON_MACE; }
	if ( skill == "polearm" ) { return IRON_SPEAR; }
	if ( skill == "sword" )   { return IRON_SWORD; }
	return -1; // unset or unrecognised: keep the slot/category pick
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

// The tooltip key for a custom item. It MUST BEGIN WITH the vanilla category key it was
// cloned from (tooltip_sword, tooltip_armor_breastpiece, tooltip_offhand, ...) because the
// tooltip renderer (mod_tools.cpp ItemTooltips_t::formatItemIcon) fills in the numeric rows
// (%+d ATK, AC, ...) only when the item's tooltip key SUBSTRING-matches one of those category
// names. A bare "tooltip_sam_<id>" matched none of them, so every value row rendered as the
// literal "%+d" format string (reported: "%+d Sword Damage" on custom weapons/shields).
// Appending "__sam_<sanitized id>" keeps the key unique per custom item while preserving the
// category prefix the renderer keys off.
static std::string samTooltipKeyFor(const std::string& srcKey, const std::string& itemStrId)
{
	std::string ttKey = srcKey.empty() ? std::string("tooltip_default") : srcKey;
	ttKey += "__sam_";
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
	// A declared weapon skill wins over both, because it decides the tooltip key and so the
	// skill the tooltip names. Falls back to the slot/category pick when nothing is declared.
	// Gated on the equip slot: weapon_skill means nothing on a shield or a helm, and letting
	// it through would clone a sword's model onto one.
	const int samSkillTmpl = ( eslot == EQUIPPABLE_IN_SLOT_WEAPON )
		? templateForWeaponSkill(def.weaponSkill) : -1;
	const ItemType tmpl = ( samSkillTmpl >= 0 )
		? (ItemType)samSkillTmpl
		: ( (eslot != NO_EQUIP) ? templateForSlot(eslot) : templateForCategory(cat) );

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
	const std::string ttKey = samTooltipKeyFor(srcKey, def.id);
	ItemTooltips.tooltips[ttKey] = std::move(ttCopy);
	items[id].tooltip = ttKey;
}
#endif

// Write one parsed def into a specific items[] slot. Used both by the ordinary
// load-order allocator below and by the framework's own built-ins, which must land
// on a FIXED id rather than one that moves with the user's mod list.
static bool registerItemAt(int id, SAMItemDef def)
{
	def.numericId = id;

	const Category cat = categoryFromName(def.category);
	const ItemEquippableSlot eslot = slotFromName(def.slot);
	ItemGeneric& slot = items[id];

	// Placeholder visuals cloned from a vanilla item. For an EQUIPPABLE item pick
	// the template by its equip slot (a shield clones a shield, boots clone boots,
	// ...) so it renders as the right kind of gear; otherwise fall back to a
	// category-appropriate template.
	// A declared weapon skill wins over both, because it decides the tooltip key and so the
	// skill the tooltip names. Falls back to the slot/category pick when nothing is declared.
	// Gated on the equip slot: weapon_skill means nothing on a shield or a helm, and letting
	// it through would clone a sword's model onto one.
	const int samSkillTmpl = ( eslot == EQUIPPABLE_IN_SLOT_WEAPON )
		? templateForWeaponSkill(def.weaponSkill) : -1;
	const ItemType tmpl = ( samSkillTmpl >= 0 )
		? (ItemType)samSkillTmpl
		: ( (eslot != NO_EQUIP) ? templateForSlot(eslot) : templateForCategory(cat) );
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
	// Same guard for the panel skin. These are mod-supplied relative paths joined onto the
	// mod folder exactly like the icon, so without this a kit_ui entry could reach outside
	// it while every sibling path could not.
	for ( auto it = def.kitUi.begin(); it != def.kitUi.end(); )
	{
		if ( SAMErrors::relPathEscapes(it->second) )
		{
			SAM_WARN(MOD, "Item [" + def.id + "] kit_ui." + it->first + " path '" + it->second
				+ "' escapes the mod folder — ignoring it.");
			it = def.kitUi.erase(it);
		}
		else { ++it; }
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

// Allocate the next mod item id in load order and write the def there. The ceiling is
// the built-in band, not NUM_ITEM_SLOTS: mods must never be handed a slot the framework
// has reserved for itself, or a mod item and a built-in would collide on the same id.
static bool registerItem(SAMItemDef def)
{
	const int id = s_nextItemId;
	if ( id >= SAM_BUILTIN_ITEM_ID_BASE )
	{
		SAM_ERROR(MOD, "Item registry full (next id " + std::to_string(id) + " >= "
			+ std::to_string(SAM_BUILTIN_ITEM_ID_BASE) + ", the start of the framework's reserved band) — skipping '"
			+ def.id + "'.");
		return false;
	}
	s_nextItemId = id + 1;
	return registerItemAt(id, std::move(def));
}

bool SAMItems::registerBuiltinAt(int id, SAMItemDef def)
{
	if ( id < SAM_BUILTIN_ITEM_ID_BASE || id >= NUM_ITEM_SLOTS )
	{
		SAM_ERROR(MOD, "Built-in item id " + std::to_string(id) + " is outside the reserved band ["
			+ std::to_string(SAM_BUILTIN_ITEM_ID_BASE) + ", " + std::to_string(NUM_ITEM_SLOTS) + ") — skipping '"
			+ def.id + "'.");
		return false;
	}
	return registerItemAt(id, std::move(def));
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
		def.weaponSkill = samLower(getStr("weapon_skill"));
		if ( !def.weaponSkill.empty()
			&& def.weaponSkill != "sword" && def.weaponSkill != "axe"
			&& def.weaponSkill != "mace"  && def.weaponSkill != "polearm" )
		{
			SAM_WARN(MOD, "Item [" + def.id + "] weapon_skill '" + def.weaponSkill
				+ "' is not one of sword/axe/mace/polearm — falling back to sword. (For a throwable, "
				"set \"category\": \"THROWN\" instead; ranged weapons cannot be custom.)");
			def.weaponSkill.clear();
		}
		{
			// "kit_ui": { "<role>": "<mod-relative png>", ... } — the crafting-panel skin used
			// when THIS item is opened as a custom tinkering kit. Roles are validated at use
			// time, not here, so an unknown role is simply never asked for rather than a load
			// error; that keeps an older exe loading a newer mod.
			auto it = j.find("kit_ui");
			if ( it != j.end() && it->is_object() )
			{
				for ( auto kv = it->begin(); kv != it->end(); ++kv )
				{
					if ( kv.value().is_string() )
					{
						const std::string rel = kv.value().get<std::string>();
						if ( !rel.empty() ) { def.kitUi[kv.key()] = rel; }
					}
				}
			}
		}
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
					// Keys the engine actually reads: ATK (items.cpp attackGetAttack), AC
					// (armorGetAC), and the two tinkering salvage yields (interface.cpp
					// tinkeringGetItemValue, which returns how much metal/magic scrap this
					// item breaks down into). Anything else is stored and never looked at,
					// so an invented key like "EFF_UNBREAKABLE" silently does nothing — and
					// the JSON schema can't catch every case for older mods. Say so.
					if ( it.key() != "ATK" && it.key() != "AC"
						&& it.key() != "TINKER_SALVAGE_METAL" && it.key() != "TINKER_SALVAGE_MAGIC" )
					{
						SAM_WARN(MOD, "Item '" + def.id + "': attribute '" + it.key() + "' is not read by the engine "
							"and will have no effect. Only ATK, AC, TINKER_SALVAGE_METAL and TINKER_SALVAGE_MAGIC "
							"are used. (Item effects come from a script, not from an attribute key.)");
					}
				}
			}
		}

		registerItem(def);
	}
}

void SAMItems::clear()
{
	s_kitUiPathCache.clear(); // resolved panel-art paths die with the registry
	s_iconPathCache.clear();  // and the resolved icon paths
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
	auto cached = s_iconPathCache.find(itemId);
	if ( cached != s_iconPathCache.end() ) { return cached->second; }

	auto it = s_registry.find(itemId);
	if ( it == s_registry.end() || it->second.icon.empty() )
	{
		s_iconPathCache[itemId] = std::string();
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
		out.clear(); // fall back to the placeholder rather than a broken path
	}
	s_iconPathCache[itemId] = out;
	return out;
}

int SAMItems::weaponSkillFor(int itemId)
{
#ifdef EDITOR
	// PRO_* live in stat.hpp, which only the game target pulls in. The editor never asks a
	// weapon its skill, so answering "none" is correct there and keeps this file compiling
	// for both targets (sam_items.cpp is in EDITOR_SOURCES).
	(void)itemId;
	return -1;
#else
	auto it = s_registry.find(itemId);
	if ( it == s_registry.end() ) { return -1; }
	// MELEE ONLY, on purpose. Ranged and thrown are not a matter of which skill trains:
	// firing needs isRangedWeapon(), a hardcoded switch over vanilla bow types that a custom
	// id can never satisfy, and the character sheet computes a thrown weapon's ATK with a
	// different formula than the one combat actually uses. Claiming either here would make
	// the tooltip lie about a weapon that cannot work. A mod wanting a throwable declares
	// category THROWN instead, which the engine dispatches by category and already works.
	const std::string& sk = it->second.weaponSkill;
	if ( sk == "axe" )     { return PRO_AXE; }
	if ( sk == "mace" )    { return PRO_MACE; }
	if ( sk == "polearm" ) { return PRO_POLEARM; }
	if ( sk == "sword" )   { return PRO_SWORD; }
	// Nothing declared (or something unrecognised, which loadFromManifest has already warned
	// about). An equippable weapon still has to answer something or the engine treats it as
	// unarmed: no skill XP, no damage variance, no durability scaling. Sword is what these
	// items already behaved as, so it is the least surprising default.
	if ( it->second.slot == "EQUIPPABLE_IN_SLOT_WEAPON" ) { return PRO_SWORD; }
	return -1;
#endif
}

std::string SAMItems::getKitUiPath(int itemId, const std::string& role)
{
	const std::pair<int, std::string> cacheKey(itemId, role);
	auto cached = s_kitUiPathCache.find(cacheKey);
	if ( cached != s_kitUiPathCache.end() ) { return cached->second; }

	auto it = s_registry.find(itemId);
	if ( it == s_registry.end() ) { return std::string(); }
	auto r = it->second.kitUi.find(role);
	if ( r == it->second.kitUi.end() || r->second.empty() )
	{
		s_kitUiPathCache[cacheKey] = std::string();
		return std::string();
	}
	// Same resolution as getIconPath: join onto the mod folder, normalise slashes, and
	// refuse a path that is not actually on disk so the caller keeps the vanilla art
	// instead of asking Image::get for a file that will never resolve.
	const std::string raw = toForwardSlashes(joinPath(it->second.modPath, r->second));
	std::string out;
	out.reserve(raw.size());
	for ( char c : raw )
	{
		if ( c == '/' && !out.empty() && out.back() == '/' ) { continue; }
		out.push_back(c);
	}
	if ( !fileExists(out) ) { out.clear(); }
	s_kitUiPathCache[cacheKey] = out;
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
	// Class appearance models (whole-body overrides like a jet, and custom heads) share the
	// one model table, so register them in the SAME batch. resolveAppearance() (which runs
	// right after this) then indexes them. Same escape-guard + dedup as item models.
	for ( const std::string& p : SAMClasses::appearanceModelPaths() )
	{
		if ( SAMErrors::relPathEscapes(p) )
		{
			SAM_WARN(MOD, "Class appearance model path '" + p + "' escapes the mod folder — ignoring it.");
			continue;
		}
		if ( seen.insert(p).second ) { reqs.push_back({ p, p }); }
	}
	// v1.4.0 — standalone models a mod declares in mod.json "models" (for sam_spawn_companion
	// and other decorative entities, tied to no item/class). Registered under their FRIENDLY
	// id (not the path), so scripts spawn by "ns:name". Same escape-guard; dedup by id.
	for ( const SAMModManifest& m : SAMWorkshop::manifests() )
	{
		for ( const auto& md : m.models )   // md = { id, file }
		{
			if ( SAMErrors::relPathEscapes(md.second) ) { continue; } // belt-and-braces (parse already guards)
			if ( seen.insert("id:" + md.first).second ) { reqs.push_back({ md.first, md.second }); }
		}
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
