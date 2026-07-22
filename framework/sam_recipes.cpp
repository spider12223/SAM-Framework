/*-------------------------------------------------------------------------------
	S.A.M Framework — Tinkering recipe registry.
	See sam_recipes.hpp for the design + the no-op rule.
-------------------------------------------------------------------------------*/

#include "sam_recipes.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <utility>

// Engine build: needed to resolve an item NAME to a runtime ItemType. In the
// standalone/editor build these headers are absent, so resolution is a no-op and
// every accessor answers false (the registry still parses, it just never resolves).
#if defined(__has_include) && __has_include("items.hpp")
#	define SAM_RECIPES_HAVE_BARONY 1
#	include "items.hpp"      // ItemType, Status
#	include "mod_tools.hpp"  // ItemTooltips.itemNameStringToItemID
#	include "sam_items.hpp"  // SAMItems::itemIdForIdString (custom "ns:item")
#endif

using nlohmann::json;
static const char* MOD = "RECIPES";

namespace
{
	std::string joinPath(const std::string& dir, const std::string& file)
	{
		if ( dir.empty() ) { return file; }
		if ( file.empty() ) { return dir; }
		const char last = dir[dir.size() - 1];
		if ( last == '/' || last == '\\' ) { return dir + file; }
		return dir + "/" + file;
	}
	bool readWholeFile(const std::string& path, std::string& out)
	{
		std::ifstream f(path.c_str(), std::ios::binary);
		if ( !f.good() ) { return false; }
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}
	std::string toLowerStr(const std::string& s)
	{
		std::string o = s;
		for ( char& c : o ) { if ( c >= 'A' && c <= 'Z' ) { c = (char)(c - 'A' + 'a'); } }
		return o;
	}

	// Barony's Status enum order (BROKEN..EXCELLENT). Vanilla craftables use EXCELLENT.
	const char* kStatusNames[] = { "BROKEN", "DECREPIT", "WORN", "SERVICABLE", "EXCELLENT" };
	const int kStatusExcellent = 4;

	// Vanilla buckets the tinkering requirement into 6 tiers of 20 (0/20/40/60/80/100),
	// and the GUI recovers the displayed number by multiplying the tier by 20. Staying on
	// that granularity means no display code has to change.
	const int kMaxSkillTier = 5;

	struct Recipe
	{
		std::string itemId;     // "TOOL_FIRE_BOMB" or "ns:item" — resolved lazily
		int metal = 0;
		int magic = 0;
		int skillTier = 0;      // 0..5
		int status = kStatusExcellent;
		int slotX = -1, slotY = -1;
		bool hasSlot = false;
		std::string kitId;      // optional: an item that opens its OWN grid
		std::string matId[2];   // optional: up to two custom materials (else scrap)
		int matCount[2] = { 0, 0 };
		int resolvedMat[2] = { -1, -1 };
		int resolvedType = -1;  // filled by ensureResolved()
		int resolvedKit = -1;   // filled by ensureResolved(); -1 = the vanilla kit
	};

	// Registry — EMPTY in vanilla (the whole no-op guarantee).
	std::vector<Recipe>      s_add;         // add / re-cost recipes
	std::vector<std::string> s_suppressIds; // raw ids a mod asked to hide

	// Resolved views, rebuilt on demand after a load (see ensureResolved).
	// (kit, ItemType) -> index into s_add. Keyed by BOTH because the same item may be
	// craftable at several benches for different costs (kit -1 = the vanilla kit).
	std::map<std::pair<int, int>, int> s_byKitType;
	// Which kit the player currently has open; set while its craftable list is built.
	int s_activeKit = -1;
	std::set<int>      s_suppressed;  // resolved ItemTypes to hide
	std::set<int>      s_kitTypes;    // resolved ItemTypes that act as custom kits
	// Kits the FRAMEWORK owns. Kept separate from s_kitTypes because ensureResolved()
	// rebuilds that set from the recipe list, which would wipe a kit no recipe mentions.
	std::set<int>      s_builtinKits;
	bool s_resolved = false;

