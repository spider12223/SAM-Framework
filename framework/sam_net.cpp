/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_net.cpp
	Desc: see sam_net.hpp.

-------------------------------------------------------------------------------*/

#include "sam_net.hpp"
#include "sam_logger.hpp"
#include "sam_event.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "main.hpp"
#include "game.hpp"
#include "entity.hpp"
#include "player.hpp"
#include "net.hpp"

namespace SAMNet
{
namespace
{
	// ---------------------------------------------------------------- the contract table

	const Contract s_contracts[] = {
#define SAM_MP(name, kind, target, arg, refusal) { #name, Kind::kind, Target::target, (std::uint8_t)(arg), Refusal::refusal },
#include "sam_mp_contracts.inc"
#undef SAM_MP
	};

	const std::unordered_map<std::string, const Contract*>& contractIndex()
	{
		static std::unordered_map<std::string, const Contract*> index;
		if ( index.empty() )
		{
			for ( const Contract& c : s_contracts ) { index[c.name] = &c; }
		}
		return index;
	}

	// ---------------------------------------------------------------- wire

	constexpr std::size_t HEADER = 15;
	constexpr std::size_t BODY_MAX = 480;            // HEADER + BODY_MAX stays under Barony's 503-byte reliable payload
	constexpr std::size_t MAX_ASSEMBLED = 256 * 1024;
	constexpr std::size_t MAX_PENDING = 2048;        // out-of-order frames one channel will hold
	constexpr std::size_t MAX_ORPHANS = 256;         // frames of a session not yet started
	constexpr std::size_t MAX_HELD_OPS = 512;        // ops for a client that has not said HELLO yet
	constexpr std::size_t MAX_INBOUND = 4096;
	constexpr std::size_t MAX_REPLAY = 1024;         // `all` calls kept for clients that join later
	constexpr std::size_t MAX_OUT_BYTES = 4 * 1024 * 1024;   // wire-ready frames waiting for one destination
	constexpr std::size_t ORPHANS_BEFORE_RESTART = 32;       // frames of a channel we cannot follow
	constexpr int FRAMES_PER_TICK = 64;              // per destination; the rest waits a tick
	constexpr Uint32 GRACE_TICKS = 20 * TICKS_PER_SECOND;
	constexpr Uint32 HELLO_RETRY_TICKS = 3 * TICKS_PER_SECOND;   // a HELLO the host has not answered
	constexpr Uint32 ADOPT_COOLDOWN_TICKS = TICKS_PER_SECOND;    // how often one slot may start a new channel
	// How long a channel may sit waiting for a frame that never came before it is started over.
	// Barony retries a safe packet six times and then drops it, which takes well under a second,
	// so five is generous: anything still missing then is missing for good. A level load blocks
	// the main loop, and `ticks` with it, so a long load cannot run this clock down.
	constexpr Uint32 STALL_TICKS = 5 * TICKS_PER_SECOND;
	constexpr std::uint8_t FLAG_MORE = 1;

	struct Frame
	{
		std::uint8_t op = 0;
		std::uint8_t flags = 0;
		std::string body;
	};

	struct RecvState
	{
		bool adopted = false;
		std::uint32_t session = 0;
		std::uint32_t next = 0;
		std::map<std::uint32_t, Frame> pending;
		std::map<std::pair<std::uint32_t, std::uint32_t>, Frame> orphans;
		std::string partial;
		bool partialOpen = false;
		bool dropping = false;   // an over-long body is being discarded up to its last frame
		// When frames started piling up behind one that has not arrived. Barony's reliable layer
		// gives up on a frame after six tries and then it is gone for good, so a hole that is
		// still there seconds later is never going to be filled, and waiting for MAX_PENDING more
		// frames to arrive behind it can take minutes on a quiet mod, or forever.
		Uint32 stallTick = 0;
		std::uint32_t stallNext = 0;   // the seq we were waiting for when that clock started
	};

	struct SendState
	{
		std::uint32_t session = 0;
		std::uint32_t seq = 0;
		std::size_t bytes = 0;         // what `out` holds, so one huge body is not counted as one frame
		std::deque<std::string> out;   // wire-ready frames
	};

	struct Peer
	{
		bool connected = false;
		bool hello = false;
		bool everHello = false;        // this machine has proved at least once that it runs S.A.M
		bool stock = false;
		Uint32 seenTick = 0;
		Uint32 adoptTick = 0;              // when this slot last opened a channel to us
		Uint32 restartTick = 0;            // when we last asked it to start its channel over
		std::uint32_t helloSession = 0;    // the channel whose HELLO has already been answered
		SendState send;
		RecvState recv;
		std::deque<std::pair<std::uint8_t, std::string>> held;
	};

	struct Inbound
	{
		int from;                 // -1 when it came from the host
		std::uint32_t session;    // the channel it arrived on, which may have been replaced since
		std::uint8_t op;
		std::string body;
	};

	Peer s_peer[MAXPLAYERS];
	SendState s_toHost;
	RecvState s_fromHost;
	bool s_helloSent = false;
	bool s_helloAcked = false;      // the host has answered on this session's channel
	bool s_helloGaveUp = false;     // said often enough; that host is not running S.A.M
	Uint32 s_helloTick = 0;         // when the last HELLO was queued
	Uint32 s_helloFirstTick = 0;    // when the first one was
	Uint32 s_fromHostAdoptTick = 0; // when the channel from the host was last replaced
	int s_helloAttempts = 0;
	std::deque<Inbound> s_inbound;

	ClientOpHandler s_clientOps[256] = {};
	HostOpHandler s_hostOps[256] = {};
	CallRunner s_runnerLua = nullptr;
	CallRunner s_runnerJs = nullptr;
	ItemRouteFn s_itemRouter = nullptr;

	std::deque<std::string> s_replay;   // Call bodies of `all` calls, in order, for late joiners
	std::set<std::string>& allowedEvents() { static std::set<std::string> v; return v; }
	std::vector<TickFn>& tickHooks() { static std::vector<TickFn> v; return v; }
	std::vector<ClearFn>& clearHooks() { static std::vector<ClearFn> v; return v; }
	std::vector<HelloFn>& helloHooks() { static std::vector<HelloFn> v; return v; }
	std::set<std::string> s_warned;

	bool s_forwarded = false;
	int s_contextPlayer = -1;

