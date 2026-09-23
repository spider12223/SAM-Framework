/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_mp_input.cpp
	Desc: see sam_mp_input.hpp.

-------------------------------------------------------------------------------*/

#include "sam_mp_input.hpp"
#include "sam_net.hpp"
#include "sam_event.hpp"
#include "sam_logger.hpp"
#include "sam_lua_runtime.hpp"   // actionNameForIndex, dispatchAction, isKeyHeld
#include "sam_mp_inventory.hpp"  // listFor: the host's mirror of a remote player's backpack
#include "sam_settings.hpp"      // the actions mods registered (sam_register_action), polled by name

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "main.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "entity.hpp"      // itemNameStrings
#include "items.hpp"       // Item, items[], useItem, uidToItem, NUM_ITEM_SLOTS
#include "player.hpp"      // players[], inputs
#include "input.hpp"       // Input::inputs[] -- const reads only, never consume*
#include "net.hpp"         // messagePlayer, sendPacketSafe, keepInventoryGlobal
#include "interface/interface.hpp"   // CalloutMenu

namespace SAMMpInput
{
namespace
{
	// ---------------------------------------------------------------- ops (see the header)

	constexpr std::uint8_t OP_EDGES   = (std::uint8_t)(SAMNet::Op::InputFirst + 0);   // client -> host
	constexpr std::uint8_t OP_STATE   = (std::uint8_t)(SAMNet::Op::InputFirst + 1);   // client -> host
	constexpr std::uint8_t OP_READY   = (std::uint8_t)(SAMNet::Op::InputFirst + 0);   // host -> client
	constexpr std::uint8_t OP_USE     = (std::uint8_t)(SAMNet::Op::EventFirst + 0);   // client -> host
	constexpr std::uint8_t OP_VERDICT = (std::uint8_t)(SAMNet::Op::EventFirst + 0);   // host -> client
	constexpr std::uint8_t OP_UNPING  = (std::uint8_t)(SAMNet::Op::EventFirst + 1);   // host -> client

	constexpr std::uint16_t READY_VERSION = 1;
	constexpr std::size_t MAX_PENDING_EVENTS = 64;     // raised on a client before the host said READY
	constexpr std::size_t MAX_PENDING_USES = 64;       // deliberate uses waiting for the host's answer
	constexpr std::size_t MAX_APPROVALS = 32;          // per player, on the host
	constexpr Uint32 APPROVAL_TICKS = 10 * TICKS_PER_SECOND;
	constexpr unsigned char REVIVE_TAIL = 0xA5;        // marks the revive answers after an LVLC map name

	// ---------------------------------------------------------------- what is polled

	// The keys on_key_pressed fires for, named exactly as pollInput names them (sam_lua_runtime.cpp
	// samKeyList). The NAME crosses the wire, not an index, so two builds cannot disagree about
	// which key moved.
	struct KeyDef
	{
		std::string name;
		SDL_Keycode code;
	};
	const std::vector<KeyDef>& keyList()
	{
		static const std::vector<KeyDef> keys = []() {
			std::vector<KeyDef> v;
			for ( char c = 'A'; c <= 'Z'; ++c ) { v.push_back({ std::string(1, c), (SDL_Keycode)(SDLK_a + (c - 'A')) }); }
			for ( char c = '0'; c <= '9'; ++c ) { v.push_back({ std::string(1, c), (SDL_Keycode)(SDLK_0 + (c - '0')) }); }
			for ( int n = 1; n <= 12; ++n ) { v.push_back({ "F" + std::to_string(n), (SDL_Keycode)(SDLK_F1 + (n - 1)) }); }
			return v;
		}();
		return keys;
	}

	// "f" -> "F", "f05" -> "F5": the spellings sam_is_key_held accepts for the reported keys.
	std::string canonicalKey(const std::string& name)
	{
		if ( name.size() == 1 )
		{
			char c = name[0];
			if ( c >= 'a' && c <= 'z' ) { c = (char)(c - 'a' + 'A'); }
			return std::string(1, c);
		}
		if ( name.size() >= 2 && name.size() <= 4 && (name[0] == 'F' || name[0] == 'f') )
		{
			bool digits = true;
			for ( std::size_t i = 1; i < name.size(); ++i )
			{
				if ( name[i] < '0' || name[i] > '9' ) { digits = false; break; }
			}
			if ( digits ) { return "F" + std::to_string(std::atoi(name.c_str() + 1)); }
		}
		return name;
	}

	bool isReportedKey(const std::string& canonical)
	{
		for ( const KeyDef& k : keyList() )
		{
			if ( k.name == canonical ) { return true; }
		}
		return false;
	}

	// The bound actions pollActions watches: the vanilla names the runtime owns, in the order
	// that is also the legacy 'SAMA' wire format, then every action a loaded mod registered
	// (sam_register_action, "<ns>:<id>"), in registration order. The vanilla names stay first
	// so an index an older client sends still means what it meant. Rebuilt when SAMSettings
	// says its registry changed -- a registration, a reload -- and only at the top of a call:
	// every poller takes the reference once per tick, and nothing a fired handler reaches
	// (sam_is_action_held, sam_register_action itself) comes back in here, so the vector a
	// caller is walking is never rebuilt under it.
	const std::vector<std::string>& actionList()
	{
		static std::vector<std::string> v;
		static unsigned builtFor = 0;
		static bool built = false;
		const unsigned gen = SAMSettings::actionGeneration();
		if ( !built || builtFor != gen )
		{
			v.clear();
			for ( int i = 0; i < 64; ++i )
			{
				const char* n = SAMLua::actionNameForIndex(i);
				if ( !n || !n[0] ) { break; }
				v.push_back(n);
			}
			for ( const SAMSettings::Action& a : SAMSettings::actions() ) { v.push_back(a.name); }
			builtFor = gen;
			built = true;
		}
		return v;
	}

	bool isKnownAction(const std::string& name)
	{
		for ( const std::string& a : actionList() )
		{
			if ( a == name ) { return true; }
		}
		return false;
	}

	int categoryOf(int type)
	{
		return ( type >= 0 && type < NUM_ITEM_SLOTS ) ? (int)items[type].category : (int)GEM;
	}

	// ---------------------------------------------------------------- host state

	// What a client last told the host about its player's keys and buttons.
	struct RemoteInput
	{
		std::set<std::string> keys;
		std::set<std::string> actions;
		std::map<std::string, std::string> bindings;
		bool legacy = false;   // fed by 'SAMA' from an older build, which reports edges out of order
	};
	RemoteInput s_remote[MAXPLAYERS];

	// Deliberate uses the host said yes to and whose USEI has not arrived yet. The USEI is a
	// VANILLA packet and carries no uid -- the host builds its own copy of the item from the
	// fields in it -- so an approval cannot name the item by uid and is matched on everything the
	// packet does carry except the count, which can legitimately change between the question and
	// the use (the player picks up another of the same thing). Matching on the type alone let one
	// banked approval be spent on a different item of that type.
	struct Approval
	{
		int type;
		int status;
		int beatitude;
		Uint32 appearance;
		bool identified;
		Uint32 tick;
	};
	std::deque<Approval> s_approved[MAXPLAYERS];

	void expireApprovals(std::deque<Approval>& q)
	{
		while ( !q.empty() && (Uint32)(ticks - q.front().tick) > APPROVAL_TICKS ) { q.pop_front(); }
	}

