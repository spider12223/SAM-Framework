/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_mp_inventory.hpp
	Desc: a host-side mirror of each remote player's backpack and spell list, and carrying item/spell changes to their machine.

	WHY THIS EXISTS. In a Barony co-op game a player's backpack and spell list live only on
	that player's own machine. The host keeps copies of the gear a client is WEARING (the
	engine needs them for combat), and nothing else: no backpack, no spells. So before this
	module a host script asking "what does player 2 carry" read an almost empty list, and a
	host script changing player 2's item could not even find it. Scripts run on the host, so
	that was most of the inventory API broken for 3 players out of 4.

	WHAT IT DOES.
	  * A client that runs S.A.M scripts reports its own backpack (every item: uid, type,
	    status, blessing, count, appearance, identified, grid position, the slot it is worn
	    in, owner, droppable) and its spell list to the host whenever either changes, on the
	    ordered SAMQ channel. It starts only when the host asks, so a stock host never sees
	    one of these packets.
	  * The host keeps each report as REAL Item objects in a private list per remote player
	    (never stats[p]->inventory, which the engine owns), each under a PROXY uid from a
	    range host items never reach, and remembers which uid the owner's machine knows it by.
	  * A backpack report carries item TYPE NUMBERS, and a mod's item numbers follow the order
	    the mods loaded, so the host names a remote player's mod item from its own table. That
	    is right when both machines load the same mods in the same order (which the mod
	    fingerprint already insists on) and wrong otherwise, so a reported type the host has no
	    definition for is left out of the mirror rather than guessed at. The spell report
	    crosses by NAME instead, because a spell list is short enough to afford it.
	  * The inventory router is installed into SAMNet, so the trampoline turns a proxy uid in
	    an `owner` call (sam_set_item_count, sam_remove_item...) into the owner's uid and
	    carries the call there. `read` calls stay on the host and read the mirror.
	  * On the owner's machine, a carried change to an item the player is wearing is echoed
	    back to the host's worn copy (the one combat uses), and a carried destroy is drained
	    at a point where no engine frame holds the item.

	Nothing here runs, sends or allocates without a script loaded: every hook is a SAMNet
	hook, and SAMNet does nothing without scripts. Singleplayer never sends anything.

	OPS (SAMNet::Op::InventoryFirst + n; the two directions are separate spaces):
	  host -> client  +0  "report your backpack and spells from now on"   [u8 version]
	  client -> host  +0  backpack snapshot                               see buildBackpack
	  client -> host  +1  spell list snapshot                             see buildSpells (ids AND names)
	  client -> host  +2  a worn item changed here: fix your copy         see onWornChanged
	  client -> host  +3  a carried call was refused here, and why        [str8 fn][str16 why][str8 code]
	Every op is S.A.M-only and goes only to (or comes only from) a machine that said HELLO.

	TIMING. A change the host carries to a player's machine is made there on the next tick,
	and the host's mirror shows it after that machine's next report (a few ticks later). A
	host script that changes a remote player's item and reads it back in the same tick still
	reads the old value.

	A NEW RUN. Everything here belongs to one character. resetForNewRun() drops it at
	doNewGame, which is the one function every route between two runs passes through, so the
	host never serves the previous character's backpack or spells as the new one's. That
	means a remote player's backpack and spells read as "not seen yet" for the first few
	ticks of EVERY run, not only after a join: read them from a timer or a later event
	rather than from game.on_game_start. The two machines do not reach doNewGame at the same
	moment, so the host also asks any S.A.M player it has nothing mirrored for to report,
	once a second, which covers the case where their game reset first and this one threw the
	new character's report away.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct list_t;   // main.hpp
class Item;      // items.hpp

namespace SAMMpInventory
{
	// What a reader about one player's backpack or spells can see on this machine.
	//   Yes      - the answer is here (this machine's own player, or the host's mirror of a
	//              remote player who has reported).
	//   No       - that player is on another machine and this one cannot see their things
	//              (a client asking about someone else; the host before that player's game
	//              has reported, or when it does not run S.A.M scripts). Warned once, naming
	//              `fn`. The reader answers nil: "cannot see" must never read as "has none".
	//   Invalid  - not a player in this game (bad index, empty slot). The reader keeps the
	//              answer it always gave for a bad player.
	enum class Seen : std::uint8_t { Yes, No, Invalid };