	// The address the datagram being handled came from, when the transport has one and net.cpp
	// told us before handleSafePacket overwrote it. See receiveOnHost.
	bool s_srcKnown = false;
	std::uint32_t s_srcHost = 0;
	std::uint16_t s_srcPort = 0;

	std::uint32_t newSession()
	{
		static std::mt19937 rng{ std::random_device{}() ^ (std::uint32_t)SDL_GetTicks() };
		std::uint32_t v = 0;
		while ( v == 0 ) { v = rng(); }
		return v;
	}

	void put32(std::string& s, std::size_t at, std::uint32_t v)
	{
		s[at + 0] = (char)((v >> 24) & 0xFF);
		s[at + 1] = (char)((v >> 16) & 0xFF);
		s[at + 2] = (char)((v >> 8) & 0xFF);
		s[at + 3] = (char)(v & 0xFF);
	}

	std::uint32_t get32(const Uint8* p)
	{
		return ((std::uint32_t)p[0] << 24) | ((std::uint32_t)p[1] << 16) | ((std::uint32_t)p[2] << 8) | (std::uint32_t)p[3];
	}

	void queueOp(SendState& s, const char* id, int player, std::uint8_t op, const std::string& body)
	{
		if ( s.session == 0 ) { s.session = newSession(); s.seq = 0; }
		std::size_t at = 0;
		do
		{
			const std::size_t n = std::min(BODY_MAX, body.size() - at);
			const bool more = (at + n) < body.size();
			std::string f(HEADER, '\0');
			std::memcpy(&f[0], id, 4);
			f[4] = (char)(Uint8)player;
			put32(f, 5, s.session);
			put32(f, 9, s.seq++);
			f[13] = (char)op;
			f[14] = (char)(more ? FLAG_MORE : 0);
			f.append(body, at, n);
			const std::size_t frameBytes = f.size();
			s.out.push_back(std::move(f));
			s.bytes += frameBytes;
			at += n;
		} while ( at < body.size() );
	}

	// Which side a channel warning is about, for the warnOnce key and the message. Keyed per
	// machine because a four player host used to report only the first casualty: one player's
	// congested channel silenced the warning for the other two.
	std::string whoKey(int from) { return ( from < 0 ) ? std::string("host") : std::to_string(from); }
	std::string whoText(int from) { return ( from < 0 ) ? std::string("the host") : ( "player " + std::to_string(from) ); }

	void deliver(RecvState& r, Frame& f, int from)
	{
		// The host holds everything it would send us until it has our HELLO, so a message that
		// arrived in order from it IS the answer the HELLO retry below waits for. Marked here
		// rather than at the packet, so a stray frame of a channel we no longer follow (which
		// never reaches this point) cannot pass for an answer.
		if ( from < 0 ) { s_helloAcked = true; }
		if ( r.dropping )
		{
			if ( !(f.flags & FLAG_MORE) ) { r.dropping = false; }
			return;
		}
		if ( f.flags & FLAG_MORE )
		{
			if ( r.partial.size() + f.body.size() > MAX_ASSEMBLED )
			{
				r.partial.clear();
				r.partialOpen = false;
				r.dropping = true;
				warnOnce("net:oversize:" + whoKey(from), "A multiplayer message from " + whoText(from)
					+ " was larger than 256 KB and was dropped.");
				return;
			}
			r.partial += f.body;
			r.partialOpen = true;
			return;
		}
		std::string body = r.partialOpen ? (r.partial + f.body) : std::move(f.body);
		r.partial.clear();
		r.partialOpen = false;
		// The caller only delivers while there is room, so a message is never dropped here: that
		// would acknowledge one that was never applied and leave an invisible hole in a stream
		// every module below reads as "all of them, in order".
		s_inbound.push_back(Inbound{ from, r.session, f.op, std::move(body) });
	}

	void adopt(RecvState& r, std::uint32_t session, int from)
	{
		r.adopted = true;
		r.session = session;
		r.next = 0;
		r.pending.clear();
		r.partial.clear();
		r.partialOpen = false;
		r.dropping = false;
		// Frames of this session that arrived before its first one.
		for ( auto it = r.orphans.begin(); it != r.orphans.end(); )
		{
			if ( it->first.first == session ) { r.pending.emplace(it->first.second, std::move(it->second)); it = r.orphans.erase(it); }
			else { ++it; }
		}
		r.orphans.clear();
		(void)from;
	}

	// Client: start this machine's channel to the host over. The next tick says HELLO again,
	// which is always the first frame of a new session, and the host answers a HELLO by
	// re-sending everything a late joiner gets -- so this one action repairs either direction.
	void restartHello(const char* why)
	{
		if ( multiplayer != CLIENT ) { return; }
		s_toHost = SendState();
		s_fromHost = RecvState();
		s_helloSent = false;
		s_helloAcked = false;
		s_helloGaveUp = false;
		s_helloAttempts = 0;
		if ( why ) { SAM_INFO("NET", std::string("Saying hello to the host again: ") + why + "."); }
	}

	// Hand every frame that is next in line to the queue the tick dispatches from, and stop while
	// that queue is full: the sequence must never advance past a message nothing applied, or the
	// channel carries on with a hole in it that no module below can see. Run again from the tick,
	// so frames that waited here are picked up even if the far side has gone quiet.
	void drainInOrder(RecvState& r, int from)
	{
		for ( auto it = r.pending.find(r.next); it != r.pending.end(); it = r.pending.find(r.next) )
		{
			if ( s_inbound.size() >= MAX_INBOUND )
			{
				warnOnce("net:inbound:" + whoKey(from), "Too many multiplayer messages arrived at once from "
					+ whoText(from) + "; they are waiting their turn.");
				break;
			}
			Frame fr = std::move(it->second);
			r.pending.erase(it);
			++r.next;
			deliver(r, fr, from);
		}
	}

	// Throw away everything this channel knows, so the far side's next session is adopted clean.
	// A session is only ever adopted at its first frame, so the two ends cannot half heal.
	void forgetChannel(RecvState& r)
	{
		r.adopted = false;
		r.session = 0;
		r.next = 0;
		r.pending.clear();
		r.orphans.clear();
		r.partial.clear();
		r.partialOpen = false;
		r.dropping = false;
		r.stallTick = 0;
		r.stallNext = 0;
	}