	// How many of `type` the host's mirror says this player carries; -1 when it cannot see their
	// backpack at all (their game has not reported yet, or does not run S.A.M scripts).
	int mirroredCount(int player, int type)
	{
		if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS ) { return -1; }
		list_t* list = SAMMpInventory::listFor(player);
		if ( !list ) { return -1; }
		int total = 0;
		for ( node_t* node = list->first; node; node = node->next )
		{
			const Item* it = (const Item*)node->element;
			if ( it && (int)it->type == type ) { total += (int)it->count; }
		}
		return total;
	}

	bool s_leftAnnounced[MAXPLAYERS] = {};
	bool s_partyWipeFired = false;

	// Host and singleplayer: this player's death procedure has run and they have not been alive
	// since. See claimPlayerDeath -- a death that a mod refuses to revive reaches actPlayer again
	// on every later floor, and everything in that procedure happens once per death, not per floor.
	bool s_deathAnnounced[MAXPLAYERS] = {};

	// ---------------------------------------------------------------- host and singleplayer state

	// The bound actions of every player whose buttons are on this machine, as last seen
	// (pollLocalActions). Per player: the old poller kept ONE map keyed by action, so a second
	// splitscreen player's press would have looked like the first player's release.
	std::vector<bool> s_localPrev[MAXPLAYERS];

	// ---------------------------------------------------------------- client state

	struct PendingUse
	{
		int count = 0;
		bool hotbarSpellbook = false;
		Uint32 tick = 0;   // when the last question about this item went out
	};

	struct ClientState
	{
		bool hostReady = false;   // the host said READY on this session's channel
		bool primed = false;      // the full STATE has gone out since READY
		std::vector<bool> keyPrev;
		std::vector<bool> actionPrev;
		std::vector<std::string> bindingSent;
		Uint32 bindingCheckTick = 0;
		std::deque<std::pair<std::string, SAMNet::EventFields>> pendingEvents;
		std::map<std::uint32_t, PendingUse> pendingUse;
	};
	ClientState s_client;
	bool s_inGrantedUse = false;

	// ---------------------------------------------------------------- revive state (every machine)

	struct ReviveState
	{
		bool decided = false;
		bool allowed[MAXPLAYERS];
		bool revived[MAXPLAYERS];
	};
	ReviveState s_revive;
	// Host: the players whose floor-load revive a handler refused, bit per player, for the floor
	// the party is on. Unlike s_revive it outlives the load, so the LVLC reminder can carry it.
	unsigned s_floorRefused = 0;

	// A floor load stood a body up for a player a handler refused to revive, and that body has to
	// come down again. The HOST does it on the body's first actPlayer tick (claimPlayerDeath) and
	// gives a player of its own a camera to watch from. A CLIENT did neither: the host's copy of
	// the body is taken down by the entity destructor's ENTD a moment later, so a joiner the mod
	// was keeping dead spent every later floor looking at a frozen frame with no controls and no
	// way to tell what had happened. Recorded on a client only, with where the loader put the body
	// (assignActions runs on the loader thread; the camera is made on the main thread next tick).
	struct KeptDead
	{
		bool pending = false;
		double x = 0.0;
		double y = 0.0;
		double yaw = 0.0;
	};
	KeptDead s_keptDead[MAXPLAYERS];

