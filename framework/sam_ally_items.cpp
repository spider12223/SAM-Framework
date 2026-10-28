/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_ally_items.cpp
	Desc: gear on allies. See sam_ally_items.hpp.

-------------------------------------------------------------------------------*/

// Must precede every include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_ally_items.hpp"
#include "sam_party.hpp"
#include "sam_mp_inventory.hpp"
#include "sam_event.hpp"
#include "sam_items.hpp"

#include "main.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "entity.hpp"
#include "monster.hpp"
#include "items.hpp"
#include "mod_tools.hpp"   // ItemTooltips: the vanilla item names
#include "player.hpp"

#include <cctype>
#include <cstdlib>
#include <map>
#include <vector>

namespace
{
	// The worn slots, in the order they are listed.
	const char* const SLOT_NAMES[] = { "helmet", "breastplate", "gloves", "shoes", "shield", "weapon", "cloak", "amulet", "ring", "mask" };

	// The Stat pointer for a slot name, or nullptr for a name that is not a slot.
	Item** slotPointer(Stat* s, const std::string& slot)
	{
		if ( !s ) { return nullptr; }
		if ( slot == "helmet" ) { return &s->helmet; }
		if ( slot == "breastplate" ) { return &s->breastplate; }
		if ( slot == "gloves" ) { return &s->gloves; }
		if ( slot == "shoes" ) { return &s->shoes; }
		if ( slot == "shield" ) { return &s->shield; }
		if ( slot == "weapon" ) { return &s->weapon; }
		if ( slot == "cloak" ) { return &s->cloak; }
		if ( slot == "amulet" ) { return &s->amulet; }
		if ( slot == "ring" ) { return &s->ring; }
		if ( slot == "mask" ) { return &s->mask; }
		return nullptr;
	}

	// Whether a slot of this monster points at the item.
	bool isWorn(Stat* s, const Item* item)
	{
		if ( !s || !item ) { return false; }
		for ( const char* name : SLOT_NAMES )
		{
			if ( *slotPointer(s, name) == item ) { return true; }
		}
		return false;
	}

	// The monster behind a uid with the stats to go with it, or nullptr.
	Entity* monsterOf(std::uint32_t uid, Stat** statsOut)
	{
		if ( uid == 0 ) { return nullptr; }
		Entity* e = uidToEntity(uid);
		if ( !e || e->behavior != &actMonster ) { return nullptr; }
		Stat* s = e->getStats();
		if ( !s ) { return nullptr; }
		if ( statsOut ) { *statsOut = s; }
		return e;
	}

	SamValue itemRow(const Item* it, bool worn)
	{
		SamValue row = SamValue::object();
		row.set("uid", SamValue::integer((long long)it->uid));
		row.set("type", SamValue::integer((long long)it->type));
		row.set("name", SamValue::string(SAMMpInventory::inventoryName((int)it->type)));
		row.set("count", SamValue::integer((long long)it->count));
		row.set("beatitude", SamValue::integer((long long)it->beatitude));
		row.set("status", SamValue::integer((long long)it->status));
		row.set("identified", SamValue::boolean(it->identified));
		row.set("equipped", SamValue::boolean(worn));
		row.set("appearance", SamValue::integer((long long)it->appearance));
		return row;
	}

	// What an ally of this species wears. These are the rules Barony applies when it decides what a
	// player's ally picks up (Entity::monsterAllyEquipmentInClass, actmonster.cpp), copied here
	// because that function answers "does it want this", mixed in with class preferences, and not
	// "can it wear this". The engine file is left alone.
	bool bodyAllows(const Stat* s, const Item& item)
	{
		bool hats = true, helm = true, gloves = false, breastplate = true;
		if ( s->type == HUMAN || s->type == VAMPIRE ) { gloves = true; }
		if ( s->type == GOATMAN || s->type == GNOME || s->type == INCUBUS || s->type == SUCCUBUS
			|| s->type == INSECTOID || s->type == VAMPIRE || s->type == KOBOLD )
		{
			hats = false;
			helm = false;
		}
		if ( s->type == VAMPIRE ) { breastplate = false; }
		if ( s->type == AUTOMATON ) { hats = false; }
		switch ( checkEquipType(&item) )
		{
			case TYPE_HAT:
				// a kobold's one exception: hoods
				if ( s->type == KOBOLD && (item.type == HAT_HOOD || item.type == HAT_HOOD_SILVER || item.type == HAT_HOOD_RED
					|| item.type == HAT_HOOD_APPRENTICE || item.type == HAT_HOOD_WHISPERS || item.type == HAT_HOOD_ASSASSIN) )
				{
					return true;
				}
				return hats;
			case TYPE_HELM: return helm;
			case TYPE_BREASTPIECE: return breastplate;
			case TYPE_GLOVES: return gloves;
			default: return true;   // weapons, shields, boots, rings and the rest: a mod vetoes through the event
		}
	}

