/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_party.cpp
	Desc: party members. See sam_party.hpp.

-------------------------------------------------------------------------------*/

// Must precede every include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_party.hpp"
#include "sam_lobby.hpp"
#include "sam_event.hpp"
#include "sam_logger.hpp"
#include "sam_classes.hpp"
#include "sam_races.hpp"
#include "sam_catalog.hpp"
#include "sam_net.hpp"

#include "main.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "entity.hpp"
#include "monster.hpp"
#include "items.hpp"
#include "net.hpp"
#include "player.hpp"
#include "menu.hpp"         // isCharacterValidFromDLC
#include "classdescriptions.hpp" // NUMAPPEARANCES
#include "prng.hpp"
#include "interface/interface.hpp"
#include "paths.hpp"         // GeneratePathTypes
#include "ui/MainMenu.hpp"   // openGameoverWindow, when the last member standing falls
#include "ui/GameUI.hpp"     // gameUIFrame
#include "ui/Frame.hpp"

void samPartyWiped();   // actplayer.cpp: the party that carried the run on is gone: the save, the music, the scores

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

static const char* MOD = "PARTY";

namespace
{
	const char* const ATTR_CLASS = "sam.party.class";
	const char* const ATTR_RACE = "sam.party.race";
	const char* const ATTR_TAG = "sam.party.tag";
	const char* const ATTR_MOD = "sam.party.mod";

	// ---- the vanilla kits (generated from charclass.cpp initClass) -------------------------

	struct LoadoutEntry
	{
		int classnum;
		ItemType type;
		Status status;
		Sint16 beatitude;
		Sint16 count;
		bool identified;
		bool equip;
	};

	const LoadoutEntry kLoadouts[] = {
#include "sam_party_loadouts.inc"
	};

	// ---- per-run state -------------------------------------------------------------------------

	int g_askedCount = 0;   // host: what a mod asked for in this lobby
	int g_runCount = 0;     // every machine: what this run counts (the host decided, the tail carried)

	struct Fallen
	{
		Stat* stats = nullptr;   // owned until respawnFallen hands it to a new monster
		int leader = 0;
	};
	std::vector<Fallen> g_fallen;

	// Exit uid -> how many times a player has tried it with someone missing. Forgotten every floor.
	std::map<Uint32, int> g_exitTries;

	void say(std::string* why, const std::string& text)
	{
		if ( why ) { *why = text; }
	}

	std::string lower(std::string s)
	{
		for ( char& c : s ) { c = (char)std::tolower((unsigned char)c); }
		return s;
	}

	std::string attr(const Stat* s, const char* key)
	{
		if ( !s ) { return std::string(); }
		auto it = s->attributes.find(key);
		return it != s->attributes.end() ? it->second : std::string();
	}

	void setAttr(Stat* s, const char* key, const std::string& value)
	{
		if ( s ) { s->attributes[key] = value.substr(0, 31); }
	}

	void setFlags(Stat* s, std::uint32_t f)
	{
		if ( !s ) { return; }
		s->MISC_FLAGS[SAMParty::SAM_STAT_FLAG_PARTY] = f ? (Sint32)((SAMParty::MARKER << 24) | (f & 0xFFFFu)) : 0;
	}

	bool livingPlayer(int p)
	{
		return p >= 0 && p < MAXPLAYERS && !client_disconnected[p] && players[p] && players[p]->entity;
	}

	// ---- names ---------------------------------------------------------------------------------

	// The race numbers the character screen offers, in its order.
	const int kPlayableRaces[] = {
		RACE_HUMAN, RACE_SKELETON, RACE_VAMPIRE, RACE_SUCCUBUS, RACE_GOATMAN, RACE_AUTOMATON,
		RACE_INCUBUS, RACE_GOBLIN, RACE_INSECTOID, RACE_GNOME, RACE_GREMLIN, RACE_DRYAD,
		RACE_MYCONID, RACE_SALAMANDER,
	};

	bool playableVanillaRace(int race)
	{
		for ( int r : kPlayableRaces ) { if ( r == race ) { return true; } }
		return false;
	}

	std::string raceId(int race)
	{
		const std::string n = SAMRaces::canonicalName(race);
		return n.empty() ? std::string("race") + std::to_string(race) : lower(n);
	}

	std::string raceName(int race)
	{
		if ( race >= SAM_RACE_ID_BASE )
		{
			const std::string n = SAMRaces::displayName(race);
			return n.empty() ? raceId(race) : n;
		}
		// ui/MainMenu.cpp's getLangEntryForPlayerRaceName, which is file-local there.
		int entry = 5369;
		if ( race > RACE_IMP && race < NUMRACES ) { entry = (race - RACE_IMP - 1) + 6779; }
		else if ( race >= 0 && race < 9 ) { entry = 5369 + race; }
		return Language::get(entry);
	}

	std::string classId(int cls)
	{
		if ( cls >= SAM_CLASS_ID_BASE )
		{
			const SAMClassDef* def = SAMClasses::getClass(cls);
			return def ? def->id : std::string();
		}
		const char* n = (cls >= 0 && cls < NUMCLASSES) ? playerClassLangEntry(cls, 0) : nullptr;
		return n ? lower(n) : std::string();
	}

	std::string className(int cls)
	{
		if ( cls >= SAM_CLASS_ID_BASE )
		{
			const SAMClassDef* def = SAMClasses::getClass(cls);
			return def ? (def->name.empty() ? def->id : def->name) : std::string();
		}
		// The language file keeps vanilla class names in lower case ("warrior"); the character
		// screen capitalises them where it draws them, so a name for display does the same.
		const char* n = (cls >= 0 && cls < NUMCLASSES) ? playerClassLangEntry(cls, 0) : nullptr;
		std::string s = n ? std::string(n) : std::string();
		if ( !s.empty() ) { s[0] = (char)std::toupper((unsigned char)s[0]); }
		return s;
	}

	// "human", "mymod:lizardfolk", or a number. -1 when it names nothing playable here.
	int resolveRace(const SamValue* v)
	{
		if ( !v || v->isNil() ) { return RACE_HUMAN; }
		if ( v->isNumber() )
		{
			const int r = v->type == SamValue::Type::Int ? (int)v->i : (int)v->n;
			if ( playableVanillaRace(r) ) { return r; }
			if ( r >= SAM_RACE_ID_BASE && SAMRaces::get(r) ) { return r; }
			return -1;
		}
		if ( v->type != SamValue::Type::Str ) { return -1; }
		const std::string want = lower(v->s);
		for ( int r : kPlayableRaces )
		{
			if ( raceId(r) == want || lower(raceName(r)) == want ) { return r; }
		}
		const int custom = SAMRaces::raceIdForIdString(v->s);
		if ( custom >= 0 ) { return custom; }
		for ( int k = 0; k < SAMRaces::count(); ++k )
		{
			const int r = SAMRaces::raceIdAtIndex(k);
			if ( r >= 0 && lower(raceId(r)) == want ) { return r; }
		}
		return -1;
	}

