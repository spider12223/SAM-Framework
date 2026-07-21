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
		int resolvedType = -1;  // filled by ensureResolved()
	};

	// Registry — EMPTY in vanilla (the whole no-op guarantee).
	std::vector<Recipe>      s_add;         // add / re-cost recipes
	std::vector<std::string> s_suppressIds; // raw ids a mod asked to hide

	// Resolved views, rebuilt on demand after a load (see ensureResolved).
	std::map<int, int> s_byType;      // resolved ItemType -> index into s_add
	std::set<int>      s_suppressed;  // resolved ItemTypes to hide
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
		s_byType.clear();
		s_suppressed.clear();
		for ( size_t i = 0; i < s_add.size(); ++i )
		{
			const int t = resolveItemId(s_add[i].itemId);
			s_add[i].resolvedType = t;
			if ( t < 0 )
			{
				SAM_WARN(MOD, "Recipe for '" + s_add[i].itemId + "' could not be resolved "
					"(unknown item name, or its mod is not loaded) — skipping it.");
				continue;
			}
			// First recipe for an item wins, so two mods can't fight over one cost.
			if ( s_byType.find(t) == s_byType.end() ) { s_byType[t] = (int)i; }
			else
			{
				SAM_WARN(MOD, "Two recipes target '" + s_add[i].itemId + "' — keeping the first.");
			}
		}
		for ( const std::string& id : s_suppressIds )
		{
			const int t = resolveItemId(id);
			if ( t < 0 )
			{
				SAM_WARN(MOD, "Cannot hide recipe for unknown item '" + id + "'.");
				continue;
			}
			s_suppressed.insert(t);
		}
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

		// ---- { "remove": "<item>" } hides a vanilla CRAFT entry -----------------
		const std::string removeId = getStr("remove");
		if ( !removeId.empty() )
		{
			s_suppressIds.push_back(removeId);
			s_resolved = false;
			SAM_INFO(MOD, "[" + manifest.ns + "] hides the craft entry for '" + removeId + "'.");
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
		// A 0/0 recipe would render in the grid and then permanently fail to craft:
		// the engine's affordability and consume paths both bail on no materials.
		if ( r.metal + r.magic <= 0 )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/metal_cost", "0",
				"a recipe with no material cost can never be crafted",
				"at least 1 total scrap across metal_cost + magic_cost",
				"set metal_cost or magic_cost to 1 or more", "recipe not loaded.");
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
	s_byType.clear();
	s_suppressed.clear();
	s_resolved = false;
}

bool SAMRecipes::any()
{
	return !s_add.empty() || !s_suppressIds.empty();
}

bool SAMRecipes::isVanillaSuppressed(int itemType)
{
	if ( s_suppressIds.empty() ) { return false; }
	ensureResolved();
	return s_suppressed.find(itemType) != s_suppressed.end();
}

bool SAMRecipes::costFor(int itemType, int& metal, int& magic)
{
	if ( s_add.empty() ) { return false; }
	ensureResolved();
	auto it = s_byType.find(itemType);
	if ( it == s_byType.end() ) { return false; }
	metal = s_add[it->second].metal;
	magic = s_add[it->second].magic;
	return true;
}

bool SAMRecipes::skillFor(int itemType, int& required)
{
	if ( s_add.empty() ) { return false; }
	ensureResolved();
	auto it = s_byType.find(itemType);
	if ( it == s_byType.end() ) { return false; }
	required = s_add[it->second].skillTier;
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

std::string SAMRecipes::itemIdAtIndex(int index)
{
	if ( index < 0 || index >= (int)s_add.size() ) { return std::string(); }
	return s_add[index].itemId;
}
