/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_mp_inventory.cpp
	Desc: see sam_mp_inventory.hpp.

-------------------------------------------------------------------------------*/

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_mp_inventory.hpp"
#include "sam_net.hpp"
#include "sam_logger.hpp"
#include "sam_items.hpp"
#include "sam_spells.hpp"

#include <map>
#include <new>
#include <unordered_map>
#include <utility>

#include "main.hpp"
#include "game.hpp"
#include "items.hpp"
#include "stat.hpp"
#include "entity.hpp"    // itemNameStrings
#include "player.hpp"

namespace SAMMpInventory
{
namespace
{
	// ---------------------------------------------------------------- ops

	namespace Op
	{
		// host -> client
		constexpr std::uint8_t ReportRequest = SAMNet::Op::InventoryFirst + 0;
		// client -> host
		constexpr std::uint8_t Backpack = SAMNet::Op::InventoryFirst + 0;
		constexpr std::uint8_t Spells = SAMNet::Op::InventoryFirst + 1;
		constexpr std::uint8_t WornChanged = SAMNet::Op::InventoryFirst + 2;
		constexpr std::uint8_t Refused = SAMNet::Op::InventoryFirst + 3;
	}

	constexpr std::uint8_t REPORT_VERSION = 1;
	constexpr std::uint8_t NOT_WORN = 0xFF;
	constexpr int NUM_SLOTS = 10;
	constexpr std::size_t MAX_ITEMS = 4096;     // a report bigger than any real backpack is refused
	constexpr std::size_t MAX_SPELLS = 1024;
	constexpr Uint32 REPORT_INTERVAL = 10;      // ticks between "did anything change?" checks (5 a second)
	constexpr Uint32 REFUSAL_COOLDOWN = 500;    // ticks (about ten seconds) between two identical refusals
	constexpr Uint32 ASK_INTERVAL = 50;         // ticks between two "please report" nudges to one player

	// Which fields of a worn item a carried write actually changed. The host writes the
	// fields in the mask and uses the ones OUTSIDE it to check its copy is still the same
	// item; see onWornChanged.
	namespace WornBit
	{
		constexpr std::uint16_t Status = 1 << 0;
		constexpr std::uint16_t Beatitude = 1 << 1;
		constexpr std::uint16_t Count = 1 << 2;
		constexpr std::uint16_t Appearance = 1 << 3;
		constexpr std::uint16_t Identified = 1 << 4;
		constexpr std::uint16_t Droppable = 1 << 5;
		constexpr std::uint16_t OwnerUid = 1 << 6;
		constexpr std::uint16_t All = 0x7F;
		// The fields an unchanged item is allowed to be identified BY on the host. Appearance
		// is the one that tells two identical items apart: a generated item takes a random
		// appearance (maps.cpp:7759) and carries it to every machine, and the host's copy of a
		// worn item gets it from the equip packet (items.cpp:8073-8100). Identified likewise
		// only ever changes on the owner's machine, which echoes it with EQUA.
		//
		// Status, beatitude and count are deliberately NOT here even though the equip packets
		// carry them: the HOST's own game changes them behind the owner's back (armour
		// degrades, a trap curses, ammo is spent) and the owner learns a tick later, so
		// requiring the owner's value to match would throw away good echoes exactly when the
		// game is busiest. isDroppable and ownerUid are not here either -- no vanilla packet
		// carries them, so a freshly equipped copy holds defaults and they identify nothing.
		constexpr std::uint16_t Identity = Appearance | Identified;
	}

	// Proxy uids live far above anything the host's item counter reaches (it starts at 1 and
	// counts up by one per item, items.cpp:222, and is never reset: a very long session makes
	// a few hundred thousand items), so a proxy is never mistaken for one of the host's own
	// items; resolveItem and the router also let a real local item win, whatever its uid. The
	// range stays below 2^31 so a script that squeezes a uid through a signed 32-bit integer
	// (JavaScript's `x | 0`) does not turn it negative.
	constexpr std::uint32_t PROXY_FIRST = 0x60000000u;
	constexpr std::uint32_t PROXY_LAST = 0x7FFFFF00u;

	// The ten slots in one fixed order, so "worn in slot 3" means the same thing on both
	// ends of the wire.
	Item** slotPtr(Stat* s, int k)
	{
		switch ( k )
		{
			case 0: return &s->weapon;
			case 1: return &s->shield;
			case 2: return &s->helmet;
			case 3: return &s->breastplate;
			case 4: return &s->gloves;
			case 5: return &s->shoes;
			case 6: return &s->cloak;
			case 7: return &s->amulet;
			case 8: return &s->ring;
			case 9: return &s->mask;
			default: return nullptr;
		}
	}

	// Which of `player`'s ten slots holds this exact item (by POINTER), or -1. itemSlot is no
	// use here: it matches by value, so a spare ring reads as the worn one.
	int wornSlotOf(int player, const Item* it)
	{
		if ( !it || player < 0 || player >= MAXPLAYERS || !stats[player] ) { return -1; }
		for ( int k = 0; k < NUM_SLOTS; ++k )
		{
			Item** slot = slotPtr(stats[player], k);
			if ( slot && *slot == it ) { return k; }
		}
		return -1;
	}

	bool validPlayer(int player)
	{
		return player >= 0 && player < MAXPLAYERS && players[player] && stats[player];
	}

	// ---------------------------------------------------------------- the host's mirror

	struct Mirror
	{
		bool haveItems = false;
		bool haveSpells = false;
		Uint32 askedTick = 0;                  // when this host last asked that machine to report
		list_t items{ nullptr, nullptr };      // real Items; engine code never sees this list
		Item* worn[NUM_SLOTS] = {};            // pointers into `items`, as the owner reported
		std::vector<int> spells;               // THIS host's id for each, -1 for one it has no such spell for
		std::vector<std::string> spellNames;   // the names the owner's machine reported, same order
	};
	Mirror s_mirror[MAXPLAYERS];

	struct ProxyOwner
	{
		int player;
		std::uint32_t ownerUid;
	};
	std::unordered_map<std::uint32_t, ProxyOwner> s_byProxy;           // proxy -> whose, and their uid
	std::map<std::pair<int, std::uint32_t>, std::uint32_t> s_byOwner;  // (player, their uid) -> proxy
	std::uint32_t s_nextProxy = PROXY_FIRST;

