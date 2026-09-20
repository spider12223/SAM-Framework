/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_mp_input.hpp
	Desc: keys, bound actions and client-raised engine events in multiplayer.

	THE PROBLEM. A mod's runtime code runs on the host, but a key press, an appraisal, a
	spell learned from a book or a "Use" all HAPPEN on the machine of the player who did
	them. Before this module each of those either fired only for the host's own player or
	fired on the wrong machine:

	  * a joiner's keyboard never produced on_key_pressed anywhere, splitscreen seats 1-3
	    never produced on_action_pressed, and sam_is_action_held(2, ...) on the host
	    answered with the HOST's mouse;
	  * appraisals, identify scrolls, spells learned and "not enough mana" by a joiner fired
	    on no machine at all;
	  * a joiner drank the potion before the host's on_item_use veto could say no, and an
	    equip restriction (on_before_equip) blocked only the host;
	  * on_before_revive ran on EVERY machine, on the level loader's worker thread, and each
	    machine applied its own answer to its own copy of the backpack.

	THE SHAPE. Everything below keeps the one-machine model: the event fires on the HOST,
	about the player it is about, and a handler names players by index. The owner's machine
	only reports what happened (SAMNet ordered channel, S.A.M-only ops), and waits for the
	host's answer where the answer decides what the owner does (a vetoable Use).

	Nothing here reaches a stock machine. A client sends only after the host has told it,
	on this session's channel, that it runs S.A.M (the READY op); a host only sends to a
	client that said HELLO. With no script loaded, none of this runs at all.

	WIRE (op numbers in SAMNet's blocks; the two directions are separate spaces):
	  client -> host  InputFirst+0  EDGES   [u8 n] n x ([u8 kind 0 key/1 action][u8 down][str8 name][str8 binding])
	  client -> host  InputFirst+1  STATE   [u8 nKeys][str8 key]... [u8 nActions]([str8 action][u8 held][str8 binding])...
	  host -> client  InputFirst+0  READY   [u16 version]
	  client -> host  EventFirst+0  USE     [u32 uid][u32 type][u32 status][u32 beatitude][u32 count][u32 appearance][u8 identified]
	  host -> client  EventFirst+0  VERDICT [u32 uid][u8 allow]
	  host -> client  EventFirst+1  UNPING  [u32 callout key]
	  (on_item_identified, on_spell_learned and on_spell_failed ride SAMNet's own Event op.)

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

class Item;

namespace SAMMpInput
{
	// ------------------------------------------------------------------ keys and actions

	// Is this player's input on another machine, as seen from here? Unlike SAMNet::isRemotePlayer
	// this is also true on the host for a slot that just disconnected: Input::inputs[] answers
	// every index but 0 with THIS machine's buttons in a netgame, so such a slot must never
	// fall through to it.
	bool onOtherMachine(int player);

	// sam_is_key_held(key [, player]). `player` < 0 means "not given": the player the current
	// event is about, else this machine's own (the default the contract check used). The host
	// answers for a player on another machine from what that machine reported: A-Z, 0-9 and
	// F1-F12, the keys on_key_pressed fires for. A local player reads this machine's keyboard.
	bool keyHeld(const std::string& name, int player);

	// Is `action` held by `player`, who is on another machine? On the host, from that machine's
	// reports (false, with one warning, for a player whose game does not run S.A.M). On a
	// client, always false: a client only knows its own buttons.
	bool remoteActionHeld(int player, const std::string& action);

	// The binding `player` (on another machine) has for `action`, as that machine reported it to
	// the host; "" when unknown, and always "" on a client. Valid until the next report.
	const char* remoteActionBinding(int player, const std::string& action);

	// The local player the keyboard belongs to: in splitscreen the player the game gave the
	// keyboard to, otherwise this machine's own player. pollInput stamps its events with this.
	int keyboardPlayer();

	// Host and singleplayer, every game tick (SAMLua::pollActions calls it): fire
	// on_action_pressed / on_action_released for EVERY player whose buttons are on this
	// machine -- splitscreen players 1-3 included, each with their own edges and their own
	// binding. A client does nothing here: its tick hook reports its player's buttons to the
	// host instead, so nothing is sent as a 'SAMA' any more.
	void pollLocalActions();

	// Host: a 'SAMA' edge from a client on an older S.A.M build (this build reports buttons on
	// the ordered channel and never sends SAMA). Dispatched as before, and ignored from a
	// client that said HELLO, so an action can never fire twice.
	void onLegacyAction(int player, int actionIndex, bool pressed);

	// ------------------------------------------------------------------ messages

	// sam_message's body, both runtimes. A joiner's game reads a few message TEXTS as orders
	// (Barony's own "you survive through your party's persistence" halves their HP and MP,
	// sets hunger and clears their effects), so a mod line that happened to equal one did that
	// to the joiner alone. A line for a player on another machine goes out with one leading
	// space: still a vanilla message, which a stock client shows, but never equal to those.
	void scriptMessage(int player, const std::string& text);

	// ------------------------------------------------------------------ client-raised events

	// player.on_item_identified. Call where an item becomes identified, with the player whose
	// item it is. Fires on the host; a client reports its own player's to the host. On the host,
	// a player on another machine whose game runs S.A.M reports it themselves, so the host's
	// equipped COPY of their gear does not count the same identification a second time.
	void reportItemIdentified(int player, const Item* item);

	// player.on_spell_learned, from addSpell (which only ever runs for a local player).
	void reportSpellLearned(int player, int spellId, const char* spellName);

	// player.on_spell_failed from the caster's own machine (castSpellInit's mana check).
	void reportSpellFailed(int player, int spellId, const char* spellName, const char* reason);

	// ------------------------------------------------------------------ Use and equip

	// Client: a deliberate Use by this machine's player, while the host runs S.A.M. Asks the
	// host first and returns true: the caller must return without doing anything. The host
	// fires player.on_item_use, answers, and on "allow" this module runs useItem again for the
	// same item (found by uid), which then goes ahead and tells the host with USEI as usual.
	// False everywhere else, and for that re-run: the caller carries on exactly as before.
	bool askHostBeforeUse(Item* item, int player);

	// Host: this deliberate USEI is the one the host already approved (and fired
	// player.on_item_use for) when the client asked. Consumes the approval. `item` is the copy the
	// USEI handler just built: the approval is matched on type, status, blessing, appearance and
	// identified -- everything the vanilla packet carries except the count, which can change
	// between the question and the use. The packet has no uid to match on.
	bool takeApprovedUse(int player, const Item* item);

	// Host: a vetoed deliberate Use by a player on another machine. `item` is the copy the USEI
	// handler built in the host's list for that player; nothing else holds it, so free it
	// rather than leave a phantom item in the host's list (and the host's save).
	void dropVetoedUseCopy(int player, Item* item);

	// player.on_before_equip, decided on the machine of the player who is equipping, before
	// anything is equipped or sent. False = a handler refused. Nothing is said to the player: the
	// handler is what knows why, and on a joiner's machine it must say it with sam_hud_text
	// (sam_message and sam_play_sound are host-kind and refused there).
	bool playerMayEquip(int player, const Item* item);

	// ------------------------------------------------------------------ revive

	// Host and singleplayer, MAIN thread, before the level-change packets go out: fire
	// player.on_before_revive once for each connected player who is dead, and remember
	// the answers for this floor load.
	void decideFloorRevives();
	// Host: append the answers to an LVLC/LVLR packet whose map name (possibly empty) ends
	// at data[len-1], and return the new length. Appends nothing when nobody was refused, so
	// the packet stays byte-identical to vanilla; a stock client stops at the name's NUL.
	// The answers stay attached to the floor, so the 3-second LVLC reminder can carry them
	// too, to a client that missed the level change and loads the floor from the reminder.
	int appendReviveVerdict(unsigned char* data, int len);
	// Client: read those answers from an LVLC/LVLR, before its level load starts.
	void readReviveVerdict(const unsigned char* data, int len);
	// Every machine: forget this floor's revive answers and go back to the vanilla "everyone is
	// allowed". The first level load of a run is never asked -- there is no level-change packet at
	// that point to carry an answer to the clients on, and asking only on the host would revive on
	// the clients and not on the host -- so the answers must not still be here when doNewGame's own
	// loads call assignActions, or the first floor of a run applies the last answer of the previous
	// one. Three things now guarantee that: the host forgets at fireFloorRevives, a client forgets
	// on its first tick after each load (the answers are consumed inside the level-change packet
	// handler, before any tick), and reviveAllowed answers the vanilla `true` whenever nothing was
	// decided for the current load. Call this too wherever a run boundary is being reset.
	void resetReviveVerdicts();
	// assignActions, any machine, any thread: was this dead player's revive allowed? True whenever
	// nobody was asked for this load, which is the vanilla answer and the one every machine gives.
	bool reviveAllowed(int player);
	// assignActions, any machine, any thread: the loader stood a body up for `player` although
	// their revive was refused, at this position and facing. On a CLIENT this is what makes the
	// next main-thread tick give a local player of its own a camera to watch the floor from; the
	// host does the same thing from actPlayer, so it ignores this. Nothing is announced and no
	// event fires: the death itself happened on the floor they died on.
	void noteKeptDead(int player, double x, double y, double yaw);
	// Host and singleplayer, from actPlayer: is this the FIRST time this player's death procedure
	// has run since they were last alive? A death that a mod refused to revive
	// (player.on_before_revive) arrives on the next floor still at 0 HP, so assignActions stands
	// the body up and the whole procedure runs again -- a second player.on_death carrying the old
	// floor's obituary, a second set of gibs, the backpack bagged a second time -- once per floor
	// for the rest of the run. True once per death; the answer is armed again by notePlayerAlive.
	// Always true when no script is loaded: a vanilla game cannot refuse a revive.
	bool claimPlayerDeath(int player);
	// actPlayer, every tick a player is alive and connected: arms claimPlayerDeath again.
	void notePlayerAlive(int player);
	// assignActions, host: this player was revived by the floor load. Recorded only; the
	// event fires from fireFloorRevives on the main thread, never on the loader thread.
	void noteFloorRevived(int player);
	// Host and singleplayer, MAIN thread, after the level load: fire player.on_player_revived
	// (revive_type "floor_load") for everyone noted, then forget this floor's answers.
	void fireFloorRevives();
	// Host: fire player.on_player_revived now (ghost respawn, a script revive, ...).
	void firePlayerRevived(int player, const char* reviveType);

	// ------------------------------------------------------------------ players leaving

	// Host: a player joined `player`'s slot (lobby join). Re-arms that slot's leave event.
	void notePlayerJoined(int player);
	// Host: `player`'s slot was just marked disconnected (left, dropped, kicked; lobby or game).
	// Fires player.on_player_left once per stay, however many paths notice the same leave.
	// Call it BEFORE anything clears stats[player]->name.
	void firePlayerLeft(int player);
	// Host: tell every other connected client that `player` is gone (vanilla DISC), for the
	// paths that used to tell only the player being dropped. Skipped when no script is
	// loaded, so a modless host's wire stays exactly as it was.
	void relayPlayerGone(int player);

	// ------------------------------------------------------------------ once per run

	// game.on_game_end from a party wipe: true the first time in a run, false after that.
	bool claimPartyWipe();
	// A new run starts (doNewGame, every machine).
	void resetRun();

	// ------------------------------------------------------------------ callouts

	// Host: a handler vetoed `player`'s ping. The pinging machine drew its marker before it
	// asked, so ask it to take the marker down again (S.A.M clients; a stock client keeps it).
	// `key` is the uid the host keyed the callout by.
	void retractCallout(int player, unsigned int key);
}