	// Has this channel been waiting too long for a frame that is never coming? Called once a tick
	// per channel, AFTER drainInOrder has taken everything it can: anything still in `pending`
	// then is behind a hole at `next`. Returns true exactly once per stall, and starts the clock
	// the first time it sees one.
	bool channelStalled(RecvState& r)
	{
		if ( r.pending.empty() ) { r.stallTick = 0; return false; }
		// The clock restarts every time the sequence moves. Frames waiting is NOT by itself a
		// stall: the tick applies a budget of them and leaves the rest, so a busy channel always
		// has some waiting. Only a `next` that has not moved at all means the frame it wants is
		// never coming.
		if ( r.stallTick == 0 || r.next != r.stallNext )
		{
			r.stallTick = ticks ? ticks : 1;
			r.stallNext = r.next;
			return false;
		}
		return (Uint32)(ticks - r.stallTick) >= STALL_TICKS;
	}

	// Apply one received frame to a channel, in order.
	void accept(RecvState& r, std::uint32_t session, std::uint32_t seq, Frame&& f, int from)
	{
		if ( !r.adopted || session != r.session )
		{
			// A sender starts every session at seq 0, so seq 0 of a session we do not know yet
			// means the far side started over (reconnected, or its game ended). Anything else
			// from an unknown session waits for that first frame.
			if ( seq == 0 )
			{
				adopt(r, session, from);
			}
			else
			{
				if ( r.orphans.size() >= MAX_ORPHANS ) { r.orphans.erase(r.orphans.begin()); }
				r.orphans[{ session, seq }] = std::move(f);
				return;
			}
		}
		if ( seq < r.next ) { return; }   // a duplicate the reliable layer delivered twice
		if ( r.pending.size() >= MAX_PENDING )
		{
			if ( r.pending.count(r.next) > 0 )
			{
				// Not a gap: the queue below is full (a level load runs no ticks, so nothing is
				// applied while it lasts) and this frame has nowhere to go. Dropping it makes a
				// gap, which the arm below then recovers from -- better than growing without end.
				warnOnce("net:slow:" + whoKey(from), "Multiplayer messages from " + whoText(from)
					+ " are arriving faster than this machine can apply them.");
				return;
			}
			// The frame this channel is waiting for never arrived and everything behind it has
			// piled up. Barony's reliable layer gives up after six tries, so nothing will deliver
			// it now. Forget this channel instead of stalling for the rest of the game: the far
			// side starts a new one (a client says HELLO again, which re-primes everything), and
			// a session is adopted only at its first frame, so the two cannot half-heal.
			warnOnce("net:gap:" + std::to_string(from),
				"A multiplayer message never arrived and later ones piled up behind it"
				+ ( from < 0 ? std::string(" (from the host)") : " (from player " + std::to_string(from) + ")" )
				+ "; that channel is being started over.");
			forgetChannel(r);
			// On a client, this is the channel FROM the host: only a fresh HELLO can restart it.
			// On the host, receiveOnHost notices the client's frames piling up unfollowed and asks
			// that machine to start over.
			if ( from < 0 ) { restartHello("the messages from the host stopped arriving in order"); }
			return;
		}
		r.pending.emplace(seq, std::move(f));
		drainInOrder(r, from);
	}

	bool parseFrame(int& player, std::uint32_t& session, std::uint32_t& seq, Frame& f)
	{
		if ( !net_packet || net_packet->len < (int)HEADER ) { return false; }
		const Uint8* d = net_packet->data;
		player = (int)d[4];
		session = get32(&d[5]);
		seq = get32(&d[9]);
		f.op = d[13];
		f.flags = d[14];
		const int bodyLen = net_packet->len - (int)HEADER;
		if ( bodyLen < 0 || bodyLen > (int)BODY_MAX ) { return false; }
		f.body.assign((const char*)&d[HEADER], (std::size_t)bodyLen);
		return true;
	}

	void resetPeer(int p)
	{
		s_peer[p] = Peer();
	}

	void sendFramesToClient(int p)
	{
		// net_packet is allocated with the netgame and freed when it ends; a tick that lands
		// either side of that (a disconnect during a flush) would otherwise write through null.
		if ( !net_packet || !net_clients || p <= 0 || p >= MAXPLAYERS ) { return; }
		Peer& peer = s_peer[p];
		int sent = 0;
		while ( !peer.send.out.empty() && sent < FRAMES_PER_TICK )
		{
			const std::string& f = peer.send.out.front();
			const std::size_t frameBytes = f.size();
			std::memcpy(net_packet->data, f.data(), f.size());
			net_packet->len = (int)f.size();
			net_packet->address.host = net_clients[p - 1].host;
			net_packet->address.port = net_clients[p - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, p - 1);
			peer.send.out.pop_front();
			peer.send.bytes -= std::min(peer.send.bytes, frameBytes);
			++sent;
		}
	}

	void sendFramesToHost()
	{
		if ( !net_packet ) { return; }
		int sent = 0;
		while ( !s_toHost.out.empty() && sent < FRAMES_PER_TICK )
		{
			const std::string& f = s_toHost.out.front();
			const std::size_t frameBytes = f.size();
			std::memcpy(net_packet->data, f.data(), f.size());
			net_packet->len = (int)f.size();
			net_packet->address.host = net_server.host;
			net_packet->address.port = net_server.port;
			sendPacketSafe(net_sock, -1, net_packet, 0);
			s_toHost.out.pop_front();
			s_toHost.bytes -= std::min(s_toHost.bytes, frameBytes);
			++sent;
		}
	}

	void runCall(const std::string& body)
	{
		Reader r(body);
		const char runtime = (char)r.u8();
		const std::string ns = r.str8();
		const std::string name = r.str8();
		if ( !r.ok ) { return; }
		const std::string args = body.substr(r.pos);
		const Contract* c = contractFor(name);
		// Only what the host is allowed to put on this machine's screen or into this
		// machine's own player's state. Anything else in a Call is not a thing the host
		// ever sends, so it is not run.
		if ( !c || (c->kind != Kind::Screen && c->kind != Kind::Owner && c->kind != Kind::All) )
		{
			// A name the contract table knows may key the warning, because that set is fixed.
			// A name it does not know came off the wire and could be anything, so it collapses
			// to one key: the warned set is never pruned, and a peer must not be able to grow it.
			warnOnce(c ? ( "net:call:" + name ) : std::string("net:call:unknown"),
				"The host asked this machine to run '" + name.substr(0, 64)
				+ "', which is not a function the host may run here; ignored.");
			return;
		}
		CallRunner run = (runtime == 'J') ? s_runnerJs : s_runnerLua;
		if ( !run ) { run = (runtime == 'J') ? s_runnerLua : s_runnerJs; }
		if ( !run ) { return; }
		ForwardScope scope;
		ContextPlayerScope who(clientnum);
		run(ns, name, args);
	}