	// "TOOL_FIRE_BOMB" / "ns:item" -> runtime ItemType, or -1. Engine build only.
	int resolveItemId(const std::string& id)
	{
#ifdef SAM_RECIPES_HAVE_BARONY
		if ( id.empty() ) { return -1; }
		if ( id.find(':') != std::string::npos )
		{
			return SAMItems::itemIdForIdString(id); // custom item (>= SAM_ITEM_ID_BASE)
		}
		auto it = ItemTooltips.itemNameStringToItemID.find(toLowerStr(id));
		if ( it != ItemTooltips.itemNameStringToItemID.end() ) { return (int)it->second; }
		return -1;
#else
		(void)id; return -1;
#endif
	}

	// Resolve every id ONCE per load cycle. Deliberately lazy: items are registered
	// per-mod in the same loader loop that registers recipes, so resolving at parse
	// time would miss any item belonging to a mod that loads later.
	void ensureResolved()
	{
		if ( s_resolved ) { return; }
		s_resolved = true;
		s_byKitType.clear();
		s_suppressed.clear();
		s_kitTypes.clear();
		for ( size_t i = 0; i < s_add.size(); ++i )
		{
			const int t = resolveItemId(s_add[i].itemId);
			s_add[i].resolvedType = t;
			s_add[i].resolvedKit = -1;
			if ( !s_add[i].kitId.empty() )
			{
				const int k = resolveItemId(s_add[i].kitId);
				if ( k < 0 )
				{
					SAM_WARN(MOD, "Recipe names kit '" + s_add[i].kitId + "' which could not be resolved "
						"— it will appear on the normal tinkering kit instead.");
				}
				else { s_add[i].resolvedKit = k; s_kitTypes.insert(k); }
			}
			for ( int m = 0; m < 2; ++m )
			{
				s_add[i].resolvedMat[m] = -1;
				if ( s_add[i].matId[m].empty() ) { continue; }
				const int mt = resolveItemId(s_add[i].matId[m]);
				if ( mt < 0 )
				{
					SAM_WARN(MOD, "Recipe material '" + s_add[i].matId[m] + "' could not be resolved "
						"— this recipe falls back to scrap costs.");
				}
				else { s_add[i].resolvedMat[m] = mt; }
			}
			if ( t < 0 )
			{
				SAM_WARN(MOD, "Recipe for '" + s_add[i].itemId + "' could not be resolved "
					"(unknown item name, or its mod is not loaded) — skipping it.");
				continue;
			}
			// NOTHING a mod does may alter the vanilla tinkering kit. Every recipe has to
			// name the bench it belongs on, and that bench is never the vanilla one: the
			// Hunter's Workbench exists now, so there is somewhere to put things that isn't
			// vanilla's grid. A recipe with no "kit" simply does not appear.
			//
			// This replaces the v1.7.0 behaviour where naming a vanilla item re-priced it in
			// place. That form only made sense while the vanilla kit was the only bench.
			if ( s_add[i].resolvedKit < 0 )
			{
				SAM_WARN(MOD, "Recipe for '" + s_add[i].itemId + "' has no \"kit\", so it will not "
					"appear. Add \"kit\": \"sam:hunters_workbench\" for the framework's bench, or name "
					"one of your own items to use your own. Recipes can no longer change the vanilla "
					"tinkering kit.");
				continue;
			}
			// First recipe for a given (kit, item) wins, so two mods can't fight over one
			// cost — but the SAME item may still have a different recipe at another bench.
			const std::pair<int, int> key(s_add[i].resolvedKit, t);
			if ( s_byKitType.find(key) == s_byKitType.end() ) { s_byKitType[key] = (int)i; }
			else
			{
				SAM_WARN(MOD, "Two recipes target '" + s_add[i].itemId + "' on the same kit — keeping the first.");
			}
		}
		// Hiding a vanilla craftable is likewise no longer possible: it existed only to free
		// a cell on vanilla's grid, and mods do not use that grid any more. Reported rather
		// than silently dropped, so a mod written against v1.7.0 says why nothing happened.
		for ( const std::string& id : s_suppressIds )
		{
			SAM_WARN(MOD, "Recipe asks to hide the vanilla craftable '" + id + "' — ignored. "
				"The vanilla tinkering kit is left exactly as the game ships it.");
		}
	}
}