	std::uint32_t newProxy()
	{
		// Never hand out a proxy still in use, even after the range wraps (which would take
		// half a billion reported items in one session).
		for ( int tries = 0; tries < 1024; ++tries )
		{
			const std::uint32_t v = s_nextProxy++;
			if ( s_nextProxy > PROXY_LAST ) { s_nextProxy = PROXY_FIRST; }
			if ( !s_byProxy.count(v) ) { return v; }
		}
		return 0;
	}

	void forgetProxiesOf(int player)
	{
		for ( auto it = s_byProxy.begin(); it != s_byProxy.end(); )
		{
			if ( it->second.player == player ) { it = s_byProxy.erase(it); }
			else { ++it; }
		}
		for ( auto it = s_byOwner.begin(); it != s_byOwner.end(); )
		{
			if ( it->first.first == player ) { it = s_byOwner.erase(it); }
			else { ++it; }
		}
	}

	void dropMirror(int player)
	{
		if ( player < 0 || player >= MAXPLAYERS ) { return; }
		Mirror& m = s_mirror[player];
		list_FreeAll(&m.items);   // defaultDeconstructor frees each Item
		m.items.first = nullptr;
		m.items.last = nullptr;
		for ( int k = 0; k < NUM_SLOTS; ++k ) { m.worn[k] = nullptr; }
		m.spells.clear();
		m.spellNames.clear();
		m.haveItems = false;
		m.haveSpells = false;
		m.askedTick = 0;   // so hostTick asks for a report straight away
		forgetProxiesOf(player);
	}

	// A real Item, zeroed and value-initialised like `Item probe{}` elsewhere, in malloc'd
	// memory so the list's defaultDeconstructor (free) owns it exactly as it owns engine items.
	Item* newMirrorItem(list_t* list)
	{
		void* mem = malloc(sizeof(Item));
		if ( !mem ) { return nullptr; }
		Item* it = new (mem) Item();
		node_t* node = list_AddNodeLast(list);
		node->element = it;
		node->deconstructor = &defaultDeconstructor;
		node->size = sizeof(Item);
		it->node = node;
		return it;
	}

	bool isMirrorItemOf(int player, const Item* it)
	{
		return it && it->node && player >= 0 && player < MAXPLAYERS && it->node->list == &s_mirror[player].items;
	}

	// The remote players the host could mirror: connected, and playing on another machine.
	bool isMirrorable(int player)
	{
		return multiplayer == SERVER && player > 0 && player < MAXPLAYERS
			&& !client_disconnected[player] && players[player] && !players[player]->isLocalPlayer();
	}

	// ---------------------------------------------------------------- wire: backpack

	struct ReportedItem
	{
		std::uint32_t uid = 0;
		int type = 0;
		int status = 0;
		int beatitude = 0;
		int count = 1;
		std::uint32_t appearance = 0;
		bool identified = false;
		Sint32 x = 0;
		Sint32 y = 0;
		std::uint8_t worn = NOT_WORN;
		std::uint32_t ownerUid = 0;
		bool droppable = true;
	};

	// [u8 version][u16 n] then per item:
	//   [u32 uid][u16 type][u8 status][u16 beatitude][u16 count][u32 appearance][u8 identified]
	//   [u32 x][u32 y][u8 worn slot or 0xFF][u32 ownerUid][u8 flags: bit0 droppable]
	std::string buildBackpack(int player)
	{
		SAMNet::Writer items;
		std::size_t n = 0;
		for ( node_t* node = stats[player]->inventory.first; node && n < MAX_ITEMS; node = node->next )
		{
			const Item* it = (const Item*)node->element;
			if ( !it ) { continue; }
			const int worn = wornSlotOf(player, it);
			items.u32(it->uid);
			items.u16((std::uint16_t)it->type);
			items.u8((std::uint8_t)it->status);
			items.u16((std::uint16_t)(Sint16)it->beatitude);
			items.u16((std::uint16_t)(Sint16)it->count);
			items.u32(it->appearance);
			items.u8(it->identified ? 1 : 0);
			items.u32((std::uint32_t)it->x);
			items.u32((std::uint32_t)it->y);
			items.u8(worn >= 0 ? (std::uint8_t)worn : NOT_WORN);
			items.u32(it->ownerUid);
			items.u8(it->isDroppable ? 1 : 0);
			++n;
		}
		SAMNet::Writer w;
		w.u8(REPORT_VERSION);
		w.u16((std::uint16_t)n);
		w.buf += items.buf;
		return w.buf;
	}

	bool parseBackpack(const std::string& body, std::vector<ReportedItem>& out)
	{
		SAMNet::Reader r(body);
		if ( r.u8() != REPORT_VERSION || !r.ok ) { return false; }
		const std::size_t n = r.u16();
		if ( !r.ok || n > MAX_ITEMS ) { return false; }
		out.reserve(n);
		for ( std::size_t i = 0; i < n; ++i )
		{
			ReportedItem ri;
			ri.uid = r.u32();
			ri.type = (int)r.u16();
			ri.status = (int)r.u8();
			ri.beatitude = (int)(Sint16)r.u16();
			ri.count = (int)(Sint16)r.u16();
			ri.appearance = r.u32();
			ri.identified = r.u8() != 0;
			ri.x = (Sint32)r.u32();
			ri.y = (Sint32)r.u32();
			ri.worn = r.u8();
			ri.ownerUid = r.u32();
			ri.droppable = ( r.u8() & 1 ) != 0;
			if ( !r.ok ) { return false; }
			out.push_back(ri);
		}
		return true;
	}