	// One row of sam_get_inventory, built once here so both runtimes return the same thing.
	struct InventoryRow
	{
		std::uint32_t uid = 0;     // this machine's uid for it (a proxy for a mirrored item)
		int type = 0;
		std::string name;          // vanilla internal name, or the mod item's "ns:item"
		int count = 0;
		int beatitude = 0;
		int status = 0;
		bool identified = false;
		bool equipped = false;     // worn right now: the same answer sam_is_item_equipped gives
	};

	Seen inventoryRows(int player, const char* fn, std::vector<InventoryRow>* out);

	// How many items of `type` (whole stacks) `player` carries. *out untouched unless Yes.
	Seen countOf(int player, int type, const char* fn, long long* out);

	// The spell ids `player` knows, and the names scripts see for them ("ns:spell" for a
	// mod's spell, the engine's internal name for a vanilla one). A remote player's machine
	// reports its spells BY NAME, because a mod's spell ids follow the order the mods loaded
	// and can differ between machines: the ids are this host's ids for those names (a spell
	// the host has nothing by that name for is in the names only).
	Seen spellIdsOf(int player, const char* fn, std::vector<int>* out);
	Seen spellNamesOf(int player, const char* fn, std::vector<std::string>* out);

	// Where this machine keeps `player`'s backpack: their own list when they play here, the
	// mirror when the host has a report from their machine, nullptr otherwise.
	list_t* listFor(int player);

	// Resolve a script's item uid. Read: this machine's own players' items first, then (host
	// only) the mirror of a remote player's backpack. Write: this machine's own players' items
	// only; a remote player's item is carried to their machine by the trampoline before any
	// body runs, so a mirror item reaching a writer means it could not be carried. A miss is
	// warned once per function. `holder` (optional) gets the player whose backpack it is, or -1.
	enum class Use : std::uint8_t { Read, Write };
	Item* resolveItem(long long uid, Use use, const char* fn, int* holder = nullptr);

	// The mirrored item with this proxy uid, and whose it is. Host only; nullptr elsewhere.
	Item* findMirrorItem(std::uint32_t uid, int* owner);

	// Is `item` worn by `player` right now? By POINTER against the ten slots on this machine,
	// or against the slots the owner reported for a mirrored item. sam_get_inventory's
	// `equipped` and sam_is_item_equipped both ask this, so they cannot disagree.
	bool isEquipped(int player, const Item* item);

	// After a script changed an item on its owner's machine (a call the host carried there):
	// if the player is wearing it, tell the host so the copy it fights with matches, and ask
	// for an early backpack report. Only the fields that really changed are sent, at the end
	// of the tick, so a mod that writes the same value every tick sends nothing. No-op on the
	// host and in singleplayer.
	void afterOwnerWrite(const Item* item);

	// Ask for an early backpack report without an item to echo: for a writer that DESTROYED
	// the item (sam_remove_item, sam_set_item_count(uid, 0)), which has nothing left to pass
	// to afterOwnerWrite. Without it the host went on seeing the destroyed item until the
	// owner's next scheduled report. No-op on the host and in singleplayer.
	void nudgeReport();

	// Forget everything that belongs to the character that just ended: the host's mirrors and
	// proxy uids, what this machine last reported, and the queued item/spell removals. Called
	// from doNewGame on EVERY machine, because a new run is reached without doEndgame (a death
	// and restart, or the host taking the lobby back to the character screen) and so without
	// the ClearHook. Deliberately leaves SAMNet's peers and this machine's "the host wants
	// reports" flag alone: that flag is set from HELLO, which never comes again for a client
	// that stayed connected.
	void resetForNewRun();

	// A call could not be done on this machine. Logs it here (pass logHere = false where the
	// caller already logged it) and, when the host carried the call here, tells the host why:
	// the host's script was already answered "sent", and the host's log is where its author is
	// looking. `code` is a short stable reason ("missing", "equipped", "stacklimit",
	// "appearance", "notyours"): both ends dedupe on (fn, code), never on the message, because
	// several messages name a uid and a script polling a list of uids would otherwise write
	// one line -- and send one packet -- per uid.
	void noteOwnerRefusal(const char* fn, const std::string& why, bool logHere = true, const char* code = nullptr);

	// Give `item` to `player` the way the engine gives a pickup: into the backpack for a player
	// on this machine; for a remote player on the host, itemPickup sends the vanilla ITEM packet
	// (a stock client takes it too) and keeps nothing here. Takes ownership of `item` and frees
	// it. False, logged under `fn`, when the player is not in this game.
	bool deliverItem(int player, Item* item, const char* fn);

	// The name sam_get_inventory reports for an item type.
	std::string inventoryName(int type);
}