namespace
{
	// The recipe for `itemType` AT THE KIT THE PLAYER HAS OPEN, or -1. Deliberately an
	// exact match: a recipe bound to a custom kit must not answer for the vanilla kit,
	// and vice versa, or one bench reports another bench's price.
	int findRecipeForActiveKit(int itemType)
	{
		ensureResolved();
		auto it = s_byKitType.find(std::pair<int, int>(s_activeKit, itemType));
		return ( it != s_byKitType.end() ) ? it->second : -1;
	}
}

void SAMRecipes::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const std::string& relPath : manifest.recipes )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "Recipe file not found: " + path + " (declared by [" + manifest.ns + "])");
			continue;
		}
		const std::string fileLabel = SAMErrors::displayFile(manifest.ns, relPath);
		json j;
		try { j = json::parse(text); }
		catch ( const json::parse_error& e )
		{
			SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "recipe not loaded.");
			continue;
		}
		if ( !j.is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
				"a JSON object: { ... }", "wrap the file contents in { }", "recipe not loaded.");
			continue;
		}

		auto getStr = [&](const char* k) -> std::string {
			auto it = j.find(k);
			return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
		};
		auto getInt = [&](const char* k, int dv) -> int {
			auto it = j.find(k);
			return (it != j.end() && it->is_number_integer()) ? it->get<int>() : dv;
		};

		// ---- { "remove": "<item>" } ---------------------------------------------
		// Retired. This hid a recipe from the VANILLA tinkering kit, which existed to free
		// up one of its four spare cells back when it was the only bench. Nothing a mod
		// ships may alter vanilla crafting any more, and the Hunter's Workbench has its own
		// empty grid, so there is nothing to free. Parsed and ignored rather than treated
		// as an error, so a mod written against v1.7.0 still loads.
		const std::string removeId = getStr("remove");
		if ( !removeId.empty() )
		{
			SAM_WARN(MOD, "[" + manifest.ns + "] asks to hide the craft entry for '" + removeId
				+ "', which is no longer supported — the vanilla tinkering kit is left alone. "
				"Bind your recipes to \"sam:hunters_workbench\" instead; it has a full empty grid.");
			continue;
		}

		// ---- otherwise it adds or re-costs a recipe -----------------------------
		Recipe r;
		r.itemId = getStr("item");
		if ( r.itemId.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/item", "", "missing (required)",
				"a vanilla ItemType (e.g. \"TOOL_FIRE_BOMB\") or a custom \"namespace:item\" id",
				"add an \"item\" field (or a \"remove\" field to hide a vanilla recipe)",
				"recipe not loaded.");
			continue;
		}

		r.metal = getInt("metal_cost", 0);
		r.magic = getInt("magic_cost", 0);
		if ( r.metal < 0 ) { r.metal = 0; }
		if ( r.magic < 0 ) { r.magic = 0; }

		r.kitId = getStr("kit"); // optional: bind this recipe to a custom kit item

		// Optional custom materials: up to two, because the crafting panel has exactly
		// two material columns. When present these replace the scrap costs entirely.
		{
			auto mIt = j.find("materials");
			if ( mIt != j.end() && mIt->is_array() )
			{
				int slot = 0;
				for ( const json& m : *mIt )
				{
					if ( slot >= 2 )
					{
						SAM_WARN(MOD, "Recipe for '" + r.itemId + "' lists more than two materials "
							"— the crafting panel only has two columns, so the extras are ignored.");
						break;
					}
					if ( !m.is_object() ) { continue; }
					auto miId = m.find("item");
					auto miCt = m.find("count");
					if ( miId == m.end() || !miId->is_string() ) { continue; }
					int ct = ( miCt != m.end() && miCt->is_number_integer() ) ? miCt->get<int>() : 1;
					if ( ct < 1 ) { ct = 1; }
					r.matId[slot] = miId->get<std::string>();
					r.matCount[slot] = ct;
					++slot;
				}
			}
		}

		// A recipe must cost SOMETHING. Custom materials satisfy that on their own; only a
		// scrap-priced recipe needs metal/magic, because the engine's affordability and
		// consume paths both bail when neither is set (it would render and never craft).
		if ( r.matId[0].empty() && r.metal + r.magic <= 0 )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/metal_cost", "0",
				"a recipe with no material cost can never be crafted",
				"at least 1 total scrap across metal_cost + magic_cost, or a \"materials\" list",
				"set metal_cost or magic_cost to 1 or more, or declare \"materials\"",
				"recipe not loaded.");
			continue;
		}

		r.skillTier = getInt("skill_level", 0);
		if ( r.skillTier < 0 || r.skillTier > kMaxSkillTier )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/skill_level", std::to_string(r.skillTier),
				"out of range",
				"a tier from 0 to 5 (each tier is 20 Tinkering skill: 0/20/40/60/80/100)",
				"clamp it into 0..5", "clamped.");
			r.skillTier = (r.skillTier < 0) ? 0 : kMaxSkillTier;
		}

		const std::string statusName = getStr("status");
		if ( !statusName.empty() )
		{
			bool found = false;
			for ( int i = 0; i < 5; ++i )
			{
				if ( statusName == kStatusNames[i] ) { r.status = i; found = true; break; }
			}
			if ( !found )
			{
				SAM_WARN(MOD, "Recipe status '" + statusName + "' is not a known condition — using EXCELLENT.");
				r.status = kStatusExcellent;
			}
		}

		// Optional explicit grid cell. Left unset means "next free cell".
		auto slotIt = j.find("slot");
		if ( slotIt != j.end() && slotIt->is_object() )
		{
			auto sx = slotIt->find("x");
			auto sy = slotIt->find("y");
			if ( sx != slotIt->end() && sx->is_number_integer()
				&& sy != slotIt->end() && sy->is_number_integer() )
			{
				r.slotX = sx->get<int>();
				r.slotY = sy->get<int>();
				r.hasSlot = (r.slotX >= 0 && r.slotY >= 0);
			}
		}

		s_add.push_back(r);
		s_resolved = false; // force a re-resolve on the next query
		SAM_INFO(MOD, "[" + manifest.ns + "] recipe: '" + r.itemId + "' for "
			+ std::to_string(r.metal) + " metal + " + std::to_string(r.magic)
			+ " magic scrap (tier " + std::to_string(r.skillTier) + ").");
	}
}

