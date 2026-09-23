/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_loot.hpp
	Desc: what the dungeon hands out, and how a mod bends it without replacing the sheet.

	Every sheet-driven random item in Barony comes out of ONE function, itemLevelCurve
	(items.cpp): filter items[] by category and level window, pick one uniformly. The
	callers decide the category and the window; the floor number is only the window.
	A "randomizer" mod used to get its result by replacing items/items.json outright,
	which broke every shop below the Hamlet (a store whose minimum level nothing met
	rolled the GEM_ROCK fallback fourteen times) and fought every mod that adds items.

	This module puts the knobs that sheet replacement reached for -- and the ones it
	could not reach -- beside the curve, as tables the curve consults and events the
	curve fires, so two mods that both touch loot compose instead of overwriting each
	other's items.json.

	  * THE CONTEXT. The curve never knew whether it was rolling for the floor, a chest,
	    a shop, a monster's pockets, an alchemy recipe or a console command; the file-
	    static itemLevelCurveType was set only by the shopkeeper. ContextScope is an RAII
	    guard each caller sets, so a table entry can say "artifacts on the floor but never
	    in a store" and a handler can tell a recipe roll from a drop (a randomizer that
	    could not would have taught the player how to brew an artifact).

	  * THE TABLES. Per-item weight (0 = never), per-item floor window (vanilla has a
	    minimum and no maximum), per-item context allow-list, per-category weight for the
	    "any category" draw, a per-category fallback for an empty pool, and a per-spell
	    override for the spellbook re-roll. Every one has a default that reproduces vanilla
	    EXACTLY: with no entries the curve consumes the RNG stream byte for byte as before,
	    so a seeded run with a mod that registers nothing walks the same dungeon.

	  * THE EVENTS. Before a roll (rewrite the window or force a type), after a roll
	    (rewrite the item), before the spellbook re-roll (keep the book the curve picked),
	    before and after a chest fills, before and after a shop stocks, after a monster's
	    starting gear is made, and at the small fixtures (fountain, sink, boulder, lockpick
	    reward) that never consult the sheet at all.

	WHERE THE EVENTS DO NOT FIRE, AND WHY. The level generator (assignActions, the map's
	own chests, the mimics made from them) runs on a WORKER THREAD while the loading
	screen draws, and the script runtimes cannot be entered from there: QuickJS measures its
	stack from the thread it was created on and fails at once on any other. maps.cpp
	already refuses to fire player.on_player_revived from that thread for that reason. So
	world.on_before_loot_roll and world.on_before_chest_fill fire for every roll made DURING
	PLAY (shops, monster gear, summoned chests, console, alchemy) and never for the rolls
	made while a floor is being built. Those are governed by the tables, which is what the
	tables are for; a mod that wants to rewrite a generated floor's chests does it from
	game.on_level_entered with sam_find_entities and the container functions.

	MULTIPLAYER. A client generates its own copy of every floor from the same seed and
	KEEPS its own floor items (actItem on a client flags them NOUPDATE; the host never
	overwrites what the client rolled). So the tables that change a roll must be the same
	on every machine, or a client sees a sword where the host put a spear: the table
	writers carry the `all` contract, exactly as sam_patch_item does -- run on the host
	and on every S.A.M client, replayed to a client that joins later. The replay only
	holds calls made during the current run, so the host ALSO sends its four tables whole
	at every HELLO (a HelloHook, as the item patches do): a weight set at the main menu,
	from a settings handler or in an earlier run reaches a joiner all the same, and the
	joiner takes the host's copy wholesale without consuming its own RNG. The events fire
	on the host only. The container writers are host-only; a chest's contents are served
	to whoever opens it, and a shop's when it is entered.

	LIFETIME. The tables live as long as the mod set: cleared whenever the item registry
	is (both loader paths call SAMItems::clear, which calls clear() here), because every
	entry is keyed by an item id the registry hands out per load. They are NOT cleared
	between runs of one session -- a weight declared at load time is a rule, like a
	species immunity, and wiping it at the second run would silently undo the mod.

	Game build only.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Entity;
class Stat;
class Item;
class BaronyRNG;

