/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_loot.cpp
	Desc: see sam_loot.hpp.

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_loot.hpp"
#include "sam_rules.hpp"   // NetOp::LootTables: the one table of RulesFirst offsets

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "sam_logger.hpp"
#include "sam_event.hpp"
#include "sam_net.hpp"       // SAM_CLIENT_REFUSES: an `all` call carried here must not refuse itself; the tables reach a joiner at HELLO
#include "sam_items.hpp"     // lootCandidates: the modded half of the pool

#include "main.hpp"
#include "game.hpp"
#include "entity.hpp"
#include "items.hpp"
#include "stat.hpp"
#include "monster.hpp"
#include "player.hpp"
#include "net.hpp"
#include "shops.hpp"
#include "prng.hpp"
#include "magic/magic.hpp"
#include "interface/interface.hpp"   // openedChest[]: who has a chest open, so a remote opener gets a stack they can see

static const char* MOD = "LOOT";

namespace SAMLoot
{
	// ---- names --------------------------------------------------------------------------------

	static const char* const kKindNames[KindCount] = {
		"floor", "chest", "shop", "monster", "recipe", "console", "other"
	};

	const char* kindName(int kind)
	{
		if ( kind < 0 || kind >= KindCount ) { return "other"; }
		return kKindNames[kind];
	}

	static std::string samLower(const std::string& in)
	{
		std::string s = in;
		for ( char& c : s ) { c = (char)std::tolower((unsigned char)c); }
		return s;
	}

	int kindFromName(const std::string& name)
	{
		const std::string want = samLower(name);
		for ( int i = 0; i < KindCount; ++i )
		{
			if ( want == kKindNames[i] ) { return i; }
		}
		return -1;
	}

	// Each name carries its OWN enum constant, as /sam_loot does: a positional list rots the
	// moment the enum is reordered, and Category and ItemType are both plain ints the compiler
	// will not tell apart for us.
	struct CatName { const char* name; Category cat; };
	static const CatName kCatNames[] = {
		{ "WEAPON", WEAPON }, { "ARMOR", ARMOR }, { "AMULET", AMULET }, { "POTION", POTION },
		{ "SCROLL", SCROLL }, { "MAGICSTAFF", MAGICSTAFF }, { "RING", RING },
		{ "SPELLBOOK", SPELLBOOK }, { "GEM", GEM }, { "THROWN", THROWN }, { "TOOL", TOOL },
		{ "FOOD", FOOD }, { "BOOK", BOOK }, { "SPELL_CAT", SPELL_CAT }, { "TOME_SPELL", TOME_SPELL }
	};
	static_assert(sizeof(kCatNames) / sizeof(kCatNames[0]) == (size_t)CATEGORY_MAX,
		"sam_loot.cpp category table is missing an entry -- add the new Category here.");

	int categoryFromName(const std::string& name)
	{
		std::string want = name;
		for ( char& c : want ) { c = (char)std::toupper((unsigned char)c); }
		for ( const CatName& e : kCatNames )
		{
			if ( want == e.name ) { return (int)e.cat; }
		}
		return -1;
	}

	bool isAnyCategoryName(const std::string& name)
	{
		std::string want = name;
		for ( char& c : want ) { c = (char)std::toupper((unsigned char)c); }
		return want == "ANY" || want == "*";
	}

	std::string categoryName(int category)
	{
		if ( category < 0 ) { return "ANY"; }
		for ( const CatName& e : kCatNames )
		{
			if ( (int)e.cat == category ) { return e.name; }
		}
		return "?";
	}

	static std::string samCategoryHint()
	{
		std::string s;
		for ( const CatName& e : kCatNames )
		{
			if ( e.cat == SPELL_CAT || e.cat == TOME_SPELL ) { continue; }   // never rolled
			if ( !s.empty() ) { s += " "; }
			s += e.name;
		}
		return s;
	}

	bool validItem(int id)
	{
		if ( id < 0 || id >= NUM_ITEM_SLOTS ) { return false; }
		const char* nm = items[id].getIdentifiedName();
		return nm && nm[0];
	}

	static std::string samItemLabel(int id)
	{
		if ( id >= 0 && id < NUM_ITEM_SLOTS )
		{
			const char* nm = items[id].getIdentifiedName();
			if ( nm && nm[0] ) { return std::string(nm) + " (" + std::to_string(id) + ")"; }
		}
		return std::to_string(id);
	}

	// ---- the context --------------------------------------------------------------------------

	static Context s_context;

	ContextScope::ContextScope(int kind, long long uid, int shopType, int chestType, int monsterType)
		: prev(s_context)
	{
		Context next;
		next.kind = ( kind < 0 || kind >= KindCount ) ? (int)Other : kind;
		next.uid = uid;
		next.shopType = shopType;
		next.chestType = chestType;
		next.monsterType = monsterType;
		// A nested scope of the same kind that names nothing keeps what the outer one knew:
		// createMonsterEquipment says "monster gear" and actMonster's scope says whose.
		if ( next.kind == prev.kind )
		{
			if ( next.uid < 0 ) { next.uid = prev.uid; }
			if ( next.shopType < 0 ) { next.shopType = prev.shopType; }
			if ( next.chestType < 0 ) { next.chestType = prev.chestType; }
			if ( next.monsterType < 0 ) { next.monsterType = prev.monsterType; }
		}
		s_context = next;
	}

	ContextScope::~ContextScope()
	{
		s_context = prev;
	}

	const Context& context() { return s_context; }

	// ---- the tables ---------------------------------------------------------------------------

	struct ItemRule
	{
		int weight = 1;
		bool hasRange = false;
		int minFloor = 0;
		int maxFloor = -1;
		unsigned contextSet = 0;      // bit k: an allow/deny was declared for kind k
		unsigned contextAllowed = 0;  // bit k: allowed (meaningful only where contextSet has the bit)
		bool empty() const { return weight == 1 && !hasRange && contextSet == 0; }
	};
	static std::map<int, ItemRule> s_items;
	static int s_weightsAboveOne = 0;   // entries whose weight is > 1: the only ones that need a weighted draw

	static std::map<int, int> s_categoryWeight;        // category -> weight (absent = 1)
	static bool s_anyCategoryWeight = false;

	struct SpellRule { bool allowed = true; int minFloor = 0; };
	static std::map<int, SpellRule> s_spells;

	// (category, kind) -> item; kind -1 is the wildcard consulted second.
	static std::map<std::pair<int, int>, int> s_fallbacks;

	static void samRecountWeights()
	{
		s_weightsAboveOne = 0;
		for ( const auto& kv : s_items )
		{
			if ( kv.second.weight > 1 ) { ++s_weightsAboveOne; }
		}
	}

	static void samDropIfEmpty(int item)
	{
		auto it = s_items.find(item);
		if ( it != s_items.end() && it->second.empty() ) { s_items.erase(it); }
	}

	// The weighted draw (BaronyRNG::discrete) sums the weights into a 32-bit unsigned and has
	// no overflow check: two weights near INT_MAX wrap the total to nothing and the roll quietly
	// ignores most of the pool (an assert in a Debug exe). Every candidate slot at this ceiling
	// (NUM_ITEM_SLOTS of them, 6064) still sums to 6.1e8, well inside the 4.3e9 the sum can hold.
	static const int kMaxWeight = 100000;

	static int samClampWeight(int weight, const char* who, const std::string& what)
	{
		if ( weight <= kMaxWeight ) { return weight; }
		SAM_WARN(MOD, std::string(who) + ": a weight of " + std::to_string(weight) + " for " + what
			+ " was cut to " + std::to_string(kMaxWeight) + ", the most the weighted draw can add up without overflowing.");
		return kMaxWeight;
	}