	// The slot name an item type is worn in (the mapping sam_get_item_slot reports), or "".
	const char* wornSlotOf(int type)
	{
		if ( type < 0 || type >= NUM_ITEM_SLOTS ) { return ""; }
		switch ( (int)items[type].item_slot )
		{
			case EQUIPPABLE_IN_SLOT_WEAPON: return "weapon";
			case EQUIPPABLE_IN_SLOT_SHIELD: return "shield";
			case EQUIPPABLE_IN_SLOT_MASK: return "mask";
			case EQUIPPABLE_IN_SLOT_HELM: return "helmet";
			case EQUIPPABLE_IN_SLOT_GLOVES: return "gloves";
			case EQUIPPABLE_IN_SLOT_BOOTS: return "shoes";
			case EQUIPPABLE_IN_SLOT_BREASTPLATE: return "breastplate";
			case EQUIPPABLE_IN_SLOT_CLOAK: return "cloak";
			case EQUIPPABLE_IN_SLOT_AMULET: return "amulet";
			case EQUIPPABLE_IN_SLOT_RING: return "ring";
			default: return "";
		}
	}

	// An item type from a vanilla name ("iron_helm") or a mod item's "ns:item"; -1 when unknown.
	int itemTypeFromName(const std::string& name)
	{
		std::string lower = name;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		auto it = ItemTooltips.itemNameStringToItemID.find(lower);
		if ( it != ItemTooltips.itemNameStringToItemID.end() ) { return it->second; }
		return SAMItems::itemIdForIdString(name);
	}

	// What a script named: an item that is really there (the pockets of this ally, or a player's
	// backpack as the host sees it), or a type.
	struct Named
	{
		const Item* item = nullptr;   // set when it named an item
		int type = -1;
	};

	bool resolveNamed(const Stat* ally, const SamValue& v, Named& out)
	{
		if ( v.isNumber() )
		{
			const long long n = v.type == SamValue::Type::Int ? v.i : (long long)v.n;
			if ( n > 0 && n <= 0xFFFFFFFFLL )
			{
				const std::uint32_t u = (std::uint32_t)n;
				const Item* it = nullptr;
				for ( node_t* node = ally ? ally->inventory.first : nullptr; node && !it; node = node->next )
				{
					const Item* cand = (const Item*)node->element;
					if ( cand && cand->uid == u ) { it = cand; }
				}
				if ( !it ) { it = uidToItem(u); }   // a player's backpack on this machine
				if ( !it ) { int owner = -1; it = SAMMpInventory::findMirrorItem(u, &owner); }   // a remote player's, as the host sees it
				if ( it ) { out.item = it; out.type = (int)it->type; return true; }
			}
			if ( n >= 0 && n < NUM_ITEM_SLOTS ) { out.type = (int)n; return true; }
			return false;
		}
		if ( v.type == SamValue::Type::Str )
		{
			const int t = itemTypeFromName(v.s);
			if ( t < 0 || t >= NUM_ITEM_SLOTS ) { return false; }
			out.type = t;
			return true;
		}
		return false;
	}

	int pocketCapacity(const Stat* s)
	{
		return s->type == AUTOMATON ? 5 : 9;   // Barony's own numbers for what an ally will pick up (entity.cpp, monsterAddNearbyItemToInventory)
	}

	// Barony's test (entity.cpp:25545): no more room once the list holds capacity + 1.
	bool pocketsFull(Stat* s)
	{
		return list_Size(&s->inventory) >= pocketCapacity(s) + 1;
	}

