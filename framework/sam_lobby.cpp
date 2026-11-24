/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_lobby.cpp
	Desc: mod-owned lobby slots. See sam_lobby.hpp.

-------------------------------------------------------------------------------*/

// Must precede every include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_lobby.hpp"
#include "sam_event.hpp"
#include "sam_logger.hpp"
#include "sam_sync.hpp"

#include "main.hpp"     // multiplayer, clientnum, client_disconnected, net_packet, net_clients, net_sock
#include "game.hpp"
#include "net.hpp"      // sendPacketSafe
#include "stat.hpp"     // stats[]: a player's name in slotInfo
#include "player.hpp"   // players[]->getAccountName()

#include <algorithm>
#include <cstring>

static const char* MOD = "LOBBY";

namespace
{
	SAMLobby::Ui g_ui;
	std::vector<SAMLobby::SlotAction> g_actions;
	const std::vector<SAMLobby::SlotAction> g_noActions;
	SAMLobby::Card g_cards[MAXPLAYERS];
	const SAMLobby::Card g_emptyCard;
	bool g_open = false;              // a lobby exists on this machine
	bool g_loadingSave = false;
	bool g_hello[MAXPLAYERS] = {};    // host: that client's S.A.M said hello
	bool g_hostAnswered = false;      // client: the host sent DONE
	bool g_helloSent = false;         // client: this lobby's hello has gone to the host

	void say(std::string* why, const std::string& text)
	{
		if ( why ) { *why = text; }
	}

	// Shortens to at most max bytes without cutting a UTF-8 character in two.
	void cutUtf8(std::string& s, std::size_t max)
	{
		if ( s.size() <= max ) { return; }
		std::size_t cut = max;
		while ( cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80 ) { --cut; }
		s.resize(cut);
	}

	// Newlines would split one row into two on the client (see displayText), and control
	// characters draw as boxes. Text is cut to `max` bytes, never in the middle of a UTF-8
	// sequence.
	std::string clean(const std::string& in, std::size_t max)
	{
		std::string out;
		out.reserve(in.size());
		for ( unsigned char c : in )
		{
			if ( c == '\n' || c == '\r' || c == '\t' ) { out.push_back(' '); }
			else if ( c < 32 || c == 127 ) { continue; }
			else { out.push_back((char)c); }
		}
		cutUtf8(out, max);
		return out;
	}

	// The real source of the datagram the host's lobby is handling, on a direct connection
	// (SAMLobby::noteSource; Steam and EOS give none).
	bool g_srcKnown = false;
	std::uint32_t g_srcHost = 0;
	std::uint16_t g_srcPort = 0;

	bool validSlot(int slot) { return slot > 0 && slot < MAXPLAYERS; }

	bool slotHasPlayer(int slot)
	{
		return slot >= 0 && slot < MAXPLAYERS && !client_disconnected[slot];
	}

	bool slotOpen(int slot)
	{
		if ( !validSlot(slot) || slotHasPlayer(slot) ) { return false; }
		return g_ui.slotOpen ? g_ui.slotOpen(slot) : true;
	}

	std::string playerName(int slot)
	{
		if ( stats[slot] && stats[slot]->name[0] ) { return stats[slot]->name; }
		if ( players[slot] )
		{
			const char* n = players[slot]->getAccountName();
			if ( n && n[0] ) { return n; }
		}
		return "Player " + std::to_string(slot + 1);
	}

	// ---- parsing a card -----------------------------------------------------------------

	bool readText(const SamValue& row, int index, const char* key, std::string& out, std::string* why)
	{
		const SamValue* v = row.get(key);
		if ( !v || v->isNil() ) { say(why, "row " + std::to_string(index) + " needs " + key + " (a string)"); return false; }
		if ( v->type != SamValue::Type::Str ) { say(why, "row " + std::to_string(index) + ": " + key + " must be a string, not a " + SamValue::typeName(v->type)); return false; }
		if ( (int)v->s.size() > SAMLobby::MAX_TEXT ) { say(why, "row " + std::to_string(index) + ": " + key + " is longer than " + std::to_string(SAMLobby::MAX_TEXT) + " bytes"); return false; }
		out = clean(v->s, SAMLobby::MAX_TEXT);
		return true;
	}