void SAMRecipes::clear()
{
	s_add.clear();
	s_suppressIds.clear();
	s_byKitType.clear();
	s_suppressed.clear();
	// s_kitTypes and s_activeKit were being left behind. Today that is masked because every
	// public entry point early-outs on s_add being empty, so a stale kit id is never read.
	// It is a live trap for the next change that relaxes one of those early-outs: a kit id
	// from an unloaded mod would survive into the next session and make an unrelated item
	// open the tinkering screen. Clear everything the load cycle owns.
	s_kitTypes.clear();
	s_builtinKits.clear();
	s_activeKit = -1;
	s_resolved = false;
}

bool SAMRecipes::any()
{
	return !s_add.empty() || !s_suppressIds.empty() || !s_builtinKits.empty();
}

bool SAMRecipes::isVanillaSuppressed(int /*itemType*/)
{
	// Always false: nothing may hide a vanilla craftable any more. Kept as a function so
	// the engine's call site stays put and the vanilla grid is provably never filtered.
	return false;
}

bool SAMRecipes::costFor(int itemType, int& metal, int& magic)
{
	if ( s_add.empty() ) { return false; }
	const int i = findRecipeForActiveKit(itemType);
	if ( i < 0 ) { return false; }
	// A custom-material recipe has no scrap cost, and the crafting panel reads THIS to
	// fill its two cost columns — reporting 0/0 would print "-" and read as free. Report
	// the material counts instead so the numbers on screen match what a craft consumes.
	// (Affordability and consumption branch on materialsFor() before ever reaching here,
	// so this only drives display.)
	if ( s_add[i].resolvedMat[0] >= 0 )
	{
		metal = s_add[i].matCount[0];
		magic = ( s_add[i].resolvedMat[1] >= 0 ) ? s_add[i].matCount[1] : 0;
		return true;
	}
	metal = s_add[i].metal;
	magic = s_add[i].magic;
	return true;
}

