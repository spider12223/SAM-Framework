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
}