	bool parseRow(const SamValue& v, int index, SAMLobby::Row& row, std::string* why)
	{
		if ( v.type != SamValue::Type::Map )
		{
			say(why, "row " + std::to_string(index) + " must be a table like { text = \"...\" }");
			return false;
		}
		const SamValue* buttons = v.get("buttons");
		if ( buttons && !buttons->isNil() )   // a JavaScript undefined or null is a Lua nil: the key is not there
		{
			if ( buttons->type != SamValue::Type::Arr || buttons->arr.empty() || (int)buttons->arr.size() > SAMLobby::MAX_BUTTONS )
			{
				say(why, "row " + std::to_string(index) + ": buttons must be a list of 1 to " + std::to_string(SAMLobby::MAX_BUTTONS) + " control ids");
				return false;
			}
			const SamValue* labels = v.get("labels");
			if ( labels && !labels->isNil() && labels->type != SamValue::Type::Arr )
			{
				say(why, "row " + std::to_string(index) + ": labels must be a list of strings, one per button");
				return false;
			}
			row.kind = SAMLobby::Row::Kind::Buttons;
			for ( std::size_t b = 0; b < buttons->arr.size(); ++b )
			{
				const SamValue& id = buttons->arr[b];
				if ( id.type != SamValue::Type::Str || !SAMLobby::validId(id.s) )
				{
					say(why, "row " + std::to_string(index) + ": button " + std::to_string(b + 1) + " is not a control id (letters, digits, _ . -, up to " + std::to_string(SAMLobby::MAX_ID) + ")");
					return false;
				}
				if ( id.s == "title" )
				{
					say(why, "row " + std::to_string(index) + ": \"title\" is the card title's own control id");
					return false;
				}
				std::string label = id.s;
				// A hole in the list is refused like any other non-string, as Lua (whose holey table
				// is not a list at all) already refuses it.
				if ( labels && labels->type == SamValue::Type::Arr && b < labels->arr.size() )
				{
					const SamValue& l = labels->arr[b];
					if ( l.type != SamValue::Type::Str ) { say(why, "row " + std::to_string(index) + ": label " + std::to_string(b + 1) + " must be a string"); return false; }
					if ( (int)l.s.size() > SAMLobby::MAX_TEXT ) { say(why, "row " + std::to_string(index) + ": label " + std::to_string(b + 1) + " is longer than " + std::to_string(SAMLobby::MAX_TEXT) + " bytes"); return false; }
					label = l.s;
				}
				row.buttons.push_back(id.s);
				row.labels.push_back(clean(label, SAMLobby::MAX_TEXT));
			}
			return true;
		}
		const SamValue* cycle = v.get("cycle");
		if ( cycle && !cycle->isNil() )
		{
			if ( cycle->type != SamValue::Type::Str || !SAMLobby::validId(cycle->s) || cycle->s == "title" )
			{
				say(why, "row " + std::to_string(index) + ": cycle must be a control id (letters, digits, _ . -, up to " + std::to_string(SAMLobby::MAX_ID) + ")");
				return false;
			}
			row.kind = SAMLobby::Row::Kind::Cycle;
			row.control = cycle->s;
			return readText(v, index, "text", row.text, why);
		}
		row.kind = SAMLobby::Row::Kind::Text;
		return readText(v, index, "text", row.text, why);
	}

	bool parseCard(const SamValue& v, SAMLobby::Card& card, std::string* why)
	{
		if ( v.type != SamValue::Type::Map )
		{
			say(why, "the card must be a table { title = \"...\", rows = { ... } }");
			return false;
		}
		const SamValue* title = v.get("title");
		if ( !title || title->type != SamValue::Type::Str )
		{
			say(why, "the card needs a title (a string)");
			return false;
		}
		if ( (int)title->s.size() > SAMLobby::MAX_TITLE )
		{
			say(why, "the title is longer than " + std::to_string(SAMLobby::MAX_TITLE) + " bytes");
			return false;
		}
		card.title = clean(title->s, SAMLobby::MAX_TITLE);
		if ( v.wrongType("edit_title", SamValue::Type::Bool) )
		{
			say(why, "edit_title must be true or false");
			return false;
		}
		card.editTitle = v.flag("edit_title", false);
		const SamValue* rows = v.get("rows");
		if ( rows && !rows->isNil() )
		{
			// A named table ({ first = {...} }) has no order, so the card would draw its rows in
			// whatever order the runtime happened to walk them. Refused rather than guessed.
			if ( rows->type != SamValue::Type::Arr )
			{
				say(why, "rows must be a list { {...}, {...} }, not a table with names");
				return false;
			}
			if ( (int)rows->arr.size() > SAMLobby::MAX_ROWS )
			{
				say(why, "a card has at most " + std::to_string(SAMLobby::MAX_ROWS) + " rows");
				return false;
			}
			for ( std::size_t r = 0; r < rows->arr.size(); ++r )
			{
				SAMLobby::Row row;
				if ( !parseRow(rows->arr[r], (int)r + 1, row, why) ) { return false; }
				card.rows.push_back(std::move(row));
			}
		}
		return true;
	}