	void resetRevive()
	{
		s_revive.decided = false;
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			s_revive.allowed[p] = true;   // the vanilla answer
			s_revive.revived[p] = false;
		}
	}
	struct ReviveInit { ReviveInit() { resetRevive(); } };
	ReviveInit s_reviveInit;

	// ---------------------------------------------------------------- helpers

	// A player whose input and backpack live on another machine, as seen from here. Unlike
	// SAMNet::isRemotePlayer this is also true for a slot that has just disconnected, because
	// Input::inputs[] would otherwise answer for such a slot with THIS machine's buttons.
	bool elsewhere(int player)
	{
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return false; }
		if ( multiplayer == SERVER ) { return player != 0 && !players[player]->isLocalPlayer(); }
		if ( multiplayer == CLIENT ) { return player != clientnum; }
		return false;
	}

	std::string playerName(int player)
	{
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return std::string(); }
		return std::string(stats[player]->name, strnlen(stats[player]->name, sizeof(stats[player]->name)));
	}

	// Client: fire `name` on the host about this machine's player. Held back until the host has
	// said READY, so a stock host never receives a frame from us; dropped at the end of the game
	// if it never does.
	void raiseOnHost(const std::string& name, const SAMNet::EventFields& fields)
	{
		if ( multiplayer != CLIENT || !SamEvent::anyScripts() ) { return; }
		if ( s_client.hostReady )
		{
			SAMNet::sendEventToHost(name, fields);
			return;
		}
		if ( s_client.pendingEvents.size() >= MAX_PENDING_EVENTS ) { s_client.pendingEvents.pop_front(); }
		s_client.pendingEvents.emplace_back(name, fields);
	}

	void writeEdge(std::string& out, int kind, bool down, const std::string& name, const std::string& binding)
	{
		SAMNet::Writer w;
		w.u8((std::uint8_t)kind);
		w.u8(down ? 1 : 0);
		w.str8(name);
		w.str8(binding);
		out += w.buf;
	}

	// ---------------------------------------------------------------- client side

	void onReady(const std::string& body)
	{
		(void)body;
		if ( multiplayer != CLIENT ) { return; }
		s_client.hostReady = true;
		s_client.primed = false;   // the next tick sends the full state, then only edges
	}

	void sendState(int p, const std::vector<bool>& keyNow, const std::vector<bool>& actNow)
	{
		const auto& keys = keyList();
		const auto& acts = actionList();
		SAMNet::Writer w;
		int held = 0;
		for ( std::size_t i = 0; i < keys.size(); ++i ) { if ( keyNow[i] ) { ++held; } }
		w.u8((std::uint8_t)held);
		for ( std::size_t i = 0; i < keys.size(); ++i ) { if ( keyNow[i] ) { w.str8(keys[i].name); } }
		w.u8((std::uint8_t)acts.size());
		for ( std::size_t i = 0; i < acts.size(); ++i )
		{
			w.str8(acts[i]);
			w.u8(actNow[i] ? 1 : 0);
			w.str8(i < s_client.bindingSent.size() ? s_client.bindingSent[i] : std::string());
		}
		(void)p;
		SAMNet::sendToHost(OP_STATE, w.buf);
	}

	// Every tick, on a client in a game with scripts. Mirrors pollInput (the same keys, no
	// other gating) and pollActions (this machine's player), but instead of firing anything
	// here it tells the host, in order, so the events fire where the rest of the mod runs.
	void clientTick()
	{
		ClientState& c = s_client;
		if ( !c.hostReady ) { return; }
		const int p = clientnum;
		if ( p < 0 || p >= MAXPLAYERS || !players[p] ) { return; }

		// Whatever this machine raised before the host said it runs S.A.M.
		while ( !c.pendingEvents.empty() )
		{
			SAMNet::sendEventToHost(c.pendingEvents.front().first, c.pendingEvents.front().second);
			c.pendingEvents.pop_front();
		}

		const auto& keys = keyList();
		const auto& acts = actionList();
		std::vector<bool> keyNow(keys.size(), false);
		std::vector<bool> actNow(acts.size(), false);
		// While a text field has focus (the chat box, the console) nothing counts as held. Barony
		// sets keystatus and Input::keys on every key down whether or not a field has focus, so a
		// joiner typing a message used to send the host one frame per keystroke and fire a mod's
		// "F" ability every time anyone typed an f. Everything reads as released rather than the
		// poll being skipped: a key that was down when the box opened still gets its release edge,
		// so nothing is left looking held forever, and the host's picture of this player converges.
		const bool typing = ( SDL_IsTextInputActive() == SDL_TRUE );
		if ( !typing )
		{
			for ( std::size_t i = 0; i < keys.size(); ++i )
			{
				auto it = keystatus.find(keys[i].code);
				keyNow[i] = ( it != keystatus.end() && it->second );
			}
			for ( std::size_t i = 0; i < acts.size(); ++i )
			{
				// binary() is const: it never sets `consumed`, so vanilla's own readers are untouched.
				actNow[i] = Input::inputs[p].binary(acts[i].c_str());
			}
		}

		// Bindings go with the state, and again whenever the player rebinds something. Checked
		// about once a second: reading twelve strings a tick for a menu change is not worth it.
		// Checked NOW when the list itself grew -- a mod registering an action mid-game
		// (sam_register_action from game.on_game_start, or the host's registration arriving
		// here as a forwarded call) -- so the new action's binding goes to the host in this
		// tick's full STATE rather than riding the next second's edges with an empty binding.
		// The vanilla names stay in front, so the entries already sent keep their index.
		const bool listGrew = ( c.primed && c.actionPrev.size() != acts.size() );
		bool bindingsChanged = false;
		if ( !c.primed || listGrew || (Uint32)(ticks - c.bindingCheckTick) >= (Uint32)TICKS_PER_SECOND )
		{
			c.bindingCheckTick = ticks;
			std::vector<std::string> now(acts.size());
			for ( std::size_t i = 0; i < acts.size(); ++i )
			{
				const char* b = Input::inputs[p].binding(acts[i].c_str());
				now[i] = b ? b : "";
			}
			if ( now != c.bindingSent )
			{
				c.bindingSent = std::move(now);
				bindingsChanged = true;
			}
		}

		if ( c.primed && c.keyPrev.size() == keys.size() && c.actionPrev.size() == acts.size() )
		{
			std::string entries;
			int n = 0;
			for ( std::size_t i = 0; i < keys.size() && n < 255; ++i )
			{
				if ( keyNow[i] == c.keyPrev[i] ) { continue; }
				writeEdge(entries, 0, keyNow[i], keys[i].name, std::string());
				++n;
			}
			for ( std::size_t i = 0; i < acts.size() && n < 255; ++i )
			{
				if ( actNow[i] == c.actionPrev[i] ) { continue; }
				writeEdge(entries, 1, actNow[i], acts[i], i < c.bindingSent.size() ? c.bindingSent[i] : std::string());
				++n;
			}
			if ( n > 0 )
			{
				SAMNet::Writer w;
				w.u8((std::uint8_t)n);
				w.buf += entries;
				SAMNet::sendToHost(OP_EDGES, w.buf);
			}
		}
		if ( !c.primed || bindingsChanged )
		{
			// The whole picture, without events: what is held right now and what every action is
			// bound to. First after READY, so a key already down when the host said hello counts.
			sendState(p, keyNow, actNow);
		}
		c.keyPrev = std::move(keyNow);
		c.actionPrev = std::move(actNow);
		c.primed = true;
	}

	// The host's answer to a deliberate Use. On "allow", do what the player asked for, now.
	void onUseVerdict(const std::string& body)
	{
		if ( multiplayer != CLIENT ) { return; }
		SAMNet::Reader r(body);
		const std::uint32_t uid = r.u32();
		const bool allow = r.u8() != 0;
		if ( !r.ok ) { return; }
		auto it = s_client.pendingUse.find(uid);
		if ( it == s_client.pendingUse.end() ) { return; }   // not a question this game asked
		const bool hotbarSpellbook = it->second.hotbarSpellbook;
		if ( --it->second.count <= 0 ) { s_client.pendingUse.erase(it); }
		if ( !allow ) { return; }   // a handler on the host said no: nothing was used, nothing is lost

		const int p = clientnum;
		if ( intro || p < 0 || p >= MAXPLAYERS || !players[p] || !players[p]->entity || !stats[p] ) { return; }
		Item* item = uidToItem(uid);
		if ( !item ) { return; }   // dropped, sold or used up while the host was answering

		// The hotbar marks a spellbook it is reading so addSpell can put the new spell in that slot.
		// The mark was cleared when the original call returned early; put it back for this one.
		if ( hotbarSpellbook ) { players[p]->magic.spellbookUidFromHotbarSlot = uid; }
		struct Granted
		{
			Granted() { s_inGrantedUse = true; }
			~Granted() { s_inGrantedUse = false; }
		} granted;
		useItem(item, p, nullptr, false, false, /*samDeliberateUse=*/true, /*samPlayerEquip=*/false);
		if ( hotbarSpellbook ) { players[p]->magic.spellbookUidFromHotbarSlot = 0; }
	}

	// A handler on the host refused this player's ping after this machine had drawn it.
	void onUnping(const std::string& body)
	{
		if ( multiplayer != CLIENT ) { return; }
		SAMNet::Reader r(body);
		const std::uint32_t key = r.u32();
		if ( !r.ok ) { return; }
		const int p = clientnum;
		if ( p < 0 || p >= MAXPLAYERS ) { return; }
		auto& callouts = CalloutMenu[p].callouts;
		auto it = callouts.find(key);
		// Exactly this key or nothing. Both machines swap the entity the same way before they key
		// the marker -- a "thanks" or an affirmative names the pinger's own body on the host
		// (net.cpp) and here (CalloutRadialMenu, the identical arms) -- so a miss means the marker
		// is already gone, not that it is filed under another name. Guessing at "the newest ping in
		// the last few seconds" could take down a SECOND ping the host had just allowed, which is
		// worse than leaving a vetoed one up: a stock client keeps its own vetoed marker anyway,
		// so that is already the documented state.
		if ( it != callouts.end() ) { callouts.erase(it); }
	}

	// A client's twin of fireFloorRevives, which game.cpp calls inside `if ( multiplayer != CLIENT )`
	// and so never runs here. The host's answers arrive on the level-change packet and are consumed
	// by assignActions inside the SAME packet handler -- changeLevel reads them, sets `loading`,
	// runs assignActions and clears `loading` with no game tick in between -- so by the first tick
	// after a load they have done their work. Forget them, because nothing else on a client does:
	// they used to sit here until the next level change, and doNewGame's own level loads have no
	// packet in front of them, so the first floor of the NEXT run applied the last answer of this
	// one and a player kept dead in one run did not stand up in the next.
	void forgetReviveVerdictsOnClient()
	{
		if ( multiplayer != CLIENT || loading || intro ) { return; }
		if ( s_revive.decided ) { resetRevive(); }
	}

	// A client, on the first tick after a floor load, for each of its own players the host is
	// keeping dead. Gives them the same camera the host makes for a kept-dead player of its own,
	// so a dead joiner watches the new floor instead of a frozen frame. This is NOT a death: it
	// all happened on the floor they died on, so nothing is announced, nothing is dropped and no
	// event fires here. The game-over window is suppressed as well -- they answered it when they
	// died, and re-asking on every floor the party descends is not an answer they owe.
	void keptDeadTick()
	{
		if ( multiplayer != CLIENT || loading || intro ) { return; }
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( !s_keptDead[p].pending ) { continue; }
			const KeptDead where = s_keptDead[p];
			s_keptDead[p] = KeptDead();
			if ( !players[p] || !players[p]->isLocalPlayer() || !stats[p] || stats[p]->HP > 0 ) { continue; }
			if ( players[p]->ghost.isActive() ) { continue; }   // already flying: they have a view
			bool watching = false;
			for ( node_t* node = map.entities->first; node && !watching; node = node->next )
			{
				const Entity* e = (const Entity*)node->element;
				// skill[2] is DEATHCAM_PLAYERNUM (the macro is private to actplayer.cpp).
				if ( e && e->behavior == &actDeathCam && e->skill[2] == p ) { watching = true; }
			}
			if ( watching ) { continue; }
			Entity* cam = newEntity(-1, 1, map.entities, nullptr);
			if ( !cam ) { continue; }
			cam->x = (real_t)where.x;
			cam->y = (real_t)where.y;
			cam->z = -2;
			cam->flags[NOUPDATE] = true;    // this machine's own camera; the host knows nothing of it
			cam->flags[PASSABLE] = true;
			cam->flags[INVISIBLE] = true;
			cam->behavior = &actDeathCam;
			cam->skill[2] = p;              // DEATHCAM_PLAYERNUM
			cam->skill[6] = 1;              // DEATHCAM_DISABLE_GAMEOVER
			cam->yaw = (real_t)where.yaw;
			cam->pitch = PI / 8;
			players[p]->ghost.initTeleportLocations((int)(where.x / 16), (int)(where.y / 16));
			// The same two lines the host's kept-dead branch ends with (actplayer.cpp), and the
			// same two the client's own UDIE handler ends with (net.cpp): hand the camera a
			// player who is in shootmode with no GUI over it and no controls, and let actDeathCam
			// give the controls back on its own schedule. Without them a kept-dead joiner arrived
			// on the new floor still holding whatever bControlEnabled and shootmode the PREVIOUS
			// floor's deathcam had restored, so the camera would not turn with the mouse.
			players[p]->closeAllGUIs(CloseGUIShootmode::CLOSEGUI_ENABLE_SHOOTMODE, CloseGUIIgnore::CLOSEGUI_CLOSE_ALL);
			players[p]->bControlEnabled = false;
		}
	}

	// ---------------------------------------------------------------- host side

	// A joiner's action or key the host has no use for is dropped, and the drop is written
	// down once per name. The joiner polls from ITS OWN registry, so a mod it runs and the
	// host does not (or one the host registers later, from an event) sends names that fail
	// isKnownAction here; that is a supported situation, but a press that vanishes with no
	// line in either log is not, since the modder's first question is "why does the host not
	// hear my key". A key outside the reported set never comes from a S.A.M client, so that
	// line points at a version mismatch or a hand-made packet.
	void noteDroppedAction(int from, const std::string& name)
	{
		SAMNet::warnOnce("input:action:" + name, "Player " + std::to_string(from) + " sent the action '" + name
			+ "', which no mod loaded on this machine registered; it is dropped. A joiner's mod action reaches scripts only while the host runs a mod that registers the same name.");
	}

	void noteDroppedKey(int from, const std::string& name)
	{
		SAMNet::warnOnce("input:sentkey:" + name, "Player " + std::to_string(from) + " sent the key '" + name
			+ "', which is not one a client reports (A-Z, 0-9, F1-F12); it is dropped.");
	}

	void onEdges(int from, const std::string& body)
	{
		if ( from <= 0 || from >= MAXPLAYERS ) { return; }
		SAMNet::Reader r(body);
		const int n = r.u8();
		for ( int i = 0; i < n && r.ok; ++i )
		{
			const int kind = r.u8();
			const bool down = r.u8() != 0;
			const std::string name = r.str8();
			const std::string binding = r.str8();
			if ( !r.ok ) { break; }
			RemoteInput& row = s_remote[from];
			if ( kind == 0 )
			{
				if ( !isReportedKey(name) ) { noteDroppedKey(from, name); continue; }
				if ( down ) { row.keys.insert(name); } else { row.keys.erase(name); }
				// The same event, fields and order pollInput fires for the host's own keyboard.
				SamEvent ev(down ? "on_key_pressed" : "on_key_released");
				ev.i("player", (long long)from).s("key_name", name);
				if ( down ) { ev.i("held", 0); }
				ev.fire();
			}
			else if ( kind == 1 )
			{
				if ( !isKnownAction(name) ) { noteDroppedAction(from, name); continue; }
				if ( down ) { row.actions.insert(name); } else { row.actions.erase(name); }
				row.bindings[name] = binding;
				// dispatchAction's event, with the binding the PLAYER has -- the host's own
				// Input::binding would answer with the host's keys.
				SamEvent ev(down ? "on_action_pressed" : "on_action_released");
				ev.i("player", (long long)from).s("action", name).s("binding", binding);
				ev.fire();
			}
		}
	}

	void onState(int from, const std::string& body)
	{
		if ( from <= 0 || from >= MAXPLAYERS ) { return; }
		SAMNet::Reader r(body);
		RemoteInput fresh;
		const int nKeys = r.u8();
		for ( int i = 0; i < nKeys && r.ok; ++i )
		{
			const std::string k = r.str8();
			if ( !r.ok ) { break; }
			if ( !isReportedKey(k) ) { noteDroppedKey(from, k); continue; }
			fresh.keys.insert(k);
		}
		const int nActs = r.u8();
		for ( int i = 0; i < nActs && r.ok; ++i )
		{
			const std::string a = r.str8();
			const bool held = r.u8() != 0;
			const std::string b = r.str8();
			if ( !r.ok ) { break; }
			if ( !isKnownAction(a) ) { noteDroppedAction(from, a); continue; }
			if ( held ) { fresh.actions.insert(a); }
			fresh.bindings[a] = b;
		}
		if ( !r.ok ) { return; }

		// A snapshot fires the same events the matching edges would. It used to be applied
		// silently, and the first snapshot after READY is a joiner's WHOLE picture: someone still
		// holding Attack or W as the floor finished loading read as held from that moment on but
		// produced no on_action_pressed, and their eventual release fired alone. Anything that
		// pairs a press with a release -- a charge ability, hold-to-aim, "how long did they hold
		// it" -- mis-stated that first hold, once per joiner per run. Diffing is free in the
		// ordinary case: a snapshot sent because a BINDING changed rides behind that same tick's
		// edges on the ordered channel, so by the time it arrives it agrees with the picture and
		// says nothing.
		RemoteInput& row = s_remote[from];
		std::vector<std::string> keyUp, keyDown, actUp, actDown;
		for ( const std::string& k : row.keys ) { if ( !fresh.keys.count(k) ) { keyUp.push_back(k); } }
		for ( const std::string& k : fresh.keys ) { if ( !row.keys.count(k) ) { keyDown.push_back(k); } }
		for ( const std::string& a : row.actions ) { if ( !fresh.actions.count(a) ) { actUp.push_back(a); } }
		for ( const std::string& a : fresh.actions ) { if ( !row.actions.count(a) ) { actDown.push_back(a); } }
		// The picture is replaced BEFORE anything fires, so a handler asking sam_is_key_held about
		// this player during its own press event gets the new answer, exactly as it does for an edge.
		s_remote[from] = std::move(fresh);

		const auto bindingOf = [from](const std::string& action) -> std::string {
			const auto& b = s_remote[from].bindings;
			auto it = b.find(action);
			return ( it == b.end() ) ? std::string() : it->second;
		};
		for ( const std::string& k : keyUp )
		{
			SamEvent ev("on_key_released");
			ev.i("player", (long long)from).s("key_name", k);
			ev.fire();
		}
		for ( const std::string& a : actUp )
		{
			SamEvent ev("on_action_released");
			ev.i("player", (long long)from).s("action", a).s("binding", bindingOf(a));
			ev.fire();
		}
		for ( const std::string& k : keyDown )
		{
			SamEvent ev("on_key_pressed");
			ev.i("player", (long long)from).s("key_name", k).i("held", 0);
			ev.fire();
		}
		for ( const std::string& a : actDown )
		{
			SamEvent ev("on_action_pressed");
			ev.i("player", (long long)from).s("action", a).s("binding", bindingOf(a));
			ev.fire();
		}
	}

	// A joiner's deliberate Use, asked BEFORE their machine does anything with the item.
	void onUseRequest(int from, const std::string& body)
	{
		if ( from <= 0 || from >= MAXPLAYERS ) { return; }
		SAMNet::Reader r(body);
		const std::uint32_t uid = r.u32();
		const int type = (int)(std::int32_t)r.u32();
		const int status = (int)(std::int32_t)r.u32();    // not part of the event; names the item
		const int beatitude = (int)(std::int32_t)r.u32(); // same
		const int count = (int)(std::int32_t)r.u32();
		const std::uint32_t appearance = r.u32();
		const bool identified = r.u8() != 0;
		if ( !r.ok ) { return; }

		// What the sender says it is about to use is taken on trust, exactly as vanilla takes the
		// USEI itself -- but an APPROVAL is banked here, and a bank is worth more to a modified
		// client than a single packet: 32 questions about one potion while a mod's cooldown was
		// open bought 32 later uses the mod was never asked about again, so "the host decides"
		// became "the host decided once, 32 times over". The bank is now as deep as the stack the
		// host's mirror says that player carries, which is the same rule their own machine applies
		// to itself (one question per item in the stack) -- and never shallower than one, so a
		// mirror that has not caught up with a pickup yet, or an item type the host's own table
		// does not name, still lets the use through rather than swallowing the click.
		auto& bank = s_approved[from];
		expireApprovals(bank);
		int outstanding = 0;
		for ( const Approval& a : bank ) { if ( a.type == type ) { ++outstanding; } }
		const int held = mirroredCount(from, type);     // -1: their backpack is not mirrored here
		const int depth = ( held > 1 ) ? held : 1;

		bool allow = false;
		if ( outstanding < depth && type >= 0 && type < NUM_ITEM_SLOTS && !client_disconnected[from] )
		{
			// Exactly the event useItem fires for the host's own player, before anything happens.
			SamEvent ev("player.on_item_use");
			ev.i("player", (long long)from)
				.i("item_type", (long long)type)
				.i("item_count", (long long)count)
				.i("category", (long long)categoryOf(type))
				.i("deliberate_use", 1);
			allow = ev.fire();
		}
		if ( allow )
		{
			if ( bank.size() >= MAX_APPROVALS ) { bank.pop_front(); }
			bank.push_back(Approval{ type, status, beatitude, appearance, identified, ticks });
		}
		SAMNet::Writer w;
		w.u32(uid);
		w.u8(allow ? 1 : 0);
		SAMNet::sendToClient(from, OP_VERDICT, w.buf);
	}

	void onHello(int player)
	{
		if ( player <= 0 || player >= MAXPLAYERS ) { return; }
		// The picture of that player's keys is deliberately NOT wiped here. A HELLO also arrives
		// when a broken channel is repaired mid-game, and the snapshot that follows READY now
		// fires the events for whatever it disagrees with: wiping first would make everything
		// that player is still holding look newly pressed. A slot that changed hands is already
		// covered -- hostTick empties a disconnected slot every tick, and onClear empties every
		// slot at a run boundary -- so what is left here is always about the same person.
		s_approved[player].clear();
		SAMNet::Writer w;
		w.u16(READY_VERSION);
		SAMNet::sendToClient(player, OP_READY, w.buf);
	}

	void hostTick()
	{
		for ( int p = 1; p < MAXPLAYERS; ++p )
		{
			if ( client_disconnected[p] )
			{
				// Nothing a departed player held is still held.
				if ( !s_remote[p].keys.empty() || !s_remote[p].actions.empty() || !s_remote[p].bindings.empty() || s_remote[p].legacy )
				{
					s_remote[p] = RemoteInput();
				}
				s_approved[p].clear();
			}
			else if ( s_leftAnnounced[p] )
			{
				s_leftAnnounced[p] = false;   // someone is in this slot again: their leave counts
			}
		}
	}

	// ---------------------------------------------------------------- hooks

	void onTick()
	{
		if ( multiplayer == CLIENT ) { keptDeadTick(); forgetReviveVerdictsOnClient(); clientTick(); }
		else if ( multiplayer == SERVER ) { hostTick(); }
	}

	// A game ended, a new run started, or mods were reloaded. This runs at EVERY run boundary now
	// (SAMNet::clear from doNewGame), which is what drops the keys and buttons the host last heard
	// about: they are per slot, and the next run puts different people in those slots. A key held
	// as one run ended used to read as held for the whole of the next one, and its release edge --
	// fired by a machine that never sent the matching press -- arrived unpaired.
	void onClear()
	{
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			s_remote[p] = RemoteInput();
			s_approved[p].clear();
			s_leftAnnounced[p] = false;
			s_localPrev[p].clear();
			s_deathAnnounced[p] = false;
			s_keptDead[p] = KeptDead();
		}
		s_client = ClientState();
		s_inGrantedUse = false;
		s_floorRefused = 0;
	}

	struct Registrar
	{
		Registrar()
		{
			SAMNet::onHostOp(OP_EDGES, &onEdges);
			SAMNet::onHostOp(OP_STATE, &onState);
			SAMNet::onHostOp(OP_USE, &onUseRequest);
			SAMNet::onClientOp(OP_READY, &onReady);
			SAMNet::onClientOp(OP_VERDICT, &onUseVerdict);
			SAMNet::onClientOp(OP_UNPING, &onUnping);
		}
	};
	Registrar s_registrar;

	SAMNet::TickHook s_tickHook(&onTick);
	SAMNet::ClearHook s_clearHook(&onClear);
	SAMNet::HelloHook s_helloHook(&onHello);

	// The events a client may ask the host to fire about its own player.
	SAMNet::AllowEvent s_allowIdentified("player.on_item_identified");
	SAMNet::AllowEvent s_allowLearned("player.on_spell_learned");
	SAMNet::AllowEvent s_allowFailed("player.on_spell_failed");
}