	// An item in the monster's pocket list (worn or not), or nullptr.
	Item* findInPockets(Stat* s, std::uint32_t uid)
	{
		for ( node_t* node = s->inventory.first; node; node = node->next )
		{
			Item* it = (Item*)node->element;
			if ( it && it->uid == uid ) { return it; }
		}
		return nullptr;
	}

	// The item leaves the list but is not freed: the node goes, the item stays and owns itself,
	// which is how Barony keeps what a monster wears.
	void removeFromPockets(Item* item)
	{
		node_t* node = item->node;
		if ( !node ) { return; }
		node->element = nullptr;
		list_RemoveNode(node);
		item->node = nullptr;
	}

	void addToPockets(Stat* s, Item* item)
	{
		node_t* node = list_AddNodeLast(&s->inventory);
		node->element = item;
		node->deconstructor = &defaultDeconstructor;
		node->size = sizeof(Item);
		item->node = node;
	}

	SAMAllyItems::Result refuse(const char* reason, const char* text);

	// ---- transfers

	struct Ticket
	{
		std::uint32_t id = 0;
		bool give = false;
		int player = 0;
		std::uint32_t allyUid = 0;
		std::uint32_t itemUid = 0;
		int count = 0;   // how many units; already the whole stack when the script said 0
	};
	std::map<std::uint32_t, Ticket> g_tickets;   // in the order they were made
	std::uint32_t g_nextTicket = 1;

	void noFree(void*) {}   // a node deconstructor that leaves its element alone

	bool localPlayer(int p)
	{
		return p >= 0 && p < MAXPLAYERS && players[p] && stats[p] && players[p]->isLocalPlayer();
	}

	Item* findIn(list_t* list, std::uint32_t uid)
	{
		for ( node_t* node = list->first; node; node = node->next )
		{
			Item* it = (Item*)node->element;
			if ( it && it->uid == uid ) { return it; }
		}
		return nullptr;
	}

	bool ticketHolds(std::uint32_t itemUid)
	{
		for ( const auto& kv : g_tickets ) { if ( kv.second.itemUid == itemUid ) { return true; } }
		return false;
	}

	// Barony's own test for room in a player's backpack. It does not look for a stack the item
	// could join, so a backpack with no free slot refuses even an item that would stack.
	bool backpackHasRoom(int p)
	{
		return players[p]->inventoryUI.bItemInventoryHasFreeSlot();
	}

	// Everything that can be told at the call, in the order the reasons are listed in the header.
	// `item` and `ally` are set on success.
	SAMAllyItems::Result checkTransfer(bool give, int player, std::uint32_t allyUid, std::uint32_t itemUid, int count, int* resolvedCount)
	{
		Stat* as = nullptr;
		if ( !monsterOf(allyUid, &as) || SAMAllyItems::leaderOf(allyUid) < 0 ) { return refuse("no_ally", "That is not an ally."); }
		if ( SAMAllyItems::leaderOf(allyUid) != player ) { return refuse("not_owner", "That ally follows someone else."); }
		if ( !localPlayer(player) ) { return refuse("not_owner", "That player is not here."); }
		list_t* from = give ? &stats[player]->inventory : &as->inventory;
		Item* item = findIn(from, itemUid);
		if ( !item ) { return refuse("unknown_item", give ? "You do not have that." : "It is not carrying that."); }
		if ( give && itemIsEquipped(item, player) )
		{
			return SAMAllyItems::canUnequipQuiet(item, stats[player]) ? refuse("equipped", "Take that off first.") : refuse("cursed", "That is cursed and will not come off.");
		}
		if ( !give && isWorn(as, item) ) { return refuse("equipped", "It is wearing that."); }
		if ( ticketHolds(itemUid) ) { return refuse("busy", "That item is already being moved."); }
		if ( count < 0 || count > item->count ) { return refuse("bad_count", "There is not that many."); }
		if ( give && pocketsFull(as) ) { return refuse("pockets_full", "Its pockets are full."); }
		if ( !give && !backpackHasRoom(player) ) { return refuse("backpack_full", "Your backpack is full."); }
		if ( resolvedCount ) { *resolvedCount = count == 0 ? (int)item->count : count; }
		SAMAllyItems::Result ok;
		ok.ok = true;
		return ok;
	}