	// ---- the wire ---------------------------------------------------------------------------

	// sendPacketSafe puts a 9-byte header in front and keeps NET_PACKET_SIZE - 9 bytes of ours.
	constexpr int MAX_FRAME = NET_PACKET_SIZE - 9;
	// The largest display text that leaves room for our header, the namespace and its length.
	constexpr int MAX_DISPLAY = MAX_FRAME - 9 - 1 - 63;

	// Our own packet, never net_packet: that is ONE global buffer, and the 'SAML' handlers run
	// while an engine handler's packet is still in it (sam_net.hpp).
	Uint8 g_buffer[NET_PACKET_SIZE];
	UDPpacket g_packet;

	void sendFrame(int hostnum, IPaddress address, const std::string& frame)
	{
		if ( frame.empty() || (int)frame.size() > MAX_FRAME ) { return; }
		std::memset(g_buffer, 0, sizeof(g_buffer));
		std::memcpy(g_buffer, frame.data(), frame.size());
		g_packet.channel = -1;
		g_packet.data = g_buffer;
		g_packet.len = (int)frame.size();
		g_packet.maxlen = NET_PACKET_SIZE;
		g_packet.status = 0;
		g_packet.address = address;
		sendPacketSafe(net_sock, -1, &g_packet, hostnum);
	}

	void sendFrameTo(int slot, const std::string& frame)
	{
		if ( multiplayer != SERVER || !validSlot(slot) || client_disconnected[slot] ) { return; }
		IPaddress a{};
		if ( net_clients ) { a = net_clients[slot - 1]; }   // Steam and EOS route by slot, not address
		sendFrame(slot - 1, a, frame);
	}

	std::string cardFrame(int slot)
	{
		const SAMLobby::Card& c = g_cards[slot];
		const bool shown = !c.ns.empty();
		std::string text = shown ? SAMLobby::displayText(c) : std::string();
		cutUtf8(text, MAX_DISPLAY);
		const std::string ns = c.ns.substr(0, 63);
		std::string f("SAML", 4);
		f.push_back((char)SAMLobby::Op::Card);
		f.push_back((char)slot);
		f.push_back((char)(shown ? 1 : 0));
		f.push_back((char)((text.size() >> 8) & 0xFF));
		f.push_back((char)(text.size() & 0xFF));
		f += text;
		f.push_back((char)ns.size());
		f += ns;
		return f;
	}

	void sendCardToPeers(int slot)
	{
		if ( multiplayer != SERVER ) { return; }
		const std::string frame = cardFrame(slot);
		for ( int p = 1; p < MAXPLAYERS; ++p )
		{
			if ( g_hello[p] && !client_disconnected[p] ) { sendFrameTo(p, frame); }
		}
	}

	void claim(int slot, const std::string& ns, SAMLobby::Card&& card)
	{
		const bool fresh = g_cards[slot].ns.empty();
		card.ns = ns;
		g_cards[slot] = std::move(card);
		if ( fresh && g_ui.setSlotLocked ) { g_ui.setSlotLocked(slot, true); }
		if ( g_ui.slotChanged ) { g_ui.slotChanged(slot); }
		sendCardToPeers(slot);
	}

	void release(int slot)
	{
		if ( g_cards[slot].ns.empty() ) { return; }
		g_cards[slot] = SAMLobby::Card();
		if ( g_ui.setSlotLocked ) { g_ui.setSlotLocked(slot, false); }
		if ( g_ui.slotChanged ) { g_ui.slotChanged(slot); }
		sendCardToPeers(slot);
	}

	void forgetCards()
	{
		for ( auto& c : g_cards ) { c = SAMLobby::Card(); }
	}
}