	void fireClientEvent(int from, const std::string& body)
	{
		Reader r(body);
		const std::string name = r.str8();
		std::vector<std::pair<std::string, long long>> ints;
		std::vector<std::pair<std::string, std::string>> strs;
		const int nInts = r.u8();
		for ( int i = 0; i < nInts && r.ok; ++i )
		{
			std::string k = r.str8();
			const long long v = r.i64();
			ints.emplace_back(std::move(k), v);
		}
		const int nStrs = r.u8();
		for ( int i = 0; i < nStrs && r.ok; ++i )
		{
			std::string k = r.str8();
			std::string v = r.str16();
			strs.emplace_back(std::move(k), std::move(v));
		}
		if ( !r.ok ) { return; }
		if ( !allowedEvents().count(name) )
		{
			// Keyed on the SENDER, never on the name: the name comes off the wire, so a client
			// sending a different one every frame would otherwise grow this set and the host's
			// log without any bound. One line per player still names what was refused.
			warnOnce("net:event:" + std::to_string(from), "Player " + std::to_string(from) + "'s game asked the host to fire '" + name
				+ "', which no part of S.A.M forwards; ignored.");
			return;
		}
		SamEvent ev(name.c_str());
		ev.i("player", from);   // the sender's own slot, whatever the packet claims
		for ( const auto& kv : ints ) { if ( kv.first != "player" ) { ev.i(kv.first.c_str(), kv.second); } }
		for ( const auto& kv : strs ) { ev.s(kv.first.c_str(), kv.second); }
		ev.fire();
	}

	void dispatchInbound()
	{
		// A budget per tick, not "everything that arrived". Every limit above this one bounds how
		// much is STORED or how fast we SEND; nothing bounded how much work one tick does, and a
		// handler can fire script events, each of which walks every loaded script in both
		// runtimes. A full queue applied in one go is a visible freeze. What is left over is
		// applied next tick, in order, which is all the channel promises.
		constexpr std::size_t MAX_PER_TICK = 256;
		std::deque<Inbound> work;
		while ( work.size() < MAX_PER_TICK && !s_inbound.empty() )
		{
			work.push_back(std::move(s_inbound.front()));
			s_inbound.pop_front();
		}
		for ( Inbound& in : work )
		{
			if ( multiplayer == CLIENT && in.from < 0 )
			{
				if ( in.op == Op::Call ) { runCall(in.body); continue; }
				if ( in.op == Op::Restart )
				{
					restartHello("the host is no longer following this machine's channel");
					continue;
				}
				if ( s_clientOps[in.op] ) { s_clientOps[in.op](in.body); }
				continue;
			}
			if ( multiplayer == SERVER && in.from > 0 && in.from < MAXPLAYERS )
			{
				if ( client_disconnected[in.from] ) { continue; }
				Peer& peer = s_peer[in.from];
				if ( in.op == Op::Hello )
				{
					Reader r(in.body);
					const std::uint16_t proto = r.u16();
					// Only from the channel this slot is actually being followed on, and only once
					// per channel. A HELLO that arrived on a channel something has since replaced
					// must not mark the slot as running S.A.M -- otherwise one forged frame could
					// make the host stream 'SAMC' at a stock client, which the header promises can
					// never happen -- and a repeat on the same channel must not re-send the replay.
					if ( in.session != peer.recv.session || in.session == peer.helloSession ) { continue; }
					peer.helloSession = in.session;
					const bool again = peer.everHello;
					peer.hello = true;
					peer.everHello = true;
					peer.stock = false;
					// EVERY hello re-primes, not just the first. The two machines reach a new run
					// at their own moment, and a lost hello is said again, so a client that started
					// its channels over would otherwise be met by a host that had not yet, and get
					// nothing at all for the rest of the run.
					//
					// The channel back to it starts over with it: that machine adopts a session only
					// at its first frame, so carrying on from the old sequence number would leave
					// everything we send orphaned on its side.
					peer.send = SendState();
					SAM_INFO("NET", "Player " + std::to_string(in.from) + (again ? " said hello again" : " runs S.A.M scripts")
						+ " (protocol " + std::to_string(proto) + "); calls for them are carried to their machine.");
					// Tables first (class and item patches the host made before they arrived), then
					// whatever was waiting for their screen. Deliberate, and worth knowing: a screen
					// call made BEFORE an `all` call arrives on that machine AFTER it, because the
					// two were kept in different lists. `all` calls are table edits a screen call
					// cannot depend on, so this is the safer order of the two.
					for ( const auto& body : s_replay ) { queueOp(peer.send, "SAMC", in.from, Op::Call, body); }
					for ( auto& h : peer.held ) { queueOp(peer.send, "SAMC", in.from, h.first, h.second); }
					peer.held.clear();
					// Then every module's catch-up for a player who missed what came before.
					for ( HelloFn fn : helloHooks() ) { fn(in.from); }
					continue;
				}
				if ( !peer.hello ) { continue; }   // nothing before HELLO means anything
				if ( in.op == Op::Event ) { fireClientEvent(in.from, in.body); continue; }
				if ( s_hostOps[in.op] ) { s_hostOps[in.op](in.from, in.body); }
			}
		}
	}
}

// -------------------------------------------------------------------- contracts

const Contract* contractFor(const std::string& name)
{
	const auto& index = contractIndex();
	auto it = index.find(name);
	return it == index.end() ? nullptr : it->second;
}

const char* kindLabel(Kind k)
{
	switch ( k )
	{
		case Kind::Host:   return "host";
		case Kind::Owner:  return "owner";
		case Kind::Screen: return "screen";
		case Kind::Read:   return "read";
		case Kind::All:    return "all";
		case Kind::Local:  return "local";
		case Kind::Any:    return "any";
	}
	return "any";
}

// -------------------------------------------------------------------- routing

bool isRemotePlayer(int player)
{
	if ( player < 0 || player >= MAXPLAYERS ) { return false; }
	if ( multiplayer == SERVER )
	{
		if ( player == 0 ) { return false; }
		return !client_disconnected[player] && !(players[player] && players[player]->isLocalPlayer());
	}
	if ( multiplayer == CLIENT ) { return player != clientnum; }
	return false;
}

bool peerHasSam(int player)
{
	return multiplayer == SERVER && player > 0 && player < MAXPLAYERS && s_peer[player].hello;
}

bool peerMayHaveSam(int player)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS ) { return false; }
	if ( client_disconnected[player] ) { return false; }
	return s_peer[player].hello || !s_peer[player].stock;
}