	int resolveClass(const SamValue* v)
	{
		if ( !v || v->isNil() ) { return CLASS_WARRIOR; }
		if ( v->isNumber() )
		{
			const int c = v->type == SamValue::Type::Int ? (int)v->i : (int)v->n;
			if ( c >= 0 && c < NUMCLASSES ) { return c; }
			if ( c >= SAM_CLASS_ID_BASE && SAMClasses::getClass(c) ) { return c; }
			return -1;
		}
		if ( v->type != SamValue::Type::Str ) { return -1; }
		const int found = SAMClasses::classIdForIdString(v->s);
		if ( found >= 0 ) { return found; }
		// The id choices() hands out, if the language table and SAM's own lookup ever differ.
		const std::string want = lower(v->s);
		for ( int c = 0; c < NUMCLASSES; ++c )
		{
			if ( classId(c) == want ) { return c; }
		}
		return -1;
	}

	Monster monsterForRace(int race)
	{
		if ( race >= SAM_RACE_ID_BASE ) { return (Monster)SAMRaces::hostMonsterForRace(race); }
		const Monster m = getMonsterFromPlayerRace(race);
		return m == NOTHING ? HUMAN : m;
	}

	// ---- the kit -------------------------------------------------------------------------------

	// The equipment slot a player would put this item in; nullptr = carry it.
	Item** equipSlotFor(Stat* s, const Item* item)
	{
		switch ( checkEquipType(item) )
		{
			case TYPE_HELM:
			case TYPE_HAT:          return &s->helmet;
			case TYPE_BREASTPIECE:  return &s->breastplate;
			case TYPE_BOOTS:        return &s->shoes;
			case TYPE_SHIELD:
			case TYPE_OFFHAND:      return &s->shield;
			case TYPE_GLOVES:       return &s->gloves;
			case TYPE_CLOAK:        return &s->cloak;
			case TYPE_RING:         return &s->ring;
			case TYPE_AMULET:       return &s->amulet;
			case TYPE_MASK:         return &s->mask;
			case TYPE_SWORD:
			case TYPE_AXE:
			case TYPE_SPEAR:
			case TYPE_MACE:
			case TYPE_BOW:
			case TYPE_PROJECTILE:   return &s->weapon;
			default: break;
		}
		if ( itemTypeIsFoci(item->type) || item->type == TOOL_DUCK
			|| item->type == TOOL_TINKERING_KIT || item->type == TOOL_FRYING_PAN )
		{
			return &s->shield;
		}
		switch ( itemCategory(item) )
		{
			case WEAPON:
			case MAGICSTAFF:
			case THROWN:
			case SPELLBOOK:
			case TOOL:   return &s->weapon;
			case RING:   return &s->ring;
			case AMULET: return &s->amulet;
			default: break;
		}
		return nullptr;
	}

	void give(Entity* monster, Stat* s, Item* item, bool equip)
	{
		if ( !item ) { return; }
		if ( equip )
		{
			Item** slot = equipSlotFor(s, item);
			if ( slot && *slot == nullptr ) { *slot = item; return; }
		}
		monster->addItemToMonsterInventory(item);
	}

	Status statusFromName(const std::string& n)
	{
		const std::string u = lower(n);
		if ( u == "broken" ) { return BROKEN; }
		if ( u == "decrepit" ) { return DECREPIT; }
		if ( u == "worn" ) { return WORN; }
		if ( u == "excellent" ) { return EXCELLENT; }
		return SERVICABLE;
	}

	void applyKit(Entity* monster, Stat* s, int cls)
	{
		if ( cls >= SAM_CLASS_ID_BASE )
		{
			const SAMClassDef* def = SAMClasses::getClass(cls);
			if ( !def ) { return; }
			for ( const SAMStartingItem& si : def->startingItems )
			{
				const int type = SAMCatalog::itemTypeFor(si.type);
				if ( type < 0 )
				{
					SAM_WARN(MOD, "class [" + def->id + "]: starting item '" + si.type + "' is not a known item; a party member of this class goes without it.");
					continue;
				}
				give(monster, s, newItem((ItemType)type, statusFromName(si.status), (Sint16)si.beatitude,
					(Sint16)std::max(1, si.count), (Uint32)si.appearance, si.identified, nullptr), si.equip);
			}
			// (the class's gold is already in: initClassStats applies it through SAMClasses::applyStats)
			return;
		}
		for ( const LoadoutEntry& e : kLoadouts )
		{
			if ( e.classnum != cls ) { continue; }
			give(monster, s, newItem(e.type, e.status, e.beatitude, e.count, local_rng.rand(), e.identified, nullptr), e.equip);
		}
	}

	// ---- following -----------------------------------------------------------------------------

	// Make a monster a follower of a player, with the tint and the ally state recruits get.
	bool bindToLeader(Entity& leader, Entity& monster, Stat& s)
	{
		const int leaderNum = leader.skill[2];
		if ( !forceFollower(leader, monster) ) { return false; }
		monster.monsterAllyIndex = leaderNum;
		monster.flags[USERFLAG2] = true;
		if ( multiplayer == SERVER )
		{
			serverUpdateEntitySkill(&monster, 42);   // monsterAllyIndex
			serverUpdateEntityFlag(&monster, USERFLAG2);
		}
		if ( monsterChangesColorWhenAlly(&s) )
		{
			int bodypart = 0;
			for ( node_t* node = monster.children.first; node != nullptr; node = node->next )
			{
				if ( bodypart >= LIMB_HUMANOID_TORSO )
				{
					if ( Entity* limb = (Entity*)node->element ) { limb->flags[USERFLAG2] = true; }
				}
				++bodypart;
			}
		}
		return true;
	}

	void syncAllyChoices(Entity* monster, Stat* s)
	{
		monster->monsterAllyClass = s->allyClass;
		monster->monsterAllyPickupItems = s->allyItemPickup;
		if ( multiplayer == SERVER )
		{
			serverUpdateEntitySkill(monster, 46);   // monsterAllyClass
			serverUpdateEntitySkill(monster, 44);   // monsterAllyPickupItems
		}
	}

	int leaderOf(Entity* monster, Stat* s)
	{
		if ( s && s->leader_uid )
		{
			Entity* l = uidToEntity(s->leader_uid);
			if ( l && l->behavior == &actPlayer ) { return l->skill[2]; }
		}
		const int idx = monster ? (int)monster->monsterAllyIndex : -1;
		return (idx >= 0 && idx < MAXPLAYERS) ? idx : -1;
	}

