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
#include "nlohmann/json.hpp"

#include "main.hpp"    // list_t/string_t, stringCopy, stringDeconstructor, list_* helpers
#include "items.hpp"   // items[], ItemGeneric, ItemType, Category, ItemEquippableSlot, NUM_ITEM_SLOTS

#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>

using nlohmann::json;

static const char* MOD = "ITEMS";

static std::map<int, SAMItemDef> s_registry;
static int s_nextItemId = SAM_ITEM_ID_BASE;

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
		const size_t len = 64;
		s->data = static_cast<char*>(malloc(sizeof(char) * len));
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
	ItemGeneric& slot = items[id];

	// Placeholder visuals cloned from a category-appropriate vanilla item.
	const ItemType tmpl = templateForCategory(cat);
	slot.index = items[tmpl].index;
	slot.fpindex = items[tmpl].fpindex;
	slot.indexShort = items[tmpl].indexShort;
	slot.variations = 1;
	deepCopyImages(slot.images, items[tmpl].images);

	// Metadata from the JSON.
	slot.setIdentifiedName(def.nameIdentified);
	slot.setUnidentifiedName(def.nameUnidentified.empty() ? def.nameIdentified : def.nameUnidentified);
	slot.category = cat;
	slot.item_slot = slotFromName(def.slot);
	slot.weight = def.weight;
	slot.gold_value = def.goldValue;
	slot.level = def.level;
	slot.tooltip = "tooltip_default";
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

		json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
		if ( j.is_discarded() || !j.is_object() )
		{
			SAM_ERROR(MOD, "Invalid item JSON (not a JSON object): " + path);
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

		if ( def.id.empty() )
		{
			SAM_ERROR(MOD, "Item missing required 'id' in: " + path);
			continue;
		}
		if ( def.nameIdentified.empty() )
		{
			SAM_ERROR(MOD, "Item [" + def.id + "] missing required 'name_identified' in: " + path);
			continue;
		}
		if ( def.category.empty() )
		{
			SAM_ERROR(MOD, "Item [" + def.id + "] missing required 'category' in: " + path);
			continue;
		}

		def.nameUnidentified = getStr("name_unidentified");
		def.slot = getStr("slot");
		if ( def.slot.empty() ) { def.slot = "NO_EQUIP"; }
		def.weight = getInt("weight", 0);
		def.goldValue = getInt("gold_value", 0);
		def.level = getInt("level", -1);
		def.model = getStr("model");
		def.modelFp = getStr("model_fp");
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
				}
			}
		}

		registerItem(def);
	}
}

void SAMItems::clear()
{
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

int SAMItems::count()
{
	return static_cast<int>(s_registry.size());
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