Decision decide(const Contract& c, int player, bool remoteItem)
{
	Decision d;
	if ( multiplayer == SINGLE || s_forwarded ) { return d; }

	auto refuse = [&d](const std::string& why) { d.route = Route::Refuse; d.why = why; return d; };
	auto noSam = [&](int p) {
		return refuse("player " + std::to_string(p) + "'s game is not running S.A.M scripts, so this cannot reach their machine.");
	};

	switch ( c.kind )
	{
		case Kind::Any:
			return d;

		case Kind::Local:
			// Target::Uid is deliberately not in this test. A Local function named by a uid
			// (sam_get_entity_ticks) answers from this machine's own copy of that entity, which
			// is the answer the reference promises for it, and refusing it for a remote player's
			// uid would take that answer away. Only a function named by a PLAYER claims to be
			// about that player, and only that claim can be wrong here.
			if ( (c.target == Target::Player || c.target == Target::OptPlayer) && isRemotePlayer(player) )
			{
				return refuse("player " + std::to_string(player) + " is on another machine; this only answers for a player on this one.");
			}
			return d;

		case Kind::All:
			if ( multiplayer == CLIENT )
			{
				return refuse("changes something every machine keeps a copy of, which the host decides. Call it on the host (or when your mod loads); S.A.M sends it to everyone.");
			}
			d.route = Route::ForwardAndHere;
			d.player = -1;
			return d;

		case Kind::Host:
			if ( multiplayer == CLIENT )
			{
				return refuse("runs on the host. Your events, on_tick and timers already run there; on a client this call does nothing.");
			}
			return d;

		case Kind::Read:
			if ( multiplayer == SERVER ) { return d; }
			if ( c.target == Target::None || c.target == Target::Item ) { return d; }
			if ( player == clientnum ) { return d; }
			return refuse("a client only knows its own player. Read it on the host, which knows everyone.");

		case Kind::Owner:
			if ( multiplayer == CLIENT )
			{
				return refuse("changes a player's own items or spells, which the host decides. Call it on the host; S.A.M carries it to the player.");
			}
			if ( remoteItem || isRemotePlayer(player) )
			{
				if ( !peerMayHaveSam(player) ) { return noSam(player); }
				d.route = Route::Forward;
				d.player = player;
			}
			return d;

		case Kind::Screen:
			if ( player == EVERYONE )
			{
				if ( multiplayer == SERVER ) { d.route = Route::ForwardAndHere; d.player = -1; }
				return d;
			}
			if ( player < 0 ) { return d; }
			if ( multiplayer == CLIENT )
			{
				if ( player == clientnum ) { return d; }
				return refuse("a client can only show things on its own screen.");
			}
			if ( !isRemotePlayer(player) ) { return d; }
			if ( !peerMayHaveSam(player) ) { return noSam(player); }
			d.route = Route::Forward;
			d.player = player;
			return d;
	}
	return d;
}

bool inForwardedCall() { return s_forwarded; }
ForwardScope::ForwardScope() : prev(s_forwarded) { s_forwarded = true; }
ForwardScope::~ForwardScope() { s_forwarded = prev; }

int contextPlayer() { return s_contextPlayer; }
ContextPlayerScope::ContextPlayerScope(long long player) : prev(s_contextPlayer)
{
	s_contextPlayer = (player >= 0 && player < MAXPLAYERS) ? (int)player : -1;
}
ContextPlayerScope::~ContextPlayerScope() { s_contextPlayer = prev; }

int defaultScreenPlayer()
{
	if ( s_contextPlayer >= 0 ) { return s_contextPlayer; }
	return multiplayer == CLIENT ? clientnum : 0;
}

int playerOfUid(std::uint32_t uid)
{
	if ( uid == 0 ) { return -1; }
	for ( int p = 0; p < MAXPLAYERS; ++p )
	{
		if ( players[p] && players[p]->entity && players[p]->entity->getUID() == uid ) { return p; }
	}
	return -1;
}

void setItemRouter(ItemRouteFn fn) { s_itemRouter = fn; }
bool routeItem(std::uint32_t uid, int& player, std::uint32_t& ownerUid)
{
	return s_itemRouter && multiplayer == SERVER && s_itemRouter(uid, player, ownerUid);
}

void warnOnce(const std::string& key, const std::string& message)
{
	if ( s_warned.insert(key).second ) { SAM_WARN("NET", message); }
}

// -------------------------------------------------------------------- channels

bool sendToClient(int player, std::uint8_t op, const std::string& body)
{
	if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS ) { return false; }
	if ( client_disconnected[player] || !SamEvent::anyScripts() ) { return false; }
	// Refuse here rather than send fragments the far side will throw away on arrival: over
	// MAX_ASSEMBLED it reassembles nothing, so the caller would be told the message went and the
	// message would simply never happen. Answering false lets the caller say so.
	if ( body.size() > MAX_ASSEMBLED )
	{
		warnOnce("net:sendsize:" + std::to_string((int)op), "A mod message of "
			+ std::to_string(body.size() / 1024) + " KB is larger than the 256 KB one message can carry;"
			" it was not sent.");
		return false;
	}
	Peer& peer = s_peer[player];
	if ( peer.stock ) { return false; }
	if ( !peer.hello )
	{
		if ( peer.held.size() >= MAX_HELD_OPS )
		{
			// Refuse the newest rather than make room by dropping the oldest: that would deliver
			// "fill the panel" without the "open the panel" that made it, which is the one thing
			// this channel exists to prevent. What is already waiting keeps the order it was made
			// in, and the caller is told this one did not go.
			warnOnce("net:held:" + std::to_string(player), "Player " + std::to_string(player)
				+ "'s game has not said hello yet and 512 mod calls are already waiting for it; this one was refused.");
			return false;
		}
		peer.held.emplace_back(op, body);
		return true;
	}
	if ( peer.send.bytes >= MAX_OUT_BYTES )
	{
		// The queue is drained 64 frames a tick, so a mod that queues faster than that would
		// otherwise grow it without end (and add a tick of delay per 64 frames). Refuse instead.
		warnOnce("net:outfull:" + std::to_string(player), "More is being sent to player " + std::to_string(player)
			+ "'s game than the connection can carry, so some of it is being refused; send less, or less often.");
		return false;
	}
	queueOp(peer.send, "SAMC", player, op, body);
	return true;
}