	// Replace `player`'s mirrored backpack with what their machine just reported. Proxy uids
	// are kept per item (keyed by the uid the owner's machine uses, which survives moving,
	// equipping and stacking into it), so a uid a script got from sam_get_inventory keeps
	// naming the same item across reports until the item really leaves the backpack.
	void applyBackpack(int player, const std::vector<ReportedItem>& reported)
	{
		Mirror& m = s_mirror[player];
		list_FreeAll(&m.items);
		m.items.first = nullptr;
		m.items.last = nullptr;
		for ( int k = 0; k < NUM_SLOTS; ++k ) { m.worn[k] = nullptr; }

		std::map<std::uint32_t, std::uint32_t> kept;   // owner uid -> proxy, for this report
		for ( const ReportedItem& ri : reported )
		{
			// A type this machine has no slot for (a mod the host does not run) cannot be
			// read safely by any item function, so it is left out rather than guessed.
			if ( ri.uid == 0 || ri.type < 0 || ri.type >= NUM_ITEM_SLOTS ) { continue; }
			// The same rule for a mod item number this host has no definition for. Without
			// this the item was listed under the literal name "custom", which the docs
			// promise no longer happens, and every type-based reader (category, stack limit,
			// sam_get_inventory_count) answered from an empty slot.
			if ( ri.type >= NUMITEMS && !SAMItems::getItem(ri.type) ) { continue; }
			if ( kept.count(ri.uid) ) { continue; }   // one item per uid; a duplicate is noise
			std::uint32_t proxy = 0;
			auto known = s_byOwner.find({ player, ri.uid });
			if ( known != s_byOwner.end() ) { proxy = known->second; }
			else
			{
				proxy = newProxy();
				if ( proxy == 0 ) { continue; }
				s_byOwner[{ player, ri.uid }] = proxy;
				s_byProxy[proxy] = ProxyOwner{ player, ri.uid };
			}
			kept[ri.uid] = proxy;

			Item* it = newMirrorItem(&m.items);
			if ( !it ) { continue; }
			it->type = (ItemType)ri.type;
			const int st = ri.status < (int)BROKEN ? (int)BROKEN : ( ri.status > (int)EXCELLENT ? (int)EXCELLENT : ri.status );
			it->status = (Status)st;
			it->beatitude = (Sint16)( ri.beatitude < -100 ? -100 : ( ri.beatitude > 100 ? 100 : ri.beatitude ) );
			// Deliberately no stack ceiling here, unlike the worn copy in onWornChanged: the
			// mirror's job is to report what that player really carries, and a count clamped
			// down here would make sam_get_inventory_count disagree with the owner's own game.
			it->count = (Sint16)( ri.count < 1 ? 1 : ri.count );
			it->appearance = ri.appearance;
			it->identified = ri.identified;
			it->uid = proxy;
			it->x = ri.x;
			it->y = ri.y;
			it->ownerUid = ri.ownerUid;
			it->isDroppable = ri.droppable;
			if ( ri.worn < NUM_SLOTS && !m.worn[ri.worn] ) { m.worn[ri.worn] = it; }
		}

		// Items that left the backpack take their proxies with them.
		for ( auto it = s_byOwner.begin(); it != s_byOwner.end(); )
		{
			if ( it->first.first == player && !kept.count(it->first.second) )
			{
				s_byProxy.erase(it->second);
				it = s_byOwner.erase(it);
			}
			else { ++it; }
		}
		m.haveItems = true;
	}

	// ---------------------------------------------------------------- wire: spells

	// [u8 version][u16 n] then per spell: [u32 id][str8 name]
	//
	// The NAME is what the host goes by. A mod's spells are numbered in the order the mods
	// loaded (SAMSpells hands out 2000, 2001... as it registers them), so one number can be a
	// different spell on another machine; "namespace:spell" and a vanilla internal name are
	// the same everywhere. The id rides along for a spell that somehow has no name.
	std::string buildSpells(int player)
	{
		SAMNet::Writer ids;
		std::size_t n = 0;
		for ( node_t* node = players[player]->magic.spellList.first; node && n < MAX_SPELLS; node = node->next )
		{
			const spell_t* sp = (const spell_t*)node->element;
			if ( !sp ) { continue; }
			ids.u32((std::uint32_t)sp->ID);
			ids.str8(SAMSpells::scriptName(sp->ID));
			++n;
		}
		SAMNet::Writer w;
		w.u8(REPORT_VERSION);
		w.u16((std::uint16_t)n);
		w.buf += ids.buf;
		return w.buf;
	}

	// ---------------------------------------------------------------- client side

	bool s_reporting = false;       // the host asked for reports (so it runs S.A.M)
	bool s_reportNow = false;
	Uint32 s_lastCheck = 0;
	std::string s_sentBackpack;
	std::string s_sentSpells;

	// What each of this player's ten worn slots held at the end of the last tick, so a
	// carried write can be echoed as "these fields changed" rather than as a whole item.
	// That is what lets the host tell its copy apart from an identical twin, and it is
	// also the change detection: a mod that writes the same value every tick sends nothing.
	struct WornSnap
	{
		Uint32 uid = 0;            // 0: nothing seen in this slot yet, so nothing to diff
		int type = 0;
		int status = 0;
		int beatitude = 0;
		int count = 0;
		Uint32 appearance = 0;
		bool identified = false;
		bool droppable = true;
		Uint32 ownerUid = 0;
	};
	WornSnap s_worn[NUM_SLOTS];
	bool s_wornDirty[NUM_SLOTS] = {};

	// One outgoing refusal per (function, reason) per cooldown. A host script polling a uid
	// whose item is gone refuses every tick, and the reliable channel would carry every one.
	std::map<std::string, Uint32> s_refusalSent;

	void forgetWornSnapshots()
	{
		for ( int k = 0; k < NUM_SLOTS; ++k ) { s_worn[k] = WornSnap(); s_wornDirty[k] = false; }
	}

	void onReportRequest(const std::string& body)
	{
		(void)body;
		s_reporting = true;
		s_reportNow = true;
		// Forget what was sent: the host may have dropped its copy (this is how a host that
		// restarted its view of us asks for everything again).
		s_sentBackpack.clear();
		s_sentSpells.clear();
		// Same reasoning for the worn slots: with no idea what the host holds, the next echo
		// has to carry every field rather than a difference against a copy it may not have.
		forgetWornSnapshots();
	}