namespace SAMLoot
{
	// ---- where a roll is happening ---------------------------------------------------------
	//
	// Set by the caller that knows, for as long as it is on the stack. Nested scopes of the
	// SAME kind inherit the outer uid and type when they carry none (createMonsterEquipment
	// knows it is making monster gear but not for whom; actMonster's scope around it does).
	enum Kind
	{
		Floor = 0, Chest, Shop, Monster, Recipe, Console, Other,
		KindCount
	};
	const char* kindName(int kind);            // "floor" ... "other"
	int kindFromName(const std::string& name); // -1 for a word that is none of them

	struct Context
	{
		int kind = Other;
		long long uid = -1;      // the chest / shopkeeper / monster entity, -1 when there is none
		int shopType = -1;
		int chestType = -1;
		int monsterType = -1;
	};
	struct ContextScope
	{
		explicit ContextScope(int kind, long long uid = -1, int shopType = -1, int chestType = -1, int monsterType = -1);
		~ContextScope();
	private:
		Context prev;
	};
	const Context& context();

	// ---- the tables (contract `all`: every machine keeps the same copy) -----------------------
	//
	// Every writer validates its item against [0, NUM_ITEM_SLOTS) AND a non-empty slot, and
	// its category against the real list, refusing with a log line that names the bad value.
	// Last call wins; there is no per-mod ownership because a weight is one number.
	bool setWeight(int item, int weight);                          // default 1, 0 = never
	bool setCategoryWeight(int category, int weight);              // the "any category" draw
	bool setFloorRange(int item, int minFloor, int maxFloor);      // maxFloor < 0: no ceiling
	bool setContextAllowed(int item, int kind, bool allowed);      // floor / chest / shop / monster
	bool setSpellDroppable(int spellId, bool allowed, int minFloor);
	bool setFallback(int category, int item, int kind);            // kind -1: every context; item -1: vanilla again

	// ---- what the engine asks --------------------------------------------------------------
	//
	// candidateWeight is the per-item verdict for the CURRENT context and floor: 0 drops the
	// item, anything else is its weight. anyItemRules is the one bool test that keeps an
	// unmodded roll on the vanilla path; anyWeightAboveOne says whether the pick must be a
	// weighted draw at all (a table of weights that are all 1 or 0 keeps the vanilla modulo).
	bool anyItemRules();
	int  candidateWeight(int item);
	bool anyWeightAboveOne();

	// The "any category" draw. With no category weights set this is the vanilla code moved
	// here verbatim, re-rolls included, consuming the RNG exactly as the site it replaced.
	enum PickMode { PickPlain = 0, PickFloorWithStaff, PickFloorNoStaff };
	int pickCategory(BaronyRNG& rng, int mode);

	// What to return for an empty pool: the fallback registered for this category and
	// context, else GEM_ROCK. Also remembers that the roll WAS a fallback, for the post event.
	int fallback(int category);

	// The spellbook re-roll's eligibility rule for one spell, with the mod override on top.
	// Vanilla: not hidden from the UI, and itemLevel >= drop_table.
	bool spellAllowed(int spellId, int dropTable, bool hiddenFromUi, int itemLevel, bool allowHidden);

	// ---- the events (host, main thread) ------------------------------------------------------
	//
	// E1 world.on_before_loot_roll. Fired BEFORE the candidate loop, so a handler that forces
	// a type costs the RNG nothing and one that declines leaves the stream untouched. Returns
	// the forced type, or -1 to roll; may rewrite the three references.
	int  beforeRoll(int& category, int& minLevel, int& maxLevel);
	// The same event from itemTypeWithinGoldValue (min_value / max_value instead of levels;
	// category "ANY" when the caller passed -1).
	int  beforeValueRoll(int& category, int& minValue, int& maxValue);

	// E1b world.on_loot_rolled, from the end of itemLevelCurvePostProcess. Rewrites the item
	// in place (my->skill[10..15] or item->...) and returns the final type.
	int  afterRoll(Entity* my, Item* item, int originalType, bool spellbookRerolled);

