/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_net.hpp
	Desc: how every script function behaves in multiplayer, and the two ordered
	      channels that carry a call to the machine it has to run on.

	THE MODEL. A mod's runtime code runs on the HOST. on_tick, timers and nearly every
	event fire there and nowhere else (game.cpp gates them on multiplayer != CLIENT), so
	a script is written once, as if for one machine, and names players by index. This
	module is what makes that true in co-op instead of true only for player 0:

	  * Every script function declares a CONTRACT in sam_mp_contracts.inc:
	      host   - the host decides; a client's call is refused. The function itself
	               sends the result on (stats, effects, monsters, the world).
	      owner  - the host decides, but the state lives on the player's OWN machine
	               (their backpack, their spell list), so the call is carried there.
	      screen - shows something to a player: carried to that player's machine.
	      read   - reads a player: the host can read everyone, a client only itself.
	      all    - changes a table every machine keeps its own copy of (a class or item
	               patch): run on the host and on every client, and replayed to a
	               client that joins later.
	      local  - answers for the machine running the script (its clock, its keys);
	               given a player on another machine, it says so instead of guessing.
	      any    - the same answer on every machine (replicated data, or pure math).
	    Both script runtimes wrap every registered function in one trampoline that
	    applies its contract, so a new function cannot forget to, and the API gate
	    (tools/gen_api_docs.mjs) refuses a function that has no contract.

	  * 'SAMC' (host -> one client) and 'SAMQ' (client -> host) are ORDERED reliable op
	    channels. Barony's reliable packets are unordered; a panel opened and then
	    filled must not arrive filled-then-opened, so each frame carries a sequence
	    number and the receiver applies frames strictly in order.

	  * A client announces itself with HELLO once it is in a game and has scripts. Until then
	    the host holds what it would have sent, and after twenty seconds it stops adding to
	    that (a script gets an honest refusal) but keeps what is already waiting, in case the
	    joiner is simply slow. A machine that never says HELLO is treated as a stock client:
	    the ordered channel sends it nothing at all. The three older packets -- 'SAMT' (scripted
	    tile edits), 'SAMB' (custom bodies) and 'SAMI' (a full-screen picture) -- follow ONE rule,
	    and it is a deliberate trade: they still go to a machine that has not said HELLO YET, so a
	    slow joiner and a mod set with no scripts at all (a JSON body needs none, and then nobody
	    is ever declared stock) keep working, and they stop the moment a peer IS declared stock.
	    A stock client therefore sees at most twenty seconds of them -- plus a dug wall, which it
	    understands anyway, always -- instead of one "mystery packet" line per announcement, per
	    edited tile and per picture for the whole game, which for a re-tiled room is thousands.
	    The price is paid by a client on an OLDER S.A.M build: it understands those packets but
	    never says HELLO, so twenty seconds in it is treated as stock and stops receiving them.
	    That is the cheaper of the two prices, and it is bounded on both sides. ('SAMI' is barely
	    reachable either way: a picture for another player is carried as a call and drawn by that
	    machine itself.)

	  * HELLO is the one frame the channels cannot do without -- a channel is adopted only at
	    its first frame -- so the client says it again every few seconds until the host
	    answers, and stops at the same twenty seconds, so a host without S.A.M sees only a
	    handful of packets it does not know. Every HELLO re-primes: the host re-sends the
	    replay, flushes what it held and runs every HelloHook, and starts its own channel to
	    that client over. Two machines cross a run boundary at their own moment, so a client
	    that cleared first must not be swallowed by a host that has not cleared yet.

	  * A channel that cannot be followed any more (a frame that never arrived, with 2048
	    behind it) is thrown away rather than left to stall: the client says HELLO again --
	    asked to by the host when it is the client->host direction -- and everything above is
	    re-sent. Both channels are also reset at every run boundary (doNewGame), because the
	    engine skips doEndgame when a game is restarted in place.

	  * Nothing is sent from inside a packet handler or a script call. Outgoing frames
	    are queued and flushed once per tick, and incoming ones are dispatched from the
	    tick too: net_packet is ONE global buffer, and an engine handler that fires an
	    event and then reads its own packet would otherwise read ours.

	WIRE (both ids):
	  [0..3] id | [4] player (SAMC: the client it is for, SAMQ: the sender)
	  [5..8] session | [9..12] seq | [13] op | [14] flags (bit0: the body continues
	  in the next seq) | [15..] body, at most BODY_MAX bytes per frame.

	A stock 5.0.2 machine logs "mystery packet" for either id and carries on.
	Pure no-op with no script loaded: nothing is sent and nothing is polled.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>