	// Send the worn-item echo for every slot a carried call wrote to this tick, and take a
	// fresh snapshot of all ten. Runs once per tick, from the tick hook, which is where the
	// channel is flushed anyway: nothing is sent later than it used to be.
	void flushWornEchoes()
	{
		for ( int k = 0; k < NUM_SLOTS; ++k )
		{
			Item** slot = slotPtr(stats[clientnum], k);
			Item* it = slot ? *slot : nullptr;
			if ( !it || it->uid == 0 )
			{
				s_worn[k] = WornSnap();
				s_wornDirty[k] = false;
				continue;
			}
			const WornSnap& was = s_worn[k];
			// A different item in the slot than the one we last looked at: we have no idea
			// what the host's copy of THIS one holds, so the echo carries every field.
			const bool sameItem = ( was.uid == it->uid && was.type == (int)it->type );
			if ( s_wornDirty[k] )
			{
				std::uint16_t mask = WornBit::All;
				if ( sameItem )
				{
					mask = 0;
					if ( was.status != (int)it->status ) { mask |= WornBit::Status; }
					if ( was.beatitude != (int)it->beatitude ) { mask |= WornBit::Beatitude; }
					if ( was.count != (int)it->count ) { mask |= WornBit::Count; }
					if ( was.appearance != it->appearance ) { mask |= WornBit::Appearance; }
					if ( was.identified != it->identified ) { mask |= WornBit::Identified; }
					if ( was.droppable != it->isDroppable ) { mask |= WornBit::Droppable; }
					if ( was.ownerUid != it->ownerUid ) { mask |= WornBit::OwnerUid; }
				}
				// Nothing actually changed: a mod that writes the same value every tick (a
				// "keep this crown pristine" line in on_tick) sent a reliable frame a tick
				// before this check existed.
				if ( mask != 0 )
				{
					SAMNet::Writer w;
					w.u8((std::uint8_t)k);
					w.u16((std::uint16_t)it->type);
					w.u16(mask);
					w.u8((std::uint8_t)it->status);
					w.u16((std::uint16_t)(Sint16)it->beatitude);
					w.u16((std::uint16_t)(Sint16)it->count);
					w.u32(it->appearance);
					w.u8(it->identified ? 1 : 0);
					w.u8(it->isDroppable ? 1 : 0);
					w.u32(it->ownerUid);
					SAMNet::sendToHost(Op::WornChanged, w.buf);
				}
			}
			s_wornDirty[k] = false;
			WornSnap& now = s_worn[k];
			now.uid = it->uid;
			now.type = (int)it->type;
			now.status = (int)it->status;
			now.beatitude = (int)it->beatitude;
			now.count = (int)it->count;
			now.appearance = it->appearance;
			now.identified = it->identified;
			now.droppable = it->isDroppable;
			now.ownerUid = it->ownerUid;
		}
	}

	void clientTick()
	{
		// A destroy the host carried here (sam_remove_item, sam_set_item_count 0) and a spell
		// removal are queued on this machine, and game.cpp drains the queue on the host only.
		// This hook runs from SAMNet::tick, after every script and every carried call of this
		// tick, with no engine item code on the stack: the same guarantee the host's drain has.
		SAMItems::drainDestroyQueue();

		if ( !s_reporting ) { return; }
		if ( clientnum < 0 || clientnum >= MAXPLAYERS || !validPlayer(clientnum) ) { return; }
		// Before the interval gate below: the host's copy of a worn item is what its combat
		// maths reads, so it is corrected on the tick of the write, not a fifth of a second
		// later, and the snapshot it is diffed against has to be taken every tick.
		flushWornEchoes();
		if ( !s_reportNow && (Uint32)(ticks - s_lastCheck) < REPORT_INTERVAL ) { return; }
		s_lastCheck = ticks;
		s_reportNow = false;

		// Whole snapshots, sent only when they differ from the last one sent. A backpack is a
		// few KB at most and the channel is ordered, so there is no delta to get wrong.
		std::string backpack = buildBackpack(clientnum);
		if ( backpack != s_sentBackpack )
		{
			if ( SAMNet::sendToHost(Op::Backpack, backpack) ) { s_sentBackpack = std::move(backpack); }
		}
		std::string spells = buildSpells(clientnum);
		if ( spells != s_sentSpells )
		{
			if ( SAMNet::sendToHost(Op::Spells, spells) ) { s_sentSpells = std::move(spells); }
		}
	}

	// ---------------------------------------------------------------- host side

	void onBackpack(int from, const std::string& body)
	{
		if ( !isMirrorable(from) ) { return; }
		std::vector<ReportedItem> reported;
		if ( !parseBackpack(body, reported) )
		{
			SAMNet::warnOnce("inv:badpack:" + std::to_string(from), "Player " + std::to_string(from)
				+ "'s game sent a backpack report this host could not read; their backpack stays as last reported.");
			return;
		}
		applyBackpack(from, reported);
	}

	void onSpells(int from, const std::string& body)
	{
		if ( !isMirrorable(from) ) { return; }
		SAMNet::Reader r(body);
		if ( r.u8() != REPORT_VERSION || !r.ok ) { return; }
		const std::size_t n = r.u16();
		if ( !r.ok || n > MAX_SPELLS ) { return; }
		std::vector<int> ids;
		std::vector<std::string> names;
		ids.reserve(n);
		names.reserve(n);
		for ( std::size_t i = 0; i < n; ++i )
		{
			const int reported = (int)r.u32();
			std::string name = r.str8();
			if ( !r.ok ) { return; }
			// This host's id for it, found by name (see buildSpells). A spell this host has no
			// such name for (a mod it does not run) keeps its name for sam_get_spells, and gets no
			// id, so sam_player_knows_spell can never match it to a different spell by number.
			int id = name.empty() ? -1 : SAMSpells::resolveSpellRef(name);
			if ( name.empty() && reported >= 0 && reported < SAM_SPELL_ID_BASE )
			{
				// Nameless on the owner's machine: a vanilla number means the same spell everywhere.
				id = reported;
				name = SAMSpells::scriptName(id);
			}
			ids.push_back(id);
			names.push_back(std::move(name));
		}
		Mirror& m = s_mirror[from];
		m.spells = std::move(ids);
		m.spellNames = std::move(names);
		m.haveSpells = true;
	}