	bool anyPlayerStanding()
	{
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( livingPlayer(p) && stats[p] && stats[p]->HP > 0 ) { return true; }
		}
		return false;
	}

	// A member that can take the party on: standing, and its mod turned carry_on on for it.
	bool memberStanding(Entity* e)
	{
		if ( !e || e->behavior != &actMonster ) { return false; }
		Stat* s = e->getStats();
		if ( !s || s->HP <= 0 ) { return false; }
		const Sint32 m = s->MISC_FLAGS[SAMParty::SAM_STAT_FLAG_PARTY];
		return (m >> 24) == (Sint32)SAMParty::MARKER && (m & SAMParty::FLAG_CARRY_ON) != 0;
	}

	int membersStanding(Entity* except)
	{
		int n = 0;
		if ( !map.creatures ) { return 0; }
		for ( node_t* node = map.creatures->first; node; node = node->next )
		{
			Entity* e = (Entity*)node->element;
			if ( e != except && memberStanding(e) ) { ++n; }
		}
		return n;
	}

	bool isExit(Entity* e)
	{
		return e && !e->flags[INVISIBLE]
			&& (e->behavior == &actLadder || e->behavior == &actPortal || e->behavior == &actMidGamePortal || e->behavior == &actCustomPortal);
	}

	// ---- every player down: carry on, or the end ---------------------------------------
	//
	// The host decides. A client cannot tell a party member from a recruit (the marker is not
	// sent), so the host says which of three states the run is in and the client's game-over
	// window and ghost follow that, the way the host's own do.
	enum CarryState : std::uint8_t { CARRY_NONE = 0, CARRY_ON = 1, CARRY_WIPED = 2 };
	std::uint8_t g_netCarry = CARRY_NONE;    // client: the host's last word
	std::uint8_t g_sentCarry = CARRY_NONE;   // host: what the clients were last told
	bool g_wiped = false;                    // host: the last member fell after the players
	bool g_carried = false;                  // host: every player was down while a carry_on member stood
	Uint32 g_goneTicks = 0;                  // host: how long that has been over with nobody declared it
	std::map<std::uint32_t, std::uint32_t> g_heading;   // host: member uid -> the exit it is walking to
	std::map<Uint32, Uint32> g_carryWait;   // host: exit uid -> tick a carrying member first waited there
	constexpr Uint32 CARRY_WAIT_TICKS = TICKS_PER_SECOND * 20;   // then it goes without the stragglers
	std::vector<std::uint32_t> g_netMembers;   // client: the party members on this floor, by uid (sorted)
	std::vector<std::uint32_t> g_sentMembers;  // host: what the clients were last told

	// Our ops, as offsets in SAMNet::Op::PartyFirst's block (host -> client; nothing goes the other way).
	// Once shipped, a number never changes and is never reused.
	constexpr int OP_CARRY = 0;     // every player down: the party carries on / was wiped
	constexpr int OP_MEMBERS = 1;   // the party members on this floor, by uid
	std::uint8_t carryOp() { return (std::uint8_t)(SAMNet::Op::PartyFirst + OP_CARRY); }
	std::uint8_t membersOp() { return (std::uint8_t)(SAMNet::Op::PartyFirst + OP_MEMBERS); }

	// Every party member on this floor, dead or alive, by uid, sorted (host).
	std::vector<std::uint32_t> memberUids()
	{
		std::vector<std::uint32_t> out;
		for ( node_t* node = map.creatures ? map.creatures->first : nullptr; node; node = node->next )
		{
			Entity* e = (Entity*)node->element;
			if ( e && e->behavior == &actMonster && SAMParty::isMember(e->getStats()) ) { out.push_back(e->getUID()); }
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	void sendMembers(int c, const std::vector<std::uint32_t>& uids)
	{
		if ( c < 1 || c >= MAXPLAYERS || client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() ) { return; }
		if ( !SAMNet::peerHasSam(c) ) { return; }
		SAMNet::Writer w;
		const std::size_t n = std::min<std::size_t>(uids.size(), 255);
		w.u8((std::uint8_t)n);
		for ( std::size_t i = 0; i < n; ++i ) { w.u32(uids[i]); }
		SAMNet::sendToClient(c, membersOp(), w.buf);
	}

	void onMembersOp(const std::string& body)   // client
	{
		SAMNet::Reader r(body);
		const int n = r.u8();
		std::vector<std::uint32_t> uids;
		for ( int i = 0; i < n; ++i ) { uids.push_back(r.u32()); }
		if ( !r.ok || multiplayer != CLIENT ) { return; }
		std::sort(uids.begin(), uids.end());
		g_netMembers.swap(uids);
	}

	// Close a game-over window that offered Dismiss (or bring a ghost down) and show the final one.
	void showFinalGameover(int p)
	{
		if ( p < 0 || p >= MAXPLAYERS || !players[p] || !players[p]->isLocalPlayer() || players[p]->entity ) { return; }
		if ( gameUIFrame[p] )
		{
			for ( Frame* f : gameUIFrame[p]->getFrames() )
			{
				if ( f && !strcmp(f->getName(), "gameover") && !f->isToBeDeleted() ) { f->removeSelf(); }
			}
		}
		if ( players[p]->ghost.my ) { players[p]->ghost.setActive(false); }
		MainMenu::openGameoverWindow(p);
	}

	// The run ends when the last member standing falls with every player already down.
	void endRunIfWiped(Entity* dying)
	{
		if ( g_wiped || anyPlayerStanding() || membersStanding(dying) > 0 ) { return; }
		// Only a run a carry_on member was carrying ends here; without one, the last player's own
		// death has already done what a vanilla game over does.
		if ( !g_carried && !(dying && (SAMParty::flags(dying->getStats()) & SAMParty::FLAG_CARRY_ON)) ) { return; }
		g_wiped = true;
		g_carried = false;
		samPartyWiped();
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( !client_disconnected[p] ) { showFinalGameover(p); }
		}
	}

	// CARRY_ON means a carry_on member stands. A client carries on only while none of its players
	// stands either (it sees their health), so it does not lag its own player's fall by a message.
	std::uint8_t hostCarryState()
	{
		if ( anyPlayerStanding() ) { g_wiped = false; return membersStanding(nullptr) > 0 ? CARRY_ON : CARRY_NONE; }   // a floor later, or a restart
		if ( g_wiped ) { return CARRY_WIPED; }
		return membersStanding(nullptr) > 0 ? CARRY_ON : CARRY_NONE;
	}

	void sendCarry(int c, std::uint8_t state)
	{
		if ( c < 1 || c >= MAXPLAYERS || client_disconnected[c] || !players[c] || players[c]->isLocalPlayer() ) { return; }
		if ( !SAMNet::peerHasSam(c) ) { return; }
		SAMNet::Writer w;
		w.u8(state);
		SAMNet::sendToClient(c, carryOp(), w.buf);
	}

	void carryTick()   // every tick of a multiplayer game, on every machine
	{
		if ( multiplayer != SERVER || ticks % 10 != 0 ) { return; }
		std::vector<std::uint32_t> uids = memberUids();
		if ( uids != g_sentMembers )
		{
			g_sentMembers = uids;
			for ( int c = 1; c < MAXPLAYERS; ++c ) { sendMembers(c, uids); }
		}
		const std::uint8_t now = hostCarryState();
		if ( now == g_sentCarry ) { return; }
		g_sentCarry = now;
		for ( int c = 1; c < MAXPLAYERS; ++c ) { sendCarry(c, now); }
	}

	void carryHello(int c)
	{
		if ( multiplayer != SERVER ) { return; }
		sendMembers(c, memberUids());
		sendCarry(c, hostCarryState());
	}

	void carryClear()
	{
		g_heading.clear();
		g_netCarry = CARRY_NONE;
		g_sentCarry = CARRY_NONE;
		g_wiped = false;
		g_carried = false;
		g_goneTicks = 0;
		g_carryWait.clear();
		g_netMembers.clear();
		g_sentMembers.clear();
	}

	void onCarryOp(const std::string& body)   // client
	{
		SAMNet::Reader r(body);
		const std::uint8_t state = r.u8();
		if ( !r.ok || multiplayer != CLIENT || state > CARRY_WIPED ) { return; }
		const std::uint8_t was = g_netCarry;
		g_netCarry = state;
		if ( state == CARRY_WIPED && was != CARRY_WIPED )
		{
			samPartyWiped();
			for ( int p = 0; p < MAXPLAYERS; ++p ) { showFinalGameover(p); }
		}
	}

	struct CarryNet
	{
		CarryNet()
		{
			SAMNet::onClientOp(carryOp(), &onCarryOp);
			SAMNet::onClientOp(membersOp(), &onMembersOp);
			SAMNet::addTickHook(&carryTick);
			SAMNet::addHelloHook(&carryHello);
			SAMNet::addClearHook(&carryClear);
		}
	};
	CarryNet g_carryNet;

	int anyLivingPlayer(int preferred)
	{
		if ( livingPlayer(preferred) ) { return preferred; }
		for ( int p = 0; p < MAXPLAYERS; ++p ) { if ( livingPlayer(p) ) { return p; } }
		return -1;
	}

	void tell(const std::string& text)
	{
		if ( text.empty() ) { return; }
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( !client_disconnected[p] ) { messagePlayer(p, MESSAGE_HINT, "%s", text.c_str()); }
		}
	}

	SamValue describe(Entity* e, Stat* s)
	{
		const std::uint32_t f = SAMParty::flags(s);
		const int cls = std::atoi(attr(s, ATTR_CLASS).c_str());
		const int race = std::atoi(attr(s, ATTR_RACE).c_str());
		SamValue v = SamValue::object();
		v.set("uid", SamValue::integer(e ? (long long)e->getUID() : 0));
		v.set("name", SamValue::string(s->name));
		v.set("class", SamValue::string(classId(cls)));
		v.set("class_id", SamValue::integer(cls));
		v.set("race", SamValue::string(raceId(race)));
		v.set("race_id", SamValue::integer(race));
		v.set("sex", SamValue::string(s->sex == FEMALE ? "female" : "male"));
		v.set("appearance", SamValue::integer(s->stat_appearance));
		v.set("leader", SamValue::integer(leaderOf(e, s)));
		v.set("tag", SamValue::string(attr(s, ATTR_TAG)));
		v.set("mod", SamValue::string(attr(s, ATTR_MOD)));
		v.set("level", SamValue::integer(s->LVL));
		v.set("hp", SamValue::integer(s->HP));
		v.set("max_hp", SamValue::integer(s->MAXHP));
		v.set("wait_at_exit", SamValue::boolean((f & SAMParty::FLAG_WAIT_AT_EXIT) != 0));
		v.set("respawn", SamValue::boolean((f & SAMParty::FLAG_RESPAWN) != 0));
		v.set("grow_like_class", SamValue::boolean((f & SAMParty::FLAG_GROW_LIKE_CLASS) != 0));
		v.set("carry_on", SamValue::boolean((f & SAMParty::FLAG_CARRY_ON) != 0));
		return v;
	}

	Entity* memberEntity(std::uint32_t uid)
	{
		Entity* e = uid ? uidToEntity(uid) : nullptr;
		return (e && SAMParty::isMember(e)) ? e : nullptr;
	}

	bool readFlagOption(const SamValue& spec, const char* key, std::uint32_t bit, std::uint32_t& f, std::string* why)
	{
		if ( spec.wrongType(key, SamValue::Type::Bool) ) { say(why, std::string(key) + " must be true or false"); return false; }
		const SamValue* v = spec.get(key);
		if ( v && !v->isNil() ) { f = spec.flag(key, false) ? (f | bit) : (f & ~bit); }
		return true;
	}

	bool readTag(const SamValue& spec, std::string& tag, std::string* why)
	{
		if ( spec.wrongType("tag", SamValue::Type::Str) ) { say(why, "tag must be a string"); return false; }
		const SamValue* t = spec.get("tag");
		if ( !t || t->isNil() ) { return true; }
		if ( (int)t->s.size() > SAMParty::MAX_TAG ) { say(why, "tag is longer than " + std::to_string(SAMParty::MAX_TAG) + " characters (a save keeps 31)"); return false; }
		tag = t->s;
		return true;
	}
}