// -------------------------------------------------------------------- keys and actions

bool onOtherMachine(int player)
{
	return elsewhere(player);
}

bool keyHeld(const std::string& name, int player)
{
	if ( player < 0 ) { player = SAMNet::defaultScreenPlayer(); }
	if ( player < 0 || player >= MAXPLAYERS ) { return false; }
	if ( multiplayer == SERVER && elsewhere(player) )
	{
		if ( client_disconnected[player] ) { return false; }   // nobody is in that slot to hold anything
		const std::string key = canonicalKey(name);
		if ( !isReportedKey(key) )
		{
			SAMNet::warnOnce("input:key:" + key, "sam_is_key_held: a player on another machine reports only A-Z, 0-9 and F1-F12; '"
				+ name + "' reads false for them.");
			return false;
		}
		// peerMayHaveSam, not peerHasSam: a joiner cannot say HELLO until it is in a game, so for
		// the first seconds of a floor a S.A.M client looks exactly like a stock one. Answering
		// false is right either way -- their keys have not reached us yet -- but SAYING they do
		// not run S.A.M is a permanent, untrue line in the log (warnOnce never repeats), and it
		// sends whoever reads the report hunting a peer problem that does not exist. Quiet until
		// the grace window closes; then the statement is true. The trampoline draws the same line
		// for the same question when it forwards a call.
		if ( !SAMNet::peerMayHaveSam(player) )
		{
			SAMNet::warnOnce("input:nosam:" + std::to_string(player), "Player " + std::to_string(player)
				+ "'s game does not run S.A.M scripts, so their keys and buttons never reach the host; they read as not held.");
			return false;
		}
		return s_remote[player].keys.count(key) > 0;
	}
	if ( multiplayer == CLIENT && player != clientnum ) { return false; }   // the contract refused this already
	return SAMLua::isKeyHeld(name);   // this machine's keyboard
}