bool sendToHost(std::uint8_t op, const std::string& body)
{
	if ( multiplayer != CLIENT || !SamEvent::anyScripts() ) { return false; }
	// The same refusal as sendToClient: over MAX_ASSEMBLED the host reassembles nothing, so
	// sending it would be telling the caller a message went that can never arrive.
	if ( body.size() > MAX_ASSEMBLED )
	{
		warnOnce("net:sendsize:host:" + std::to_string((int)op), "A mod message of "
			+ std::to_string(body.size() / 1024) + " KB is larger than the 256 KB one message can carry;"
			" it was not sent to the host.");
		return false;
	}
	if ( !s_helloSent )
	{
		// HELLO is always the first frame of a session: the host ignores everything before it,
		// and adopts a channel only at that frame. The tick says it again until the host answers.
		Writer w;
		w.u16(PROTOCOL);
		queueOp(s_toHost, "SAMQ", clientnum, Op::Hello, w.buf);
		s_helloSent = true;
		s_helloTick = ticks;
		if ( s_helloAttempts == 0 ) { s_helloFirstTick = ticks; }
		++s_helloAttempts;
	}
	if ( op == Op::Hello ) { return true; }
	if ( s_helloGaveUp )
	{
		// That host never answered a hello, so it is not running S.A.M. Nothing else is sent to it,
		// ever: a stock 5.0.2 host can only log each frame as a mystery packet. Modules already
		// wait for the host's READY before they send anything; this makes it structural.
		return false;
	}
	if ( s_toHost.bytes >= MAX_OUT_BYTES )
	{
		warnOnce("net:outfull:host", "More is being sent to the host than the connection can carry, so some of it is being refused.");
		return false;
	}
	queueOp(s_toHost, "SAMQ", clientnum, op, body);
	return true;
}

void onClientOp(std::uint8_t op, ClientOpHandler fn) { s_clientOps[op] = fn; }
void onHostOp(std::uint8_t op, HostOpHandler fn) { s_hostOps[op] = fn; }
void allowClientEvent(const char* name) { if ( name ) { allowedEvents().insert(name); } }
void addTickHook(TickFn fn) { if ( fn ) { tickHooks().push_back(fn); } }
void addClearHook(ClearFn fn) { if ( fn ) { clearHooks().push_back(fn); } }
void addHelloHook(HelloFn fn) { if ( fn ) { helloHooks().push_back(fn); } }

bool sendEventToHost(const std::string& name, const EventFields& fields)
{
	Writer w;
	w.str8(name);
	w.u8((std::uint8_t)std::min(fields.nInts, 255));
	w.buf += fields.ints;
	w.u8((std::uint8_t)std::min(fields.nStrings, 255));
	w.buf += fields.strings;
	return sendToHost(Op::Event, w.buf);
}

void noteDatagramSource(bool known, std::uint32_t host, std::uint16_t port)
{
	s_srcKnown = known;
	s_srcHost = host;
	s_srcPort = port;
}

void receiveOnClient()
{
	if ( multiplayer != CLIENT || !SamEvent::anyScripts() ) { return; }
	int player = 0;
	std::uint32_t session = 0, seq = 0;
	Frame f;
	if ( !parseFrame(player, session, seq, f) ) { return; }
	if ( player != clientnum ) { return; }
	// Only from the host, where the transport lets us tell (see receiveOnHost).
	if ( s_srcKnown && (s_srcHost != net_server.host || s_srcPort != net_server.port) )
	{
		warnOnce("net:spoof:host", "A multiplayer message claimed to be from the host but came from somewhere else; ignored.");
		return;
	}
	// Over Steam or EOS the reader has no source address at all, so "from the host" above is only
	// the sender's own claim. The host defends its side by adopting a channel no faster than once
	// a second (receiveOnHost); this is the same rule for ours, so nothing that can reach this
	// machine may throw away the channel it is following over and over. Dropping the frame is
	// safe: a session is adopted at its first frame only, and if that leaves us following nothing,
	// the orphan arm in tick() says hello again.
	const bool newSession = ( !s_fromHost.adopted || session != s_fromHost.session );
	if ( seq == 0 && newSession )
	{
		if ( s_fromHost.adopted && (Uint32)(ticks - s_fromHostAdoptTick) < ADOPT_COOLDOWN_TICKS ) { return; }
		s_fromHostAdoptTick = ticks;
	}
	accept(s_fromHost, session, seq, std::move(f), -1);
}

void receiveOnHost()
{
	if ( multiplayer != SERVER || !SamEvent::anyScripts() ) { return; }
	int from = 0;
	std::uint32_t session = 0, seq = 0;
	Frame f;
	if ( !parseFrame(from, session, seq, f) ) { return; }
	if ( from <= 0 || from >= MAXPLAYERS || client_disconnected[from] ) { return; }
	// The slot in the frame is the SENDER'S OWN CLAIM, as it is in every vanilla packet. Where the
	// transport gives us a real source address (a direct connection, and only if net.cpp stashed it
	// for us: handleSafePacket overwrites net_packet->address with the address of the acknowledgement
	// it sends back before any handler runs), that settles it and nobody can speak for another
	// player's slot. Over Steam or EOS the reader has no address at all, and then the rules below are
	// what is left: a channel is opened only by a HELLO, no faster than once a second, and a HELLO
	// only ever marks the machine already being followed on that channel -- so a forged frame can
	// disturb a channel but cannot make the host treat a stock client as a S.A.M one.
	if ( s_srcKnown && net_clients )
	{
		if ( s_srcHost != net_clients[from - 1].host || s_srcPort != net_clients[from - 1].port )
		{
			warnOnce("net:spoof:" + std::to_string(from), "A multiplayer message claimed to be from player " + std::to_string(from)
				+ " but came from somewhere else; ignored.");
			return;
		}
	}
	Peer& peer = s_peer[from];
	if ( !peer.connected )
	{
		// Heard from before the tick noticed the slot: note it now, or the tick's
		// "newly connected" reset would throw this frame (its HELLO) away.
		peer.connected = true;
		peer.seenTick = ticks;
	}
	if ( !peer.recv.adopted || session != peer.recv.session )
	{
		if ( seq == 0 )
		{
			// A client's channel always opens with HELLO, so nothing else may open one -- and not
			// faster than once a second, so a flood cannot keep throwing away the channel of the
			// player whose slot it names.
			if ( f.op != Op::Hello ) { return; }
			if ( peer.recv.adopted && (Uint32)(ticks - peer.adoptTick) < ADOPT_COOLDOWN_TICKS ) { return; }
			peer.adoptTick = ticks;
		}
	}
	accept(peer.recv, session, seq, std::move(f), from);
	if ( peer.everHello && peer.recv.orphans.size() >= ORPHANS_BEFORE_RESTART
		&& (Uint32)(ticks - peer.restartTick) >= HELLO_RETRY_TICKS )
	{
		// Frames of a channel we are not following keep arriving: this machine's channel and ours
		// are out of step (a gap gave up on the old one, or something else displaced it), so
		// nothing it sends can ever be delivered. Ask it to start over. Only ever to a machine
		// that has already said HELLO, so a stock client is still never sent anything.
		peer.restartTick = ticks;
		queueOp(peer.send, "SAMC", from, Op::Restart, std::string());
	}
}