namespace SAMParty
{
	// ================================================================== the marker

	std::uint32_t flags(const Stat* s)
	{
		if ( !s ) { return 0; }
		const std::uint32_t m = (std::uint32_t)s->MISC_FLAGS[SAM_STAT_FLAG_PARTY];
		return ((m >> 24) & 0xFFu) == MARKER ? (m & 0xFFFFu) : 0;
	}

	bool isMember(const Stat* s)
	{
		return (flags(s) & FLAG_MEMBER) != 0;
	}

	bool isMember(Entity* e)
	{
		return e && e->behavior == &actMonster && isMember(e->getStats());
	}

	bool takesOrdersFreely(Entity* follower)
	{
		if ( !follower ) { return false; }
		if ( multiplayer == CLIENT )
		{
			return std::binary_search(g_netMembers.begin(), g_netMembers.end(), (std::uint32_t)follower->getUID());
		}
		return isMember(follower);
	}

	// ================================================================== script side

	std::uint32_t spawn(int leader, const SamValue& spec, std::string* why, const std::string& ns)
	{
		if ( multiplayer == CLIENT ) { say(why, "party members are the host's"); return 0; }
		if ( !spec.isMap() ) { say(why, "the second argument must be a table { name, race, class, ... }"); return 0; }

		// Everything about the spec first, so a bad one says what is wrong with it rather
		// than "nobody to follow".
		const int race = resolveRace(spec.get("race"));
		if ( race < 0 ) { say(why, "race '" + spec.str("race", "?") + "' is not a race this game can play (see sam_party_choices)"); return 0; }
		const int cls = resolveClass(spec.get("class"));
		if ( cls < 0 ) { say(why, "class '" + spec.str("class", "?") + "' is not a class this game has (see sam_party_choices)"); return 0; }

		sex_t sex = (local_rng.rand() % 2) ? FEMALE : MALE;
		if ( const SamValue* sx = spec.get("sex") )
		{
			if ( sx->type == SamValue::Type::Str )
			{
				const std::string v = lower(sx->s);
				if ( v == "female" || v == "f" ) { sex = FEMALE; }
				else if ( v == "male" || v == "m" ) { sex = MALE; }
				else { say(why, "sex must be \"male\" or \"female\""); return 0; }
			}
			else if ( sx->isNumber() ) { sex = spec.whole("sex", 0) ? FEMALE : MALE; }
			else if ( !sx->isNil() ) { say(why, "sex must be \"male\" or \"female\""); return 0; }
		}

		if ( spec.wrongType("appearance", SamValue::Type::Int) ) { say(why, "appearance must be a number"); return 0; }
		const long long look = spec.whole("appearance", 0);
		if ( look < 0 || look >= NUMAPPEARANCES ) { say(why, "appearance must be 0 to " + std::to_string(NUMAPPEARANCES - 1)); return 0; }

		if ( spec.wrongType("name", SamValue::Type::Str) ) { say(why, "name must be a string"); return 0; }
		std::string name = spec.str("name", "");
		if ( (int)name.size() > MAX_NAME ) { say(why, "name is longer than " + std::to_string(MAX_NAME) + " characters"); return 0; }

		std::string tag = ns;
		if ( !readTag(spec, tag, why) ) { return 0; }

		std::uint32_t f = FLAG_MEMBER | FLAG_WAIT_AT_EXIT | FLAG_RESPAWN | FLAG_GROW_LIKE_CLASS;
		if ( !readFlagOption(spec, "wait_at_exit", FLAG_WAIT_AT_EXIT, f, why)
			|| !readFlagOption(spec, "respawn", FLAG_RESPAWN, f, why)
			|| !readFlagOption(spec, "grow_like_class", FLAG_GROW_LIKE_CLASS, f, why)
			|| !readFlagOption(spec, "carry_on", FLAG_CARRY_ON, f, why) )
		{
			return 0;
		}

		int pickup = ALLY_PICKUP_NONPLAYER;
		if ( spec.wrongType("pickup", SamValue::Type::Str) ) { say(why, "pickup must be \"unowned\", \"all\" or \"none\""); return 0; }
		{
			const std::string p = lower(spec.str("pickup", "unowned"));
			if ( p == "all" ) { pickup = ALLY_PICKUP_ALL; }
			else if ( p == "none" ) { pickup = ALLY_PICKUP_NONE; }
			else if ( p != "unowned" ) { say(why, "pickup must be \"unowned\", \"all\" or \"none\""); return 0; }
		}

		if ( !livingPlayer(leader) )
		{
			say(why, "player " + std::to_string(leader) + " is not in the world to lead it (spawn from game.on_game_start or later)");
			return 0;
		}
		Entity* leaderEntity = players[leader]->entity;
		const Monster type = monsterForRace(race);
		Entity* monster = summonMonster(type, (long)(std::floor(leaderEntity->x / 16) * 16 + 8), (long)(std::floor(leaderEntity->y / 16) * 16 + 8));
		Stat* s = monster ? monster->getStats() : nullptr;
		if ( !monster || !s )
		{
			say(why, "there is no room beside player " + std::to_string(leader));
			return 0;
		}

		// MONSTER_INIT 1: stats are set, so actMonster's first tick builds the body and skips
		// the random monster roll (gear, stats, a random name).
		monster->skill[3] = 1;

		// The same baseline a player starts from, then the class.
		s->clearStats();
		s->type = type;
		s->sex = sex;
		s->playerRace = race;
		s->stat_appearance = (type == HUMAN) ? (Uint32)look : 0;
		if ( name.empty() ) { name = randomName(sex == FEMALE ? "female" : "male"); }
		stringCopy(s->name, name.c_str(), sizeof(s->name), name.size());
		initClassStats(cls, s);
		s->HP = std::max(1, s->MAXHP);
		s->OLDHP = s->HP;
		s->MP = s->MAXMP;
		s->leader_uid = 0;
		s->MISC_FLAGS[STAT_FLAG_DISABLE_MINIBOSS] = 1;
		s->monsterForceAllegiance = Stat::MONSTER_FORCE_ALLEGIANCE_NONE;
		setFlags(s, f);
		setAttr(s, ATTR_CLASS, std::to_string(cls));
		setAttr(s, ATTR_RACE, std::to_string(race));
		setAttr(s, ATTR_TAG, tag);
		setAttr(s, ATTR_MOD, ns);

		applyKit(monster, s, cls);
		if ( !bindToLeader(*leaderEntity, *monster, *s) )
		{
			SAM_WARN(MOD, std::string("sam_party_spawn: ") + s->name + " could not join player " + std::to_string(leader) + "'s followers; it is on the players' side but will not follow.");
		}
		s->allyClass = ALLY_CLASS_MIXED;
		s->allyItemPickup = pickup;
		syncAllyChoices(monster, s);

		SamEvent e("party.on_member_spawned");
		e.i("uid", (long long)monster->getUID()).i("leader", leader).s("tag", tag).s("mod", ns)
			.s("member_name", s->name).s("class", classId(cls)).s("race", raceId(race)).i("respawned", 0).s("message", "");
		e.fire();
		return monster->getUID();
	}