	SAMAllyItems::Result queueTransfer(bool give, int player, std::uint32_t allyUid, std::uint32_t itemUid, int count)
	{
		int n = 0;
		SAMAllyItems::Result r = checkTransfer(give, player, allyUid, itemUid, count, &n);
		if ( !r.ok ) { return r; }
		Ticket t;
		t.id = g_nextTicket++;
		t.give = give;
		t.player = player;
		t.allyUid = allyUid;
		t.itemUid = itemUid;
		t.count = n;
		g_tickets[t.id] = t;
		r.ticket = t.id;
		return r;
	}

	SAMAllyItems::Result refuse(const char* reason, const char* text)
	{
		SAMAllyItems::Result r;
		r.reason = reason;
		r.text = text;
		return r;
	}
}

namespace SAMAllyItems
{
	SamValue toValue(const Result& r)
	{
		SamValue v = SamValue::object();
		v.set("ok", SamValue::boolean(r.ok));
		v.set("reason", SamValue::string(r.reason));
		v.set("text", SamValue::string(r.text));
		if ( r.ticket ) { v.set("ticket", SamValue::integer((long long)r.ticket)); }
		return v;
	}

	int leaderOf(std::uint32_t uid)
	{
		if ( uid == 0 ) { return -1; }
		return SAMParty::followerLeader(uidToEntity(uid));
	}

	SamValue getInventory(std::uint32_t uid)
	{
		Stat* s = nullptr;
		if ( !monsterOf(uid, &s) ) { return SamValue::nil(); }
		SamValue out = SamValue::object();
		SamValue pockets = SamValue::array();
		for ( node_t* node = s->inventory.first; node; node = node->next )
		{
			const Item* it = (const Item*)node->element;
			if ( !it || isWorn(s, it) ) { continue; }
			pockets.push(itemRow(it, false));
		}
		SamValue worn = SamValue::object();
		for ( const char* name : SLOT_NAMES )
		{
			const Item* it = *slotPointer(s, name);
			if ( it ) { worn.set(name, itemRow(it, true)); }
		}
		out.set("pockets", pockets);
		out.set("worn", worn);
		return out;
	}

	bool canUnequipQuiet(const Item* it, const Stat* wielder)
	{
		if ( !it ) { return true; }
		// NOTE: the engine has a "spellbooks always unequippable" branch at items.cpp:6259,
		// and it is INSIDE a /* */ block: "Spellbooks are no longer equipable." It never
		// runs, and its literals are stale anyway (100-103 are rings, not books). It was
		// copied here by mistake and is deliberately absent now.
		if ( it->type == TOOL_DUCK ) { return true; }
		if ( wielder )
		{
			// An automaton is never stuck with anything (items.cpp:6279).
			if ( wielder->type == AUTOMATON ) { return true; }
			// Succubus and friends: a BLESSING is what sticks, not a curse.
			if ( shouldInvertEquipmentBeatitude(wielder) ) { return it->beatitude <= 0; }
		}
		return it->beatitude >= 0;
	}

	// The checks every equip goes through, after the monster, its leader, the slot and the item
	// have been found: the slot fits, the body allows it, what is worn there can come off, and
	// last the mods. Moves nothing. The item may go (a handler can use it up): look again after.
	static Result evaluate(std::uint32_t uid, Stat* s, int leader, const std::string& slot, Item** slotPtr, const Named& named)
	{
		if ( slot != wornSlotOf(named.type) ) { return refuse("wrong_slot", "That does not go in that slot."); }

		// The body gate needs an Item to look at; a type alone gets a throwaway one (never listed).
		Item* temp = nullptr;
		const Item* subject = named.item;
		if ( !subject )
		{
			temp = static_cast<Item*>(calloc(1, sizeof(Item)));
			temp->type = (ItemType)named.type;
			subject = temp;
		}
		const bool bodyOk = bodyAllows(s, *subject);
		free(temp);
		if ( !bodyOk ) { return refuse("race", "It cannot wear that."); }

		if ( *slotPtr && *slotPtr != named.item && !canUnequipQuiet(*slotPtr, s) )
		{
			return refuse("cursed_in_slot", "What it wears there is cursed and will not come off.");
		}

		SamEvent e("item.on_before_monster_equip");
		e.i("uid", (long long)uid).i("leader", leader).s("slot", slot)
			.i("item_uid", named.item ? (long long)named.item->uid : 0LL).i("item_type", named.type)
			.s("item_name", SAMMpInventory::inventoryName(named.type)).s("message", "");
		if ( !e.fire() )
		{
			const std::string said = e.getStr("message", "");
			return refuse("mod_veto", said.empty() ? "A mod does not allow that." : said.c_str());
		}
		Result ok;
		ok.ok = true;
		return ok;
	}