bool forwardCall(int player, char runtime, const std::string& ns, const std::string& name, const std::string& args)
{
	if ( args.size() > MAX_CALL_BYTES )
	{
		warnOnce("net:callsize:" + name, name + ": the arguments are over 16 KB, too large to send to player "
			+ std::to_string(player) + "'s machine.");
		return false;
	}
	Writer w;
	w.u8((std::uint8_t)runtime);
	w.str8(ns);
	w.str8(name);
	w.buf += args;
	return sendToClient(player, Op::Call, w.buf);
}

void forwardCallToAll(char runtime, const std::string& ns, const std::string& name, const std::string& args, bool replayLater)
{
	if ( multiplayer != SERVER || !SamEvent::anyScripts() ) { return; }
	if ( args.size() > MAX_CALL_BYTES )
	{
		warnOnce("net:callsize:" + name, name + ": the arguments are over 16 KB, too large to send to the other players.");
		return;
	}
	Writer w;
	w.u8((std::uint8_t)runtime);
	w.str8(ns);
	w.str8(name);
	w.buf += args;
	for ( int p = 1; p < MAXPLAYERS; ++p )
	{
		if ( client_disconnected[p] || !isRemotePlayer(p) ) { continue; }
		if ( replayLater && !s_peer[p].hello ) { continue; }   // gets it from the replay at HELLO
		if ( !sendToClient(p, Op::Call, w.buf) && s_peer[p].stock )
		{
			// Naming a player gets a clear refusal from decide(); "every player" and every `all`
			// call used to skip a machine without S.A.M in complete silence. One line per player.
			warnOnce("net:stock:" + std::to_string(p), "Player " + std::to_string(p)
				+ "'s game is not running S.A.M scripts, so it sees none of what the mod shows or changes for everyone.");
		}
	}
	if ( replayLater )
	{
		if ( s_replay.size() >= MAX_REPLAY )
		{
			// Keep the FIRST 1024 rather than the last: a late joiner applies the replay in order,
			// so a prefix leaves its class and item tables consistent as far as it goes. Dropping
			// the oldest left a hole in the middle that nothing else could ever fill, because an
			// `all` call is refused on a client and the replay is the only way one reaches it.
			warnOnce("net:replay", "More than 1024 class/item changes have been made in this game; '" + name
				+ "' and any after it will not reach a player who joins later.");
			return;
		}
		s_replay.push_back(w.buf);
	}
}

void setCallRunner(char runtime, CallRunner fn)
{
	if ( runtime == 'J' ) { s_runnerJs = fn; }
	else { s_runnerLua = fn; }
}