	int followerLeader(Entity* monster)
	{
		if ( !monster || monster->behavior != &actMonster ) { return -1; }
		Stat* s = monster->getStats();
		return s ? leaderOf(monster, s) : -1;
	}

	bool isMemberUid(std::uint32_t uid)
	{
		return memberEntity(uid) != nullptr;
	}

	SamValue info(std::uint32_t uid)
	{
		Entity* e = memberEntity(uid);
		return e ? describe(e, e->getStats()) : SamValue::nil();
	}

	bool setOptions(std::uint32_t uid, const SamValue& options, std::string* why)
	{
		if ( multiplayer == CLIENT ) { say(why, "party members are the host's"); return false; }
		Entity* e = memberEntity(uid);
		if ( !e ) { say(why, "uid " + std::to_string(uid) + " is not a party member on this floor"); return false; }
		if ( !options.isMap() ) { say(why, "the second argument must be a table { wait_at_exit, respawn, grow_like_class, carry_on, tag }"); return false; }
		Stat* s = e->getStats();
		std::uint32_t f = flags(s);
		std::string tag = attr(s, ATTR_TAG);
		if ( !readFlagOption(options, "wait_at_exit", FLAG_WAIT_AT_EXIT, f, why)
			|| !readFlagOption(options, "respawn", FLAG_RESPAWN, f, why)
			|| !readFlagOption(options, "grow_like_class", FLAG_GROW_LIKE_CLASS, f, why)
			|| !readFlagOption(options, "carry_on", FLAG_CARRY_ON, f, why)
			|| !readTag(options, tag, why) )
		{
			return false;
		}
		if ( (f & FLAG_CARRY_ON) && !(flags(s) & FLAG_CARRY_ON) && !anyPlayerStanding() )
		{
			say(why, "carry_on cannot be turned on once every player is down: that run has ended");
			return false;
		}
		setFlags(s, f | FLAG_MEMBER);
		setAttr(s, ATTR_TAG, tag);
		return true;
	}

	SamValue list()
	{
		SamValue out = SamValue::array();
		if ( !map.creatures ) { return out; }
		for ( node_t* node = map.creatures->first; node; node = node->next )
		{
			Entity* e = (Entity*)node->element;
			if ( isMember(e) ) { out.push(describe(e, e->getStats())); }
		}
		return out;
	}