	// [u8 slot][u16 type][u16 changed mask][u8 status][u16 beatitude][u16 count]
	// [u32 appearance][u8 identified][u8 flags: bit0 droppable][u32 ownerUid]
	//
	// The owner changed an item its player is WEARING, because the host carried a script's
	// call there. The engine's copy of that item on the host (stats[p]'s slot, which combat,
	// AC and stat bonuses read, and which a thief copies isDroppable from when it steals worn
	// armour, entity.cpp:15044) has to match. The vanilla client->host echoes cannot do it
	// safely, which is why this is a S.A.M op rather than one of them:
	//   * BEAT (net.cpp:9505) and REPA (9391) have no amulet or ring slot, and REPA does not
	//     even check the item type;
	//   * EQUA (8965) only applies when every other field already matches, and never
	//     carries status or count;
	//   * EQUI/EQUS/EQUM with a new count is how vanilla updates a count (items.cpp:4360),
	//     but when the host's copy already has that count, equipItem takes it as a second
	//     equip of the same item and UNEQUIPS it on the host (items.cpp:2646-2762).
	// This one sets the fields outright, after checking the host's copy is still that item.
	// Only a S.A.M client sends it, and only when a S.A.M host carried a call to it.
	//
	// IDENTITY. The frame says which fields the write changed. The host writes those, and for
	// the identifying fields the write did NOT change (WornBit::Identity) it requires its own
	// copy to agree before it writes anything -- the same rule EQUA follows
	// (net.cpp:9069-9084) and for the same reason: matching on the item TYPE alone let an
	// identical twin take the write when the player swapped gear in the window between the
	// write and the frame arriving (the two travel on different channels, and Barony's
	// reliable packets are unordered). Two items with the same type AND the same appearance
	// are still indistinguishable here; see the doc note.
	void onWornChanged(int from, const std::string& body)
	{
		if ( !isMirrorable(from) ) { return; }
		SAMNet::Reader r(body);
		const int k = r.u8();
		const int type = (int)r.u16();
		const std::uint16_t mask = r.u16();
		const int status = (int)r.u8();
		const int beatitude = (int)(Sint16)r.u16();
		const int count = (int)(Sint16)r.u16();
		const std::uint32_t appearance = r.u32();
		const bool identified = r.u8() != 0;
		const bool droppable = ( r.u8() & 1 ) != 0;
		const std::uint32_t ownerUid = r.u32();
		if ( !r.ok || k < 0 || k >= NUM_SLOTS || !stats[from] ) { return; }
		Item** slot = slotPtr(stats[from], k);
		if ( !slot || !*slot ) { return; }
		Item* copy = *slot;
		// A different item in that slot now (re-equipped since): the report is about an item
		// the host no longer holds, and the next equip already told it the truth.
		if ( (int)copy->type != type ) { return; }
		const std::uint16_t check = (std::uint16_t)( WornBit::Identity & ~mask );
		const bool same =
			( !(check & WornBit::Appearance) || copy->appearance == appearance )
			&& ( !(check & WornBit::Identified) || copy->identified == identified );
		if ( !same )
		{
			// Keyed on the player and the slot, so a script writing every tick cannot fill
			// the log. The mirror the readers answer from is still right; it is only the
			// host's own combat copy that keeps what the last equip gave it.
			SAMNet::warnOnce("inv:wornstale:" + std::to_string(from) + ":" + std::to_string(k),
				"Player " + std::to_string(from) + "'s game changed an item they are wearing, but this host's"
				" copy of that slot is a different item by now (they swapped gear in the same moment). The"
				" change was not applied to the host's copy; it applies again the next time the item is equipped.");
			return;
		}
		if ( mask & WornBit::Status )
		{
			copy->status = (Status)( status < (int)BROKEN ? (int)BROKEN : ( status > (int)EXCELLENT ? (int)EXCELLENT : status ) );
		}
		if ( mask & WornBit::Beatitude )
		{
			copy->beatitude = (Sint16)( beatitude < -100 ? -100 : ( beatitude > 100 ? 100 : beatitude ) );
		}
		if ( mask & WornBit::Count )
		{
			// sam_set_item_count refuses anything above the engine's per-item, per-player
			// ceiling, so the echo of that same write is clamped to it too rather than letting
			// a modified client put an arbitrary stack on the copy the host fights with. Only
			// when the ceiling really is a ceiling: getMaxStackLimit answers 1 for worn gear
			// the engine never stacks, and the vanilla equip packet already delivers such an
			// item's real count, so clamping there would make the copy disagree with EQUI.
			const int cap = copy->getMaxStackLimit(from);
			const int n = count < 1 ? 1 : ( cap > 1 && count > cap ? cap : count );
			copy->count = (Sint16)n;
		}
		if ( mask & WornBit::Appearance ) { copy->appearance = appearance; }
		if ( mask & WornBit::Identified ) { copy->identified = identified; }
		if ( mask & WornBit::Droppable ) { copy->isDroppable = droppable; }
		// sam_set_item_owner writes this and nothing else. Without it on the wire the host's
		// mirror and the host's own combat copy answered differently for the same item, and
		// the shopkeeper/thief rules read the copy.
		if ( mask & WornBit::OwnerUid ) { copy->ownerUid = ownerUid; }
	}

	void onRefused(int from, const std::string& body)
	{
		SAMNet::Reader r(body);
		const std::string fn = r.str8();
		const std::string why = r.str16();
		const std::string code = r.str8();
		if ( !r.ok ) { return; }
		// Keyed on the REASON CODE, never on the message: the message names the uid, so a
		// script writing to uids it kept after the items were used up wrote one warning line
		// per uid here (and left one warned-key string per uid behind for the session) while
		// the client it came from logged once. The full text is still printed, once.
		//
		// But `fn` and `code` came off the wire, and the warned-key set is never pruned, so a
		// peer sending made-up names would grow it without limit and write a log line for each.
		// Only a name the contract table knows may reach the key; anything else collapses to
		// one key for that player, which bounds the set at players x functions x codes.
		const bool knownFn = ( SAMNet::contractFor(fn) != nullptr );
		const std::string keyCode = ( knownFn && code.size() <= 32 && !code.empty() ) ? code : std::string("?");
		SAMNet::warnOnce("inv:refused:" + std::to_string(from) + ":" + ( knownFn ? fn : std::string("?") ) + ":" + keyCode,
			"Player " + std::to_string(from) + "'s game could not do " + ( knownFn ? fn : std::string("a S.A.M call") )
			+ ": " + why.substr(0, 200));
	}

