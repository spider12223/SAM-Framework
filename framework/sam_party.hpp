/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_party.hpp
	Desc: party members -- a monster on the players' side the way a player is.

	WHY THIS EXISTS

	Barony already has allies: a recruited follower, a summon, a bot. Each of them is
	somebody's PET. It counts against its leader's follower cap, turns hostile or vanishes
	when its leader dies, gets a monster's random gear and a monster's random level-ups,
	and the game never waits for it at an exit. A mod that wants a companion who is part of
	the party -- who starts with a class's kit, grows like that class, stays loyal when the
	player it follows goes down, and is someone the stairs wait for -- has nowhere to stand.
	sam_spawn_companion is a different thing (a scripted model that walks beside a player);
	this is a real monster with a real Stat that fights, levels and dies.

	sam_party_spawn(leader, { name, race, class, sex, appearance, tag, ... }) makes one:

	  * Stats: the same baseline a player starts from, then initClassStats for its class
	    (so a S.A.M custom class's stats, passives and overrides apply through the same hook
	    they do for a player), and a custom race's deltas through the same tail.
	  * Kit: a vanilla class's starting items (sam_party_loadouts.inc, generated from
	    charclass.cpp), or a custom class's starting_items, equipped where a player would.
	  * Growth: level-ups go through playerStatIncrease for its class instead of the monster
	    roll (option grow_like_class).
	  * Loyalty: it is not a recruit (no follower cap), it stays on the players' side when
	    its leader dies, and it crosses floors as a follower does.
	  * Exits: with wait_at_exit, every exit waits until it is within 4 tiles, sends it
	    there, and says who is missing. party.on_waiting_at_exit can let the party go.
	  * Death: with respawn, it comes back beside its leader on the next floor.
	    party.on_member_died can say no (a permadeath mode).
	  * Difficulty: a mod can ask for N party members to count as players when the floor is
	    built, the way Barony counts connected players (sam_party_set_difficulty_count).

	THE MARKER

	Stat::MISC_FLAGS[SAM_STAT_FLAG_PARTY] = ('P' << 24) | flags. MISC_FLAGS is copied by
	copyStats and written into a follower's save, so the marker crosses floors and saves with
	no code of ours, and testing it is one integer compare -- checkEnemy and checkFriend ask
	it on every AI decision. The class (a custom one is an id >= 1000, too wide for the
	marker), the race, the tag and the owning mod live in Stat::attributes under "sam.party.*",
	all under the 31-character limit a follower save keeps. Vanilla never uses index 30.

	DIFFICULTY WITHOUT A DESYNC

	Every machine generates each floor itself from a shared seed, and the number of monsters
	depends on how many players are connected. Counting party members on the host alone would
	give a stock client a different dungeon. So the host decides at startGame (and at both
	in-game restarts, beginRun), and uses the mod's count only if EVERY connected client said
	hello on 'SAML' (sam_lobby.hpp), i.e. runs a S.A.M that reads the count. The number then
	rides a 4-byte tail on vanilla 'STRT' / 'RSTR' ('S' 'P' version count, after byte 17), the
	same technique as the class-id tail on 'JOIN'. A stock client reads 17 bytes and stops; a
	S.A.M client reads the tail before it builds anything. Otherwise the run is balanced as
	vanilla and the host's log says why.

	HOST AUTHORITY. Monsters are the host's: everything that changes a party member is host
	only. The engine hooks below are no-ops for a non-member (one compare) and on a client.

	Game build only (GAME_SOURCES): calls entity and monster code.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>

#include "sam_value.hpp"

class Entity;
class Stat;

namespace SAMParty
{
	// The MISC_FLAGS index. If your tree already uses 30, change it here; nothing else
	// depends on the number.
	constexpr int SAM_STAT_FLAG_PARTY = 30;
	constexpr std::uint32_t MARKER = (std::uint32_t)'P';

	constexpr std::uint32_t FLAG_MEMBER = 1u << 0;
	constexpr std::uint32_t FLAG_WAIT_AT_EXIT = 1u << 1;
	constexpr std::uint32_t FLAG_RESPAWN = 1u << 2;
	constexpr std::uint32_t FLAG_GROW_LIKE_CLASS = 1u << 3;

	constexpr int MAX_NAME = 31;     // Stat::name is 32 bytes with its terminator
	constexpr int MAX_TAG = 31;      // a follower save keeps 31 characters of an attribute
	constexpr int ASSEMBLE_RANGE = 64;   // 4 tiles: how close a waiting member must be to an exit
	constexpr std::uint8_t TAIL_VERSION = 1;