	SamValue fallen()
	{
		SamValue out = SamValue::array();
		for ( const Fallen& fl : g_fallen )
		{
			SamValue v = SamValue::object();
			v.set("name", SamValue::string(fl.stats->name));
			v.set("class", SamValue::string(classId(std::atoi(attr(fl.stats, ATTR_CLASS).c_str()))));
			v.set("race", SamValue::string(raceId(std::atoi(attr(fl.stats, ATTR_RACE).c_str()))));
			v.set("tag", SamValue::string(attr(fl.stats, ATTR_TAG)));
			v.set("mod", SamValue::string(attr(fl.stats, ATTR_MOD)));
			v.set("leader", SamValue::integer(fl.leader));
			out.push(v);
		}
		return out;
	}

	bool setDifficultyCount(int count, std::string* why)
	{
		if ( multiplayer == CLIENT ) { say(why, "the host decides the difficulty"); return false; }
		if ( count < 0 || count > MAXPLAYERS - 1 ) { say(why, "the count must be 0 to " + std::to_string(MAXPLAYERS - 1)); return false; }
		g_askedCount = count;
		return true;
	}

	SamValue choices()
	{
		SamValue classes = SamValue::array();
		for ( int c = 0; c < NUMCLASSES; ++c )
		{
			const std::string id = classId(c);
			if ( id.empty() ) { continue; }
			SamValue v = SamValue::object();
			v.set("id", SamValue::string(id));
			v.set("name", SamValue::string(className(c)));
			v.set("number", SamValue::integer(c));
			classes.push(v);
		}
		for ( int k = 0; k < SAMClasses::count(); ++k )
		{
			const int c = SAMClasses::classIdAtIndex(k);
			if ( c < 0 ) { continue; }
			SamValue v = SamValue::object();
			v.set("id", SamValue::string(classId(c)));
			v.set("name", SamValue::string(className(c)));
			v.set("number", SamValue::integer(c));
			classes.push(v);
		}
		SamValue races = SamValue::array();
		auto addRace = [&](int r) {
			SamValue v = SamValue::object();
			v.set("id", SamValue::string(raceId(r)));
			v.set("name", SamValue::string(raceName(r)));
			v.set("number", SamValue::integer(r));
			races.push(v);
		};
		for ( int r : kPlayableRaces ) { addRace(r); }
		for ( int k = 0; k < SAMRaces::count(); ++k )
		{
			const int r = SAMRaces::raceIdAtIndex(k);
			if ( r >= 0 ) { addRace(r); }
		}
		SamValue out = SamValue::object();
		out.set("classes", classes);
		out.set("races", races);
		out.set("appearances", SamValue::integer(NUMAPPEARANCES));
		return out;
	}

	int choiceAllowed(const std::string& race, const std::string& cls)
	{
		const SamValue r = SamValue::string(race), c = SamValue::string(cls);
		const int raceNum = resolveRace(&r);
		const int classNum = resolveClass(&c);
		if ( raceNum < 0 || classNum < 0 ) { return -1; }
		// A S.A.M race or class carries no DLC or unlock of its own (menu.cpp's custom-race arm
		// returns VALID_OK_CHARACTER the same way).
		if ( raceNum >= SAM_RACE_ID_BASE || classNum >= SAM_CLASS_ID_BASE ) { return 1; }
		const int player = (clientnum >= 0 && clientnum < MAXPLAYERS && stats[clientnum]) ? clientnum : -1;
		if ( player < 0 ) { return 1; }
		return isCharacterValidFromDLC(player, classNum, raceNum, 0) == VALID_OK_CHARACTER ? 1 : 0;
	}

	std::string randomName(const std::string& sex)
	{
		const std::string s = lower(sex);
		const bool female = s == "female" ? true : s == "male" ? false : (local_rng.rand() % 2) != 0;
		const std::vector<std::string>& names = female ? randomPlayerNamesFemale : randomPlayerNamesMale;
		if ( names.empty() ) { return female ? "Companion" : "Companion"; }
		std::string n = names[local_rng.rand() % names.size()];
		if ( (int)n.size() > MAX_NAME ) { n.resize(MAX_NAME); }
		return n;
	}

	// ================================================================== engine side

