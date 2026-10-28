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

#include "main.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "entity.hpp"
#include "monster.hpp"

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
}