namespace SAMNet
{
	// ------------------------------------------------------------------ contracts

	enum class Kind : std::uint8_t { Host, Owner, Screen, Read, All, Local, Any };

	// What names the player a call is about.
	enum class Target : std::uint8_t
	{
		None,       // not about one player
		Player,     // argument `arg` is a player index (optional ones resolve like OptPlayer,
		            // but stay in the arguments: the function reads them itself)
		Uid,        // argument `arg` is an entity uid; the player is whoever that entity is
		Item,       // argument `arg` is an item uid; the player is whoever holds the item
		OptPlayer,  // an OPTIONAL trailing player argument at `arg` that the function body never
		            // sees (the trampoline strips it). Absent or nil: the player the current
		            // event is about, else this machine's own player. -1: every player.
		            // "The player the event is about" is the event's int field named exactly
		            // `player` (both runtimes scan for that key), so an event that names its
		            // player something else must ALSO carry a `player` field or this rule does
		            // not apply to it.
	};

	// What a refused call returns.
	enum class Refusal : std::uint8_t { False, Nil, Zero, Empty, None };

	struct Contract
	{
		const char* name;
		Kind kind;
		Target target;
		std::uint8_t arg;     // 1-based position of the target argument; 0 with Target::None
		Refusal refusal;
	};

	const Contract* contractFor(const std::string& name);
	const char* kindLabel(Kind k);   // "host", "owner", "screen", "read", "all", "local", "any"

	// ------------------------------------------------------------------ routing

	enum class Route : std::uint8_t
	{
		Here,            // run the function on this machine
		Forward,         // carry it to `player`'s machine instead
		ForwardAndHere,  // run it here AND carry it to every remote player (screen with player -1,
		                 // and every `all` call)
		Refuse,          // do nothing; `why` is the warning
	};
	struct Decision
	{
		Route route = Route::Here;
		int player = -1;
		std::string why;
	};

	constexpr int EVERYONE = -2;   // the trampoline's resolved target for "player -1"

	// The trampoline has resolved the target player (-1 none/unknown, EVERYONE). `remoteItem`
	// is true when the target is an item the host only mirrors for a remote player.
	Decision decide(const Contract& c, int player, bool remoteItem);

	// True while a call the host carried here is running. The contract checks and the
	// inline host-only guards let it through: the host already made the decision.
	bool inForwardedCall();
	struct ForwardScope
	{
		ForwardScope();
		~ForwardScope();
		bool prev;
	};

	// The player the event being dispatched is about (-1 when it is about nobody). A screen
	// function called with no player inside a handler shows to THAT player, so "when player 2
	// levels up, show a banner" lands on player 2's screen without the script naming anyone.
	int contextPlayer();
	struct ContextPlayerScope
	{
		explicit ContextPlayerScope(long long player);
		~ContextPlayerScope();
		int prev;
	};
	int defaultScreenPlayer();

	// The player slot whose entity this uid is, or -1.
	int playerOfUid(std::uint32_t uid);

	// Is this player index on another machine, as seen from here?
	bool isRemotePlayer(int player);

	// The inventory mirror installs this: does `uid` name an item the host only mirrors for a
	// remote player? If so, give the holder and the uid the holder's machine knows it by.
	using ItemRouteFn = bool (*)(std::uint32_t uid, int& player, std::uint32_t& ownerUid);
	void setItemRouter(ItemRouteFn fn);
	bool routeItem(std::uint32_t uid, int& player, std::uint32_t& ownerUid);

	// Say something once per session per key. A refused call inside on_tick would otherwise
	// write the same line fifty times a second.
	void warnOnce(const std::string& key, const std::string& message);

	// ------------------------------------------------------------------ peers (host side)