	int allegiance(Entity* me, Stat* myStats, Entity* you, Stat* yourStats, bool askingIfEnemy)
	{
		if ( !me || !you ) { return -1; }
		const bool meMember = me->behavior == &actMonster && isMember(myStats);
		const bool youMember = you->behavior == &actMonster && isMember(yourStats);
		if ( !meMember && !youMember ) { return -1; }   // the vanilla path: one compare each

		Stat* memberStats = meMember ? myStats : yourStats;
		Entity* other = meMember ? you : me;
		Stat* otherStats = meMember ? yourStats : myStats;

		// A player, a party member, or a follower of either: always friends.
		auto playerSide = [](Entity* e, Stat* st) {
			if ( e->behavior == &actPlayer || e->behavior == &actPlayerLimb ) { return true; }
			if ( e->behavior == &actMonster && isMember(st) ) { return true; }
			if ( st && st->leader_uid )
			{
				Entity* l = uidToEntity(st->leader_uid);
				if ( l && (l->behavior == &actPlayer || (l->behavior == &actMonster && isMember(l->getStats()))) ) { return true; }
			}
			return false;
		};
		if ( playerSide(other, otherStats) ) { return askingIfEnemy ? 0 : 1; }

		// With a living leader, the follower path already does the right thing.
		if ( memberStats && memberStats->leader_uid && uidToEntity(memberStats->leader_uid) ) { return -1; }

		// The leader is down: friend or foe the way a living player would judge it. That call
		// runs the race arms for THAT player's race, so a custom race's declared allegiances
		// still apply to the party through its players.
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( livingPlayer(p) )
			{
				const bool answer = askingIfEnemy ? players[p]->entity->checkEnemy(other) : players[p]->entity->checkFriend(other);
				return answer ? 1 : 0;
			}
		}
		const bool friendly = otherStats ? monsterally[HUMAN][otherStats->type] : false;
		return askingIfEnemy ? (friendly ? 0 : 1) : (friendly ? 1 : 0);
	}

	void onDeath(Entity* monster, Stat* s)
	{
		if ( multiplayer == CLIENT || !monster || !isMember(s) ) { return; }
		endRunIfWiped(monster);
		const int leader = leaderOf(monster, s);
		const bool willRespawn = (flags(s) & FLAG_RESPAWN) != 0;
		const std::string tag = attr(s, ATTR_TAG), ns = attr(s, ATTR_MOD);

		SamEvent e("party.on_member_died");
		const std::string message = willRespawn ? std::string(s->name) + " will rejoin the party on the next floor." : std::string();
		e.i("uid", (long long)monster->getUID()).i("leader", leader).s("tag", tag).s("mod", ns).s("member_name", s->name)
			.i("will_respawn", willRespawn ? 1 : 0).s("message", message);
		const bool allowed = e.fire();
		if ( !willRespawn || !allowed ) { return; }   // permadeath, or a mod said no

		Stat* comeback = s->copyStats();
		if ( !comeback ) { return; }
		// Gear follows a player's rule: with Keep Inventory the copy keeps it and the body drops
		// nothing; without, the body drops everything (the engine does that next) and the copy
		// comes back with nothing.
		Stat* strip = keepInventoryGlobal ? s : comeback;
		Item** slots[] = { &strip->helmet, &strip->breastplate, &strip->gloves, &strip->shoes, &strip->shield,
			&strip->weapon, &strip->cloak, &strip->amulet, &strip->ring, &strip->mask };
		for ( Item** slot : slots )
		{
			if ( !*slot ) { continue; }
			if ( (*slot)->node ) { list_RemoveNode((*slot)->node); }
			else { free(*slot); }
			*slot = nullptr;
		}
		list_FreeAll(&strip->inventory);
		strip->GOLD = 0;   // the copy or the body, whichever is stripped: gold is never on both
		for ( int c = 0; c < NUMEFFECTS; ++c )
		{
			comeback->clearEffect(c);
			comeback->EFFECTS_TIMERS[c] = 0;
		}
		list_FreeAll(&comeback->magic_effects);
		list_FreeAll(&comeback->FOLLOWERS);
		comeback->HP = std::max(1, comeback->MAXHP);
		comeback->OLDHP = comeback->HP;
		comeback->MP = comeback->MAXMP;
		comeback->leader_uid = 0;
		g_fallen.push_back(Fallen{ comeback, leader < 0 ? 0 : leader });
		tell(e.getStr("message", message));
	}

	void respawnFallen()
	{
		g_exitTries.clear();
		g_heading.clear();
		g_carryWait.clear();
		if ( multiplayer == CLIENT ) { return; }
		std::vector<Fallen> waiting;
		for ( Fallen& fl : g_fallen )
		{
			const int lead = anyLivingPlayer(fl.leader);
			Entity* leader = lead >= 0 ? players[lead]->entity : nullptr;
			Entity* monster = leader ? summonMonster(fl.stats->type, (long)(std::floor(leader->x / 16) * 16 + 8), (long)(std::floor(leader->y / 16) * 16 + 8)) : nullptr;
			if ( !monster || !monster->children.last )
			{
				waiting.push_back(fl);   // nowhere to stand: another floor
				continue;
			}
			// Swap the kept Stat in for the one summonMonster made, the way gameLogic's
			// follower placement does.
			monster->skill[3] = 1;
			list_RemoveNode(monster->children.last);
			node_t* node = list_AddNodeLast(&monster->children);
			node->element = fl.stats;
			node->deconstructor = &statDeconstructor;
			node->size = sizeof(Stat*);
			Stat* s = fl.stats;
			bindToLeader(*leader, *monster, *s);
			syncAllyChoices(monster, s);

			const std::string message = std::string(s->name) + " rejoins the party.";
			SamEvent e("party.on_member_spawned");
			e.i("uid", (long long)monster->getUID()).i("leader", lead).s("tag", attr(s, ATTR_TAG)).s("mod", attr(s, ATTR_MOD))
				.s("member_name", s->name).s("class", classId(std::atoi(attr(s, ATTR_CLASS).c_str())))
				.s("race", raceId(std::atoi(attr(s, ATTR_RACE).c_str()))).i("respawned", 1).s("message", message);
			e.fire();
			const std::string said = e.getStr("message", message);
			if ( !said.empty() ) { messagePlayer(lead, MESSAGE_HINT, "%s", said.c_str()); }
		}
		g_fallen.swap(waiting);   // the rest are owned by their new monsters now
	}

	bool exitsMayClose(Entity* exitEntity, int player)
	{
		if ( multiplayer == CLIENT || !exitEntity || !map.creatures ) { return true; }
		std::vector<Entity*> stragglers;
		for ( node_t* node = map.creatures->first; node; node = node->next )
		{
			Entity* e = (Entity*)node->element;
			if ( !isMember(e) ) { continue; }
			Stat* s = e->getStats();
			if ( !(flags(s) & FLAG_WAIT_AT_EXIT) || s->HP <= 0 ) { continue; }
			const double d = std::sqrt(std::pow(exitEntity->x - e->x, 2) + std::pow(exitEntity->y - e->y, 2));
			if ( d > ASSEMBLE_RANGE ) { stragglers.push_back(e); }
		}
		if ( stragglers.empty() ) { g_carryWait.erase(exitEntity->getUID()); return true; }

		if ( partyCarriesOn() )
		{
			// Nobody clicked: a member standing here is taking the party on (exitUser), and think()
			// is already walking the rest over. This runs every tick, so no message and no
			// on_waiting_at_exit (an "attempt" a tick would give up at once); wait a while instead.
			auto w = g_carryWait.find(exitEntity->getUID());
			if ( w == g_carryWait.end() ) { g_carryWait[exitEntity->getUID()] = ticks; return false; }
			if ( ticks - w->second < CARRY_WAIT_TICKS ) { return false; }
			g_carryWait.erase(w);
			return true;   // the stragglers still come along (followers always change floor with the party)
		}

		const int attempt = ++g_exitTries[exitEntity->getUID()];
		Stat* first = stragglers.front()->getStats();
		const std::string message = (stragglers.size() == 1 && first)
			? std::string("Waiting for ") + first->name + " to reach the exit..."
			: std::string("Waiting for your party to reach the exit...");
		SamEvent e("party.on_waiting_at_exit");
		e.i("player", player).i("exit_uid", (long long)exitEntity->getUID()).i("waiting", (long long)stragglers.size())
			.i("first_uid", (long long)stragglers.front()->getUID()).s("first_name", first ? first->name : "")
			.i("attempt", attempt).s("message", message);
		if ( !e.fire() )
		{
			g_exitTries.erase(exitEntity->getUID());
			return true;   // a mod let the party go without them
		}
		const int exitX = (int)(exitEntity->x / 16);
		const int exitY = (int)(exitEntity->y / 16);
		for ( Entity* s : stragglers )
		{
			if ( s->monsterAllyState != ALLY_STATE_MOVETO ) { s->monsterAllySendCommand(ALLY_CMD_MOVETO_CONFIRM, exitX, exitY, 0); }
		}
		const std::string said = e.getStr("message", message);
		if ( player >= 0 && player < MAXPLAYERS && !said.empty() ) { messagePlayer(player, MESSAGE_INTERACTION, "%s", said.c_str()); }
		return false;
	}

	void tick()
	{
		if ( multiplayer == CLIENT ) { return; }
		if ( anyPlayerStanding() ) { g_carried = false; g_wiped = false; g_goneTicks = 0; return; }   // a floor later, or a restart
		if ( g_wiped ) { return; }
		if ( membersStanding(nullptr) > 0 ) { g_carried = true; g_goneTicks = 0; return; }
		if ( !g_carried ) { return; }
		// The members that carried the run are gone and none of them died on the way (a mod
		// removed them, or they were swept off the floor). Wait a moment in case a floor change is
		// still placing them, then end the run rather than leave the players watching as ghosts.
		if ( ++g_goneTicks >= TICKS_PER_SECOND * 2 ) { endRunIfWiped(nullptr); }
	}

	bool partyCarriesOn()
	{
		if ( multiplayer == CLIENT ) { return g_netCarry == CARRY_ON && !anyPlayerStanding(); }   // the host's word, and no player of ours up
		if ( anyPlayerStanding() || g_wiped ) { return false; }
		return membersStanding(nullptr) > 0;
	}

	int exitUser(Entity* exitEntity)
	{
		if ( !isExit(exitEntity) || !partyCarriesOn() || !map.creatures ) { return -1; }
		for ( node_t* node = map.creatures->first; node; node = node->next )
		{
			Entity* e = (Entity*)node->element;
			if ( !memberStanding(e) ) { continue; }
			const double d = std::sqrt(std::pow(exitEntity->x - e->x, 2) + std::pow(exitEntity->y - e->y, 2));
			if ( d > TOUCHRANGE ) { continue; }
			const int lead = leaderOf(e, e->getStats());
			if ( lead >= 0 && lead < MAXPLAYERS && !client_disconnected[lead] ) { return lead; }
			for ( int p = 0; p < MAXPLAYERS; ++p ) { if ( !client_disconnected[p] ) { return p; } }
		}
		return -1;
	}

	void think(Entity* monster, Stat* s)
	{
		if ( multiplayer == CLIENT || !memberStanding(monster) ) { return; }
		const std::uint32_t me = monster->getUID();
		if ( !partyCarriesOn() ) { g_heading.erase(me); return; }
		if ( monster->monsterState == MONSTER_STATE_ATTACK ) { return; }   // finish the fight first

		// The exit it is already walking to, while it still is and it is not time to re-plan.
		auto it = g_heading.find(me);
		Entity* current = it != g_heading.end() ? uidToEntity(it->second) : nullptr;
		if ( current && !isExit(current) ) { current = nullptr; }
		if ( current )
		{
			const double d = std::sqrt(std::pow(current->x - monster->x, 2) + std::pow(current->y - monster->y, 2));
			if ( d <= TOUCHRANGE / 2 ) { return; }   // there: exitUser takes it from here
			if ( monster->monsterAllyState == ALLY_STATE_MOVETO && monster->monsterState == MONSTER_STATE_HUNT
				&& (ticks / TICKS_PER_SECOND) % 5 != 0 ) { return; }   // on its way; re-plan every 5 s in case it got stuck
		}

		// Nearest first, but only an exit it can actually walk to: a floor can also have a
		// secret ladder behind a wall, and aiming at that one left it standing still.
		std::vector<std::pair<double, Entity*>> exits;
		if ( current ) { exits.push_back({ -2.0, current }); }
		// Then the exit another member is already making for, so the party meets at one exit
		// instead of each waiting at its own for the others.
		Entity* shared = nullptr;
		for ( const auto& h : g_heading )
		{
			if ( h.first == me ) { continue; }
			Entity* other = uidToEntity(h.first);
			Entity* ex = uidToEntity(h.second);
			if ( memberStanding(other) && isExit(ex) && ex != current ) { shared = ex; break; }
		}
		if ( shared ) { exits.push_back({ -1.0, shared }); }
		for ( node_t* node = map.entities ? map.entities->first : nullptr; node; node = node->next )
		{
			Entity* e = (Entity*)node->element;
			if ( !isExit(e) || e == current || e == shared ) { continue; }
			exits.push_back({ std::pow(e->x - monster->x, 2) + std::pow(e->y - monster->y, 2), e });
		}
		std::sort(exits.begin(), exits.end(), [](const std::pair<double, Entity*>& l, const std::pair<double, Entity*>& r) { return l.first < r.first; });
		for ( const auto& ex : exits )
		{
			Entity* e = ex.second;
			if ( std::sqrt(std::pow(e->x - monster->x, 2) + std::pow(e->y - monster->y, 2)) <= TOUCHRANGE / 2 )
			{
				g_heading[me] = e->getUID();
				return;
			}
			if ( monster->monsterSetPathToLocation((int)(e->x / 16), (int)(e->y / 16), 1,
				GeneratePathTypes::GENERATE_PATH_PLAYER_ALLY_MOVETO) )
			{
				monster->monsterState = MONSTER_STATE_HUNT;
				monster->monsterAllyState = ALLY_STATE_MOVETO;
				serverUpdateEntitySkill(monster, 0);
				g_heading[me] = e->getUID();
				return;
			}
		}
		g_heading.erase(me);   // no exit it can reach from here: try again next second
	}

	bool levelUpLikePlayer(Entity* monster, Stat* s, int increasestat[3])
	{
		if ( !monster || monster->behavior != &actMonster || !(flags(s) & FLAG_MEMBER) || !(flags(s) & FLAG_GROW_LIKE_CLASS) ) { return false; }
		const std::string c = attr(s, ATTR_CLASS);
		if ( c.empty() ) { return false; }
		monster->playerStatIncrease(std::atoi(c.c_str()), increasestat);
		return true;
	}

	int balancePlayers()
	{
		int n = 0;
		for ( int p = 0; p < MAXPLAYERS; ++p ) { if ( !client_disconnected[p] ) { ++n; } }
		return std::max(1, std::min(n + g_runCount, (int)MAXPLAYERS));
	}

	int runDifficultyCount() { return g_runCount; }

	void lobbyOpened()
	{
		g_askedCount = 0;
	}

	void beginRun()
	{
		if ( multiplayer == CLIENT ) { return; }
		int connected = 0;
		for ( int p = 0; p < MAXPLAYERS; ++p ) { if ( !client_disconnected[p] ) { ++connected; } }
		const int count = std::max(0, std::min(g_askedCount, (int)MAXPLAYERS - connected));
		if ( count > 0 && !SAMLobby::everyPeerSaidHello() )
		{
			g_runCount = 0;
			SAM_WARN(MOD, "A mod asked for " + std::to_string(count) + " party member(s) to count toward difficulty, but a player is "
				"without S.A.M party support, so this run is balanced for the connected players only (every machine has to build the same dungeon).");
			return;
		}
		g_runCount = count;
		if ( count > 0 )
		{
			SAM_INFO(MOD, "This run counts " + std::to_string(count) + " party member(s) as players: balanced for " + std::to_string(connected + count) + ".");
		}
	}

	void writeStartTail(std::uint8_t* data, int& len)
	{
		if ( !data || g_runCount <= 0 || len != 17 ) { return; }   // nothing to say: the packet stays vanilla
		data[17] = 'S';
		data[18] = 'P';
		data[19] = TAIL_VERSION;
		data[20] = (std::uint8_t)g_runCount;
		len = 21;
	}

	void readStartTail(const std::uint8_t* data, int len)
	{
		g_runCount = 0;
		if ( !data || len < 21 || data[17] != 'S' || data[18] != 'P' || data[19] < 1 ) { return; }
		g_runCount = std::min((int)data[20], (int)MAXPLAYERS - 1);
	}

	void clear()
	{
		carryClear();
		for ( Fallen& fl : g_fallen ) { delete fl.stats; }
		g_fallen.clear();
		g_exitTries.clear();
	}
}