namespace SAMLobby
{
	void setUi(const Ui& ui) { g_ui = ui; }

	bool validId(const std::string& id)
	{
		if ( id.empty() || (int)id.size() > MAX_ID ) { return false; }
		for ( unsigned char c : id )
		{
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
				|| c == '_' || c == '.' || c == '-';
			if ( !ok ) { return false; }
		}
		return true;
	}

	bool addSlotAction(const std::string& ns, const std::string& id, const std::string& label, std::string* why)
	{
		if ( ns.empty() ) { say(why, "no owning mod namespace"); return false; }
		if ( !validId(id) ) { say(why, "'" + id + "' is not an action id (letters, digits, _ . -, up to " + std::to_string(MAX_ID) + ")"); return false; }
		const std::string text = clean(label, MAX_TEXT);
		if ( text.empty() ) { say(why, "the label is empty"); return false; }
		if ( (int)label.size() > MAX_TEXT ) { say(why, "the label is longer than " + std::to_string(MAX_TEXT) + " bytes"); return false; }
		for ( auto& a : g_actions )
		{
			if ( a.ns == ns && a.id == id ) { a.label = text; return true; }   // re-registering: a new label
		}
		if ( (int)g_actions.size() >= MAX_ACTIONS )
		{
			say(why, "the lobby already has " + std::to_string(MAX_ACTIONS) + " slot actions from loaded mods");
			return false;
		}
		g_actions.push_back(SlotAction{ ns, id, text });
		if ( (int)g_actions.size() > SHOWN_ACTIONS )
		{
			SAM_WARN(MOD, "More than " + std::to_string(SHOWN_ACTIONS) + " slot actions are registered; an empty slot shows the first "
				+ std::to_string(SHOWN_ACTIONS) + " (" + ns + ":" + id + " is not drawn).");
		}
		return true;
	}

	const std::vector<SlotAction>& slotActions()
	{
		return multiplayer == CLIENT ? g_noActions : g_actions;
	}

	bool setCard(const std::string& ns, int slot, const SamValue& value, std::string* why)
	{
		if ( ns.empty() ) { say(why, "no owning mod namespace"); return false; }
		if ( multiplayer == CLIENT ) { say(why, "only the host sets lobby cards"); return false; }
		if ( !g_open ) { say(why, "no lobby is open (call it from lobby.on_lobby_opened, lobby.on_slot_action or lobby.on_card_input)"); return false; }
		if ( g_loadingSave ) { say(why, "a lobby for a saved game keeps the party it was saved with"); return false; }
		if ( !validSlot(slot) ) { say(why, "slot " + std::to_string(slot) + " cannot take a card (slots 1 to " + std::to_string(MAXPLAYERS - 1) + ")"); return false; }
		if ( slotHasPlayer(slot) ) { say(why, "a player is in slot " + std::to_string(slot)); return false; }
		const Card& have = g_cards[slot];
		if ( !have.ns.empty() && have.ns != ns ) { say(why, "slot " + std::to_string(slot) + " belongs to " + have.ns); return false; }
		if ( have.ns.empty() && !slotOpen(slot) ) { say(why, "slot " + std::to_string(slot) + " is closed in this lobby"); return false; }
		Card card;
		if ( !parseCard(value, card, why) ) { return false; }
		// Every limit can hold and the whole still not fit the one frame a client is sent (five rows
		// of three long labels): refused here, instead of cut short on the way.
		const std::size_t size = displayText(card).size();
		if ( size > (std::size_t)MAX_DISPLAY )
		{
			say(why, "the card is too long to send: " + std::to_string(size) + " bytes of text, at most " + std::to_string(MAX_DISPLAY));
			return false;
		}
		claim(slot, ns, std::move(card));
		return true;
	}

	bool clearCard(const std::string& ns, int slot, std::string* why)
	{
		if ( multiplayer == CLIENT ) { say(why, "only the host clears lobby cards"); return false; }
		if ( !validSlot(slot) ) { say(why, "slot " + std::to_string(slot) + " has no card"); return false; }
		const Card& have = g_cards[slot];
		if ( have.ns.empty() ) { say(why, "slot " + std::to_string(slot) + " has no card"); return false; }
		if ( have.ns != ns ) { say(why, "slot " + std::to_string(slot) + " belongs to " + have.ns); return false; }
		release(slot);
		return true;
	}