bool SAMRecipes::skillFor(int itemType, int& required)
{
	if ( s_add.empty() ) { return false; }
	const int i = findRecipeForActiveKit(itemType);
	if ( i < 0 ) { return false; }
	required = s_add[i].skillTier;
	return true;
}

int SAMRecipes::count()
{
	return (int)s_add.size();
}

bool SAMRecipes::entryAtIndex(int index, int& itemType, int& status, int& slotX, int& slotY, bool& hasSlot)
{
	if ( index < 0 || index >= (int)s_add.size() ) { return false; }
	ensureResolved();
	const Recipe& r = s_add[index];
	if ( r.resolvedType < 0 ) { return false; }
	itemType = r.resolvedType;
	status   = r.status;
	slotX    = r.slotX;
	slotY    = r.slotY;
	hasSlot  = r.hasSlot;
	return true;
}

bool SAMRecipes::materialsFor(int itemType, int& typeA, int& cntA, int& typeB, int& cntB)
{
	if ( s_add.empty() ) { return false; }
	const int i = findRecipeForActiveKit(itemType);
	if ( i < 0 ) { return false; }
	const Recipe& r = s_add[i];
	if ( r.resolvedMat[0] < 0 ) { return false; } // no custom materials -> scrap as usual
	typeA = r.resolvedMat[0];
	cntA  = r.matCount[0];
	typeB = r.resolvedMat[1];      // -1 when the recipe only uses one material
	cntB  = ( typeB >= 0 ) ? r.matCount[1] : 0;
	return true;
}

void SAMRecipes::setActiveKit(int kitItemType)
{
	s_activeKit = kitItemType;
}

int SAMRecipes::activeKit()
{
	return s_activeKit;
}

void SAMRecipes::registerBuiltinKit(int itemType)
{
	if ( itemType >= 0 ) { s_builtinKits.insert(itemType); }
}

bool SAMRecipes::isCustomKit(int itemType)
{
	// Compound early-out on purpose. This runs once per inventory node while the tinker
	// screen builds its list, so the vanilla path must stay a single empty() check and
	// never reach a std::set lookup.
	if ( s_add.empty() && s_builtinKits.empty() ) { return false; }
	if ( s_builtinKits.find(itemType) != s_builtinKits.end() ) { return true; }
	if ( s_add.empty() ) { return false; }
	ensureResolved();
	return s_kitTypes.find(itemType) != s_kitTypes.end();
}

int SAMRecipes::kitForIndex(int index)
{
	if ( index < 0 || index >= (int)s_add.size() ) { return -1; }
	ensureResolved();
	return s_add[index].resolvedKit;
}

std::string SAMRecipes::itemIdAtIndex(int index)
{
	if ( index < 0 || index >= (int)s_add.size() ) { return std::string(); }
	return s_add[index].itemId;
}