	Result canEquip(std::uint32_t uid, const SamValue& item, const std::string& slot)
	{
		Stat* s = nullptr;
		if ( !monsterOf(uid, &s) ) { return refuse("no_monster", "That is not a monster."); }
		const int leader = leaderOf(uid);
		if ( leader < 0 ) { return refuse("no_ally", "That monster is not following anyone."); }
		Item** slotPtr = slotPointer(s, slot);
		if ( !slotPtr ) { return refuse("bad_slot", "There is no such slot."); }
		Named named;
		if ( !resolveNamed(s, item, named) ) { return refuse("unknown_item", "There is no such item."); }
		return evaluate(uid, s, leader, slot, slotPtr, named);
	}

	Result equipItem(std::uint32_t uid, const std::string& slot, std::uint32_t itemUid)
	{
		Stat* s = nullptr;
		if ( !monsterOf(uid, &s) ) { return refuse("no_monster", "That is not a monster."); }
		const int leader = leaderOf(uid);
		if ( leader < 0 ) { return refuse("no_ally", "That monster is not following anyone."); }
		Item** slotPtr = slotPointer(s, slot);
		if ( !slotPtr ) { return refuse("bad_slot", "There is no such slot."); }
		Item* item = findInPockets(s, itemUid);
		if ( !item ) { return refuse("not_in_pockets", "That item is not in its pockets."); }
		Named named;
		named.item = item;
		named.type = (int)item->type;
		Result r = evaluate(uid, s, leader, slot, slotPtr, named);
		if ( !r.ok ) { return r; }
		// A handler could have used the item up or changed what is worn: look again.
		if ( findInPockets(s, itemUid) != item || isWorn(s, item) ) { return refuse("not_in_pockets", "That item is not in its pockets."); }
		if ( *slotPtr && *slotPtr != item && !canUnequipQuiet(*slotPtr, s) )
		{
			return refuse("cursed_in_slot", "What it wears there is cursed and will not come off.");
		}

		// Relink, never copy: the item leaves the list (its node goes, the item stays) and becomes
		// what the slot points to; what was worn comes back to the list.
		Item* occupant = *slotPtr;
		removeFromPockets(item);
		*slotPtr = item;
		if ( occupant && occupant != item ) { addToPockets(s, occupant); }
		return r;
	}

	Result unequipToInventory(std::uint32_t uid, const std::string& slot)
	{
		Stat* s = nullptr;
		if ( !monsterOf(uid, &s) ) { return refuse("no_monster", "That is not a monster."); }
		if ( leaderOf(uid) < 0 ) { return refuse("no_ally", "That monster is not following anyone."); }
		Item** slotPtr = slotPointer(s, slot);
		if ( !slotPtr ) { return refuse("bad_slot", "There is no such slot."); }
		Item* item = *slotPtr;
		if ( !item ) { return refuse("empty_slot", "Nothing is worn there."); }
		if ( !canUnequipQuiet(item, s) ) { return refuse("cursed", "It is cursed and will not come off."); }
		if ( !item->node && pocketsFull(s) ) { return refuse("pockets_full", "Its pockets are full."); }
		*slotPtr = nullptr;
		if ( !item->node ) { addToPockets(s, item); }
		Result ok;
		ok.ok = true;
		return ok;
	}

	Result giveItem(int player, std::uint32_t allyUid, std::uint32_t itemUid, int count)
	{
		return queueTransfer(true, player, allyUid, itemUid, count);
	}