	void hostTick()
	{
		for ( int p = 1; p < MAXPLAYERS; ++p )
		{
			Mirror& m = s_mirror[p];
			if ( ( m.haveItems || m.haveSpells || m.items.first ) && !isMirrorable(p) ) { dropMirror(p); }
			// Nothing mirrored for a player whose machine runs S.A.M: ask them to report.
			// Normally the HELLO did that once and their game keeps the host up to date by
			// itself, but a new run drops the mirror on both machines and the two do not reach
			// doNewGame at the same moment. If that player's game reset FIRST it has already
			// sent its new character's backpack, this host threw it away a moment later, and
			// nothing would have made it send the same bytes again until the backpack next
			// changed -- which can be a whole floor. One small frame a second closes that, and
			// heals a report lost to anything else too.
			else if ( isMirrorable(p) && SAMNet::peerHasSam(p) && ( !m.haveItems || !m.haveSpells )
				&& (Uint32)(ticks - m.askedTick) >= ASK_INTERVAL )
			{
				m.askedTick = ticks;
				SAMNet::Writer w;
				w.u8(REPORT_VERSION);
				SAMNet::sendToClient(p, Op::ReportRequest, w.buf);
			}
		}
	}

	// ---------------------------------------------------------------- hooks

	void onTick()
	{
		if ( multiplayer == CLIENT ) { clientTick(); }
		else if ( multiplayer == SERVER ) { hostTick(); }
	}

	void onHello(int player)
	{
		// A fresh start for that slot: whatever we mirrored belonged to an earlier session.
		dropMirror(player);
		SAMNet::Writer w;
		w.u8(REPORT_VERSION);
		SAMNet::sendToClient(player, Op::ReportRequest, w.buf);
		if ( player >= 0 && player < MAXPLAYERS ) { s_mirror[player].askedTick = ticks; }
	}

	void onClear()
	{
		for ( int p = 0; p < MAXPLAYERS; ++p ) { dropMirror(p); }
		s_byProxy.clear();
		s_byOwner.clear();
		s_nextProxy = PROXY_FIRST;
		s_reporting = false;
		s_reportNow = false;
		s_lastCheck = 0;
		s_sentBackpack.clear();
		s_sentSpells.clear();
		forgetWornSnapshots();
		s_refusalSent.clear();
		SAMItems::clearDestroyQueue();
		SAMSpells::clearRemoveQueue();
	}

	// Installed into SAMNet: the trampoline asks this for every item-uid argument on the host.
	// A proxy of a connected remote player's item -> that player and the uid their machine
	// uses, so an `owner` call is carried there. Anything else (the host's own items, a proxy
	// whose player has left) stays here.
	bool routeItem(std::uint32_t uid, int& player, std::uint32_t& ownerUid)
	{
		if ( multiplayer != SERVER ) { return false; }
		auto it = s_byProxy.find(uid);
		if ( it == s_byProxy.end() ) { return false; }
		if ( !isMirrorable(it->second.player) ) { return false; }
		if ( uidToItem(uid) ) { return false; }   // a real item of this machine always wins
		player = it->second.player;
		ownerUid = it->second.ownerUid;
		return true;
	}

	struct Registration
	{
		Registration()
		{
			SAMNet::setItemRouter(&routeItem);
			SAMNet::onClientOp(Op::ReportRequest, &onReportRequest);
			SAMNet::onHostOp(Op::Backpack, &onBackpack);
			SAMNet::onHostOp(Op::Spells, &onSpells);
			SAMNet::onHostOp(Op::WornChanged, &onWornChanged);
			SAMNet::onHostOp(Op::Refused, &onRefused);
		}
	};
	Registration s_registration;
	SAMNet::TickHook s_tickHook(&onTick);
	SAMNet::HelloHook s_helloHook(&onHello);
	SAMNet::ClearHook s_clearHook(&onClear);

	// ---------------------------------------------------------------- shared by the readers