	// ---- the script side (host; each refusal returns 0 / false with the reason in *why)

	// spec: { name, race, class, sex, appearance, tag, wait_at_exit, respawn, grow_like_class,
	//         pickup }. race and class are an id string ("human", "warrior", "mymod:knight")
	// or a number. Returns the new member's uid, or 0.
	std::uint32_t spawn(int leader, const SamValue& spec, std::string* why, const std::string& ns = std::string());

	bool isMemberUid(std::uint32_t uid);
	// { uid, name, class, class_id, race, race_id, sex, appearance, leader, tag, mod, level,
	//   hp, max_hp, wait_at_exit, respawn, grow_like_class }, or nil for a non-member.
	SamValue info(std::uint32_t uid);
	// options: any of { wait_at_exit, respawn, grow_like_class, tag }.
	bool setOptions(std::uint32_t uid, const SamValue& options, std::string* why);
	SamValue list();      // info() for every living member on this floor
	SamValue fallen();    // { name, class, race, tag, mod, leader } for each waiting to come back

	// How many party members the next run should count as players (0..MAXPLAYERS-1).
	// Read by beginRun on the host; reset when a lobby opens.
	bool setDifficultyCount(int count, std::string* why);

	// What THIS machine can offer: { classes = { { id, name }, ... }, races = { ... },
	// appearances = n }. DLC and unlocks are per machine, so choiceAllowed is too.
	SamValue choices();
	// 1 allowed, 0 not (the character screen's rule), -1 an unknown race or class.
	int choiceAllowed(const std::string& race, const std::string& cls);
	std::string randomName(const std::string& sex);   // "male" / "female"; anything else picks one

	// ---- the engine side

	bool isMember(const Stat* myStats);
	bool isMember(Entity* entity);
	std::uint32_t flags(const Stat* myStats);

	// checkEnemy / checkFriend, right after the everybodyfriendly early-out:
	//   int r = SAMParty::allegiance(this, myStats, your, yourStats, askingIfEnemy);
	//   if ( r >= 0 ) { return r == 1; }
	// -1 (no opinion) unless a party member is on one side, and also while that member's
	// leader is alive: then the normal follower path decides, masks and confusion included.
	int allegiance(Entity* me, Stat* myStats, Entity* you, Stat* yourStats, bool askingIfEnemy);

	// actMonster's death block, BEFORE the gear drops (Keep Inventory decides which side keeps it).
	void onDeath(Entity* monster, Stat* myStats);
	// gameLogic's level change, right after the followers are placed.
	void respawnFallen();
	// Each exit, after vanilla's "assemble your party" check passes. False: not yet.
	bool exitsMayClose(Entity* exitEntity, int player);
	// Every player is down but a party member still stands (host or solo; false on a client).
	// The game-over window then offers Dismiss, the fallen wait as ghosts, and the members make
	// for the exit: the run goes on, and the fallen stand up on the next floor as in co-op.
	bool partyCarriesOn();
	// actLadder / actPortal / actMidGamePortal / actCustomPortal: the player to act for when
	// partyCarriesOn() and a living member is at this exit, else -1.
	int exitUser(Entity* exitEntity);
	// actMonster, once a second per monster: a member heads for the nearest exit while
	// partyCarriesOn(); when the last one falls with every player down, the run ends.
	void think(Entity* monster, Stat* myStats);
	// The follower menu: true for a party member (no leadership gate on its orders). Works on
	// a client too: the host sends the floor's party-member uids (NetOp::PartyMembers).
	bool takesOrdersFreely(Entity* follower);
	// handleEffects' monster level-up branch. True when it raised the stats like a player.
	bool levelUpLikePlayer(Entity* monster, Stat* myStats, int increasestat[3]);

	// The count maps.cpp, getHungerTickRate and the Devil use in place of their own
	// connected-player loop: connected players plus this run's party count, at most MAXPLAYERS.
	// Equal to the vanilla loop whenever the count is 0. The same on every machine.
	int balancePlayers();
	int runDifficultyCount();

	void lobbyOpened();                 // createLobby: forget the last lobby's count
	void beginRun();                    // host, before 'STRT'/'RSTR' is sent
	void writeStartTail(std::uint8_t* data, int& len);   // after `net_packet->len = 17;`
	void readStartTail(const std::uint8_t* data, int len); // client 'STRT' / 'RSTR'
	void clear();                       // doNewGame (host): forget the fallen and the exit tries
}