	bool setWeight(int item, int weight)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_set_loot_weight refused: host only."); return false; }
		if ( !validItem(item) )
		{
			SAM_ERROR(MOD, "sam_set_loot_weight: " + std::to_string(item) + " is not an item this game has.");
			return false;
		}
		if ( weight < 0 )
		{
			SAM_ERROR(MOD, "sam_set_loot_weight: a weight cannot be negative (got " + std::to_string(weight)
				+ " for " + samItemLabel(item) + "). 0 means never, 1 is vanilla.");
			return false;
		}
		s_items[item].weight = samClampWeight(weight, "sam_set_loot_weight", samItemLabel(item));
		samDropIfEmpty(item);
		samRecountWeights();
		return true;
	}

	bool setCategoryWeight(int category, int weight)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_set_loot_category_weight refused: host only."); return false; }
		if ( category < 0 || category >= (int)CATEGORY_MAX - 2 )
		{
			SAM_ERROR(MOD, "sam_set_loot_category_weight: '" + categoryName(category)
				+ "' is not a category the \"any category\" draw can land on. Valid: " + samCategoryHint());
			return false;
		}
		if ( weight < 0 )
		{
			SAM_ERROR(MOD, "sam_set_loot_category_weight: a weight cannot be negative (got "
				+ std::to_string(weight) + " for " + categoryName(category) + ").");
			return false;
		}
		weight = samClampWeight(weight, "sam_set_loot_category_weight", categoryName(category));
		if ( weight == 1 ) { s_categoryWeight.erase(category); }
		else { s_categoryWeight[category] = weight; }
		s_anyCategoryWeight = !s_categoryWeight.empty();
		return true;
	}

	bool setFloorRange(int item, int minFloor, int maxFloor)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_set_loot_floor_range refused: host only."); return false; }
		if ( !validItem(item) )
		{
			SAM_ERROR(MOD, "sam_set_loot_floor_range: " + std::to_string(item) + " is not an item this game has.");
			return false;
		}
		if ( maxFloor >= 0 && maxFloor < minFloor )
		{
			SAM_ERROR(MOD, "sam_set_loot_floor_range: max_floor " + std::to_string(maxFloor) + " is below min_floor "
				+ std::to_string(minFloor) + " for " + samItemLabel(item) + ".");
			return false;
		}
		ItemRule& r = s_items[item];
		if ( minFloor <= 0 && maxFloor < 0 ) { r.hasRange = false; r.minFloor = 0; r.maxFloor = -1; }
		else { r.hasRange = true; r.minFloor = std::max(0, minFloor); r.maxFloor = maxFloor; }
		samDropIfEmpty(item);
		return true;
	}

	bool setContextAllowed(int item, int kind, bool allowed)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_set_loot_context refused: host only."); return false; }
		if ( !validItem(item) )
		{
			SAM_ERROR(MOD, "sam_set_loot_context: " + std::to_string(item) + " is not an item this game has.");
			return false;
		}
		if ( kind < 0 || kind >= KindCount )
		{
			SAM_ERROR(MOD, "sam_set_loot_context: unknown context. Valid: floor chest shop monster recipe console other.");
			return false;
		}
		ItemRule& r = s_items[item];
		const unsigned bit = 1u << kind;
		// Allowed is the default, so an allow simply takes back any deny: the rule can go back
		// to empty and the unmodded fast path stays unmodded.
		if ( allowed ) { r.contextSet &= ~bit; }
		else { r.contextSet |= bit; }
		r.contextAllowed &= ~bit;
		samDropIfEmpty(item);
		return true;
	}

	bool setSpellDroppable(int spellId, bool allowed, int minFloor)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_set_spell_droppable refused: host only."); return false; }
		if ( spellId < 0 || !getSpellFromID(spellId) )
		{
			SAM_ERROR(MOD, "sam_set_spell_droppable: " + std::to_string(spellId) + " is not a spell this game has.");
			return false;
		}
		if ( getSpellbookFromSpellID(spellId) < 0 && allowed )
		{
			// A spell with no book cannot come out of a spellbook re-roll however droppable it
			// is declared. Say so rather than accept an entry that can never take effect.
			SAM_WARN(MOD, "sam_set_spell_droppable: spell " + std::to_string(spellId)
				+ " has no spellbook or tome, so no roll can ever produce it. The entry was kept in case a mod adds one.");
		}
		SpellRule& r = s_spells[spellId];
		r.allowed = allowed;
		r.minFloor = std::max(0, minFloor);
		return true;
	}

	bool setFallback(int category, int item, int kind)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_set_loot_fallback refused: host only."); return false; }
		// -1 is "ANY": the any-category form of itemTypeWithinGoldValue (the lockpick reward).
		if ( category < -1 || category >= (int)CATEGORY_MAX )
		{
			SAM_ERROR(MOD, "sam_set_loot_fallback: not a category. Valid: ANY " + samCategoryHint());
			return false;
		}
		if ( kind >= KindCount )
		{
			SAM_ERROR(MOD, "sam_set_loot_fallback: unknown context. Valid: floor chest shop monster recipe console other.");
			return false;
		}
		const std::pair<int, int> key(category, kind < 0 ? -1 : kind);
		if ( item < 0 ) { s_fallbacks.erase(key); return true; }
		if ( !validItem(item) )
		{
			SAM_ERROR(MOD, "sam_set_loot_fallback: " + std::to_string(item) + " is not an item this game has.");
			return false;
		}
		s_fallbacks[key] = item;
		return true;
	}

	// ---- what the engine asks -----------------------------------------------------------------

	bool anyItemRules() { return !s_items.empty(); }
	bool anyWeightAboveOne() { return s_weightsAboveOne > 0; }

	int candidateWeight(int item)
	{
		if ( s_items.empty() ) { return 1; }
		auto it = s_items.find(item);
		if ( it == s_items.end() ) { return 1; }
		const ItemRule& r = it->second;
		if ( r.weight == 0 ) { return 0; }
		if ( r.hasRange )
		{
			if ( currentlevel < r.minFloor ) { return 0; }
			if ( r.maxFloor >= 0 && currentlevel > r.maxFloor ) { return 0; }
		}
		const int k = s_context.kind;
		if ( k >= 0 && k < KindCount && (r.contextSet & (1u << k)) && !(r.contextAllowed & (1u << k)) ) { return 0; }
		return r.weight;
	}

	// The vanilla draws, moved here verbatim so the RNG stream is what it was. Each mode is
	// one site: PickPlain is the chest, the general store and the troll; the two floor modes
	// are the "possible magicstaff" / "impossible magicstaff" branches of assignActions.
	static int samVanillaPick(BaronyRNG& rng, int mode)
	{
		if ( mode == PickFloorWithStaff )
		{
			int randType = rng.rand() % (Category::CATEGORY_MAX - 2);
			if ( randType == THROWN && rng.rand() % 3 ) // THROWN items 66% to be re-roll.
			{
				randType = rng.rand() % (Category::CATEGORY_MAX - 2);
			}
			if ( randType == BOOK && rng.rand() % 2 ) // BOOK items % to be re-roll, exclude book cat
			{
				randType = rng.rand() % (Category::CATEGORY_MAX - 3);
			}
			return randType;
		}
		if ( mode == PickFloorNoStaff )
		{
			int randType = rng.rand() % (Category::CATEGORY_MAX - 3);
			if ( randType >= MAGICSTAFF )
			{
				randType++;
			}
			if ( randType == THROWN && rng.rand() % 3 ) // THROWN items 66% to be re-roll.
			{
				randType = rng.rand() % (Category::CATEGORY_MAX - 3);
				if ( randType >= MAGICSTAFF )
				{
					randType++;
				}
			}
			if ( randType == BOOK && rng.rand() % 2 ) // BOOK items % to be re-roll, exclude book cat
			{
				randType = rng.rand() % (Category::CATEGORY_MAX - 4);
				if ( randType >= MAGICSTAFF )
				{
					randType++;
				}
			}
			return randType;
		}
		return rng.rand() % (Category::CATEGORY_MAX - 2); // exclude spell_cat
	}

	int pickCategory(BaronyRNG& rng, int mode)
	{
		if ( !s_anyCategoryWeight ) { return samVanillaPick(rng, mode); }
		// A weight replaces the vanilla re-roll quirks for that draw: THROWN and BOOK become
		// plain weights like every other category (documented). One draw, no re-rolls.
		std::vector<unsigned int> w;
		std::vector<int> cats;
		for ( int c = 0; c < (int)Category::CATEGORY_MAX - 2; ++c )
		{
			if ( mode == PickFloorNoStaff && c == MAGICSTAFF ) { continue; }
			auto it = s_categoryWeight.find(c);
			const int weight = ( it == s_categoryWeight.end() ) ? 1 : it->second;
			if ( weight <= 0 ) { continue; }
			w.push_back((unsigned int)weight);
			cats.push_back(c);
		}
		if ( cats.empty() )
		{
			// Every category weighted to nothing is a mod error, not a reason to hand out rocks.
			SAMNet::warnOnce("loot.catweights.allzero", "S.A.M: every loot category is weighted 0; the vanilla draw was used instead.");
			return samVanillaPick(rng, mode);
		}
		const int pick = rng.discrete(w.data(), (int)w.size());
		return cats[std::max(0, std::min((int)cats.size() - 1, pick))];
	}

	// What the last curve call returned when its pool was empty, so the post event can say
	// is_fallback without every handler comparing against GEM_ROCK (a legitimate garbage-chest
	// rock is not a fallback). Reset at every pre-roll, consumed by the post event.
	static bool s_lastRollWasFallback = false;
	static int s_lastFallbackType = -1;

	int fallback(int category)
	{
		int out = GEM_ROCK;
		if ( !s_fallbacks.empty() )
		{
			auto it = s_fallbacks.find(std::make_pair(category, s_context.kind));
			if ( it == s_fallbacks.end() ) { it = s_fallbacks.find(std::make_pair(category, -1)); }
			if ( it != s_fallbacks.end() && validItem(it->second) ) { out = it->second; }
		}
		s_lastRollWasFallback = true;
		s_lastFallbackType = out;
		return out;
	}

	bool spellAllowed(int spellId, int dropTable, bool hiddenFromUi, int itemLevel, bool allowHidden)
	{
		if ( !s_spells.empty() )
		{
			auto it = s_spells.find(spellId);
			if ( it != s_spells.end() )
			{
				// The mod's word replaces the sheet's: allowed from its floor, or never.
				return it->second.allowed && itemLevel >= it->second.minFloor;
			}
		}
		if ( hiddenFromUi && !allowHidden ) { return false; }
		return itemLevel >= dropTable;
	}

	// ---- events -------------------------------------------------------------------------------

	// Static initialisation runs on the main thread, before main(): this is the one thread the
	// script runtimes may be entered from. The level generator is not it (see the header).
	static const std::thread::id s_mainThread = std::this_thread::get_id();
	static int s_inEvent = 0;   // a loot event is being handled: no nested roll from inside it

	static bool samScriptsMayRun()
	{
		if ( multiplayer == CLIENT ) { return false; }
		if ( std::this_thread::get_id() != s_mainThread ) { return false; }
		return SamEvent::anyScripts();
	}

	struct EventDepth
	{
		EventDepth() { ++s_inEvent; }
		~EventDepth() { --s_inEvent; }
	};

	// The Item* an engine roll site is holding while one of its events runs. afterRoll writes
	// six fields into it after the handler returns, and the stocking loop that called the
	// post-process keeps using it after that; a handler that removed that very item from its
	// container would free memory the engine is about to write. removeFromContainer and
	// setShopStock compare against this and defer that one removal to the next frame's drain
	// (drainDeferredRemovals), where nothing on the stack holds it any more.
	static Item* s_heldItem = nullptr;

	struct HeldItemScope
	{
		explicit HeldItemScope(Item* it) : prev(s_heldItem) { s_heldItem = it; }
		~HeldItemScope() { s_heldItem = prev; }
		Item* prev;
	};

	static void samPutContext(SamEvent& e)
	{
		const Context& c = s_context;
		e.s("context", kindName(c.kind));
		e.i("floor", (long long)currentlevel);
		e.i("shop_type", c.kind == Shop ? c.shopType : -1);
		e.i("shop_uid", c.kind == Shop ? c.uid : -1);
		e.i("chest_uid", c.kind == Chest ? c.uid : -1);
		e.i("chest_type", c.kind == Chest ? c.chestType : -1);
		e.i("monster_uid", c.kind == Monster ? c.uid : -1);
		e.i("monster_type", c.kind == Monster ? c.monsterType : -1);
	}

	static int samReadForcedType(const SamEvent& e, const char* who)
	{
		const long long forced = e.get("item_type", -1);
		if ( forced < 0 ) { return -1; }
		if ( !validItem((int)forced) )
		{
			SAM_WARN(MOD, std::string(who) + ": a handler set item_type to " + std::to_string(forced)
				+ ", which is not an item this game has; the roll went ahead.");
			return -1;
		}
		return (int)forced;
	}

	static int samReadCategory(const SamEvent& e, int fallbackCat, bool allowAny, const char* who)
	{
		const std::string was = categoryName(fallbackCat);
		const std::string now = e.getStr("category", was);
		if ( now == was ) { return fallbackCat; }
		if ( allowAny && isAnyCategoryName(now) ) { return -1; }
		const int cat = categoryFromName(now);
		if ( cat < 0 || cat >= (int)CATEGORY_MAX )
		{
			SAM_WARN(MOD, std::string(who) + ": a handler set category to '" + now
				+ "', which is not one. Valid: " + samCategoryHint() + ". The original was kept.");
			return fallbackCat;
		}
		return cat;
	}

	int beforeRoll(int& category, int& minLevel, int& maxLevel)
	{
		s_lastRollWasFallback = false;
		s_lastFallbackType = -1;
		if ( !samScriptsMayRun() ) { return -1; }
		EventDepth depth;
		SamEvent e("world.on_before_loot_roll");
		e.s("category", categoryName(category));
		e.i("min_level", minLevel).i("max_level", maxLevel);
		e.i("min_value", -1).i("max_value", -1);
		e.i("item_type", -1);
		samPutContext(e);
		e.fire();   // not cancellable: every site makes an item of whatever comes back
		category = samReadCategory(e, category, false, "world.on_before_loot_roll");
		minLevel = (int)e.get("min_level", minLevel);
		maxLevel = (int)e.get("max_level", maxLevel);
		if ( maxLevel < minLevel )
		{
			SAM_WARN(MOD, "world.on_before_loot_roll: a handler left max_level below min_level ("
				+ std::to_string(minLevel) + ".." + std::to_string(maxLevel) + "); the pool will be empty.");
		}
		return samReadForcedType(e, "world.on_before_loot_roll");
	}

	int beforeValueRoll(int& category, int& minValue, int& maxValue)
	{
		s_lastRollWasFallback = false;
		s_lastFallbackType = -1;
		if ( !samScriptsMayRun() ) { return -1; }
		EventDepth depth;
		SamEvent e("world.on_before_loot_roll");
		e.s("category", categoryName(category));
		e.i("min_level", -1).i("max_level", -1);
		e.i("min_value", minValue).i("max_value", maxValue);
		e.i("item_type", -1);
		samPutContext(e);
		e.fire();
		category = samReadCategory(e, category, true, "world.on_before_loot_roll");
		minValue = (int)e.get("min_value", minValue);
		maxValue = (int)e.get("max_value", maxValue);
		return samReadForcedType(e, "world.on_before_loot_roll");
	}

	int afterRoll(Entity* my, Item* item, int originalType, bool spellbookRerolled)
	{
		const bool onEntity = ( my && my->behavior == &actItem );
		if ( !onEntity && !item ) { return originalType; }
		int type = onEntity ? my->skill[10] : (int)item->type;
		const bool wasFallback = s_lastRollWasFallback && s_lastFallbackType == originalType;
		s_lastRollWasFallback = false;
		s_lastFallbackType = -1;
		if ( !samScriptsMayRun() ) { return type; }
		EventDepth depth;
		HeldItemScope held(onEntity ? nullptr : item);   // written to below, after the handler returns
		int status = onEntity ? my->skill[11] : (int)item->status;
		int beatitude = onEntity ? my->skill[12] : (int)item->beatitude;
		int count = onEntity ? my->skill[13] : (int)item->count;
		long long appearance = onEntity ? (long long)(Uint32)my->skill[14] : (long long)item->appearance;
		int identified = onEntity ? (my->skill[15] ? 1 : 0) : (item->identified ? 1 : 0);

		SamEvent e("world.on_loot_rolled");
		e.i("item_type", type).i("status", status).i("beatitude", beatitude).i("count", count)
			.i("appearance", appearance).i("identified", identified)
			.i("is_fallback", wasFallback ? 1 : 0).i("was_spellbook_rerolled", spellbookRerolled ? 1 : 0);
		samPutContext(e);
		e.fire();

		const long long newType = e.get("item_type", type);
		if ( newType != type )
		{
			if ( validItem((int)newType) ) { type = (int)newType; }
			else
			{
				SAM_WARN(MOD, "world.on_loot_rolled: a handler set item_type to " + std::to_string(newType)
					+ ", which is not an item this game has; the rolled item was kept.");
			}
		}
		status = std::max((int)BROKEN, std::min((int)EXCELLENT, (int)e.get("status", status)));
		beatitude = std::max(-100, std::min(100, (int)e.get("beatitude", beatitude)));
		count = std::max(1, std::min(9999, (int)e.get("count", count)));
		appearance = e.get("appearance", appearance);
		identified = e.get("identified", identified) != 0 ? 1 : 0;

		if ( onEntity )
		{
			my->skill[10] = type;
			my->skill[11] = status;
			my->skill[12] = beatitude;
			my->skill[13] = count;
			my->skill[14] = (Sint32)(Uint32)appearance;
			my->skill[15] = identified;
		}
		else
		{
			item->type = static_cast<ItemType>(type);
			item->status = static_cast<Status>(status);
			item->beatitude = (Sint16)beatitude;
			item->count = (Sint16)count;
			item->appearance = (Uint32)appearance;
			item->identified = ( identified != 0 );
		}
		return type;
	}

	bool beforeSpellbookReroll(Item* item, int itemType, int itemLevel, int& forcedSpell, bool& allowHidden)
	{
		forcedSpell = -1;
		allowHidden = false;
		if ( !samScriptsMayRun() ) { return false; }
		EventDepth depth;
		HeldItemScope held(item);   // the post-process writes the re-rolled type into it next
		SamEvent e("world.on_before_spellbook_reroll");
		e.i("item_type", itemType).i("item_level", itemLevel);
		e.i("keep", 0).i("spell_id", -1).i("allow_hidden", 0);
		samPutContext(e);
		e.fire();
		const bool keep = e.get("keep", 0) != 0;
		allowHidden = e.get("allow_hidden", 0) != 0;
		const long long spell = e.get("spell_id", -1);
		if ( spell >= 0 )
		{
			if ( getSpellFromID((int)spell) && getSpellbookFromSpellID((int)spell) >= 0 ) { forcedSpell = (int)spell; }
			else
			{
				SAM_WARN(MOD, "world.on_before_spellbook_reroll: a handler set spell_id to " + std::to_string(spell)
					+ ", which is not a spell with a book; the normal re-roll went ahead.");
			}
		}
		return keep;
	}

	static int samCountList(list_t* inv)
	{
		int n = 0;
		if ( !inv ) { return 0; }
		for ( node_t* node = inv->first; node != nullptr; node = node->next ) { if ( node->element ) { ++n; } }
		return n;
	}

	static list_t* samInventoryOf(Entity* e)
	{
		if ( !e ) { return nullptr; }
		if ( e->behavior == &actChest ) { return e->getChestInventoryList(); }
		if ( e->behavior == &actMonster )
		{
			if ( Stat* s = e->getStats() ) { return &s->inventory; }
		}
		return nullptr;
	}

	bool beforeChestFill(Entity* my, int& chestType, int& minQuality)
	{
		if ( !my || !samScriptsMayRun() ) { return true; }
		EventDepth depth;
		SamEvent e("world.on_before_chest_fill");
		e.i("chest_uid", (long long)my->getUID());
		e.i("is_mimic", my->behavior == &actMonster ? 1 : 0);
		e.i("x", (long long)(my->x / 16)).i("y", (long long)(my->y / 16));
		e.i("floor", (long long)currentlevel);
		e.i("chest_type", chestType).i("min_quality", minQuality);
		const bool fill = e.fire();
		chestType = std::max(0, std::min(8, (int)e.get("chest_type", chestType)));
		minQuality = std::max(0, (int)e.get("min_quality", minQuality));
		return fill;
	}

	void afterChestFill(Entity* my, int chestType)
	{
		if ( !my || !samScriptsMayRun() ) { return; }
		EventDepth depth;
		SamEvent e("world.on_chest_filled");
		e.i("chest_uid", (long long)my->getUID());
		e.i("is_mimic", my->behavior == &actMonster ? 1 : 0);
		e.i("x", (long long)(my->x / 16)).i("y", (long long)(my->y / 16));
		e.i("floor", (long long)currentlevel);
		e.i("chest_type", chestType);
		e.i("item_count", samCountList(samInventoryOf(my)));
		e.fire();
	}

	bool beforeShopStock(Entity* my, Stat* stats, int& storeType, int& numItems, int& shopLevel, int& blessed)
	{
		(void)stats;
		if ( !my || !samScriptsMayRun() ) { return true; }
		EventDepth depth;
		SamEvent e("world.on_before_shop_stock");
		e.i("shopkeeper_uid", (long long)my->getUID());
		e.i("floor", (long long)currentlevel);
		e.i("store_type", storeType).i("num_items", numItems).i("shop_level", shopLevel).i("blessed", blessed);
		const bool stock = e.fire();
		// Each clamp applies only to a value a handler actually changed. The editor's custom
		// shopkeeper flags carry up to 254 items and a bless of 15, which the engine accepts,
		// and a mod that registers nothing must stock that shopkeeper exactly as no mod would.
		const long long st = e.get("store_type", storeType);
		if ( st != storeType ) { storeType = (int)std::max(-1LL, std::min(10LL, st)); }
		const long long n = e.get("num_items", numItems);
		if ( n != numItems ) { numItems = (int)std::max(0LL, std::min(60LL, n)); }
		const long long lvl = e.get("shop_level", shopLevel);
		if ( lvl != shopLevel ) { shopLevel = (int)std::max(0LL, std::min((long long)INT32_MAX, lvl)); }
		const long long bl = e.get("blessed", blessed);
		if ( bl != blessed ) { blessed = (int)std::max(1LL, std::min(3LL, bl)); }
		if ( !stock ) { storeType = -1; }
		return stock;
	}

	void afterShopStock(Entity* my, Stat* stats)
	{
		if ( !my || !stats || !samScriptsMayRun() ) { return; }
		EventDepth depth;
		SamEvent e("world.on_shop_stocked");
		e.i("shopkeeper_uid", (long long)my->getUID());
		e.i("store_type", (long long)my->monsterStoreType);
		e.i("floor", (long long)currentlevel);
		e.i("item_count", samCountList(&stats->inventory));
		e.fire();
	}

	void afterMonsterInit(Entity* my, Stat* stats)
	{
		if ( !my || !stats || !samScriptsMayRun() ) { return; }
		EventDepth depth;
		const int type = (int)stats->type;
		SamEvent e("world.on_monster_inventory");
		e.i("monster_uid", (long long)my->getUID());
		e.i("monster_type", type);
		e.s("monster_name", std::string(stats->name[0] ? stats->name
			: ((type >= 0 && type < NUMMONSTERS) ? monstertypename[type] : "")));
		e.i("floor", (long long)currentlevel);
		e.i("item_count", samCountList(&stats->inventory));
		int worn = 0;
		Item* slots[] = { stats->helmet, stats->breastplate, stats->gloves, stats->shoes, stats->shield,
			stats->weapon, stats->cloak, stats->amulet, stats->ring, stats->mask };
		for ( Item* s : slots ) { if ( s ) { ++worn; } }
		e.i("worn_count", worn);
		e.i("is_shopkeeper", ( type == SHOPKEEPER || my->monsterCanTradeWith(-1) ) ? 1 : 0);
		e.fire();
	}

	bool fixtureLoot(const char* source, int player, int x, int y, int& type, int& status, int& beatitude, int& count)
	{
		if ( !samScriptsMayRun() ) { return true; }
		EventDepth depth;
		SamEvent e("world.on_fixture_loot");
		e.s("source", source ? source : "?");
		e.i("player", player).i("x", x).i("y", y).i("floor", (long long)currentlevel);
		e.i("item_type", type).i("status", status).i("beatitude", beatitude).i("count", count);
		const bool make = e.fire();
		const long long newType = e.get("item_type", type);
		if ( newType != type )
		{
			if ( validItem((int)newType) ) { type = (int)newType; }
			else
			{
				SAM_WARN(MOD, "world.on_fixture_loot: a handler set item_type to " + std::to_string(newType)
					+ ", which is not an item this game has; the original was kept.");
			}
		}
		status = std::max((int)BROKEN, std::min((int)EXCELLENT, (int)e.get("status", status)));
		beatitude = std::max(-100, std::min(100, (int)e.get("beatitude", beatitude)));
		count = std::max(1, std::min(9999, (int)e.get("count", count)));
		return make;
	}

	// ---- reads and rolls ----------------------------------------------------------------------

	bool pool(int category, int minLevel, int maxLevel, int kind, std::vector<PoolEntry>& out)
	{
		out.clear();
		if ( category < 0 || category >= (int)CATEGORY_MAX )
		{
			SAM_ERROR(MOD, "sam_get_loot_pool: not a category. Valid: " + samCategoryHint());
			return false;
		}
		// The context test wants a context on the stack; -1 means "do not test it", which a
		// kind no rule can name gives us for free.
		ContextScope scope(kind < 0 ? (int)Other : kind);
		const bool testContext = ( kind >= 0 );
		auto consider = [&](int c)
		{
			const ItemGeneric& g = items[c];
			if ( (int)g.category != category ) { return; }
			if ( g.level == -1 ) { return; }
			if ( g.level < minLevel || g.level > maxLevel ) { return; }
			int w = 1;
			if ( !s_items.empty() )
			{
				if ( testContext ) { w = candidateWeight(c); }
				else
				{
					auto it = s_items.find(c);
					if ( it != s_items.end() )
					{
						const ItemRule& r = it->second;
						w = r.weight;
						if ( w > 0 && r.hasRange && (currentlevel < r.minFloor || (r.maxFloor >= 0 && currentlevel > r.maxFloor)) ) { w = 0; }
					}
				}
			}
			if ( w <= 0 ) { return; }
			PoolEntry p;
			p.type = c;
			const char* nm = g.getIdentifiedName();
			p.name = nm ? nm : "";
			p.level = g.level;
			p.weight = w;
			out.push_back(p);
		};
		for ( int c = 0; c < NUMITEMS; ++c ) { consider(c); }
		std::vector<int> modded;
		SAMItems::lootCandidates(category, minLevel, maxLevel, modded);
		for ( int c : modded ) { consider(c); }
		return true;
	}

	int roll(int category, int minLevel, int maxLevel, int kind)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_roll_loot refused: host only."); return -1; }
		if ( category < 0 || category >= (int)CATEGORY_MAX )
		{
			SAM_ERROR(MOD, "sam_roll_loot: not a category. Valid: " + samCategoryHint());
			return -1;
		}
		if ( s_inEvent > 0 )
		{
			// Both runtimes keep ONE "last event" store for write-backs; a roll from inside a loot
			// handler would fire a nested event into it and wipe what the outer handler wrote.
			SAMNet::warnOnce("loot.roll.nested", "S.A.M: sam_roll_loot cannot be called from inside a loot event handler; the call was refused.");
			return -1;
		}
		ContextScope scope(kind < 0 ? (int)Other : kind);
		// The game's own local generator, not a fresh one seeded from the clock: seedTime()
		// gives every call made inside the same time slice the same seed, so a script rolling
		// two hundred times in one tick got the same item two hundred times. LootTest caught it.
		BaronyRNG& rng = local_rng;
		const ItemType t = itemLevelCurve(static_cast<Category>(category), minLevel, maxLevel, rng);
		Item* tmp = newItem(t, EXCELLENT, 0, 1, rng.rand(), false, nullptr);
		if ( !tmp ) { return (int)t; }
		itemLevelCurvePostProcess(nullptr, tmp, rng, currentlevel, nullptr, nullptr);
		const int type = (int)tmp->type;
		free(tmp);
		return type;
	}

	// ---- containers ---------------------------------------------------------------------------

	static bool samIsShopkeeper(Entity* e)
	{
		if ( !e || e->behavior != &actMonster ) { return false; }
		Stat* s = e->getStats();
		if ( s && s->type == SHOPKEEPER ) { return true; }
		return e->monsterCanTradeWith(-1);
	}

	static void samCloseShopWindows(Entity* e)
	{
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( shopkeeper[p] == e->getUID() )
			{
				if ( multiplayer == SERVER && p > 0 && !client_disconnected[p] && players[p] && !players[p]->isLocalPlayer() )
				{
					// closeShop resets only the host's side of the trade. Left alone, that player's
					// window still lists the old stock and its Buy button still works: the client
					// takes the item and its gold locally, then the host charges for whatever sits
					// in that slot now. The engine's own "the shopkeeper walked away" path
					// (entity.cpp, monster_shopkeeper.cpp) tells the browser with 'SHPC'; a stock
					// 5.0.2 client understands it and closes the window, and the next talk serves
					// the stock as it now is.
					strcpy((char*)net_packet->data, "SHPC");
					SDLNet_Write32(e->getUID(), &net_packet->data[4]);
					net_packet->address.host = net_clients[p - 1].host;
					net_packet->address.port = net_clients[p - 1].port;
					net_packet->len = 8;
					sendPacketSafe(net_sock, -1, net_packet, p - 1);
				}
				closeShop(p);
			}
		}
	}

	static void samStandardPotionAppearance(Item* item)
	{
		if ( !item || itemCategory(item) != POTION ) { return; }
		for ( size_t p = 0; p < potionStandardAppearanceMap.size(); ++p )
		{
			if ( potionStandardAppearanceMap[p].first == item->type )
			{
				item->appearance = potionStandardAppearanceMap[p].second;
			}
		}
	}

	// The shopkeeper's slot layout as initShopkeeper lays it out (monster_shopkeeper.cpp): the
	// stock sorted by price, dearest first, filling from the top left; the data-driven
	// consumables from the bottom right backwards. Run again after every change so an added
	// item has a slot the shop window can show. The mysterious merchant keeps his fixed
	// arrangement: new items there take the next free slot instead.
	static void samNextFreeSlot(const std::set<int>& taken, int& x, int& y, int maxX, int maxY)
	{
		for ( int yy = 0; yy < maxY; ++yy )
		{
			for ( int xx = 0; xx < maxX; ++xx )
			{
				if ( taken.find(xx + yy * 100) == taken.end() ) { x = xx; y = yy; return; }
			}
		}
		x = 0; y = 0;   // full: overlap the first slot rather than invent a row the window cannot draw
	}

	static void samLayoutShop(Entity* e, Stat* s)
	{
		if ( !e || !s ) { return; }
		const int maxX = Player::ShopGUI_t::MAX_SHOP_X;
		const int maxY = Player::ShopGUI_t::MAX_SHOP_Y;
		if ( e->monsterStoreType == 10 )
		{
			std::set<int> taken;
			std::vector<Item*> unplaced;
			for ( node_t* node = s->inventory.first; node != nullptr; node = node->next )
			{
				Item* item = (Item*)node->element;
				if ( !item ) { continue; }
				const int key = item->x + item->y * 100;
				if ( item->x < 0 || item->y < 0 || taken.find(key) != taken.end() ) { unplaced.push_back(item); }
				else { taken.insert(key); }
			}
			for ( Item* item : unplaced )
			{
				int x = 0, y = 0;
				samNextFreeSlot(taken, x, y, maxX, maxY);
				item->x = x; item->y = y;
				taken.insert(x + y * 100);
			}
			return;
		}
		std::vector<std::pair<int, Item*>> priced;
		std::vector<Item*> consumables;
		for ( node_t* node = s->inventory.first; node != nullptr; node = node->next )
		{
			Item* item = (Item*)node->element;
			if ( !item ) { continue; }
			if ( item->itemSpecialShopConsumable ) { consumables.push_back(item); }
			else { priced.push_back(std::make_pair(item->buyValue(clientnum), item)); }
		}
		std::sort(priced.begin(), priced.end(), [](const std::pair<int, Item*>& lhs, const std::pair<int, Item*>& rhs) {
			return lhs.first > rhs.first;
		});
		int slotx = 0, sloty = 0;
		std::set<int> takenSlots;
		for ( auto& v : priced )
		{
			Item* item = v.second;
			item->x = slotx;
			item->y = sloty;
			takenSlots.insert(slotx + sloty * 100);
			++slotx;
			if ( slotx >= maxX ) { slotx = 0; ++sloty; }
			if ( sloty >= maxY ) { break; }
		}
		slotx = maxX - 1;
		sloty = maxY - 1;
		for ( auto it = consumables.rbegin(); it != consumables.rend(); ++it )
		{
			if ( takenSlots.find(slotx + sloty * 100) != takenSlots.end() ) { break; }
			(*it)->x = slotx;
			(*it)->y = sloty;
			--slotx;
			if ( slotx < 0 ) { slotx = maxX - 1; --sloty; }
			if ( sloty < 0 ) { break; }
		}
	}

	static void samChestSlot(list_t* inv, Item* placed)
	{
		std::set<int> taken;
		for ( node_t* node = inv->first; node != nullptr; node = node->next )
		{
			Item* item = (Item*)node->element;
			if ( !item || item == placed ) { continue; }
			taken.insert(item->x + item->y * 100);
		}
		int x = 0, y = 0;
		samNextFreeSlot(taken, x, y, Player::Inventory_t::MAX_CHEST_X, 64);
		placed->x = x;
		placed->y = y;
	}

	static Entity* samResolveContainer(std::uint32_t uid, const char* who)
	{
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e )
		{
			SAM_WARN(MOD, std::string(who) + ": no entity with uid " + std::to_string(uid) + ".");
			return nullptr;
		}
		if ( e->behavior != &actChest && e->behavior != &actMonster )
		{
			SAM_WARN(MOD, std::string(who) + ": uid " + std::to_string(uid) + " is not a chest or a creature.");
			return nullptr;
		}
		return e;
	}

	long long addToContainer(std::uint32_t uid, int type, int count, int status, int beatitude, bool identified, long long appearance)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_add_item_to_container refused: host only."); return -1; }
		Entity* e = samResolveContainer(uid, "sam_add_item_to_container");
		if ( !e ) { return -1; }
		if ( !validItem(type) )
		{
			SAM_ERROR(MOD, "sam_add_item_to_container: " + std::to_string(type) + " is not an item this game has.");
			return -1;
		}
		if ( items[type].category == SPELL_CAT )
		{
			SAM_ERROR(MOD, "sam_add_item_to_container: " + samItemLabel(type) + " is a spell, not a thing that can sit in a container.");
			return -1;
		}
		count = std::max(1, std::min(9999, count));
		status = std::max((int)BROKEN, std::min((int)EXCELLENT, status));
		beatitude = std::max(-100, std::min(100, beatitude));
		const Uint32 app = ( appearance < 0 ) ? (Uint32)local_rng.rand() : (Uint32)appearance;

		if ( e->behavior == &actChest )
		{
			list_t* inv = e->getChestInventoryList();
			if ( !inv )
			{
				SAM_WARN(MOD, "sam_add_item_to_container: chest " + std::to_string(uid) + " has no inventory list yet.");
				return -1;
			}
			if ( e->chestStatus )
			{
				// Open: the engine's own path, which also tells the opener's machine -- but only
				// for a NEW stack ('CITM' is sent after the merge branch has returned), so an
				// opener on another machine gets a new stack every time: their window would
				// otherwise show 1 of a stack the chest holds 2 of, and their GUI re-arranges a
				// slot collision itself. The host's own window reads the chest list directly, so
				// a merge there is fine.
				bool remoteOpener = false;
				for ( int p = 0; p < MAXPLAYERS; ++p )
				{
					if ( openedChest[p] == e && multiplayer == SERVER && players[p] && !players[p]->isLocalPlayer() ) { remoteOpener = true; }
				}
				Item* it = newItem(static_cast<ItemType>(type), static_cast<Status>(status), (Sint16)beatitude, (Sint16)count, app, identified, nullptr);
				if ( !it ) { return -1; }
				Item* placed = e->addItemToChest(it, remoteOpener, nullptr);
				if ( !placed ) { free(it); return -1; }
				if ( placed != it ) { free(it); }   // merged into a stack already there
				return (long long)placed->uid;
			}
			Item* it = newItem(static_cast<ItemType>(type), static_cast<Status>(status), (Sint16)beatitude, (Sint16)count, app, identified, inv);
			if ( !it ) { return -1; }
			samChestSlot(inv, it);
			return (long long)it->uid;
		}

		Stat* s = e->getStats();
		if ( !s )
		{
			SAM_WARN(MOD, "sam_add_item_to_container: creature " + std::to_string(uid) + " has no stats.");
			return -1;
		}
		const bool shop = samIsShopkeeper(e);
		if ( shop ) { samCloseShopWindows(e); }
		Item* it = newItem(static_cast<ItemType>(type), static_cast<Status>(status), (Sint16)beatitude, (Sint16)count, app, identified, &s->inventory);
		if ( !it ) { return -1; }
		it->itemSpecialShopConsumable = false;
		if ( shop )
		{
			samStandardPotionAppearance(it);
			samLayoutShop(e, s);
		}
		return (long long)it->uid;
	}

	// The removal itself: close whatever window shows the container, take the item (or part of
	// the stack) out, and re-lay a shop. Called on the spot, or from the drain for the one item
	// an event was holding.
	static void samRemoveNow(Entity* e, Item* found, int count)
	{
		if ( e->behavior == &actChest && e->chestStatus )
		{
			// Somebody is looking at it. There is no packet for "the host took this out", so the
			// window is closed; the next open serves the contents as they now are.
			e->closeChest();
		}
		const bool shop = samIsShopkeeper(e);
		if ( shop ) { samCloseShopWindows(e); }
		if ( count <= 0 || count >= found->count ) { list_RemoveNode(found->node); }
		else { found->count -= (Sint16)count; }
		if ( shop ) { if ( Stat* s = e->getStats() ) { samLayoutShop(e, s); } }
	}

	// Keyed by uids, not pointers: between now and the drain the container may be gone or the
	// item taken by ordinary play, and a stale uid simply fails to resolve where a stale
	// pointer would be undetectable.
	struct PendingRemoval { std::uint32_t container; std::uint32_t item; int count; };
	static std::vector<PendingRemoval> s_pendingRemovals;

	static void samDeferRemoval(Entity* e, Item* found, int count)
	{
		for ( PendingRemoval& p : s_pendingRemovals )
		{
			if ( p.container == e->getUID() && p.item == found->uid )
			{
				// Asked twice from the same handler: the larger removal wins (0 is the whole stack).
				if ( count <= 0 || p.count <= 0 ) { p.count = 0; } else { p.count = std::max(p.count, count); }
				return;
			}
		}
		s_pendingRemovals.push_back({ e->getUID(), found->uid, count });
		SAMNet::warnOnce("loot.remove.deferred", "S.A.M: an item was removed from inside the loot event that is about it"
			" (world.on_loot_rolled / world.on_before_spellbook_reroll); the engine is still writing to that item, so the"
			" removal is done at the next frame instead. It is gone before anyone can see it.");
	}

	void drainDeferredRemovals()
	{
		if ( s_pendingRemovals.empty() ) { return; }
		// Swapped out first: a removal closes windows and re-lays a shop, and anything queued
		// during this pass belongs to the next frame, not to an unbounded loop here.
		std::vector<PendingRemoval> batch;
		batch.swap(s_pendingRemovals);
		for ( const PendingRemoval& p : batch )
		{
			Entity* e = uidToEntity((Sint32)p.container);
			if ( !e || (e->behavior != &actChest && e->behavior != &actMonster) ) { continue; }
			list_t* inv = samInventoryOf(e);
			if ( !inv ) { continue; }
			Item* found = nullptr;
			for ( node_t* node = inv->first; node != nullptr && !found; node = node->next )
			{
				Item* item = (Item*)node->element;
				if ( item && item->uid == p.item ) { found = item; }
			}
			if ( !found || !found->node ) { continue; }   // ordinary play got there first; nothing to do
			samRemoveNow(e, found, p.count);
		}
	}

	bool removeFromContainer(std::uint32_t uid, long long typeOrUid, int count)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_remove_item_from_container refused: host only."); return false; }
		Entity* e = samResolveContainer(uid, "sam_remove_item_from_container");
		if ( !e ) { return false; }
		list_t* inv = samInventoryOf(e);
		if ( !inv )
		{
			SAM_WARN(MOD, "sam_remove_item_from_container: " + std::to_string(uid) + " has no inventory list.");
			return false;
		}
		// An item uid first (every uid is above any item type), then a type.
		Item* found = nullptr;
		for ( node_t* node = inv->first; node != nullptr && !found; node = node->next )
		{
			Item* item = (Item*)node->element;
			if ( item && (long long)item->uid == typeOrUid ) { found = item; }
		}
		if ( !found && typeOrUid >= 0 && typeOrUid < NUM_ITEM_SLOTS )
		{
			// The event's own item first. A handler inside world.on_loot_rolled that says "remove
			// type T" means the item the event is about, and a shop that already holds an earlier T
			// from a previous roll must not hand over that one instead: it did, the earlier item
			// went at once and the held one stayed, and LootTest caught it on the first type that
			// happened to roll twice.
			if ( s_heldItem && (long long)s_heldItem->type == typeOrUid && s_heldItem->node && s_heldItem->node->list == inv )
			{
				found = s_heldItem;
			}
			for ( node_t* node = inv->first; node != nullptr && !found; node = node->next )
			{
				Item* item = (Item*)node->element;
				if ( item && (long long)item->type == typeOrUid ) { found = item; }
			}
		}
		if ( !found || !found->node )
		{
			SAM_WARN(MOD, "sam_remove_item_from_container: " + std::to_string(uid) + " holds no item " + std::to_string(typeOrUid) + ".");
			return false;
		}
		if ( found == s_heldItem )
		{
			samDeferRemoval(e, found, count);
			return true;
		}
		samRemoveNow(e, found, count);
		return true;
	}

	bool setShopStock(std::uint32_t uid, const std::vector<StockEntry>& entries)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_set_shop_stock refused: host only."); return false; }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e || !samIsShopkeeper(e) )
		{
			SAM_WARN(MOD, "sam_set_shop_stock: uid " + std::to_string(uid) + " is not a shopkeeper.");
			return false;
		}
		Stat* s = e->getStats();
		if ( !s ) { return false; }
		// Validate the whole list before touching the shop, so a false means it is untouched.
		for ( size_t i = 0; i < entries.size(); ++i )
		{
			if ( !validItem(entries[i].type) || items[entries[i].type].category == SPELL_CAT )
			{
				SAM_ERROR(MOD, "sam_set_shop_stock: entry " + std::to_string(i + 1) + " (" + std::to_string(entries[i].type)
					+ ") is not an item this game has. Nothing was changed.");
				return false;
			}
		}
		samCloseShopWindows(e);
		node_t* nextnode = nullptr;
		for ( node_t* node = s->inventory.first; node != nullptr; node = nextnode )
		{
			nextnode = node->next;
			Item* item = (Item*)node->element;
			if ( !item ) { continue; }
			if ( item->itemSpecialShopConsumable ) { continue; }   // the data-driven consumables stay
			if ( item == s_heldItem )
			{
				// The item the running event is about: the engine still writes to it, so it
				// leaves at the next frame's drain and the new stock is laid out around it.
				samDeferRemoval(e, item, 0);
				continue;
			}
			list_RemoveNode(node);
		}
		for ( const StockEntry& en : entries )
		{
			const int count = std::max(1, std::min(9999, en.count));
			const int status = std::max((int)BROKEN, std::min((int)EXCELLENT, en.status));
			const int beat = std::max(-100, std::min(100, en.beatitude));
			Item* it = newItem(static_cast<ItemType>(en.type), static_cast<Status>(status), (Sint16)beat, (Sint16)count,
				(Uint32)local_rng.rand(), en.identified, &s->inventory);
			if ( it )
			{
				it->itemSpecialShopConsumable = false;
				samStandardPotionAppearance(it);
			}
		}
		samLayoutShop(e, s);
		return true;
	}

	bool setShopType(std::uint32_t uid, int storeType)
	{
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN(MOD, "sam_set_shop_type refused: host only."); return false; }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e || e->behavior != &actMonster )
		{
			SAM_WARN(MOD, "sam_set_shop_type: uid " + std::to_string(uid) + " is not a creature.");
			return false;
		}
		Stat* s = e->getStats();
		if ( !s || s->type != SHOPKEEPER )
		{
			SAM_WARN(MOD, "sam_set_shop_type: uid " + std::to_string(uid) + " is not a shopkeeper.");
			return false;
		}
		if ( storeType < 0 || storeType > 10 )
		{
			SAM_ERROR(MOD, "sam_set_shop_type: store types run 0..10 (0 arms, 1 hats, 2 jewelry, 3 books, 4 apothecary,"
				" 5 staves, 6 food, 7 hardware, 8 hunting, 9 general, 10 mysterious); got " + std::to_string(storeType) + ".");
			return false;
		}
		// skill[3] is MONSTER_INIT: 0 before the species init has run, 1 while stats are being made,
		// 2 once initShopkeeper has stocked the shop. The store type is read once, at init.
		if ( e->skill[3] >= 2 )
		{
			SAM_WARN(MOD, "sam_set_shop_type: shopkeeper " + std::to_string(uid) + " has already stocked its shop;"
				" its type cannot change now. Use world.on_before_shop_stock, or sam_set_shop_stock for the stock itself.");
			return false;
		}
		s->MISC_FLAGS[STAT_FLAG_NPC] = ( storeType == 10 ) ? 14 : 1 + storeType;
		return true;
	}

	// ---- teardown ----------------------------------------------------------------------------

	void clear()
	{
		s_items.clear();
		s_weightsAboveOne = 0;
		s_categoryWeight.clear();
		s_anyCategoryWeight = false;
		s_spells.clear();
		s_fallbacks.clear();
		s_lastRollWasFallback = false;
		s_lastFallbackType = -1;
		s_pendingRemovals.clear();
	}

	// ---- multiplayer: the tables reach a client that joins -------------------------------------
	//
	// The `all` replay only holds calls made during the current run (SAMNet::clear empties it at
	// doNewGame). A weight set at the main menu, from a mod.on_setting_changed handler or in an
	// earlier run is therefore on the host and nowhere else, and a joiner who rolls its own floor
	// from the same seed puts a different item on the same tile. So every HELLO sends the four
	// tables whole, exactly as samItemsSendPatches sends the item patches: the client drops what
	// it holds and takes the host's copy, touching no RNG. Op RulesFirst + 10; the number is
	// reserved beside the others in SAMRules::NetOp (sam_rules.hpp) as LootTables.
	// Body: [u16 n] per item rule (u32 id, u32 weight, u8 has_range, u32 min_floor, u32 max_floor
	// as its signed bit pattern, u32 context_set, u32 context_allowed); [u16 n] per category
	// weight (u32 category, u32 weight); [u16 n] per spell rule (u32 spell, u8 allowed,
	// u32 min_floor); [u16 n] per fallback (u32 category, u32 kind -- both signed bit patterns,
	// -1 is ANY / every context -- u32 item).
	static std::uint8_t samLootTablesOp() { return (std::uint8_t)(SAMNet::Op::RulesFirst + SAMRules::NetOp::LootTables); }

	static void samLootSendTables(int peer)
	{
		if ( multiplayer != SERVER ) { return; }
		SAMNet::Writer w;
		w.u16((std::uint16_t)std::min<size_t>(s_items.size(), 65535));
		{
			int n = 0;
			for ( const auto& kv : s_items )
			{
				if ( n++ >= 65535 ) { break; }
				const ItemRule& r = kv.second;
				w.u32((std::uint32_t)kv.first);
				w.u32((std::uint32_t)(std::int32_t)r.weight);
				w.u8(r.hasRange ? 1 : 0);
				w.u32((std::uint32_t)(std::int32_t)r.minFloor);
				w.u32((std::uint32_t)(std::int32_t)r.maxFloor);
				w.u32((std::uint32_t)r.contextSet);
				w.u32((std::uint32_t)r.contextAllowed);
			}
		}
		w.u16((std::uint16_t)s_categoryWeight.size());
		for ( const auto& kv : s_categoryWeight )
		{
			w.u32((std::uint32_t)(std::int32_t)kv.first);
			w.u32((std::uint32_t)(std::int32_t)kv.second);
		}
		w.u16((std::uint16_t)std::min<size_t>(s_spells.size(), 65535));
		{
			int n = 0;
			for ( const auto& kv : s_spells )
			{
				if ( n++ >= 65535 ) { break; }
				w.u32((std::uint32_t)(std::int32_t)kv.first);
				w.u8(kv.second.allowed ? 1 : 0);
				w.u32((std::uint32_t)(std::int32_t)kv.second.minFloor);
			}
		}
		w.u16((std::uint16_t)std::min<size_t>(s_fallbacks.size(), 65535));
		{
			int n = 0;
			for ( const auto& kv : s_fallbacks )
			{
				if ( n++ >= 65535 ) { break; }
				w.u32((std::uint32_t)(std::int32_t)kv.first.first);
				w.u32((std::uint32_t)(std::int32_t)kv.first.second);
				w.u32((std::uint32_t)(std::int32_t)kv.second);
			}
		}
		SAMNet::sendToClient(peer, samLootTablesOp(), w.buf);
	}

	static void samLootOnTables(const std::string& body)
	{
		SAMNet::Reader r(body);
		// Read everything before touching a table: half the host's tables are worse than ours.
		std::map<int, ItemRule> itemsIn;
		std::map<int, int> catsIn;
		std::map<int, SpellRule> spellsIn;
		std::map<std::pair<int, int>, int> fallbacksIn;
		const int nItems = r.u16();
		for ( int i = 0; i < nItems && r.ok; ++i )
		{
			const int id = (int)(std::int32_t)r.u32();
			ItemRule rule;
			rule.weight = (int)(std::int32_t)r.u32();
			rule.hasRange = r.u8() != 0;
			rule.minFloor = (int)(std::int32_t)r.u32();
			rule.maxFloor = (int)(std::int32_t)r.u32();
			rule.contextSet = r.u32();
			rule.contextAllowed = r.u32();
			// The same tests the setters make, so a bad frame cannot put in what a call could not.
			if ( !validItem(id) || rule.weight < 0 ) { continue; }
			rule.weight = std::min(rule.weight, kMaxWeight);
			rule.minFloor = std::max(0, rule.minFloor);
			if ( rule.maxFloor >= 0 && rule.maxFloor < rule.minFloor ) { rule.hasRange = false; rule.minFloor = 0; rule.maxFloor = -1; }
			rule.contextSet &= (1u << KindCount) - 1u;
			rule.contextAllowed &= rule.contextSet;
			if ( !rule.empty() ) { itemsIn[id] = rule; }
		}
		const int nCats = r.u16();
		for ( int i = 0; i < nCats && r.ok; ++i )
		{
			const int cat = (int)(std::int32_t)r.u32();
			const int weight = (int)(std::int32_t)r.u32();
			if ( cat < 0 || cat >= (int)CATEGORY_MAX - 2 || weight < 0 || weight == 1 ) { continue; }
			catsIn[cat] = std::min(weight, kMaxWeight);
		}
		const int nSpells = r.u16();
		for ( int i = 0; i < nSpells && r.ok; ++i )
		{
			const int spell = (int)(std::int32_t)r.u32();
			SpellRule rule;
			rule.allowed = r.u8() != 0;
			rule.minFloor = std::max(0, (int)(std::int32_t)r.u32());
			if ( spell < 0 || !getSpellFromID(spell) ) { continue; }
			spellsIn[spell] = rule;
		}
		const int nFallbacks = r.u16();
		for ( int i = 0; i < nFallbacks && r.ok; ++i )
		{
			const int cat = (int)(std::int32_t)r.u32();
			const int kind = (int)(std::int32_t)r.u32();
			const int item = (int)(std::int32_t)r.u32();
			if ( cat < -1 || cat >= (int)CATEGORY_MAX || kind < -1 || kind >= KindCount || !validItem(item) ) { continue; }
			fallbacksIn[std::make_pair(cat, kind)] = item;
		}
		if ( !r.ok || multiplayer != CLIENT ) { return; }
		s_items.swap(itemsIn);
		samRecountWeights();
		s_categoryWeight.swap(catsIn);
		s_anyCategoryWeight = !s_categoryWeight.empty();
		s_spells.swap(spellsIn);
		s_fallbacks.swap(fallbacksIn);
		SAM_INFO(MOD, "Took the host's loot tables (" + std::to_string(s_items.size()) + " item rule(s), "
			+ std::to_string(s_categoryWeight.size()) + " category weight(s), " + std::to_string(s_spells.size())
			+ " spell rule(s), " + std::to_string(s_fallbacks.size()) + " fallback(s)).");
	}

	// Transient state dies with the game; the tables are rules and outlive it (see the header).
	// A deferred removal is keyed by uids the next run hands out again, so it dies with it too.
	static void samLootNetClear()
	{
		s_lastRollWasFallback = false;
		s_lastFallbackType = -1;
		s_pendingRemovals.clear();
	}
	struct SamLootNet
	{
		SamLootNet()
		{
			SAMNet::addClearHook(&samLootNetClear);
			SAMNet::onClientOp(samLootTablesOp(), &samLootOnTables);
			SAMNet::addHelloHook(&samLootSendTables);
		}
	};
	static SamLootNet s_lootNet;
}