void tick()
{
	if ( multiplayer == SINGLE || intro ) { return; }
	if ( !SamEvent::anyScripts() ) { return; }

	if ( multiplayer == SERVER )
	{
		for ( int p = 1; p < MAXPLAYERS; ++p )
		{
			const bool conn = !client_disconnected[p];
			Peer& peer = s_peer[p];
			if ( conn != peer.connected )
			{
				resetPeer(p);
				peer.connected = conn;
				peer.seenTick = ticks;
			}
			if ( conn && !peer.hello && !peer.stock && (Uint32)(ticks - peer.seenTick) > GRACE_TICKS )
			{
				peer.stock = true;
				if ( !peer.held.empty() )
				{
					SAM_INFO("NET", "Player " + std::to_string(p) + "'s game has not said it runs S.A.M scripts, so nothing more"
						+ " is queued for it; the " + std::to_string(peer.held.size()) + " mod call(s) already waiting still go out if it does.");
				}
				// What is already held is NOT thrown away. A joiner can still be loading its map
				// and its mod assets twenty seconds in, and a sam_give_item or sam_grant_spell made
				// in game.on_game_start cannot be derived again once it is gone. It is bounded by
				// MAX_HELD_OPS, nothing is added while the peer is marked stock (a script gets an
				// honest refusal instead), and only a real disconnect or the end of the game drops it.
			}
		}
	}
	else if ( multiplayer == CLIENT )
	{
		if ( !s_helloSent )
		{
			sendToHost(Op::Hello, std::string());
		}
		else if ( !s_helloAcked && !s_helloGaveUp && (Uint32)(ticks - s_helloTick) >= HELLO_RETRY_TICKS )
		{
			// HELLO is the one frame the channel cannot do without: the host ignores everything
			// before it, and Barony's reliable layer gives up after six tries. Losing that single
			// frame used to disable S.A.M for this player for the whole run, with the host's log
			// blaming a mod list that was perfectly correct. Say it again -- on a NEW session,
			// because a channel is adopted only at its first frame -- until the host answers.
			if ( (Uint32)(ticks - s_helloFirstTick) < GRACE_TICKS )
			{
				s_toHost = SendState();   // start over: anything queued behind a hello nobody heard is lost anyway
				s_helloSent = false;
				sendToHost(Op::Hello, std::string());
			}
			else
			{
				// The same twenty seconds the host waits. Past it, stop: a host that is not
				// running S.A.M must not be sent packets it can only log as a mystery.
				s_helloGaveUp = true;
				SAM_INFO("NET", "The host never answered this machine's S.A.M hello, so this game is being played as if the host had no mods.");
			}
		}
	}

	// Frames that had to wait because the queue below was full (a long level load applies
	// nothing) are picked up here, so a channel cannot sit on them until the far side
	// happens to send another one.
	if ( multiplayer == SERVER )
	{
		for ( int p = 1; p < MAXPLAYERS; ++p )
		{
			if ( client_disconnected[p] ) { continue; }
			Peer& peer = s_peer[p];
			if ( !peer.recv.pending.empty() ) { drainInOrder(peer.recv, p); }
			// Anything still waiting is behind a frame that has not come. Barony drops a safe
			// packet for good after six tries, so a hole that is still there five seconds later
			// is permanent, and the MAX_PENDING arm in accept() would not notice until 2048 more
			// frames arrived behind it -- minutes on a quiet mod, and never on a quiet one.
			if ( channelStalled(peer.recv) )
			{
				warnOnce("net:stall:" + std::to_string(p),
					"A message from player " + std::to_string(p) + " never arrived, so that channel"
					" is being started over; anything their game told us in the meantime is lost.");
				forgetChannel(peer.recv);
				if ( peer.everHello && (Uint32)(ticks - peer.restartTick) >= HELLO_RETRY_TICKS )
				{
					// Ask them to open a fresh one. This goes out on OUR channel to them, which is
					// a different channel and healthy unless it is stalled too -- and if it is,
					// their own check below does the same thing from that end.
					peer.restartTick = ticks;
					queueOp(peer.send, "SAMC", p, Op::Restart, std::string());
				}
			}
		}
	}
	else
	{
		if ( !s_fromHost.pending.empty() ) { drainInOrder(s_fromHost, -1); }
		if ( channelStalled(s_fromHost) )
		{
			warnOnce("net:stall:host", "A message from the host never arrived, so this machine is"
				" starting its S.A.M channel over.");
			forgetChannel(s_fromHost);
			restartHello("a message from the host never arrived and later ones piled up behind it");
		}
		else if ( s_fromHost.orphans.size() >= ORPHANS_BEFORE_RESTART
			&& (Uint32)(ticks - s_helloTick) >= HELLO_RETRY_TICKS )
		{
			// Frames of a channel this machine is not following keep arriving: its first frame was
			// missed, so nothing the host sends can ever be delivered. The host has this arm for
			// the other direction (receiveOnHost); this is ours.
			restartHello("the host's messages are on a channel this machine never picked up");
		}
	}

	dispatchInbound();

	for ( TickFn fn : tickHooks() ) { fn(); }

	if ( multiplayer == SERVER )
	{
		for ( int p = 1; p < MAXPLAYERS; ++p )
		{
			if ( !client_disconnected[p] && !s_peer[p].send.out.empty() ) { sendFramesToClient(p); }
		}
	}
	else if ( multiplayer == CLIENT )
	{
		sendFramesToHost();
	}
}

void clear()
{
	for ( int p = 0; p < MAXPLAYERS; ++p ) { resetPeer(p); }
	s_toHost = SendState();
	s_fromHost = RecvState();
	s_helloSent = false;
	s_helloAcked = false;
	s_helloGaveUp = false;
	s_helloTick = 0;
	s_helloFirstTick = 0;
	s_helloAttempts = 0;
	s_inbound.clear();
	s_replay.clear();
	s_warned.clear();
	s_contextPlayer = -1;
	s_srcKnown = false;
	for ( ClearFn fn : clearHooks() ) { fn(); }
}

// -------------------------------------------------------------------- bytes

void Writer::u8(std::uint8_t v) { buf.push_back((char)v); }
void Writer::u16(std::uint16_t v) { u8((std::uint8_t)(v >> 8)); u8((std::uint8_t)v); }
void Writer::u32(std::uint32_t v) { u16((std::uint16_t)(v >> 16)); u16((std::uint16_t)v); }
void Writer::i64(long long v)
{
	const std::uint64_t u = (std::uint64_t)v;
	u32((std::uint32_t)(u >> 32));
	u32((std::uint32_t)u);
}
void Writer::f64(double v)
{
	std::uint64_t u = 0;
	static_assert(sizeof(u) == sizeof(v), "double must be 64-bit");
	std::memcpy(&u, &v, sizeof(u));
	i64((long long)u);
}
void Writer::str8(const std::string& s)
{
	const std::size_t n = std::min<std::size_t>(s.size(), 255);
	u8((std::uint8_t)n);
	buf.append(s, 0, n);
}
void Writer::str16(const std::string& s)
{
	const std::size_t n = std::min<std::size_t>(s.size(), 65535);
	u16((std::uint16_t)n);
	buf.append(s, 0, n);
}

std::uint8_t Reader::u8()
{
	if ( !ok || pos + 1 > data.size() ) { ok = false; return 0; }
	return (std::uint8_t)data[pos++];
}
std::uint16_t Reader::u16() { const std::uint16_t hi = u8(); return (std::uint16_t)((hi << 8) | u8()); }
std::uint32_t Reader::u32() { const std::uint32_t hi = u16(); return (hi << 16) | u16(); }
long long Reader::i64() { const std::uint64_t hi = u32(); return (long long)((hi << 32) | u32()); }
double Reader::f64()
{
	const std::uint64_t u = (std::uint64_t)i64();
	double v = 0;
	std::memcpy(&v, &u, sizeof(v));
	return v;
}
std::string Reader::str8()
{
	const std::size_t n = u8();
	if ( !ok || pos + n > data.size() ) { ok = false; return std::string(); }
	std::string s = data.substr(pos, n);
	pos += n;
	return s;
}
std::string Reader::str16()
{
	const std::size_t n = u16();
	if ( !ok || pos + n > data.size() ) { ok = false; return std::string(); }
	std::string s = data.substr(pos, n);
	pos += n;
	return s;
}

void EventFields::addInt(const std::string& key, long long v)
{
	Writer w;
	w.str8(key);
	w.i64(v);
	ints += w.buf;
	++nInts;
}
void EventFields::addStr(const std::string& key, const std::string& v)
{
	Writer w;
	w.str8(key);
	w.str16(v);
	strings += w.buf;
	++nStrings;
}

}
