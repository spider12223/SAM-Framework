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

	// Custom inventory icon. The modern inventory UI draws items[type].images[0]'s
	// PATH STRING via Image::get (which resolves an absolute path directly, exactly
	// like the class-select portrait) — it does NOT read items[].surfaces. So point
	// image node 0 at the mod's PNG. deepCopyImages allocates a 64-byte buffer that
	// is too small for an absolute path, so free + re-malloc it to fit. If the file
	// is missing we keep the placeholder (cloned above), never crash.
	if ( !def.icon.empty() )
	{
		const std::string abs = toForwardSlashes(joinPath(def.modPath, def.icon));
		if ( fileExists(abs) && slot.images.first )
		{
			string_t* s = static_cast<string_t*>(slot.images.first->element);
			free(s->data);
			s->data = static_cast<char*>(malloc(abs.size() + 1));
			memset(s->data, 0, abs.size() + 1);
			stringCopy(s->data, abs.c_str(), abs.size(), abs.size());
			SAM_INFO(MOD, "Item [" + def.id + "] custom inventory icon -> " + abs);
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