	Result takeItem(int player, std::uint32_t allyUid, std::uint32_t itemUid, int count)
	{
		return queueTransfer(false, player, allyUid, itemUid, count);
	}

	namespace
	{
		struct Outcome
		{
			Ticket t;
			bool done = false;
			std::string reason;
			std::uint32_t newItemUid = 0;
		};

		void fail(Outcome& o, const char* reason) { o.done = false; o.reason = reason; }

		// The move itself, on a ticket that was accepted a tick ago: everything is looked at again,
		// because the game went on in between.
		void perform(Outcome& o)
		{
			const Ticket& t = o.t;
			Stat* as = nullptr;
			if ( !monsterOf(t.allyUid, &as) || leaderOf(t.allyUid) != t.player ) { fail(o, "ally_gone"); return; }
			if ( !localPlayer(t.player) ) { fail(o, "player_gone"); return; }
			Item* item = findIn(t.give ? &stats[t.player]->inventory : &as->inventory, t.itemUid);
			if ( !item ) { fail(o, "item_gone"); return; }
			if ( t.give ? itemIsEquipped(item, t.player) : isWorn(as, item) ) { fail(o, "equipped"); return; }
			if ( t.count > item->count || t.count <= 0 ) { fail(o, "bad_count"); return; }
			const bool whole = t.count == item->count;

			if ( t.give )
			{
				if ( pocketsFull(as) ) { fail(o, "pockets_full"); return; }
				if ( whole )
				{
					// The same Item changes hands: its node goes (the list's own clean-up of
					// selection, hotbar and doll runs, the item is not freed) and the ally's list takes it.
					node_t* node = item->node;
					node->deconstructor = &noFree;
					list_RemoveNode(node);
					item->node = nullptr;
					addToPockets(as, item);
					o.newItemUid = item->uid;
				}
				else
				{
					Item* part = newItem(item->type, item->status, item->beatitude, (Sint16)t.count, item->appearance, item->identified, &as->inventory);
					part->ownerUid = item->ownerUid;
					item->count -= (Sint16)t.count;
					o.newItemUid = part->uid;
				}
			}
			else
			{
				if ( !backpackHasRoom(t.player) ) { fail(o, "backpack_full"); return; }
				// Into a backpack Barony's own pick-up does the placing (it stacks, and keeps looks
				// apart), and it makes a new Item or joins an existing stack: that item's uid is the
				// new one. The pocket item is spent.
				Item* got = nullptr;
				if ( whole )
				{
					got = itemPickup(t.player, item);
					if ( got ) { list_RemoveNode(item->node); }
				}
				else
				{
					Item* part = newItem(item->type, item->status, item->beatitude, (Sint16)t.count, item->appearance, item->identified, nullptr);
					part->ownerUid = item->ownerUid;
					got = itemPickup(t.player, part);
					free(part);
					if ( got ) { item->count -= (Sint16)t.count; }
				}
				if ( !got ) { fail(o, "backpack_full"); return; }
				o.newItemUid = got->uid;
			}
			o.done = true;
		}
	}

	void tick()
	{
		if ( g_tickets.empty() ) { return; }
		// Take the waiting tickets out first: a handler may start new transfers while we fire.
		std::vector<Outcome> work;
		for ( const auto& kv : g_tickets )
		{
			Outcome o;
			o.t = kv.second;
			work.push_back(o);
		}
		g_tickets.clear();
		for ( Outcome& o : work )
		{
			perform(o);
			if ( o.done )
			{
				SamEvent e("item.on_ally_transfer_done");
				e.i("player", o.t.player).i("uid", (long long)o.t.allyUid).i("ticket", (long long)o.t.id)
					.i("item_uid", (long long)o.t.itemUid).i("new_item_uid", (long long)o.newItemUid).i("count", o.t.count);
				e.fire();
			}
			else
			{
				SamEvent e("item.on_ally_transfer_failed");
				e.i("player", o.t.player).i("uid", (long long)o.t.allyUid).i("ticket", (long long)o.t.id)
					.i("item_uid", (long long)o.t.itemUid).s("reason", o.reason);
				e.fire();
			}
		}
	}
}