bool remoteActionHeld(int player, const std::string& action)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS ) { return false; }
	if ( client_disconnected[player] ) { return false; }   // an empty slot holds nothing, and says nothing
	const RemoteInput& row = s_remote[player];
	// peerMayHaveSam: see keyHeld. Silent while that joiner still might say HELLO.
	if ( !row.legacy && !SAMNet::peerMayHaveSam(player) )
	{
		SAMNet::warnOnce("input:nosam:" + std::to_string(player), "Player " + std::to_string(player)
			+ "'s game does not run S.A.M scripts, so their keys and buttons never reach the host; they read as not held.");
		return false;
	}
	return row.actions.count(action) > 0;
}

const char* remoteActionBinding(int player, const std::string& action)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS ) { return ""; }
	const auto& b = s_remote[player].bindings;
	auto it = b.find(action);
	return ( it == b.end() ) ? "" : it->second.c_str();
}

int keyboardPlayer()
{
	if ( multiplayer == SINGLE )
	{
		const int p = inputs.getPlayerIDAllowedKeyboard();
		return ( p >= 0 && p < MAXPLAYERS ) ? p : 0;
	}
	return clientnum;
}

void pollLocalActions()
{
	// A client's buttons go to the host from clientTick, in order with its keys, and fire
	// there. Nothing fires on the client and nothing is sent from here.
	if ( multiplayer == CLIENT || !SamEvent::anyScripts() ) { return; }
	const auto& acts = actionList();
	// Nothing is held while a text field has focus -- the same rule a joiner's machine applies to
	// itself in clientTick, so a bound action cannot fire for one player's typing and not another's.
	// In splitscreen this quietens every seat while one of them types: the engine's text focus is
	// per machine, not per seat, and the alternative is a seat firing abilities as someone chats.
	const bool typing = ( SDL_IsTextInputActive() == SDL_TRUE );
	for ( int p = 0; p < MAXPLAYERS; ++p )
	{
		std::vector<bool>& prev = s_localPrev[p];
		// Every player whose buttons are on THIS machine: the host's own player, and in
		// splitscreen every seat. Input::inputs[p] for anyone else answers with this machine's
		// buttons in a netgame, so a remote slot must never be polled here.
		if ( !players[p] || !players[p]->isLocalPlayer() || client_disconnected[p] )
		{
			prev.clear();
			continue;
		}
		// resize, not assign: a mod registering an action mid-game (game.on_game_start is the
		// usual place) appends to the list, and the vanilla entries in front keep their index. A
		// re-prime of the whole vector here would read every action the player was holding at
		// that moment as newly pressed.
		if ( prev.size() != acts.size() ) { prev.resize(acts.size(), false); }
		for ( std::size_t i = 0; i < acts.size(); ++i )
		{
			// binary() is const: it reads binding_t::binary and never sets `consumed`, which is
			// the only thing that could starve vanilla's own readers (blocking, attacking).
			const bool down = !typing && Input::inputs[p].binary(acts[i].c_str());
			if ( down == prev[i] ) { continue; }
			prev[i] = down;
			// This seat's own binding: every local player's Input is its own, so a splitscreen
			// player's prompt names THEIR controller button, not the keyboard's key.
			const char* b = Input::inputs[p].binding(acts[i].c_str());
			SamEvent ev(down ? "on_action_pressed" : "on_action_released");
			ev.i("player", (long long)p).s("action", acts[i]).s("binding", std::string(b ? b : ""));
			ev.fire();
		}
	}
}