	// Which list holds `player`'s backpack here, with the warning when it cannot be seen.
	Seen backpackList(int player, const char* fn, list_t** out)
	{
		if ( !validPlayer(player) ) { return Seen::Invalid; }
		if ( players[player]->isLocalPlayer() ) { *out = &stats[player]->inventory; return Seen::Yes; }
		const std::string name = fn ? fn : "?";
		if ( multiplayer == SERVER )
		{
			if ( client_disconnected[player] ) { return Seen::Invalid; }
			if ( s_mirror[player].haveItems ) { *out = &s_mirror[player].items; return Seen::Yes; }
			SAMNet::warnOnce("inv:unseen:" + name + ":" + std::to_string(player),
				name + ": player " + std::to_string(player) + "'s backpack is on their own machine, and their game "
				"has not reported it yet. Their game reports a moment after joining, again for the first ticks of "
				"every new run (the host drops the last character's copy when the run starts, so game.on_game_start "
				"is too early to read it: use a short timer or a later event), and never if it does not run S.A.M "
				"scripts. Returning nil.");
			return Seen::No;
		}
		if ( multiplayer == CLIENT )
		{
			SAMNet::warnOnce("inv:unseen:" + name + ":" + std::to_string(player),
				name + ": a client only sees its own player's backpack; ask on the host. Returning nil.");
			return Seen::No;
		}
		return Seen::Invalid;   // singleplayer: an empty slot
	}
}

// -------------------------------------------------------------------- public

list_t* listFor(int player)
{
	if ( !validPlayer(player) ) { return nullptr; }
	if ( players[player]->isLocalPlayer() ) { return &stats[player]->inventory; }
	if ( isMirrorable(player) && s_mirror[player].haveItems ) { return &s_mirror[player].items; }
	return nullptr;
}

Seen inventoryRows(int player, const char* fn, std::vector<InventoryRow>* out)
{
	list_t* list = nullptr;
	const Seen seen = backpackList(player, fn, &list);
	if ( seen != Seen::Yes || !out ) { return seen; }
	for ( node_t* node = list->first; node; node = node->next )
	{
		const Item* it = (const Item*)node->element;
		if ( !it ) { continue; }
		InventoryRow row;
		row.uid = it->uid;
		row.type = (int)it->type;
		row.name = inventoryName((int)it->type);
		row.count = (int)it->count;
		row.beatitude = (int)it->beatitude;
		row.status = (int)it->status;
		row.identified = it->identified;
		row.equipped = isEquipped(player, it);
		out->push_back(std::move(row));
	}
	return seen;
}

Seen countOf(int player, int type, const char* fn, long long* out)
{
	list_t* list = nullptr;
	const Seen seen = backpackList(player, fn, &list);
	if ( seen != Seen::Yes ) { return seen; }
	long long total = 0;
	for ( node_t* node = list->first; node; node = node->next )
	{
		const Item* it = (const Item*)node->element;
		if ( it && (int)it->type == type ) { total += it->count; }
	}
	if ( out ) { *out = total; }
	return seen;
}

Seen spellIdsOf(int player, const char* fn, std::vector<int>* out)
{
	if ( !validPlayer(player) ) { return Seen::Invalid; }
	if ( players[player]->isLocalPlayer() )
	{
		for ( node_t* node = players[player]->magic.spellList.first; node; node = node->next )
		{
			const spell_t* sp = (const spell_t*)node->element;
			if ( sp && out ) { out->push_back(sp->ID); }
		}
		return Seen::Yes;
	}
	const std::string name = fn ? fn : "?";
	if ( multiplayer == SERVER )
	{
		if ( client_disconnected[player] ) { return Seen::Invalid; }
		if ( s_mirror[player].haveSpells )
		{
			// -1 is a spell this host has nothing by that name for: no id here can mean it.
			if ( out ) { for ( const int id : s_mirror[player].spells ) { if ( id >= 0 ) { out->push_back(id); } } }
			return Seen::Yes;
		}
		SAMNet::warnOnce("inv:unseenspells:" + name + ":" + std::to_string(player),
			name + ": player " + std::to_string(player) + "'s spells are on their own machine, and their game "
			"has not reported them yet. Their game reports a moment after joining, again for the first ticks of "
			"every new run (the host drops the last character's copy when the run starts, so game.on_game_start "
			"is too early to read it: use a short timer or a later event), and never if it does not run S.A.M "
			"scripts. Returning nil.");
		return Seen::No;
	}
	if ( multiplayer == CLIENT )
	{
		SAMNet::warnOnce("inv:unseenspells:" + name + ":" + std::to_string(player),
			name + ": a client only sees its own player's spells; ask on the host. Returning nil.");
		return Seen::No;
	}
	return Seen::Invalid;   // singleplayer: an empty slot
}

Seen spellNamesOf(int player, const char* fn, std::vector<std::string>* out)
{
	// A remote player's spells by the names their own machine reported: right even for a
	// spell this host numbers differently, or does not have at all.
	if ( validPlayer(player) && isMirrorable(player) && s_mirror[player].haveSpells )
	{
		if ( out )
		{
			for ( const std::string& nm : s_mirror[player].spellNames ) { if ( !nm.empty() ) { out->push_back(nm); } }
		}
		return Seen::Yes;
	}
	std::vector<int> ids;
	const Seen seen = spellIdsOf(player, fn, &ids);
	if ( seen != Seen::Yes || !out ) { return seen; }
	for ( const int id : ids )
	{
		std::string name = SAMSpells::scriptName(id);
		if ( !name.empty() ) { out->push_back(std::move(name)); }
	}
	return seen;
}

Item* findMirrorItem(std::uint32_t uid, int* owner)
{
	if ( owner ) { *owner = -1; }
	if ( multiplayer != SERVER ) { return nullptr; }
	auto found = s_byProxy.find(uid);
	if ( found == s_byProxy.end() ) { return nullptr; }
	const int p = found->second.player;
	if ( !isMirrorable(p) ) { return nullptr; }
	for ( node_t* node = s_mirror[p].items.first; node; node = node->next )
	{
		Item* it = (Item*)node->element;
		if ( it && it->uid == uid )
		{
			if ( owner ) { *owner = p; }
			return it;
		}
	}
	return nullptr;
}

Item* resolveItem(long long uid, Use use, const char* fn, int* holder)
{
	if ( holder ) { *holder = -1; }
	if ( uid > 0 && uid <= 0xFFFFFFFFLL )
	{
		const Uint32 u = (Uint32)uid;
		if ( Item* it = uidToItem(u) )
		{
			if ( holder )
			{
				for ( int p = 0; p < MAXPLAYERS; ++p )
				{
					if ( stats[p] && it->node && it->node->list == &stats[p]->inventory ) { *holder = p; break; }
				}
			}
			return it;
		}
		int owner = -1;
		if ( Item* mirrored = findMirrorItem(u, &owner) )
		{
			if ( use == Use::Read )
			{
				if ( holder ) { *holder = owner; }
				return mirrored;
			}
			// A write reaches a mirrored item only when the trampoline could not carry the
			// call to its owner. Changing the host's copy would change nothing anyone plays with.
			const std::string name = fn ? fn : "?";
			SAMNet::warnOnce("inv:mirrorwrite:" + name, name + ": uid " + std::to_string(uid) + " is player "
				+ std::to_string(owner) + "'s item, on their machine, and their game could not be reached; nothing changed.");
			return nullptr;
		}
	}
	// Once per function, not per uid: a script polling uids it kept after the items were used
	// up would otherwise write a line per uid.
	const std::string name = fn ? fn : "?";
	const std::string why = "no item with uid " + std::to_string(uid) + " here. It may have been used up, dropped"
		" or destroyed. An item uid names an item on one machine: the host sees every player's items"
		" (sam_get_inventory hands out their uids), a client only its own.";
	SAMNet::warnOnce("inv:miss:" + name, name + ": " + why);
	// The host sent this call here and has already told its script "sent": say why it did
	// nothing, in the host's log, where that script's author is looking. The reason code, not
	// the message, is what both ends dedupe on: the message names the uid.
	noteOwnerRefusal(fn, why, false, "missing");
	return nullptr;
}

bool isEquipped(int player, const Item* item)
{
	if ( !item || !validPlayer(player) ) { return false; }
	if ( isMirrorItemOf(player, item) )
	{
		const Mirror& m = s_mirror[player];
		for ( int k = 0; k < NUM_SLOTS; ++k ) { if ( m.worn[k] == item ) { return true; } }
		return false;
	}
	return wornSlotOf(player, item) >= 0;
}

void afterOwnerWrite(const Item* item)
{
	if ( multiplayer != CLIENT || !item ) { return; }
	// Only mark the slot. The frame itself is built at the end of this same tick, in
	// flushWornEchoes, where the item can be compared with what it held before the scripts
	// ran: the fields that actually changed are the ones the host writes, and the ones that
	// did not are how it tells its copy from an identical twin. Nothing goes out later than
	// it used to -- the channel is flushed after the tick hooks either way.
	const int k = wornSlotOf(clientnum, item);
	if ( k >= 0 ) { s_wornDirty[k] = true; }
	// The backpack report goes out on this tick's check rather than up to a fifth of a second
	// later, so a host script that changes an item and then reads it again sees it sooner.
	// Left unconditional on purpose: this only asks for an earlier CHECK, and the check sends
	// nothing unless the snapshot's bytes differ, so a write that changed nothing costs one
	// rebuild and no packet -- while a write to a field the echo does not carry (a grid
	// position, the slot an item is worn in) still reaches the mirror on the next tick.
	s_reportNow = true;
}

void nudgeReport()
{
	// For a writer that has no item left to echo: sam_remove_item and sam_set_item_count(uid, 0)
	// queue a destroy and return before afterOwnerWrite, so without this the host went on
	// seeing the destroyed item for up to REPORT_INTERVAL ticks and a quest check written as
	// "if they still carry it, give the reward" paid out several times for one item.
	if ( multiplayer == CLIENT ) { s_reportNow = true; }
}

void resetForNewRun()
{
	// A new run reaches a brand new character WITHOUT the game ending: dying and restarting,
	// and the host taking a co-op lobby back to the character screen, both get there and
	// neither runs doEndgame, which is the only thing that calls SAMNet::clear() and so the
	// only thing that ran onClear below. Everything keyed to the character that just ended
	// therefore has to be dropped here, in doNewGame, the one function every route passes
	// through -- otherwise the host serves the PREVIOUS character's backpack and spell list
	// as the new one's for the first ticks of the run, which is exactly when
	// game.on_game_start fires and a starter-kit mod asks "do they already have this?".
	//
	// This runs on every machine, not only the host: the client half is what makes its game
	// re-report the new character at once instead of waiting out the report interval.
	//
	// It deliberately does NOT touch s_reporting, or anything of SAMNet's. s_reporting is set
	// by the host's one report request, which is sent from HELLO and never comes again for a
	// client that stayed connected through the restart: clearing it would switch that
	// player's reporting off for the rest of the session.
	for ( int p = 0; p < MAXPLAYERS; ++p ) { dropMirror(p); }
	s_byProxy.clear();
	s_byOwner.clear();
	s_nextProxy = PROXY_FIRST;
	s_reportNow = true;
	s_lastCheck = 0;
	s_sentBackpack.clear();
	s_sentSpells.clear();
	forgetWornSnapshots();
	s_refusalSent.clear();
	// A destroy or a spell removal queued in the last frame of the old run is resolved at
	// drain time by uid and by (player, spell): left in the queue it lands on the new
	// character, who is the same player slot.
	SAMItems::clearDestroyQueue();
	SAMSpells::clearRemoveQueue();
}

void noteOwnerRefusal(const char* fn, const std::string& why, bool logHere, const char* code)
{
	const std::string name = fn ? fn : "?";
	// Everything here is keyed on (function, reason code) rather than on the message, because
	// several messages name a uid: keying on the text deduped nothing for exactly the script
	// that needs deduping, the one polling a list of uids in on_tick.
	const std::string key = name + "|" + ( code ? code : "refused" );
	if ( logHere ) { SAMNet::warnOnce("inv:ref:" + key, name + ": " + why); }
	if ( multiplayer == CLIENT && SAMNet::inForwardedCall() )
	{
		// The host's log is deduped, but the frame carrying the refusal was not: a host script
		// polling a dead uid every tick put one reliable packet a tick on the wire until the
		// next backpack report retired the proxy. Say it again only after the cooldown, in
		// case the host's own dedupe has been reset by a new run.
		auto seen = s_refusalSent.find(key);
		if ( seen != s_refusalSent.end() && (Uint32)(ticks - seen->second) < REFUSAL_COOLDOWN ) { return; }
		s_refusalSent[key] = ticks;
		SAMNet::Writer w;
		w.str8(name);
		w.str16(why);
		w.str8(code ? code : "refused");
		SAMNet::sendToHost(Op::Refused, w.buf);
	}
}

bool deliverItem(int player, Item* item, const char* fn)
{
	if ( !item ) { return false; }
	const std::string name = fn ? fn : "?";
	if ( !validPlayer(player) )
	{
		free(item);
		SAM_ERROR("ITEMS", name + ": invalid player index " + std::to_string(player) + ".");
		return false;
	}
	if ( players[player]->isLocalPlayer() )
	{
		// Copies or merges into the backpack and returns the backpack's item, never ours.
		itemPickup(player, item);
		free(item);
		return true;
	}
	if ( isMirrorable(player) )
	{
		// The engine's own remote pickup (items.cpp:4253-4268): it stamps the owner, sends the
		// vanilla ITEM packet to that player's machine, whose handler picks it up there
		// (net.cpp:4993), and hands `item` straight back without keeping anything on the host.
		// A stock client takes ITEM too; a mod item it has no slot for arrives as a rock.
		itemPickup(player, item);
		free(item);
		return true;
	}
	free(item);
	SAM_ERROR("ITEMS", name + ": player " + std::to_string(player) + " is not in this game (not connected,"
		" or an empty slot); nothing given.");
	return false;
}

std::string inventoryName(int type)
{
	if ( type >= 0 && type < NUMITEMS ) { return std::string(itemNameStrings[type + 2]); }
	// A mod's item answers with its own "ns:item" id: the one every S.A.M call accepts, and
	// different for every mod item, which the literal word "custom" was not. For a remote
	// player's item this is THIS machine's name for that type number, so it is right only
	// while both machines load the same mods in the same order; applyBackpack drops a type
	// this machine has no definition for, so a mirrored item never reaches the line below.
	if ( const SAMItemDef* def = SAMItems::getItem(type) ) { return def->id; }
	return "custom";
}

}
