/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_ally_items.hpp
	Desc: gear on allies (unreleased). Read what a monster carries and wears, ask whether an
	      item may be worn, and move the SAME item between a player's backpack, an ally's
	      pockets and its worn slots. See docs/ally-items.md.

	      Game build only (GAME_SOURCES): calls entity and monster code. Host only: monsters
	      are the host's.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>

#include "sam_value.hpp"

class Item;
class Stat;

class Item;
class Stat;

namespace SAMAllyItems
{
	// What a call answers: ok, or the reason it was refused (a short code a script can compare)
	// and a sentence for a player. ticket is set by a give/take that was accepted.
	struct Result
	{
		bool ok = false;
		std::string reason;
		std::string text;
		std::uint32_t ticket = 0;
	};

	// { ok, reason, text }, plus ticket when it is not 0.
	SamValue toValue(const Result& r);

	// The player index (0..3) a creature follows, or -1 for a creature that follows nobody
	// (a wild monster, a player, a uid that is nothing).
	int leaderOf(std::uint32_t uid);

	// What a monster carries and wears: { pockets = [item...], worn = { helmet = item, ... } }, or
	// nil for a uid that is not a monster. An item is the same row sam_get_inventory gives
	// ({ uid, type, name, count, beatitude, status, identified, equipped }) plus `appearance`.
	// The slots are helmet, breastplate, gloves, shoes, shield, weapon, cloak, amulet, ring, mask.
	// An item a slot points to is listed under worn only, even when it also sits in the list.
	SamValue getInventory(std::uint32_t uid);

	// Item::canUnequip's decision without its side effect (it identifies the item): false for a
	// cursed item its wielder cannot take off. The same rule as the runtimes' samItemCanUnequipQuiet
	// (which are file-local, one per runtime); the ally calls need it in C++.
	bool canUnequipQuiet(const Item* it, const Stat* wielder);

	// Whether the ally `uid` may wear `item` in `slot`. `item` is the uid of an item in the ally's
	// pockets or in a player's backpack on this host, or an item type (a number, a vanilla name or
	// "ns:item"); a uid that names such an item wins over a type with the same number. The checks,
	// in order, and the reason each gives: no_monster, no_ally, bad_slot, unknown_item, wrong_slot,
	// race, cursed_in_slot, then item.on_before_monster_equip (mod_veto). Moves nothing.
	Result canEquip(std::uint32_t uid, const SamValue& item, const std::string& slot);

	// Puts an item from the ally's pockets on, in `slot`. The same checks as canEquip, with
	// not_in_pockets for an item that is not there. The SAME Item is relinked (the uid stays), and
	// what was worn comes back to the pockets; nothing is dropped.
	Result equipItem(std::uint32_t uid, const std::string& slot, std::uint32_t itemUid);

	// Takes what is worn in `slot` back to the pockets (same Item, same uid). Refused: empty_slot,
	// cursed (the engine's rule), pockets_full.
	Result unequipToInventory(std::uint32_t uid, const std::string& slot);

	// Item::canUnequip's decision without its side effect (it identifies the item): false for a
	// cursed item its wielder cannot take off. The same rule as the runtimes' samItemCanUnequipQuiet
	// (which are file-local, one per runtime); the ally calls need it in C++.
	bool canUnequipQuiet(const Item* it, const Stat* wielder);

	// Whether the ally `uid` may wear `item` in `slot`. `item` is the uid of an item in the ally's
	// pockets or in a player's backpack on this host, or an item type (a number, a vanilla name or
	// "ns:item"); a uid that names such an item wins over a type with the same number. The checks,
	// in order, and the reason each gives: no_monster, no_ally, bad_slot, unknown_item, wrong_slot,
	// race, cursed_in_slot, then item.on_before_monster_equip (mod_veto). Moves nothing.
	Result canEquip(std::uint32_t uid, const SamValue& item, const std::string& slot);

	// ---- moving between a player and an ally (a player who plays on this machine)

	// Hand an item from `player`'s backpack to one of their allies / take one out of its pockets.
	// `count` 0 is the whole stack. ok means ACCEPTED: the outcome always arrives as
	// item.on_ally_transfer_done or item.on_ally_transfer_failed (both carry the ticket), never
	// from the call itself, so a mod has one code path. Nothing moves until the next tick().
	// Refused at the call: no_ally, not_owner, unknown_item, equipped, cursed, busy, pockets_full,
	// backpack_full, bad_count.
	Result giveItem(int player, std::uint32_t allyUid, std::uint32_t itemUid, int count);
	Result takeItem(int player, std::uint32_t allyUid, std::uint32_t itemUid, int count);

	// Called once a game tick on the host: completes the transfers waiting for it and fires their
	// events. A transfer whose ally or item went away in between fails with the reason, and
	// nothing is lost: the item never left its owner.
	void tick();
}