void onLegacyAction(int player, int actionIndex, bool pressed)
{
	if ( multiplayer != SERVER ) { return; }
	// Slot 0 is the host itself; nobody else may speak for it.
	if ( player <= 0 || player >= MAXPLAYERS || client_disconnected[player] ) { return; }
	// A client on this build reports its buttons in order on the SAMNet channel and never sends
	// SAMA; a SAMA from a client that said HELLO would be a second copy of an edge.
	if ( SAMNet::peerHasSam(player) ) { return; }
	const char* name = SAMLua::actionNameForIndex(actionIndex);
	if ( !name || !name[0] ) { return; }
	// Best effort: SAMA is reliable but NOT ordered, so a quick tap whose press was resent can
	// arrive release-first and leave the button looking held until its next edge.
	RemoteInput& row = s_remote[player];
	row.legacy = true;
	if ( pressed ) { row.actions.insert(name); } else { row.actions.erase(name); }
	SAMLua::dispatchAction(player, actionIndex, pressed);
}

// -------------------------------------------------------------------- messages

void scriptMessage(int player, const std::string& text)
{
	if ( player < 0 || player >= MAXPLAYERS ) { return; }
	// Only a player on another machine: the host shows its own players' lines itself and never
	// reads them back. Unconditionally for them, because the joiner compares against ITS OWN
	// language file, which the host cannot see.
	if ( multiplayer == SERVER && player != 0 && players[player] && !players[player]->isLocalPlayer() )
	{
		const std::string line = " " + text;
		messagePlayer(player, MESSAGE_MISC, "%s", line.c_str());
		return;
	}
	messagePlayer(player, MESSAGE_MISC, "%s", text.c_str());
}

// -------------------------------------------------------------------- client-raised events

void reportItemIdentified(int player, const Item* item)
{
	if ( !item || player < 0 || player >= MAXPLAYERS || !players[player] ) { return; }
	if ( !SamEvent::anyScripts() ) { return; }
	// The CANONICAL name, because item_name is the field a script feeds back into sam_grant_item
	// and friends. The human string stays, under a name that says so.
	const std::string internalName = ( item->type >= 0 && item->type < NUMITEMS ) ? std::string(itemNameStrings[item->type + 2]) : std::string();
	const std::string displayName = ( item->type >= 0 && item->type < NUM_ITEM_SLOTS ) ? std::string(items[item->type].getIdentifiedName()) : std::string();

	if ( multiplayer != CLIENT )
	{
		// A player on another machine identifies on their own machine, which reports it (below).
		// The host only ever sees its equipped COPY of their gear reveal a curse, and counting that
		// as well would fire one identification twice. A stock client reports nothing, so the copy
		// is still the only evidence for them.
		//
		// peerMayHaveSam, not peerHasSam: a joiner holds its reports until the host says READY, and
		// READY needs HELLO, which cannot arrive until that machine is in a game. With peerHasSam,
		// a curse revealed on the host's copy in those first seconds counted here AND again when the
		// joiner's own held report was flushed -- one identification, two events. While a peer may
		// still turn out to run S.A.M, let their own report be the single source.
		if ( !players[player]->isLocalPlayer() && SAMNet::peerMayHaveSam(player) ) { return; }
		SamEvent ev("player.on_item_identified");
		ev.i("player", (long long)player)
			.i("item_type", (long long)item->type)
			.s("item_name", internalName)
			.s("item_display_name", displayName);
		ev.fire();
		return;
	}
	if ( !players[player]->isLocalPlayer() ) { return; }
	SAMNet::EventFields f;
	f.addInt("item_type", (long long)item->type);
	f.addStr("item_name", internalName);
	f.addStr("item_display_name", displayName);
	raiseOnHost("player.on_item_identified", f);
}

void reportSpellLearned(int player, int spellId, const char* spellName)
{
	if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return; }
	if ( !SamEvent::anyScripts() ) { return; }
	const std::string name = spellName ? spellName : "";
	if ( multiplayer != CLIENT )
	{
		SamEvent ev("player.on_spell_learned");
		ev.i("player", (long long)player).i("spell_id", (long long)spellId).s("spell_name", name);
		ev.fire();
		return;
	}
	if ( !players[player]->isLocalPlayer() ) { return; }
	SAMNet::EventFields f;
	f.addInt("spell_id", (long long)spellId);   // 32 bits of it: a custom spell's id is 2000 and up
	f.addStr("spell_name", name);
	raiseOnHost("player.on_spell_learned", f);
}