	bool peerHasSam(int player);       // that client said HELLO
	bool peerMayHaveSam(int player);   // HELLO, or still inside the grace window

	// ------------------------------------------------------------------ channels

	namespace Op
	{
		// host -> client
		constexpr std::uint8_t Call = 1;       // run a script function: [runtime][ns][name][args]
		constexpr std::uint8_t Restart = 3;    // say HELLO again: the host is no longer following you

		// client -> host
		constexpr std::uint8_t Hello = 1;      // [u16 protocol]
		constexpr std::uint8_t Event = 2;      // fire an event on the host, about the sender

		// 16 and up belong to subsystems. Each module owns a block, documents its ops beside
		// its handlers, and registers them with onClientOp / onHostOp. The two directions are
		// separate spaces: host->client op 20 and client->host op 20 are different ops.
		constexpr std::uint8_t SoundFirst = 16;      // 16..23  sounds and music      (sam_sounds / sam_music)
		constexpr std::uint8_t UiFirst = 24;         // 24..31  panels and HUD        (sam_ui / sam_hud)
		constexpr std::uint8_t InputFirst = 32;      // 32..39  keys and actions      (sam_mp_input)
		constexpr std::uint8_t InventoryFirst = 40;  // 40..55  backpacks and spells  (sam_mp_inventory)
		constexpr std::uint8_t EntityFirst = 56;     // 56..71  entities and world    (sam_mp_entities)
		constexpr std::uint8_t RulesFirst = 72;      // 72..87  player state, rules   (sam_rules and friends)
		constexpr std::uint8_t EventFirst = 88;      // 88..103 engine events a client raises (sam_mp_input)
	}
	constexpr std::uint16_t PROTOCOL = 1;

	// Queue an op. Host: for one client, false if that client cannot take it (not connected, a
	// stock client, or more is already waiting for it than can be held or carried -- the ordering
	// is worth more than the message, so a full queue refuses the newest rather than dropping the
	// oldest). Client: for the host. A body of any size is split across frames and rejoined in
	// order on the far side.
	bool sendToClient(int player, std::uint8_t op, const std::string& body);
	bool sendToHost(std::uint8_t op, const std::string& body);

	using ClientOpHandler = void (*)(const std::string& body);            // runs on a client
	using HostOpHandler = void (*)(int from, const std::string& body);    // runs on the host
	void onClientOp(std::uint8_t op, ClientOpHandler fn);
	void onHostOp(std::uint8_t op, HostOpHandler fn);

	// A client may ask the host to fire only the events a module has allowed by name. A module
	// allows its events at static-initialisation time with `static SAMNet::AllowEvent a("x.on_y");`.
	void allowClientEvent(const char* name);
	struct AllowEvent { explicit AllowEvent(const char* name) { allowClientEvent(name); } };

	// Module hooks, so a subsystem never has to edit game.cpp or menu.cpp for its multiplayer
	// state. All run only while scripts are loaded. Register at static-initialisation time:
	//   static SAMNet::TickHook t(&myTick);   (every game tick, on every machine, IN A MULTIPLAYER
	//                                          GAME, before the channels are flushed. Singleplayer
	//                                          and the menu run no tick hooks at all, so a sweep
	//                                          that has to happen in singleplayer too needs its own
	//                                          call site: tick() returns early on `multiplayer ==
	//                                          SINGLE || intro`)
	//   static SAMNet::ClearHook c(&myClear); (a game ended, a new run started, or mods reloaded:
	//                                          forget peers and anything keyed by a uid or a slot)
	//   static SAMNet::HelloHook h(&mySync);  (host: player p just said HELLO - send them the
	//                                          state a late joiner has missed. It can run more
	//                                          than once for one connection: a new run and a
	//                                          repaired channel both start with a fresh HELLO,
	//                                          so send the whole picture, not a difference)
	using TickFn = void (*)();
	using ClearFn = void (*)();
	using HelloFn = void (*)(int player);
	void addTickHook(TickFn fn);
	void addClearHook(ClearFn fn);
	void addHelloHook(HelloFn fn);
	struct TickHook { explicit TickHook(TickFn fn) { addTickHook(fn); } };
	struct ClearHook { explicit ClearHook(ClearFn fn) { addClearHook(fn); } };
	struct HelloHook { explicit HelloHook(HelloFn fn) { addHelloHook(fn); } };