	SamValue slotInfo(int slot)
	{
		if ( slot < 0 || slot >= MAXPLAYERS ) { return SamValue::nil(); }
		SamValue v = SamValue::object();
		v.set("slot", SamValue::integer(slot));
		if ( !g_cards[slot].ns.empty() )
		{
			v.set("state", SamValue::string("card"));
			v.set("mod", SamValue::string(g_cards[slot].ns));
			v.set("title", SamValue::string(g_cards[slot].title));
		}
		else if ( slotHasPlayer(slot) )
		{
			v.set("state", SamValue::string("player"));
			v.set("name", SamValue::string(playerName(slot)));
		}
		else if ( !g_loadingSave && slotOpen(slot) )
		{
			v.set("state", SamValue::string("open"));
		}
		else
		{
			v.set("state", SamValue::string("closed"));
		}
		return v;
	}

	bool isClaimed(int slot)
	{
		return validSlot(slot) && !g_cards[slot].ns.empty();
	}

	const Card& card(int slot)
	{
		return validSlot(slot) ? g_cards[slot] : g_emptyCard;
	}

	void cardInput(int slot, const std::string& control, const std::string& input, const std::string& text)
	{
		if ( multiplayer == CLIENT || !isClaimed(slot) ) { return; }
		SamEvent e("lobby.on_card_input");
		e.s("mod", g_cards[slot].ns).i("slot", slot).s("control", control).s("input", input).s("text", text);
		e.fire();
	}

	void runSlotAction(int actionIndex, int slot)
	{
		if ( multiplayer == CLIENT || actionIndex < 0 || actionIndex >= (int)g_actions.size() || !validSlot(slot) ) { return; }
		const SlotAction a = g_actions[(std::size_t)actionIndex];   // a handler may register another
		SamEvent e("lobby.on_slot_action");
		e.s("mod", a.ns).s("action", a.id).i("slot", slot);
		e.fire();
	}

	// The hello a client says once the host has shown it runs S.A.M. A stock host gets nothing:
	// a host must look vanilla to a stock peer, and a client must look vanilla to a stock host.
	void sendHello()
	{
		if ( multiplayer != CLIENT || !g_open || g_helloSent ) { return; }
		if ( clientnum < 0 || clientnum >= MAXPLAYERS ) { return; }
		g_helloSent = true;
		std::string hello("SAML", 4);
		hello.push_back((char)Op::Hello);
		hello.push_back((char)PROTOCOL);
		hello.push_back((char)clientnum);
		sendFrame(0, net_server, hello);
	}

	void hostShownSam()
	{
		sendHello();
	}

	void lobbyOpened(bool loadingSave)
	{
		g_open = true;
		g_loadingSave = loadingSave;
		g_hostAnswered = false;
		g_helloSent = false;
		forgetCards();
		if ( multiplayer == CLIENT )
		{
			// Hello whether or not this machine has a mod loaded: the host's difficulty gate
			// (sam_party.hpp) asks whether every client can READ the start packet's tail, and
			// that is the framework, not a mod. But only to a host that has shown it runs
			// S.A.M (it sent its fingerprint); if that has not happened yet, hostShownSam()
			// sends it when it does.
			if ( SAMSync::hostShownSam() ) { sendHello(); }
			return;
		}
		// The host keeps what each connected client said: the connection outlives the lobby,
		// so a restarted run must not forget it (playerJoined clears a slot for a new one).
		SamEvent e("lobby.on_lobby_opened");
		e.i("loading_save", loadingSave ? 1 : 0).i("online", multiplayer == SERVER ? 1 : 0);
		e.fire();
	}

	void lobbyClosed()
	{
		g_open = false;
		g_loadingSave = false;
		g_hostAnswered = false;
		g_helloSent = false;
		forgetCards();
		// g_hello stays: see lobbyOpened.
	}

	void clear()
	{
		for ( int s = 1; s < MAXPLAYERS; ++s ) { release(s); }
		g_actions.clear();
	}

	void playerJoined(int slot)
	{
		if ( slot >= 0 && slot < MAXPLAYERS ) { g_hello[slot] = false; }
	}

	bool peerSaidHello(int slot)
	{
		return slot >= 0 && slot < MAXPLAYERS && g_hello[slot];
	}