void reportSpellFailed(int player, int spellId, const char* spellName, const char* reason)
{
	if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return; }
	if ( !SamEvent::anyScripts() ) { return; }
	const std::string name = spellName ? spellName : "";
	const std::string why = reason ? reason : "";
	if ( multiplayer != CLIENT )
	{
		SamEvent ev("player.on_spell_failed");
		ev.i("player", (long long)player).i("spell_id", (long long)spellId).s("spell_name", name).s("reason", why);
		ev.fire();
		return;
	}
	if ( !players[player]->isLocalPlayer() ) { return; }
	SAMNet::EventFields f;
	f.addInt("spell_id", (long long)spellId);
	f.addStr("spell_name", name);
	f.addStr("reason", why);
	raiseOnHost("player.on_spell_failed", f);
}

// -------------------------------------------------------------------- Use and equip

bool askHostBeforeUse(Item* item, int player)
{
	if ( multiplayer != CLIENT || s_inGrantedUse || !s_client.hostReady || !item ) { return false; }
	if ( player != clientnum || player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->isLocalPlayer() ) { return false; }
	if ( intro || item->uid == 0 || !SamEvent::anyScripts() ) { return false; }
	if ( s_client.pendingUse.size() >= MAX_PENDING_USES && !s_client.pendingUse.count(item->uid) ) { return false; }

	// One question per item in the stack, never more. Nothing is consumed until the host answers,
	// so the item stays in the list and in the hotbar and a second click inside the round trip
	// used to ask a second time -- the host fired player.on_item_use twice for one potion, and a
	// mod that counts uses or charges a cost double-charged joiners and never the host. A STACK
	// shares one uid, though, so drinking two out of a stack of five must still ask twice: the
	// count of questions in flight is capped by the stack, not at one. True, not false: the
	// caller must still do nothing here, because the answer to the first question is coming.
	{
		auto it = s_client.pendingUse.find((std::uint32_t)item->uid);
		if ( it != s_client.pendingUse.end() && (Uint32)(ticks - it->second.tick) > APPROVAL_TICKS )
		{
			// No answer in ten seconds: the question or the answer was lost (a channel that had to
			// be started over drops what was in flight). Forget it and ask again, rather than
			// leaving the item unusable for the rest of the run.
			s_client.pendingUse.erase(it);
			it = s_client.pendingUse.end();
		}
		const int asked = ( it == s_client.pendingUse.end() ) ? 0 : it->second.count;
		const int stack = ( item->count > 1 ) ? (int)item->count : 1;
		if ( asked >= stack ) { return true; }
	}

	SAMNet::Writer w;
	w.u32((std::uint32_t)item->uid);
	w.u32((std::uint32_t)(std::int32_t)item->type);
	w.u32((std::uint32_t)(std::int32_t)item->status);
	w.u32((std::uint32_t)(std::int32_t)item->beatitude);
	w.u32((std::uint32_t)(std::int32_t)item->count);
	w.u32((std::uint32_t)item->appearance);
	w.u8(item->identified ? 1 : 0);
	if ( !SAMNet::sendToHost(OP_USE, w.buf) ) { return false; }

	PendingUse& pu = s_client.pendingUse[(std::uint32_t)item->uid];
	++pu.count;
	pu.tick = ticks;
	pu.hotbarSpellbook = ( players[player]->magic.spellbookUidFromHotbarSlot == item->uid );
	return true;
}

bool takeApprovedUse(int player, const Item* item)
{
	if ( multiplayer != SERVER || !item || player <= 0 || player >= MAXPLAYERS ) { return false; }
	auto& q = s_approved[player];
	expireApprovals(q);
	for ( auto it = q.begin(); it != q.end(); ++it )
	{
		// Everything the USEI carries except the count: see the Approval comment.
		if ( it->type == (int)item->type
			&& it->status == (int)item->status
			&& it->beatitude == (int)item->beatitude
			&& it->appearance == (Uint32)item->appearance
			&& it->identified == ( item->identified != 0 ) )
		{
			q.erase(it);
			return true;
		}
	}
	return false;
}

void dropVetoedUseCopy(int player, Item* item)
{
	if ( multiplayer != SERVER || !item || player <= 0 || player >= MAXPLAYERS || !stats[player] || !players[player] ) { return; }
	if ( players[player]->isLocalPlayer() ) { return; }   // a local player's REAL item: a veto keeps it
	Stat* st = stats[player];
	if ( !item->node || item->node->list != &st->inventory ) { return; }
	// By pointer, not itemIsEquipped: that compares by VALUE, and this copy has the same values as
	// the host's equipped copy of the same item. The veto returned before any equip, so the copy is
	// in no slot -- but a pointer that somehow is must never be freed out from under the slot.
	if ( st->helmet == item || st->breastplate == item || st->gloves == item || st->shoes == item
		|| st->shield == item || st->weapon == item || st->cloak == item || st->amulet == item
		|| st->ring == item || st->mask == item )
	{
		return;
	}
	list_RemoveNode(item->node);
}

bool playerMayEquip(int player, const Item* item)
{
	if ( !item || player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->isLocalPlayer() ) { return true; }
	if ( !SamEvent::anyScripts() ) { return true; }
	SamEvent ev("player.on_before_equip");
	ev.i("player", (long long)player)
		.i("item_type", (long long)item->type)
		.i("category", (long long)categoryOf((int)item->type));
	// The refusal says nothing on its own. It used to print one hard-coded English sentence,
	// which is the only line in the game that does not go through Language::get, and there is no
	// vanilla string that means "you cannot equip that" to borrow. The handler is the one that
	// knows WHY, and it can say so on either machine -- this runs in the joiner's own game, where
	// sam_message and sam_play_sound are host-kind and refused, but sam_hud_text shows to the
	// player whose handler it is. That is in the doc entry for the event.
	return ev.fire();
}

// -------------------------------------------------------------------- revive

void decideFloorRevives()
{
	resetRevive();
	if ( multiplayer == CLIENT ) { return; }
	s_revive.decided = true;
	s_floorRefused = 0;
	if ( !SamEvent::anyScripts() ) { return; }   // nobody to ask: everyone is allowed, as in vanilla
	for ( int p = 0; p < MAXPLAYERS; ++p )
	{
		if ( client_disconnected[p] || !stats[p] || stats[p]->HP > 0 ) { continue; }
		// Here, on the main thread, once, where the answer can still travel with the level
		// change. It used to fire inside assignActions on the loader thread of EVERY machine,
		// and each machine applied its own answer to its own copy of the player.
		SamEvent ev("player.on_before_revive");
		ev.i("player", (long long)p)
			.i("floor", (long long)currentlevel)
			.i("keep_gear", keepInventoryGlobal ? 1 : 0);
		s_revive.allowed[p] = ev.fire();
		// A refusal only means anything on a machine that reads the verdict. A stock 5.0.2 client
		// ignores the tail, revives itself with its gear, and then the host's ENTD deletes the
		// body it just made: that player is dead with no death cam, no prompt and no message,
		// while everyone else sees them alive for a moment. Better to let them live than to
		// half-kill them, so a refusal that cannot be delivered is not made.
		if ( !s_revive.allowed[p] && SAMNet::isRemotePlayer(p) && !SAMNet::peerMayHaveSam(p) )
		{
			SAMNet::warnOnce("revive:stock:" + std::to_string(p),
				"A script refused to revive player " + std::to_string(p) + ", but their game is not"
				" running S.A.M and cannot be told, so they were revived as usual. player.on_before_revive"
				" can only refuse a player whose machine has the framework.");
			s_revive.allowed[p] = true;
		}
		if ( !s_revive.allowed[p] && p < 8 ) { s_floorRefused |= (1u << p); }
	}
}