	// Client side: fire `name` on the host, about this client's player. `ints`/`strings`
	// are the fields; "player" is always overwritten by the host with the sender's slot.
	struct EventFields;
	bool sendEventToHost(const std::string& name, const EventFields& fields);

	// The net.cpp handlers: 'SAMC' in the client table, 'SAMQ' in the host table.
	void receiveOnClient();
	void receiveOnHost();

	// The address a datagram came from, told to us by net.cpp the moment it is read and BEFORE
	// handleSafePacket overwrites net_packet->address with the address of the acknowledgement it
	// sends back. `known` is false when the transport has no source address to give (Steam and
	// EOS read from a queue that carries none), and then the player byte in a frame is taken on
	// trust, exactly as every vanilla packet takes it. Never called: nothing is checked.
	void noteDatagramSource(bool known, std::uint32_t host, std::uint16_t port);

	// Carry a script call to `player` (built by a runtime trampoline). `runtime` is 'L' or 'J'.
	bool forwardCall(int player, char runtime, const std::string& ns, const std::string& name, const std::string& args);
	// Carry it to every remote player. `replayLater`: also keep it and send it to a client
	// that says HELLO later (an `all` call changes a table a late joiner would otherwise
	// never hear about); such a call goes only to clients that have already said HELLO.
	void forwardCallToAll(char runtime, const std::string& ns, const std::string& name, const std::string& args, bool replayLater);
	using CallRunner = void (*)(const std::string& ns, const std::string& name, const std::string& args);
	void setCallRunner(char runtime, CallRunner fn);

	// Once per game tick on every machine (game.cpp), after the scripts have run.
	void tick();

	// Forget every peer, channel and held op (a game ended, a new run started, mods were reloaded).
	// Called from doNewGame as well as doEndgame: the engine skips doEndgame when a game is
	// restarted in place, and a channel that outlives the run it belongs to is a dead channel.
	void clear();

	// ------------------------------------------------------------------ bytes

	struct Writer
	{
		std::string buf;
		void u8(std::uint8_t v);
		void u16(std::uint16_t v);
		void u32(std::uint32_t v);
		void i64(long long v);
		void f64(double v);
		void str8(const std::string& s);    // up to 255 bytes, longer is cut
		void str16(const std::string& s);   // up to 65535 bytes, longer is cut
	};

	struct Reader
	{
		explicit Reader(const std::string& s) : data(s) {}
		const std::string& data;
		std::size_t pos = 0;
		bool ok = true;      // false once any read ran off the end; every later read returns 0/""
		std::uint8_t u8();
		std::uint16_t u16();
		std::uint32_t u32();
		long long i64();
		double f64();
		std::string str8();
		std::string str16();
		bool atEnd() const { return pos >= data.size(); }
	};

	struct EventFields
	{
		std::string ints;      // built with addInt
		std::string strings;   // built with addStr
		int nInts = 0;
		int nStrings = 0;
		void addInt(const std::string& key, long long v);
		void addStr(const std::string& key, const std::string& v);
	};

	// ------------------------------------------------------------------ script values
	//
	// Arguments cross the wire as a small tagged encoding the two runtimes both read and
	// write: nil, false, true, an integer, a double, a string, an array, or a string/integer
	// keyed map, nested up to MAX_DEPTH. A function or other native value cannot cross,
	// and the trampoline refuses to carry a call that holds one.
	namespace Tag
	{
		constexpr std::uint8_t Nil = 0, False = 1, True = 2, Int = 3, Num = 4, Str = 5, Arr = 6, Map = 7;
	}
	constexpr int MAX_DEPTH = 4;
	constexpr std::size_t MAX_CALL_BYTES = 16 * 1024;
}

// A guard for the inline "host only" checks inside a function body: a call the host carried
// to this machine has already been decided, so it must not be refused here.
#define SAM_CLIENT_REFUSES ( multiplayer == CLIENT && !SAMNet::inForwardedCall() )