	bool everyPeerSaidHello()
	{
		if ( multiplayer != SERVER ) { return true; }
		for ( int p = 1; p < MAXPLAYERS; ++p )
		{
			if ( !client_disconnected[p] && !g_hello[p] ) { return false; }
		}
		return true;
	}

	void noteSource(bool known, std::uint32_t host, std::uint16_t port)
	{
		g_srcKnown = known;
		g_srcHost = host;
		g_srcPort = port;
	}

	void receiveOnHost(const std::uint8_t* data, int len)
	{
		if ( multiplayer != SERVER || !data || len < 7 || len > NET_PACKET_SIZE ) { return; }
		std::uint8_t in[7];
		std::memcpy(in, data, 7);
		if ( in[4] != Op::Hello || in[5] < 1 ) { return; }
		const int slot = in[6];
		if ( !validSlot(slot) || client_disconnected[slot] ) { return; }
		// The slot is the sender's own claim, as in every vanilla packet. On a direct connection the
		// datagram's source settles it, so one client cannot say hello for another (a stock player
		// taken for a S.A.M one would build a dungeon for a party count it never hears of). Over
		// Steam or EOS there is no source to ask, the same limit SAMNet's own channel has.
		if ( g_srcKnown && net_clients && (g_srcHost != net_clients[slot - 1].host || g_srcPort != net_clients[slot - 1].port) )
		{
			SAM_WARN(MOD, "A lobby hello claimed to be from player " + std::to_string(slot) + " but came from somewhere else; ignored.");
			return;
		}
		g_hello[slot] = true;
		for ( int s = 1; s < MAXPLAYERS; ++s )
		{
			if ( !g_cards[s].ns.empty() ) { sendFrameTo(slot, cardFrame(s)); }
		}
		std::string done("SAML", 4);
		done.push_back((char)Op::Done);
		sendFrameTo(slot, done);
	}

	void receiveOnClient(const std::uint8_t* data, int len)
	{
		if ( multiplayer != CLIENT || !data || len < 5 || len > NET_PACKET_SIZE ) { return; }
		const std::string in((const char*)data, (std::size_t)len);
		const std::uint8_t op = (std::uint8_t)in[4];
		if ( op == Op::Done ) { g_hostAnswered = true; return; }
		if ( op != Op::Card || len < 10 ) { return; }
		const int slot = (std::uint8_t)in[5];
		if ( !validSlot(slot) ) { return; }
		const bool shown = in[6] != 0;
		const std::size_t n = ((std::size_t)(std::uint8_t)in[7] << 8) | (std::uint8_t)in[8];
		if ( 9 + n + 1 > in.size() ) { return; }   // a frame that says more follows than does
		const std::size_t nsLen = (std::uint8_t)in[9 + n];
		if ( 9 + n + 1 + nsLen > in.size() ) { return; }
		if ( shown )
		{
			Card c = parseDisplayText(in.substr(9, n));
			c.ns = in.substr(9 + n + 1, nsLen);
			if ( c.ns.empty() ) { c.ns = "?"; }
			g_cards[slot] = std::move(c);
		}
		else
		{
			g_cards[slot] = Card();
		}
		if ( g_ui.slotChanged ) { g_ui.slotChanged(slot); }
	}

	std::string displayText(const Card& c)
	{
		std::string t = c.title;
		for ( const Row& r : c.rows )
		{
			t.push_back('\n');
			if ( r.kind == Row::Kind::Buttons )
			{
				for ( std::size_t b = 0; b < r.labels.size(); ++b )
				{
					if ( b ) { t += "  |  "; }
					t += r.labels[b];
				}
			}
			else
			{
				t += r.text;
			}
		}
		return t;
	}

	Card parseDisplayText(const std::string& text)
	{
		Card c;
		std::size_t start = 0;
		bool first = true;
		while ( start <= text.size() )
		{
			std::size_t end = text.find('\n', start);
			if ( end == std::string::npos ) { end = text.size(); }
			const std::string line = text.substr(start, end - start);
			if ( first ) { c.title = clean(line, MAX_TITLE); first = false; }
			else if ( (int)c.rows.size() < MAX_ROWS )
			{
				Row r;
				r.kind = Row::Kind::Text;
				r.text = clean(line, 3 * MAX_TEXT + 10);
				c.rows.push_back(std::move(r));
			}
			start = end + 1;
		}
		return c;
	}
}