int appendReviveVerdict(unsigned char* data, int len)
{
	if ( multiplayer != SERVER || !data ) { return len; }
	const unsigned mask = s_floorRefused;
	if ( mask == 0 ) { return len; }   // nothing refused: the packet stays byte-identical to vanilla
	if ( len < 15 || len + 2 > NET_PACKET_SIZE || data[len - 1] != 0 ) { return len; }
	data[len] = REVIVE_TAIL;
	data[len + 1] = (unsigned char)mask;
	return len + 2;
}

void readReviveVerdict(const unsigned char* data, int len)
{
	resetRevive();   // everyone is allowed unless the host says otherwise (a stock host never does)
	if ( multiplayer != CLIENT || !data || len <= 15 ) { return; }
	// The map name (possibly empty) starts at 14 and ends at its NUL; the answers follow it.
	int end = -1;
	for ( int i = 14; i < len && i < NET_PACKET_SIZE; ++i )
	{
		if ( data[i] == 0 ) { end = i; break; }
	}
	if ( end < 0 || end + 2 >= len ) { return; }
	if ( data[end + 1] != REVIVE_TAIL ) { return; }
	const unsigned mask = data[end + 2];
	for ( int p = 0; p < MAXPLAYERS && p < 8; ++p )
	{
		s_revive.allowed[p] = ( (mask & (1u << p)) == 0 );
	}
	s_revive.decided = true;
}

bool reviveAllowed(int player)
{
	if ( player < 0 || player >= MAXPLAYERS ) { return true; }
	// Nobody was asked for THIS load, so the answer is vanilla's: everyone stands up. doNewGame's
	// own level loads call assignActions with no decideFloorRevives and no LVLC in front of them
	// (the first floor of a run is never asked -- there is no level-change packet yet to carry an
	// answer on), and without this they read whatever the last floor of the PREVIOUS run left
	// here: a co-op save holding a dead player could revive on one machine and not on another.
	if ( !s_revive.decided ) { return true; }
	return s_revive.allowed[player];
}

void noteKeptDead(int player, double x, double y, double yaw)
{
	// Only a client records this: the host takes the body down from actPlayer instead, which is
	// a tick sooner and works for its splitscreen seats too.
	if ( multiplayer != CLIENT || player < 0 || player >= MAXPLAYERS ) { return; }
	s_keptDead[player].pending = true;
	s_keptDead[player].x = x;
	s_keptDead[player].y = y;
	s_keptDead[player].yaw = yaw;
}

void noteFloorRevived(int player)
{
	if ( player < 0 || player >= MAXPLAYERS ) { return; }
	s_revive.revived[player] = true;   // read on the main thread once the loader has finished
}

void resetReviveVerdicts()
{
	resetRevive();
	s_floorRefused = 0;
}

bool claimPlayerDeath(int player)
{
	if ( player < 0 || player >= MAXPLAYERS ) { return true; }
	// With no script loaded no revive is ever refused, so a second death procedure for one death
	// cannot happen and this must not be able to change a vanilla game in any way.
	if ( !SamEvent::anyScripts() ) { return true; }
	if ( s_deathAnnounced[player] ) { return false; }
	s_deathAnnounced[player] = true;
	return true;
}

void notePlayerAlive(int player)
{
	if ( player < 0 || player >= MAXPLAYERS ) { return; }
	s_deathAnnounced[player] = false;
}

void fireFloorRevives()
{
	if ( multiplayer != CLIENT )
	{
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( s_revive.revived[p] ) { firePlayerRevived(p, "floor_load"); }
		}
	}
	resetRevive();
}

void firePlayerRevived(int player, const char* reviveType)
{
	if ( multiplayer == CLIENT || player < 0 || player >= MAXPLAYERS ) { return; }
	// Alive again, so their next death is a new death. actPlayer clears this too, on the first
	// tick their body is up; doing it here as well means a revive path that somehow never runs
	// actPlayer cannot leave a player whose death nothing would announce.
	s_deathAnnounced[player] = false;
	SamEvent ev("player.on_player_revived");
	ev.i("player", (long long)player)
		.i("revived_by", (long long)(-1))
		.i("floor", (long long)currentlevel)
		.s("revive_type", std::string(reviveType ? reviveType : ""));
	ev.fire();
}

// -------------------------------------------------------------------- players leaving

void notePlayerJoined(int player)
{
	if ( player >= 0 && player < MAXPLAYERS ) { s_leftAnnounced[player] = false; }
}

void firePlayerLeft(int player)
{
	if ( multiplayer == CLIENT || player <= 0 || player >= MAXPLAYERS ) { return; }
	// Several paths can notice one departure: the keep-alive drop, and then the DISC the client's
	// own game still sends on its way out. Once per stay in the slot.
	if ( s_leftAnnounced[player] ) { return; }
	s_leftAnnounced[player] = true;
	SamEvent ev("player.on_player_left");
	// `player` as well as the historical `player_index`, which stays for handlers that read it.
	// The rule that a screen call with no player shows to the player the event is about looks for
	// a field named exactly `player` (both runtimes scan for that key), so without this a
	// sam_hud_text written inside this handler landed on player 0 -- the host -- rather than on
	// the player who left. It now goes to them and is honestly refused, because they are gone.
	ev.i("player", (long long)player).i("player_index", (long long)player).s("player_name", playerName(player));
	ev.fire();
}

void relayPlayerGone(int player)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS || !net_packet ) { return; }
	if ( !SamEvent::anyScripts() ) { return; }   // a modless host's wire stays byte-identical
	// Vanilla DISC, which a stock client handles for a player > 0 by marking the slot gone, so
	// sam_player_count and everything else that reads client_disconnected agree on every machine.
	for ( int c = 1; c < MAXPLAYERS; ++c )
	{
		if ( c == player || client_disconnected[c] || players[c]->isLocalPlayer() ) { continue; }
		memcpy((char*)net_packet->data, "DISC", 4);
		net_packet->data[4] = (Uint8)player;
		net_packet->len = 5;
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

// -------------------------------------------------------------------- once per run

bool claimPartyWipe()
{
	if ( s_partyWipeFired ) { return false; }
	s_partyWipeFired = true;
	return true;
}

void resetRun()
{
	s_partyWipeFired = false;
	resetReviveVerdicts();
	for ( int p = 0; p < MAXPLAYERS; ++p )
	{
		s_approved[p].clear();
		s_deathAnnounced[p] = false;
		s_keptDead[p] = KeptDead();
	}
	s_client.pendingUse.clear();
}

// -------------------------------------------------------------------- callouts

void retractCallout(int player, unsigned int key)
{
	// peerMayHaveSam: a ping in the first seconds of a floor is vetoed before that joiner's HELLO
	// has landed, and with peerHasSam the retraction was never even queued -- the joiner kept a
	// marker nobody else could see. sendToClient holds an op for a peer inside the grace window and
	// delivers it the moment HELLO arrives; a machine that never says HELLO still receives nothing.
	if ( multiplayer != SERVER || !SAMNet::isRemotePlayer(player) || !SAMNet::peerMayHaveSam(player) ) { return; }
	SAMNet::Writer w;
	w.u32((std::uint32_t)key);
	SAMNet::sendToClient(player, OP_UNPING, w.buf);
}

}