	// E2 world.on_before_spellbook_reroll. True = keep the book the curve picked. Outputs:
	// a spell to force (-1 none) and whether sheet-hidden spells may be considered. `item`
	// is the Item* the post-process is holding (null when the roll is on an actItem
	// entity), so a handler that removes that very item from its container has the removal
	// deferred instead of freeing what the engine still writes to (see the containers).
	bool beforeSpellbookReroll(Item* item, int itemType, int itemLevel, int& forcedSpell, bool& allowHidden);

	// E3 world.on_before_chest_fill (cancel = leave it empty) and E3b world.on_chest_filled.
	bool beforeChestFill(Entity* my, int& chestType, int& minQuality);
	void afterChestFill(Entity* my, int chestType);

	// E4 world.on_before_shop_stock (cancel = generate nothing) and E4b world.on_shop_stocked.
	bool beforeShopStock(Entity* my, Stat* stats, int& storeType, int& numItems, int& shopLevel, int& blessed);
	void afterShopStock(Entity* my, Stat* stats);

	// E5 world.on_monster_inventory: right after a species' init has made its starting gear.
	void afterMonsterInit(Entity* my, Stat* stats);

	// E6 world.on_fixture_loot. False = no item. `source` is "fountain", "sink", "boulder"
	// or "lockpick"; x/y are tiles; player is -1 when nobody in particular caused it.
	bool fixtureLoot(const char* source, int player, int x, int y, int& type, int& status, int& beatitude, int& count);

	// ---- reads and rolls for scripts ---------------------------------------------------------
	struct PoolEntry
	{
		int type = -1;
		std::string name;
		int level = -1;
		int weight = 1;
	};
	// The candidate set the curve would consider for this window, before the per-roll
	// stochastic drops and the store-type exclusions. kind -1: without the context test.
	bool pool(int category, int minLevel, int maxLevel, int kind, std::vector<PoolEntry>& out);
	// Roll the engine's own curve (+ post-processing, + both events) once. -1 when refused.
	int  roll(int category, int minLevel, int maxLevel, int kind);

	// ---- containers: chests, monsters, shopkeepers ---------------------------------------------
	//
	// A chest that a player has OPEN goes through Entity::addItemToChest, the engine's own
	// path, which tells that player's machine (as a new stack when that player is on another
	// machine: a merge into an existing stack sends nothing, and the opener's window would
	// keep the old count). A closed chest has no opener to tell: the item joins the chest's
	// list and is served whole on the next open. A removal from an open chest closes it
	// first (there is no "the host took something out" packet; the reopen re-serves the
	// contents). A shopkeeper gets the price-sorted slot layout run again, consumables kept
	// bottom-right, after any open shop window is closed -- on this machine, and with the
	// engine's own 'SHPC' on a browser's machine, so nobody keeps buying from stock that is gone.
	//
	// ONE REMOVAL IS DEFERRED. world.on_loot_rolled and world.on_before_spellbook_reroll fire
	// while the engine is still holding the Item* they are about, and writes to it after the
	// handler returns (afterRoll's own write-back, then the stocking loop). A removal of that
	// item from inside its own event is therefore queued and performed by drainDeferredRemovals
	// at the next frame's safe point (SAMItems::drainDestroyQueue calls it, host and single-
	// player), the same rule every script destroy follows. Every other removal is immediate.
	long long addToContainer(std::uint32_t uid, int type, int count, int status, int beatitude, bool identified, long long appearance);
	bool removeFromContainer(std::uint32_t uid, long long typeOrUid, int count);
	void drainDeferredRemovals();

	struct StockEntry
	{
		int type = -1;
		int count = 1;
		int status = 3;        // SERVICABLE
		int beatitude = 0;
		bool identified = true;
	};
	bool setShopStock(std::uint32_t uid, const std::vector<StockEntry>& items);
	bool setShopType(std::uint32_t uid, int storeType);

	// ---- names ---------------------------------------------------------------------------------
	int categoryFromName(const std::string& name);   // -1 for an unknown word; "ANY" is -1 too, ask anyCategory
	bool isAnyCategoryName(const std::string& name);
	std::string categoryName(int category);
	bool validItem(int id);                           // in range AND a non-empty slot

	// Everything. Called by SAMItems::clear on both loader paths.
	void clear();
}
