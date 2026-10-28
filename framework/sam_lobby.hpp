/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_lobby.hpp
	Desc: mod-owned lobby slots. A mod puts a link on every empty player slot
	      ("+ Add Companion"), and a card of its own in a slot it claims.

	WHY THIS EXISTS

	The lobby is the one screen a mod could not touch. A mod that adds a party member, a
	hireling, or a pet that starts the run with you has to let the host choose it BEFORE
	the run, and the place a player expects to choose a party member is a player slot.
	Before this, such a mod had to invent a key press at the main menu and keep its choice
	somewhere the other players could not see.

	THE MODEL: EVENTS AND PLAIN DATA, NOT CALLBACKS

	A card is a description: a title and up to five rows. A row is a line of text, a
	cycle (text with "<" and ">"), or one to three buttons. Every press on the host fires
	lobby.on_card_input { mod, slot, control, input, text }, and the mod redraws by calling
	sam_lobby_set_card again with the new description. Nothing per-mod lives here except
	the card itself, so this works the same in Lua and in JavaScript, and nothing holds a
	script function across a mod reload.

	    sam_lobby_add_slot_action("add", "+ Add Companion")   -- top level, every machine
	    lobby.on_slot_action { mod, action, slot }            -- the host pressed it
	    sam_lobby_set_card(slot, { title = "Brienne", edit_title = true, rows = {
	        { cycle = "race", text = "Human" },
	        { buttons = { "sex", "look" }, labels = { "Female", "Look 3" } },
	        { text = "Follows Kyle" } } })
	    lobby.on_card_input { mod, slot, control, input, text }  -- input: "prev", "next",
	                                                             -- "click" or "text"

	WHO SEES WHAT

	  * The HOST draws the card with its controls. Only the host can set or clear one
	    (contract: host), and only in a slot with no player in it that the lobby has open.
	  * A claimed slot is LOCKED, through vanilla 'LOCK', whose handler every client already
	    has (vanilla never sends it after the join, but every build handles it). So a STOCK
	    client draws a locked slot and no human can join into a slot a mod is using.
	  * A S.A.M client says hello on 'SAML' when its lobby opens. The host answers with every
	    card (as display text) and DONE, and from then on sends that client every change. It
	    draws the card read-only. A client that never says hello is never sent 'SAML' at all,
	    following sam_sync.hpp's rule that a host must look vanilla to a stock peer.

	WIRE ('SAML', lobby only, reliable via sendPacketSafe):

	  client -> host   [4] 1 HELLO  [5] protocol  [6] the sender's slot
	  host -> client   [4] 2 CARD   [5] slot  [6] 1 shown / 0 gone  [7..8] text length (BE)
	                   [9..] display text  [..] namespace length (1 byte), namespace
	                   [4] 3 DONE   (every card has been sent)

	The slot in a hello is the sender's own claim -- the same trust 'SAMF' and every
	vanilla lobby packet give it. A lie only adds a slot to the ones sent the host's cards
	(which are public: every player can see them), or marks a stock client as S.A.M for the
	difficulty gate in sam_party.hpp. For friends-only co-op that is acceptable.

	A stock 5.0.2 host logs one "mystery packet" line for a client's hello.

	Game build only (GAME_SOURCES): calls net code.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sam_value.hpp"

namespace SAMLobby
{
	// ---- limits (the card has to fit a 280x146 stone and a 512-byte packet)
	constexpr int MAX_ROWS = 5;
	constexpr int MAX_BUTTONS = 3;
	constexpr int MAX_TITLE = 31;        // a Stat name is 32 bytes with its terminator
	constexpr int MAX_TEXT = 40;         // a row's text, a button's label, an action's label
	constexpr int MAX_ID = 32;           // control and action ids: [A-Za-z0-9_.-]
	constexpr int MAX_ACTIONS = 4;       // registered, across all mods
	constexpr int SHOWN_ACTIONS = 2;     // drawn on one empty slot
	constexpr std::uint8_t PROTOCOL = 1;

	namespace Op
	{
		constexpr std::uint8_t Hello = 1;
		constexpr std::uint8_t Card = 2;
		constexpr std::uint8_t Done = 3;
	}

	struct Row
	{
		enum class Kind { Text, Cycle, Buttons };
		Kind kind = Kind::Text;
		std::string control;                 // Cycle: the control id its arrows send
		std::string text;                    // Text, Cycle
		std::vector<std::string> buttons;    // Buttons: control ids
		std::vector<std::string> labels;     // Buttons: what each says (defaults to the id)
	};

	struct Card
	{
		std::string ns;                      // the mod that owns the slot ("" = unclaimed)
		std::string title;
		bool editTitle = false;              // host: the title is a text box, sent as control "title"
		std::vector<Row> rows;
	};

	struct SlotAction
	{
		std::string ns, id, label;
	};

	// The engine half (ui/MainMenu.cpp installs it). Every member may be null.
	struct Ui
	{
		void (*slotChanged)(int slot) = nullptr;               // redraw that slot's stone
		bool (*slotOpen)(int slot) = nullptr;                  // may a mod use this empty slot?
		void (*setSlotLocked)(int slot, bool locked) = nullptr; // and tell every client ('LOCK')
		void (*chat)(const char* text) = nullptr;              // a line in the lobby chat
	};
	void setUi(const Ui& ui);

	// ---- the script side (each returns false with the reason in *why)
	bool addSlotAction(const std::string& ns, const std::string& id, const std::string& label, std::string* why);
	// The actions to draw. Empty on a client: only the host's links do anything.
	const std::vector<SlotAction>& slotActions();

	bool setCard(const std::string& ns, int slot, const SamValue& card, std::string* why);
	bool clearCard(const std::string& ns, int slot, std::string* why);

	// { state = "player", name } | { state = "card", mod, title } | { state = "open" } |
	// { state = "closed" }; nil for a slot that does not exist.
	SamValue slotInfo(int slot);

	// ---- the engine side
	bool isClaimed(int slot);
	const Card& card(int slot);                              // an empty Card when unclaimed

	// The host pressed something. Fire the events; nothing happens for an unclaimed slot.
	void cardInput(int slot, const std::string& control, const std::string& input, const std::string& text);
	void runSlotAction(int actionIndex, int slot);

	// createLobby, once the stones exist. Host: fires lobby.on_lobby_opened. Client: hello.
	void lobbyOpened(bool loadingSave);
	// doNewGame, on every machine: the lobby is gone, forget its cards and peers.
	void lobbyClosed();
	// Mods were loaded or unloaded: give every slot back and forget every action.
	void clear();

	// A client just joined into `slot` (it has not said hello yet).
	void playerJoined(int slot);
	bool peerSaidHello(int slot);
	// Every connected client has said hello (true in singleplayer and with no clients).
	bool everyPeerSaidHello();

	// 'SAML' handlers: serverPacketHandlers / clientPacketHandlers.
	void receiveOnHost(const std::uint8_t* data, int len);
	void receiveOnClient(const std::uint8_t* data, int len);

	// What a client is sent for a card, and the read-only card it draws from it: the title,
	// then one line per row (a cycle's text, a row of buttons as its labels joined by "  |  ").
	std::string displayText(const Card& card);
	Card parseDisplayText(const std::string& text);

	// [A-Za-z0-9_.-], 1..MAX_ID characters.
	bool validId(const std::string& id);
}
