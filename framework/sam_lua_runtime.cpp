/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_lua_runtime.cpp
	Desc: implementation of the sandboxed Lua 5.4 scripting runtime.

	Safety mechanisms (all active for every script):
	  * Memory cap    — a custom lua_Alloc denies any allocation that would push
	                    total Lua memory past SandboxConfig::memoryCapBytes. Lua
	                    turns the denial into a recoverable LUA_ERRMEM.
	  * Watchdog      — a LUA_MASKCOUNT hook fires every watchdogInterval VM
	                    instructions; once instructionBudget instructions elapse
	                    within a single callback it raises a Lua error (longjmp)
	                    that unwinds cleanly back to our lua_pcall. Kills
	                    `while true do end` in ~milliseconds without hanging.
	  * Stripped libs — only base/table/string/math/utf8 are opened. os, io,
	                    package/require, debug and coroutine are never loaded, and
	                    dofile/loadfile/load/loadstring are nil'd for good measure.
	  * Isolation     — every script run and every callback goes through
	                    lua_pcall, so an error disables only that script and is
	                    logged; the host process is never taken down.
	  * No pointers   — events cross the C++/Lua boundary as copied primitives.

-------------------------------------------------------------------------------*/

#ifndef NOMINMAX
#define NOMINMAX // Lua/Windows headers: keep windows.h min/max macros away (SAM discipline)
#endif

#include "sam_lua_runtime.hpp"
#include "sam_js_runtime.hpp"  // Part 2: sam_fire_hook cross-dispatches to JS scripts too
#include "sam_test.hpp"   // sam_test_done: end an unattended -samtest run
#include "sam_speed.hpp"  // sam_set_game_speed / sam_get_game_speed: the simulation speed
#include "sam_loot.hpp"   // P_ITEMS: the loot pool, its tables, the containers and the shops
#include "sam_settings.hpp"  // sam_register_action / sam_register_setting / sam_get_setting / sam_set_setting / sam_list_settings
#include "sam_logger.hpp"
#include "sam_errors.hpp"   // writeFileAtomic

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstdlib>
#include <cstring>
#include <algorithm> // std::sort: deterministic key order in sam_random_weighted
#include <cmath>    // lround — move-speed fixed-point encoding
#include <string>
#include <type_traits>   // the ItemType assertion on the appearance guard
#include <set>      // the damage-immunity set, checked from Entity::modHP
#include <vector>
#include <map>
#include <fstream>
#include <filesystem>
#include "nlohmann/json.hpp"

// When compiled INSIDE the Barony engine (its headers are on the include path),
// enable host functions that actually AFFECT the game (sam_grant_item). In the
// standalone sandbox test these headers are absent, so those bindings are simply
// omitted and the pure sandbox still builds.
#if defined(__has_include) && __has_include("items.hpp")
#	define SAM_LUA_HAVE_BARONY 1
#	include "main.hpp"      // multiplayer, CLIENT, MAXPLAYERS
#	include "game.hpp"      // engine globals
#	include "items.hpp"     // ItemType, Status, newItem, itemPickup
#	include "player.hpp"    // players[], isLocalPlayer()
#	include "input.hpp"     // Input::inputs[] — bound-action reads (const only, never consume*)
#	include "net.hpp"
#	include "mod_tools.hpp" // ItemTooltips.itemNameStringToItemID
#	include "stat.hpp"      // Stat members, EFF_* effect ids, stats[], MAX_PLAYER_STAT_VALUE
#	include "entity.hpp"    // Entity::setEffect/setHP/setMP/getUID, act* behaviors, map iteration
#	include "monster.hpp"   // actMonster, Monster enum
#	include "collision.hpp" // entityDist
#	include "scores.hpp"    // completionTime (the run clock the game itself displays)
#	include "paths.hpp"     // GeneratePathTypes (monster movement bindings)
#	include "engine/audio/sound.hpp" // playSoundPlayer, numsounds
#	include "files.hpp"     // outputdir (savegames base dir for persistent mod data)
#	include "sam_items.hpp" // SAMItems::itemIdForIdString (custom item names in queries)
#	include "sam_effects.hpp" // custom status effects (resolve "ns:effect" ids)
#	include "sam_sounds.hpp" // custom sounds (resolve "ns:sound" ids in sam_play_sound)
#	include "sam_races.hpp" // custom races (sam_get_race id lookup)
#	include "sam_hud.hpp"  // script-driven HUD layer
#	include "sam_images.hpp" // the mod's own pictures (overlay + HUD art)
#	include "sam_ui.hpp"     // interactive mod panels
#	include "sam_catalog.hpp" // reading the game content registries
#	include "sam_world.hpp" // world queries, terrain, mechanisms
#	include "sam_world_state.hpp" // per-character mod state carried in the savegame
#	include "sam_workshop.hpp" // SAMModManifest (sam_get_mods)
#	include "sam_classes.hpp" // v0.7.0 F5: SAMClasses::patchClass / addClassPassive
#	include "sam_monster_patches.hpp" // v0.7.0 F5: SAMMonsterPatch::set
#	include "sam_monsters.hpp" // SAMMonsters::traitBitForName (sam_monster_has_trait)
#	include "sam_combat.hpp" // species damage resistance + the on_damage_multiplier hook
#	include "sam_camera.hpp" // script control of where a player's camera is
#	include "sam_rules.hpp"  // stat modifiers, effect immunity, the XP curve
#	include "sam_music.hpp"  // mod music: sam_play_music and friends
#	include "sam_bodies.hpp"   // runtime model control (sam_set_model)
#	include "sam_spells.hpp"  // custom-spell registry (sam_grant_spell)
#	include "sam_models.hpp"  // v1.4.0: SAMModels::modelIndexForId (companion custom .vox)
#	include "sam_net.hpp"     // multiplayer contracts + the channel that carries a call to its player
#	include "sam_mp_inventory.hpp" // remote players' backpacks and spells, as the host mirrors them
#	include "sam_mp_entities.hpp" // multiplayer: script-made entities, pinned props, terrain, gibs
#	include "sam_mp_input.hpp" // keys, bound actions and client-raised events in multiplayer
#	include "magic/magic.hpp" // addSpell (grant a spell to a player)
#	include "ui/MainMenu.hpp" // MainMenu::emptyBinding / hiddenBinding: the engine's "nothing here" binding texts
#	include <cctype>
#endif

namespace
{
	// ---- module-scope runtime state -------------------------------------------

	lua_State* L = nullptr;
	SAMLua::SandboxConfig g_cfg;

	// Custom allocator bookkeeping — enforces the hard memory cap.
	struct AllocState
	{
		std::size_t used  = 0;
		std::size_t limit = 10u * 1024u * 1024u;
		std::size_t peak  = 0;
	};
	AllocState g_alloc;

	// Instruction-budget watchdog state, reset before every protected call.
	struct HookState
	{
		long long elapsed  = 0;      // instructions counted this callback (interval-granular)
		long long budget   = 500000; // per-callback ceiling
		int       interval = 1000;   // hook granularity
		bool      tripped  = false;  // set true when the watchdog fired
	};
	HookState g_hook;
	int g_callDepth = 0; // reentrancy depth for protectedCall (nesting-aware watchdog)

	// One loaded behavior script.
	struct Script
	{
		std::string path;
		std::string ns;               // owning mod namespace (per-mod data / custom hooks / timers)
		int  callbackRef = LUA_NOREF; // registry ref to its on_event function
		int  tickRef     = LUA_NOREF; // registry ref to its on_tick function (v0.7.0), or NOREF
		bool enabled     = false;
	};
	std::vector<Script> g_scripts;

	// Namespace of the script currently executing — set around every callback and
	// top-level load so host APIs (sam_save_data, custom hooks, timers) can attribute
	// a call to the mod that made it.
	std::string g_currentNs;

	// ---- multiplayer: every sam_* function goes through one trampoline ----------------------
	//
	// samLuaRegister keeps the real function beside its multiplayer contract
	// (sam_mp_contracts.inc; the kinds are explained in sam_net.hpp) and registers a closure
	// that applies the contract before the function runs: refuse on the wrong machine, carry
	// the call to the machine of the player it is about, or just run it. A function registered
	// any other way would skip all of that, so tools/gen_api_docs.mjs refuses a raw
	// lua_setglobal of a sam_ name.
	struct SAMLuaFn
	{
		std::string name;
		lua_CFunction fn = nullptr;
#ifdef SAM_LUA_HAVE_BARONY
		const SAMNet::Contract* contract = nullptr;
#endif
	};
	std::vector<SAMLuaFn> g_luaFns;
	std::map<std::string, int> g_luaFnIndex;

#ifdef SAM_LUA_HAVE_BARONY
	// A script value, for the far side. Only plain data crosses (see SAMNet::Tag).
	bool samLuaEncode(lua_State* Ls, int idx, SAMNet::Writer& w, int depth, std::string& err)
	{
		idx = lua_absindex(Ls, idx);
		switch ( lua_type(Ls, idx) )
		{
			case LUA_TNONE:
			case LUA_TNIL:
				w.u8(SAMNet::Tag::Nil);
				return true;
			case LUA_TBOOLEAN:
				w.u8(lua_toboolean(Ls, idx) ? SAMNet::Tag::True : SAMNet::Tag::False);
				return true;
			case LUA_TNUMBER:
				if ( lua_isinteger(Ls, idx) ) { w.u8(SAMNet::Tag::Int); w.i64((long long)lua_tointeger(Ls, idx)); }
				else { w.u8(SAMNet::Tag::Num); w.f64((double)lua_tonumber(Ls, idx)); }
				return true;
			case LUA_TSTRING:
			{
				std::size_t n = 0;
				const char* s = lua_tolstring(Ls, idx, &n);
				if ( n > 65535 ) { err = "a string over 64 KB"; return false; }
				w.u8(SAMNet::Tag::Str);
				w.str16(std::string(s, n));
				return true;
			}
			case LUA_TTABLE:
			{
				if ( depth >= SAMNet::MAX_DEPTH ) { err = "tables nested more than 4 deep"; return false; }
				if ( !lua_checkstack(Ls, 4) ) { err = "too deep"; return false; }
				// A sequence 1..n and nothing else crosses as an array; anything else as a map.
				const lua_Integer n = (lua_Integer)lua_rawlen(Ls, idx);
				lua_Integer count = 0;
				bool sequence = true;
				lua_pushnil(Ls);
				while ( lua_next(Ls, idx) != 0 )
				{
					++count;
					if ( !lua_isinteger(Ls, -2) || lua_tointeger(Ls, -2) < 1 || lua_tointeger(Ls, -2) > n ) { sequence = false; }
					lua_pop(Ls, 1);
				}
				if ( count > 4096 ) { err = "a table of more than 4096 entries"; return false; }
				if ( sequence && count == n )
				{
					w.u8(SAMNet::Tag::Arr);
					w.u16((std::uint16_t)n);
					for ( lua_Integer i = 1; i <= n; ++i )
					{
						lua_rawgeti(Ls, idx, i);
						const bool ok = samLuaEncode(Ls, -1, w, depth + 1, err);
						lua_pop(Ls, 1);
						if ( !ok ) { return false; }
					}
					return true;
				}
				w.u8(SAMNet::Tag::Map);
				w.u16((std::uint16_t)count);
				lua_pushnil(Ls);
				while ( lua_next(Ls, idx) != 0 )
				{
					// Key at -2, value at -1. lua_tolstring on a key that IS a string does not
					// convert it, so lua_next stays valid.
					if ( lua_type(Ls, -2) == LUA_TSTRING )
					{
						std::size_t kn = 0;
						const char* k = lua_tolstring(Ls, -2, &kn);
						w.u8(SAMNet::Tag::Str);
						w.str16(std::string(k, std::min<std::size_t>(kn, 65535)));
					}
					else if ( lua_isinteger(Ls, -2) )
					{
						w.u8(SAMNet::Tag::Int);
						w.i64((long long)lua_tointeger(Ls, -2));
					}
					else
					{
						lua_pop(Ls, 2);
						err = "a table key that is neither a string nor a whole number";
						return false;
					}
					if ( !samLuaEncode(Ls, -1, w, depth + 1, err) ) { lua_pop(Ls, 2); return false; }
					lua_pop(Ls, 1);
				}
				return true;
			}
			default:
				err = std::string("a ") + lua_typename(Ls, lua_type(Ls, idx));
				return false;
		}
	}

	// Push one value read back off the wire.
	bool samLuaDecode(lua_State* Ls, SAMNet::Reader& r, int depth)
	{
		if ( !lua_checkstack(Ls, 4) ) { return false; }
		const std::uint8_t t = r.u8();
		if ( !r.ok ) { return false; }
		switch ( t )
		{
			case SAMNet::Tag::Nil:   lua_pushnil(Ls); return true;
			case SAMNet::Tag::False: lua_pushboolean(Ls, 0); return true;
			case SAMNet::Tag::True:  lua_pushboolean(Ls, 1); return true;
			case SAMNet::Tag::Int:   { const long long v = r.i64(); lua_pushinteger(Ls, (lua_Integer)v); return r.ok; }
			case SAMNet::Tag::Num:   { const double v = r.f64(); lua_pushnumber(Ls, (lua_Number)v); return r.ok; }
			case SAMNet::Tag::Str:
			{
				const std::string s = r.str16();
				if ( !r.ok ) { return false; }
				lua_pushlstring(Ls, s.data(), s.size());
				return true;
			}
			case SAMNet::Tag::Arr:
			{
				if ( depth >= SAMNet::MAX_DEPTH ) { return false; }
				const int n = r.u16();
				lua_createtable(Ls, n, 0);
				for ( int i = 1; i <= n; ++i )
				{
					if ( !samLuaDecode(Ls, r, depth + 1) ) { lua_pop(Ls, 1); return false; }
					lua_rawseti(Ls, -2, i);
				}
				return r.ok;
			}
			case SAMNet::Tag::Map:
			{
				if ( depth >= SAMNet::MAX_DEPTH ) { return false; }
				const int n = r.u16();
				lua_createtable(Ls, 0, n);
				for ( int i = 0; i < n; ++i )
				{
					if ( !samLuaDecode(Ls, r, depth + 1) ) { lua_pop(Ls, 1); return false; }
					if ( lua_isnil(Ls, -1) ) { lua_pop(Ls, 2); return false; }
					if ( !samLuaDecode(Ls, r, depth + 1) ) { lua_pop(Ls, 2); return false; }
					lua_rawset(Ls, -3);
				}
				return r.ok;
			}
			default:
				return false;
		}
	}

	// What a refused call returns, or (forwarded == true) what a call carried to another
	// machine returns here: `true` for a function that answers success with a boolean.
	int samLuaPushRefusal(lua_State* Ls, SAMNet::Refusal r, bool forwarded)
	{
		switch ( r )
		{
			case SAMNet::Refusal::False: lua_pushboolean(Ls, forwarded ? 1 : 0); return 1;
			case SAMNet::Refusal::Nil:   lua_pushnil(Ls); return 1;
			case SAMNet::Refusal::Zero:  lua_pushinteger(Ls, 0); return 1;
			case SAMNet::Refusal::Empty: lua_newtable(Ls); return 1;
			case SAMNet::Refusal::None:  return 0;
		}
		return 0;
	}

	// Arguments 1..top for the far side, as [u8 count][values]. `dropFrom` (1-based, 0 = none)
	// and everything after it is left out; `replaceArg` (1-based, 0 = none) is written as the
	// integer `replaceValue` instead of what the script passed.
	bool samLuaEncodeArgs(lua_State* Ls, int dropFrom, int replaceArg, long long replaceValue, std::string& out, std::string& err)
	{
		SAMNet::Writer w;
		int top = lua_gettop(Ls);
		if ( dropFrom > 0 && top >= dropFrom ) { top = dropFrom - 1; }
		if ( top > 255 ) { err = "more than 255 arguments"; return false; }
		w.u8((std::uint8_t)top);
		for ( int i = 1; i <= top; ++i )
		{
			if ( i == replaceArg ) { w.u8(SAMNet::Tag::Int); w.i64(replaceValue); continue; }
			std::string why;
			if ( !samLuaEncode(Ls, i, w, 0, why) ) { err = "argument " + std::to_string(i) + " is " + why; return false; }
		}
		out = std::move(w.buf);
		return true;
	}

	// A screen function that NAMES its player in argument `arg` (not the optional trailing one
	// the trampoline strips before the body sees it). Those are the calls a script may write
	// with -1 for "every player": the camera set, the screen flash, the impact frame, the
	// pictures. Each of them draws into ONE player's own viewport, which is why -1 has to be
	// expanded to a real seat rather than handed to the body.
	bool samScreenNamesPlayer(const SAMNet::Contract* c)
	{
		return c->kind == SAMNet::Kind::Screen && c->target == SAMNet::Target::Player && c->arg > 0;
	}

	bool samLuaPlayerArgIsEveryone(lua_State* Ls, const SAMNet::Contract* c)
	{
		return c->arg > 0 && lua_gettop(Ls) >= (int)c->arg && lua_isnumber(Ls, (int)c->arg)
			&& (long long)lua_tointeger(Ls, (int)c->arg) == -1;
	}

	void samLuaSetPlayerArg(lua_State* Ls, const SAMNet::Contract* c, int player)
	{
		lua_pushinteger(Ls, (lua_Integer)player);
		lua_replace(Ls, (int)c->arg);
	}

	// The seats sitting at THIS machine. In a networked game that is one player; on a couch it
	// is up to four, and "every player" has to mean all of them. Falls back to this machine's
	// own slot so a call is never simply dropped.
	int samLocalSeats(int (&out)[MAXPLAYERS])
	{
		int n = 0;
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( !client_disconnected[p] && players[p] && players[p]->isLocalPlayer() ) { out[n++] = p; }
		}
		if ( n == 0 ) { out[n++] = clientnum; }
		return n;
	}

	// One error line per `all` function whose arguments could not be carried. Not warnOnce,
	// because this stops the call happening on any machine and a modder has to see it; not a
	// bare SAM_ERROR either, because a script that makes the call from on_tick would fill the
	// log fifty times a second.
	std::set<std::string> g_allEncodeSaid;

	int samLuaTrampoline(lua_State* Ls)
	{
		const int idx = (int)lua_tointeger(Ls, lua_upvalueindex(1));
		if ( idx < 0 || idx >= (int)g_luaFns.size() ) { return 0; }
		const lua_CFunction fn = g_luaFns[idx].fn;
		const SAMNet::Contract* c = g_luaFns[idx].contract;
		if ( !c ) { return fn(Ls); }

		// A trailing player the body never had: gone before the body runs, in singleplayer too,
		// so a script behaves the same everywhere.
		const bool opt = (c->target == SAMNet::Target::OptPlayer);
		auto stripOpt = [&]() { if ( opt && lua_gettop(Ls) >= (int)c->arg ) { lua_settop(Ls, (int)c->arg - 1); } };

		// A player argument that is PRESENT but names no seat is a mistake, not a default. It
		// used to fall into the same -1 that "no player given" falls into, and every kind then
		// read that as "this machine": sam_ui_open("shop", 40, 40, 300, 200, "Shop", true, 5)
		// opened the panel on the caller's own screen and answered true, and for an OptPlayer
		// function the bad index was stripped before the body could complain about it. -1 still
		// means every player; anything else outside 0..MAXPLAYERS-1, and anything that is not a
		// number at all, is refused and said once.
		//
		// Checked before the singleplayer branch on purpose, so the same call is refused in a
		// solo game and in a co-op game.
		if ( c->target == SAMNet::Target::Player || c->target == SAMNet::Target::OptPlayer )
		{
			const int pi = (int)c->arg;
			if ( pi > 0 && lua_gettop(Ls) >= pi && !lua_isnil(Ls, pi) )
			{
				const std::string& fname = g_luaFns[idx].name;
				const std::string seats = "0.." + std::to_string(MAXPLAYERS - 1) + ", or -1 for every player";
				if ( !lua_isnumber(Ls, pi) )
				{
					SAMNet::warnOnce(fname + "|argplayer", fname + ": argument " + std::to_string(pi)
						+ " names the player and has to be a number (" + seats + "). Nothing was done.");
					return samLuaPushRefusal(Ls, c->refusal, false);
				}
				// Through lua_tonumber, not lua_tointeger: lua_tointeger answers 0 for 2.7, and
				// player 0 is a real seat, so a fractional index used to draw on the first
				// player's screen in silence. The range test also rejects a NaN and keeps the
				// cast below in range.
				const lua_Number pn = lua_tonumber(Ls, pi);
				const bool integral = ( pn >= -9.0e15 && pn <= 9.0e15 && (lua_Number)(long long)pn == pn );
				const long long pv = integral ? (long long)pn : 0;
				if ( !integral || pv < -1 || pv >= MAXPLAYERS )
				{
					SAMNet::warnOnce(fname + "|argplayer", fname + ": there is no player "
						+ ( integral ? std::to_string(pv) : std::to_string((double)pn) )
						+ " (" + seats + "). Nothing was done.");
					return samLuaPushRefusal(Ls, c->refusal, false);
				}
			}
		}

		if ( multiplayer == SINGLE || SAMNet::inForwardedCall() )
		{
			// "Every player" is every player on this machine when there is nobody else on the
			// network. The multiplayer host expands -1 the same way further down (forward to each
			// remote player, run the body here for its own), so without this a screen call
			// written as -1 worked in a co-op game and quietly did nothing in singleplayer,
			// which is where mods are written.
			//
			// A splitscreen game is multiplayer == SINGLE, so this branch is the only one it can
			// take: rewriting -1 to clientnum flashed one quarter of the screen and left the
			// other seats with nothing at all. With one seat -- every ordinary singleplayer game
			// -- the loop below runs exactly once and this is the behaviour it always had.
			// Nothing goes on the wire either way, so a stock client cannot see any of it.
			if ( samScreenNamesPlayer(c) && samLuaPlayerArgIsEveryone(Ls, c) )
			{
				int seats[MAXPLAYERS];
				const int nSeats = samLocalSeats(seats);
				if ( nSeats == 1 )
				{
					samLuaSetPlayerArg(Ls, c, seats[0]);
					stripOpt();
					return fn(Ls);
				}
				// More than one seat: the body runs once per seat, and the script is told whether
				// any of them took it. Every one of these functions answers with a single
				// true/false, so one answer still says what the script needs to know.
				bool any = false;
				const int base = lua_gettop(Ls);
				for ( int i = 0; i < nSeats; ++i )
				{
					samLuaSetPlayerArg(Ls, c, seats[i]);
					const int n = fn(Ls);
					if ( n > 0 && lua_toboolean(Ls, lua_gettop(Ls) - n + 1) ) { any = true; }
					lua_settop(Ls, base);
				}
				lua_pushboolean(Ls, any ? 1 : 0);
				return 1;
			}
			stripOpt();
			return fn(Ls);
		}

		// Which player is this call about?
		int player = -1;
		bool remoteItem = false;
		std::uint32_t ownerUid = 0;
		const bool hasArg = c->arg > 0 && lua_gettop(Ls) >= (int)c->arg && !lua_isnil(Ls, (int)c->arg);
		switch ( c->target )
		{
			case SAMNet::Target::Player:
			case SAMNet::Target::OptPlayer:
				if ( hasArg && lua_isnumber(Ls, (int)c->arg) )
				{
					const long long v = (long long)lua_tointeger(Ls, (int)c->arg);
					player = (v == -1) ? SAMNet::EVERYONE : ((v >= 0 && v < MAXPLAYERS) ? (int)v : -1);
				}
				else if ( !hasArg && (c->kind == SAMNet::Kind::Screen || c->kind == SAMNet::Kind::Read) )
				{
					player = SAMNet::defaultScreenPlayer();
				}
				break;
			case SAMNet::Target::Uid:
				if ( hasArg && lua_isnumber(Ls, (int)c->arg) ) { player = SAMNet::playerOfUid((std::uint32_t)lua_tointeger(Ls, (int)c->arg)); }
				break;
			case SAMNet::Target::Item:
				if ( hasArg && lua_isnumber(Ls, (int)c->arg) )
				{
					int p = -1;
					std::uint32_t ou = 0;
					if ( SAMNet::routeItem((std::uint32_t)lua_tointeger(Ls, (int)c->arg), p, ou) ) { remoteItem = true; player = p; ownerUid = ou; }
				}
				break;
			default:
				break;
		}

		const SAMNet::Decision d = SAMNet::decide(*c, player, remoteItem);
		const std::string name = g_luaFns[idx].name;
		switch ( d.route )
		{
			case SAMNet::Route::Here:
				// Only a client gets here with "every player" still in the arguments: expanding
				// -1 to the other machines is the host's job, so on a client it means this
				// machine's own screen -- exactly what -1 already means for a HUD line or a
				// panel. Without this the body was handed -1, every one of these bodies rejects
				// it, and the call drew nothing and said nothing.
				if ( samScreenNamesPlayer(c) && player == SAMNet::EVERYONE )
				{
					samLuaSetPlayerArg(Ls, c, clientnum);
				}
				stripOpt();
				return fn(Ls);

			case SAMNet::Route::Refuse:
				SAMNet::warnOnce(name + "|" + d.why, name + ": " + d.why);
				return samLuaPushRefusal(Ls, c->refusal, false);

			case SAMNet::Route::Forward:
			{
				std::string args, err;
				if ( !samLuaEncodeArgs(Ls, opt ? (int)c->arg : 0, remoteItem ? (int)c->arg : 0, (long long)ownerUid, args, err) )
				{
					SAMNet::warnOnce(name + "|encode", name + ": cannot be sent to player " + std::to_string(d.player) + "'s machine: " + err + ".");
					return samLuaPushRefusal(Ls, c->refusal, false);
				}
				// What this answers is what the reference promises: true once the call really is
				// on its way. forwardCall refuses a frame over 16 KB and a queue that cannot take
				// another, and a script told "sent" about a panel that was never built has no way
				// left to find out.
				const bool sent = SAMNet::forwardCall(d.player, 'L', g_currentNs, name, args);
				return samLuaPushRefusal(Ls, c->refusal, sent);
			}

			case SAMNet::Route::ForwardAndHere:
			{
				if ( c->target == SAMNet::Target::Player && player == SAMNet::EVERYONE )
				{
					// "Every player" for a function that names its player: each machine gets the
					// call about its own player, and this one runs it for its own.
					for ( int p = 1; p < MAXPLAYERS; ++p )
					{
						if ( !SAMNet::isRemotePlayer(p) ) { continue; }
						if ( !SAMNet::peerMayHaveSam(p) )
						{
							// Naming that player gets a warning; "every player" used to skip them
							// in silence, so the same mistake told the modder opposite things
							// depending on which form they wrote. Keyed on the player, and on the
							// same key forwardCallToAll uses, so it stays one line per player.
							SAMNet::warnOnce("net:stock:" + std::to_string(p), "Player " + std::to_string(p)
								+ "'s game is not running S.A.M scripts, so it sees none of what the mod shows or changes for everyone.");
							continue;
						}
						std::string args, err;
						if ( samLuaEncodeArgs(Ls, 0, (int)c->arg, p, args, err) ) { SAMNet::forwardCall(p, 'L', g_currentNs, name, args); }
					}
					lua_pushinteger(Ls, (lua_Integer)clientnum);
					lua_replace(Ls, (int)c->arg);
					return fn(Ls);
				}
				std::string args, err;
				if ( samLuaEncodeArgs(Ls, opt ? (int)c->arg : 0, 0, 0, args, err) )
				{
					SAMNet::forwardCallToAll('L', g_currentNs, name, args, c->kind == SAMNet::Kind::All);
				}
				else if ( c->kind == SAMNet::Kind::All )
				{
					// An `all` call changes a table every machine keeps its OWN copy of. Running
					// the body here after the arguments turned out to be uncarryable would patch
					// the host's table and nobody else's, and every client would then build that
					// character from a different table -- the one divergence this kind exists to
					// prevent. So it is refused on this machine too: nothing is patched anywhere,
					// which is at least the same everywhere, and the script is told false.
					// Singleplayer returns long before this branch, so a mod tested there keeps
					// working; it says this the first time it is hosted, which is when it matters.
					if ( g_allEncodeSaid.insert(name).second )
					{
						SAM_ERROR("LUA", name + ": " + err + ", so it cannot be sent to the other players -- and it was"
							" NOT applied on this machine either, because a table every machine keeps its own copy of has"
							" to change on all of them or on none. Keep functions and anything else that cannot cross the"
							" network out of the table you pass.");
					}
					return samLuaPushRefusal(Ls, c->refusal, false);
				}
				else
				{
					SAMNet::warnOnce(name + "|encode", name + ": cannot be sent to the other players: " + err + ".");
				}
				stripOpt();
				return fn(Ls);
			}
		}
		stripOpt();
		return fn(Ls);
	}

	// Runs on a client: a call the host carried here (sam_net.cpp checked it is one the host
	// may send). The function itself is called, never the global of that name, so a script
	// that reassigned sam_hud_text cannot intercept what the host sent.
	void samLuaRunForwarded(const std::string& ns, const std::string& name, const std::string& args)
	{
		if ( !L ) { return; }
		auto it = g_luaFnIndex.find(name);
		if ( it == g_luaFnIndex.end() ) { return; }
		const lua_CFunction fn = g_luaFns[it->second].fn;
		SAMNet::Reader r(args);
		const int n = r.u8();
		const int base = lua_gettop(L);
		if ( !r.ok || !lua_checkstack(L, n + 4) ) { return; }
		lua_pushcfunction(L, fn);
		for ( int i = 0; i < n; ++i )
		{
			if ( !samLuaDecode(L, r, 0) )
			{
				lua_settop(L, base);
				SAMNet::warnOnce("net:decode:" + name, "The host sent '" + name + "' with arguments this machine could not read; ignored.");
				return;
			}
		}
		const std::string savedNs = g_currentNs;
		g_currentNs = ns;
		if ( lua_pcall(L, n, 0, 0) != LUA_OK )
		{
			const char* e = lua_tostring(L, -1);
			SAM_WARN("NET", name + " (sent by the host) failed on this machine: " + std::string(e ? e : "?"));
		}
		g_currentNs = savedNs;
		lua_settop(L, base);
	}
#endif

	// Register one sam_* function. See the section comment above.
	void samLuaRegister(lua_State* Ls, const char* name, lua_CFunction fn)
	{
#ifdef SAM_LUA_HAVE_BARONY
		int idx = 0;
		auto it = g_luaFnIndex.find(name);
		if ( it == g_luaFnIndex.end() )
		{
			idx = (int)g_luaFns.size();
			SAMLuaFn f;
			f.name = name;
			f.fn = fn;
			f.contract = SAMNet::contractFor(name);
			if ( !f.contract )
			{
				SAM_WARN("NET", std::string(name) + " has no multiplayer contract (sam_mp_contracts.inc); it runs unchecked on every machine.");
			}
			g_luaFns.push_back(std::move(f));
			g_luaFnIndex[name] = idx;
		}
		else
		{
			idx = it->second;
			g_luaFns[idx].fn = fn;
		}
		lua_pushinteger(Ls, (lua_Integer)idx);
		lua_pushcclosure(Ls, samLuaTrampoline, 1);
		lua_setglobal(Ls, name);
#else
		lua_pushcfunction(Ls, fn);
		lua_setglobal(Ls, name);
#endif
	}

	// ---- the engine's `hit` global, borrowed and put back -------------------------------
	//
	// `hit` is ONE global with six fields and the engine is often mid-way through using it when
	// a script runs. clipMove's first statement is `hit.entity = NULL;` and it writes hit.side on
	// every success path, so any SAM call that moves something stamps on it. The engine's own
	// attempt to be careful saves a single field (entity.cpp:17923 `Entity* ohit = hit.entity;`),
	// which is not enough -- so this saves the whole struct, and being RAII it cannot be forgotten
	// on an early return.
	struct SAMHitGuard
	{
		hit_t saved;
		SAMHitGuard() : saved(hit) {}
		~SAMHitGuard() { hit = saved; }
	};

	// ---- damage immunity, declared by a script ------------------------------------------
	//
	// Checked at the top of Entity::modHP, which is the single choke point every point of damage
	// in the game goes through -- both S.A.M damage events already fire from there.
	//
	// Held by UID, and CLEARED ON EVERY FLOOR AND EVERY NEW RUN -- but NOT for the reason this
	// comment first gave. I wrote that uids restart from 1 on each level. They do not: entity_uids
	// is reset only when a run starts (menu.cpp:8715, 9486) and otherwise counts up for the whole
	// game. The real reason is better: the engine ROLLS THE COUNTER BACK for throwaway particles
	// it does not want to spend a uid on (`entity_uids--` at entity.cpp:17172 and :23988,
	// game.cpp:1443 and :3341), so the very next entity created takes that number. A uid a script
	// remembered can therefore name something else entirely, within a single floor. Clearing on
	// every boundary bounds how long a stale entry can survive to do it.
	//
	// Empty in a vanilla game, so the cost on the engine's hot path is one empty() test.
	std::set<Uint32> g_damageImmune;

	// ---- the entity flag table, shared by both runtimes ----------------------------------
	//
	// Barony's flags are a bool[24] indexed by the #defines in entity.hpp:21-40. Scripts name
	// them; nothing in the API takes the index, because the array's neighbours in the class are
	// `char* string`, `light_t* light` and `list_t children`, so an out-of-range index is a write
	// over live pointers. That exact hole shipped on the wire once (net.cpp:3158, now closed).
	//
	// `settable` is false where the flag is not a script's to own. Two of those have a proper
	// function instead and the refusal names it; two are the network sweep's own bookkeeping.
#ifdef SAM_LUA_HAVE_BARONY
	struct SAMEntityFlag
	{
		const char* name;
		int index;
		bool settable;
		const char* instead;   // the function that owns this flag, if one does
		const char* why;       // said to the modder when settable is false and instead is null
	};

	static const SAMEntityFlag g_entityFlags[] =
	{
		{ "BRIGHT",              BRIGHT,              true,  nullptr, nullptr },
		{ "INVISIBLE",           INVISIBLE,           false, "sam_set_visible",
			nullptr },
		{ "NOUPDATE",            NOUPDATE,            false, nullptr,
			"it is how the network sweep decides an entity needs no further updates, so owning it"
			" can freeze the thing on every client while the host watches it move normally" },
		{ "UPDATENEEDED",        UPDATENEEDED,        false, nullptr,
			"it is the network sweep's own dirty bit, and every function that changes an entity"
			" already sets it for you" },
		{ "GENIUS",              GENIUS,              true,  nullptr, nullptr },
		{ "OVERDRAW",            OVERDRAW,            true,  nullptr, nullptr },
		{ "SPRITE",              SPRITE,              true,  nullptr, nullptr },
		{ "BLOCKSIGHT",          BLOCKSIGHT,          true,  nullptr, nullptr },
		{ "BURNING",             BURNING,             false, "sam_set_on_fire",
			nullptr },
		{ "BURNABLE",            BURNABLE,            true,  nullptr, nullptr },
		{ "UNCLICKABLE",         UNCLICKABLE,         true,  nullptr, nullptr },
		{ "PASSABLE",            PASSABLE,            true,  nullptr, nullptr },
		{ "USERFLAG1",           USERFLAG1,           true,  nullptr, nullptr },
		{ "USERFLAG2",           USERFLAG2,           true,  nullptr, nullptr },
		{ "INVISIBLE_DITHER",    INVISIBLE_DITHER,    true,  nullptr, nullptr },
		{ "NOCLIP_WALLS",        NOCLIP_WALLS,        true,  nullptr, nullptr },
		{ "NOCLIP_CREATURES",    NOCLIP_CREATURES,    true,  nullptr, nullptr },
		{ "ENTITY_SKIP_CULLING", ENTITY_SKIP_CULLING, true,  nullptr, nullptr },
		{ "STASIS_DITHER",       STASIS_DITHER,       false, nullptr,
			"Entity::handleEffects rewrites it from the stasis effect on every frame, so setting it"
			" here would be undone before the next one drew" },
	};

	static std::string samEntityFlagList()
	{
		std::string out;
		for ( const SAMEntityFlag& f : g_entityFlags )
		{
			if ( !out.empty() ) { out += ", "; }
			out += f.name;
		}
		return out;
	}

	// -1 for an unknown name, or for a read-only flag when forWrite is true. Warns in both cases,
	// because a silent -1 would be as useless as the wrong answer.
	int samResolveEntityFlag(const char* name, bool forWrite, const char* who)
	{
		const std::string want = name ? name : "";
		for ( const SAMEntityFlag& f : g_entityFlags )
		{
			if ( want != f.name ) { continue; }
			if ( forWrite && !f.settable )
			{
				if ( f.instead )
				{
					SAM_WARN("LUA", std::string(who) + ": " + f.name + " is not set through this"
						" function. Use " + f.instead + ", which knows the extra rules that go"
						" with it.");
				}
				else
				{
					SAM_WARN("LUA", std::string(who) + ": " + f.name + " is not a script's to set,"
						" because " + (f.why ? f.why : "the engine owns it") + ".");
				}
				return -1;
			}
			return f.index;
		}
		SAM_WARN("LUA", std::string(who) + ": there is no entity flag called \"" + want + "\"."
			" The flags are: " + samEntityFlagList() + ".");
		return -1;
	}
#else
	int samResolveEntityFlag(const char*, bool, const char*) { return -1; }
#endif

	// ---- one entity resolver, for every function that WRITES to an entity ----------------
	//
	// uidToEntity is a bare map lookup with no validation (entity.cpp:3318). Two things get
	// through it that no script should ever be allowed to write to.
	//
	// A SENTINEL uid. Real uids start at 1, but the engine hands out 0, -2, -3 and -4 as shared
	// markers -- gibs, sparks, leaves, torch flames, dozens of live entities carrying the same
	// number at once. Writing through one mutates whichever the map currently points at, and
	// list.cpp erases the map entry by uid, so the first of them to die evicts the entry for all.
	//
	// A LIMB. A bodypart's position, size and flags are rewritten from its owner every frame, so
	// a write to one vanishes without a word. Detected BY POINTER against the owner's bodyparts
	// vector: comparing by value cannot tell two limbs apart, which is the same lesson itemCompare
	// taught when destroying a spare sword unequipped the worn one.
	bool samIsBodypart(const Entity* e)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( !e ) { return false; }
		// A limb does not always know its owner: the player's HUD limbs and the death-ghost limbs
		// are pushed onto bodyparts with parent left at 0, so the parent lookup below misses them
		// entirely and they resolved as ordinary writable entities. Check the players directly
		// first -- there are at most four, so this costs nothing.
		for ( int samP = 0; samP < MAXPLAYERS; ++samP )
		{
			if ( !players[samP] || !players[samP]->entity ) { continue; }
			for ( const Entity* bp : players[samP]->entity->bodyparts )
			{
				if ( bp == e ) { return true; }
			}
		}
		if ( e->parent == 0 ) { return false; }
		Entity* p = uidToEntity((Sint32)e->parent);
		if ( !p ) { return false; }
		for ( const Entity* bp : p->bodyparts ) { if ( bp == e ) { return true; } }
#else
		(void)e;
#endif
		return false;
	}

	Entity* samResolveEntityRead(long long uid, const char* who)
	{
#ifdef SAM_LUA_HAVE_BARONY
		// The upper bound is not decoration. A uid is a Uint32, so the sentinel -3 IS the value
		// 4294967293 -- which sails past `uid <= 0` on a long long and is turned straight back
		// into -3 by the (Sint32) cast below. sam_remove_entity(4294967293) therefore walked
		// through this guard, freed whichever gib held the shared marker, and list_RemoveNode
		// then erased map.entities_map[-3] for every other -3 entity alive. Real uids come from
		// a counter that starts at 1 and never approaches 2^31, so nothing legitimate is refused.
		if ( uid <= 0 || uid > 0x7FFFFFFFLL )
		{
			SAM_WARN("LUA", std::string(who) + ": uid " + std::to_string(uid) + " is not one entity."
				" 0 and the negatives are shared engine markers that dozens of gibs and sparks hold"
				" at once, and anything past 2147483647 is the same markers written unsigned.");
			return nullptr;
		}
		return uidToEntity((Sint32)uid);
#else
		(void)uid; (void)who; return nullptr;
#endif
	}

	// The same guard for READERS, without the warning. A writer that is handed a sentinel uid has
	// made a mistake worth saying out loud; a reader has usually just been handed this API's own
	// "no entity" value back -- sam_get_monster_target answers 0 for a monster chasing nobody --
	// so nil is the true answer to "where is nothing", and a log line would be noise once a frame.
	Entity* samResolveEntityQuiet(long long uid)
	{
#ifdef SAM_LUA_HAVE_BARONY
		// Bounded above as well as below; see samResolveEntityRead for why 4294967293 is -3.
		if ( uid <= 0 || uid > 0x7FFFFFFFLL ) { return nullptr; }
		return uidToEntity((Sint32)uid);
#else
		(void)uid; return nullptr;
#endif
	}

	Entity* samResolveWritable(long long uid, const char* who)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", std::string(who) + " refused: host only.");
			return nullptr;
		}
		Entity* e = samResolveEntityRead(uid, who);
		if ( !e ) { return nullptr; }
		if ( samIsBodypart(e) )
		{
			SAM_WARN("LUA", std::string(who) + ": uid " + std::to_string(uid) + " is a limb. Its"
				" position, size and flags are rewritten from its owner every frame, so this would"
				" have no lasting effect. Pass the owner's uid.");
			return nullptr;
		}
		return e;
#else
		(void)uid; (void)who; return nullptr;
#endif
	}

	// ---- entity removal, deferred ---------------------------------------------------------
	//
	// Freeing an Entity while a script runs is a use-after-free, for the same reason freeing an
	// Item was: the script only runs because the engine called into it, and the caller is still
	// holding the pointer. Entity::attack calls modHP and then keeps dereferencing hit.entity for
	// another ~160 lines; modHP is what fires the damage events a mod handles.
	//
	// Queued by UID, never by pointer -- a stale pointer is undetectable where a stale uid simply
	// fails to resolve. Re-resolved and re-checked on drain, exactly as the item queue does.
	std::vector<Uint32> g_pendingRemove;

	bool samQueueRemoveEntity(Uint32 uid, const char* who)
	{
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveWritable((long long)uid, who);
		if ( !e ) { return false; }
		if ( e->behavior == &actPlayer )
		{
			SAM_WARN("LUA", std::string(who) + " refused: cannot remove a player.");
			return false;
		}
		for ( Uint32 q : g_pendingRemove ) { if ( q == uid ) { return true; } }   // already queued
		g_pendingRemove.push_back(uid);
		return true;
#else
		(void)uid; (void)who; return false;
#endif
	}




	// A REQUIRED flag. Batch 2 settled what a missing flag means: sam_set_item_droppable(uid)
	// refuses rather than guessing, because guessing wrong on a flag is invisible until the
	// wrong thing is on screen. Same rule here.
	static bool samBoolReq(lua_State* Ls, int idx, const char* who, bool* out)
	{
		if ( lua_isnoneornil(Ls, idx) )
		{
			SAM_ERROR("LUA", std::string(who) + ": argument " + std::to_string(idx)
				+ " (true or false) is required.");
			return false;
		}
		// lua_type, NOT lua_isnumber. lua_isnumber answers TRUE for a STRING that converts to a
		// number, so the string "0" took the numeric branch and came out false here while
		// JS_ToBool("0") is true -- every non-empty string is truthy in JavaScript. These helpers
		// exist to give the two runtimes ONE rule, and gating on lua_isnumber quietly broke that
		// on the very argument type they were written to unify. lua_type never coerces.
		if ( lua_type(Ls, idx) == LUA_TNUMBER )
		{
			const double d = lua_tonumber(Ls, idx);
			*out = ( d != 0.0 && d == d );   // d == d excludes NaN, which JS_ToBool calls false
			return true;
		}
		if ( lua_type(Ls, idx) == LUA_TSTRING )
		{
			size_t n = 0; lua_tolstring(Ls, idx, &n);
			*out = ( n != 0 );               // JavaScript: "" is false, every other string true
			return true;
		}
		*out = lua_toboolean(Ls, idx) != 0;
		return true;
	}

	// A flag argument, matching the JS twin exactly. lua_toboolean answers TRUE for the number
	// 0, which JS_ToBool does not, so a script passing 0 meaning "off" got "on" in Lua and "off"
	// in JavaScript -- a divergence that is live in shipped code (sam_set_visible).
	static bool samBoolArg(lua_State* Ls, int idx, bool dflt)
	{
		if ( lua_isnoneornil(Ls, idx) ) { return dflt; }
		// See samBoolReq: lua_isnumber is true for a numeric STRING, which put "0" on the numeric
		// branch and disagreed with JS_ToBool. Branch on the real type instead.
		if ( lua_type(Ls, idx) == LUA_TNUMBER )
		{
			const double d = lua_tonumber(Ls, idx);
			return ( d != 0.0 && d == d );
		}
		if ( lua_type(Ls, idx) == LUA_TSTRING )
		{
			size_t n = 0; lua_tolstring(Ls, idx, &n);
			return ( n != 0 );
		}
		return lua_toboolean(Ls, idx) != 0;
	}



	// A bare asset id means "one of mine". sam_show_image has always accepted that and
	// namespaced it to the calling mod; models and sounds demanded the full "ns:id", so inside a
	// single mod sam_show_image(p, "banner") worked and sam_set_model(uid, "myship") did not.
	//
	// Tried as a FALLBACK, after the id exactly as written, so a vanilla model path or an id that
	// already resolves is never shadowed by a same-named asset of the calling mod.
	// WHOSE namespace is an argument, defaulting to the Lua runtime's own. Reading g_currentNs
	// unconditionally meant this fallback never fired for a JavaScript mod (that runtime keeps a
	// separate current-namespace global), and that in the three spawn functions both runtimes
	// share, a JS callback invoked from a Lua event resolved ITS bare id inside the firing LUA
	// mod's namespace -- one mod's asset name answered out of another mod's registry.
	int samResolveModelAsset(const std::string& id, const std::string& nsIn = std::string())
	{
		const std::string& ns = nsIn.empty() ? g_currentNs : nsIn;
		int idx = SAMModels::modelIndexForId(id);
		if ( idx < 0 && id.find(':') == std::string::npos && !ns.empty() )
		{
			idx = SAMModels::modelIndexForId(ns + ":" + id);
		}
		return idx;
	}
	int samResolveSoundAsset(const std::string& id, const std::string& nsIn = std::string())
	{
		const std::string& ns = nsIn.empty() ? g_currentNs : nsIn;
		int idx = SAMSounds::soundIndexForId(id);
		if ( idx < 0 && id.find(':') == std::string::npos && !ns.empty() )
		{
			idx = SAMSounds::soundIndexForId(ns + ":" + id);
		}
		return idx;
	}


	// Part 4 timers — per-script, keyed by (ns,id). Ticked once per game tick (host).
	struct Timer
	{
		std::string id;
		std::string ns;
		int  callbackRef = LUA_NOREF;
		long long remaining = 0; // ticks until next fire
		long long interval  = 0; // repeat interval (0 = one-shot)
		bool repeating = false;
	};
	std::vector<Timer> g_timers;

	// Part 2 custom hooks — registered names (docs/tracking) + a recursion guard so a
	// script that fires a hook which re-fires cannot loop forever.
	std::vector<std::string> g_customHooks;
	int g_fireDepth = 0;

	// v0.7.0 Feature 2 — damage interception. The host (Entity::modHP) opens a window
	// around the on_before_damage dispatch; sam_modify_damage (either runtime) writes
	// the replacement value here, which the host reads back and applies. Shared so the
	// JS runtime and entity.cpp both reach the same latch through SAMLua.
	bool      g_bdActive = false;
	int       g_bdPlayer = -1;
	long long g_bdValue  = 0;

	// Monster-damage latch. See the header for why it is separate and unkeyed.
	bool      g_bdmActive = false;
	long long g_bdmValue  = 0;

	// Generic value-rewrite latch (see the header). One at a time, by design: the engine
	// opens it immediately before a dispatch and closes it immediately after.
	bool        g_hvActive = false;
	long long   g_hvValue  = 0;
	std::string g_hvName;

	// v0.7.0 Feature 4 — per-monster scratch data (boss phases etc.). Keyed by monster
	// UID then key; values are JSON strings (same marshaling as sam_save_data). In-memory,
	// cleared on runtime shutdown. Shared so the JS runtime reaches it through SAMLua.
	std::map<unsigned, std::map<std::string, std::string>> g_monsterData;

	// v1.2.9 — per-player scratch data (cooldowns, ability flags, stacks). Keyed by player
	// index then key; JSON-string values like g_monsterData. In-memory, per session, cleared
	// on shutdown — the right tool for a per-player cooldown you tick often, unlike the
	// disk-backed, cross-run sam_save_data. Shared so the JS runtime reaches it via SAMLua.
	std::map<std::string, std::string> g_playerData[MAXPLAYERS];

	// ---- custom allocator (memory cap) ----------------------------------------

	void* luaAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize)
	{
		AllocState* a = static_cast<AllocState*>(ud);

		if ( nsize == 0 )
		{
			// free. When ptr != nullptr, osize is the real block size.
			if ( ptr )
			{
				a->used -= osize;
				std::free(ptr);
			}
			return nullptr;
		}

		// When ptr == nullptr, osize is a Lua type tag, NOT a real size, so only
		// subtract the old size for genuine reallocations.
		const std::size_t oldContribution = ptr ? osize : 0u;
		const std::size_t projected = a->used - oldContribution + nsize;

		if ( projected > a->limit )
		{
			// Deny — Lua converts a nullptr return into a recoverable memory error.
			return nullptr;
		}

		void* np = std::realloc(ptr, nsize);
		if ( !np )
		{
			return nullptr;
		}

		a->used = projected;
		if ( a->used > a->peak )
		{
			a->peak = a->used;
		}
		return np;
	}

	// ---- headroom guard for framework-driven Lua work -------------------------
	//
	// luaAlloc denies any allocation past the cap and Lua turns that into LUA_ERRMEM.
	// Inside a lua_pcall that is a contained failure: the script is disabled and the game
	// goes on. But dispatchEvent, dispatchTick, tickTimers and runBehavior all build the
	// event table and take registry refs BEFORE they enter the pcall -- with no handler on
	// the C stack, an ERRMEM there reaches lua_panic and abort()s the process. The
	// sequence that gets there is mundane: a script fills a global until the cap trips,
	// is disabled, and leaves the data reachable, pinning usage at the limit; the very
	// next engine event dies in pushEventTable.
	//
	// So before driving Lua the framework checks for headroom, tries one full collection
	// if there is none, and if that does not buy room it REFUSES to run scripts rather
	// than gamble. Scripts stop until memory is freed; the game keeps running. Logged,
	// throttled, and never reached while any mod behaves.
	constexpr std::size_t kDispatchHeadroomBytes = 256u * 1024u;
	bool luaHasHeadroom(const char* what)
	{
		if ( !L || g_alloc.limit == 0 ) { return true; }
		if ( g_alloc.used + kDispatchHeadroomBytes <= g_alloc.limit ) { return true; }
		// A full collection is safe here: it only frees, and in 5.4 an erroring __gc is
		// delivered as a warning rather than raised.
		lua_gc(L, LUA_GCCOLLECT, 0);
		if ( g_alloc.used + kDispatchHeadroomBytes <= g_alloc.limit ) { return true; }
		static unsigned s_suppressed = 0;
		if ( (s_suppressed++ % 600u) == 0u )
		{
			SAM_ERROR("LUA", "Memory cap: " + std::to_string(g_alloc.used / 1024) + " KB in use of "
				+ std::to_string(g_alloc.limit / 1024) + " KB; refusing to run " + what
				+ " until scripts free memory (running them now would abort the game).");
		}
		return false;
	}

	// ---- instruction-budget watchdog ------------------------------------------

	void instructionHook(lua_State* Ls, lua_Debug* /*ar*/)
	{
		g_hook.elapsed += g_hook.interval;
		if ( g_hook.elapsed >= g_hook.budget )
		{
			g_hook.tripped = true;
			// Raises a Lua error and longjmps back to the active lua_pcall. This
			// is the standard, safe way to abort a runaway script.
			luaL_error(Ls, "instruction budget exceeded (%d) - watchdog terminated script",
			           (int)g_hook.budget);
		}
	}

	void armWatchdog()
	{
		g_hook.elapsed = 0;
		g_hook.tripped = false;
		lua_sethook(L, instructionHook, LUA_MASKCOUNT, g_hook.interval);
	}

	void disarmWatchdog()
	{
		lua_sethook(L, nullptr, 0, 0);
	}

	// A protected call with the watchdog armed. `what` is a label for logging.
	// The callable + its `nargs` args must already be on the stack.
	// Set by the most recent dispatchEvent: did any handler return false? Read immediately
// after dispatch by an engine site that offers a cancellable decision. Deliberately a
// plain flag rather than a return value, so adding cancellation to an existing hook does
// not change dispatchEvent's signature or any of its 78 call sites.
bool g_lastDispatchCancelled = false;

// What the most recent dispatch's handlers wrote back onto the event table. Keyed by field
// name; only fields the ENGINE placed on the event are ever collected, so a script cannot
// smuggle in a name the site did not offer. Cleared at the top of every dispatch, so a site
// that reads it is always reading its own event and never a stale one from another.
// ---- script-registered entity behaviours -------------------------------------------------
// One row per behaviour a mod defined. An entity stores the INDEX into this vector, so the
// per-frame path never touches a string. Rows are never removed while the game runs (an
// entity in the world may still point at one); the whole table is dropped on mod reload.
struct ScriptedBehavior
{
	std::string name;          // "namespace:behaviour"
	std::string ns;            // owning mod, restored around the call for sam_save_data
	int         luaRef = -2;   // LUA_NOREF; a Lua function, or
	void*       jsFn   = nullptr; // a JS function (JSValue*), whichever registered it
};
std::vector<ScriptedBehavior> g_behaviors;

std::map<std::string, double>      g_lastEventNumbers;
std::map<std::string, std::string> g_lastEventStrings;

bool protectedCall(int nargs, int nresults, const std::string& what)
	{
		// Nesting-aware: only the OUTERMOST call arms/disarms the watchdog, so the
		// instruction budget spans a whole reentrant tree (e.g. sam_fire_hook inside an
		// on_event) instead of a nested call silently clearing the outer watchdog.
		const bool outerCall = ( g_callDepth == 0 );
		if ( outerCall ) { armWatchdog(); }
		++g_callDepth;
		const int rc = lua_pcall(L, nargs, nresults, 0);
		--g_callDepth;
		if ( outerCall ) { disarmWatchdog(); }

		if ( rc != LUA_OK )
		{
			const char* e = lua_tostring(L, -1);
			const std::string err = e ? e : "(no error message)";
			lua_pop(L, 1);

			if ( g_hook.tripped )
			{
				SAM_ERROR("LUA", "Watchdog killed [" + what + "]: " + err);
			}
			else if ( rc == LUA_ERRMEM )
			{
				SAM_ERROR("LUA", "Memory cap hit in [" + what + "]: " + err);
			}
			else
			{
				SAM_ERROR("LUA", "Error in [" + what + "]: " + err);
			}
			return false;
		}
		return true;
	}

	// ---- host functions exposed to scripts ------------------------------------

	// sam.log(msg) — the only output channel a script gets. String only; routed
	// through SAMLogger so script output is visible and captured like everything
	// else. (A real build would add more sam.* helpers, all primitive-only.)
	int lua_sam_log(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* msg = luaL_checkstring(Ls, 1);
		SAM_INFO("SCRIPT", msg ? msg : "");
		return 0;
	}

#ifdef SAM_LUA_HAVE_BARONY
	// sam_grant_item(playerIndex, "ITEM_NAME") — the first host function that
	// actually MUTATES the game: it gives a vanilla item to a player. Lua passes
	// only primitives (an int index + a string name); the Entity*/Item* pointers
	// never leave C++, honouring the no-raw-pointers contract. Runs on the
	// authoritative host only (multiplayer != CLIENT). Returns a boolean.
	int lua_sam_grant_item(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		const std::string itemName = nameC ? nameC : "";

		// Host-authoritative only — clients never mutate game state.
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_grant_item refused: host only (multiplayer == CLIENT).");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		if ( player < 0 || player >= MAXPLAYERS || !players[player] )
		{
			SAM_ERROR("LUA", "sam_grant_item: invalid player index " + std::to_string(player) + ".");
			lua_pushboolean(Ls, 0);
			return 1;
		}

		// Resolve a custom "namespace:item" id first, else a vanilla name (case-insensitive),
		// so scripts can grant modded gear the same as vanilla.
		int resolvedType = -1;
		if ( itemName.find(':') != std::string::npos )
		{
			resolvedType = SAMItems::itemIdForIdString(itemName);
		}
		// One resolver, shared with every other name-taking call: digits, "ns:id", the internal
		// name, then the DISPLAYED name -- which is what sam_list_items and sam_get_container_items
		// hand out, and what this used to refuse.
		if ( resolvedType < 0 ) { resolvedType = SAMCatalog::itemTypeFor(itemName); }
		if ( resolvedType < 0 )
		{
			SAM_ERROR("LUA", "sam_grant_item: unknown item '" + itemName
				+ "' (expected a vanilla name like \"IRON_DAGGER\" or a custom \"namespace:item\") — nothing granted.");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		const ItemType type = static_cast<ItemType>(resolvedType);

		// Optional trailing args: beatitude (blessed +N / cursed -N), status (0=BROKEN .. 4=
		// EXCELLENT), count. Keeping the 2-arg form (plain, uncursed) working unchanged.
		const Sint16 beatitude = (Sint16)luaL_optinteger(Ls, 3, 0);
		int statusArg = (int)luaL_optinteger(Ls, 4, (int)EXCELLENT);
		if ( statusArg < (int)BROKEN ) { statusArg = (int)BROKEN; }
		if ( statusArg > (int)EXCELLENT ) { statusArg = (int)EXCELLENT; }
		const Status status = (Status)statusArg;
		const int countArg = (int)luaL_optinteger(Ls, 5, 1);
		const Sint16 count = (Sint16)(countArg < 1 ? 1 : countArg);

		Item* item = newItem(type, status, beatitude, count, 0, true, nullptr);
		if ( !item )
		{
			SAM_ERROR("LUA", "sam_grant_item: newItem failed for '" + itemName + "'.");
			lua_pushboolean(Ls, 0);
			return 1;
		}

		// Into the backpack for a player on this machine. For a remote player the engine's own
		// remote pickup sends the vanilla ITEM packet (a stock client takes it too) and keeps
		// nothing here. The helper frees `item` either way, and refuses a slot that is not a
		// connected player (in singleplayer, slots 1-3 are not players at all).
		if ( !SAMMpInventory::deliverItem(player, item, "sam_grant_item") )
		{
			lua_pushboolean(Ls, 0);
			return 1;
		}

		SAM_INFO("LUA", "Granted item " + itemName + " to player " + std::to_string(player));
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// ---- shared helpers for the host API (primitives only) --------------------
	inline int samClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

	// What sam_set_stat owes the client after a write. The flush itself is
	// SAMLua::flushStatToClient/flushGoldToClient — public so the JS runtime calls the
	// same code instead of growing a second copy that drifts out of step.
	enum SamStatSync { SAM_SYNC_NONE, SAM_SYNC_ATTR, SAM_SYNC_GOLD, SAM_SYNC_HUNGER };

	// Hunger's own bounds, from the engine's clamps in actplayer.cpp (0 .. 1500).
	// 0 is starving; the tier edges (hungry/weak/starving/oversatiated) are derived by
	// getEntityHungerInterval and vary per race, so we clamp the raw value only.
	const int SAM_HUNGER_MIN = 0;
	const int SAM_HUNGER_MAX = 1500;

	inline std::string samUpper(const char* in)
	{
		std::string o = in ? in : "";
		for ( char& c : o ) { c = (char)std::toupper((unsigned char)c); }
		return o;
	}

	// Every named effect the engine defines, so a script can name any of them.
	//
	// This used to be a hand-written if-chain covering 14 effects out of the 135 in
	// stat.hpp — so STUNNED, FEAR, ROOTED, TELEPATH, MAGICREFLECT, THORNS and 115 others
	// were simply unreachable by name. A script asking for one got "unknown effect" and
	// silently did nothing, with no hint that the effect was real but unexposed.
	// Generated from stat.hpp's EFF_* constants; keep it in step if the engine adds more.
	struct SamEffectName { const char* name; int id; };
	static const SamEffectName samEffectNames[] = {
		{ "ASLEEP",                EFF_ASLEEP },
		{ "POISONED",              EFF_POISONED },
		{ "STUNNED",               EFF_STUNNED },
		{ "CONFUSED",              EFF_CONFUSED },
		{ "DRUNK",                 EFF_DRUNK },
		{ "INVISIBLE",             EFF_INVISIBLE },
		{ "BLIND",                 EFF_BLIND },
		{ "GREASY",                EFF_GREASY },
		{ "MESSY",                 EFF_MESSY },
		{ "FAST",                  EFF_FAST },
		{ "PARALYZED",             EFF_PARALYZED },
		{ "LEVITATING",            EFF_LEVITATING },
		{ "TELEPATH",              EFF_TELEPATH },
		{ "VOMITING",              EFF_VOMITING },
		{ "BLEEDING",              EFF_BLEEDING },
		{ "SLOW",                  EFF_SLOW },
		{ "MAGICRESIST",           EFF_MAGICRESIST },
		{ "MAGICREFLECT",          EFF_MAGICREFLECT },
		{ "VAMPIRICAURA",          EFF_VAMPIRICAURA },
		{ "SHRINE_RED_BUFF",       EFF_SHRINE_RED_BUFF },
		{ "SHRINE_GREEN_BUFF",     EFF_SHRINE_GREEN_BUFF },
		{ "SHRINE_BLUE_BUFF",      EFF_SHRINE_BLUE_BUFF },
		{ "HP_REGEN",              EFF_HP_REGEN },
		{ "MP_REGEN",              EFF_MP_REGEN },
		{ "PACIFY",                EFF_PACIFY },
		{ "POLYMORPH",             EFF_POLYMORPH },
		{ "KNOCKBACK",             EFF_KNOCKBACK },
		{ "WITHDRAWAL",            EFF_WITHDRAWAL },
		{ "POTION_STR",            EFF_POTION_STR },
		{ "SHAPESHIFT",            EFF_SHAPESHIFT },
		{ "WEBBED",                EFF_WEBBED },
		{ "FEAR",                  EFF_FEAR },
		{ "MAGICAMPLIFY",          EFF_MAGICAMPLIFY },
		{ "DISORIENTED",           EFF_DISORIENTED },
		{ "SHADOW_TAGGED",         EFF_SHADOW_TAGGED },
		{ "TROLLS_BLOOD",          EFF_TROLLS_BLOOD },
		{ "FLUTTER",               EFF_FLUTTER },
		{ "DASH",                  EFF_DASH },
		{ "DISTRACTED_COOLDOWN",   EFF_DISTRACTED_COOLDOWN },
		{ "MIMIC_LOCKED",          EFF_MIMIC_LOCKED },
		{ "ROOTED",                EFF_ROOTED },
		{ "NAUSEA_PROTECTION",     EFF_NAUSEA_PROTECTION },
		{ "CON_BONUS",             EFF_CON_BONUS },
		{ "PWR",                   EFF_PWR },
		{ "AGILITY",               EFF_AGILITY },
		{ "RALLY",                 EFF_RALLY },
		{ "MARIGOLD",              EFF_MARIGOLD },
		{ "ENSEMBLE_FLUTE",        EFF_ENSEMBLE_FLUTE },
		{ "ENSEMBLE_LYRE",         EFF_ENSEMBLE_LYRE },
		{ "ENSEMBLE_DRUM",         EFF_ENSEMBLE_DRUM },
		{ "ENSEMBLE_LUTE",         EFF_ENSEMBLE_LUTE },
		{ "ENSEMBLE_HORN",         EFF_ENSEMBLE_HORN },
		{ "LIFT",                  EFF_LIFT },
		{ "GUARD_SPIRIT",          EFF_GUARD_SPIRIT },
		{ "GUARD_BODY",            EFF_GUARD_BODY },
		{ "DIVINE_GUARD",          EFF_DIVINE_GUARD },
		{ "NIMBLENESS",            EFF_NIMBLENESS },
		{ "GREATER_MIGHT",         EFF_GREATER_MIGHT },
		{ "COUNSEL",               EFF_COUNSEL },
		{ "STURDINESS",            EFF_STURDINESS },
		{ "BLESS_FOOD",            EFF_BLESS_FOOD },
		{ "PINPOINT",              EFF_PINPOINT },
		{ "PENANCE",               EFF_PENANCE },
		{ "SACRED_PATH",           EFF_SACRED_PATH },
		{ "DETECT_ENEMY",          EFF_DETECT_ENEMY },
		{ "BLOOD_WARD",            EFF_BLOOD_WARD },
		{ "TRUE_BLOOD",            EFF_TRUE_BLOOD },
		{ "DIVINE_ZEAL",           EFF_DIVINE_ZEAL },
		{ "MAXIMISE",              EFF_MAXIMISE },
		{ "MINIMISE",              EFF_MINIMISE },
		{ "WEAKNESS",              EFF_WEAKNESS },
		{ "INCOHERENCE",           EFF_INCOHERENCE },
		{ "OVERCHARGE",            EFF_OVERCHARGE },
		{ "ENVENOM_WEAPON",        EFF_ENVENOM_WEAPON },
		{ "MAGIC_GREASE",          EFF_MAGIC_GREASE },
		{ "COMMAND",               EFF_COMMAND },
		{ "MIMIC_VOID",            EFF_MIMIC_VOID },
		{ "CURSE_FLESH",           EFF_CURSE_FLESH },
		{ "NUMBING_BOLT",          EFF_NUMBING_BOLT },
		{ "DELAY_PAIN",            EFF_DELAY_PAIN },
		{ "SEEK_CREATURE",         EFF_SEEK_CREATURE },
		{ "TABOO",                 EFF_TABOO },
		{ "COURAGE",               EFF_COURAGE },
		{ "COWARDICE",             EFF_COWARDICE },
		{ "SPORES",                EFF_SPORES },
		{ "ABUNDANCE",             EFF_ABUNDANCE },
		{ "GREATER_ABUNDANCE",     EFF_GREATER_ABUNDANCE },
		{ "PRESERVE",              EFF_PRESERVE },
		{ "MIST_FORM",             EFF_MIST_FORM },
		{ "FORCE_SHIELD",          EFF_FORCE_SHIELD },
		{ "LIGHTEN_LOAD",          EFF_LIGHTEN_LOAD },
		{ "ATTRACT_ITEMS",         EFF_ATTRACT_ITEMS },
		{ "RETURN_ITEM",           EFF_RETURN_ITEM },
		{ "DEMESNE_DOOR",          EFF_DEMESNE_DOOR },
		{ "REFLECTOR_SHIELD",      EFF_REFLECTOR_SHIELD },
		{ "DIZZY",                 EFF_DIZZY },
		{ "SPIN",                  EFF_SPIN },
		{ "CRITICAL_SPELL",        EFF_CRITICAL_SPELL },
		{ "MAGIC_WELL",            EFF_MAGIC_WELL },
		{ "STATIC",                EFF_STATIC },
		{ "ABSORB_MAGIC",          EFF_ABSORB_MAGIC },
		{ "FLAME_CLOAK",           EFF_FLAME_CLOAK },
		{ "DUSTED",                EFF_DUSTED },
		{ "NOISE_VISIBILITY",      EFF_NOISE_VISIBILITY },
		{ "RATION_SPICY",          EFF_RATION_SPICY },
		{ "RATION_SOUR",           EFF_RATION_SOUR },
		{ "RATION_BITTER",         EFF_RATION_BITTER },
		{ "RATION_HEARTY",         EFF_RATION_HEARTY },
		{ "RATION_HERBAL",         EFF_RATION_HERBAL },
		{ "RATION_SWEET",          EFF_RATION_SWEET },
		{ "GROWTH",                EFF_GROWTH },
		{ "THORNS",                EFF_THORNS },
		{ "BLADEVINES",            EFF_BLADEVINES },
		{ "BASTION_MUSHROOM",      EFF_BASTION_MUSHROOM },
		{ "BASTION_ROOTS",         EFF_BASTION_ROOTS },
		{ "FOCI_LIGHT_PEACE",      EFF_FOCI_LIGHT_PEACE },
		{ "FOCI_LIGHT_JUSTICE",    EFF_FOCI_LIGHT_JUSTICE },
		{ "FOCI_LIGHT_PROVIDENCE", EFF_FOCI_LIGHT_PROVIDENCE },
		{ "FOCI_LIGHT_PURITY",     EFF_FOCI_LIGHT_PURITY },
		{ "FOCI_LIGHT_SANCTUARY",  EFF_FOCI_LIGHT_SANCTUARY },
		{ "STASIS",                EFF_STASIS },
		{ "HP_MP_REGEN",           EFF_HP_MP_REGEN },
		{ "DISRUPTED",             EFF_DISRUPTED },
		{ "FROST",                 EFF_FROST },
		{ "MAGICIANS_ARMOR",       EFF_MAGICIANS_ARMOR },
		{ "PROJECT_SPIRIT",        EFF_PROJECT_SPIRIT },
		{ "DEFY_FLESH",            EFF_DEFY_FLESH },
		{ "PINPOINT_DAMAGE",       EFF_PINPOINT_DAMAGE },
		{ "SALAMANDER_HEART",      EFF_SALAMANDER_HEART },
		{ "DIVINE_FIRE",           EFF_DIVINE_FIRE },
		{ "HEALING_WORD",          EFF_HEALING_WORD },
		{ "HOLY_FIRE",             EFF_HOLY_FIRE },
		{ "SIGIL",                 EFF_SIGIL },
		{ "SANCTUARY",             EFF_SANCTUARY },
		{ "DUCKED",                EFF_DUCKED },
	};

	// Map a case-insensitive effect name to its EFF_* id, or -1 if unknown.
	// A short sample of valid effect names, for error messages. Guessing the name is the
	// single most common scripting mistake, and "unknown effect 'X'" on its own gives a
	// modder nowhere to go.
	std::string samEffectNameHint()
	{
		std::string out;
		int n = 0;
		for ( const auto& e : samEffectNames )
		{
			if ( n++ >= 6 ) { break; }
			if ( !out.empty() ) { out += ", "; }
			out += e.name;
		}
		return out + ", ... (no EFF_ prefix needed)";
	}

	int samEffectNameToId(const char* nameIn)
	{
		std::string n = samUpper(nameIn);
		// Accept the engine's own "EFF_" prefix as well as the bare name. The C++ constants
		// are EFF_FAST, EFF_POISONED and so on, so that is what a modder reading the engine
		// (or guessing) writes first -- and it used to fail with a bare "unknown effect".
		if ( n.rfind("EFF_", 0) == 0 ) { n = n.substr(4); }
#ifdef SAM_LUA_HAVE_BARONY
		// A registered custom effect id ("namespace:effect", case-sensitive) resolves to its
		// assigned slot 135..159. Checked first so a mod effect wins over any vanilla name.
		if ( nameIn )
		{
			const int customSlot = SAMEffects::idForName(nameIn);
			if ( customSlot >= 0 ) { return customSlot; }
		}
#endif
		for ( const auto& e : samEffectNames )
		{
			if ( n == e.name ) { return e.id; }
		}
		// Custom S.A.M effect slots [135, NUMEFFECTS): accept a raw number ("135") or
		// "CUSTOM:135". The engine's effect array reserves NUMEFFECTS=160 bits but the
		// highest vanilla effect is EFF_DUCKED=134, so slots 135-159 are unused yet
		// already serialized, saved, ticked and auto-expired — scripts can drive
		// pseudo-effects with them (apply, react in on_status_effect_tick, revert on
		// expiry). Only 135+ is allowed so a number can't stomp a vanilla effect slot.
		{
			const std::string num = (n.rfind("CUSTOM:", 0) == 0) ? n.substr(7) : n;
			if ( !num.empty() && num.find_first_not_of("0123456789") == std::string::npos )
			{
				const int v = atoi(num.c_str());
				if ( v >= 135 && v < NUMEFFECTS ) { return v; }
			}
		}
		return -1;
	}

	// sam_grant_gold(player, amount) — add gold to a player (host-authoritative),
	// mirroring the vanilla actgold award path + client HUD packet.
	int lua_sam_grant_gold(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const int amount = (int)luaL_checkinteger(Ls, 2);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_grant_gold refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("LUA", "sam_grant_gold: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		stats[player]->GOLD += amount;
		if ( stats[player]->GOLD < 0 ) { stats[player]->GOLD = 0; }
		if ( multiplayer == SERVER && player > 0 && !players[player]->isLocalPlayer() )
		{
			strcpy((char*)net_packet->data, "GOLD");
			SDLNet_Write32(stats[player]->GOLD, &net_packet->data[4]);
			net_packet->address.host = net_clients[player - 1].host;
			net_packet->address.port = net_clients[player - 1].port;
			net_packet->len = 8;
			sendPacketSafe(net_sock, -1, net_packet, player - 1);
		}
		SAM_INFO("LUA", "Granted " + std::to_string(amount) + " gold to player " + std::to_string(player));
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_apply_effect(player, "EFFECT", ticks[, strength]) — apply a status effect for N
	// ticks (50 ticks == 1s). Optional strength sets the tier/magnitude for effects that
	// carry one (GROWTH stacks, potion STR); omit it for the plain default. setEffect
	// auto-syncs the client. Returns false if immune.
	int lua_sam_apply_effect(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		const int ticks = (int)luaL_checkinteger(Ls, 3);
		const int strength = lua_isnoneornil(Ls, 4) ? 0 : (int)luaL_checkinteger(Ls, 4);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_apply_effect refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("LUA", "sam_apply_effect: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { SAM_ERROR("LUA", std::string("sam_apply_effect: unknown effect '") + (nameC ? nameC : "") + "'. Valid: " + samEffectNameHint()); lua_pushboolean(Ls, 0); return 1; }
		bool ok;
		if ( strength > 0 )
		{
			const Uint8 st = (Uint8)(strength > 255 ? 255 : strength);
			// value = explicit Uint8 strength, updateClients=true, guarantee=true, overrideEffectStrength=true.
			ok = players[player]->entity->setEffect(eff, st, ticks, true, true, true);
		}
		else
		{
			ok = players[player]->entity->setEffect(eff, true, ticks, true);
		}
		SAM_INFO("LUA", std::string("Applied effect ") + (nameC ? nameC : "") + " to player " + std::to_string(player) + (ok ? "" : " (refused/immune)"));
		lua_pushboolean(Ls, ok ? 1 : 0);
		return 1;
	}

	// sam_remove_effect(player, "EFFECT") — clear a status effect (host-authoritative).
	int lua_sam_remove_effect(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_remove_effect refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("LUA", "sam_remove_effect: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { SAM_ERROR("LUA", std::string("sam_remove_effect: unknown effect '") + (nameC ? nameC : "") + "'. Valid: " + samEffectNameHint()); lua_pushboolean(Ls, 0); return 1; }
		players[player]->entity->setEffect(eff, false, 0, true);
		SAM_INFO("LUA", std::string("Removed effect ") + (nameC ? nameC : "") + " from player " + std::to_string(player));
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_get_stat(player, "STAT") -> number. Host-authoritative read.
	int lua_sam_get_stat(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		// A CLIENT may read ITS OWN player, and nobody else's.
		//
		// The old refusal was whole-function and stricter than the facts. A client holds a
		// correct stats[clientnum]: the 'UPHP' handler writes HP into it and 'UPMP' does the
		// same for MP, which is exactly what a client-side HUD mod needs and was being refused
		// data it already had. Every other slot stays refused, because a client is not sent
		// another player's stats at all -- it would read a zeroed structure and believe it.
		//
		// The trampoline enforces this first (the contract is Read/Player, and SAMNet::decide
		// refuses exactly this case), so nothing reaches the branch below today. It stays as a
		// backstop, and it now answers what a refusal answers: NO value. It used to push 0,
		// which is the one thing this function's refusal was chosen not to be -- 0 HP is a
		// corpse, 0 GOLD is a pauper -- so the branch stated the rule and returned its opposite.
		if ( multiplayer == CLIENT && player != clientnum )
		{
			SAM_WARN("LUA", "sam_get_stat: on a client you can read your own player ("
				+ std::to_string(clientnum) + ") only. Another player's stats are not sent to"
				" your machine, so the number here would be invented rather than stale.");
			return 0;
		}
		// Same rule for the two argument errors below: no value, not 0.
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("LUA", "sam_get_stat: invalid player index " + std::to_string(player) + "."); return 0; }
		const std::string n = samUpper(nameC);
		Stat* s = stats[player];
		long long v = 0;
		if      ( n == "STR" )   { v = s->STR; }
		else if ( n == "DEX" )   { v = s->DEX; }
		else if ( n == "CON" )   { v = s->CON; }
		else if ( n == "INT" )   { v = s->INT; }
		else if ( n == "PER" )   { v = s->PER; }
		else if ( n == "CHR" )   { v = s->CHR; }
		else if ( n == "HP" )    { v = s->HP; }
		else if ( n == "MAXHP" ) { v = s->MAXHP; }
		else if ( n == "MP" )    { v = s->MP; }
		else if ( n == "MAXMP" ) { v = s->MAXMP; }
		else if ( n == "GOLD" )  { v = s->GOLD; }
		else if ( n == "HUNGER" ) { v = s->HUNGER; }
		else if ( n == "LEVEL" || n == "LVL" ) { v = s->LVL; }
		else if ( n == "EXP" )   { v = s->EXP; }
		else { SAM_ERROR("LUA", std::string("sam_get_stat: unknown stat '") + (nameC ? nameC : "") + "'."); return 0; }
		lua_pushinteger(Ls, (lua_Integer)v);
		return 1;
	}

	// sam_set_stat(player, "STAT", value) — bounded set (never HP>MAXHP etc.).
	int lua_sam_set_stat(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		const int value = (int)luaL_checkinteger(Ls, 3);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_stat refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("LUA", "sam_set_stat: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		const std::string n = samUpper(nameC);
		Stat* s = stats[player];
		Entity* e = players[player]->entity;
		SamStatSync sync = SAM_SYNC_NONE;
		// setHP/setMP emit UPHP/UPMP themselves — but only when there IS an entity. With
		// none (a dead player awaiting respawn) the raw write below reaches no client on
		// its own, so it needs the ATTR flush like every other field. ATTR already carries
		// HP/MAXHP/MP/MAXMP, so this costs no new wire format.
		if      ( n == "HP" )    { if ( e ) { e->setHP(value); } else { s->HP = samClampInt(value, 0, s->MAXHP); sync = SAM_SYNC_ATTR; } }
		else if ( n == "MP" )    { if ( e ) { e->setMP(value); } else { s->MP = samClampInt(value, 0, s->MAXMP); sync = SAM_SYNC_ATTR; } }
		else if ( n == "MAXHP" ) { s->MAXHP = samClampInt(value, 1, SAMLua::STAT_WIRE_MAX); if ( s->HP > s->MAXHP ) { s->HP = s->MAXHP; } sync = SAM_SYNC_ATTR; }
		else if ( n == "MAXMP" ) { s->MAXMP = samClampInt(value, 0, SAMLua::STAT_WIRE_MAX); if ( s->MP > s->MAXMP ) { s->MP = s->MAXMP; } sync = SAM_SYNC_ATTR; }
		else if ( n == "STR" )   { s->STR = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = SAM_SYNC_ATTR; }
		else if ( n == "DEX" )   { s->DEX = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = SAM_SYNC_ATTR; }
		else if ( n == "CON" )   { s->CON = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = SAM_SYNC_ATTR; }
		else if ( n == "INT" )   { s->INT = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = SAM_SYNC_ATTR; }
		else if ( n == "PER" )   { s->PER = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = SAM_SYNC_ATTR; }
		else if ( n == "CHR" )   { s->CHR = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = SAM_SYNC_ATTR; }
		else if ( n == "GOLD" )  { s->GOLD = (value < 0 ? 0 : value); sync = SAM_SYNC_GOLD; }
		else if ( n == "HUNGER" ) { s->HUNGER = samClampInt(value, SAM_HUNGER_MIN, SAM_HUNGER_MAX); sync = SAM_SYNC_HUNGER; }
		else if ( n == "LEVEL" || n == "LVL" ) { s->LVL = samClampInt(value, 1, 255); sync = SAM_SYNC_ATTR; }
		// EXP up to 255 (its wire byte): 100+ triggers the engine's real level-up on the
		// host's next tick. The old 0..99 cap silently made leveling-by-EXP impossible.
		else if ( n == "EXP" )   { s->EXP = samClampInt(value, 0, 255); sync = SAM_SYNC_ATTR; }
		else { SAM_ERROR("LUA", std::string("sam_set_stat: unknown stat '") + (nameC ? nameC : "") + "'."); lua_pushboolean(Ls, 0); return 1; }
		// Without this the write lands host-side only and the client's sheet silently
		// disagrees until some unrelated event happens to fire an ATTR of its own.
		// HP/MP are absent on purpose — setHP/setMP already emit UPHP/UPMP.
		if      ( sync == SAM_SYNC_ATTR ) { SAMLua::flushStatToClient(player); }
		else if ( sync == SAM_SYNC_GOLD ) { SAMLua::flushGoldToClient(player); }
		// Hunger has its own engine sender ('HNGR'), and unlike ATTR it already does all
		// the guarding itself (SERVER-only, skips the local/disconnected player), so this
		// is just a call rather than another hand-inlined packet.
		else if ( sync == SAM_SYNC_HUNGER ) { serverUpdateHunger(player); }
		// ATTR reaches only the owner, and nothing at all for the host's own player. Every other
		// machine's party level comes from 'UPLV', which the engine sends only at a real level-up.
		if ( n == "LEVEL" || n == "LVL" ) { serverUpdatePlayerLVL(); }
		SAM_INFO("LUA", std::string("Set stat ") + n + " = " + std::to_string(value) + " on player " + std::to_string(player));
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_set_move_speed(player, mult) — host-only; syncs to the owning client.
	int lua_sam_set_move_speed(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const double mult = (double)luaL_checknumber(Ls, 2);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_move_speed refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS )
		{ SAM_ERROR("LUA", "sam_set_move_speed: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		SAMLua::setMoveSpeedMult(player, mult);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_get_move_speed(player) -> number. Readable everywhere (the contract: any): the host sends
	// every player's multiplier to every S.A.M client whenever it changes, so every machine reads the
	// same value, and on the player's own machine it is exactly what its movement code uses.
	int lua_sam_get_move_speed(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		lua_pushnumber(Ls, (lua_Number)SAMLua::getMoveSpeedMult(player));
		return 1;
	}

	// sam_add_move_speed(player, delta) — ADD to the current multiplier (set only sets).
	// Host-only; clamped to [0.1, 3.0] and synced like set. So two abilities can each add
	// their own share instead of overwriting each other.
	int lua_sam_add_move_speed(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const double delta = (double)luaL_checknumber(Ls, 2);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_add_move_speed refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS )
		{ SAM_ERROR("LUA", "sam_add_move_speed: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		SAMLua::setMoveSpeedMult(player, SAMLua::getMoveSpeedMult(player) + delta); // setter clamps + syncs
		lua_pushnumber(Ls, (lua_Number)SAMLua::getMoveSpeedMult(player));
		return 1;
	}

	// sam_level_up(player [, count]) — run the engine's real level-up path `count` times.
	// Host-only. Adds exactly what the next `count` levels cost; the host's
	// Entity::handleEffects drains one threshold per tick and runs the FULL vanilla level-up
	// (attribute rolls, HP/MP, level-up screen/sound, the ATTR/LVLI packets, and one
	// player.on_level_up per level). No code duplication, no manual flush — the engine owns it.
	// Since EXP sits below the current threshold between levels, this grants exactly `count`.
	//
	// The price comes from the XP curve. It used to add a flat 100 a level, so under a curve of
	// 50 it granted twice the levels asked for, and under 250 it granted none.
	int lua_sam_level_up(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const int count = lua_isnoneornil(Ls, 2) ? 1 : (int)luaL_checkinteger(Ls, 2);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_level_up refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("LUA", "sam_level_up: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		const int levels = samClampInt(count, 1, 255);
		long long exp = (long long)stats[player]->EXP;
		for ( int k = 0; k < levels; ++k ) { exp += SAMRules::xpThreshold((int)stats[player]->LVL + k); }
		if ( exp > 2147483647LL ) { exp = 2147483647LL; }
		stats[player]->EXP = (Sint32)exp;
		SAM_INFO("LUA", "Queued " + std::to_string(levels) + " level-up(s) for player " + std::to_string(player) + ".");
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_get_floor() -> number (0-based current dungeon level).
	int lua_sam_get_floor(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushinteger(Ls, (lua_Integer)currentlevel);
		return 1;
	}

	// ---- v2.4 toolkit: run seed, lobby flags, ghost state, deterministic RNG ----------

	// The ten SV_FLAG_* bits (net.hpp:82-91) by name. "keep_inventory" is deliberately
	// answered from the DERIVED global keepInventoryGlobal (net.hpp:94) rather than the raw
	// bit, because that is what the engine itself branches on (maps.cpp:7144,
	// actplayer.cpp:11211) and tutorial mode makes the two disagree.
	int samLobbyFlag(const std::string& nameIn, bool& ok)
	{
		ok = true;
		std::string n = nameIn;
		for ( char& c : n ) { c = (char)std::tolower((unsigned char)c); }
		if ( n == "cheats" )        { return (svFlags & SV_FLAG_CHEATS) != 0; }
		if ( n == "friendlyfire" || n == "friendly_fire" ) { return (svFlags & SV_FLAG_FRIENDLYFIRE) != 0; }
		if ( n == "minotaurs" )     { return (svFlags & SV_FLAG_MINOTAURS) != 0; }
		if ( n == "hunger" )        { return (svFlags & SV_FLAG_HUNGER) != 0; }
		if ( n == "traps" )         { return (svFlags & SV_FLAG_TRAPS) != 0; }
		if ( n == "hardcore" )      { return (svFlags & SV_FLAG_HARDCORE) != 0; }
		if ( n == "classic" )       { return (svFlags & SV_FLAG_CLASSIC) != 0; }
		if ( n == "keepinventory" || n == "keep_inventory" ) { return keepInventoryGlobal ? 1 : 0; }
		if ( n == "lifesaving" )    { return (svFlags & SV_FLAG_LIFESAVING) != 0; }
		if ( n == "assist_items" || n == "assistitems" ) { return (svFlags & SV_FLAG_ASSIST_ITEMS) != 0; }
		ok = false;
		return 0;
	}
	const char* samLobbyFlagNames()
	{
		return "cheats, friendlyfire, minotaurs, hunger, traps, hardcore, classic, "
			"keep_inventory, lifesaving, assist_items";
	}

	// Deterministic per-mod RNG, used by sam_random below.
	//
	// A script rolling with math.random gets a DIFFERENT answer on every machine, so any
	// roll that changes the world desyncs co-op. And a script must never draw from the
	// engine's own local_rng/map_rng: those streams are consumed in lockstep by the game
	// itself, so taking a number out of one shifts every later engine roll and changes the
	// dungeon that seed was supposed to produce. This is a self-contained splitmix64 seeded
	// from (run seed, namespace, stream name) with its own per-stream counter, so it touches
	// neither. Same run + same namespace + same stream + same call index = same number
	// everywhere, which is what makes a roll safe to act on without sending it.
	unsigned long long samFnv1a64(const std::string& str)
	{
		unsigned long long h = 14695981039346656037ULL;
		for ( size_t i = 0; i < str.size(); ++i ) { h ^= (unsigned char)str[i]; h *= 1099511628211ULL; }
		return h;
	}
	unsigned long long samSplitMix64(unsigned long long x)
	{
		x += 0x9E3779B97F4A7C15ULL;
		unsigned long long z = x;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}
	// Per-stream draw counters. These MUST be cleared when a run starts: they used to live
	// for the whole process, so a host on its third run of the session had stream "loot" at
	// 812 while a friend who had just launched was at 0, and the same call returned different
	// numbers on the two machines even though uniqueGameKey matched.
	std::map<std::string, unsigned long long> g_rngCounters;

	// Shared by both runtimes so Lua and JS cannot drift apart on the same stream.
	long long samRandomDraw(const std::string& ns, const std::string& stream, long long lo, long long hi)
	{
		if ( hi < lo ) { const long long t = lo; lo = hi; hi = t; }
		const std::string key = ns + "\x1f" + stream;
		unsigned long long& counter = g_rngCounters[key];
		const unsigned long long state = samFnv1a64(key)
			^ ((unsigned long long)uniqueGameKey * 0x9E3779B97F4A7C15ULL)
			^ (counter * 0xD1B54A32D192ED03ULL);
		++counter;
		const unsigned long long r = samSplitMix64(state);
		const unsigned long long span = (unsigned long long)(hi - lo) + 1ULL;
		return lo + (long long)(r % span);
	}

	// sam_get_seed() -> the run seed (uniqueGameKey, game.hpp:90). Stable for a whole run,
	// identical on host and clients, 0 on the main menu before a run starts. This is the
	// number to derive anything that must agree across a party from. mapseed is NOT it: the
	// engine re-rolls that every floor.
	int lua_sam_get_seed(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushinteger(Ls, (lua_Integer)(unsigned long long)uniqueGameKey);
		return 1;
	}

	// sam_get_flag("minotaurs") -> boolean, or nil for an unknown name. Reads a lobby
	// setting. Valid on clients too: a client's svFlags is the host's copy.
	int lua_sam_get_flag(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* nameC = luaL_checkstring(Ls, 1);
		bool ok = false;
		const int v = samLobbyFlag(nameC ? nameC : "", ok);
		if ( !ok )
		{
			SAM_WARN("LUA", std::string("sam_get_flag: unknown flag '") + (nameC ? nameC : "")
				+ "'. Valid: " + samLobbyFlagNames());
			lua_pushnil(Ls);
			return 1;
		}
		lua_pushboolean(Ls, v ? 1 : 0);
		return 1;
	}

	// sam_is_ghost(player) -> boolean. True while the player is a ghost that can act.
	// Every machine runs its own Ghost_t, so this is correct for remote players too.
	int lua_sam_is_ghost(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, players[player]->ghost.isActive() ? 1 : 0);
		return 1;
	}

	// sam_is_spirit_ghost(player) -> boolean. Distinguishes the Project Spirit ghost (the
	// player is still ALIVE) from the death ghost. Without this, sam_is_ghost alone
	// conflates the two and a mod that pays out on death fires for a living caster.
	int lua_sam_is_spirit_ghost(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, players[player]->ghost.isSpiritGhost() ? 1 : 0);
		return 1;
	}

	// sam_random("stream", lo, hi) -> integer in [lo, hi]. Deterministic; see samRandomDraw.
	int lua_sam_random(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* streamC = luaL_checkstring(Ls, 1);
		const long long lo = (long long)luaL_checkinteger(Ls, 2);
		const long long hi = (long long)luaL_checkinteger(Ls, 3);
		lua_pushinteger(Ls, (lua_Integer)samRandomDraw(g_currentNs, streamC ? streamC : "", lo, hi));
		return 1;
	}

	// sam_spawn_item(x, y, "ITEM_NAME") — spawn a ground item at map tile (x,y).
	int lua_sam_spawn_item(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int x = (int)luaL_checkinteger(Ls, 1);
		const int y = (int)luaL_checkinteger(Ls, 2);
		const char* nameC = luaL_checkstring(Ls, 3);
		const std::string itemName = nameC ? nameC : "";
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_spawn_item refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		// Resolve a custom "namespace:item" id first, else a vanilla name (case-insensitive),
		// matching sam_grant_item. Without the first tier a mod could not drop its OWN items,
		// which is the main thing scripts spawn.
		int resolvedType = -1;
		if ( itemName.find(':') != std::string::npos )
		{
			resolvedType = SAMItems::itemIdForIdString(itemName);
		}
		// One resolver, shared with every other name-taking call: digits, "ns:id", the internal
		// name, then the DISPLAYED name -- which is what sam_list_items and sam_get_container_items
		// hand out, and what this used to refuse.
		if ( resolvedType < 0 ) { resolvedType = SAMCatalog::itemTypeFor(itemName); }
		if ( resolvedType < 0 )
		{
			SAM_ERROR("LUA", "sam_spawn_item: unknown item '" + itemName
				+ "' (expected a vanilla name like \"IRON_DAGGER\" or a custom \"namespace:item\").");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		// v1.11.0 -- status/beatitude/count are settable, and the uid comes back.
		//
		// These were hardcoded to EXCELLENT/0/1, which made it impossible to put an item back
		// the way you found it: a stash mod could save that you had a cursed, worn ring and
		// then only ever return a pristine one. Restoring world state needs all three.
		//
		// Returning the uid closes a documented gap. Without it a script could place an item
		// and then never refer to it again -- it could not move it, remove it, or check it
		// later. Backwards compatible: a uid is non-zero, so `if sam_spawn_item(...)` still
		// reads as true exactly where it used to.
		const int statusArg = (int)luaL_optinteger(Ls, 4, (int)EXCELLENT);
		const int beatitudeArg = (int)luaL_optinteger(Ls, 5, 0);
		const int countArg = (int)luaL_optinteger(Ls, 6, 1);
		const Status st = (Status)samClampInt(statusArg, (int)BROKEN, (int)EXCELLENT);
		const Sint16 be = (Sint16)samClampInt(beatitudeArg, -100, 100);
		const Sint16 ct = (Sint16)samClampInt(countArg, 1, 1000);
		if ( multiplayer == SERVER && ct > 255 )
		{
			SAMNet::warnOnce("sam_spawn_item|count", "sam_spawn_item: a ground stack over 255 shows as its count"
				" modulo 256 on the other players' screens (the engine's ground-item packet holds one byte)."
				" Picking it up still gives the full count. Spawn several smaller stacks if the number matters.");
		}

		Entity* e = spawnGroundItem(static_cast<ItemType>(resolvedType), st, be, ct, x, y);
		if ( !e ) { SAM_ERROR("LUA", "sam_spawn_item: invalid tile (" + std::to_string(x) + "," + std::to_string(y) + ")."); lua_pushnil(Ls); return 1; }
		SAM_INFO("LUA", "Spawned item " + itemName + " at (" + std::to_string(x) + "," + std::to_string(y)
			+ ") uid " + std::to_string((unsigned long long)e->getUID()));
		lua_pushinteger(Ls, (lua_Integer)e->getUID());
		return 1;
	}

	// sam_item_id("VANILLA_NAME" | "namespace:item") -> number|nil. Resolve an item
	// type's numeric id, for matching against event fields like on_block's shield_type.
	// A name containing ':' resolves a custom S.A.M item; otherwise the vanilla tooltip
	// name map is used (case-insensitive). Returns nil if the item is unknown.
	int lua_sam_item_id(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* nameC = luaL_checkstring(Ls, 1);
		const std::string name = nameC ? nameC : "";
		int id = -1;
		if ( name.find(':') != std::string::npos )
		{
			id = SAMItems::itemIdForIdString(name);
		}
		else { id = SAMCatalog::itemTypeFor(name); }   // shared resolver; accepts a displayed name too
		if ( id < 0 ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)id);
		return 1;
	}

	// sam_message(player, "text") — show a line in the player's message log.
	int lua_sam_message(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* text = luaL_checkstring(Ls, 2);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_message refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS )
		{ SAM_ERROR("LUA", "sam_message: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		SAMMpInput::scriptMessage(player, text ? text : "");   // a joiner's game reads some texts as orders
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_play_sound(soundId[, vol]) — play a sound for all connected players. soundId is
	// a vanilla numeric index OR the "namespace:sound" id of a custom sound.
	// Resolve a sound argument: a number is a raw engine id, a string is a mod's own
	// "ns:sound" registered by SAMSounds. Mirrors what sam_play_sound already accepts.
	static int samResolveSoundId(lua_State* Ls, int idx)
	{
		if ( lua_type(Ls, idx) == LUA_TSTRING )
		{
			const char* nm = lua_tostring(Ls, idx);
			const int id = samResolveSoundAsset(nm ? nm : "");
			if ( id < 0 )
			{
				SAM_WARN("LUA", std::string("unknown sound '") + (nm ? nm : "") + "'. A mod's sound is \"namespace:name\""
					" (a file sounds/name.ogg is \"<namespace>:name\"); sam_list_sounds() lists every one loaded.");
			}
			return id;
		}
		return (int)luaL_checkinteger(Ls, idx);
	}

	int lua_sam_play_sound(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		int soundId = -1;
		if ( lua_type(Ls, 1) == LUA_TSTRING )
		{
			const char* nm = lua_tostring(Ls, 1);
			soundId = samResolveSoundAsset(nm ? nm : "");
			if ( soundId < 0 )
			{ SAM_ERROR("LUA", std::string("sam_play_sound: unknown sound name '") + (nm ? nm : "") + "'."); lua_pushboolean(Ls, 0); return 1; }
		}
		else
		{
			soundId = (int)luaL_checkinteger(Ls, 1);
		}
		int vol = 128;
		if ( lua_gettop(Ls) >= 2 && !lua_isnoneornil(Ls, 2) ) { vol = (int)luaL_checkinteger(Ls, 2); }
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_play_sound refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( soundId < 0 || (Uint32)soundId >= numsounds )
		{ SAM_ERROR("LUA", "sam_play_sound: sound id " + std::to_string(soundId) + " out of range (0.." + std::to_string(numsounds) + ")."); lua_pushboolean(Ls, 0); return 1; }
		vol = samClampInt(vol, 0, 255);
		// Once on this machine however many local players share its speakers, and to each remote
		// player the vanilla packet -- or a mod sound by NAME (sam_sounds.hpp, "multiplayer").
		SAMSounds::playForEveryone(soundId, (Uint8)vol);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_get_nearby_entities(player, radiusTiles) -> { uid, uid, ... } (max 32).
	// Returns creature UIDs only; never a raw pointer.
	int lua_sam_get_nearby_entities(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const double radiusTiles = (double)luaL_checknumber(Ls, 2);
		lua_newtable(Ls);
		// Readable on a client too (contract: any): monsters and players replicate there, with their
		// positions. The host's answer is authoritative for where things are this very frame.
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity || !map.entities ) { return 1; }
		Entity* pe = players[player]->entity;
		const double thresholdPx = radiusTiles * 16.0;
		int idx = 1;
		for ( node_t* node = map.entities->first; node != nullptr; node = node->next )
		{
			Entity* ent = (Entity*)node->element;
			if ( !ent || ent == pe ) { continue; }
			if ( !(ent->behavior == &actMonster || ent->behavior == &actPlayer) ) { continue; }
			if ( entityDist(pe, ent) <= thresholdPx )
			{
				lua_pushinteger(Ls, (lua_Integer)ent->getUID());
				lua_rawseti(Ls, -2, idx++);
				if ( idx > 32 ) { break; }
			}
		}
		return 1;
	}

	// ---- expanded player queries (Part 5) --------------------------------------

	int g_samSessionKills[MAXPLAYERS] = { 0 }; // SAM-tracked (Barony has no per-player counter)

	// ---- per-player move-speed multiplier --------------------------------------

	double g_samMoveSpeed[MAXPLAYERS] = { 1.0, 1.0, 1.0, 1.0 };
	static_assert(MAXPLAYERS == 4, "g_samMoveSpeed's initializer must cover every player slot");

#ifdef SAM_LUA_HAVE_BARONY
	// ---- v1.6.0 impact-frame state (SDL-typed → engine build only) -------------
	// Per-player screen flash: an owner-machine HUD overlay that fades from maxAlpha to 0
	// over durMs (real-time, so it looks the same at any framerate). maxAlpha==0 means idle.
	struct SamFlashState { Uint32 startMs = 0; Uint32 durMs = 0; Uint8 r = 255, g = 255, b = 255; Uint8 maxAlpha = 0; Uint8 style = 0; Uint16 lines = 0; };
	static SamFlashState g_samFlash[MAXPLAYERS];
	// One global hitstop deadline (SDL_GetTicks() ms). 0 = inactive. Singleplayer only.
	static Uint32 g_samHitstopUntilMs = 0;
#endif

	const double SAM_MOVE_SPEED_MIN = 0.1;
	// v1.2.9 — raised 3.0 -> 5.0. The engine hard-clamps final player velocity magnitude at
	// 5.0 (actplayer.cpp), which for a boosted player we lift to a tunnel-safe 7.0; a ~4-5x
	// multiplier is what actually reaches that new ceiling, so 3.0 was leaving real speed on
	// the table for fast builds. Above ~5x the velocity clamp eats everything anyway.
	const double SAM_MOVE_SPEED_MAX = 5.0;

	// Order matters: NaN must be caught BEFORE the clamp, not by it. NaN compares false
	// against everything, so `v < MIN ? MIN : (v > MAX ? MAX : v)` returns NaN unchanged
	// and it would reach speedFactor, where it poisons PLAYER_VELX/Y permanently — the
	// player simply stops moving for the rest of the game with nothing in the log.
	inline double samSanitizeSpeed(double v)
	{
		if ( !(v == v) ) { return 1.0; }                       // NaN
		if ( v < SAM_MOVE_SPEED_MIN ) { return SAM_MOVE_SPEED_MIN; } // also catches -inf
		if ( v > SAM_MOVE_SPEED_MAX ) { return SAM_MOVE_SPEED_MAX; } // also catches +inf
		return v;
	}

	// Tell the clients a multiplier changed. Movement is computed where the player lives, so a
	// remote player's multiplier has to reach their machine or it does nothing.
	void samSendMoveSpeed(int player)
	{
		// To every client that runs S.A.M, on the ordered channel, only when the value changed;
		// see SAMRules::syncMoveSpeed. The raw 'SAMS' this used to send went to the owner alone,
		// could arrive out of order, and was never sent again to a client that joined later.
		SAMRules::syncMoveSpeed(player);
	}


	std::string samItemName(int type)
	{
		for ( const auto& kv : ItemTooltips.itemNameStringToItemID )
		{
			if ( kv.second == type ) { std::string n = kv.first; for ( char& c : n ) { c = (char)std::toupper((unsigned char)c); } return n; }
		}
		if ( type >= 0 && type < NUM_ITEM_SLOTS ) { return std::string(items[type].getIdentifiedName()); }
		return "";
	}

	Item* samEquippedSlot(int player, const std::string& slot)
	{
		Stat* s = stats[player];
		if ( slot == "WEAPON" )                          { return s->weapon; }
		if ( slot == "SHIELD" )                          { return s->shield; }
		// "HELM" as well: sam_get_item_slot emits HELM, and one S.A.M call's output must be
		// accepted by the next. The monster twin (samMonsterSlot) already took both, which is
		// what makes this an oversight rather than a deliberate second vocabulary.
		if ( slot == "HELMET" || slot == "HELM" )        { return s->helmet; }
		if ( slot == "ARMOR" || slot == "BREASTPLATE" )  { return s->breastplate; }
		if ( slot == "GLOVES" )                          { return s->gloves; }
		if ( slot == "BOOTS" || slot == "SHOES" )        { return s->shoes; }
		if ( slot == "RING" )                            { return s->ring; }
		if ( slot == "AMULET" )                          { return s->amulet; }
		if ( slot == "CLOAK" )                           { return s->cloak; }
		if ( slot == "MASK" )                            { return s->mask; }
		return nullptr;
	}

	int lua_sam_get_equipped_item(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* slotC = luaL_checkstring(Ls, 2);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushnil(Ls); return 1; }
		Item* it = samEquippedSlot(player, samUpper(slotC));
		if ( !it ) { lua_pushnil(Ls); return 1; }
		lua_pushstring(Ls, samItemName((int)it->type).c_str());
		return 1;
	}

	// sam_get_equipped_item_id(player, slot) -> number|nil. The NUMERIC item type, so it
	// can be compared against sam_item_id("ns:item"). sam_get_equipped_item above returns
	// a display NAME built from the vanilla name table, which never contains custom items
	// — so it can never match a custom id, which made "is MY item equipped?" impossible.
	int lua_sam_get_equipped_item_id(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* slotC = luaL_checkstring(Ls, 2);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushnil(Ls); return 1; }
		Item* it = samEquippedSlot(player, samUpper(slotC));
		if ( !it ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)it->type);
		return 1;
	}

	// sam_is_defending(player) -> boolean. The real engine blocking state, not just the
	// button being down. The host reads every player (vanilla's 'SHLD' reports remote ones);
	// a client reads only its own, since the host never sends its own player's stance.
	int lua_sam_is_defending(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, stats[player]->defending ? 1 : 0);
		return 1;
	}

	// sam_is_action_held(player, "Use") -> boolean. Reads a BOUND action, so it follows
	// the player's own keybinds. On the host it answers for every player: a joiner's game
	// reports its buttons (SAMMpInput; false for a player whose game does not run S.A.M).
	// A client answers only for its own player (the contract).
	int lua_sam_is_action_held(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* action = luaL_checkstring(Ls, 2);
		lua_pushboolean(Ls, SAMLua::isActionHeld(player, action ? action : "") ? 1 : 0);
		return 1;
	}

	// sam_get_action_binding(player, "Use") -> string|nil. The physical input behind an
	// action ("Mouse3"), for prompts. nil when the player has it unbound. On the host, a player
	// on another machine answers with the binding their game reported (nil until it has).
	int lua_sam_get_action_binding(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* action = luaL_checkstring(Ls, 2);
		const char* b = SAMLua::actionBinding(player, action ? action : "");
		if ( !b || !b[0] ) { lua_pushnil(Ls); return 1; }
		lua_pushstring(Ls, b);
		return 1;
	}

	// ===== mod actions and mod settings (sam_settings.cpp) ==================================
	//
	// A mod's own rows in the game's Settings screens. The namespace is the calling mod's
	// (g_currentNs), the same rule sam_save_data follows, so two mods cannot collide and an
	// action's full name is "<ns>:<id>".

#ifdef SAM_LUA_HAVE_BARONY
	// A Lua value as a setting value: a number, a boolean or a string, and nothing else (a
	// table, nil or a missing argument is Kind::None, which the declaration refuses with
	// "takes a number/boolean/string"). A number is any Lua number, integer or float.
	void samLuaToSettingValue(lua_State* Ls, int idx, SAMSettings::Value& out)
	{
		out = SAMSettings::Value();
		switch ( lua_type(Ls, idx) )
		{
			case LUA_TNUMBER:  out.kind = SAMSettings::Kind::Number; out.number = (double)lua_tonumber(Ls, idx); break;
			case LUA_TBOOLEAN: out.kind = SAMSettings::Kind::Bool;   out.flag = ( lua_toboolean(Ls, idx) != 0 ); break;
			case LUA_TSTRING:  out.kind = SAMSettings::Kind::String; out.text = lua_tostring(Ls, idx); break;
			default: break;
		}
	}

	// The value back to Lua. A whole number is pushed as an INTEGER (2, not 2.0), which is what
	// a script compares against and what JSON gives the JS twin; anything else is a float.
	void samLuaPushSettingValue(lua_State* Ls, const SAMSettings::Value& v)
	{
		switch ( v.kind )
		{
			case SAMSettings::Kind::Number:
				if ( v.number == std::floor(v.number) && std::fabs(v.number) < 9.0e15 ) { lua_pushinteger(Ls, (lua_Integer)v.number); }
				else { lua_pushnumber(Ls, (lua_Number)v.number); }
				break;
			case SAMSettings::Kind::Bool:   lua_pushboolean(Ls, v.flag ? 1 : 0); break;
			case SAMSettings::Kind::String: lua_pushstring(Ls, v.text.c_str()); break;
			default: lua_pushnil(Ls); break;
		}
	}

	// The table sam_register_setting takes, read the way sam_patch_item reads its table: keys
	// case-insensitive, a number only from a number, a string only from a string, so a table
	// means the same thing in both languages. `type` is required; `tip` and `tooltip` are one
	// key. The stack is left as it was found, on every path.
	bool samLuaReadSettingSpec(lua_State* Ls, int idx, SAMSettings::Spec& spec, std::string& why)
	{
		bool haveType = false;
		lua_pushnil(Ls);
		while ( lua_next(Ls, idx) != 0 )
		{
			if ( lua_type(Ls, -2) == LUA_TSTRING )
			{
				const std::string uk = samUpper(lua_tostring(Ls, -2));
				const bool isNum = ( lua_type(Ls, -1) == LUA_TNUMBER );
				const bool isStr = ( lua_type(Ls, -1) == LUA_TSTRING );
				if ( uk == "TYPE" && isStr )
				{
					const std::string tname = lua_tostring(Ls, -1);
					if ( !SAMSettings::typeFromName(tname, spec.type) )
					{
						why = "'" + spec.id + "': unknown type \"" + tname + "\" (slider, toggle, dropdown, number or text)";
						lua_pop(Ls, 2);   // the value and the key: the loop is not continued
						return false;
					}
					haveType = true;
				}
				else if ( uk == "LABEL" && isStr ) { spec.label = lua_tostring(Ls, -1); }
				else if ( (uk == "TIP" || uk == "TOOLTIP") && isStr ) { spec.tooltip = lua_tostring(Ls, -1); }
				else if ( uk == "MIN" && isNum ) { spec.hasMin = true; spec.min = (double)lua_tonumber(Ls, -1); }
				else if ( uk == "MAX" && isNum ) { spec.hasMax = true; spec.max = (double)lua_tonumber(Ls, -1); }
				else if ( uk == "STEP" && isNum ) { spec.step = (double)lua_tonumber(Ls, -1); }
				else if ( uk == "DEFAULT" ) { samLuaToSettingValue(Ls, lua_gettop(Ls), spec.def); }
				else if ( uk == "OPTIONS" && lua_istable(Ls, -1) )
				{
					const int oIdx = lua_gettop(Ls);
					const lua_Integer n = luaL_len(Ls, oIdx);
					for ( lua_Integer i = 1; i <= n && i <= 256; ++i )
					{
						lua_rawgeti(Ls, oIdx, i);
						if ( lua_type(Ls, -1) == LUA_TSTRING ) { spec.options.push_back(lua_tostring(Ls, -1)); }
						lua_pop(Ls, 1);
					}
				}
			}
			lua_pop(Ls, 1);
		}
		if ( !haveType )
		{
			why = "'" + spec.id + "': the table needs type = \"slider\" | \"toggle\" | \"dropdown\" | \"number\" | \"text\"";
			return false;
		}
		return true;
	}
#endif   // SAM_LUA_HAVE_BARONY: the three helpers above are only called under the guard

	// sam_register_action(id, label, default_key [, default_pad]) -> bool. One rebindable row in
	// Settings > Controls > Bindings under the mod's name. The action's full name is "<ns>:<id>":
	// what on_action_pressed carries in `action`, and what sam_is_action_held and
	// sam_get_action_binding take. The defaults are only defaults: a key the player already
	// bound (config.json) wins. Registering the same id again refreshes the label and defaults
	// and adds nothing, so top-level code may call it on every load.
	int lua_sam_register_action(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* id = luaL_checkstring(Ls, 1);
		const char* label = lua_isnoneornil(Ls, 2) ? "" : luaL_checkstring(Ls, 2);
		const char* key = lua_isnoneornil(Ls, 3) ? "" : luaL_checkstring(Ls, 3);
		const char* pad = lua_isnoneornil(Ls, 4) ? "" : luaL_checkstring(Ls, 4);
#ifdef SAM_LUA_HAVE_BARONY
		if ( g_currentNs.empty() ) { SAM_WARN("LUA", "sam_register_action: no owning mod namespace; ignored."); lua_pushboolean(Ls, 0); return 1; }
		std::string why;
		if ( !SAMSettings::registerAction(g_currentNs, id ? id : "", label ? label : "", key ? key : "", pad ? pad : "", &why) )
		{
			SAM_WARN("LUA", "sam_register_action refused: " + why);
			lua_pushboolean(Ls, 0);
			return 1;
		}
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)id; (void)label; (void)key; (void)pad;
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_register_setting(id, { type=, label=, tip=, default=, min=, max=, step=, options= })
	// -> bool. One row in Settings > General under MOD SETTINGS. The value in force is read from
	// the mod's settings file the first time an id is registered each load (the file wins over
	// the default). Registering the same id again refreshes the declaration and keeps the value
	// when the new declaration still accepts it.
	int lua_sam_register_setting(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* id = luaL_checkstring(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		if ( g_currentNs.empty() ) { SAM_WARN("LUA", "sam_register_setting: no owning mod namespace; ignored."); lua_pushboolean(Ls, 0); return 1; }
		if ( !lua_istable(Ls, 2) )
		{
			SAM_WARN("LUA", "sam_register_setting('" + std::string(id ? id : "") + "'): the second argument must be a table { type=, label=, default=, ... }.");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		SAMSettings::Spec spec;
		spec.id = id ? id : "";
		std::string why;
		if ( !samLuaReadSettingSpec(Ls, 2, spec, why) || !SAMSettings::registerSetting(g_currentNs, spec, &why) )
		{
			SAM_WARN("LUA", "sam_register_setting refused: " + why);
			lua_pushboolean(Ls, 0);
			return 1;
		}
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)id;
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_get_setting(id) -> number | boolean | string | nil. The value in force: what the player
	// confirmed in Settings, else what the mod's file holds, else the default. nil for an id this
	// mod never registered (silently: a mod may poll this every tick).
	int lua_sam_get_setting(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* id = luaL_checkstring(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		const SAMSettings::Setting* s = SAMSettings::find(g_currentNs, id ? id : "");
		if ( !s ) { lua_pushnil(Ls); return 1; }
		samLuaPushSettingValue(Ls, s->value);
		return 1;
#else
		(void)id;
		lua_pushnil(Ls); return 1;
#endif
	}

	// sam_set_setting(id, value) -> bool. Validated against the declaration (a slider is snapped
	// to its step and must be inside its range, a dropdown value must be one of its options, a
	// toggle takes a boolean only, a text at most 31 characters), written to the mod's file, and
	// announced with mod.on_setting_changed (source "script"). false with the reason in the log;
	// the value already in force is true and silent.
	int lua_sam_set_setting(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* id = luaL_checkstring(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		SAMSettings::Value v;
		samLuaToSettingValue(Ls, 2, v);
		std::string why;
		if ( !SAMSettings::set(g_currentNs, id ? id : "", v, SAMSettings::Source::Script, &why) )
		{
			SAM_WARN("LUA", "sam_set_setting refused: " + why);
			lua_pushboolean(Ls, 0);
			return 1;
		}
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)id;
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_list_settings() -> array of { id, type, label, value, default [, min, max] [, step]
	// [, options] } for the calling mod, in registration order. min/max only when declared,
	// step only for a slider, options only for a dropdown.
	int lua_sam_list_settings(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_newtable(Ls);
#ifdef SAM_LUA_HAVE_BARONY
		int n = 0;
		for ( const SAMSettings::Setting& s : SAMSettings::settings() )
		{
			if ( s.ns != g_currentNs ) { continue; }
			lua_newtable(Ls);
			lua_pushstring(Ls, s.id.c_str());                     lua_setfield(Ls, -2, "id");
			lua_pushstring(Ls, SAMSettings::typeName(s.type));    lua_setfield(Ls, -2, "type");
			lua_pushstring(Ls, s.label.c_str());                  lua_setfield(Ls, -2, "label");
			samLuaPushSettingValue(Ls, s.value);                  lua_setfield(Ls, -2, "value");
			samLuaPushSettingValue(Ls, s.def);                    lua_setfield(Ls, -2, "default");
			if ( s.hasMin ) { lua_pushnumber(Ls, (lua_Number)s.min); lua_setfield(Ls, -2, "min"); }
			if ( s.hasMax ) { lua_pushnumber(Ls, (lua_Number)s.max); lua_setfield(Ls, -2, "max"); }
			if ( s.type == SAMSettings::Type::Slider ) { lua_pushnumber(Ls, (lua_Number)s.step); lua_setfield(Ls, -2, "step"); }
			if ( s.type == SAMSettings::Type::Dropdown )
			{
				lua_newtable(Ls);
				int k = 0;
				for ( const std::string& o : s.options ) { lua_pushstring(Ls, o.c_str()); lua_rawseti(Ls, -2, ++k); }
				lua_setfield(Ls, -2, "options");
			}
			lua_rawseti(Ls, -2, ++n);
		}
#endif
		return 1;
	}

	int lua_sam_get_inventory_count(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushinteger(Ls, 0); return 1; }
		// Through the shared resolver, so this accepts everything sam_grant_item does.
		const int wantType = SAMCatalog::itemTypeFor(nameC ? nameC : "");
		// An unresolvable name is NOT "you have none of them". This answered 0 either way, with
		// no log, so a typo was indistinguishable from an empty bag -- the only call in this
		// family that failed in complete silence.
		if ( wantType < 0 )
		{
			SAM_ERROR("LUA", "sam_get_inventory_count: unknown item '" + std::string(nameC ? nameC : "")
				+ "'. Returning 0, which is NOT the same as owning none -- check the name.");
			lua_pushinteger(Ls, 0);
			return 1;
		}
		// Through the inventory mirror: on the host a remote player's backpack is the one their
		// own machine reported, never stats[p]->inventory, which there holds only copies of the
		// gear they wear. nil when this machine cannot see that backpack: 0 would read as "none".
		long long total = 0;
		if ( SAMMpInventory::countOf(player, wantType, "sam_get_inventory_count", &total) == SAMMpInventory::Seen::No )
		{
			lua_pushnil(Ls);
			return 1;
		}
		lua_pushinteger(Ls, (lua_Integer)total);
		return 1;
	}

	int lua_sam_has_effect(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { SAM_WARN("LUA", std::string("sam_has_effect: unknown effect '") + (nameC ? nameC : "") + "'. Valid: " + samEffectNameHint()); lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, stats[player]->getEffectActive(eff) != 0 ? 1 : 0);
		return 1;
	}

	// sam_get_effect_duration(player, "EFFECT") -> remaining ticks (50 = 1s). 0 if the
	// effect is not active; -1 for a permanent effect (no timer). Host only (the contract): a
	// client never counts effect timers down -- it holds only a "less than 5 s left" flag --
	// so an answer there would read an active effect as inactive.
	int lua_sam_get_effect_duration(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushinteger(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 || stats[player]->getEffectActive(eff) == 0 ) { lua_pushinteger(Ls, 0); return 1; }
		lua_pushinteger(Ls, (lua_Integer)stats[player]->EFFECTS_TIMERS[eff]);
		return 1;
	}

	// sam_get_effect_strength(player, "EFFECT") -> strength/tier (0 if inactive). Some
	// effects store a magnitude (GROWTH tiers, potion STR); this reads it. Client-readable.
	int lua_sam_get_effect_strength(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushinteger(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { lua_pushinteger(Ls, 0); return 1; }
		lua_pushinteger(Ls, (lua_Integer)stats[player]->getEffectActive(eff));
		return 1;
	}

	// sam_get_effects(player) -> array of { name, ticks, strength } for every active effect,
	// so a mod can react to "any debuff" or strip buffs without polling ~130 names one by
	// one. Includes custom pseudo-effect slots (135..) reported as "CUSTOM:<id>".
	int lua_sam_get_effects(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		lua_newtable(Ls);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return 1; } // empty array
		int n = 0;
		auto pushEntry = [&](const std::string& name, int id, Uint8 strength) {
			lua_newtable(Ls);
			lua_pushstring(Ls, name.c_str());                              lua_setfield(Ls, -2, "name");
			lua_pushinteger(Ls, (lua_Integer)stats[player]->EFFECTS_TIMERS[id]); lua_setfield(Ls, -2, "ticks");
			lua_pushinteger(Ls, (lua_Integer)strength);                    lua_setfield(Ls, -2, "strength");
			lua_rawseti(Ls, -2, ++n);
		};
		// One loop over every id, named exactly as the JS twin names them: the lowercase vanilla
		// name or the mod's "ns:effect" id (what player.on_effect_applied emits), and "CUSTOM:<id>"
		// for a live slot no mod named. This used to walk the UPPERCASE name table for vanilla
		// effects and push "" for an unnamed custom slot, so the same call answered differently
		// in the two languages.
		for ( int id = 0; id < NUMEFFECTS; ++id )
		{
			const Uint8 s = stats[player]->getEffectActive(id);
			if ( s == 0 ) { continue; }
			std::string name = SAMLua::effectNameFromId(id);
			if ( name.empty() ) { name = "CUSTOM:" + std::to_string(id); }
			pushEntry(name, id, s);
		}
		return 1;
	}

	// sam_clear_effects(player) -> count. Strip EVERY active status effect (buffs and debuffs
	// alike, vanilla and custom) from a player in one call. Host-authoritative. Returns how
	// many were cleared. (v1.5.0)
	int lua_sam_clear_effects(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_clear_effects refused: host only."); lua_pushinteger(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity || !stats[player] )
		{ SAM_ERROR("LUA", "sam_clear_effects: invalid player index " + std::to_string(player) + "."); lua_pushinteger(Ls, 0); return 1; }
		int cleared = 0;
		for ( int eff = 0; eff < NUMEFFECTS; ++eff )
		{
			if ( stats[player]->getEffectActive(eff) != 0 )
			{
				players[player]->entity->setEffect(eff, false, 0, true);
				++cleared;
			}
		}
		SAM_INFO("SAM", "sam_clear_effects: cleared " + std::to_string(cleared) + " effect(s) from player " + std::to_string(player));
		lua_pushinteger(Ls, (lua_Integer)cleared);
		return 1;
	}

	// sam_set_effect_duration(player, "EFFECT", ticks) -> bool. Retime an ALREADY-ACTIVE effect
	// in place (no re-fire of on_effect_applied). No-op if the effect isn't currently active, so
	// it never spawns a fresh one. Host-authoritative. 50 ticks = 1 second. (v1.5.0)
	int lua_sam_set_effect_duration(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		const int ticks = (int)luaL_checkinteger(Ls, 3);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_effect_duration refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity || !stats[player] )
		{ SAM_ERROR("LUA", "sam_set_effect_duration: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { SAM_ERROR("LUA", std::string("sam_set_effect_duration: unknown effect '") + (nameC ? nameC : "") + "'. Valid: " + samEffectNameHint()); lua_pushboolean(Ls, 0); return 1; }
		if ( stats[player]->getEffectActive(eff) == 0 ) { lua_pushboolean(Ls, 0); return 1; } // not active: don't create it
		// value=true keeps it active; overrideEffectStrength=false preserves strength; overrideDuration=true writes ticks.
		players[player]->entity->setEffect(eff, true, ticks, true, true, false, true);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_set_effect_strength(player, "EFFECT", strength) -> bool. Change the magnitude/tier of an
	// ALREADY-ACTIVE effect while keeping its remaining duration. No-op if inactive. Host-only.
	// strength clamped 1..255. (v1.5.0)
	int lua_sam_set_effect_strength(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		const int strength = (int)luaL_checkinteger(Ls, 3);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_effect_strength refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity || !stats[player] )
		{ SAM_ERROR("LUA", "sam_set_effect_strength: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { SAM_ERROR("LUA", std::string("sam_set_effect_strength: unknown effect '") + (nameC ? nameC : "") + "'. Valid: " + samEffectNameHint()); lua_pushboolean(Ls, 0); return 1; }
		if ( stats[player]->getEffectActive(eff) == 0 ) { lua_pushboolean(Ls, 0); return 1; }
		const Uint8 st = (Uint8)(strength < 1 ? 1 : (strength > 255 ? 255 : strength));
		const int keepDur = stats[player]->EFFECTS_TIMERS[eff];
		// value=strength (Uint8) with overrideEffectStrength=true; keep the current duration.
		players[player]->entity->setEffect(eff, st, keepDur, true, true, true, true);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_get_item_category(item) -> category name ("WEAPON"/"ARMOR"/"GEM"/...), or nil.
	// `item` is a numeric item id (e.g. an event's item_type) OR a name (vanilla or "ns:item").
	// Lets a script react by category, e.g. reward identifying any GEM.
	int lua_sam_get_item_category(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		int type = -1;
		if ( lua_isnumber(Ls, 1) )
		{
			type = (int)lua_tointeger(Ls, 1);
		}
		else if ( lua_isstring(Ls, 1) )
		{
			const std::string name = lua_tostring(Ls, 1);
			if ( name.find(':') != std::string::npos ) { type = SAMItems::itemIdForIdString(name); }
			if ( type < 0 ) { type = SAMCatalog::itemTypeFor(name); }   // shared resolver
		}
		if ( type < 0 || type >= NUM_ITEM_SLOTS ) { lua_pushnil(Ls); return 1; }
		const std::string cat = SAMItems::categoryName((int)items[type].category);
		if ( cat.empty() ) { lua_pushnil(Ls); return 1; }
		lua_pushstring(Ls, cat.c_str());
		return 1;
	}

	// Resolve a player's class to a display name. playerClassLangEntry (editor.cpp) is NOT
	// SAM-aware: for a custom id (>= SAM_CLASS_ID_BASE) it computes a bogus lang index
	// (3223 + id - CLASS_CONJURER) and returns an unrelated string, so a script gating on
	// sam_get_class(p) == "MyClass" never matched. Resolve custom ids from the registry —
	// the same source the class-select UI uses — before falling back to the vanilla lookup.
	// Shared by the Lua and JS bindings so the two can't disagree.
	const char* samClassName(int player)
	{
		if ( player < 0 || player >= MAXPLAYERS ) { return ""; }
		const int cls = client_classes[player];
		if ( cls >= SAM_CLASS_ID_BASE )
		{
			const SAMClassDef* def = SAMClasses::getClass(cls);
			// The class's ID, not its display name. `name` is "Blood Knight" and resolved
			// NOWHERE -- every class consumer (patch_class, add_class_passive, ...) matches on
			// `id`, so the old value could not be fed back into anything. sam_get_race has
			// always done it this way; this is the same shape.
			return def ? def->id.c_str() : "";
		}
		// Vanilla stays the lowercase engine name ("barbarian"), which classIdForIdString now
		// accepts, so both halves of this function round-trip.
		return playerClassLangEntry(cls, player);
	}

	int lua_sam_get_class(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		if ( player < 0 || player >= MAXPLAYERS ) { lua_pushnil(Ls); return 1; }
		lua_pushstring(Ls, samClassName(player));
		return 1;
	}

	// The race identifier for a player: a custom race's "namespace:race" id, or the
	// vanilla race's name ("human", "skeleton", "goatman", ...). Lets a race behavior
	// script gate its logic to players of that race.
	const char* samRaceName(int player)
	{
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return ""; }
		const int race = stats[player]->playerRace;
		if ( race >= SAM_RACE_ID_BASE )
		{
			const SAMRaceDef* def = SAMRaces::get(race);
			return def ? def->id.c_str() : "";
		}
		const int mon = (int)getMonsterFromPlayerRace(race);
		if ( mon >= 0 && mon < NUMMONSTERS ) { return monstertypename[mon]; }
		return "";
	}
	int lua_sam_get_race(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		// nil for a slot with no player in it too, as the JS twin answers; it used to be "".
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushnil(Ls); return 1; }
		lua_pushstring(Ls, samRaceName(player));
		return 1;
	}

	int lua_sam_get_kills(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		lua_pushinteger(Ls, (lua_Integer)((player >= 0 && player < MAXPLAYERS) ? g_samSessionKills[player] : 0));
		return 1;
	}

	// ---- multiplayer awareness ----------------------------------------------------------
	//
	// Most S.A.M functions are host-only and warn-and-return-false on a client, but until now a
	// script had no way to ASK. So a co-op mod either spammed the log with refusals or guessed.
	// These three are the cheap fix and are safe to call from anywhere.

	// sam_test_done(passed, failed) -> boolean. Ends an unattended -samtest run and sets the
	// process exit code from `failed`. Outside a test run it does nothing and answers false,
	// which is what lets a mod keep the call in when it is published.
	int lua_sam_test_done(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int passed = (int)luaL_checkinteger(Ls, 1);
		const int failed = (int)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, SAMTest::done(passed, failed) ? 1 : 0);
#else
		(void)passed; (void)failed;
		lua_pushboolean(Ls, 0);
#endif
		return 1;
	}

	// sam_set_game_speed(multiplier [, ticks]) -> bool. Run the simulation at `multiplier` times
	// real time, 0.1..8: everything counted in game ticks scales (monsters, hunger, effects, timers,
	// on_tick), nothing on a wall clock does (rendering, UI, sam_hitstop, a screen flash). ticks > 0
	// makes it a temporary window that ends by itself, which is how a mod does bullet time.
	// SINGLEPLAYER ONLY: SAMSpeed::set refuses a netgame and says why, once.
	int lua_sam_set_game_speed(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const double mult = (double)luaL_checknumber(Ls, 1);
		const lua_Integer forTicks = luaL_optinteger(Ls, 2, 0);
#ifdef SAM_LUA_HAVE_BARONY
		// Past INT_MAX ticks means "for the rest of the run"; clamp rather than wrap. Negative is
		// "not a window".
		const int window = forTicks > 2147483647LL ? 2147483647 : ( forTicks < 0 ? 0 : (int)forTicks );
		lua_pushboolean(Ls, SAMSpeed::set(mult, window) ? 1 : 0);
#else
		(void)mult; (void)forTicks;
		lua_pushboolean(Ls, 0);
#endif
		return 1;
	}

	// sam_get_game_speed() -> number. 1.0 with nothing set, and always 1.0 in a netgame.
	int lua_sam_get_game_speed(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushnumber(Ls, (lua_Number)SAMSpeed::multiplier());
#else
		lua_pushnumber(Ls, (lua_Number)1.0);
#endif
		return 1;
	}

	// sam_is_host() -> boolean. True in singleplayer and on the server; false on a client.
	int lua_sam_is_host(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, (multiplayer != CLIENT) ? 1 : 0);
#else
		lua_pushboolean(Ls, 1);
#endif
		return 1;
	}

	// sam_player_count() -> integer. How many players are actually connected right now.
	int lua_sam_player_count(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		int n = 0;
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( !client_disconnected[i] ) { ++n; }
		}
		lua_pushinteger(Ls, (lua_Integer)n);
#else
		lua_pushinteger(Ls, 1);
#endif
		return 1;
	}

	// sam_local_player() -> integer. The player index THIS machine controls. On a client that is
	// not 0, which is the assumption most single-player-tested mods quietly bake in.
	int lua_sam_local_player(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushinteger(Ls, (lua_Integer)clientnum);
#else
		lua_pushinteger(Ls, 0);
#endif
		return 1;
	}

	// ---- mod-defined networking ---------------------------------------------------------
	//
	// Barony's packets are a fixed table of four-character ids, so a mod could never send
	// anything of its own: a co-op mod had no way to tell the other machine ANYTHING. This
	// adds one generic envelope, "SAMP", carrying a mod-chosen tag and an opaque payload.
	//
	// Deliberately NOT chunked. NET_PACKET_SIZE is 512 and this is a single datagram, so an
	// oversized send is REFUSED with a clear error rather than silently truncated -- a mod
	// that loses the tail of its own message is a much worse bug to chase than one that was
	// told no. Send several small messages. Delivery is reliable but NOT ordered: number them if
	// order matters.

	// sam_send_packet(target, tag, payload) -> boolean
	//   host:   target is a player index 0..3, or -1 for every connected client
	//   client: target is ignored; the packet always goes to the host
	// The other side receives an "on_packet" event with .from, .tag and .payload.
	int lua_sam_send_packet(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int target = (int)luaL_checkinteger(Ls, 1);
		size_t tagLen = 0, payLen = 0;
		const char* tagC = luaL_checklstring(Ls, 2, &tagLen);
		const char* payC = luaL_optlstring(Ls, 3, "", &payLen);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == SINGLE )
		{
			// Nothing to send to. Not an error: a mod should be able to call this
			// unconditionally and have it be a no-op in singleplayer.
			lua_pushboolean(Ls, 0);
			return 1;
		}
		if ( tagLen == 0 || tagLen > SAMLua::SAM_PACKET_MAX_TAG )
		{
			SAM_ERROR("LUA", "sam_send_packet: tag must be 1.." + std::to_string(SAMLua::SAM_PACKET_MAX_TAG)
				+ " characters (got " + std::to_string(tagLen) + "). Packet not sent.");
			lua_pushboolean(Ls, 0); return 1;
		}
		if ( payLen > SAMLua::SAM_PACKET_MAX_PAYLOAD )
		{
			SAM_ERROR("LUA", "sam_send_packet: payload is " + std::to_string(payLen)
				+ " bytes, the limit is " + std::to_string(SAMLua::SAM_PACKET_MAX_PAYLOAD)
				+ " (one datagram). Packet not sent -- split it into several packets.");
			lua_pushboolean(Ls, 0); return 1;
		}
		const bool ok = SAMLua::sendModPacket(target, std::string(tagC, tagLen), std::string(payC, payLen));
		lua_pushboolean(Ls, ok ? 1 : 0);
		return 1;
#else
		(void)target; (void)tagC; (void)payC; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// ---- script HUD ---------------------------------------------------------------------
	// Widgets persist until changed or cleared, so a mod updates them on an event rather
	// than every frame. Colours are 0xRRGGBBAA. Coordinates are virtual screen pixels, the
	// same space the vanilla HUD uses, so a mod lines up at any resolution.

	static Uint32 samHudColor(lua_State* Ls, int idx, Uint32 dflt)
	{
		if ( lua_isnoneornil(Ls, idx) ) { return dflt; }
		const unsigned long long v = (unsigned long long)luaL_checkinteger(Ls, idx);
		return makeColor((Uint8)((v >> 24) & 0xFF), (Uint8)((v >> 16) & 0xFF),
		                 (Uint8)((v >> 8) & 0xFF),  (Uint8)(v & 0xFF));
	}

	// sam_hud_text(id, x, y, text [, 0xRRGGBBAA]) -> boolean
	int lua_sam_hud_text(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* id = luaL_checkstring(Ls, 1);
		const int x = (int)luaL_checkinteger(Ls, 2);
		const int y = (int)luaL_checkinteger(Ls, 3);
		const char* val = luaL_checkstring(Ls, 4);
		const Uint32 col = samHudColor(Ls, 5, makeColor(255, 255, 255, 255));
		lua_pushboolean(Ls, SAMHud::text(g_currentNs, id ? id : "", x, y, val ? val : "", col) ? 1 : 0);
		return 1;
	}

	// sam_hud_bar(id, x, y, w, h, frac [, 0xRRGGBBAA]) -> boolean
	int lua_sam_hud_bar(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* id = luaL_checkstring(Ls, 1);
		const int x = (int)luaL_checkinteger(Ls, 2);
		const int y = (int)luaL_checkinteger(Ls, 3);
		const int w = (int)luaL_checkinteger(Ls, 4);
		const int h = (int)luaL_checkinteger(Ls, 5);
		const double frac = (double)luaL_checknumber(Ls, 6);
		const Uint32 col = samHudColor(Ls, 7, makeColor(200, 40, 40, 255));
		lua_pushboolean(Ls, SAMHud::bar(g_currentNs, id ? id : "", x, y, w, h, frac, col) ? 1 : 0);
		return 1;
	}

	// sam_hud_clear([id]) -> boolean. No id clears the whole HUD.
	int lua_sam_hud_clear(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		// No id means "clear MY HUD", not everybody's: clearAll is the loader's, not a script's.
		if ( lua_isnoneornil(Ls, 1) ) { SAMHud::clearNamespace(g_currentNs); lua_pushboolean(Ls, 1); return 1; }
		const char* id = luaL_checkstring(Ls, 1);
		lua_pushboolean(Ls, SAMHud::clear(g_currentNs, id ? id : "") ? 1 : 0);
		return 1;
	}

	// ===== the mod's own pictures =========================================================
	//
	// A mod can now put ITS OWN art on the screen -- previously it could only replace one of
	// Barony's existing images, never add one. Two lifetimes, because the two uses want
	// opposite things: an OVERLAY covers the view for a set number of milliseconds and then
	// removes itself (a jumpscare, a title card, a death splash), while a HUD image stays put
	// until the script clears it (a portrait, a custom gauge, a marker).
	//
	// Naming a picture, in order: "ns:name" from any mod's manifest, a bare "name" meaning
	// one of the CALLING mod's declared images, or a path inside the calling mod's folder.
	// The manifest forms are checked at load; a raw path can only fail at draw time.

	// "contain" keeps the picture's aspect ratio inside the view; "stretch" (the default)
	// fills the whole view. Numbers work too, so a script can pass through a stored value.
	static int samImageFit(lua_State* Ls, int idx)
	{
		if ( lua_isnoneornil(Ls, idx) ) { return SAMImages::FIT_STRETCH; }
		if ( lua_isnumber(Ls, idx) ) { return (int)lua_tointeger(Ls, idx); }
		const char* fs = lua_tostring(Ls, idx);
		if ( fs && (strcmp(fs, "contain") == 0 || strcmp(fs, "fit") == 0) ) { return SAMImages::FIT_CONTAIN; }
		return SAMImages::FIT_STRETCH;
	}

	// sam_show_image(player, image [, duration_ms [, alpha [, "stretch"|"contain" ]]]) -> boolean
	// duration_ms <= 0 means "stay until sam_hide_image". alpha is 0..255.
	int lua_sam_show_image(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* img = luaL_checkstring(Ls, 2);
		const int ms = lua_isnoneornil(Ls, 3) ? 0 : (int)luaL_checkinteger(Ls, 3);
		const int alpha = lua_isnoneornil(Ls, 4) ? 255 : (int)luaL_checkinteger(Ls, 4);
		const int fit = samImageFit(Ls, 5);
		lua_pushboolean(Ls, SAMImages::show(player, g_currentNs, img ? img : "",
			ms, alpha, fit, 0, 0, 0, 0) ? 1 : 0);
		return 1;
	}

	// sam_show_image_at(player, image, x, y, w, h [, duration_ms [, alpha ]]) -> boolean
	// Coordinates are virtual screen pixels, the same space sam_hud_text uses. w or h of 0
	// means "the picture's own size" for that axis. Still drawn OVER the HUD.
	int lua_sam_show_image_at(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* img = luaL_checkstring(Ls, 2);
		const int x = (int)luaL_checkinteger(Ls, 3);
		const int y = (int)luaL_checkinteger(Ls, 4);
		const int w = (int)luaL_checkinteger(Ls, 5);
		const int h = (int)luaL_checkinteger(Ls, 6);
		const int ms = lua_isnoneornil(Ls, 7) ? 0 : (int)luaL_checkinteger(Ls, 7);
		const int alpha = lua_isnoneornil(Ls, 8) ? 255 : (int)luaL_checkinteger(Ls, 8);
		lua_pushboolean(Ls, SAMImages::show(player, g_currentNs, img ? img : "",
			ms, alpha, SAMImages::FIT_RECT, x, y, w, h) ? 1 : 0);
		return 1;
	}

	// sam_hide_image([player]) -> boolean. No player clears every player's overlay.
	int lua_sam_hide_image(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		if ( lua_isnoneornil(Ls, 1) )
		{
			bool any = false;
			for ( int c = 0; c < MAXPLAYERS; ++c ) { if ( SAMImages::hide(c) ) { any = true; } }
			lua_pushboolean(Ls, any ? 1 : 0);
			return 1;
		}
		lua_pushboolean(Ls, SAMImages::hide((int)luaL_checkinteger(Ls, 1)) ? 1 : 0);
		return 1;
	}

	// sam_hud_image(id, x, y, w, h, image [, 0xRRGGBBAA]) -> boolean
	// A persistent picture in the script HUD. w/h of 0 means the picture's own size.
	// The colour is MIXED with the art, so white (the default) leaves it untouched and the
	// alpha byte fades it. Cleared by sam_hud_clear(id) like any other HUD element.
	int lua_sam_hud_image(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* id = luaL_checkstring(Ls, 1);
		const int x = (int)luaL_checkinteger(Ls, 2);
		const int y = (int)luaL_checkinteger(Ls, 3);
		const int w = (int)luaL_checkinteger(Ls, 4);
		const int h = (int)luaL_checkinteger(Ls, 5);
		const char* img = luaL_checkstring(Ls, 6);
		const Uint32 col = samHudColor(Ls, 7, makeColor(255, 255, 255, 255));
		const std::string path = SAMImages::resolve(g_currentNs, img ? img : "");
		if ( path.empty() ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMHud::image(g_currentNs, id ? id : "", x, y, w, h, path, col) ? 1 : 0);
		return 1;
	}

	// ===== interactive panels =============================================================
	//
	// sam_hud_* draws and cannot listen. These open a real panel the player can click, built
	// on the engine's own Frame/Button widgets. A click fires the ui.on_click event carrying
	// the panel and widget ids, rather than taking a callback function: an event crosses the
	// Lua/JS boundary for free and cannot leave a dangling reference in a C callback.
	//
	// Panel and widget ids are scoped to the calling mod, so two mods can both own a "main".

	// sam_ui_open(panel, x, y, w, h [, title [, modal]]) -> boolean
	int lua_sam_ui_open(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const int x = (int)luaL_checkinteger(Ls, 2);
		const int y = (int)luaL_checkinteger(Ls, 3);
		const int w = (int)luaL_checkinteger(Ls, 4);
		const int h = (int)luaL_checkinteger(Ls, 5);
		const char* title = lua_isnoneornil(Ls, 6) ? "" : luaL_checkstring(Ls, 6);
		const bool modal = samBoolArg(Ls, 7, false);   // one boolean rule: 0 is false in both runtimes
		lua_pushboolean(Ls, SAMUi::open(g_currentNs, panel ? panel : "", x, y, w, h,
			title ? title : "", modal) ? 1 : 0);
		return 1;
	}

	// sam_ui_close([panel]) -> boolean. No panel closes every panel THIS mod opened.
	int lua_sam_ui_close(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		if ( lua_isnoneornil(Ls, 1) )
		{
			SAMUi::closeNamespace(g_currentNs);
			lua_pushboolean(Ls, 1);
			return 1;
		}
		const char* panel = luaL_checkstring(Ls, 1);
		lua_pushboolean(Ls, SAMUi::close(g_currentNs, panel ? panel : "") ? 1 : 0);
		return 1;
	}

	// sam_ui_is_open(panel [, player]) -> boolean. `player` defaults to the player the current event
	// is about, else this machine's own. On the host a remote player's answer is what their game last
	// reported (SAMUi::isOpenFor), so it lags that player by the network delay.
	int lua_sam_ui_is_open(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const int player = lua_isnoneornil(Ls, 2) ? -1 : (int)luaL_checkinteger(Ls, 2);
		lua_pushboolean(Ls, SAMUi::isOpenFor(player, g_currentNs, panel ? panel : "") ? 1 : 0);
		return 1;
	}

	// sam_ui_clear(panel) -> boolean. Empties the panel but leaves it open -- what a search
	// box does on every keystroke.
	int lua_sam_ui_clear(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		lua_pushboolean(Ls, SAMUi::clearWidgets(g_currentNs, panel ? panel : "") ? 1 : 0);
		return 1;
	}

	// sam_ui_label(panel, id, x, y, w, text [, 0xRRGGBBAA]) -> boolean
	int lua_sam_ui_label(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		const int x = (int)luaL_checkinteger(Ls, 3);
		const int y = (int)luaL_checkinteger(Ls, 4);
		const int w = (int)luaL_checkinteger(Ls, 5);
		const char* text = luaL_checkstring(Ls, 6);
		const Uint32 col = samHudColor(Ls, 7, makeColor(220, 210, 190, 255));
		lua_pushboolean(Ls, SAMUi::label(g_currentNs, panel ? panel : "", id ? id : "",
			x, y, w, text ? text : "", col) ? 1 : 0);
		return 1;
	}

	// sam_ui_button(panel, id, x, y, w, h, text) -> boolean
	// Clicking fires ui.on_click with .mod, .panel and .widget.
	int lua_sam_ui_button(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		const int x = (int)luaL_checkinteger(Ls, 3);
		const int y = (int)luaL_checkinteger(Ls, 4);
		const int w = (int)luaL_checkinteger(Ls, 5);
		const int h = (int)luaL_checkinteger(Ls, 6);
		const char* text = luaL_checkstring(Ls, 7);
		lua_pushboolean(Ls, SAMUi::button(g_currentNs, panel ? panel : "", id ? id : "",
			x, y, w, h, text ? text : "") ? 1 : 0);
		return 1;
	}

	// sam_ui_image(panel, id, x, y, w, h, image [, 0xRRGGBBAA]) -> boolean
	int lua_sam_ui_image(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		const int x = (int)luaL_checkinteger(Ls, 3);
		const int y = (int)luaL_checkinteger(Ls, 4);
		const int w = (int)luaL_checkinteger(Ls, 5);
		const int h = (int)luaL_checkinteger(Ls, 6);
		const char* img = luaL_checkstring(Ls, 7);
		const Uint32 col = samHudColor(Ls, 8, makeColor(255, 255, 255, 255));
		const std::string path = SAMImages::resolve(g_currentNs, img ? img : "");
		if ( path.empty() ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMUi::image(g_currentNs, panel ? panel : "", id ? id : "",
			x, y, w, h, path, col) ? 1 : 0);
		return 1;
	}

	// sam_ui_list(panel, id, x, y, w, h) -> boolean
	// A scrolling, clickable list -- the widget a browser is made of. Rows are added
	// separately with sam_ui_list_add so a search box can rebuild the contents on every
	// keystroke without recreating the list itself.
	int lua_sam_ui_list(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		const int x = (int)luaL_checkinteger(Ls, 3);
		const int y = (int)luaL_checkinteger(Ls, 4);
		const int w = (int)luaL_checkinteger(Ls, 5);
		const int h = (int)luaL_checkinteger(Ls, 6);
		lua_pushboolean(Ls, SAMUi::list(g_currentNs, panel ? panel : "", id ? id : "",
			x, y, w, h) ? 1 : 0);
		return 1;
	}

	// sam_ui_list_add(panel, list_id, row_id, text [, 0xRRGGBBAA]) -> boolean
	// Clicking the row fires ui.on_select with row_id in .value.
	int lua_sam_ui_list_add(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		const char* rowId = luaL_checkstring(Ls, 3);
		const char* text = luaL_checkstring(Ls, 4);
		const Uint32 col = samHudColor(Ls, 5, makeColor(220, 210, 190, 255));
		lua_pushboolean(Ls, SAMUi::listAdd(g_currentNs, panel ? panel : "", id ? id : "",
			rowId ? rowId : "", text ? text : "", col) ? 1 : 0);
		return 1;
	}

	// sam_ui_list_clear(panel, list_id) -> boolean
	int lua_sam_ui_list_clear(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		lua_pushboolean(Ls, SAMUi::listClear(g_currentNs, panel ? panel : "", id ? id : "") ? 1 : 0);
		return 1;
	}

	// sam_ui_input(panel, id, x, y, w, h [, initial_text]) -> boolean
	// An editable text box. Pressing enter fires ui.on_submit with the text in .value.
	int lua_sam_ui_input(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		const int x = (int)luaL_checkinteger(Ls, 3);
		const int y = (int)luaL_checkinteger(Ls, 4);
		const int w = (int)luaL_checkinteger(Ls, 5);
		const int h = (int)luaL_checkinteger(Ls, 6);
		const char* text = lua_isnoneornil(Ls, 7) ? "" : luaL_checkstring(Ls, 7);
		lua_pushboolean(Ls, SAMUi::input(g_currentNs, panel ? panel : "", id ? id : "",
			x, y, w, h, text ? text : "") ? 1 : 0);
		return 1;
	}

	// sam_ui_input_text(panel, id [, player]) -> string. What the player has typed so far, without
	// waiting for them to press enter. `player` as for sam_ui_is_open.
	int lua_sam_ui_input_text(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		const int player = lua_isnoneornil(Ls, 3) ? -1 : (int)luaL_checkinteger(Ls, 3);
		lua_pushstring(Ls, SAMUi::inputTextFor(player, g_currentNs, panel ? panel : "", id ? id : "").c_str());
		return 1;
	}

	// ---- appearance. Nothing about a mod's panel should look like "the S.A.M style". ----

	// sam_ui_panel_style(panel [, bg [, border [, border_width]]]) -> boolean
	// Colours are 0xRRGGBBAA; 0 or omitted leaves that one alone.
	int lua_sam_ui_panel_style(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const Uint32 bg = samHudColor(Ls, 2, 0);
		const Uint32 border = samHudColor(Ls, 3, 0);
		const int bw = lua_isnoneornil(Ls, 4) ? -1 : (int)luaL_checkinteger(Ls, 4);
		lua_pushboolean(Ls, SAMUi::panelStyle(g_currentNs, panel ? panel : "", bg, border, bw) ? 1 : 0);
		return 1;
	}

	// sam_ui_font(panel, widget_or_empty, font) -> boolean
	// An empty widget id sets the whole panel's font, including widgets added later.
	// Faces the game ships: fonts/pixel_maz_multiline.ttf#16#2 (the panel default),
	// fonts/pixelmix.ttf#16#2, fonts/kongtext.ttf#16#2, fonts/pixel_maz.ttf#32#2 (large).
	int lua_sam_ui_font(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		const char* f = luaL_checkstring(Ls, 3);
		lua_pushboolean(Ls, SAMUi::font(g_currentNs, panel ? panel : "", id ? id : "",
			f ? f : "") ? 1 : 0);
		return 1;
	}

	// sam_ui_list_row_height(panel, list_id, px) -> boolean. 0 derives it from the font.
	int lua_sam_ui_list_row_height(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		const int px = (int)luaL_checkinteger(Ls, 3);
		lua_pushboolean(Ls, SAMUi::listRowHeight(g_currentNs, panel ? panel : "",
			id ? id : "", px) ? 1 : 0);
		return 1;
	}

	// sam_ui_text_size(text [, font]) -> width, height (nil if the font could not load).
	// Measure before you place. This is the answer to a label silently running underneath
	// the widget beside it.
	int lua_sam_ui_text_size(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* text = luaL_checkstring(Ls, 1);
		const char* f = lua_isnoneornil(Ls, 2) ? "" : luaL_checkstring(Ls, 2);
		int w = 0, h = 0;
		if ( !SAMUi::textSize(text ? text : "", f ? f : "", w, h) ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, w);
		lua_pushinteger(Ls, h);
		return 2;
	}

	// ===== reading the game's own content =================================================
	//
	// Until now a script could ask about the item in your hand and nothing else, so an index
	// of the game -- a recipe browser, a bestiary, a drop reference -- could not be written at
	// all. These walk the engine's live tables, so modded content is included automatically.
	//
	// They are read-only and therefore not host-gated: a client can browse safely.
	// They walk the whole table, so call once and keep the result rather than per frame.

	// sam_list_items([category]) -> array of { type, name, unidentified, category, level,
	//                                          weight, value, custom }
	int lua_sam_list_items(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* cat = lua_isnoneornil(Ls, 1) ? "" : luaL_checkstring(Ls, 1);
		const std::vector<SAMCatalog::ItemEntry> list = SAMCatalog::items(cat ? cat : "");
		lua_newtable(Ls);
		int n = 0;
		for ( const SAMCatalog::ItemEntry& e : list )
		{
			lua_newtable(Ls);
			lua_pushinteger(Ls, e.type);            lua_setfield(Ls, -2, "type");
			lua_pushstring(Ls, e.name.c_str());     lua_setfield(Ls, -2, "name");
			lua_pushstring(Ls, e.unidName.c_str()); lua_setfield(Ls, -2, "unidentified");
			lua_pushstring(Ls, e.category.c_str()); lua_setfield(Ls, -2, "category");
			lua_pushinteger(Ls, e.level);           lua_setfield(Ls, -2, "level");
			lua_pushinteger(Ls, e.weight);          lua_setfield(Ls, -2, "weight");
			lua_pushinteger(Ls, e.value);           lua_setfield(Ls, -2, "value");
			lua_pushboolean(Ls, e.custom ? 1 : 0);  lua_setfield(Ls, -2, "custom");
			lua_rawseti(Ls, -2, ++n);
		}
		return 1;
	}

	// sam_get_item_info(type_or_name) -> table | nil. Adds an `attributes` sub-table, which
	// is where a tooltip's numbers come from.
	int lua_sam_get_item_info(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		int type = -1;
		if ( lua_isnumber(Ls, 1) ) { type = (int)lua_tointeger(Ls, 1); }
		else { type = SAMCatalog::itemTypeFor(luaL_checkstring(Ls, 1)); }

		SAMCatalog::ItemEntry e;
		std::map<std::string, int> attrs;
		if ( !SAMCatalog::itemInfo(type, e, attrs) ) { lua_pushnil(Ls); return 1; }

		lua_newtable(Ls);
		lua_pushinteger(Ls, e.type);            lua_setfield(Ls, -2, "type");
		lua_pushstring(Ls, e.name.c_str());     lua_setfield(Ls, -2, "name");
		lua_pushstring(Ls, e.unidName.c_str()); lua_setfield(Ls, -2, "unidentified");
		lua_pushstring(Ls, e.category.c_str()); lua_setfield(Ls, -2, "category");
		lua_pushinteger(Ls, e.level);           lua_setfield(Ls, -2, "level");
		lua_pushinteger(Ls, e.weight);          lua_setfield(Ls, -2, "weight");
		lua_pushinteger(Ls, e.value);           lua_setfield(Ls, -2, "value");
		lua_pushboolean(Ls, e.custom ? 1 : 0);  lua_setfield(Ls, -2, "custom");
		lua_newtable(Ls);
		for ( const auto& kv : attrs )
		{
			lua_pushinteger(Ls, kv.second);
			lua_setfield(Ls, -2, kv.first.c_str());
		}
		lua_setfield(Ls, -2, "attributes");
		return 1;
	}

	// sam_list_monsters() -> array of { type, name }
	int lua_sam_list_monsters(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const std::vector<SAMCatalog::MonsterEntry> list = SAMCatalog::monsters();
		lua_newtable(Ls);
		int n = 0;
		for ( const SAMCatalog::MonsterEntry& e : list )
		{
			lua_newtable(Ls);
			lua_pushinteger(Ls, e.type);        lua_setfield(Ls, -2, "type");
			lua_pushstring(Ls, e.name.c_str()); lua_setfield(Ls, -2, "name");
			lua_rawseti(Ls, -2, ++n);
		}
		return 1;
	}

	// sam_list_spells() -> array of { id, name, cost }
	int lua_sam_list_spells(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const std::vector<SAMCatalog::SpellEntry> list = SAMCatalog::spells();
		lua_newtable(Ls);
		int n = 0;
		for ( const SAMCatalog::SpellEntry& e : list )
		{
			lua_newtable(Ls);
			lua_pushinteger(Ls, e.id);          lua_setfield(Ls, -2, "id");
			lua_pushstring(Ls, e.name.c_str()); lua_setfield(Ls, -2, "name");
			lua_pushinteger(Ls, e.cost);        lua_setfield(Ls, -2, "cost");
			lua_rawseti(Ls, -2, ++n);
		}
		return 1;
	}

	// sam_spawn_projectile(x, y, angle, speed [, damage [, lifetime_ticks [, model [, owner]]]])
	//   -> uid | nil
	// angle is radians (sam_get_facing's convention), speed is world pixels per tick -- a
	// vanilla arrow is about 8. On contact it fires on_projectile_hit with .projectile,
	// .target, .x, .y and .damage. Host only.
	int lua_sam_spawn_projectile(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const double x = (double)luaL_checknumber(Ls, 1);
		const double y = (double)luaL_checknumber(Ls, 2);
		const double angle = (double)luaL_checknumber(Ls, 3);
		const double speed = (double)luaL_checknumber(Ls, 4);
		const int dmg = (int)luaL_optinteger(Ls, 5, 0);
		const int life = (int)luaL_optinteger(Ls, 6, 100);
		const char* model = lua_isnoneornil(Ls, 7) ? "" : luaL_checkstring(Ls, 7);
		const int owner = (int)luaL_optinteger(Ls, 8, -1);
		const unsigned long long uid = SAMLua::spawnProjectile(owner, x, y, angle, speed,
			dmg, life, model ? model : "");
		if ( uid == 0 ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)uid);
		return 1;
	}

	// sam_get_image_size(image) -> width, height (nil if it could not be loaded).
	// Loading a picture just to measure it is the only way to centre one, so this exists
	// rather than making every mod hard-code the numbers it exported at.
	int lua_sam_get_image_size(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* img = luaL_checkstring(Ls, 1);
		int w = 0, h = 0;
		if ( !SAMImages::size(g_currentNs, img ? img : "", w, h) ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, w);
		lua_pushinteger(Ls, h);
		return 2;
	}

	// ===== world, perception and truth ====================================================
	//
	// ARGUMENT CONVENTION: everything added here takes a UID, never a player index. The older
	// API is split (sam_get_stat wants an index, sam_get_monster_stat wants a uid) and that
	// mismatch is the most common scripting mistake there is, because it fails SILENTLY -- a
	// uid passed where an index belongs simply falls out of range and returns nothing. Call
	// sam_get_player_uid(n) once and pass uids from there.

	// Accept both spellings of a skill: the class schema uses "PRO_SWORD" while the
	// player.on_proficiency_increased event hands scripts "sword", and the in-game names
	// differ again (Lockpicking is shown as Tinkering, Appraisal as Lore). Take all of them.
	static int samSkillFromName(const char* nameC)
	{
		if ( !nameC ) { return -1; }
		std::string n = nameC;
		for ( char& c : n ) { c = (char)std::toupper((unsigned char)c); }
		if ( n.rfind("PRO_", 0) != 0 ) { n = "PRO_" + n; }
		static const std::map<std::string, int> m = {
			{ "PRO_LOCKPICKING", PRO_LOCKPICKING }, { "PRO_STEALTH", PRO_STEALTH },
			{ "PRO_TRADING", PRO_TRADING },          { "PRO_APPRAISAL", PRO_APPRAISAL },
			{ "PRO_LEADERSHIP", PRO_LEADERSHIP },    { "PRO_RANGED", PRO_RANGED },
			{ "PRO_SWORD", PRO_SWORD },              { "PRO_MACE", PRO_MACE },
			{ "PRO_AXE", PRO_AXE },                  { "PRO_POLEARM", PRO_POLEARM },
			{ "PRO_SHIELD", PRO_SHIELD },            { "PRO_UNARMED", PRO_UNARMED },
			{ "PRO_ALCHEMY", PRO_ALCHEMY },          { "PRO_THAUMATURGY", PRO_THAUMATURGY },
			{ "PRO_MYSTICISM", PRO_MYSTICISM },      { "PRO_SORCERY", PRO_SORCERY },
			{ "PRO_TINKERING", PRO_LOCKPICKING },    { "PRO_LORE", PRO_APPRAISAL },
			{ "PRO_BLOCKING", PRO_SHIELD },
		};
		auto it = m.find(n);
		return ( it != m.end() ) ? it->second : -1;
	}

	// sam_get_tile(x, y) -> table | nil
	int lua_sam_get_tile(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int x = (int)luaL_checkinteger(Ls, 1);
		const int y = (int)luaL_checkinteger(Ls, 2);
		const SAMWorld::TileInfo t = SAMWorld::tile(x, y);
		if ( !t.valid ) { lua_pushnil(Ls); return 1; }
		lua_newtable(Ls);
		lua_pushinteger(Ls, t.wall);      lua_setfield(Ls, -2, "wall");
		lua_pushinteger(Ls, t.floor);     lua_setfield(Ls, -2, "floor");
		lua_pushinteger(Ls, t.ceiling);   lua_setfield(Ls, -2, "ceiling");
		lua_pushboolean(Ls, t.solid);     lua_setfield(Ls, -2, "solid");
		lua_pushboolean(Ls, t.water);     lua_setfield(Ls, -2, "water");
		lua_pushboolean(Ls, t.lava);      lua_setfield(Ls, -2, "lava");
		lua_pushboolean(Ls, t.walkable);  lua_setfield(Ls, -2, "walkable");
		return 1;
	}

	// sam_set_tile(x, y, layer, tileId) -> boolean. layer 0=floor 1=wall 2=ceiling.
	int lua_sam_set_tile(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int x = (int)luaL_checkinteger(Ls, 1);
		const int y = (int)luaL_checkinteger(Ls, 2);
		const int l = (int)luaL_checkinteger(Ls, 3);
		const int id = (int)luaL_checkinteger(Ls, 4);
		lua_pushboolean(Ls, SAMWorld::setTile(x, y, l, id) ? 1 : 0);
		return 1;
	}

	// sam_is_spawnable(x, y) -> boolean. In bounds, not a wall, not lava.
	int lua_sam_is_spawnable(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushboolean(Ls, SAMWorld::spawnable((int)luaL_checkinteger(Ls, 1),
			(int)luaL_checkinteger(Ls, 2)) ? 1 : 0);
		return 1;
	}

	// sam_line_of_sight(x1, y1, x2, y2 [, blockedByEntities]) -> visible, blockedX, blockedY
	int lua_sam_line_of_sight(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const double x1 = (double)luaL_checknumber(Ls, 1);
		const double y1 = (double)luaL_checknumber(Ls, 2);
		const double x2 = (double)luaL_checknumber(Ls, 3);
		const double y2 = (double)luaL_checknumber(Ls, 4);
		const bool ents = samBoolArg(Ls, 5, false);   // one boolean rule: 0 is false in both runtimes
		int bx = -1, by = -1;
		const bool ok = SAMWorld::lineOfSight(x1, y1, x2, y2, ents, bx, by);
		lua_pushboolean(Ls, ok ? 1 : 0);
		lua_pushinteger(Ls, bx);
		lua_pushinteger(Ls, by);
		return 3;
	}

	// sam_tiles_connected(x1, y1, x2, y2 [, flying]) -> boolean
	// The softlock check: after a mod edits terrain, ask whether the exit is still reachable.
	int lua_sam_tiles_connected(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int x1 = (int)luaL_checkinteger(Ls, 1), y1 = (int)luaL_checkinteger(Ls, 2);
		const int x2 = (int)luaL_checkinteger(Ls, 3), y2 = (int)luaL_checkinteger(Ls, 4);
		const bool fly = samBoolArg(Ls, 5, false);   // one boolean rule: 0 is false in both runtimes
		lua_pushboolean(Ls, SAMWorld::connected(x1, y1, x2, y2, fly) ? 1 : 0);
		return 1;
	}

	// sam_get_light_at(x, y) -> 0..255
	int lua_sam_get_light_at(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		// Default -1, not 0: -1 is the shared lightmap the monster AI reads. Passing a real
		// player index asks the different question of how bright the tile looks on that screen.
		lua_pushinteger(Ls, SAMWorld::lightAt((int)luaL_checkinteger(Ls, 1),
			(int)luaL_checkinteger(Ls, 2), (int)luaL_optinteger(Ls, 3, -1)));
		return 1;
	}

	// sam_find_entities(x, y, radiusTiles [, kind]) -> { uid, ... }
	// What sam_get_nearby_entities cannot do: that one skips anything which is not a monster
	// or a player, so doors, chests, levers, gold and dropped items were invisible to scripts.
	int lua_sam_find_entities(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int x = (int)luaL_checkinteger(Ls, 1);
		const int y = (int)luaL_checkinteger(Ls, 2);
		const double r = (double)luaL_checknumber(Ls, 3);
		const char* kind = luaL_optstring(Ls, 4, "any");
		const std::vector<uint32_t> ids = SAMWorld::findEntities(x, y, r, kind ? kind : "any");
		lua_newtable(Ls);
		int i = 1;
		for ( uint32_t u : ids ) { lua_pushinteger(Ls, (lua_Integer)u); lua_rawseti(Ls, -2, i++); }
		return 1;
	}

	// sam_get_container_items(uid) -> array of tables | nil. Works on a chest or a creature.
	int lua_sam_get_container_items(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		std::vector<SAMWorld::ItemInfo> found;
		if ( !SAMWorld::containerItems((uint32_t)uid, found) ) { lua_pushnil(Ls); return 1; }
		lua_newtable(Ls);
		int i = 1;
		for ( const auto& it : found )
		{
			lua_newtable(Ls);
			lua_pushinteger(Ls, it.type);        lua_setfield(Ls, -2, "type");
			lua_pushstring(Ls, it.name.c_str()); lua_setfield(Ls, -2, "name");
			lua_pushinteger(Ls, it.count);       lua_setfield(Ls, -2, "count");
			lua_pushinteger(Ls, it.status);      lua_setfield(Ls, -2, "status");
			lua_pushinteger(Ls, it.beatitude);   lua_setfield(Ls, -2, "beatitude");
			lua_pushboolean(Ls, it.identified);  lua_setfield(Ls, -2, "identified");
			lua_rawseti(Ls, -2, i++);
		}
		return 1;
	}

	// The framework already FIRES world.on_door_opened but had no verb to open one.
	int lua_sam_set_door(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		lua_pushboolean(Ls, SAMWorld::setDoor((uint32_t)uid, samBoolArg(Ls, 2, false)) ? 1 : 0);
		return 1;
	}

	int lua_sam_set_door_locked(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		lua_pushboolean(Ls, SAMWorld::setDoorLocked((uint32_t)uid, samBoolArg(Ls, 2, false)) ? 1 : 0);
		return 1;
	}

	int lua_sam_power_entity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		lua_pushboolean(Ls, SAMWorld::powerEntity((uint32_t)uid, samBoolArg(Ls, 2, false)) ? 1 : 0);
		return 1;
	}

	int lua_sam_toggle_switch(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushboolean(Ls, SAMWorld::toggleSwitch((uint32_t)luaL_checkinteger(Ls, 1)) ? 1 : 0);
		return 1;
	}

	// sam_get_level_info() -> table. sam_get_floor returns a bare number that cannot tell a
	// secret branch from the main one, so location-gated content was impossible.
	int lua_sam_get_level_info(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const SAMWorld::LevelInfo l = SAMWorld::level();
		lua_newtable(Ls);
		lua_pushinteger(Ls, l.floor);          lua_setfield(Ls, -2, "floor");
		lua_pushstring(Ls, l.name.c_str());    lua_setfield(Ls, -2, "name");
		lua_pushstring(Ls, l.author.c_str());  lua_setfield(Ls, -2, "author");
		lua_pushinteger(Ls, l.width);          lua_setfield(Ls, -2, "width");
		lua_pushinteger(Ls, l.height);         lua_setfield(Ls, -2, "height");
		lua_pushboolean(Ls, l.secret);         lua_setfield(Ls, -2, "secret");
		lua_pushinteger(Ls, l.skybox);         lua_setfield(Ls, -2, "skybox");
		lua_pushboolean(Ls, l.noDigging);      lua_setfield(Ls, -2, "no_digging");
		lua_pushboolean(Ls, l.noTeleport);     lua_setfield(Ls, -2, "no_teleport");
		lua_pushboolean(Ls, l.noLevitation);   lua_setfield(Ls, -2, "no_levitation");
		return 1;
	}

	// ---- effective stats, skills and factions --------------------------------------------
	//
	// sam_get_stat reads the RAW Stat field. That is not the number the game fights with:
	// statGetSTR and friends add equipment, rings, status effects, hunger, drunkenness and
	// shapeshift on top. Any mod formula built on the raw value silently disagrees with the
	// engine the moment a player equips a ring, which is a horrible bug to chase. These
	// return what combat actually uses.

	// The readers' resolver, the one the JS twins use: a sentinel uid (0, -2, -3, -4 -- shared by
	// dozens of gibs and sparks at once) or one past 32 bits is nobody, silently.
	static Entity* samEntityFromUid(long long uid)
	{
		return samResolveEntityQuiet(uid);
	}

	// sam_get_effective_stat(uid, "STR") -> number
	int lua_sam_get_effective_stat(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samEntityFromUid(uid);
		if ( !e ) { lua_pushnil(Ls); return 1; }
		Stat* st = e->getStats();
		if ( !st ) { lua_pushnil(Ls); return 1; }
		const std::string n = samUpper(nameC);
		long long v = 0;
		if      ( n == "STR" ) { v = statGetSTR(st, e); }
		else if ( n == "DEX" ) { v = statGetDEX(st, e); }
		else if ( n == "CON" ) { v = statGetCON(st, e); }
		else if ( n == "INT" ) { v = statGetINT(st, e); }
		else if ( n == "PER" ) { v = statGetPER(st, e); }
		else if ( n == "CHR" ) { v = statGetCHR(st, e); }
		else
		{
			SAM_WARN("LUA", std::string("sam_get_effective_stat: unknown stat '")
				+ (nameC ? nameC : "") + "' (STR DEX CON INT PER CHR).");
			lua_pushnil(Ls); return 1;
		}
		lua_pushinteger(Ls, (lua_Integer)v);
		return 1;
#else
		(void)uid; (void)nameC; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_ac(uid) -> number. Armor class as the damage formula sees it, gear included.
	int lua_sam_get_ac(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samEntityFromUid(uid);
		if ( !e || !e->getStats() ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)AC(e->getStats()));
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_skill(uid, "sword" [, effective]) -> 0..100
	// `effective` (default true) includes the equipment bonus the game actually uses; pass
	// false for the raw trained rank. Skill ranks were completely unreadable before this,
	// even though the framework has always FIRED player.on_proficiency_increased.
	int lua_sam_get_skill(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		// samBoolArg, not lua_toboolean: the number 0 is TRUE to lua_toboolean and FALSE to
		// JS_ToBool, so sam_get_skill(uid, "sword", 0) returned the gear-modified proficiency in
		// Lua and the raw trained rank in JavaScript -- different numbers, neither flagged.
		const bool eff = samBoolArg(Ls, 3, true);
#ifdef SAM_LUA_HAVE_BARONY
		const int skill = samSkillFromName(nameC);
		if ( skill < 0 )
		{
			SAM_WARN("LUA", std::string("sam_get_skill: unknown skill '") + (nameC ? nameC : "")
				+ "'. Try sword, blocking, lore, tinkering, sorcery, ...");
			lua_pushnil(Ls); return 1;
		}
		Entity* e = samEntityFromUid(uid);
		if ( !e || !e->getStats() ) { lua_pushnil(Ls); return 1; }
		Stat* st = e->getStats();
		lua_pushinteger(Ls, (lua_Integer)(eff ? st->getModifiedProficiency(skill)
		                                       : st->getProficiency(skill)));
		return 1;
#else
		(void)uid; (void)nameC; (void)eff; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_is_enemy(uidA, uidB) -> boolean   |   sam_is_friend(uidA, uidB) -> boolean
	// Faction. Without this a script enumerating nearby creatures cannot tell a player's
	// summon or charmed follower from something hostile, so no AoE, aura, taunt or
	// heal-allies logic was possible.
	static int samFactionCheck(lua_State* Ls, bool wantEnemy)
	{
		SAMLogger::noteApiCall();
		const long long a = (long long)luaL_checkinteger(Ls, 1);
		const long long b = (long long)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* ea = samEntityFromUid(a);
		Entity* eb = samEntityFromUid(b);
		if ( !ea || !eb ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, (wantEnemy ? ea->checkEnemy(eb) : ea->checkFriend(eb)) ? 1 : 0);
		return 1;
#else
		(void)a; (void)b; (void)wantEnemy; lua_pushboolean(Ls, 0); return 1;
#endif
	}
	int lua_sam_is_enemy(lua_State* Ls)  { return samFactionCheck(Ls, true); }
	int lua_sam_is_friend(lua_State* Ls) { return samFactionCheck(Ls, false); }

	// sam_get_mods() -> array of { ns, name, version, author }
	// Cross-mod integration with zero engine work: soft-depend on another mod, avoid double
	// registering, or light up extra content when a partner mod is present.
	int lua_sam_get_mods(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_newtable(Ls);
#ifdef SAM_LUA_HAVE_BARONY
		int i = 1;
		for ( const SAMModManifest& m : SAMWorkshop::manifests() )
		{
			lua_newtable(Ls);
			lua_pushstring(Ls, m.ns.c_str());      lua_setfield(Ls, -2, "ns");
			lua_pushstring(Ls, m.name.c_str());    lua_setfield(Ls, -2, "name");
			lua_pushstring(Ls, m.version.c_str()); lua_setfield(Ls, -2, "version");
			lua_pushstring(Ls, m.author.c_str());  lua_setfield(Ls, -2, "author");
			lua_rawseti(Ls, -2, i++);
		}
#endif
		return 1;
	}

	// sam_is_mod_loaded("ns") -> boolean
	int lua_sam_is_mod_loaded(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* nsC = luaL_checkstring(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		const std::string want = nsC ? nsC : "";
		for ( const SAMModManifest& m : SAMWorkshop::manifests() )
		{
			if ( m.ns == want ) { lua_pushboolean(Ls, 1); return 1; }
		}
#else
		(void)nsC;
#endif
		lua_pushboolean(Ls, 0);
		return 1;
	}

	// ---- world-space presentation ---------------------------------------------------------
	//
	// Every effect S.A.M had was a CAMERA effect on the local player: screen flash, camera
	// shake, hitstop, impact frames. Nothing could mark a point in the WORLD, and every mod
	// sound was a flat 2D blast at identical volume for every player no matter where they
	// were standing. These fix both.

	// sam_play_sound_at(soundId, tileX, tileY [, volume]) -> boolean
	// Positional audio: it attenuates with distance and pans, so a trap firing across the
	// level is quiet, and in co-op each player hears it from where THEY are.
	int lua_sam_play_sound_at(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int snd = samResolveSoundId(Ls, 1);
		const double tx = (double)luaL_checknumber(Ls, 2);
		const double ty = (double)luaL_checknumber(Ls, 3);
		int vol = (int)luaL_optinteger(Ls, 4, 128);
#ifdef SAM_LUA_HAVE_BARONY
		if ( snd < 0 || snd >= (int)numsounds ) { lua_pushboolean(Ls, 0); return 1; }
		if ( vol < 0 ) { vol = 0; }
		if ( vol > 255 ) { vol = 255; }
		playSoundPos(tx * 16.0 + 8.0, ty * 16.0 + 8.0, (Uint16)snd, (Uint8)vol);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)snd; (void)tx; (void)ty; (void)vol; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_play_sound_entity(soundId, uid [, volume]) -> boolean
	// Same, but the sound follows the entity as it moves.
	int lua_sam_play_sound_entity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int snd = samResolveSoundId(Ls, 1);
		const long long uid = (long long)luaL_checkinteger(Ls, 2);
		int vol = (int)luaL_optinteger(Ls, 3, 128);
#ifdef SAM_LUA_HAVE_BARONY
		if ( snd < 0 || snd >= (int)numsounds ) { lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveEntityQuiet((long long)uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		if ( vol < 0 ) { vol = 0; }
		if ( vol > 255 ) { vol = 255; }
		playSoundEntity(e, (Uint16)snd, (Uint8)vol);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)snd; (void)uid; (void)vol; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// ---- music, and controlling sounds -----------------------------------------------------
	//
	// A mod's tracks come from music/ (or mod.json "music"); replacing vanilla music and giving
	// floors their own needs no script at all. These are for the moments a script decides:
	// a boss fight, a cutscene, a victory sting.

	// A bare name is one of the calling mod's own, like every other asset.
	static std::string samMusicId(const char* idC)
	{
		std::string id = idC ? idC : "";
		if ( id.find(':') == std::string::npos && !g_currentNs.empty() ) { id = g_currentNs + ":" + id; }
		return id;
	}

	// sam_play_music(id [, fade_seconds [, loop [, persist]]]) -> boolean
	// Takes over the music for EVERY player until sam_stop_music(), or until the floor changes
	// unless persist is true. A track that does not loop hands the music back when it ends.
	int lua_sam_play_music(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* idC = luaL_checkstring(Ls, 1);
		const double fade = (double)luaL_optnumber(Ls, 2, 1.5);
		const bool loop = samBoolArg(Ls, 3, true);
		const bool persist = samBoolArg(Ls, 4, false);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_play_music refused: host only. The host picks forced music and sends it to everyone.");
			lua_pushboolean(Ls, 0); return 1;
		}
		std::string err;
		if ( !SAMMusic::play(samMusicId(idC), fade, loop, persist, &err) )
		{
			SAM_ERROR("LUA", "sam_play_music: " + err + ". A track is a file in the mod's music/ folder,"
				" \"<namespace>:<file name>\"; sam_list_music() lists every one loaded.");
			lua_pushboolean(Ls, 0); return 1;
		}
		lua_pushboolean(Ls, 1); return 1;
#else
		(void)idC; (void)fade; (void)loop; (void)persist; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_stop_music() -> whether a script's track was playing. The game crossfades back to what
	// it would have played.
	int lua_sam_stop_music(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_stop_music refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMMusic::stop() ? 1 : 0); return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_get_music() -> the track playing on THIS machine: "ns:name", a vanilla name
	// ("mines02"), or nil when nothing plays. A replaced vanilla track answers with the vanilla
	// name, since that is what the game asked for.
	int lua_sam_get_music(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const std::string n = SAMMusic::nowPlaying();
		if ( n.empty() ) { lua_pushnil(Ls); } else { lua_pushstring(Ls, n.c_str()); }
		return 1;
#else
		lua_pushnil(Ls); return 1;
#endif
	}

	// sam_list_music() -> { "ns:name", ... } every track a loaded mod declares.
	int lua_sam_list_music(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_newtable(Ls);
#ifdef SAM_LUA_HAVE_BARONY
		int i = 1;
		for ( const std::string& id : SAMMusic::listIds() ) { lua_pushstring(Ls, id.c_str()); lua_rawseti(Ls, -2, i++); }
#endif
		return 1;
	}

	// sam_list_sounds() -> { "ns:name", ... } every sound a loaded mod adds.
	int lua_sam_list_sounds(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_newtable(Ls);
#ifdef SAM_LUA_HAVE_BARONY
		int i = 1;
		for ( const std::string& id : SAMSounds::listIds() ) { lua_pushstring(Ls, id.c_str()); lua_rawseti(Ls, -2, i++); }
#endif
		return 1;
	}

	// sam_stop_sound(id) -> how many were stopped here. Stops every playing copy of one of your
	// sounds; on the host, on every client too. The way to end a looping sound.
	int lua_sam_stop_sound(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* idC = luaL_checkstring(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		std::string id = idC ? idC : "";
		if ( !SAMSounds::isRegistered(id) && id.find(':') == std::string::npos && !g_currentNs.empty() ) { id = g_currentNs + ":" + id; }
		if ( !SAMSounds::isRegistered(id) )
		{
			SAM_WARN("LUA", "sam_stop_sound: unknown sound '" + std::string(idC ? idC : "") + "'.");
			lua_pushinteger(Ls, 0); return 1;
		}
		const int n = SAMSounds::stopById(id);
		SAMSounds::broadcastStop(id);
		lua_pushinteger(Ls, (lua_Integer)n); return 1;
#else
		(void)idC; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_spawn_particle(kind, tileX, tileY [, z [, scale]]) -> boolean
	// kind: "poof" | "explosion" | "bang" | "sleep"
	int lua_sam_spawn_particle(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* kindC = luaL_checkstring(Ls, 1);
		const double tx = (double)luaL_checknumber(Ls, 2);
		const double ty = (double)luaL_checknumber(Ls, 3);
		const double z = (double)luaL_optnumber(Ls, 4, 0.0);
		const double scale = (double)luaL_optnumber(Ls, 5, 1.0);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_spawn_particle refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		const std::string k = kindC ? kindC : "";
		const Sint16 px = (Sint16)(tx * 16.0 + 8.0);
		const Sint16 py = (Sint16)(ty * 16.0 + 8.0);
		const Sint16 pz = (Sint16)z;
		Entity* made = nullptr;
		// updateClients = true so co-op players all see it, not just the host.
		if      ( k == "poof" )      { made = spawnPoof(px, py, pz, SAMMpEntities::wirePoofScale(scale, "sam_spawn_particle"), true); }
		else if ( k == "explosion" ) { made = spawnExplosion(px, py, pz); }
		else if ( k == "bang" )      { made = spawnBang(px, py, pz); }
		else if ( k == "sleep" )     { made = spawnSleepZ(px, py, pz); }
		else
		{
			SAM_WARN("LUA", std::string("sam_spawn_particle: unknown kind '") + k
				+ "' (poof, explosion, bang, sleep).");
			lua_pushboolean(Ls, 0); return 1;
		}
		lua_pushboolean(Ls, made ? 1 : 0);
		return 1;
#else
		(void)kindC; (void)tx; (void)ty; (void)z; (void)scale; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_damage_number(uid, amount [, type]) -> boolean
	// The floating combat number the game shows on a hit. Lets a mod's custom damage read
	// like real damage instead of being invisible.
	int lua_sam_damage_number(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int amount = (int)luaL_checkinteger(Ls, 2);
		const int gibType = (int)luaL_optinteger(Ls, 3, 0);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_damage_number refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveEntityQuiet((long long)uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		spawnDamageGib(e, amount, gibType, 0, true);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)amount; (void)gibType; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	int lua_sam_get_time_played(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushinteger(Ls, (lua_Integer)ticks);
		return 1;
	}
#endif // SAM_LUA_HAVE_BARONY

	// ---- persistent per-mod data (Part 3) --------------------------------------
	// JSON under <savegames>/sam_mod_data/<namespace>/<key>.json. Namespace comes
	// from the currently-executing script (g_currentNs).

	// A mod-data key becomes a FILENAME, so the characters a filesystem refuses have to go.
	// This used to map '/', '\\', ':' and '.' all onto '_', which broke two ways: the key could
	// not be listed back (sam_list_data_keys returned "quest_main" for "quest.main", and the
	// script's own comparison never matched), and the mapping was many-to-one, so "a.b" and
	// "a_b" shared one file and the second silently destroyed the first.
	//
	// Percent-encoding is one-to-one and decodable, so the key survives the round trip and two
	// different keys can never collide. Anything outside [A-Za-z0-9_-] is escaped, '%' included.
	std::string samEncodeKey(const std::string& s)
	{
		static const char* kHex = "0123456789ABCDEF";
		std::string o;
		for ( unsigned char c : s )
		{
			const bool plain = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' )
				|| ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
			if ( plain ) { o += (char)c; }
			else { o += '%'; o += kHex[(c >> 4) & 0xF]; o += kHex[c & 0xF]; }
		}
		return o.empty() ? std::string("_") : o;
	}

	// The inverse. A filename with no '%' is either a plain key or one written by an older
	// build, and comes back unchanged -- which is exactly what that build would have returned.
	std::string samDecodeKey(const std::string& s)
	{
		std::string o;
		for ( size_t i = 0; i < s.size(); ++i )
		{
			if ( s[i] == '%' && i + 2 < s.size() )
			{
				auto hex = [](char c) -> int {
					if ( c >= '0' && c <= '9' ) { return c - '0'; }
					if ( c >= 'A' && c <= 'F' ) { return c - 'A' + 10; }
					if ( c >= 'a' && c <= 'f' ) { return c - 'a' + 10; }
					return -1;
				};
				const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
				if ( hi >= 0 && lo >= 0 ) { o += (char)((hi << 4) | lo); i += 2; continue; }
			}
			o += s[i];
		}
		return o;
	}

	std::string samSanitize(const std::string& s)
	{
		std::string o;
		for ( char c : s ) { o += ( c == '/' || c == '\\' || c == ':' || c == '.' ) ? '_' : c; }
		return o.empty() ? std::string("_") : o;
	}

	// Directory half of samModDataFile, so sam_list_data_keys can enumerate it.
	std::string samModDataDir(const std::string& ns)
	{
#ifdef SAM_LUA_HAVE_BARONY
		const std::string base = std::string(outputdir) + "/savegames/sam_mod_data";
#else
		const std::string base = "./sam_mod_data";
#endif
		return base + "/" + samSanitize(ns);
	}

	// A saved key's FILENAME changed in this batch, and the files already on disk did not.
	//
	// The old scheme rewrote only / \ : and . to '_' and passed everything else through; the new
	// one percent-escapes every byte outside [A-Za-z0-9_-] so the mapping is REVERSIBLE and
	// sam_list_data_keys can show the key a mod actually used. That codec is right. What was
	// missing is that a mod shipped on 2.6.1 wrote "high.score" to high_score.json, and after the
	// change it would look under high%2Escore.json, find nothing, and be told nil -- which is also
	// exactly what "never saved" looks like, so it would quietly start over with the player's
	// progress gone and nothing in any log.
	//
	// So the legacy file is RENAMED into place the first time the key is touched. A rename rather
	// than a copy means it happens once and sam_list_data_keys stops reporting the orphan under a
	// mangled name.
	//
	// The old scheme was many-to-one -- "high.score" and "high_score" shared one file -- so two
	// keys can compete for the same legacy file and whichever is touched first claims it. That
	// ambiguity is pre-existing and is the very reason the codec was replaced; the distinction was
	// never written to disk, so nothing here can recover it.
	void samMigrateLegacyDataFile(const std::string& dir, const std::string& key)
	{
		const std::string legacyStem = samSanitize(key);
		const std::string modernStem = samEncodeKey(key);
		if ( legacyStem == modernStem ) { return; }   // this key's filename did not change
		std::error_code ec;
		const std::filesystem::path modern = std::filesystem::path(dir) / (modernStem + ".json");
		if ( std::filesystem::exists(modern, ec) ) { return; }
		const std::filesystem::path legacy = std::filesystem::path(dir) / (legacyStem + ".json");
		if ( !std::filesystem::exists(legacy, ec) ) { return; }
		std::filesystem::rename(legacy, modern, ec);
		if ( !ec )
		{
			SAM_INFO("SAM", "Moved saved data for key '" + key + "' onto the new file name.");
		}
	}

	std::string samModDataFile(const std::string& ns, const std::string& key)
	{
#ifdef SAM_LUA_HAVE_BARONY
		const std::string base = std::string(outputdir) + "/savegames/sam_mod_data";
#else
		const std::string base = "./sam_mod_data";
#endif
		const std::string dir = base + "/" + samSanitize(ns);
		samMigrateLegacyDataFile(dir, key);   // rescue anything an older build wrote under this key
		return dir + "/" + samEncodeKey(key) + ".json";
	}

	// Parity with the JS twin's samJsStringifyForStore. JSON.stringify of a function is
	// undefined, so JS refuses to write at all rather than replace a good saved value with
	// nothing. luaToJson below turns every type it does not recognise into null, so the same
	// mistake wrote that null over the stored value and still answered true: passing the
	// function instead of calling it destroyed a mod's save in Lua and was caught in JS.
	// Checked at the top level only, which is where the whole value is at stake.
	bool samLuaNoJsonForm(lua_State* Ls, int idx, const char* what)
	{
		switch ( lua_type(Ls, idx) )
		{
			case LUA_TFUNCTION:
			case LUA_TUSERDATA:
			case LUA_TLIGHTUSERDATA:
			case LUA_TTHREAD:
				SAM_WARN("LUA", std::string(what) + ": value has no JSON form (a function or userdata)"
					" - nothing written.");
				return true;
			default:
				return false;
		}
	}

	nlohmann::json luaToJson(lua_State* Ls, int idx, int depth)
	{
		// Depth cap + lua_checkstack: on the object path each level keeps a key on
		// the Lua stack while recursing into its value, so a deep table can exceed
		// the LUA_MINSTACK slots guaranteed to a C function. Grow the stack with the
		// non-throwing form (luaL_checkstack would longjmp past C++ destructors) so
		// the lua_next/lua_rawgeti/lua_pushnil pushes never write past the stack.
		if ( depth > 32 || !lua_checkstack(Ls, 6) ) { return nullptr; }
		idx = lua_absindex(Ls, idx);
		switch ( lua_type(Ls, idx) )
		{
			case LUA_TBOOLEAN: return (bool)lua_toboolean(Ls, idx);
			case LUA_TNUMBER:
				if ( lua_isinteger(Ls, idx) ) { return (long long)lua_tointeger(Ls, idx); }
				return (double)lua_tonumber(Ls, idx);
			case LUA_TSTRING: return std::string(lua_tostring(Ls, idx));
			case LUA_TTABLE:
			{
				if ( lua_rawlen(Ls, idx) > 0 ) // sequence -> JSON array
				{
					nlohmann::json arr = nlohmann::json::array();
					const int n = (int)lua_rawlen(Ls, idx);
					for ( int i = 1; i <= n; ++i )
					{
						lua_rawgeti(Ls, idx, i);
						arr.push_back(luaToJson(Ls, -1, depth + 1));
						lua_pop(Ls, 1);
					}
					return arr;
				}
				nlohmann::json obj = nlohmann::json::object(); // map -> JSON object
				lua_pushnil(Ls);
				while ( lua_next(Ls, idx) != 0 )
				{
					std::string k;
					if ( lua_type(Ls, -2) == LUA_TSTRING ) { k = lua_tostring(Ls, -2); }
					else if ( lua_type(Ls, -2) == LUA_TNUMBER ) { k = std::to_string((long long)lua_tointeger(Ls, -2)); }
					if ( !k.empty() ) { obj[k] = luaToJson(Ls, -1, depth + 1); }
					lua_pop(Ls, 1);
				}
				return obj;
			}
			default: return nullptr;
		}
	}

	// Max JSON nesting we will parse/marshal. Guards both nlohmann's recursive
	// parser and jsonToLua below against C-stack overflow from crafted/corrupt
	// on-disk mod data. luaToJson (writer) caps at 32, so anything SAM produces
	// stays well under this; deeper hand-authored data is rejected as corrupt.
	static const int SAM_JSON_MAX_DEPTH = 64;

	// Reject text that nests deeper than `limit` BEFORE handing it to
	// nlohmann::json::parse (whose recursive descent has no depth limit and can
	// blow the native stack). String contents are skipped so brackets inside
	// strings don't inflate the count. O(n) and short-circuits on the first
	// over-limit bracket.
	bool jsonDepthWithinLimit(const std::string& text, int limit)
	{
		int depth = 0;
		bool inStr = false, esc = false;
		for ( char c : text )
		{
			if ( inStr )
			{
				if ( esc )            { esc = false; }
				else if ( c == '\\' ) { esc = true; }
				else if ( c == '"' )  { inStr = false; }
				continue;
			}
			if ( c == '"' )                  { inStr = true; }
			else if ( c == '[' || c == '{' ) { if ( ++depth > limit ) { return false; } }
			else if ( c == ']' || c == '}' ) { if ( depth > 0 ) { --depth; } }
		}
		return true;
	}

	void jsonToLua(lua_State* Ls, const nlohmann::json& j, int depth)
	{
		// Depth cap bounds native recursion; lua_checkstack grows the Lua value
		// stack so lua_createtable/push never write past it (a C function is only
		// guaranteed LUA_MINSTACK slots). Use lua_checkstack, NOT luaL_checkstack:
		// the latter raises a Lua error (longjmp) that would skip the C++
		// destructors of the nlohmann iterators in scope here.
		if ( depth > SAM_JSON_MAX_DEPTH || !lua_checkstack(Ls, 4) ) { lua_pushnil(Ls); return; }
		if ( j.is_boolean() )                                    { lua_pushboolean(Ls, j.get<bool>() ? 1 : 0); }
		else if ( j.is_number_integer() || j.is_number_unsigned() ) { lua_pushinteger(Ls, (lua_Integer)j.get<long long>()); }
		else if ( j.is_number() )                               { lua_pushnumber(Ls, j.get<double>()); }
		else if ( j.is_string() )                               { lua_pushstring(Ls, j.get<std::string>().c_str()); }
		else if ( j.is_array() )
		{
			lua_createtable(Ls, (int)j.size(), 0);
			int i = 1;
			for ( const auto& el : j ) { jsonToLua(Ls, el, depth + 1); lua_rawseti(Ls, -2, i++); }
		}
		else if ( j.is_object() )
		{
			lua_createtable(Ls, 0, (int)j.size());
			for ( auto it = j.begin(); it != j.end(); ++it ) { jsonToLua(Ls, it.value(), depth + 1); lua_setfield(Ls, -2, it.key().c_str()); }
		}
		else { lua_pushnil(Ls); }
	}

	// sam_save_data(key, value) — persist a value for the calling mod.
	// sam_list_data_keys() -> array of this mod's saved key names. Lets a mod enumerate
	// what it has stored instead of having to remember every key it ever wrote.
	int lua_sam_list_data_keys(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_newtable(Ls);
		const std::string dir = samModDataDir(g_currentNs);
		int idx = 1;
		std::error_code ec;
		if ( !std::filesystem::exists(dir, ec) || ec ) { return 1; }
		std::filesystem::directory_iterator it(dir, ec);
		if ( ec ) { return 1; }
		for ( const auto& entry : it )
		{
			std::error_code ec2;
			if ( !entry.is_regular_file(ec2) || ec2 ) { continue; }
			std::string fn = entry.path().filename().string();
			if ( fn.size() <= 5 || fn.substr(fn.size() - 5) != ".json" ) { continue; }
			fn.erase(fn.size() - 5);
			// Decoded, so this returns the key sam_save_data was CALLED with. It used to return
			// the mangled filename, which is why `k == "quest.main"` never matched.
			const std::string decoded = samDecodeKey(fn);
			lua_pushstring(Ls, decoded.c_str());
			lua_rawseti(Ls, -2, idx++);
		}
		return 1;
	}

	int lua_sam_save_data(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* keyC = luaL_checkstring(Ls, 1);
		const std::string key = keyC ? keyC : "";
		if ( g_currentNs.empty() ) { SAM_WARN("LUA", "sam_save_data: no owning mod namespace — ignored."); lua_pushboolean(Ls, 0); return 1; }
		if ( samLuaNoJsonForm(Ls, 2, "sam_save_data") ) { lua_pushboolean(Ls, 0); return 1; }
		nlohmann::json j = luaToJson(Ls, 2, 0);
		const std::string path = samModDataFile(g_currentNs, key);
		try
		{
			// 'replace' handler: non-UTF-8 Lua-string bytes -> U+FFFD instead of a
			// thrown type_error (the try/catch already prevents a crash, but this
			// persists the data instead of dropping the whole save).
			const std::string text = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
			// Temp file + rename: a crash mid-write keeps the previous value on disk
			// instead of leaving a truncated file that will not parse next launch.
			if ( !SAMErrors::writeFileAtomic(path, text) ) { SAM_ERROR("LUA", "sam_save_data: cannot write " + path); lua_pushboolean(Ls, 0); return 1; }
		}
		catch ( ... ) { SAM_ERROR("LUA", "sam_save_data: failed writing key '" + key + "'."); lua_pushboolean(Ls, 0); return 1; }
		SAM_INFO("SAM", "Saved data key '" + key + "' for [" + g_currentNs + "]");
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_load_data(key) -> value or nil.
	int lua_sam_load_data(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* keyC = luaL_checkstring(Ls, 1);
		const std::string key = keyC ? keyC : "";
		if ( g_currentNs.empty() ) { lua_pushnil(Ls); return 1; }
		std::ifstream f(samModDataFile(g_currentNs, key), std::ios::binary);
		if ( !f.is_open() ) { lua_pushnil(Ls); return 1; }
		const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if ( !jsonDepthWithinLimit(text, SAM_JSON_MAX_DEPTH) )
		{
			SAM_WARN("LUA", "sam_load_data: data for key '" + key + "' nests too deep — nil.");
			lua_pushnil(Ls); return 1;
		}
		nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
		if ( j.is_discarded() ) { SAM_WARN("LUA", "sam_load_data: corrupt data for key '" + key + "' — nil."); lua_pushnil(Ls); return 1; }
		jsonToLua(Ls, j, 0);
		SAM_INFO("SAM", "Loaded data key '" + key + "' for [" + g_currentNs + "]");
		return 1;
	}

	// sam_delete_data(key).
	int lua_sam_delete_data(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* keyC = luaL_checkstring(Ls, 1);
		const std::string key = keyC ? keyC : "";
		if ( g_currentNs.empty() ) { lua_pushboolean(Ls, 0); return 1; }
		std::error_code ec;
		const bool removed = std::filesystem::remove(std::filesystem::path(samModDataFile(g_currentNs, key)), ec);
		SAM_INFO("SAM", "Deleted data key '" + key + "' for [" + g_currentNs + "]" + (removed ? "" : " (was absent)"));
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// ---- timers (Part 4) -------------------------------------------------------

	void samRemoveTimer(const std::string& ns, const std::string& id)
	{
		for ( size_t i = 0; i < g_timers.size(); ++i )
		{
			if ( g_timers[i].ns == ns && g_timers[i].id == id )
			{
				if ( L && g_timers[i].callbackRef != LUA_NOREF ) { luaL_unref(L, LUA_REGISTRYINDEX, g_timers[i].callbackRef); }
				g_timers.erase(g_timers.begin() + i);
				return;
			}
		}
	}

	int samSetTimerImpl(lua_State* Ls, bool repeating)
	{
		const char* idC = luaL_checkstring(Ls, 1);
		const long long ticks = (long long)luaL_checkinteger(Ls, 2);
		luaL_checktype(Ls, 3, LUA_TFUNCTION);
		const std::string id = idC ? idC : "";
		samRemoveTimer(g_currentNs, id);          // replace an existing timer with the same id
		lua_pushvalue(Ls, 3);                      // dup the callback to the top
		const int ref = luaL_ref(Ls, LUA_REGISTRYINDEX); // pops it, stores a registry ref
		Timer t;
		t.id = id; t.ns = g_currentNs; t.callbackRef = ref;
		// 1..INT32_MAX, the same bounds as the JS twin.
		const long long clamped = ticks < 1 ? 1 : ( ticks > 2147483647LL ? 2147483647LL : ticks );
		t.remaining = clamped;
		t.interval  = repeating ? clamped : 0;
		t.repeating = repeating;
		g_timers.push_back(t);
		SAM_INFO("SAM", std::string("Timer '") + id + "' set for " + std::to_string(ticks) + " ticks" + (repeating ? " (repeating)" : ""));
		return 0;
	}

	int lua_sam_set_timer(lua_State* Ls)           { return samSetTimerImpl(Ls, false); }
	int lua_sam_set_repeating_timer(lua_State* Ls) { return samSetTimerImpl(Ls, true); }

	int lua_sam_cancel_timer(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* idC = luaL_checkstring(Ls, 1);
		samRemoveTimer(g_currentNs, idC ? idC : "");
		return 0;
	}

	// ---- custom hooks (Part 2) -------------------------------------------------

	int lua_sam_register_hook(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* nameC = luaL_checkstring(Ls, 1);
		const std::string name = nameC ? nameC : "";
		if ( name.find(':') == std::string::npos )
		{
			SAM_WARN("LUA", "sam_register_hook: name '" + name + "' must be namespaced (\"namespace:hook_name\").");
			return 0;
		}
		g_customHooks.push_back(name);
		SAM_INFO("LUA", "Registered custom hook: " + name);
		return 0;
	}

	// sam_fire_hook("ns:name", event_table) — dispatch a custom event to ALL Lua + JS
	// scripts (cross-runtime), host-authoritative. Only primitive fields cross over.
	int lua_sam_fire_hook(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* nameC = luaL_checkstring(Ls, 1);
		const std::string name = nameC ? nameC : "";
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_fire_hook refused: host only."); return 0; }
#endif
		if ( g_fireDepth >= 8 ) { SAM_WARN("LUA", "sam_fire_hook: recursion too deep — '" + name + "' not fired."); return 0; }

		SAMLua::Event ev;   ev.setName(name);
		SAMJs::Event  jsev; jsev.setName(name);
		if ( lua_type(Ls, 2) == LUA_TTABLE )
		{
			lua_pushnil(Ls);
			while ( lua_next(Ls, 2) != 0 )
			{
				if ( lua_type(Ls, -2) == LUA_TSTRING )
				{
					const std::string k = lua_tostring(Ls, -2);
					const int vt = lua_type(Ls, -1);
					if ( vt == LUA_TNUMBER )       { const long long n = lua_isinteger(Ls, -1) ? (long long)lua_tointeger(Ls, -1) : (long long)lua_tonumber(Ls, -1); ev.i(k, n); jsev.i(k, n); }
					else if ( vt == LUA_TBOOLEAN ) { const long long b = lua_toboolean(Ls, -1) ? 1 : 0; ev.i(k, b); jsev.i(k, b); }
					else if ( vt == LUA_TSTRING )  { const std::string v = lua_tostring(Ls, -1); ev.s(k, v); jsev.s(k, v); }
				}
				lua_pop(Ls, 1);
			}
		}

		++g_fireDepth;
		const std::string savedNs = g_currentNs;
		// Lua FIRST, then JS, as two statements: the order every engine site and the JS twin use.
		// SAMLua::dispatchEvent clears and re-seeds the shared write-back store from THIS hook's
		// payload and the JS pass reads the live store, so JS-first ran JS listeners against the
		// enclosing event's store and lost their edits. `+` does not sequence its operands.
		int n = SAMLua::dispatchEvent(ev);
		n += SAMJs::dispatchEvent(jsev);
		g_currentNs = savedNs; // the nested dispatch cleared g_currentNs; restore the firer's
		--g_fireDepth;
		SAM_INFO("SAM", "Fired custom hook: " + name + " to " + std::to_string(n) + " script(s)");
		lua_pushinteger(Ls, (lua_Integer)n); // return the count of scripts reached
		return 1;
	}

	// v0.7.0 Feature 2: sam_modify_damage(player, new_value) — rewrite the incoming
	// damage from inside an on_before_damage callback (clamped to >= 0 by the latch).
	int lua_sam_modify_damage(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const long long v = (long long)luaL_checkinteger(Ls, 2);
		if ( !SAMLua::beforeDamageActive() )
		{
			SAM_WARN("LUA", "sam_modify_damage: only valid inside an on_before_damage callback — ignored.");
			return 0;
		}
		SAMLua::beforeDamageModify(player, v);
		return 0;
	}

	// sam_modify_monster_damage(newValue) — rewrite the damage a MONSTER is about to take.
	// Only valid inside an on_before_monster_damage callback. No subject argument: only one
	// monster is ever mid-dispatch, so the latch needs no key.
	int lua_sam_modify_monster_damage(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long v = (long long)luaL_checkinteger(Ls, 1);
		if ( !SAMLua::beforeMonsterDamageActive() )
		{
			SAM_WARN("LUA", "sam_modify_monster_damage: only valid inside an on_before_monster_damage callback — ignored.");
			return 0;
		}
		SAMLua::beforeMonsterDamageModify(v);
		return 0;
	}

	// sam_modify_value(newValue) — rewrite the number the engine is about to use, inside any
	// hook that offers one (today player.on_xp_gained; player.on_gold_collected opens NO value
	// latch, so gold cannot be rewritten this way). The
	// error names the hook you ARE inside, so a wrong-place call says something useful.
	int lua_sam_modify_value(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long v = (long long)luaL_checkinteger(Ls, 1);
		if ( !SAMLua::hookValueActive() )
		{
			SAM_WARN("LUA", "sam_modify_value: no hook is offering a value to rewrite right now — ignored. "
				"It works inside player.on_xp_gained; for damage use sam_modify_damage (player) "
				"or sam_modify_monster_damage (monster).");
			return 0;
		}
		SAMLua::hookValueModify(v);
		return 0;
	}

	// v0.7.0 Feature 2: sam_deal_damage(entity_uid, amount) — deal `amount` damage to
	// any entity by UID (host-only, UID-only, existence-validated). Positive = damage.
	int lua_sam_deal_damage(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int amount = (int)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		// The writer's resolver: this deals real damage, and it was still resolving a shared
		// sentinel uid to whichever gib the map happened to be holding under it.
		Entity* e = samResolveWritable(uid, "sam_deal_damage");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		const int dmg = ( amount < 0 ) ? amount : -amount; // positive request => negative modHP
		e->modHP(dmg);
		SAM_INFO("SAM", "sam_deal_damage: " + std::to_string(-dmg) + " damage to uid " + std::to_string(uid));
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)amount;
		lua_pushboolean(Ls, 0);
		return 1;
#endif
	}

	// v0.7.0 Feature 3: sam_is_key_held(key_name [, player]) -> boolean. On the host, a player on
	// another machine reads the keys their game reported (A-Z, 0-9, F1-F12; SAMMpInput::keyHeld).
	int lua_sam_is_key_held(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* nameC = luaL_checkstring(Ls, 1);
		// The optional player (the contract has already resolved and checked it). Absent or nil:
		// the player the current event is about, else this machine's own.
		const int player = lua_isnoneornil(Ls, 2) ? -1 : (int)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, SAMMpInput::keyHeld(nameC ? nameC : "", player) ? 1 : 0);
#else
		(void)player;
		lua_pushboolean(Ls, SAMLua::isKeyHeld(nameC ? nameC : "") ? 1 : 0);
#endif
		return 1;
	}

	// ---- v0.7.0 Feature 4: monster / NPC scripting (UID-based) -----------------
#ifdef SAM_LUA_HAVE_BARONY
	// Resolve a UID to a monster Entity* (behavior==actMonster + has stats), else nullptr.
	Entity* samResolveMonster(long long uid)
	{
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e || e->behavior != &actMonster || !e->getStats() ) { return nullptr; }
		return e;
	}

	// Map a monster-type name (case-insensitive) to its Monster enum, or -1. The engine's
	// monstertypename[] entries are all lowercase, so lowercase the input first.
	int samMonsterNameToId(const char* nameIn)
	{
		std::string want = nameIn ? nameIn : "";
		for ( char& c : want ) { c = (char)std::tolower((unsigned char)c); }
		for ( int i = 0; i < NUMMONSTERS; ++i ) { if ( want == monstertypename[i] ) { return i; } }
		return -1;
	}

	// ---- v2 world-ops: position / teleport / spawn / inventory -----------------
	// All positions are MAP TILE coordinates (integers), matching sam_spawn_item and
	// how the engine's teleport() reads coords. Tile centre in pixels = tile*16 + 8.

	// sam_get_player_uid(player) -> entity uid | nil. Bridges player index -> a uid so
	// the uid-based world-ops below (get/set position) can act on a player's body.
	int lua_sam_get_player_uid(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)players[player]->entity->getUID());
		return 1;
	}


	// ============================================================================
	// v2.6 batch 1: reads and dice.
	//
	// Everything below is either a read of a field the engine already replicates to every
	// machine, or a helper built on the deterministic sam_random stream. That is why the
	// batch is safe: no new packet, no save-format change, and nothing that needs to decide
	// host authority. The three exceptions say so in their own comment.
	//
	// DISTANCES ARE IN TILES. The engine works in world pixels (16 per tile) and entityDist
	// returns pixels, but every other spatial call in this API speaks tiles, so these divide.
	// Mixing the two units silently is a worse trap than the conversion.
	// ============================================================================

	// sam_get_position_precise(uid) -> x, y, z (world pixels, fractional) | nil
	// sam_get_position truncates to a tile and throws away z entirely, so nothing in script
	// could tell a flying bat from a rat underneath it, or two monsters sharing a tile.
	int lua_sam_get_position_precise(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		Entity* e = samResolveEntityQuiet((long long)uid);
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushnumber(Ls, (lua_Number)e->x);
		lua_pushnumber(Ls, (lua_Number)e->y);
		lua_pushnumber(Ls, (lua_Number)e->z);
		return 3;
	}

	// sam_get_distance(uidA, uidB) -> tiles | nil
	int lua_sam_get_distance(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Entity* a = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 1));
		Entity* b = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 2));
		if ( !a || !b ) { lua_pushnil(Ls); return 1; }
		lua_pushnumber(Ls, (lua_Number)(entityDist(a, b) / 16.0));
		return 1;
	}

	// sam_get_distance_to(uid, tileX, tileY) -> tiles | nil
	// Measured to the CENTRE of the tile, which is where the engine puts things.
	int lua_sam_get_distance_to(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Entity* e = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 1));
		const int tx = (int)luaL_checkinteger(Ls, 2);
		const int ty = (int)luaL_checkinteger(Ls, 3);
		if ( !e ) { lua_pushnil(Ls); return 1; }
		const double dx = e->x - ((double)tx * 16.0 + 8.0);
		const double dy = e->y - ((double)ty * 16.0 + 8.0);
		lua_pushnumber(Ls, (lua_Number)(std::sqrt(dx * dx + dy * dy) / 16.0));
		return 1;
	}

	// sam_get_entity_type(uid) -> "player"|"monster"|"item"|"door"|... | nil
	// The inverse of the behavior-pointer filter sam_find_entities already applies.
	int lua_sam_get_entity_type(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Entity* e = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 1));
		if ( !e ) { lua_pushnil(Ls); return 1; }
		const char* kind = "other";
		if ( e->behavior == &actPlayer )            { kind = "player"; }
		else if ( e->behavior == &actMonster )      { kind = "monster"; }
		else if ( e->behavior == &actItem )         { kind = "item"; }
		else if ( e->behavior == &actDoor )         { kind = "door"; }
		else if ( e->behavior == &actChest )        { kind = "chest"; }
		else if ( e->behavior == &actLadder )       { kind = "ladder"; }
		else if ( e->behavior == &actPortal )       { kind = "portal"; }
		else if ( e->behavior == &actGate )         { kind = "gate"; }
		else if ( e->behavior == &actSwitch )       { kind = "switch"; }
		else if ( e->behavior == &actFountain )     { kind = "fountain"; }
		else if ( e->behavior == &actSink )         { kind = "sink"; }
		else if ( e->behavior == &actBoulder )      { kind = "boulder"; }
		else if ( e->behavior == &actGib )          { kind = "gib"; }
		// "gold" was accepted by sam_find_entities and produced by nothing, so a uid it had just
		// handed back described itself as "other".
		else if ( e->behavior == &actGoldBag )      { kind = "gold"; }
		lua_pushstring(Ls, kind);
		return 1;
	}

	// sam_get_scale(uid) -> x, y, z | nil. Reader for the existing sam_set_scale.
	int lua_sam_get_scale(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Entity* e = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 1));
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushnumber(Ls, (lua_Number)e->scalex);
		lua_pushnumber(Ls, (lua_Number)e->scaley);
		lua_pushnumber(Ls, (lua_Number)e->scalez);
		return 3;
	}

	// sam_is_visible(uid) -> boolean | nil. Reader for the existing sam_set_visible.
	int lua_sam_is_visible(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Entity* e = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 1));
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushboolean(Ls, e->flags[INVISIBLE] ? 0 : 1);
		return 1;
	}

	// sam_get_velocity(uid) -> vx, vy, vz (pixels per tick) | nil
	int lua_sam_get_velocity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Entity* e = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 1));
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushnumber(Ls, (lua_Number)e->vel_x);
		lua_pushnumber(Ls, (lua_Number)e->vel_y);
		lua_pushnumber(Ls, (lua_Number)e->vel_z);
		return 3;
	}

	// sam_get_entity_size(uid) -> sizex, sizey (bounding box, pixels) | nil
	// Any script-side overlap or aim-cone maths needs this and it was invisible.
	int lua_sam_get_entity_size(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Entity* e = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 1));
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)e->sizex);
		lua_pushinteger(Ls, (lua_Integer)e->sizey);
		return 2;
	}

	// sam_get_entity_sprite(uid) -> model index | nil
	int lua_sam_get_entity_sprite(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Entity* e = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 1));
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)e->sprite);
		return 1;
	}

	// sam_get_entity_ticks(uid) -> frames this entity has existed | nil
	int lua_sam_get_entity_ticks(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Entity* e = samResolveEntityQuiet((long long)luaL_checkinteger(Ls, 1));
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)e->ticks);
		return 1;
	}

	// ---- world reads -----------------------------------------------------------

	// sam_get_map_seed() -> the seed THIS FLOOR was generated from.
	// Distinct from sam_get_seed, which is the whole run. Identical on host and clients,
	// because a client regenerates the floor from it, so it is safe to derive shared
	// per-floor randomness from.
	int lua_sam_get_map_seed(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushinteger(Ls, (lua_Integer)(unsigned long long)mapseed);
		return 1;
	}

	// sam_is_dark_level() -> boolean
	int lua_sam_is_dark_level(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushboolean(Ls, darkmap ? 1 : 0);
		return 1;
	}

	// sam_get_playable_bounds() -> x1, y1, x2, y2 (tiles, half-open)
	// The interior the generator will actually use, which excludes the perimeter gap. A
	// spawner that ignores this puts things inside the outer wall.
	int lua_sam_get_playable_bounds(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushinteger(Ls, (lua_Integer)getMapPossibleLocationX1());
		lua_pushinteger(Ls, (lua_Integer)getMapPossibleLocationY1());
		lua_pushinteger(Ls, (lua_Integer)getMapPossibleLocationX2());
		lua_pushinteger(Ls, (lua_Integer)getMapPossibleLocationY2());
		return 4;
	}

	// sam_is_tile_diggable(x, y) -> boolean. The engine's own test, so a mod's mining
	// mechanic refuses exactly where the game refuses.
	int lua_sam_is_tile_diggable(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int x = (int)luaL_checkinteger(Ls, 1);
		const int y = (int)luaL_checkinteger(Ls, 2);
		// mapTileDiggable does NO validation of its own: it indexes map.tiles immediately,
		// because every engine caller hands it a raycast hit that is in bounds by
		// construction. A script hands it whatever the modder typed, so the guard has to be
		// here. map.tiles is also null until a level is loaded, which a menu timer can hit.
		if ( !map.tiles || x < 0 || x >= (int)map.width || y < 0 || y >= (int)map.height )
		{
			lua_pushboolean(Ls, 0);
			return 1;
		}
		lua_pushboolean(Ls, mapTileDiggable(x, y) ? 1 : 0);
		return 1;
	}

	// sam_get_map_flags() -> table of the per-map rules a mod should respect
	int lua_sam_get_map_flags(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		// MFLAG_* are NOT indices. Each is a macro that already extracts its byte out of
		// map.flags, e.g. main.hpp:536 is ((map.flags[MAP_FLAG_GENBYTES3] >> 24) & 0xFF).
		// The first version of this used them to index map.flags a second time, so every
		// field read map.flags[0] and every answer was wrong. sam_world.cpp:446 had the
		// correct form all along: compare the macro against zero.
		lua_newtable(Ls);
		const struct { const char* name; int value; } flags[] = {
			{ "no_digging",     MFLAG_DISABLEDIGGING },
			{ "no_teleport",    MFLAG_DISABLETELEPORT },
			{ "no_levitation",  MFLAG_DISABLELEVITATION },
			{ "no_opening",     MFLAG_DISABLEOPENING },
			{ "no_messages",    MFLAG_DISABLEMESSAGES },
			{ "no_hunger",      MFLAG_DISABLEHUNGER },
			{ "gen_adjacent",   MFLAG_GENADJACENTROOMS },
		};
		for ( const auto& f : flags )
		{
			lua_pushboolean(Ls, f.value != 0 ? 1 : 0);
			lua_setfield(Ls, -2, f.name);
		}
		// A count of tiles, not a yes/no, so it is reported as the number it is.
		lua_pushinteger(Ls, (lua_Integer)MFLAG_PERIMETER_GAP);
		lua_setfield(Ls, -2, "perimeter_gap");
		return 1;
	}

	// sam_get_exit_position() -> tileX, tileY | nil. The ladder or portal off this floor,
	// found the same way the game's own /dowse command finds it.
	int lua_sam_get_exit_position(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		// No floor loaded (the main menu, between levels): no list to walk, so no exit.
		if ( !map.entities ) { lua_pushnil(Ls); return 1; }
		for ( node_t* node = map.entities->first; node; node = node->next )
		{
			Entity* e = (Entity*)node->element;
			if ( !e ) { continue; }
			// Mirror the engine's own exit finder (drawminimap.cpp:1348-1359) rather than
			// matching any ladder or portal. skill[3] == 1 is LADDER_SECRET, and is also
			// what Mages Guild's purely decorative portal sets; portalNotSecret marks a
			// real Hell exit. skill[19] is the framework's own decorative sam_spawn_portal,
			// which on a client is prepended to the list and would otherwise win.
			bool isExit = false;
			if ( e->behavior == &actLadder )            { isExit = ( e->skill[3] != 1 ); }
			else if ( e->behavior == &actPortal )       { isExit = ( e->skill[19] != 1 ) && ( e->portalNotSecret == 1 ); }
			else if ( e->behavior == &actCustomPortal ) { isExit = true; }  // does set loadnextlevel
			if ( isExit )
			{
				lua_pushinteger(Ls, (lua_Integer)((int)e->x >> 4));
				lua_pushinteger(Ls, (lua_Integer)((int)e->y >> 4));
				return 2;
			}
		}
		lua_pushnil(Ls);
		return 1;
	}

	// ---- time and run state ----------------------------------------------------

	// sam_get_run_time() -> seconds of ACTUAL PLAY this run.
	// completionTime is the number the game itself shows, and it stops while paused, in the
	// intro and while dead. sam_get_time_played returns the global `ticks` counter, which
	// keeps running in menus and resets on relaunch, so it is not this.
	int lua_sam_get_run_time(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushnumber(Ls, (lua_Number)completionTime / (lua_Number)TICKS_PER_SECOND);
		return 1;
	}

	// sam_get_tick_rate() -> 50. Barony's logic step is fixed, so there is no delta time to
	// expose; this is the constant every "per second" conversion needs.
	int lua_sam_get_tick_rate(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushinteger(Ls, (lua_Integer)TICKS_PER_SECOND);
		return 1;
	}

	// sam_get_fps() -> this machine's render rate. LOCAL and per-machine: never feed it into
	// a gameplay roll or two players desync.
	int lua_sam_get_fps(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushnumber(Ls, (lua_Number)fps);
		return 1;
	}

	// sam_get_real_time() -> unix seconds. Per-machine wall clock. Same warning as fps: two
	// players' clocks differ, so this must not decide anything shared or saved.
	int lua_sam_get_real_time(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushinteger(Ls, (lua_Integer)(long long)getTime());
		return 1;
	}

	// sam_get_date() -> { year, month, day, hour, min, sec }. Enables seasonal content.
	// Per-machine, same warning as above.
	int lua_sam_get_date(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
		getTimeAndDate(getTime(), &y, &mo, &d, &h, &mi, &sec);
		lua_newtable(Ls);
		const struct { const char* k; int v; } parts[] = {
			{ "year", y }, { "month", mo }, { "day", d },
			{ "hour", h }, { "min", mi }, { "sec", sec },
		};
		for ( const auto& p : parts )
		{
			lua_pushinteger(Ls, (lua_Integer)p.v);
			lua_setfield(Ls, -2, p.k);
		}
		return 1;
	}

	// sam_is_paused() -> boolean. Per-machine: each client has its own gamePaused.
	int lua_sam_is_paused(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushboolean(Ls, gamePaused ? 1 : 0);
		return 1;
	}

	// sam_is_in_game() -> false while the main menu / intro is up.
	int lua_sam_is_in_game(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushboolean(Ls, intro ? 0 : 1);
		return 1;
	}

	// sam_is_loading() -> true during a level change. A timer callback can fire here, and a
	// script had no way to tell.
	int lua_sam_is_loading(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushboolean(Ls, loading ? 1 : 0);
		return 1;
	}

	// ---- dice ------------------------------------------------------------------
	// All four draw from samRandomDraw, the per-mod deterministic stream behind sam_random,
	// and never from the engine's local_rng/map_rng: those are lockstep streams and drawing
	// from them desyncs multiplayer. Same run seed plus same stream plus same call order
	// gives the same result on every machine.

	// sam_random_float(stream) -> 0.0 .. 1.0
	int lua_sam_random_float(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* streamC = luaL_checkstring(Ls, 1);
		const long long v = samRandomDraw(g_currentNs, streamC ? streamC : "", 0, 1000000);
		lua_pushnumber(Ls, (lua_Number)v / (lua_Number)1000000.0);
		return 1;
	}

	// sam_random_chance(stream, percent) -> boolean. The single most-typed line in any mod.
	// 0 or less is always false and 100 or more always true, so callers need no clamping.
	int lua_sam_random_chance(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* streamC = luaL_checkstring(Ls, 1);
		const double pct = (double)luaL_checknumber(Ls, 2);
		if ( pct <= 0.0 ) { lua_pushboolean(Ls, 0); return 1; }
		if ( pct >= 100.0 ) { lua_pushboolean(Ls, 1); return 1; }
		const long long v = samRandomDraw(g_currentNs, streamC ? streamC : "", 1, 1000000);
		lua_pushboolean(Ls, ((double)v <= pct * 10000.0) ? 1 : 0);
		return 1;
	}

	// sam_random_from_list(stream, table) -> one element | nil for an empty list.
	// Lua indexes from 1; the JS twin indexes from 0. That difference is deliberate and is
	// exactly the parity class that has bitten this project before, so both are written to
	// their own language's convention rather than one being ported literally.
	int lua_sam_random_from_list(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* streamC = luaL_checkstring(Ls, 1);
		luaL_checktype(Ls, 2, LUA_TTABLE);
		const lua_Integer n = (lua_Integer)lua_rawlen(Ls, 2);
		if ( n <= 0 ) { lua_pushnil(Ls); return 1; }
		const long long pick = samRandomDraw(g_currentNs, streamC ? streamC : "", 1, (long long)n);
		lua_rawgeti(Ls, 2, (lua_Integer)pick);
		return 1;
	}

	// sam_random_weighted(stream, { key = weight, ... }) -> key | nil
	// Weights need not sum to anything; a weight of 0 or less can never be drawn.
	int lua_sam_random_weighted(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* streamC = luaL_checkstring(Ls, 1);
		luaL_checktype(Ls, 2, LUA_TTABLE);

		// Collect and SORT the keys before picking. Lua 5.4 randomises its string hash seed
		// per process (lstate.c luai_makeseed), so pairs() order differs between two runs of
		// the same program: five runs of the shipped interpreter over one table gave five
		// different orders. Walking the table directly would make the host and a client pick
		// DIFFERENT keys from the same deterministic draw, which is precisely the desync this
		// function exists to prevent. Sorting also makes Lua and JS agree with each other.
		std::vector<std::pair<std::string, double>> entries;
		double total = 0.0;
		lua_pushnil(Ls);
		while ( lua_next(Ls, 2) != 0 )
		{
			const int keyType = lua_type(Ls, -2);
			if ( keyType == LUA_TSTRING || keyType == LUA_TNUMBER )
			{
				const double w = (double)lua_tonumber(Ls, -1);
				if ( w > 0.0 )
				{
					// A number key by its text, as JS sees {1: 5}. Converted from a COPY: lua_tostring
					// on the key itself would change it in place and break lua_next.
					lua_pushvalue(Ls, -2);
					entries.emplace_back(lua_tostring(Ls, -1), w);
					lua_pop(Ls, 1);
					total += w;
				}
			}
			lua_pop(Ls, 1);
		}
		if ( entries.empty() || total <= 0.0 ) { lua_pushnil(Ls); return 1; }
		std::sort(entries.begin(), entries.end(),
			[](const std::pair<std::string, double>& a, const std::pair<std::string, double>& b)
			{ return a.first < b.first; });

		// Draw over [1, 999999] so target can never equal total exactly: at the top of an
		// inclusive range floating-point residue left every branch untaken and the function
		// returned nil for a perfectly valid table.
		const long long draw = samRandomDraw(g_currentNs, streamC ? streamC : "", 1, 999999);
		double target = total * ((double)draw / 1000000.0);
		for ( const auto& e : entries )
		{
			target -= e.second;
			if ( target <= 0.0 ) { lua_pushstring(Ls, e.first.c_str()); return 1; }
		}
		// Belt and braces: rounding can only ever leave us at the last entry.
		lua_pushstring(Ls, entries.back().first.c_str());
		return 1;
	}

	// ---- persistence parity ----------------------------------------------------

	// sam_has_data(key) -> boolean. Genuinely distinguishes "stored" from "absent": storing
	// nil writes a real file containing null, so sam_has_data is true while sam_load_data is
	// nil. Use sam_delete_data for real absence.
	int lua_sam_has_data(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* keyC = luaL_checkstring(Ls, 1);
		const std::string key = keyC ? keyC : "";
		if ( g_currentNs.empty() || key.empty() ) { lua_pushboolean(Ls, 0); return 1; }
		// MUST go through samModDataFile, which sanitizes. Every writer does, so building the
		// path by hand looked for "boss.phase.json" while sam_save_data had written
		// "boss_phase.json": the check returned false forever and a first-run guard wiped
		// progress on every launch. Raw concatenation also let a key escape the namespace
		// directory.
		const std::string path = samModDataFile(g_currentNs, key);
		std::error_code ec;
		lua_pushboolean(Ls, std::filesystem::exists(path, ec) && !ec ? 1 : 0);
		return 1;
	}

	// sam_world_bytes() / sam_world_bytes_free() -> where a mod stands against the 64 KB
	// savegame budget. Today a mod only learns the ceiling exists when a write returns false
	// mid-run, with nothing to have checked beforehand.
	int lua_sam_world_bytes(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_pushinteger(Ls, (lua_Integer)SAMWorldState::totalBytes());
		return 1;
	}

	int lua_sam_world_bytes_free(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long used = (long long)SAMWorldState::totalBytes();
		const long long cap = (long long)SAMWorldState::kMaxTotalBytes;
		lua_pushinteger(Ls, (lua_Integer)(used >= cap ? 0 : cap - used));
		return 1;
	}


	// ============================================================================
	// v2.6 batch 2: inventory and items.
	//
	// TWO RULES apply to everything here.
	//
	// 1. An item uid names an item on ONE machine (the engine's uidToItem, items.cpp:295,
	//    skips every player who is not local). Every uid therefore goes through
	//    samItemFromUid / samItemForWrite, i.e. SAMMpInventory::resolveItem: this machine's
	//    own players' items, and on the host also the mirror of each remote player's backpack,
	//    which that player's own machine reports (sam_mp_inventory.hpp). A uid that names
	//    nothing here is warned once and answered with a value no real answer uses: nil for
	//    the true/false questions, false for the two whose real answers include nil.
	// 2. The writers are `owner` functions (sam_mp_contracts.inc). On the host they change the
	//    host's own items; a remote player's item is carried by the trampoline to that player's
	//    machine, with the uid it knows the item by, and the body runs THERE. That is why
	//    their host-only guards are SAM_CLIENT_REFUSES, and why each calls
	//    SAMMpInventory::afterOwnerWrite: a worn item's copy on the host, which combat reads,
	//    has to hear about the change.
	//
	// Enums come back as STRINGS, matching every other S.A.M getter: a bare 3 for a status
	// is unreadable in a mod and stops meaning the same thing if the enum ever grows.
	// ============================================================================


	// Item::canUnequip's decision WITHOUT its side effect. The engine version is non-const
	// and sets identified = true then calls onItemIdentified on two paths (items.cpp:6301,
	// :6325), so calling it to answer a question silently identifies the item, and on a
	// client sends an appearance update to the server. The branches below mirror
	// items.cpp:6257-6330 exactly; only the identifying is left out.
	static bool samItemCanUnequipQuiet(const Item* it, const Stat* wielder)
	{
		if ( !it ) { return true; }
		// NOTE: the engine has a "spellbooks always unequippable" branch at items.cpp:6259,
		// and it is INSIDE a /* */ block: "Spellbooks are no longer equipable." It never
		// runs, and its literals are stale anyway (100-103 are rings, not books). It was
		// copied here by mistake and is deliberately absent now.
		if ( it->type == TOOL_DUCK ) { return true; }
		if ( wielder )
		{
			// An automaton is never stuck with anything (items.cpp:6279).
			if ( wielder->type == AUTOMATON ) { return true; }
			// Succubus and friends: a BLESSING is what sticks, not a curse.
			if ( shouldInvertEquipmentBeatitude(wielder) ) { return it->beatitude <= 0; }
		}
		return it->beatitude >= 0;
	}

	// Compile-time proof that every case in the switch below is an ItemType.
	//
	// This is not decoration. The previous version listed TOME_SPELL among the cases, and
	// TOME_SPELL is a CATEGORY whose value is 14, so the guard protected item type 14 (a
	// steel sword) and left every tome wide open: the exact opposite of its purpose. A
	// switch over `int` takes either enum silently. This does not, so it cannot happen again.
	template<typename T> constexpr bool samIsItemType(T) { return std::is_same<T, ItemType>::value; }
	static_assert(samIsItemType(READABLE_BOOK) && samIsItemType(SCROLL_MAIL)
		&& samIsItemType(ENCHANTED_FEATHER) && samIsItemType(MAGICSTAFF_SCEPTER)
		&& samIsItemType(TOOL_PLAYER_LOOT_BAG) && samIsItemType(TOOL_SENTRYBOT)
		&& samIsItemType(TOOL_SPELLBOT) && samIsItemType(TOOL_GYROBOT)
		&& samIsItemType(TOOL_DUMMYBOT),
		"every case in samItemAppearanceIsGameplay must be an ItemType, not a Category");

	// Types whose `appearance` carries gameplay state rather than a look. Writing it on any
	// of these changes what the item IS, and appearance is persisted (scores.hpp:606), so
	// the damage is permanent.
	//
	// DERIVED, not remembered: this list is every decode of `appearance` in the engine that
	// is not the ordinary `% items[type].variations` cosmetic use. The previous version of
	// this function listed TOME_SPELL as a case in a switch over ItemType; TOME_SPELL is a
	// CATEGORY (items.hpp:584, value 14) and ItemType 14 is a sword, so it guarded swords
	// and left every tome open. Tomes are matched by category here, which is what they are.
	static bool samItemAppearanceIsGameplay(int type)
	{
		if ( type < 0 || type >= NUM_ITEM_SLOTS ) { return false; }
		// A tome's appearance picks the spell it teaches (items.cpp:7449 % TOME_APPEARANCE_MAX).
		if ( items[type].category == TOME_SPELL ) { return true; }
		// A spell item's appearance IS the spell the player knows (spell.cpp:1961).
		if ( items[type].category == SPELL_CAT ) { return true; }
		switch ( type )
		{
			case READABLE_BOOK:            // % numbooks picks which book this is
			case SCROLL_MAIL:              // % 25 picks which letter
			case ENCHANTED_FEATHER:        // % ENCHANTED_FEATHER_MAX_DURABILITY: charges left
			case MAGICSTAFF_SCEPTER:       // % MAGICSTAFF_SCEPTER_CHARGE_MAX: charges left
			case TOOL_PLAYER_LOOT_BAG:     // owner in the low bits, contents keyed on the rest
			case TOOL_SENTRYBOT:           // the four bots encode HP (entity.cpp:31653)
			case TOOL_SPELLBOT:
			case TOOL_GYROBOT:
			case TOOL_DUMMYBOT:
				return true;
			default:
				return false;
		}
	}

	// The engine's Status enum (items.hpp:588) as text, both directions.
	static const char* samItemStatusName(int st)
	{
		switch ( st )
		{
			case BROKEN:     return "BROKEN";
			case DECREPIT:   return "DECREPIT";
			case WORN:       return "WORN";
			case SERVICABLE: return "SERVICABLE";
			case EXCELLENT:  return "EXCELLENT";
			default:         return "UNKNOWN";
		}
	}

	static bool samItemStatusFromName(const std::string& n, int& out)
	{
		std::string u;
		for ( char c : n ) { u += (char)toupper((unsigned char)c); }
		if ( u == "BROKEN" )     { out = BROKEN;     return true; }
		if ( u == "DECREPIT" )   { out = DECREPIT;   return true; }
		if ( u == "WORN" )       { out = WORN;       return true; }
		if ( u == "SERVICABLE" || u == "SERVICEABLE" ) { out = SERVICABLE; return true; }
		if ( u == "EXCELLENT" )  { out = EXCELLENT;  return true; }
		return false;
	}

	// items.hpp:637. NO_EQUIP means "cannot be worn", which is different from "unknown".
	static const char* samItemSlotName(int slot)
	{
		switch ( slot )
		{
			case EQUIPPABLE_IN_SLOT_WEAPON:      return "WEAPON";
			case EQUIPPABLE_IN_SLOT_SHIELD:      return "SHIELD";
			case EQUIPPABLE_IN_SLOT_MASK:        return "MASK";
			case EQUIPPABLE_IN_SLOT_HELM:        return "HELM";
			case EQUIPPABLE_IN_SLOT_GLOVES:      return "GLOVES";
			case EQUIPPABLE_IN_SLOT_BOOTS:       return "BOOTS";
			case EQUIPPABLE_IN_SLOT_BREASTPLATE: return "BREASTPLATE";
			case EQUIPPABLE_IN_SLOT_CLOAK:       return "CLOAK";
			case EQUIPPABLE_IN_SLOT_AMULET:      return "AMULET";
			case EQUIPPABLE_IN_SLOT_RING:        return "RING";
			default:                             return "NONE";
		}
	}

	// Resolve an item uid for a READER: this machine's own players' items, then, on the host,
	// the mirror of each remote player's backpack that their own machine reports
	// (sam_mp_inventory.hpp), so a host script reads every player's items by the uids
	// sam_get_inventory hands out. A miss is warned once per function, naming it. `holder`
	// (optional) gets the player whose item it is.
	static Item* samItemFromUid(long long uid, const char* fn, int* holder = nullptr)
	{
		return SAMMpInventory::resolveItem(uid, SAMMpInventory::Use::Read, fn, holder);
	}

	// The same for a WRITER: this machine's own players' items only. On the host a remote
	// player's item never reaches a writer's body: the trampoline carries the call to that
	// player's machine first, with the uid that machine knows the item by, and the body runs
	// there, where this resolves it.
	static Item* samItemForWrite(long long uid, const char* fn, int* holder = nullptr)
	{
		return SAMMpInventory::resolveItem(uid, SAMMpInventory::Use::Write, fn, holder);
	}

	// sam_get_item(uid) -> table | nil
	// One call for every plain field on the item, because fifteen one-line getters would be
	// fifteen things to look up. The computed values (name, weight, value) are separate
	// functions below, since each runs real engine code rather than reading a member.
	int lua_sam_get_item(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_get_item");
		if ( !it ) { lua_pushnil(Ls); return 1; }
		lua_newtable(Ls);
		lua_pushinteger(Ls, (lua_Integer)it->type);       lua_setfield(Ls, -2, "type");
		lua_pushinteger(Ls, (lua_Integer)it->count);      lua_setfield(Ls, -2, "count");
		lua_pushinteger(Ls, (lua_Integer)it->beatitude);  lua_setfield(Ls, -2, "beatitude");
		lua_pushinteger(Ls, (lua_Integer)it->status);     lua_setfield(Ls, -2, "status");
		lua_pushstring(Ls, samItemStatusName((int)it->status)); lua_setfield(Ls, -2, "status_name");
		lua_pushboolean(Ls, it->identified ? 1 : 0);      lua_setfield(Ls, -2, "identified");
		lua_pushinteger(Ls, (lua_Integer)it->appearance); lua_setfield(Ls, -2, "appearance");
		lua_pushinteger(Ls, (lua_Integer)it->ownerUid);   lua_setfield(Ls, -2, "owner_uid");
		lua_pushboolean(Ls, it->isDroppable ? 1 : 0);     lua_setfield(Ls, -2, "droppable");
		lua_pushinteger(Ls, (lua_Integer)it->x);          lua_setfield(Ls, -2, "grid_x");
		lua_pushinteger(Ls, (lua_Integer)it->y);          lua_setfield(Ls, -2, "grid_y");
		return 1;
	}

	// sam_get_item_name(uid) -> string | nil
	// Item::getName renders the player-facing name: the blessed/cursed and condition
	// prefixes, and the unidentified alias rather than the true name. It writes into a
	// shared buffer, so the string is copied into Lua on the next line and never held.
	int lua_sam_get_item_name(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_get_item_name");
		if ( !it ) { lua_pushnil(Ls); return 1; }
		const char* n = it->getName();
		lua_pushstring(Ls, n ? n : "");
		return 1;
	}

	// sam_get_item_value(uid) -> gold | nil. Per instance, so it accounts for the stack,
	// the tome and the custom-id band. Distinct from sam_get_item_info's flat table value.
	int lua_sam_get_item_value(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_get_item_value");
		if ( !it ) { lua_pushnil(Ls); return 1; }
		// getGoldValue is PER UNIT and never touches count (items.cpp:5794), so a stack of
		// fifty gems priced as one. Multiply, because "what is this pile worth" is the
		// question a shop or loot mod is asking.
		lua_pushinteger(Ls, (lua_Integer)((long long)it->getGoldValue() * (long long)(it->count > 0 ? it->count : 1)));
		return 1;
	}

	// sam_get_item_weight(uid) -> weight | nil. Weight times count, with the quiver rule.
	int lua_sam_get_item_weight(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_get_item_weight");
		if ( !it ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)it->getWeight());
		return 1;
	}

	// sam_get_item_attack(uid [, player]) -> tohit | nil
	// With a player, the number that character would actually get; without, the base.
	int lua_sam_get_item_attack(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_get_item_attack");
		if ( !it ) { lua_pushnil(Ls); return 1; }
		const Stat* w = nullptr;
		if ( !lua_isnoneornil(Ls, 2) )
		{
			const int p = (int)luaL_checkinteger(Ls, 2);
			if ( p >= 0 && p < MAXPLAYERS ) { w = stats[p]; }
		}
		lua_pushinteger(Ls, (lua_Integer)it->weaponGetAttack(w));
		return 1;
	}

	// sam_get_item_ac(uid [, player]) -> armour class | nil
	int lua_sam_get_item_ac(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_get_item_ac");
		if ( !it ) { lua_pushnil(Ls); return 1; }
		const Stat* w = nullptr;
		if ( !lua_isnoneornil(Ls, 2) )
		{
			const int p = (int)luaL_checkinteger(Ls, 2);
			if ( p >= 0 && p < MAXPLAYERS ) { w = stats[p]; }
		}
		lua_pushinteger(Ls, (lua_Integer)it->armorGetAC(w));
		return 1;
	}

	// sam_get_tome_spell(uid) -> spell id | nil. Bridges an item to the spell API, so a mod
	// can read which spell a spellbook teaches and then use sam_grant_spell with it.
	int lua_sam_get_tome_spell(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_get_tome_spell");
		// false, not nil, when the uid names no item here: nil already means "this item teaches
		// no spell", and "cannot see it" must not read as that.
		if ( !it ) { lua_pushboolean(Ls, 0); return 1; }
		// Branch on category the way the engine's own getItemVariationFromSpellbookOrTome
		// does (items.cpp:1252): getTomeSpellID matches only the three TOME_ types and
		// returns SPELL_NONE for every SPELLBOOK, so binding it alone answered 0 for the
		// commonest case. nil rather than 0 for "no spell", so `if spell then` works and so
		// the two runtimes agree: 0 is truthy in Lua and falsy in JS.
		int spellID = SPELL_NONE;
		if ( itemCategory(it) == SPELLBOOK )        { spellID = getSpellIDFromSpellbook(it->type); }
		else if ( itemCategory(it) == TOME_SPELL )  { spellID = it->getTomeSpellID(); }
		if ( spellID == SPELL_NONE ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)spellID);
		return 1;
	}

	// sam_get_food_satiation(itemType) -> hunger restored. A TYPE, not a uid: this is the
	// static table value, so a mod can price food it has not spawned yet.
	int lua_sam_get_food_satiation(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int t = (int)luaL_checkinteger(Ls, 1);
		if ( t < 0 || t >= NUM_ITEM_SLOTS ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)Item::getBaseFoodSatiation((ItemType)t));
		return 1;
	}

	// ---- writes (host only) ----------------------------------------------------

	int lua_sam_set_item_beatitude(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int v = (int)luaL_checkinteger(Ls, 2);
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("LUA", "sam_set_item_beatitude refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Item* it = samItemForWrite(uid, "sam_set_item_beatitude");
		if ( !it ) { lua_pushboolean(Ls, 0); return 1; }
		// Sint16 on the item; clamp to a sane blessing range rather than letting a script
		// store a number the tooltip and the damage maths will not survive.
		// -100..100, matching newItem (items.cpp:217), the ground decode (game.cpp:1870) and
		// sam_spawn_item. Clamping to 10 here made the same number mean two different things
		// depending on which entry point a mod used.
		it->beatitude = (Sint16)((v < -100) ? -100 : ((v > 100) ? 100 : v));
		// On the owner's machine (the host carried this call here) a WORN item also has a copy
		// on the host, which combat reads: tell it. No-op on the host and in singleplayer.
		SAMMpInventory::afterOwnerWrite(it);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	int lua_sam_set_item_status(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		int st = -1;
		// lua_type, not lua_isstring: lua_isstring says yes to a NUMBER as well, so the guard
		// had to exclude numbers by hand and the STRING "3" fell through to the numeric path
		// while the JS twin sent it to the name lookup and refused it. A string is a status
		// name in both runtimes now, and a number is a number.
		if ( lua_type(Ls, 2) == LUA_TSTRING )
		{
			if ( !samItemStatusFromName(lua_tostring(Ls, 2), st) )
			{
				SAM_ERROR("LUA", "sam_set_item_status: unknown status. Valid: BROKEN, DECREPIT, WORN, SERVICABLE, EXCELLENT.");
				lua_pushboolean(Ls, 0); return 1;
			}
		}
		else { st = (int)luaL_checkinteger(Ls, 2); }
		// Refused, not clamped. Clamping a negative up to BROKEN made sam_set_item_status(uid, -3)
		// succeed here and fail in JS, which is the worst kind of difference: silent.
		if ( st < BROKEN || st > EXCELLENT )
		{
			SAM_ERROR("LUA", "sam_set_item_status: status must be 0 (BROKEN) to 4 (EXCELLENT), or a name.");
			lua_pushboolean(Ls, 0); return 1;
		}
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("LUA", "sam_set_item_status refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Item* it = samItemForWrite(uid, "sam_set_item_status");
		if ( !it ) { lua_pushboolean(Ls, 0); return 1; }
		// Setting an EQUIPPED item to BROKEN does not unequip it: the engine refuses to USE
		// a broken item but leaves it worn. That is vanilla behaviour, not an oversight here.
		it->status = (Status)st;
		SAMMpInventory::afterOwnerWrite(it);   // a worn item's copy on the host: see sam_set_item_beatitude
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_set_item_count(uid, n). Zero goes through consumeItem rather than writing a zero
	// stack, because a count of 0 left in the bag is an item the UI draws and nothing owns.
	int lua_sam_set_item_count(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int n = (int)luaL_checkinteger(Ls, 2);
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("LUA", "sam_set_item_count refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		// The player whose backpack holds it: on the host its own player, on the owner's machine
		// (the host carried the call here) that machine's player.
		int holder = -1;
		Item* it = samItemForWrite(uid, "sam_set_item_count", &holder);
		if ( !it ) { lua_pushboolean(Ls, 0); return 1; }
		const int owner = holder >= 0 ? holder : clientnum;
		// DESTROY is queued, never immediate. This binding can only run from a script, and
		// a script only runs because the engine called into it, so the caller still holds
		// this pointer: useItem keeps using `item` after firing player.on_item_use
		// (items.cpp:2955 then :2983). See SAMItems::queueDestroy. On a client the queue is
		// drained by the inventory mirror's tick hook (game.cpp drains it on the host only).
		if ( n <= 0 )
		{
			const bool queued = SAMItems::queueDestroy((Uint32)it->uid, owner);
			if ( !queued ) { SAMMpInventory::noteOwnerRefusal("sam_set_item_count", "that item is equipped; unequip it before destroying it.", false, "equipped"); }
			// A destroy has no item left to echo, so this is the only thing that tells the
			// host to look again now instead of at its next scheduled report.
			else { SAMMpInventory::nudgeReport(); }
			lua_pushboolean(Ls, queued ? 1 : 0);
			return 1;
		}
		// Clamp BEFORE the Sint16 cast. Unclamped, 40000 stored as -25536 and 65536 stored
		// as 0, which is the empty stack this function is supposed to make impossible; a
		// negative count then inverts carry weight and is written to the save. The engine's
		// own ceiling is per item and per player, and this batch exposes it as
		// sam_get_max_stack, so there is no excuse for picking a different number here.
		const int cap = it->getMaxStackLimit(owner);
		if ( n > cap )
		{
			const std::string why = std::to_string(n) + " exceeds this item's"
				" stack limit of " + std::to_string(cap) + "; refused. Use sam_get_max_stack to check first.";
			SAM_WARN("LUA", "sam_set_item_count: " + why);
			SAMMpInventory::noteOwnerRefusal("sam_set_item_count", why, false, "stacklimit");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		it->count = (Sint16)n;
		// A worn stack (throwing weapons, a quiver) also has a copy on the host that combat
		// counts down: tell it.
		SAMMpInventory::afterOwnerWrite(it);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_identify_item(player, uid). Through the engine's own Item::onItemIdentified, which
	// picks a unique appearance among the player's lookalikes and, on a client, sends a worn
	// item's new look to the host (EQUA). It also raises player.on_item_identified through
	// SAMMpInput::reportItemIdentified: fired directly on the host for the host's own item,
	// reported to the host by the owner's game when this body runs there (the host carried
	// the call). So the event fires once, on the host, either way. `player` must be the
	// item's holder: identifying the host's item "as player 2" fired player 2's event for it.
	//
	// "Holder" means a player whose BACKPACK the item is in. SAMMpInventory::resolveItem finds
	// a holder only by scanning stats[p]->inventory, so an item lying on the floor, in a chest
	// or in a shop comes back with holder == -1 -- and -1 is not "somebody else's", it is
	// "nobody's". Testing `holder != player` refused every one of those, which broke the
	// sam_spawn_item + sam_identify_item pair that worked in 2.8.0. Only a real other holder
	// is refused.
	int lua_sam_identify_item(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const long long uid = (long long)luaL_checkinteger(Ls, 2);
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("LUA", "sam_identify_item refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS ) { lua_pushboolean(Ls, 0); return 1; }
		int holder = -1;
		Item* it = samItemForWrite(uid, "sam_identify_item", &holder);
		if ( !it ) { lua_pushboolean(Ls, 0); return 1; }
		if ( holder >= 0 && holder != player )
		{
			SAMMpInventory::noteOwnerRefusal("sam_identify_item", "uid " + std::to_string(uid) + " is player "
				+ std::to_string(holder) + "'s item, not player " + std::to_string(player) + "'s; nothing identified.", true, "notyours");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		if ( it->identified ) { lua_pushboolean(Ls, 1); return 1; }   // already done, not a failure
		it->identified = true;
		Item::onItemIdentified(player, it);
		SAMMpInventory::afterOwnerWrite(it);   // a worn item's copy on the host: identified and its new look
		lua_pushboolean(Ls, 1);
		return 1;
	}

	int lua_sam_set_item_appearance(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const long long ap = (long long)luaL_checkinteger(Ls, 2);
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("LUA", "sam_set_item_appearance refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Item* it = samItemForWrite(uid, "sam_set_item_appearance");
		if ( !it || ap < 0 ) { lua_pushboolean(Ls, 0); return 1; }
		// Appearance is NOT decoration on every type. A spell tome's taught spell is
		// appearance % TOME_APPEARANCE_MAX (items.cpp:7451), a loot bag's owner and contents
		// are keyed on it (items.cpp:8127), the four robots encode HP in it
		// (entity.cpp:31649) and a scepter encodes charges (items.cpp:8177). All of that is
		// written to the save, so a "cosmetic" write here is permanent damage. Refuse.
		if ( samItemAppearanceIsGameplay(it->type) )
		{
			SAM_ERROR("LUA", "sam_set_item_appearance: this item type stores gameplay data in"
				" its appearance (a tome's spell, a loot bag's contents, a robot's HP, a"
				" scepter's charges). Refused, because the change would be saved.");
			SAMMpInventory::noteOwnerRefusal("sam_set_item_appearance", "this item type stores gameplay data in its appearance; refused.", false, "appearance");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		it->appearance = (Uint32)ap;
		SAMMpInventory::afterOwnerWrite(it);   // a worn item's copy on the host: see sam_set_item_beatitude
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_set_item_droppable(uid, bool). A boss crown that is scenery rather than loot had
	// no way to say so.
	int lua_sam_set_item_droppable(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		// Required. lua_toboolean on a missing argument is false, so forgetting the flag made
		// the item permanently undroppable and said it had succeeded.
		if ( lua_isnoneornil(Ls, 2) )
		{
			SAM_ERROR("LUA", "sam_set_item_droppable: the true/false argument is required.");
			lua_pushboolean(Ls, 0); return 1;
		}
		const bool d = lua_toboolean(Ls, 2) != 0;
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("LUA", "sam_set_item_droppable refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Item* it = samItemForWrite(uid, "sam_set_item_droppable");
		if ( !it ) { lua_pushboolean(Ls, 0); return 1; }
		it->isDroppable = d;
		// A thief that steals worn armour copies this flag from the host's copy of it
		// (entity.cpp:15044): on the owner's machine, tell that copy.
		SAMMpInventory::afterOwnerWrite(it);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	int lua_sam_get_item_owner(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_get_item_owner");
		// false, not nil, when the uid names no item here: nil already means "nobody owns it".
		if ( !it ) { lua_pushboolean(Ls, 0); return 1; }
		// nil for "nobody", not 0: the docs promise nil, and 0 is truthy in Lua while the
		// JS twin's 0 is falsy, so the same script took opposite branches in the two runtimes.
		if ( it->ownerUid == 0 ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)it->ownerUid);
		return 1;
	}

	// sam_set_item_owner(uid, entityUid). Ownership is what the shopkeeper theft rules read,
	// so this is how a soulbound or stolen-goods mod expresses itself without its own book.
	int lua_sam_set_item_owner(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const long long owner = (long long)luaL_checkinteger(Ls, 2);
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("LUA", "sam_set_item_owner refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Item* it = samItemForWrite(uid, "sam_set_item_owner");
		if ( !it || owner < 0 ) { lua_pushboolean(Ls, 0); return 1; }
		it->ownerUid = (Uint32)owner;
		SAMMpInventory::afterOwnerWrite(it);   // on the owner's machine: re-report the backpack now
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// ---- predicates (read-only, client-safe) -----------------------------------

	int lua_sam_is_ranged_weapon(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int t = (int)luaL_checkinteger(Ls, 1);
		if ( t < 0 || t >= NUM_ITEM_SLOTS ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, isRangedWeapon((ItemType)t) ? 1 : 0);
		return 1;
	}

	int lua_sam_is_melee_weapon(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_is_melee_weapon");
		if ( !it ) { lua_pushnil(Ls); return 1; }   // cannot see it: neither yes nor no
		lua_pushboolean(Ls, isMeleeWeapon(*it) ? 1 : 0);
		return 1;
	}

	int lua_sam_is_shield(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_is_shield");
		if ( !it ) { lua_pushnil(Ls); return 1; }   // cannot see it: neither yes nor no
		// isItemEquippableInShieldSlot, not isShield. The latter demands category == ARMOR
		// and TYPE_SHIELD, so it is false for lanterns, torches, quivers and spellbooks,
		// which are precisely the offhand things a mod asks about. This form also honours
		// the S.A.M SHIELD_SLOT trait (items.cpp:7364).
		lua_pushboolean(Ls, isItemEquippableInShieldSlot(it) ? 1 : 0);
		return 1;
	}

	int lua_sam_is_potion_bad(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_is_potion_bad");
		if ( !it ) { lua_pushnil(Ls); return 1; }   // cannot see it: neither yes nor no
		lua_pushboolean(Ls, isPotionBad(*it) ? 1 : 0);
		return 1;
	}

	// sam_item_has_trait(itemType, "QUIVER") -> boolean. The parity twin of the shipped
	// sam_monster_has_trait. Takes a TYPE so it works on an item you have not spawned.
	int lua_sam_item_has_trait(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int t = (int)luaL_checkinteger(Ls, 1);
		const char* traitC = luaL_checkstring(Ls, 2);
		std::string u;
		for ( const char* c = traitC; c && *c; ++c ) { u += (char)toupper((unsigned char)*c); }
		if ( t < 0 || t >= NUM_ITEM_SLOTS ) { lua_pushboolean(Ls, 0); return 1; }
		const ItemType ty = (ItemType)t;
		// All ELEVEN traits a mod may declare, not the four this used to know. Two sources, asked
		// in this order: the bit a custom item declared (ItemGeneric::samTraits, set from
		// kTraitNames in sam_items.cpp), then the engine's own predicate for a vanilla item.
		// Four of these -- automaton_food, tinker_throwable, usable, beatitude_ac -- had no
		// reader anywhere before this.
		const Uint64 declared = ( t >= 0 && t < NUM_ITEM_SLOTS ) ? ::items[t].samTraits : 0ULL;
		bool v = false;
		bool known = true;
		if      ( u == "RANGED" )           { v = ( declared & SAMItemTrait::RANGED ) || isRangedWeapon(ty); }
		else if ( u == "QUIVER" )           { v = ( declared & SAMItemTrait::QUIVER ) || itemTypeIsQuiver(ty); }
		else if ( u == "FOCI" )             { v = ( declared & SAMItemTrait::FOCI ) || itemTypeIsFoci(ty); }
		else if ( u == "INSTRUMENT" )       { v = ( declared & SAMItemTrait::INSTRUMENT ) || itemTypeIsInstrument(ty); }
		else if ( u == "THROWN_BALL" )      { v = ( declared & SAMItemTrait::THROWN_BALL ) || itemTypeIsThrownBall(ty); }
		else if ( u == "SHIELD_SLOT" )      { v = ( declared & SAMItemTrait::SHIELD_SLOT ) != 0; }
		else if ( u == "POTION_BAD" )       { v = ( declared & SAMItemTrait::POTION_BAD ) != 0; }
		else if ( u == "AUTOMATON_FOOD" )   { v = ( declared & SAMItemTrait::AUTOMATON_FOOD ) != 0; }
		else if ( u == "TINKER_THROWABLE" ) { v = ( declared & SAMItemTrait::TINKER_THROWABLE ) != 0; }
		else if ( u == "USABLE" )           { v = ( declared & SAMItemTrait::USABLE ) != 0; }
		else if ( u == "BEATITUDE_AC" )     { v = ( declared & SAMItemTrait::BEATITUDE_AC ) != 0; }
		else                                { known = false; }
		if ( !known )
		{
			SAM_ERROR("LUA", "sam_item_has_trait: unknown trait '" + u + "'. Valid: RANGED, QUIVER,"
				" FOCI, INSTRUMENT, THROWN_BALL, SHIELD_SLOT, POTION_BAD, AUTOMATON_FOOD,"
				" TINKER_THROWABLE, USABLE, BEATITUDE_AC.");
			lua_pushboolean(Ls, 0); return 1;
		}
		lua_pushboolean(Ls, v ? 1 : 0);
		return 1;
	}

	// sam_get_item_slot(itemType) -> "WEAPON" | ... | "NONE". A TYPE, because this is the
	// static table fact about where a thing is worn, and it works for custom items too.
	int lua_sam_get_item_slot(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int t = (int)luaL_checkinteger(Ls, 1);
		if ( t < 0 || t >= NUM_ITEM_SLOTS ) { lua_pushnil(Ls); return 1; }
		lua_pushstring(Ls, samItemSlotName((int)items[t].item_slot));
		return 1;
	}

	int lua_sam_is_better_weapon(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* a = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_is_better_weapon");
		const bool hasB = !lua_isnoneornil(Ls, 2);
		Item* b = hasB ? samItemFromUid((long long)luaL_checkinteger(Ls, 2), "sam_is_better_weapon") : nullptr;
		// nil when either uid names no item here. A second uid that was given but did not resolve
		// used to become "nothing", which silently changed the question to "is this better than
		// no weapon at all".
		if ( !a || ( hasB && !b ) ) { lua_pushnil(Ls); return 1; }
		// isThisABetterWeapon short-circuits to TRUE whenever there is nothing to compare
		// against (items.cpp:7199), with no category test, so the documented "is this an
		// upgrade over nothing" form said yes to bread. Ask the category ourselves.
		if ( !b && itemCategory(a) != WEAPON ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, Item::isThisABetterWeapon(*a, b) ? 1 : 0);
		return 1;
	}

	int lua_sam_is_better_armor(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		Item* a = samItemFromUid((long long)luaL_checkinteger(Ls, 1), "sam_is_better_armor");
		const bool hasB = !lua_isnoneornil(Ls, 2);
		Item* b = hasB ? samItemFromUid((long long)luaL_checkinteger(Ls, 2), "sam_is_better_armor") : nullptr;
		// nil when either uid names no item here. A second uid that was given but did not resolve
		// used to become "nothing", which silently changed the question to "is this better than
		// no armour at all".
		if ( !a || ( hasB && !b ) ) { lua_pushnil(Ls); return 1; }
		// isThisABetterArmor short-circuits to TRUE the moment there is nothing to compare
		// against (items.cpp:7215) and never asks whether the new item is armour at all, so
		// the documented "is this worth wearing" form said yes to a potion. The engine only
		// ever reaches that function through checkEquipType, which has already routed the
		// item into one of these seven slots (entity.cpp:26149-26228). That routing is the
		// guard, and it is the same short-circuit already fixed in sam_is_better_weapon.
		if ( !b )
		{
			const int t = (int)a->type;
			if ( t < 0 || t >= NUM_ITEM_SLOTS ) { lua_pushboolean(Ls, 0); return 1; }
			const int slot = (int)items[t].item_slot;
			if ( slot != EQUIPPABLE_IN_SLOT_SHIELD && slot != EQUIPPABLE_IN_SLOT_MASK
				&& slot != EQUIPPABLE_IN_SLOT_HELM && slot != EQUIPPABLE_IN_SLOT_GLOVES
				&& slot != EQUIPPABLE_IN_SLOT_BOOTS && slot != EQUIPPABLE_IN_SLOT_BREASTPLATE
				&& slot != EQUIPPABLE_IN_SLOT_CLOAK )
			{
				lua_pushboolean(Ls, 0); return 1;
			}
		}
		lua_pushboolean(Ls, Item::isThisABetterArmor(*a, b) ? 1 : 0);
		return 1;
	}

	// ---- queries ---------------------------------------------------------------

	int lua_sam_is_item_equipped(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 2), "sam_is_item_equipped");
		if ( !it ) { lua_pushnil(Ls); return 1; }   // cannot see it: not the same as "not worn"
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushboolean(Ls, 0); return 1; }
		// By POINTER, through the one helper sam_get_inventory's `equipped` also uses, so the two
		// can never disagree. itemIsEquipped compares with itemCompare (items.cpp:4895), so two
		// identical rings are indistinguishable and the spare reports as worn; and on the host a
		// mirrored item is checked against the slots its owner reported.
		lua_pushboolean(Ls, SAMMpInventory::isEquipped(player, it) ? 1 : 0);
		return 1;
	}

	// sam_can_unequip(player, uid) -> boolean. False for a cursed item, which is exactly
	// what a mod needs to check before promising the player a swap.
	int lua_sam_can_unequip(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 2), "sam_can_unequip");
		if ( !it ) { lua_pushnil(Ls); return 1; }   // cannot see it: false would read as "cursed"
		if ( player < 0 || player >= MAXPLAYERS ) { lua_pushboolean(Ls, 0); return 1; }
		// NOT Item::canUnequip. That function is non-const and IDENTIFIES the item as a side
		// effect (items.cpp:6301 and :6325 both do identified = true; onItemIdentified(...)),
		// so a mod polling this in on_tick would silently identify every cursed item the
		// player owns, and on a client would send an appearance update to the server.
		// Answer from the fields it would have read.
		lua_pushboolean(Ls, samItemCanUnequipQuiet(it, stats[player]) ? 1 : 0);
		return 1;
	}

	// sam_inventory_has_space(player) -> boolean.
	// LOCAL PLAYER ONLY, and this one really is a hard limit rather than caution: the query
	// reads players[p]->inventoryUI, a UI grid that exists only on the machine drawing it.
	// Asking about a remote player would return that machine's own bag, which is worse than
	// refusing, so it refuses.
	int lua_sam_inventory_has_space(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { lua_pushnil(Ls); return 1; }
		if ( !players[player]->isLocalPlayer() )
		{
			SAM_WARN("LUA", "sam_inventory_has_space: the inventory grid is local to each machine;"
				" a remote player cannot be asked. Returning nil.");
			lua_pushnil(Ls);
			return 1;
		}
		lua_pushboolean(Ls, players[player]->inventoryUI.bItemInventoryHasFreeSlot() ? 1 : 0);
		return 1;
	}

	int lua_sam_get_max_stack(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		Item* it = samItemFromUid((long long)luaL_checkinteger(Ls, 2), "sam_get_max_stack");
		if ( !it || player < 0 || player >= MAXPLAYERS ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)it->getMaxStackLimit(player));
		return 1;
	}

	// sam_can_items_stack(player, uidA, uidB) -> boolean. Needed by anything that writes a
	// count, so it ships beside sam_set_item_count rather than later.
	int lua_sam_can_items_stack(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		Item* a = samItemFromUid((long long)luaL_checkinteger(Ls, 2), "sam_can_items_stack");
		Item* b = samItemFromUid((long long)luaL_checkinteger(Ls, 3), "sam_can_items_stack");
		if ( !a || !b ) { lua_pushnil(Ls); return 1; }   // cannot see one: not "they would not merge"
		if ( player < 0 || player >= MAXPLAYERS ) { lua_pushboolean(Ls, 0); return 1; }
		// Same type, same beatitude, same status and same identified state is what the engine
		// means by stackable; shouldItemStack then applies the per-type ceiling.
		// The engine's own test, rather than a hand-rolled field comparison that missed the
		// derived model index, the unidentified scroll label, the types that skip status,
		// and the never-stack classes. a != b because one uid twice is not a merge, and the
		// ceiling belongs to the DESTINATION stack.
		lua_pushboolean(Ls, ( a != b && itemCompare(a, b, false) == 0
			&& b->shouldItemStack(player) ) ? 1 : 0);
		return 1;
	}

	// sam_monster_can_wield(monsterUid, itemType) -> boolean. Host-only in effect: it reads
	// the monster's Stat, which a client does not hold for an ordinary creature.
	int lua_sam_monster_can_wield(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long muid = (long long)luaL_checkinteger(Ls, 1);
		const int t = (int)luaL_checkinteger(Ls, 2);
		// The trampoline refuses a client's call before this runs (Host contract); this is a
		// backstop, and it answers what the refusal answers -- no value. It used to answer
		// false, which is a real answer to "can it wield this?".
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_monster_can_wield refused: host only."); return 0; }
		Entity* e = samResolveMonster(muid);
		if ( !e || t < 0 || t >= NUM_ITEM_SLOTS ) { lua_pushboolean(Ls, 0); return 1; }
		// Value-initialised: Item has no constructor and its header says no destructor is
		// ever called, so a plain declaration would leave uid, x, y and ownerUid holding
		// whatever was on the stack, and canWieldItem may read them.
		Item probe{};
		probe.type = (ItemType)t;
		probe.status = EXCELLENT;
		probe.beatitude = 0;
		probe.count = 1;
		probe.appearance = 0;
		probe.identified = true;
		lua_pushboolean(Ls, e->canWieldItem(probe) ? 1 : 0);
		return 1;
	}

	// sam_get_position(uid) -> tileX, tileY | nil. Any live entity (player/monster/item).
	int lua_sam_get_position(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		Entity* e = samResolveEntityQuiet((long long)uid);
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)((int)e->x >> 4)); // pixel -> tile
		lua_pushinteger(Ls, (lua_Integer)((int)e->y >> 4));
		return 2;
	}

	// sam_set_position(uid, tileX, tileY) -> boolean. Players go through the safe
	// teleport() path (obstacle + MFLAG_DISABLETELEPORT guards + TELE packet) so they
	// can't tunnel into walls; other entities move by x/y + UPDATENEEDED (the server's
	// per-frame broadcast picks it up — no new packet). Host only.
	// sam_can_stand(uid, tileX, tileY) -> boolean
	//
	// "Would the engine accept THIS entity at that tile?" -- which is a different question from
	// sam_is_spawnable, whose whole body is `t.valid && t.walkable`: a map read that cannot see
	// entities, and cannot see the asker's own collision profile. checkObstacle can: levitation,
	// size, and the pass-through set all change the answer, and no script can compute those.
	//
	// THE BOUNDS TEST IS NOT OPTIONAL. checkObstacle returns 0 -- meaning CLEAR -- for every
	// coordinate outside the map, because its body sits inside a bounds check and falls through
	// to a final `return 0;` when that fails. Without the test below this function would
	// confidently green-light a teleport into the void, which is the fallback-returns-a-real-value
	// shape this batch exists to stop repeating.
	//
	// True is NECESSARY, not SUFFICIENT: Entity::teleport applies further rules of its own
	// (entityInsideSomething, the Minotaur-level ban, MFLAG_DISABLETELEPORT for players).
	int lua_sam_can_stand(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int tx = (int)luaL_checkinteger(Ls, 2);
		const int ty = (int)luaL_checkinteger(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveEntityRead(uid, "sam_can_stand");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ lua_pushboolean(Ls, 0); return 1; }
		// On a CLIENT this cannot answer the question it exists to answer. checkObstacle's entity
		// pass walks TileEntityList, and that grid is filled only by the host's entity loop -- the
		// client never calls addEntity, which is why barony_clear has an explicit CLIENT branch
		// and checkObstacle has none. Walls and floors still test correctly, so the answer LOOKS
		// right while every creature standing there is invisible to it. Say so rather than hand
		// back a confident half-answer.
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_can_stand: on a connected client this only sees walls and floor,"
				" not creatures, because the client does not keep the entity grid the test reads."
				" Ask the host if the answer has to include who is standing there.");
		}
		SAMHitGuard samHit;   // checkObstacle writes the engine's `hit` global
		lua_pushboolean(Ls, checkObstacle((tx << 4) + 8, (ty << 4) + 8, e, nullptr) == 0 ? 1 : 0);
		return 1;
#else
		(void)uid; (void)tx; (void)ty; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	int lua_sam_set_position(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int tx = (int)luaL_checkinteger(Ls, 2);
		const int ty = (int)luaL_checkinteger(Ls, 3);
		// The shared resolver, not uidToEntity: this is the world function every mod uses and it
		// was still the one accepting a shared sentinel uid and a limb.
		Entity* e = samResolveWritable(uid, "sam_set_position");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("LUA", "sam_set_position: tile (" + std::to_string(tx) + "," + std::to_string(ty) + ") out of bounds."); lua_pushboolean(Ls, 0); return 1; }
		if ( e->behavior == &actPlayer )
		{
			const bool ok = e->teleport(tx, ty); // may refuse (walls / minotaur level / map flag)
			// teleport() sent TELE; point a remote player's host copy at the new spot at once instead
			// of dragging it back to new_x until the owner's first report arrives.
			if ( ok ) { SAMMpEntities::teleported(e); }
			lua_pushboolean(Ls, ok ? 1 : 0);
			return 1;
		}
		// Everything that is not a player is placed wherever it is told, walls included, because
		// putting a decoration inside a wall alcove is a real thing mods do on purpose. But a
		// monster dropped into one is stuck for good, and until now nothing said a word about it.
		// So this WARNS and still places: refusing would break the deliberate case, and
		// sam_can_stand is the test to run first when the answer matters.
		{
			SAMHitGuard samHit;   // checkObstacle writes the engine's `hit` global
			if ( checkObstacle((tx << 4) + 8, (ty << 4) + 8, e, nullptr) != 0 )
			{
				SAM_WARN("LUA", "sam_set_position: tile (" + std::to_string(tx) + ","
					+ std::to_string(ty) + ") is blocked for this entity, so it is being placed"
					" inside something. Fine for a decoration; a monster put there cannot get out."
					" sam_can_stand(uid, x, y) answers this before you move anything.");
			}
		}
		e->x = (double)(tx * 16 + 8);
		e->y = (double)(ty * 16 + 8);
		e->flags[UPDATENEEDED] = true;
		e->flags[NOUPDATE] = false;
		TileEntityList.updateEntity(*e); // re-bucket in the spatial grid, as teleport() does
		// A ground item or gold bag is woken (it settles as a moved item does, on every machine) and
		// sent as the engine's GHOI, which a stock client handles; a prop a client pins goes to S.A.M
		// clients on the ordered channel, with a warning when a stock client cannot be told.
		SAMMpEntities::moved(e);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// samSummonCustomVariant: summon a mod-declared monster variant by its "ns:slug" id, or
	// nullptr if that id was never declared.
	//
	// (The five lines that used to open this comment described sam_spawn_monster and its SUMM
	// packet. They ended up here when this helper was inserted between the comment and the
	// function it belonged to, so the file documented a network packet on a routine that sends
	// none. sam_spawn_monster's own description now lives on sam_spawn_monster, below.)
	// Hands off to createMonsterFromFile -- the SAME routine level generation uses
	// (maps.cpp:8003) -- so stats, equipment, traits, body model and followers are applied
	// exactly as on a generated one. Before this a script could only summon a vanilla
	// species and then hand-patch it, which never reproduced the body or the followers.
	Entity* samSummonCustomVariant(const std::string& id, int tx, int ty)
	{
		const SAMMonsters::VariantRef* ref = SAMMonsters::variantForId(id);
		if ( !ref ) { return nullptr; }
		const int base = samMonsterNameToId(ref->baseType.c_str());
		if ( base <= 0 ) { return nullptr; }
		Entity* e = summonMonster(static_cast<Monster>(base), tx * 16 + 8, ty * 16 + 8);
		if ( !e ) { return nullptr; }
		if ( Stat* st = e->getStats() )
		{
			Monster outType = static_cast<Monster>(base);
			monsterCurveCustomManager.createMonsterFromFile(e, st, ref->variantFile, outType);
		}
		return e;
	}

	// ---- v2.5 runtime model control -------------------------------------------------
	//
	// Until now nothing could change a model once the game was running: every model was
	// fixed at spawn by JSON. These carry the model ID over the wire, so a transformation
	// is seen by every player rather than only the host -- an index would mean whatever
	// that slot happened to hold on each machine.

	// sam_set_model(uid, "ns:model") -> boolean. Works on any entity, including a limb.
	int lua_sam_set_model(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* idC = luaL_checkstring(Ls, 2);
		const std::string modelId = idC ? idC : "";
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_model refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( !SAMBodies::setBodyById((uint32_t)uid, modelId) )
		{
			SAM_ERROR("LUA", "sam_set_model: no model registered as '" + modelId
				+ "'. Declare it in mod.json \"models\" (or use an item/class/race model path).");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_clear_model(uid) -> boolean. The entity goes back to whatever it would otherwise
	// draw: its JSON body, or its own sprite.
	int lua_sam_clear_model(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_clear_model refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		SAMBodies::clearBodyById((uint32_t)uid);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_get_model(uid) -> "ns:model" | nil. Answers about what a SCRIPT set, which is the
	// only thing a script can meaningfully read back -- a JSON body is the mod's own data.
	int lua_sam_get_model(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const std::string id = SAMBodies::bodyIdFor((uint32_t)uid);
		if ( id.empty() ) { lua_pushnil(Ls); } else { lua_pushstring(Ls, id.c_str()); }
		return 1;
	}

	// sam_set_scale(uid, s) -> boolean. Barony quantises scale on the wire to steps of 1/128
	// and caps it just under 2, so anything larger would look right to the host and wrong to
	// everybody else. Clamped here rather than silently truncated there.
	int lua_sam_set_scale(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		double sc = (double)luaL_checknumber(Ls, 2);
		Entity* e = samResolveWritable(uid, "sam_set_scale");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		// A player's scale and a slime's are rewritten every frame on every machine: refused, said why.
		if ( !SAMMpEntities::scaleSticks(e, "sam_set_scale") ) { lua_pushboolean(Ls, 0); return 1; }
		if ( !std::isfinite(sc) )
		{
			SAM_ERROR("LUA", "sam_set_scale: scale must be a finite number.");
			lua_pushboolean(Ls, 0); return 1;
		}
		// A scale of 0 or less used to become 1.0 -- FULL SIZE, silently, with a true return,
		// while the clamp below warns. A script easing a model down to nothing popped it back to
		// full on the last frame. Refused instead: it is not a value this function can honour.
		if ( !(sc > 0.0) )
		{
			SAM_ERROR("LUA", "sam_set_scale: scale must be greater than 0 (was "
				+ std::to_string(sc) + "). To make something disappear use sam_set_visible.");
			lua_pushboolean(Ls, 0); return 1;
		}
		// And the bottom of the range needs the same guard as the top: the wire packs scale as
		// (Uint8)(scale * 128) (net.cpp:552), so anything under 1/128 arrives on every other
		// machine as 0 and the model vanishes there while the host draws a speck.
		if ( sc < 1.0 / 128.0 )
		{
			SAM_WARN("LUA", "sam_set_scale: " + std::to_string(sc) + " is below the 1/128 the"
				" network can carry, so other players would see it disappear. Clamped.");
			sc = 1.0 / 128.0;
		}
		// Clamped ALWAYS, not just when `multiplayer != SINGLE`. The old guard meant a mod
		// authored in singleplayer at 3.0 worked for its author and was silently wrong for
		// everyone else, because the wire cannot carry past 1.99 -- the mod is written once and
		// played in both modes, so the limit has to be the same in both.
		if ( sc > 1.99 )
		{
			SAM_WARN("LUA", "sam_set_scale: " + std::to_string(sc) + " is past the 1.99 the network"
				" can carry, so other players would not see it. Clamped.");
			sc = 1.99;
		}
		e->scalex = sc; e->scaley = sc; e->scalez = sc;
		// Without this the 8 Hz sweep that tells clients never fires, so the host sees the new
		// size and nobody else ever does.
		e->flags[UPDATENEEDED] = true;
		// A ground item or a prop the client pins never takes ENTU; S.A.M clients are told directly.
		SAMMpEntities::transformed(e, SAMMpEntities::Changed::SCALE);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_set_damage_immune(uid, on) -> boolean
	//
	// Stops a PLAYER OR A MONSTER taking damage at Entity::modHP, beside the engine's own three
	// immunities (godmode, EFF_STASIS and the salamander heart). It does not stop the hit landing,
	// the sound, or the knockback -- only the loss of health -- which is exactly what
	// "invulnerable" means everywhere else in Barony.
	//
	// Refused on anything else, and the refusal says why. modHP is every point of damage to a
	// CREATURE, but a breakable decoration is not a creature: it carries colliderCurrentHP, which
	// five gameplay sites decrement directly. Accepting one would have been a success return and
	// a matching reader both agreeing about a protection that does not exist.
	//
	// A mod could already do this from an on_before_damage handler. The difference is that this
	// costs nothing per hit and needs no bookkeeping.
	//
	// SESSION STATE. It is not saved, and it is cleared on every floor, because uids restart from
	// 1 on each level and a leftover entry would hand a boss's invulnerability to a rat.
	int lua_sam_set_damage_immune(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		bool on = false;
		if ( !samBoolReq(Ls, 2, "sam_set_damage_immune", &on) ) { lua_pushboolean(Ls, 0); return 1; }
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveWritable(uid, "sam_set_damage_immune");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		// Only a PLAYER OR A MONSTER takes damage through Entity::modHP, which is where this
		// immunity lives. Chests, doors, furniture and breakable decorations carry separate
		// health pools -- colliderCurrentHP is decremented directly at five gameplay sites
		// (actgeneral.cpp:2357/3786, actmonster.cpp:6679/7949) and never passes through modHP --
		// so protecting one used to return true, read back true, and let the next arrow destroy
		// it anyway. Guarded on behaviour rather than getStats(), because a monster's Stat hangs
		// off its children and can momentarily be absent.
		if ( !(e->behavior == &actPlayer || e->behavior == &actMonster) )
		{
			SAM_WARN("LUA", "sam_set_damage_immune refused: uid " + std::to_string(uid) + " is not"
				" a player or a monster. Only those take damage through the one place this"
				" immunity lives. Chests, doors, furniture and breakable props carry their own"
				" health that nothing here can reach.");
			lua_pushboolean(Ls, 0); return 1;
		}
		if ( on ) { g_damageImmune.insert((Uint32)uid); }
		else      { g_damageImmune.erase((Uint32)uid); }
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_is_damage_immune(uid) -> boolean. Shipped with the setter on purpose: sam_set_scale and
	// sam_set_visible both went out without a reader and both needed one adding later.
	int lua_sam_is_damage_immune(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, ( uid > 0 && g_damageImmune.count((Uint32)uid) > 0 ) ? 1 : 0);
		return 1;
#else
		(void)uid; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// The shared body of sam_apply_force, so the two runtimes cannot drift on the part that took
	// the reading to get right. Returns false whenever nothing would actually consume the push --
	// which is a stronger promise than it first looks, because two kinds of entity accept a
	// velocity and then ignore it: one whose act function never reads vel_x at all (actPortal),
	// and a ground item that has come to rest and short-circuits before the physics.
	//
	// EFF_KNOCKBACK FIRST, ALWAYS. Both act functions discard velocity without it -- actMonster
	// zeroes vel_x/vel_y (actmonster.cpp:5378) and actPlayer zeroes the knockback fields
	// (actplayer.cpp:4776) -- so every knockback site in the engine sets the effect before it
	// writes anything, and so does this.
	bool samApplyForce(Entity* e, double force, double angle, int ticks)
	{
#ifdef SAM_LUA_HAVE_BARONY
		// The effect's OWN ANSWER decides whether the shove happens, because setEffect refuses
		// EFF_KNOCKBACK outright for a whole band of species (entity.cpp:24740-24753), and for an
		// entity with no Stat at all. Both act functions then zero the velocity on the very next
		// frame, so pushing on regardless would be a function that reports success and moves
		// nothing -- exactly what this batch exists to stop.
		if ( ( e->behavior == &actPlayer || e->behavior == &actMonster )
			&& !e->setEffect(EFF_KNOCKBACK, true, ticks, false) )
		{
			// The species named here are the ones setEffect actually refuses:
			// `type >= LICH && type < KOBOLD` (entity.cpp:24746), and the enum runs LICH,
			// MINOTAUR, DEVIL, SHOPKEEPER, KOBOLD (monster.hpp:41-45), plus LICH_FIRE and
			// LICH_ICE. This message used to say "shadows", which are enum 50 and not immune at
			// all, and never mentioned the minotaur, which is the one boss anybody would try
			// this on.
			SAM_WARN("LUA", "sam_apply_force: this creature refuses to be knocked back, so nothing"
				" would have moved. Liches, minotaurs, the devil and shopkeepers are immune, and"
				" the engine's own knockback does nothing to them either.");
			return false;
		}
		if ( e->behavior == &actPlayer )
		{
			// A player takes the impulse as the knockback SPEED itself; actPlayer converts it
			// through the tangent every frame. Writing vel_x on a player does nothing at all.
			e->monsterKnockbackVelocity = force;
			e->monsterKnockbackTangentDir = angle;
			// A remote player's own machine is drawing the shove, so it has to be told. fskill 9
			// is monsterKnockbackVelocity and 11 is the tangent (entity.hpp:279, 288); the engine
			// sends exactly this pair at actarrow.cpp:1541.
			const int pn = e->skill[2];
			if ( multiplayer == SERVER && pn >= 0 && pn < MAXPLAYERS
				&& players[pn] && !players[pn]->isLocalPlayer() )
			{
				serverUpdateEntityFSkill(e, 9);
				serverUpdateEntityFSkill(e, 11);
			}
			return true;
		}
		if ( e->behavior == &actMonster )
		{
			// A monster takes it as a real velocity, and monsterKnockbackVelocity is the RECOVERY
			// rate the engine uses afterwards to accelerate it back to its walking speed -- 0.01
			// is what every engine site uses, so the stagger lasts as long as the engine's own.
			e->vel_x = std::cos(angle) * force;
			e->vel_y = std::sin(angle) * force;
			e->monsterKnockbackVelocity = 0.01;
			e->monsterKnockbackTangentDir = angle;
			e->flags[UPDATENEEDED] = true;
			return true;
		}
		// Everything else has to be on an ALLOWLIST, not merely have a behaviour. The old guard
		// was `if ( !e->behavior )` -- a null pointer, which virtually never happens -- so every
		// prop got a velocity write and an unconditional true. actPortal, which is the entity
		// sam_spawn_portal creates for scripts, contains no reference to vel_ anywhere in
		// actladder.cpp: the one decorative type this API hands out was guaranteed to be a
		// silent no-op. An allowlist means a prop type added later defaults to an honest
		// refusal rather than a false success.
		const bool samCanBePushed = e->behavior == &actItem || e->behavior == &actArrow
			|| e->behavior == &actThrown || e->behavior == &actBoulder
			|| e->behavior == &actGoldBag || e->behavior == &actGib;
		if ( !samCanBePushed )
		{
			SAM_WARN("LUA", "sam_apply_force: nothing reads this entity's velocity, so a shove"
				" would not move it. It works on players, monsters, ground items, arrows, thrown"
				" weapons, boulders, gold and gibs. For anything else use sam_move_entity, which"
				" moves it directly.");
			return false;
		}
		// A ground item that has come to rest is SWITCHED OFF: actItem returns at
		// actitem.cpp:1063 before it ever reaches the physics at 1352 that reads vel_x. So the
		// velocity had to be woken as well as written, exactly as the engine's own push sites do
		// (actitem.cpp:728, 1014). Without this the item never moved a single pixel, for ever,
		// while the call returned true.
		if ( e->behavior == &actItem )
		{
			e->itemNotMoving = 0;
			e->itemNotMovingClient = 0;
			if ( multiplayer == SERVER )
			{
				serverUpdateEntitySkill(e, 18);   // itemNotMoving
				serverUpdateEntitySkill(e, 19);   // itemNotMovingClient
			}
		}
		e->vel_x = std::cos(angle) * force;
		e->vel_y = std::sin(angle) * force;
		e->flags[UPDATENEEDED] = true;
		// A gold bag at rest ignores velocity (goldBouncing), on every machine; and a client pins
		// items and gold with NOUPDATE, so the velocity has to go as the engine's GHOI, with one more
		// once the host copy rests. SAMMpEntities::pushed does both; any other entity passes through.
		SAMMpEntities::pushed(e);
		return true;
#else
		(void)e; (void)force; (void)angle; (void)ticks; return false;
#endif
	}

	// sam_move_entity(uid, dxTiles, dyTiles) -> tiles actually moved | nil
	//
	// A relative move that RESPECTS WALLS, through the engine's own clipMove, which slides along
	// whatever it hits instead of stopping dead. It returns the distance it managed, and so does
	// this: a bare true would hide the difference between a clear corridor and a wall two inches
	// away, which is the whole thing a script pushing something needs to know.
	//
	// DISTANCES ARE IN TILES, like every other spatial call in this API. The engine works in
	// world units, 16 to a tile, and mixing the two silently is a worse trap than converting.
	int lua_sam_move_entity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const double dx = (double)luaL_checknumber(Ls, 2);
		const double dy = (double)luaL_checknumber(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveWritable(uid, "sam_move_entity");
		if ( !e ) { lua_pushnil(Ls); return 1; }
		if ( !std::isfinite(dx) || !std::isfinite(dy) )
		{
			SAM_ERROR("LUA", "sam_move_entity: the distance must be two finite numbers.");
			lua_pushnil(Ls); return 1;
		}
		// A REMOTE player is moved from where its owner last reported (actPlayer resets the host copy
		// to that anyway) and the owner is told: a S.A.M machine gets the delta on the ordered channel
		// and applies it through its own collision, a stock one gets the engine's PMOV correction.
		// A ground item or gold bag is woken and sent to every client as the engine's own GHOI; a
		// prop a client pins goes to S.A.M clients on the ordered channel. SAMMpEntities::endMove.
		// SUB-STEPPED, because clipMove is not a sweep. Its entire collision test is
		// barony_clear() on the DESTINATION POINT (collision.cpp:1768) -- so one big step over a
		// one-tile wall lands on the far side, finds it clear, and returns the full distance as
		// though the path had been open. Every engine caller escapes this by passing a single
		// frame of velocity; a script can pass any number at all. sam_apply_force clamps for
		// exactly this reason sixty lines below, and this is the same rule, applied as steps so
		// that a legitimate long move still slides along walls instead of being truncated.
		real_t samVx = (real_t)(dx * 16.0), samVy = (real_t)(dy * 16.0);
		if ( std::fabs(samVx) > 16.0 * 1024.0 || std::fabs(samVy) > 16.0 * 1024.0 )
		{
			SAM_ERROR("LUA", "sam_move_entity: that distance is past the end of any map. Use"
				" sam_set_position to place something somewhere far away.");
			lua_pushnil(Ls); return 1;
		}
		// 7.0 is this project's tunnel-safe step: two samples 7 apart cannot both miss a wall
		// 16 wide, so nothing can be stepped over.
		const int samSteps = std::max(1, (int)std::ceil(std::sqrt(samVx * samVx + samVy * samVy) / 7.0));
		samVx /= (real_t)samSteps;
		samVy /= (real_t)samSteps;
		double samStartX = 0.0, samStartY = 0.0;
		SAMMpEntities::beginMove(e, samStartX, samStartY);
		SAMHitGuard samHit;   // clipMove's first statement is `hit.entity = NULL;`
		real_t moved = 0.0;
		for ( int samI = 0; samI < samSteps; ++samI )
		{
			const real_t samGot = clipMove(&e->x, &e->y, samVx, samVy, e);
			moved += samGot;
			if ( samGot <= 0.0 ) { break; }   // wedged against something; further steps cannot help
		}
		e->flags[UPDATENEEDED] = true;
		e->flags[NOUPDATE] = false;
		TileEntityList.updateEntity(*e);
		SAMMpEntities::endMove(e, samStartX, samStartY);
		lua_pushnumber(Ls, (lua_Number)(moved / 16.0));
		return 1;
#else
		(void)uid; (void)dx; (void)dy; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_apply_force(uid, force, angle [, ticks]) -> boolean
	//
	// A shove, in the engine's own knockback terms. The angle is a Barony yaw in radians, the same
	// number sam_get_facing hands back, so "away from me" is atan2(theirY - myY, theirX - myX).
	// Force is on the engine's scale: an arrow is 0.6 and a strong hit about 1.4
	// (actarrow.cpp:1498).
	//
	// This is NOT a velocity write, and that is the point -- see samApplyForce above for why a
	// velocity write does nothing at all to a player or a monster.
	int lua_sam_apply_force(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		double force = (double)luaL_checknumber(Ls, 2);
		double angle = (double)luaL_checknumber(Ls, 3);
		int ticks = (int)luaL_optinteger(Ls, 4, 30);   // 30 is what the engine's own sites use
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveWritable(uid, "sam_apply_force");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		if ( !std::isfinite(force) || !std::isfinite(angle) )
		{
			SAM_ERROR("LUA", "sam_apply_force: force and angle must be finite numbers.");
			lua_pushboolean(Ls, 0); return 1;
		}
		// 7.0 is this project's established tunnel-safe ceiling, the same number the move-speed
		// cap uses: collision is tested at the destination point, so a big enough single step
		// steps straight over a wall instead of into it.
		if ( force > 7.0 || force < -7.0 )
		{
			SAM_WARN("LUA", "sam_apply_force: force is clamped to 7, past which a single step can"
				" jump clean over a wall instead of hitting it.");
			force = ( force > 0.0 ) ? 7.0 : -7.0;
		}
		if ( ticks < 1 ) { ticks = 1; }
		if ( ticks > 3600 ) { ticks = 3600; }
		// The angle goes to a remote player as monsterKnockbackTangentDir, and ENFS packs an
		// fskill as (Sint16)(value * 256) (net.cpp:916) -- about +/-128 radians. A modder who
		// passes degrees by mistake sends 180, which wraps to -76.0 on the shoved player's own
		// machine, and that is the machine that renders the shove. Wrapping into [0, 2*PI) is
		// exact for a direction and puts every value inside the wire's range.
		angle = std::fmod(angle, 2.0 * PI);
		if ( angle < 0.0 ) { angle += 2.0 * PI; }
		lua_pushboolean(Ls, samApplyForce(e, force, angle, ticks) ? 1 : 0);
		return 1;
#else
		(void)uid; (void)force; (void)angle; (void)ticks; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_on_fire(uid [, on]) -> boolean
	//
	// Returns whether the entity is ON FIRE once the call is done, which is not what
	// Entity::SetEntityOnFire returns. Its false covers three different situations and one of them
	// is "it was already burning" -- the inner `if ( !flags[BURNING] )` simply falls out of the
	// bottom of the function -- so passing that boolean along would report failure about something
	// that is visibly alight, and a script retrying on false would retry for ever.
	//
	// The two real failures each get their own warning, because "nothing happened" with no reason
	// is the thing that makes a modder rewrite working code.
	//
	// The flag defaults to true. That is not a contradiction of sam_set_visible refusing a missing
	// flag: the test is whether the name already answers the question. "set on fire" does.
	int lua_sam_set_on_fire(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const bool on = samBoolArg(Ls, 2, true);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveWritable(uid, "sam_set_on_fire");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }

		if ( !on )
		{
			// Putting it out is the whole reason this takes a flag at all. The burn timer lives
			// in Entity::handleEffects, which runs from actMonster and actPlayer and nowhere else
			// -- so an entity with no Stat that gets lit stays lit for the rest of the level with
			// nothing in the engine able to stop it. This is the way back.
			if ( e->flags[BURNING] )
			{
				e->flags[BURNING] = false;
				e->char_fire = 0;
				if ( multiplayer == SERVER ) { serverUpdateEntityFlag(e, BURNING); }
				// The same line the engine prints when a fire burns itself out, for the same
				// reason: a player whose character stops burning should be told.
				if ( e->behavior == &actPlayer )
				{
					messagePlayer(e->skill[2], MESSAGE_STATUS, "%s", Language::get(647));
				}
			}
			lua_pushboolean(Ls, 0);   // asked for not-burning, and it is not burning
			return 1;
		}

		if ( e->flags[BURNING] ) { lua_pushboolean(Ls, 1); return 1; }   // already, and that is a yes
		if ( !e->flags[BURNABLE] )
		{
			// A monster gets BURNABLE from its own species init (initRat sets it, monster_rat.cpp:26),
			// and that runs on its FIRST actMonster tick, under MONSTER_INIT = skill[3]. So a
			// monster spawned by a script this frame is not burnable yet, and telling the modder
			// "a rat is not BURNABLE" would send them hunting for a bug that is not there.
			if ( e->behavior == &actMonster && e->skill[3] == 0 )
			{
				SAM_WARN("LUA", "sam_set_on_fire: this monster was spawned this frame and has not run"
					" its own setup yet, so it is not burnable YET. Wait a frame (sam_set_timer with"
					" a short delay) and it will light normally.");
			}
			else
			{
				SAM_WARN("LUA", "sam_set_on_fire: this entity is not BURNABLE, so the engine will"
					" never light it. sam_set_entity_flag(uid, \"BURNABLE\", true) first if that is"
					" what you want.");
			}
			lua_pushboolean(Ls, 0); return 1;
		}
		const bool lit = e->SetEntityOnFire(nullptr);
		if ( !lit )
		{
			SAM_WARN("LUA", "sam_set_on_fire: this creature resists fire. Skeletons and automatons"
				" never burn, and neither does anyone wearing a machinist apron or an amulet of"
				" burning resistance.");
		}
		else if ( !e->getStats() )
		{
			SAM_WARN("LUA", "sam_set_on_fire: this entity has no stats, and the burn timer only"
				" runs for players and monsters, so it will burn for ever and hurt nothing. That is"
				" fine for a brazier. Call sam_set_on_fire(uid, false) to put it out.");
		}
		lua_pushboolean(Ls, lit ? 1 : 0);
		return 1;
#else
		(void)uid; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_get_entity_flag(uid, "FLAG") -> boolean | nil
	//
	// nil, not false, for an unknown flag name. false is a real answer to "is this passable", so
	// handing it back for a typo would send the script down the "no" branch believing it had
	// asked a real question.
	//
	// Readable on a client, with the usual caveat: a client's copy of a flag is only as fresh as
	// the last update the host sent about that entity.
	int lua_sam_get_entity_flag(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* flagName = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		const int idx = samResolveEntityFlag(flagName, false, "sam_get_entity_flag");
		if ( idx < 0 ) { lua_pushnil(Ls); return 1; }
		Entity* e = samResolveEntityRead(uid, "sam_get_entity_flag");
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushboolean(Ls, e->flags[idx] ? 1 : 0);
		return 1;
#else
		(void)uid; (void)flagName; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_set_entity_flag(uid, "FLAG", on) -> boolean
	//
	// PASSABLE for a decoration nobody should bump into, BLOCKSIGHT for a prop that should cast a
	// shadow, UNCLICKABLE for scenery, BRIGHT for something that glows. The flag is REQUIRED and
	// so is the value; neither is guessed.
	//
	// Four flags are read-only through here and say why. INVISIBLE and BURNING have their own
	// functions that know rules this one does not: sam_set_visible refuses custom-bodied monsters
	// because the draw pass keeps those visible on purpose, and sam_set_on_fire has to start the
	// burn timer the flag is only the shadow of. NOUPDATE and UPDATENEEDED are the network sweep's
	// own state.
	int lua_sam_set_entity_flag(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* flagName = luaL_checkstring(Ls, 2);
		bool on = false;
		if ( !samBoolReq(Ls, 3, "sam_set_entity_flag", &on) ) { lua_pushboolean(Ls, 0); return 1; }
#ifdef SAM_LUA_HAVE_BARONY
		const int idx = samResolveEntityFlag(flagName, true, "sam_set_entity_flag");
		if ( idx < 0 ) { lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveWritable(uid, "sam_set_entity_flag");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		// The flag table is global; whether a write lasts depends on what the entity is.
		if ( !SAMMpEntities::flagSticks(e, idx, "sam_set_entity_flag") ) { lua_pushboolean(Ls, 0); return 1; }
		e->flags[idx] = on;
		// ENTF carries one flag by index and the client applies it directly, so this is the
		// whole of the sync. Without it the host alone would believe the wall is walk-through.
		if ( multiplayer == SERVER ) { serverUpdateEntityFlag(e, idx); }
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)flagName; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_entity_size(uid, size [, sizeY]) -> boolean
	//
	// The collision half-extent, in world units, 16 to a tile. It is what the engine actually
	// tests against: collision.cpp:443 compares `x + sizex > x2 - sizex`, and a size of 0 means
	// "nothing collides with this".
	//
	// CLAMPED TO 0..127, ALWAYS. sizex is a Sint32 in memory but crosses the wire as
	// `(Sint8)entity->sizex` (net.cpp:551), so 200 arrives on a client as -56 and the comparison
	// above inverts: that client believes the hitbox is inside-out, while the host sees nothing
	// wrong at all. Clamping only in multiplayer would mean a mod authored in singleplayer was
	// broken for everyone else, which is the same trap sam_set_scale shipped with.
	//
	// Unlike elevation, this one STICKS: every engine write to sizex is behind a spawn-time INIT
	// gate (actmonster.cpp:2930 is SAM's own custom-body hitbox, under MONSTER_INIT), so nothing
	// overwrites it on the next frame.
	int lua_sam_set_entity_size(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		long long sx = (long long)luaL_checkinteger(Ls, 2);
		long long sy = (long long)luaL_optinteger(Ls, 3, (lua_Integer)sx);
		Entity* e = samResolveWritable(uid, "sam_set_entity_size");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		// A remote player's own machine moves them with its own copy of this box, and only a S.A.M
		// machine can be told: refused for a player whose game is not running S.A.M.
		if ( !SAMMpEntities::sizeReachesOwner(e, "sam_set_entity_size") ) { lua_pushboolean(Ls, 0); return 1; }
		if ( sx < 0 || sx > 127 || sy < 0 || sy > 127 )
		{
			SAM_WARN("LUA", "sam_set_entity_size: sizes are clamped to 0..127, because the network"
				" carries them as one signed byte and a larger number arrives negative, which turns"
				" the hitbox inside-out on every other player's machine.");
		}
		if ( sx < 0 ) { sx = 0; } if ( sx > 127 ) { sx = 127; }
		if ( sy < 0 ) { sy = 0; } if ( sy > 127 ) { sy = 127; }
		e->sizex = (Sint32)sx;
		e->sizey = (Sint32)sy;
		e->flags[UPDATENEEDED] = true;   // ENTU carries both, but only for an entity marked dirty
		// ...and never to a client's own player or an entity the client pins: those are told directly.
		SAMMpEntities::transformed(e, SAMMpEntities::Changed::SIZE);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_set_elevation(uid, z) -> boolean
	//
	// Height, as the ENGINE means it: the same z sam_get_position_precise hands back, so the two
	// round-trip. Barony's z grows DOWNWARD -- gravity is `vel_z += 0.04; z += vel_z;`
	// (actplayer.cpp:1705) -- so NEGATIVE IS UP. Flipping the sign for comfort would have made
	// sam_set_elevation(uid, sam_get_position_precise(uid)) move the thing.
	//
	// REFUSES PLAYERS AND MONSTERS, and that is the whole reason this function is small. A
	// creature's height is rewritten from scratch every single frame by its own species code:
	// humanMoveBodyparts contains a bare `my->z = -1;` (monster_human.cpp:1058), the goatman does
	// the same (monster_g.cpp:648), the skeleton (monster_skeleton.cpp:856), the bat
	// (monster_bat.cpp:297), and actPlayer (actplayer.cpp:9393). Allowing it would return true and
	// be erased before the next frame drew -- the same silent nothing that got limbs refused, so
	// it gets refused the same way. Lifting a creature needs an effect the engine already drives
	// (EFF_LEVITATION, EFF_LIFT), not a one-off write.
	//
	// So it is for what it is genuinely for: props, ground items and spawned portals. NOT
	// companions -- their own hover curve rewrites z every tick, so they are refused with the
	// creatures. An entity a script owns through sam_register_behavior is allowed, but if that
	// script's own handler writes z it will win, for the same reason.
	int lua_sam_set_elevation(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		double z = (double)luaL_checknumber(Ls, 2);
		Entity* e = samResolveWritable(uid, "sam_set_elevation");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		if ( !std::isfinite(z) )
		{
			SAM_ERROR("LUA", "sam_set_elevation: z must be a finite number.");
			lua_pushboolean(Ls, 0); return 1;
		}
		// The COMPANION belongs in this list and was missing from it, which made this function's
		// own documentation recommend the case it silently fails on: actSamCompanion writes
		// `my->z = p->z - SAM_COMPANION_RISE + ...` unconditionally every tick, which is exactly
		// what players and monsters are refused for.
		if ( e->behavior == &actPlayer || e->behavior == &actMonster || SAMLua::isCompanionEntity(e) )
		{
			SAM_WARN("LUA", "sam_set_elevation refused: a player's, a monster's or a companion's"
				" height is rewritten every frame by code that owns it (a companion by its own"
				" hover curve), so this would be erased before the next frame drew. Use a"
				" levitation effect for a creature; a companion's float height is fixed.");
			lua_pushboolean(Ls, 0); return 1;
		}
		// The wire carries z as (Sint16)(z * 32), so |z| must stay under 1024 or it wraps and the
		// entity appears at the opposite extreme on every client. Clamped in every mode, for the
		// same reason as scale and size.
		if ( z < -1023.0 || z > 1023.0 )
		{
			SAM_WARN("LUA", "sam_set_elevation: z is clamped to -1023..1023, which is what the"
				" network can carry. Negative is up.");
			z = ( z < 0.0 ) ? -1023.0 : 1023.0;
		}
		e->z = z;
		e->new_z = z;                    // the client's interpolation target, or it slides back
		e->flags[UPDATENEEDED] = true;
		// A ground item or a prop the client pins never takes ENTU; S.A.M clients are told directly.
		SAMMpEntities::transformed(e, SAMMpEntities::Changed::HEIGHT);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_set_visible(uid, bool) -> boolean. Hides the model without touching the entity, so
	// it still collides, still acts, still exists.
	int lua_sam_set_visible(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		// Two things were wrong here and both are fixed together. lua_toboolean says TRUE for the
		// number 0 while the JS twin's JS_ToBool says FALSE, so sam_set_visible(uid, 0) meant
		// opposite things in the two runtimes; and a MISSING flag was read as "hide", which is a
		// guess. One numeric rule now, JavaScript's, and no guess at all.
		bool vis = false;
		if ( !samBoolReq(Ls, 2, "sam_set_visible", &vis) ) { lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveWritable(uid, "sam_set_visible");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		// A creature's INVISIBLE is recomputed on the host every frame from its invisibility effect,
		// and a ground item is made visible again every frame, sending nothing -- so the host undid
		// this at once while every client kept it hidden for good. Refused, with the reason (use
		// sam_apply_effect(uid, "INVISIBLE", ticks) for a creature). This also covers what the old
		// custom-body refusal here guarded: only a MONSTER's custom body is un-skipped by the draw
		// pass, and any other entity now honours INVISIBLE with a custom model (SAMBodies::hiddenByEffect).
		if ( !SAMMpEntities::visibilitySticks(e, "sam_set_visible") ) { lua_pushboolean(Ls, 0); return 1; }
		e->flags[INVISIBLE] = !vis;
		// A custom model is drawn at the draw site, not through entity->sprite, so the draw pass
		// has to be told this hide came from a script: the engine's own INVISIBLE on a limb only
		// means the slot is empty (SAMBodies::hiddenByEffect).
		SAMBodies::noteScriptVisibility((uint32_t)e->getUID(), vis);
		if ( multiplayer == SERVER ) { serverUpdateEntityFlag(e, INVISIBLE); }
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_spawn_monster(tileX, tileY, "name" [, shopType]) -> uid | nil. Whitelisted to the Monster
	// enum (the name resolves case-insensitively), or a mod-declared "ns:slug" variant. shopType
	// (0-14) only applies to "shopkeeper" and picks the store kind. Host only; net replication is
	// done by summonMonster itself (the SUMM packet). Returns the new monster's uid so scripts can
	// move or query it afterwards.
	int lua_sam_spawn_monster(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int tx = (int)luaL_checkinteger(Ls, 1);
		const int ty = (int)luaL_checkinteger(Ls, 2);
		const char* nameC = luaL_checkstring(Ls, 3);
		const std::string monName = nameC ? nameC : "";
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_spawn_monster refused: host only."); lua_pushnil(Ls); return 1; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("LUA", "sam_spawn_monster: tile (" + std::to_string(tx) + "," + std::to_string(ty) + ") out of bounds."); lua_pushnil(Ls); return 1; }

		// A name containing ':' is a mod's OWN monster; anything else is a vanilla species.
		Entity* e = nullptr;
		int creature = 0;
		if ( monName.find(':') != std::string::npos )
		{
			e = samSummonCustomVariant(monName, tx, ty);
			if ( !e )
			{
				SAM_ERROR("LUA", "sam_spawn_monster: no monster declared as '" + monName
					+ "' (check the id against the mod's monster JSON, and that the mod loaded).");
				lua_pushnil(Ls); return 1;
			}
		}
		else
		{
			creature = samMonsterNameToId(nameC);
			if ( creature <= 0 ) { SAM_ERROR("LUA", "sam_spawn_monster: unknown monster '" + monName + "'."); lua_pushnil(Ls); return 1; }
			e = summonMonster(static_cast<Monster>(creature), tx * 16 + 8, ty * 16 + 8); // pixel coords
		}
		if ( !e ) { SAM_ERROR("LUA", "sam_spawn_monster: spawn failed (blocked tile?)."); lua_pushnil(Ls); return 1; }
		if ( !lua_isnoneornil(Ls, 4) && creature == SHOPKEEPER )
		{
			int shopType = (int)luaL_optinteger(Ls, 4, 0);
			if ( shopType < 0 ) { shopType = 0; }
			if ( shopType > 14 ) { shopType = 14; }
			if ( Stat* s = e->getStats() ) { s->MISC_FLAGS[STAT_FLAG_NPC] = 1 + shopType; }
		}
		SAM_INFO("LUA", "Spawned monster " + monName + " at (" + std::to_string(tx) + "," + std::to_string(ty) + ")");
		lua_pushinteger(Ls, (lua_Integer)e->getUID());
		return 1;
	}

	// sam_get_inventory(player) -> array of { uid, type, name, count, beatitude, status,
	// identified, equipped }. Empty for an invalid player; nil for a player whose backpack
	// this machine cannot see. On the host that is every remote player whose game runs S.A.M
	// scripts: their own machine reports its backpack (sam_mp_inventory.hpp), and the uids
	// here are the host's names for those items, which every item function accepts (a change
	// is carried to the owner's machine). `name` is the vanilla internal name, or a mod item's
	// "ns:item". Reader.
	int lua_sam_get_inventory(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		std::vector<SAMMpInventory::InventoryRow> rows;
		if ( SAMMpInventory::inventoryRows(player, "sam_get_inventory", &rows) == SAMMpInventory::Seen::No )
		{
			lua_pushnil(Ls);
			return 1;
		}
		lua_newtable(Ls);
		int idx = 1;
		for ( const auto& row : rows )
		{
			lua_newtable(Ls);
			lua_pushinteger(Ls, (lua_Integer)row.uid);        lua_setfield(Ls, -2, "uid");
			lua_pushinteger(Ls, (lua_Integer)row.type);       lua_setfield(Ls, -2, "type");
			lua_pushstring(Ls, row.name.c_str());             lua_setfield(Ls, -2, "name");
			lua_pushinteger(Ls, (lua_Integer)row.count);      lua_setfield(Ls, -2, "count");
			lua_pushinteger(Ls, (lua_Integer)row.beatitude);  lua_setfield(Ls, -2, "beatitude");
			lua_pushinteger(Ls, (lua_Integer)row.status);     lua_setfield(Ls, -2, "status");
			lua_pushboolean(Ls, row.identified ? 1 : 0);      lua_setfield(Ls, -2, "identified");
			lua_pushboolean(Ls, row.equipped ? 1 : 0);        lua_setfield(Ls, -2, "equipped");
			lua_rawseti(Ls, -2, idx++);
		}
		return 1;
	}

	// sam_remove_item(itemUid) -> boolean. Removes an inventory item by its uid. Refuses
	// an EQUIPPED item (freeing it would dangle stats[p]->weapon etc. -> crash) — unequip
	// first. Consumes the whole stack. An `owner` function: the host's own items here; a
	// remote player's item (a uid from sam_get_inventory on the host) is carried to that
	// player's machine and removed there.
	int lua_sam_remove_item(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("LUA", "sam_remove_item refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		int holder = -1;
		Item* it = samItemForWrite(uid, "sam_remove_item", &holder);
		if ( !it ) { lua_pushboolean(Ls, 0); return 1; }
		// Through the queue, like sam_set_item_count. This used to free inline, which is a
		// use-after-free on the commonest mod pattern there is: player.on_item_use fires at
		// items.cpp:2955 and useItem keeps dereferencing the same pointer at :3011 and :3046.
		// The old equipped test here used itemSlot, which matches by VALUE and so refused a
		// loose item that merely looked like a worn one; queueDestroy compares pointers. On a
		// client the queue is drained by the inventory mirror's tick hook.
		const bool queued = SAMItems::queueDestroy((Uint32)it->uid, holder >= 0 ? holder : clientnum);
		if ( !queued ) { SAMMpInventory::noteOwnerRefusal("sam_remove_item", "that item is equipped; unequip it first.", false, "equipped"); }
		else { SAMMpInventory::nudgeReport(); }   // see the twin in sam_set_item_count
		lua_pushboolean(Ls, queued ? 1 : 0);
		return 1;
	}
#endif

	int lua_sam_get_monster_stat(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_get_monster_stat: no monster uid " + std::to_string(uid)); lua_pushinteger(Ls, 0); return 1; }
		Stat* s = e->getStats();
		const std::string n = samUpper(nameC);
		long long v = 0;
		if      ( n == "STR" ) { v = s->STR; }
		else if ( n == "DEX" || n == "SPEED" ) { v = s->DEX; } // no speed field; DEX drives movement
		else if ( n == "CON" ) { v = s->CON; }
		else if ( n == "INT" ) { v = s->INT; }
		else if ( n == "PER" ) { v = s->PER; }
		else if ( n == "CHR" ) { v = s->CHR; }
		else if ( n == "HP" )  { v = s->HP; }
		else if ( n == "MAXHP" ) { v = s->MAXHP; }
		else if ( n == "MP" )  { v = s->MP; }
		else if ( n == "MAXMP" ) { v = s->MAXMP; }
		else if ( n == "LEVEL" || n == "LVL" ) { v = s->LVL; }
		else { SAM_WARN("LUA", std::string("sam_get_monster_stat: unknown stat '") + (nameC ? nameC : "") + "'"); }
		lua_pushinteger(Ls, (lua_Integer)v);
		return 1;
#else
		(void)uid; (void)nameC; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_get_monster_type(uid) -> "rat" / "skeleton" / ... or nil.
	//
	// A monster's SPECIES, as the lowercase name the engine itself uses in monstertypename[].
	// Until now the only way to identify a creature from a script was the raw integer in an
	// event payload, and the docs told modders to log it once and hardcode the number.
	//
	// NOTE this is the BASE type: a custom monster is a variant of a vanilla species, so a
	// mod's "Rathalos" built on a bat answers "bat". Use sam_get_monster_name for the variant's
	// own name, or sam_monster_has_trait to tell modded creatures apart.
	int lua_sam_get_monster_type(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushnil(Ls); return 1; }
		const int t = (int)e->getStats()->type;
		if ( t < 0 || t >= NUMMONSTERS ) { lua_pushnil(Ls); return 1; }
		lua_pushstring(Ls, monstertypename[t]);
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_monster_name(uid) -> the creature's DISPLAY name, or nil.
	//
	// For a mod's custom monster this is the variant name it was given ("Rathalos"). A plain
	// vanilla creature carries an empty variant name, so fall back to the species name and
	// never hand a script an empty string.
	int lua_sam_get_monster_name(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushnil(Ls); return 1; }
		Stat* st = e->getStats();
		if ( st->name[0] ) { lua_pushstring(Ls, st->name); return 1; }
		const int t = (int)st->type;
		if ( t < 0 || t >= NUMMONSTERS ) { lua_pushnil(Ls); return 1; }
		lua_pushstring(Ls, monstertypename[t]);
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// ---- monster movement -------------------------------------------------------------
	//
	// Barony has no per-species AI: actMonster is ONE generic state machine that every
	// creature runs, with inline exceptions for particular types. So "write your own AI" does
	// not mean replacing a brain, it means being able to STEER the shared one. These three
	// bindings are that steering, and they are deliberately thin wrappers over machinery the
	// engine already has rather than a parallel movement system.
	//
	// All coordinates are TILE coordinates, matching sam_get_position.

	// sam_monster_path_to(uid, tileX, tileY) -> boolean
	// Ask the engine to path a monster to a tile using its real pathfinder, then put it in the
	// hunt state so the existing follow logic walks the path. Returns false if no path exists.
	int lua_sam_monster_path_to(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int tx = (int)luaL_checkinteger(Ls, 2);
		const int ty = (int)luaL_checkinteger(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_monster_path_to refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		// adjacentTilesToCheck = 1 so a blocked exact tile still finds a neighbour, which is what
		// the vanilla ally-follow calls do.
		const bool ok = e->monsterSetPathToLocation(tx, ty, 1,
			GeneratePathTypes::GENERATE_PATH_PLAYER_ALLY_MOVETO);
		if ( ok ) { e->monsterState = MONSTER_STATE_HUNT; }
		lua_pushboolean(Ls, ok ? 1 : 0);
		return 1;
#else
		(void)uid; (void)tx; (void)ty; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_monster_face(uid, tileX, tileY) -> boolean. Turn a monster to look at a tile.
	int lua_sam_monster_face(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int tx = (int)luaL_checkinteger(Ls, 2);
		const int ty = (int)luaL_checkinteger(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_monster_face refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		// Tile centre, so facing a tile does not aim at its corner.
		const real_t wx = (real_t)(tx * 16 + 8);
		const real_t wy = (real_t)(ty * 16 + 8);
		e->yaw = atan2(wy - e->y, wx - e->x);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)tx; (void)ty; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_monster_attack(uid) -> boolean. Make a monster swing now, using whatever pose its
	// current weapon calls for (getAttackPose is what the engine's own melee path uses).
	int lua_sam_monster_attack(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_monster_attack refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		e->attack(e->getAttackPose(), 0, nullptr);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_monster_charge(uid, ticks) -> boolean
	//
	// Send a monster into a straight-line charge for `ticks`. This drives
	// MONSTER_STATE_GENERIC_CHARGE, a fully implemented behaviour that was DEAD CODE: the state
	// is handled in actMonster but nothing in the engine ever set it. It aims at the monster's
	// current target if it has line of sight, otherwise it charges along its current facing --
	// so pair it with sam_monster_face to aim a charge wherever you like.
	//
	// Self-terminating: it stops on its own when the timer runs out OR the moment it hits
	// anything, so a script cannot wedge a monster by charging it into a wall.
	int lua_sam_monster_charge(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		int ticks = (int)luaL_optinteger(Ls, 2, 50);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_monster_charge refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		if ( ticks < 1 ) { ticks = 1; }
		if ( ticks > 500 ) { ticks = 500; }   // ~10s ceiling; a charge is a dash, not a mode
		e->monsterState = MONSTER_STATE_GENERIC_CHARGE;
		e->monsterSpecialTimer = ticks;
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)ticks; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_monster_has_effect(uid, "EFFECT") -> boolean. The monster counterpart of
	// sam_has_effect — e.g. "when a monster takes damage AND it has POISONED".
	int lua_sam_monster_has_effect(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { SAM_WARN("LUA", std::string("sam_monster_has_effect: unknown effect '") + (nameC ? nameC : "") + "'. Valid: " + samEffectNameHint()); lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, e->getStats()->getEffectActive(eff) != 0 ? 1 : 0);
		return 1;
#else
		(void)uid; (void)nameC; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_monster_has_trait(uid, "undead") -> bool. Reads back what the mod declared in
	// JSON. Without this a mod can SAY a monster is undead and the engine will agree, but
	// the mod's own script can't ask -- so a "bonus vs undead" rule had no way to test for
	// undead. False for every vanilla monster (mask is 0), so it is a no-op without a mod.
	int lua_sam_monster_has_trait(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		const unsigned long long bit = SAMMonsters::traitBitForName(nameC);
		if ( bit == 0 )
		{
			SAM_WARN("LUA", std::string("sam_monster_has_trait: unknown trait '") + (nameC ? nameC : "")
				+ "'. Valid: boss, trader, untargetable, immobile_turret, never_retreat, "
				  "water_walking, undead, ally_recolour, tinker_construct, no_digestion, pass_through.");
			lua_pushboolean(Ls, 0); return 1;
		}
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		Stat* st = e->getStats();
		if ( !st ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, samMonsterHasTrait(st, bit) ? 1 : 0);
		return 1;
#else
		(void)uid; (void)nameC; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// The ten monster equipment slots (stat.hpp:456-465). Same vocabulary the monster
	// schema's equipped_items uses, so an author does not learn a second set of names.
	Item** samMonsterSlot(Stat* st, const std::string& slotIn)
	{
		if ( !st ) { return nullptr; }
		std::string s = slotIn;
		for ( char& c : s ) { c = (char)std::tolower((unsigned char)c); }
		if ( s == "helmet" || s == "helm" )        { return &st->helmet; }
		if ( s == "breastplate" || s == "armor" )  { return &st->breastplate; }
		if ( s == "gloves" )                       { return &st->gloves; }
		if ( s == "shoes" || s == "boots" )        { return &st->shoes; }
		if ( s == "shield" )                       { return &st->shield; }
		if ( s == "weapon" )                       { return &st->weapon; }
		if ( s == "cloak" )                        { return &st->cloak; }
		if ( s == "amulet" )                       { return &st->amulet; }
		if ( s == "ring" )                         { return &st->ring; }
		if ( s == "mask" )                         { return &st->mask; }
		return nullptr;
	}
	const char* samMonsterSlotNames()
	{
		return "helmet, breastplate, gloves, shoes, shield, weapon, cloak, amulet, ring, mask";
	}

	// sam_monster_equip(uid, "slot", "item" [, beatitude [, status [, count]]]) -> boolean.
	// Puts a real Item into a live monster's equipment slot: it is worn, it is used in
	// combat, and it drops when the monster dies. Whatever was in the slot is dropped on
	// the floor by monsterEquipItem rather than leaked.
	// sam_set_monster_name(uid, "Snivelwick the Twice-Fed") -> boolean.
	//
	// Stat::name is a fixed char[128] (stat.hpp:339), so this bound-copies; it never strcpy's
	// a script string into it. Two things worth knowing before you use it:
	//
	//  * HOST-SIDE, and that is enough for enemies: the host's enemy bar and combat messages carry
	//    the new name to every player ('ENHP', 'MSGS'). A FOLLOWER's name is drawn on its leader's
	//    own machine from a copy the engine sends only when it is recruited, so the new name is
	//    carried there too (S.A.M's channel; a leader without S.A.M keeps the old name).
	//  * Barony treats some names as GENERIC (entity.cpp:29221): a name containing "lesser",
	//    "young", "enslaved", "damaged", "corrupted", "cultist" or "encased" makes the engine
	//    fall back to the species name. That is vanilla behaviour, not a bug here, but it
	//    will look like one if your epithet table happens to contain those words.
	int lua_sam_set_monster_name(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		const std::string newName = nameC ? nameC : "";
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_monster_name refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_set_monster_name: no monster uid " + std::to_string(uid)); lua_pushboolean(Ls, 0); return 1; }
		Stat* st = e->getStats();
		if ( !st ) { lua_pushboolean(Ls, 0); return 1; }
		stringCopy(st->name, newName.c_str(), sizeof(st->name), newName.size());
		SAMMonsters::syncFollowerName(e);   // a follower's owner, on its own machine
		lua_pushboolean(Ls, 1);
		return 1;
	}

	int lua_sam_monster_equip(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* slotC = luaL_checkstring(Ls, 2);
		const char* itemC = luaL_checkstring(Ls, 3);
		const std::string itemName = itemC ? itemC : "";
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_monster_equip refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_monster_equip: no monster uid " + std::to_string(uid)); lua_pushboolean(Ls, 0); return 1; }
		Stat* st = e->getStats();
		Item** slot = samMonsterSlot(st, slotC ? slotC : "");
		if ( !slot )
		{
			SAM_ERROR("LUA", std::string("sam_monster_equip: unknown slot '") + (slotC ? slotC : "")
				+ "'. Valid: " + samMonsterSlotNames());
			lua_pushboolean(Ls, 0); return 1;
		}
		// Same two-step resolution as sam_grant_item: a custom "ns:item" first, then a
		// vanilla name, so modded gear works exactly like vanilla gear.
		int resolvedType = -1;
		if ( itemName.find(':') != std::string::npos ) { resolvedType = SAMItems::itemIdForIdString(itemName); }
		// One resolver, shared with every other name-taking call: digits, "ns:id", the internal
		// name, then the DISPLAYED name -- which is what sam_list_items and sam_get_container_items
		// hand out, and what this used to refuse.
		if ( resolvedType < 0 ) { resolvedType = SAMCatalog::itemTypeFor(itemName); }
		if ( resolvedType < 0 )
		{
			SAM_ERROR("LUA", "sam_monster_equip: unknown item '" + itemName
				+ "' (expected a vanilla name like \"IRON_DAGGER\" or a custom \"namespace:item\").");
			lua_pushboolean(Ls, 0); return 1;
		}
		const Sint16 beatitude = (Sint16)luaL_optinteger(Ls, 4, 0);
		int statusArg = (int)luaL_optinteger(Ls, 5, (int)EXCELLENT);
		statusArg = samClampInt(statusArg, (int)BROKEN, (int)EXCELLENT);
		int count = (int)luaL_optinteger(Ls, 6, 1);
		if ( count < 1 ) { count = 1; }
		Item* item = newItem(static_cast<ItemType>(resolvedType), static_cast<Status>(statusArg),
			beatitude, count, 0, true, nullptr);
		if ( !item ) { lua_pushboolean(Ls, 0); return 1; }
		e->monsterEquipItem(*item, slot);
		SAM_INFO("SAM", "sam_monster_equip: " + itemName + " -> " + std::string(slotC ? slotC : "")
			+ " on uid " + std::to_string(uid));
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_monster_unequip(uid, "slot") -> boolean. Drops what is in the slot on the floor,
	// the same way equipping over it would.
	int lua_sam_monster_unequip(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* slotC = luaL_checkstring(Ls, 2);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_monster_unequip refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		Stat* st = e->getStats();
		Item** slot = samMonsterSlot(st, slotC ? slotC : "");
		if ( !slot )
		{
			SAM_ERROR("LUA", std::string("sam_monster_unequip: unknown slot '") + (slotC ? slotC : "")
				+ "'. Valid: " + samMonsterSlotNames());
			lua_pushboolean(Ls, 0); return 1;
		}
		if ( !*slot ) { lua_pushboolean(Ls, 0); return 1; }
		// The whole stack. dropItemMonster drops ONE unit unless told the count (items.cpp:2271),
		// and clearing the slot after it orphaned the rest of a stack of throwing weapons. With
		// the full count it empties the stack, clears the slot and frees the item itself; the
		// slot is cleared here as well because its own slot lookup matches by value.
		dropItemMonster(*slot, e, st, (*slot)->count);
		*slot = nullptr;
		lua_pushboolean(Ls, 1);
		return 1;
	}

	int lua_sam_set_monster_stat(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		const int value = (int)luaL_checkinteger(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_monster_stat refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_set_monster_stat: no monster uid " + std::to_string(uid)); lua_pushboolean(Ls, 0); return 1; }
		Stat* s = e->getStats();
		const std::string n = samUpper(nameC);
		if      ( n == "HP" )    { e->setHP(value); }
		// Bounded by the 16-bit enemy HP bar every client draws ('ENHP'). The clamp goes through
		// setHP, which also tells an ally's owner; syncFollowerSheet carries the new max and level
		// to the ally panel of a follower whose leader is on another machine.
		else if ( n == "MAXHP" ) { s->MAXHP = samClampInt(value, 1, SAMLua::STAT_WIRE_MAX); if ( s->HP > s->MAXHP ) { e->setHP(s->MAXHP); } SAMMonsters::syncFollowerSheet(e); }
		else if ( n == "MP" )    { e->setMP(value); }
		else if ( n == "MAXMP" ) { s->MAXMP = (value < 0 ? 0 : value); if ( s->MP > s->MAXMP ) { s->MP = s->MAXMP; } }
		else if ( n == "STR" )   { s->STR = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "DEX" || n == "SPEED" ) { s->DEX = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "CON" )   { s->CON = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "INT" )   { s->INT = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "PER" )   { s->PER = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "CHR" )   { s->CHR = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "LEVEL" || n == "LVL" ) { s->LVL = samClampInt(value, 1, 255); SAMMonsters::syncFollowerSheet(e); }
		else { SAM_WARN("LUA", std::string("sam_set_monster_stat: unknown stat '") + (nameC ? nameC : "") + "'"); lua_pushboolean(Ls, 0); return 1; }
		SAM_INFO("SAM", "sam_set_monster_stat: " + n + "=" + std::to_string(value) + " on uid " + std::to_string(uid));
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)nameC; (void)value; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	int lua_sam_apply_monster_effect(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		const int ticks = (int)luaL_checkinteger(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_apply_monster_effect refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_apply_monster_effect: no monster uid " + std::to_string(uid)); lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { SAM_WARN("LUA", std::string("sam_apply_monster_effect: unknown effect '") + (nameC ? nameC : "") + "'"); lua_pushboolean(Ls, 0); return 1; }
		const bool ok = e->setEffect(eff, true, ticks, true);
		SAM_INFO("SAM", std::string("sam_apply_monster_effect: ") + (nameC ? nameC : "") + " to uid " + std::to_string(uid) + (ok ? "" : " (immune)"));
		lua_pushboolean(Ls, ok ? 1 : 0);
		return 1;
#else
		(void)uid; (void)nameC; (void)ticks; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// ---- v1.5.0: monster status-effect read/remove parity (players already had these) ------

	// sam_remove_monster_effect(uid, "EFFECT") -> bool. Clear a status effect from a monster by
	// uid. Host-authoritative. The monster counterpart of sam_remove_effect.
	int lua_sam_remove_monster_effect(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_remove_monster_effect refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_remove_monster_effect: no monster uid " + std::to_string(uid)); lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { SAM_WARN("LUA", std::string("sam_remove_monster_effect: unknown effect '") + (nameC ? nameC : "") + "'"); lua_pushboolean(Ls, 0); return 1; }
		e->setEffect(eff, false, 0, true);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)nameC; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_get_monster_effect_duration(uid, "EFFECT") -> remaining ticks (0 inactive, -1 permanent).
	int lua_sam_get_monster_effect_duration(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushinteger(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 || e->getStats()->getEffectActive(eff) == 0 ) { lua_pushinteger(Ls, 0); return 1; }
		lua_pushinteger(Ls, (lua_Integer)e->getStats()->EFFECTS_TIMERS[eff]);
		return 1;
#else
		(void)uid; (void)nameC; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_get_monster_effect_strength(uid, "EFFECT") -> strength/tier (0 if inactive).
	int lua_sam_get_monster_effect_strength(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushinteger(Ls, 0); return 1; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { lua_pushinteger(Ls, 0); return 1; }
		lua_pushinteger(Ls, (lua_Integer)e->getStats()->getEffectActive(eff));
		return 1;
#else
		(void)uid; (void)nameC; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_get_monster_effects(uid) -> array of { name, ticks, strength } for every active effect
	// on the monster (custom slots reported as "CUSTOM:<id>"). The monster twin of sam_get_effects.
	int lua_sam_get_monster_effects(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		lua_newtable(Ls);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return 1; }
		Stat* s = e->getStats();
		int n = 0;
		auto pushEntry = [&](const std::string& name, int id, Uint8 strength) {
			lua_newtable(Ls);
			lua_pushstring(Ls, name.c_str());                        lua_setfield(Ls, -2, "name");
			lua_pushinteger(Ls, (lua_Integer)s->EFFECTS_TIMERS[id]); lua_setfield(Ls, -2, "ticks");
			lua_pushinteger(Ls, (lua_Integer)strength);             lua_setfield(Ls, -2, "strength");
			lua_rawseti(Ls, -2, ++n);
		};
		for ( int id = 0; id < NUMEFFECTS; ++id )   // see sam_get_effects: the JS twin's naming
		{
			const Uint8 st = s->getEffectActive(id);
			if ( st == 0 ) { continue; }
			std::string name = SAMLua::effectNameFromId(id);
			if ( name.empty() ) { name = "CUSTOM:" + std::to_string(id); }
			pushEntry(name, id, st);
		}
		return 1;
#else
		(void)uid; return 1;
#endif
	}

	int lua_sam_kill_monster(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_kill_monster refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_kill_monster: no monster uid " + std::to_string(uid)); lua_pushboolean(Ls, 0); return 1; }
		e->setHP(0); // actMonster runs death + drops on its next tick; fires on_monster_died
		SAM_INFO("SAM", "sam_kill_monster: uid " + std::to_string(uid));
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// ==========================================================================
	//  v1.4.0 — floating "companion" entity (a JoJo-style Stand / familiar).
	//  Renders a registered custom .vox model, trails its owner player a short
	//  distance behind with a gentle hover, and thrusts forward on demand (the
	//  punch motion). Adds a brand-new behavior function and touches no vanilla
	//  code path — a pure no-op unless a mod calls sam_spawn_companion.
	// ==========================================================================
#ifdef SAM_LUA_HAVE_BARONY
	// The follow distances and the punch curve live in SAMMpEntities::companionStep, which the host
	// behaviour below and a S.A.M client's own animator both run, so the two can never drift.
	constexpr int    SAM_COMPANION_PUNCH_TICKS = SAMMpEntities::COMPANION_PUNCH_TICKS; // one forward-thrust window
	constexpr double SAM_COMPANION_RISE        = SAMMpEntities::COMPANION_RISE;        // float height above owner (z is neg-up)

	// Per-frame follow behavior (host-authoritative). Skill layout — none aliased to any
	// vanilla use (the portal marker is skill[19]==1; a companion is skill[19]==2):
	//   skill[17] = owner player index; skill[2] is -7, the client behaviour code (SAMMpEntities::adopt)
	//   skill[18] = punch ticks remaining (0 = idle behind; >0 = thrusting forward)
	//   skill[19] = 2 (S.A.M companion marker)   fskill[0] = hover-bob phase
	void samCompanionBehavior(Entity* my)
	{
		my->flags[PASSABLE] = true;   // never collide with anything
		my->flags[BRIGHT]   = true;   // full-bright so the Stand reads in a dark dungeon
		const int owner = my->skill[17];
		if ( owner < 0 || owner >= MAXPLAYERS || !players[owner] || !players[owner]->entity )
		{
			// Owner gone (died / descended a level) -> despawn. Self-remove is safe: the
			// entity act loop advances a saved next-node first, exactly as spell missiles do.
			if ( my->mynode ) { list_RemoveNode(my->mynode); }
			return;
		}
		// Replicated since the multiplayer overhaul: spawnCompanion hands it to SAMMpEntities::adopt
		// (UPDATENEEDED, a client code, the model by name). A S.A.M client runs this same step from
		// its owner's entity; the host sends ENTS 18 when a punch starts.
		SAMMpEntities::companionStep(my, players[owner]->entity);
	}

	// ---- v2.0 script-owned entity behaviour ------------------------------------------------
	//
	// Slot layout for an entity whose brain is a script (marker 4, alongside portal=1,
	// companion=2, projectile=3):
	//   skill[19] = 4                  S.A.M scripted-behaviour marker
	//   skill[18] = behaviour index     into g_behaviors
	//   skill[17] = owning player, or -1
	//   skill[2]  = -7, the client behaviour code (SAMMpEntities::adopt) -- not the script's
	// the rest of skill[0..15] is left entirely to the script as per-entity scratch, which is why the
	// marker lives at the top of the range rather than the bottom.
	constexpr int SAM_SCRIPTED_MARKER = 4;

	void samScriptedBehavior(Entity* my)
	{
		// Host-authoritative like every other framework behaviour: a client running its own
		// copy would disagree with the host about the result and desync.
		if ( multiplayer == CLIENT ) { return; }
		if ( !my ) { return; }
		SAMLua::runBehavior(my->skill[18], (unsigned long long)my->getUID());
	}

	// ---- v1.11.0 custom projectiles -------------------------------------------------------
	//
	// Until now the only thing a script could launch was a fixed vanilla spell. There was no
	// way to fire something with its own speed, model, damage and lifetime, which ruled out
	// ranged enemies with real attack patterns, bosses with telegraphed volleys, weapons that
	// fire anything other than an arrow, and traps.
	//
	// Skill layout, sharing the marker space already used by portals (skill[19]==1) and
	// companions (skill[19]==2):
	//   skill[19] = 3   S.A.M projectile marker
	//   skill[2]  = -7, the client behaviour code (SAMMpEntities::adopt); who fired it is `parent`
	//   skill[17] = damage to deal on contact
	//   skill[18] = ticks of life remaining
	//   fskill[2] / fskill[3] = velocity per tick, in world pixels
	constexpr int SAM_PROJECTILE_MARKER = 3;

	void samProjectileBehavior(Entity* my)
	{
		// Host-authoritative, like every other framework behavior: a client would otherwise
		// simulate its own copy and the two would disagree about what got hit.
		if ( multiplayer == CLIENT ) { return; }

		my->flags[PASSABLE] = true;   // we resolve our own collisions via clipMove

		if ( my->skill[18] <= 0 )
		{
			if ( my->mynode ) { list_RemoveNode(my->mynode); }
			return;
		}
		--my->skill[18];

		const real_t vx = my->fskill[2];
		const real_t vy = my->fskill[3];

		// clipMove advances x/y and returns how far it actually got, filling the global `hit`
		// with whatever stopped it -- the same call actArrow uses (actarrow.cpp:434).
		const hit_t savedHit = hit;
		const real_t want = sqrt(vx * vx + vy * vy);
		const real_t got = clipMove(&my->x, &my->y, vx, vy, my);
		const bool blocked = ( got != want );
		Entity* struck = hit.entity;
		const real_t hx = my->x, hy = my->y;
		hit = savedHit;   // never leave the engine's global perturbed by our move

		if ( !blocked ) { return; }

		// Do not let a shot immediately kill itself on the thing that fired it.
		if ( struck && my->parent != 0 && struck->getUID() == (Uint32)my->parent )
		{
			return;
		}

		const int dmg = my->skill[17];
		Uint32 targetUid = 0;
		if ( struck )
		{
			targetUid = struck->getUID();
			Stat* hitstats = struck->getStats();
			if ( dmg > 0 && hitstats )
			{
				Entity* owner = my->parent ? uidToEntity((Sint32)my->parent) : nullptr;
				struck->modHP(-dmg);
				// modHP installs a generic "mysterious causes" death message, so say what
				// actually happened whether or not the shooter is still around -- an
				// unowned or monster-fired shot used to leave the victim's death unexplained.
				struck->setObituary("was shot down.");

				// Attribute the hit the way the engine attributes an arrow (actarrow.cpp:1272
				// and :1288). Without this a projectile kill granted NO experience, no
				// compendium credit, and left the victim placidly unaware of who shot it --
				// so a mod's ranged enemy could be farmed for free, and a mod's ranged weapon
				// levelled nothing.
				if ( owner )
				{
					if ( hitstats->HP <= 0 )
					{
						owner->awardXP(struck, true, true);
					}
					else if ( struck->behavior == &actMonster )
					{
						// The trailing type test is the engine's boss guard, applied by every
						// other damage source in the game (actarrow.cpp:1286 and eleven more).
						// Bosses between LICH and SHOPKEEPER run their own scripted state
						// machines, and forcing an attack target yanks them out of it -- so a
						// mod projectile could break a Lich, Minotaur or Devil fight.
						if ( struck->monsterAlertBeforeHit(owner)
							&& struck->monsterState != MONSTER_STATE_ATTACK
							&& ( hitstats->type < LICH || hitstats->type >= SHOPKEEPER ) )
						{
							struck->monsterAcquireAttackTarget(*owner, MONSTER_STATE_PATH, true);
						}
					}
				}
			}
		}

		// Tell the script what happened before the entity goes away, so a handler can spawn a
		// follow-up (a burst, an explosion) at the point of impact.
		//
		// The handler is HANDED OUR OWN UID, so calling sam_remove_entity on it is a normal
		// thing for a mod to do -- and doing so frees this very entity while we are still
		// inside its behavior. Remember the uid across the call and re-resolve afterwards
		// rather than touching `my` again: reading my->mynode on a freed entity is a
		// use-after-free, and removing it twice corrupts the entity list.
		const Uint32 selfUid = my->getUID();
		SAMLua::dispatchProjectileHit((unsigned long long)selfUid,
			(unsigned long long)targetUid, (int)(hx / 16.0), (int)(hy / 16.0), dmg);

		Entity* self = uidToEntity((Sint32)selfUid);
		if ( self && self->mynode ) { list_RemoveNode(self->mynode); }
	}
#endif // SAM_LUA_HAVE_BARONY

	// sam_spawn_companion(player, model_id [, scale]) -> uid | nil. Spawns a floating
	// companion that renders a registered custom .vox model and trails the player, ready to
	// thrust forward on sam_companion_punch. Remove it with sam_remove_entity. Host only.
	int lua_sam_spawn_companion(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* modelC = luaL_checkstring(Ls, 2);
		const double scale = (double)luaL_optnumber(Ls, 3, 1.0);
		const unsigned long long uid = SAMLua::spawnCompanion(player, modelC ? modelC : "", scale);
		if ( uid == 0 ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)uid);
		return 1;
	}

	// sam_companion_punch(uid) -> bool. Trigger the forward punch thrust on a companion.
	// Calling it repeatedly (e.g. on a fast timer) reads as a continuous ORA-ORA flurry.
	int lua_sam_companion_punch(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		lua_pushboolean(Ls, SAMLua::companionPunch((unsigned long long)uid) ? 1 : 0);
		return 1;
	}

	// sam_get_facing(player) -> yaw radians [0,2PI) | nil. 0 = +x (east), increasing toward
	// +y; forward unit vector is (cos yaw, sin yaw). Host-authoritative for remote players.
	int lua_sam_get_facing(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const double yaw = SAMLua::getFacing(player);
		if ( yaw < 0.0 ) { lua_pushnil(Ls); return 1; }
		lua_pushnumber(Ls, (lua_Number)yaw);
		return 1;
	}

	// sam_screen_flash(player, r, g, b [, intensity=1.0] [, duration_ms=180]) -> bool.
	// Flash player's whole screen in an RGB colour that fades to nothing over duration_ms —
	// the anime "impact frame". intensity 0..1 is the peak opacity. Drawn on the machine the
	// player lives on. Returns true if accepted (valid player, engine build).
	int lua_sam_screen_flash(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player   = (int)luaL_checkinteger(Ls, 1);
		const int r        = (int)luaL_checkinteger(Ls, 2);
		const int g        = (int)luaL_checkinteger(Ls, 3);
		const int b        = (int)luaL_checkinteger(Ls, 4);
		const double inten = (double)luaL_optnumber(Ls, 5, 1.0);
		const int ms       = (int)luaL_optinteger(Ls, 6, 180);
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] )
		{ lua_pushboolean(Ls, 0); return 1; }
		SAMLua::triggerScreenFlash(player, r, g, b, inten, ms, 0, 0); // style 0 = plain fill
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)player; (void)r; (void)g; (void)b; (void)inten; (void)ms;
		lua_pushboolean(Ls, 0);
		return 1;
#endif
	}

	// sam_impact_frame(player, r, g, b [, intensity=1.0] [, duration_ms=220] [, lines=110]) -> bool.
	// The EXAGGERATED, anime version of the flash: a colour pop PLUS manga speed lines that
	// converge on the screen centre PLUS a bright core flare. Pair it with sam_camera_shake +
	// sam_hitstop for a full "impact frame". `lines` is the speed-line count (0 = a plain flash).
	int lua_sam_impact_frame(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player   = (int)luaL_checkinteger(Ls, 1);
		const int r        = (int)luaL_checkinteger(Ls, 2);
		const int g        = (int)luaL_checkinteger(Ls, 3);
		const int b        = (int)luaL_checkinteger(Ls, 4);
		const double inten = (double)luaL_optnumber(Ls, 5, 1.0);
		const int ms       = (int)luaL_optinteger(Ls, 6, 220);
		const int lines    = (int)luaL_optinteger(Ls, 7, 110);
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] )
		{ lua_pushboolean(Ls, 0); return 1; }
		SAMLua::triggerScreenFlash(player, r, g, b, inten, ms, 1, lines); // style 1 = manga burst
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)player; (void)r; (void)g; (void)b; (void)inten; (void)ms; (void)lines;
		lua_pushboolean(Ls, 0);
		return 1;
#endif
	}

	// sam_camera_shake(player, magnitude) -> bool. Shake player's camera; magnitude ~1..20
	// (1 = a nudge, ~10 = a solid hit, 20+ = violent). Feeds Barony's own shake channels, so
	// it decays naturally. For a remote client the host forwards it over the 'SHAK' packet.
	int lua_sam_camera_shake(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player  = (int)luaL_checkinteger(Ls, 1);
		const double mag  = (double)luaL_checknumber(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] )
		{ lua_pushboolean(Ls, 0); return 1; }
		SAMLua::triggerCameraShake(player, mag);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)player; (void)mag;
		lua_pushboolean(Ls, 0);
		return 1;
#endif
	}

	// sam_hitstop(duration_ms) -> bool. Briefly freeze enemy/projectile logic (a freeze-frame)
	// for duration_ms (capped ~400). Player, HUD weapon and hand-magic keep animating, so it
	// reads as a punchy impact beat. SINGLEPLAYER ONLY (returns false in multiplayer).
	int lua_sam_hitstop(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int ms = (int)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer != SINGLE )
		{
			SAMNet::warnOnce("sam_hitstop|singleplayer", "sam_hitstop: singleplayer only, so it did nothing."
				" A freeze-frame would stop the host's monsters while every client's kept moving.");
			lua_pushboolean(Ls, 0); return 1;
		}
		SAMLua::triggerHitstop(ms);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)ms;
		lua_pushboolean(Ls, 0);
		return 1;
#endif
	}

	// sam_spawn_portal(tileX, tileY) -> uid | nil. Creates a purely-DECORATIVE, walkable
	// portal (the swirling vortex, sprite 254) at a tile: it animates and glows purple but
	// is never interactive and never sends anyone to the next floor (see the skill[15]
	// guard in actPortal). Returns the new entity's uid so a script can move it
	// (sam_set_position) or clear it (sam_remove_entity). Host only.
	int lua_sam_spawn_portal(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int tx = (int)luaL_checkinteger(Ls, 1);
		const int ty = (int)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_spawn_portal refused: host only."); lua_pushnil(Ls); return 1; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("LUA", "sam_spawn_portal: tile (" + std::to_string(tx) + "," + std::to_string(ty) + ") out of bounds."); lua_pushnil(Ls); return 1; }
		Entity* e = newEntity(254, 1, map.entities, nullptr);
		if ( !e ) { SAM_ERROR("LUA", "sam_spawn_portal: entity creation failed."); lua_pushnil(Ls); return 1; }
		e->x = tx * 16 + 8;                 // tile centre, pixel coords
		e->y = ty * 16 + 8;
		e->z = 0;
		e->sprite = 254;                    // portal swirl
		e->sizex = 4;
		e->sizey = 4;
		e->yaw = 1.5707963267948966;        // PI/2, matching a real portal's facing
		e->flags[PASSABLE] = true;          // walkable, so a player can stand on it
		e->behavior = &actPortal;
		e->skill[19] = 1;                   // S.A.M decorative marker (guard in actPortal); skill[19]/[20] are outside the portal alias range
		// Every player sees it: UPDATENEEDED, and a portal code on the ENTU tail so a S.A.M client
		// runs actPortal's inert decorative path. A stock client binds it by sprite 254 and runs the
		// vanilla portal (tooltip, ambience); stepping in is decided here, where skill[19] keeps it inert.
		SAMMpEntities::adopt(e, SAMMpEntities::Kind::Portal);
		SAM_INFO("LUA", "sam_spawn_portal: decorative portal at (" + std::to_string(tx) + "," + std::to_string(ty) + ") uid " + std::to_string(e->getUID()));
		lua_pushinteger(Ls, (lua_Integer)e->getUID());
		return 1;
#else
		(void)tx; (void)ty; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_remove_entity(uid) -> bool. Remove a non-player world entity by uid — a
	// sam_spawn_portal marker, a spawned monster, a ground item, etc. Refuses players
	// (use the normal death/teleport paths for those). Frees any light it owned. Host only.
	int lua_sam_remove_entity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		// QUEUED, not freed here. This used to call list_RemoveNode inline, which frees the
		// Entity -- and a script only runs because the engine called into it. Entity::attack
		// calls modHP and then keeps dereferencing hit.entity for another ~160 lines; modHP is
		// what fires the damage events a mod handles. So a three-line mod that removes the
		// monster it was just told about freed it out from under the attack in progress.
		// The removal happens on the next frame instead; sam_get_entity_type still resolves the
		// uid in between.
		lua_pushboolean(Ls, samQueueRemoveEntity((Uint32)uid, "sam_remove_entity") ? 1 : 0);
		return 1;
#else
		(void)uid; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// ===== v1.11.0 persistent world state ==================================================
	//
	// Barony throws floors away: every level is regenerated from `mapseed`, and no map
	// content is written to the save at all. That ruled out a whole family of mods -- a
	// home base you return to, a bank, anything that remembers what you did two floors ago.
	//
	// Rather than bolt a second save system onto the side of the game (which is how saves
	// get corrupted), these bindings expose the places the ENGINE already persists things
	// correctly:
	//
	//   stash -> the void chest's inventory, written into the savegame with full item
	//            fidelity and read back on load.
	//   hub   -> the level-travel globals, which already move a party to any floor.
	//   flags -> SaveGameInfo::additional_data, already saved and already hashed.

	// sam_set_chest_stash(chest_uid [, on]) -> bool
	//
	// Turns a chest the mod already placed into permanent storage. Its contents then live
	// in the player's savegame instead of on the floor, so they survive descending, dying
	// on a later floor, quitting, and loading again. This is the game's own void chest: the
	// GUI, the networking and the save round-trip are all vanilla, and we only flip which
	// list the chest reads from.
	//
	// Contents are shared by every stash chest in a run (there is one storage list) and the
	// chest window holds 12 stacks. That makes this a stash, not a bank.
	int lua_sam_set_chest_stash(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		// samBoolArg, not lua_toboolean: 0 is TRUE to Lua and FALSE to JavaScript, so
		// sam_set_chest_stash(uid, 0) meant opposite things in the two runtimes.
		const bool on = samBoolArg(Ls, 2, true);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_chest_stash refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveEntityQuiet((long long)uid);
		if ( !e ) { SAM_WARN("LUA", "sam_set_chest_stash: no entity with uid " + std::to_string(uid) + "."); lua_pushboolean(Ls, 0); return 1; }
		if ( e->behavior != &actChest )
		{
			SAM_WARN("LUA", "sam_set_chest_stash: uid " + std::to_string(uid) + " is not a chest.");
			lua_pushboolean(Ls, 0); return 1;
		}
		// -1 rather than a positive count: the engine only counts this timer down while it
		// is > 0, but every "is this void storage" test is != 0. So -1 never expires and
		// still reads correctly at every site. One behavioural difference is deliberate --
		// vanilla drops a void chest's items when it is smashed, by resetting the state to
		// 0 first, and that reset is also `> 0`. A permanent stash therefore keeps its
		// contents instead of spilling the shared storage list onto the floor.
		// Two things must be handled before the routing is flipped underneath the chest.
		//
		// 1. If the chest is OPEN right now, its window is already bound to the old item
		//    list. Swapping which list the chest reads from while that window is up leaves
		//    the player looking at contents the chest no longer owns. Close it first --
		//    closeChest() is a no-op when nobody has it open.
		if ( e->chestStatus ) { e->closeChest(); }
		//
		// 2. A chest that already holds loot does not lose it, but that loot becomes
		//    unreachable for as long as the chest is a stash (its own list is still there,
		//    the chest is simply reading the shared storage list instead). That is
		//    recoverable and turning the stash back off restores it, but it looks exactly
		//    like the items were deleted -- so say so rather than let a modder guess.
		if ( on && e->chestVoidState == 0 )
		{
			int held = 0;
			if ( e->children.first && e->children.first->element )
			{
				list_t* own = (list_t*)e->children.first->element;
				for ( node_t* n = own->first; n != nullptr; n = n->next ) { ++held; }
			}
			if ( held > 0 )
			{
				SAM_WARN("LUA", "sam_set_chest_stash: this chest already holds "
					+ std::to_string(held) + " item stack(s). They are not destroyed, but they"
					" are hidden while it is a stash; turn the stash off to reach them again."
					" Prefer converting an empty chest.");
			}
		}
		e->chestVoidState = on ? -1 : 0;
		serverUpdateEntitySkill(e, 17);
		SAM_INFO("LUA", std::string("Chest ") + std::to_string(uid)
			+ (on ? " is now a stash." : " is a normal chest again."));
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_travel_to_level(floor [, opts]) -> bool
	//   opts.secret = true  -> take the floor from the secret levels list instead
	//
	// Sends the party to an absolute floor number, including BACK UP, which the game
	// otherwise never does. Travel is deferred exactly the way a ladder defers it -- the
	// engine consumes the request at a safe point later in the frame -- so it is fine to
	// call this from inside an event handler.
	int lua_sam_travel_to_level(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int target = (int)luaL_checkinteger(Ls, 1);
		bool secret = false;
		if ( lua_istable(Ls, 2) )
		{
			lua_getfield(Ls, 2, "secret");
			secret = samBoolArg(Ls, -1, false);   // one boolean rule: {secret = 0} is false in both runtimes
			lua_pop(Ls, 1);
		}
		lua_pushboolean(Ls, SAMLua::travelToLevel(target, secret, "LUA") ? 1 : 0);
		return 1;
	}

	// sam_world_save(key, value) -> bool
	//
	// Saves a value INSIDE the current character's savegame. Compare sam_save_data, which
	// writes a file shared by every character and outlives the save that made it: that is
	// the right home for a mod's settings and the wrong home for a character's progress.
	// Use this one for anything a new character must not inherit.
	//
	// Values are size-capped (see sam_world_state.hpp). Keep flags and counters here, and
	// keep items in a stash chest, which the engine persists properly on its own.
	int lua_sam_world_save(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* keyC = luaL_checkstring(Ls, 1);
		const std::string key = keyC ? keyC : "";
		if ( g_currentNs.empty() ) { SAM_WARN("LUA", "sam_world_save: no owning mod namespace - ignored."); lua_pushboolean(Ls, 0); return 1; }
		if ( samLuaNoJsonForm(Ls, 2, "sam_world_save") ) { lua_pushboolean(Ls, 0); return 1; }
		nlohmann::json j = luaToJson(Ls, 2, 0);
		const std::string encoded = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		lua_pushboolean(Ls, SAMWorldState::set(g_currentNs, key, encoded) ? 1 : 0);
		return 1;
	}

	// sam_world_load(key) -> value | nil
	int lua_sam_world_load(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* keyC = luaL_checkstring(Ls, 1);
		const std::string key = keyC ? keyC : "";
		if ( g_currentNs.empty() ) { lua_pushnil(Ls); return 1; }
		std::string raw;
		if ( !SAMWorldState::get(g_currentNs, key, raw) ) { lua_pushnil(Ls); return 1; }
		// This value came out of a save file, which a player can edit and which a bad
		// shutdown can truncate, so a parse failure is expected rather than exceptional:
		// report it and hand back nil instead of throwing across the C boundary into Lua.
		nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
		if ( j.is_discarded() )
		{
			SAM_WARN("LUA", "sam_world_load: saved value for '" + key + "' is corrupt - ignoring it.");
			lua_pushnil(Ls); return 1;
		}
		jsonToLua(Ls, j, 0);
		return 1;
	}

	// sam_world_clear(key) -> bool
	int lua_sam_world_clear(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* keyC = luaL_checkstring(Ls, 1);
		if ( g_currentNs.empty() ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMWorldState::erase(g_currentNs, keyC ? keyC : "") ? 1 : 0);
		return 1;
	}

	// sam_world_keys() -> array of this mod's saved keys
	int lua_sam_world_keys(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		lua_newtable(Ls);
		if ( g_currentNs.empty() ) { return 1; }
		int n = 0;
		for ( const std::string& k : SAMWorldState::keys(g_currentNs) )
		{
			lua_pushstring(Ls, k.c_str());
			lua_rawseti(Ls, -2, ++n);
		}
		return 1;
	}

	// ===== v2.0: a mod runs its own loop ====================================================

	// sam_register_behavior("ns:name", fn) -> true
	//
	// fn(uid) runs once per frame for every entity carrying this behaviour. That is the
	// point: the script is not reacting to one of our events, it IS the entity's brain, and
	// everything the framework exposes is available inside it.
	//
	// Registering a name twice replaces the function and keeps the index, so entities already
	// in the world follow the new code. All behaviours are dropped when mods reload.
	// sam_attach_behavior(uid, "name") -> boolean. Runs `name` every tick for a LIVING
	// monster, AFTER its own AI. Unlike sam_register_behavior this does not replace
	// actMonster, so the creature keeps its AI, its death handling and its drops.
	int lua_sam_attach_behavior(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_attach_behavior refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_attach_behavior: no monster uid " + std::to_string(uid)); lua_pushboolean(Ls, 0); return 1; }
		// The same ':' rule sam_register_behavior uses. Prefixing unconditionally meant the
		// "mymod:sentry" the docs tell you to register with became "mymod:mymod:sentry" here, so
		// only the bare form ever survived the round trip.
		std::string full = nameC ? nameC : "";
		if ( full.find(':') == std::string::npos ) { full = g_currentNs + ":" + full; }
		const int idx = SAMLua::behaviorIndexFor(full);
		if ( idx < 0 )
		{
			SAM_ERROR("LUA", "sam_attach_behavior: no behavior named '" + full
				+ "' — register it with sam_register_behavior first.");
			lua_pushboolean(Ls, 0); return 1;
		}
		SAMLua::attachMonsterBehavior((unsigned long long)uid, idx);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_detach_behavior(uid) -> boolean.
	int lua_sam_detach_behavior(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		SAMLua::detachMonsterBehavior((unsigned long long)uid);
		lua_pushboolean(Ls, 1);
		return 1;
	}

	int lua_sam_register_behavior(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* nameC = luaL_checkstring(Ls, 1);
		if ( !lua_isfunction(Ls, 2) )
		{
			SAM_ERROR("LUA", "sam_register_behavior: second argument must be a function.");
			lua_pushboolean(Ls, 0); return 1;
		}
		if ( g_currentNs.empty() )
		{
			SAM_WARN("LUA", "sam_register_behavior: no owning mod namespace - ignored.");
			lua_pushboolean(Ls, 0); return 1;
		}
		std::string full = nameC ? nameC : "";
		// Namespace it for the author if they did not, so two mods cannot claim one name.
		if ( full.find(':') == std::string::npos ) { full = g_currentNs + ":" + full; }

		lua_pushvalue(Ls, 2);                              // copy the function
		const int ref = luaL_ref(Ls, LUA_REGISTRYINDEX);   // and keep it alive
		lua_pushboolean(Ls, SAMLua::registerBehavior(full, g_currentNs, ref) >= 0 ? 1 : 0);
		return 1;
	}

	// sam_set_entity_facing(uid, radians) -> bool
	// sam_look_at(uid, target_uid) -> bool        (the one a turret wants)
	// sam_get_entity_facing(uid) -> radians | nil
	//
	// sam_get_facing takes a PLAYER index and reads where that player is looking. These take
	// an entity UID, which is what a behaviour is handed, and they work on anything that is
	// not a player.
	int lua_sam_set_entity_facing(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const double rad = (double)luaL_checknumber(Ls, 2);
		lua_pushboolean(Ls, SAMLua::setEntityFacing((unsigned long long)uid, rad) ? 1 : 0);
		return 1;
	}

	int lua_sam_look_at(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const long long tgt = (long long)luaL_checkinteger(Ls, 2);
		lua_pushboolean(Ls, SAMLua::lookAt((unsigned long long)uid, (unsigned long long)tgt) ? 1 : 0);
		return 1;
	}

	int lua_sam_get_entity_facing(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const double y = SAMLua::entityFacing((unsigned long long)uid);
		if ( y < 0.0 ) { lua_pushnil(Ls); return 1; }
		lua_pushnumber(Ls, (lua_Number)y);
		return 1;
	}

	// sam_spawn_entity(tile_x, tile_y, "ns:behaviour" [, model]) -> uid | nil
	int lua_sam_spawn_entity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const double x = (double)luaL_checknumber(Ls, 1);
		const double y = (double)luaL_checknumber(Ls, 2);
		const char* behC = luaL_checkstring(Ls, 3);
		const char* modelC = lua_isnoneornil(Ls, 4) ? "" : luaL_checkstring(Ls, 4);
		const unsigned long long uid = SAMLua::spawnScriptedEntity(
			x, y, behC ? behC : "", modelC ? modelC : "", g_currentNs);
		if ( uid == 0 ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)uid);
		return 1;
	}

	int lua_sam_spawn_monsters(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long nearUid = (long long)luaL_checkinteger(Ls, 1);
		const char* typeC = luaL_checkstring(Ls, 2);
		int count = (int)luaL_checkinteger(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_spawn_monsters refused: host only."); lua_pushinteger(Ls, 0); return 1; }
		Entity* anchor = uidToEntity((Sint32)nearUid);
		if ( !anchor ) { SAM_WARN("LUA", "sam_spawn_monsters: no anchor entity uid " + std::to_string(nearUid)); lua_pushinteger(Ls, 0); return 1; }
		const int mtype = samMonsterNameToId(typeC);
		// 0 is NOTHING, not a creature: refused, as sam_spawn_monster refuses it.
		if ( mtype <= 0 ) { SAM_WARN("LUA", std::string("sam_spawn_monsters: unknown monster type '") + (typeC ? typeC : "") + "'"); lua_pushinteger(Ls, 0); return 1; }
		if ( count < 1 ) { count = 1; }
		if ( count > 8 ) { count = 8; } // hard cap per spec
		int spawned = 0;
		for ( int i = 0; i < count; ++i )
		{
			Entity* m = summonMonster((Monster)mtype, anchor->x, anchor->y); // finds a free adjacent tile itself
			if ( m ) { ++spawned; }
		}
		SAM_INFO("SAM", "sam_spawn_monsters: " + std::to_string(spawned) + "x " + (typeC ? typeC : "") + " near uid " + std::to_string(nearUid));
		lua_pushinteger(Ls, spawned);
		return 1;
#else
		(void)nearUid; (void)typeC; (void)count; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	int lua_sam_get_monster_target(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		int idx = -1;
		if ( Entity* e = samResolveMonster(uid) )
		{
			Entity* t = uidToEntity((Sint32)e->monsterTarget);
			if ( t && t->behavior == &actPlayer ) { idx = t->skill[2]; }
		}
		lua_pushinteger(Ls, idx);
		return 1;
#else
		(void)uid; lua_pushinteger(Ls, -1); return 1;
#endif
	}

	int lua_sam_set_monster_target(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int player = (int)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_monster_target refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_set_monster_target: no monster uid " + std::to_string(uid)); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_WARN("LUA", "sam_set_monster_target: invalid player " + std::to_string(player)); lua_pushboolean(Ls, 0); return 1; }
		e->monsterAcquireAttackTarget(*players[player]->entity, MONSTER_STATE_PATH);
		SAM_INFO("SAM", "sam_set_monster_target: uid " + std::to_string(uid) + " -> player " + std::to_string(player));
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)player; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_get_monster_data(uid, key) -> value (nil if unset). Per-monster scratch store.
	int lua_sam_get_monster_data(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* keyC = luaL_checkstring(Ls, 2);
		const std::string js = SAMLua::monsterDataGet((unsigned)(Sint32)uid, keyC ? keyC : "");
		if ( js.empty() ) { lua_pushnil(Ls); return 1; }
		nlohmann::json j = nlohmann::json::parse(js, nullptr, false);
		if ( j.is_discarded() ) { lua_pushnil(Ls); return 1; }
		jsonToLua(Ls, j, 0);
		return 1;
	}

	// sam_set_monster_data(uid, key, value) — store any primitive/table for a monster.
	int lua_sam_set_monster_data(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* keyC = luaL_checkstring(Ls, 2);
		if ( samLuaNoJsonForm(Ls, 3, "sam_set_monster_data") ) { lua_pushboolean(Ls, 0); return 1; }
		nlohmann::json j = luaToJson(Ls, 3, 0);
		// Lua strings are raw bytes; a non-UTF-8 value makes strict dump() throw
		// nlohmann::type_error. Since Lua is built as C (setjmp/longjmp), that C++
		// exception would unwind past lua_pcall -> std::terminate (host crash).
		// Use the 'replace' handler so invalid bytes become U+FFFD and dump()
		// never throws. (Matches the try/catch guard in sam_save_data.)
		SAMLua::monsterDataSet((unsigned)(Sint32)uid, keyC ? keyC : "",
			j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
		lua_pushboolean(Ls, 1);   // true, as the JS twin has always returned
		return 1;
	}

	// sam_get_player_data(player, key) -> value (nil if unset). Per-player, in-memory,
	// per-session scratch — cooldowns, ability flags, stack counters. Cleared on new game;
	// unlike sam_save_data it does not touch disk or persist across runs.
	int lua_sam_get_player_data(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* keyC = luaL_checkstring(Ls, 2);
		if ( player < 0 || player >= MAXPLAYERS ) { lua_pushnil(Ls); return 1; }
		const std::string js = SAMLua::playerDataGet(player, keyC ? keyC : "");
		if ( js.empty() ) { lua_pushnil(Ls); return 1; }
		nlohmann::json j = nlohmann::json::parse(js, nullptr, false);
		if ( j.is_discarded() ) { lua_pushnil(Ls); return 1; }
		jsonToLua(Ls, j, 0);
		return 1;
	}

	// sam_set_player_data(player, key, value) — store any primitive/table for a player.
	int lua_sam_set_player_data(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* keyC = luaL_checkstring(Ls, 2);
		if ( player < 0 || player >= MAXPLAYERS ) { return 0; }
		if ( samLuaNoJsonForm(Ls, 3, "sam_set_player_data") ) { return 0; }
		nlohmann::json j = luaToJson(Ls, 3, 0);
		// 'replace' handler: a non-UTF-8 Lua byte string must not make dump() throw a C++
		// exception across the Lua C boundary (crash). Mirrors sam_set_monster_data.
		SAMLua::playerDataSet(player, keyC ? keyC : "",
			j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
		return 0;
	}

	// ---- v0.7.0 Feature 5: modify existing content (patch class/item/monster) -----
#ifdef SAM_LUA_HAVE_BARONY
	// Resolve arg `idx` (integer classnum or "namespace:class" string) -> class id, or -1.
	int samResolveClassArg(lua_State* Ls, int idx)
	{
		if ( lua_isnumber(Ls, idx) )
		{
			const int n = (int)lua_tointeger(Ls, idx);
			if ( (n >= 0 && n < NUMCLASSES) || (n >= SAM_CLASS_ID_BASE && SAMClasses::getClass(n)) ) { return n; }
			return -1;
		}
		if ( lua_isstring(Ls, idx) ) { return SAMClasses::classIdForIdString(lua_tostring(Ls, idx)); }
		return -1;
	}
	// Resolve arg `idx` (integer id, vanilla item name, or "ns:item") -> item slot, or -1.
	int samResolveItemArg(lua_State* Ls, int idx)
	{
		if ( lua_isnumber(Ls, idx) ) { return (int)lua_tointeger(Ls, idx); }
		if ( lua_isstring(Ls, idx) )
		{
			std::string lower = lua_tostring(Ls, idx);
			for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
			auto it = ItemTooltips.itemNameStringToItemID.find(lower);
			if ( it != ItemTooltips.itemNameStringToItemID.end() ) { return it->second; }
			return SAMItems::itemIdForIdString(lua_tostring(Ls, idx));
		}
		return -1;
	}
	// Resolve arg `idx` (integer EFF_ id or effect name) -> effect id, or -1.
	int samResolvePassiveArg(lua_State* Ls, int idx)
	{
		if ( lua_isnumber(Ls, idx) ) { return (int)lua_tointeger(Ls, idx); }
		if ( lua_isstring(Ls, idx) ) { return samEffectNameToId(lua_tostring(Ls, idx)); }
		return -1;
	}
#endif

	int lua_sam_patch_class(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const int classnum = samResolveClassArg(Ls, 1);
		if ( classnum < 0 ) { SAM_ERROR("LUA", "sam_patch_class: unknown class."); lua_pushboolean(Ls, 0); return 1; }
		SAMClassStatPatch patch;
		if ( lua_istable(Ls, 2) )
		{
			lua_pushnil(Ls);
			while ( lua_next(Ls, 2) != 0 )
			{
				if ( lua_type(Ls, -2) == LUA_TSTRING )
				{
					const std::string uk = samUpper(lua_tostring(Ls, -2));
					if ( uk == "SKILLS" && lua_istable(Ls, -1) )
					{
						const int sIdx = lua_gettop(Ls);
						lua_pushnil(Ls);
						while ( lua_next(Ls, sIdx) != 0 )
						{
							// A number only, as in JS: a string used to become proficiency 0.
							if ( lua_type(Ls, -2) == LUA_TSTRING && lua_type(Ls, -1) == LUA_TNUMBER ) { patch.skills[lua_tostring(Ls, -2)] = (int)lua_tonumber(Ls, -1); }
							lua_pop(Ls, 1);
						}
					}
					else if ( lua_type(Ls, -1) == LUA_TNUMBER ) { patch.stats[uk] = (int)lua_tonumber(Ls, -1); }   // not a numeric string: JS's rule
				}
				lua_pop(Ls, 1);
			}
		}
		lua_pushboolean(Ls, SAMClasses::patchClass(classnum, patch) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	int lua_sam_unpatch_class(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const int classnum = samResolveClassArg(Ls, 1);
		if ( classnum < 0 ) { lua_pushboolean(Ls, 0); return 1; }
		SAMClasses::unpatchClass(classnum);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	int lua_sam_patch_item(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const int id = samResolveItemArg(Ls, 1);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_patch_item: unknown item."); lua_pushboolean(Ls, 0); return 1; }
		SAMItemPatch patch;
		if ( lua_istable(Ls, 2) )
		{
			// The JS twin's rule, so one table means one patch in both languages: numbers only from
			// a number, strings only from a string, and "value" beats "gold_value" and
			// "name_identified" beats "name" whatever order the table iterates in.
			bool haveValue = false, haveGold = false, haveNameId = false, haveName = false;
			int valueV = 0, goldV = 0;
			std::string nameIdV, nameV;
			lua_pushnil(Ls);
			while ( lua_next(Ls, 2) != 0 )
			{
				if ( lua_type(Ls, -2) == LUA_TSTRING )
				{
					const std::string uk = samUpper(lua_tostring(Ls, -2));
					const bool isNum = ( lua_type(Ls, -1) == LUA_TNUMBER );
					const bool isStr = ( lua_type(Ls, -1) == LUA_TSTRING );
					if ( uk == "ATTRIBUTES" && lua_istable(Ls, -1) )
					{
						const int aIdx = lua_gettop(Ls);
						lua_pushnil(Ls);
						while ( lua_next(Ls, aIdx) != 0 )
						{
							if ( lua_type(Ls, -2) == LUA_TSTRING && lua_type(Ls, -1) == LUA_TNUMBER )
							{
								patch.attributes[lua_tostring(Ls, -2)] = (int)lua_tonumber(Ls, -1);
							}
							lua_pop(Ls, 1);
						}
					}
					else if ( uk == "WEIGHT" && isNum ) { patch.hasWeight = true; patch.weight = (int)lua_tonumber(Ls, -1); }
					else if ( uk == "VALUE" && isNum ) { haveValue = true; valueV = (int)lua_tonumber(Ls, -1); }
					else if ( uk == "GOLD_VALUE" && isNum ) { haveGold = true; goldV = (int)lua_tonumber(Ls, -1); }
					else if ( uk == "LEVEL" && isNum ) { patch.hasLevel = true; patch.level = (int)lua_tonumber(Ls, -1); }
					else if ( uk == "CATEGORY" && isStr ) { patch.hasCategory = true; patch.category = samUpper(lua_tostring(Ls, -1)); }
					else if ( uk == "SLOT" && isStr ) { patch.hasSlot = true; patch.slot = lua_tostring(Ls, -1); }
					else if ( uk == "TOOLTIP" && isStr ) { patch.hasTooltip = true; patch.tooltip = lua_tostring(Ls, -1); }
					else if ( uk == "NAME_IDENTIFIED" && isStr ) { haveNameId = true; nameIdV = lua_tostring(Ls, -1); }
					else if ( uk == "NAME" && isStr ) { haveName = true; nameV = lua_tostring(Ls, -1); }
					else if ( uk == "NAME_UNIDENTIFIED" && isStr ) { patch.hasNameUnid = true; patch.nameUnidentified = lua_tostring(Ls, -1); }
				}
				lua_pop(Ls, 1);
			}
			if ( haveValue ) { patch.hasValue = true; patch.value = valueV; }
			else if ( haveGold ) { patch.hasValue = true; patch.value = goldV; }
			if ( haveNameId ) { patch.hasNameId = true; patch.nameIdentified = nameIdV; }
			else if ( haveName ) { patch.hasNameId = true; patch.nameIdentified = nameV; }
		}
		lua_pushboolean(Ls, SAMItems::patchItem(id, patch) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	int lua_sam_patch_monster(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_patch_monster refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		int mtype = -1;
		// A NUMBER is a type id and anything else a name, as in JS: lua_isnumber also accepted the
		// string "5" as type 5, which the JS twin sends to the name lookup (and does not find).
		if ( lua_type(Ls, 1) == LUA_TNUMBER ) { mtype = (int)lua_tonumber(Ls, 1); }
		else if ( lua_isstring(Ls, 1) ) { mtype = samMonsterNameToId(lua_tostring(Ls, 1)); }
		if ( mtype <= 0 || mtype >= NUMMONSTERS ) { SAM_ERROR("LUA", "sam_patch_monster: unknown monster type."); lua_pushboolean(Ls, 0); return 1; }
		int applied = 0;
		if ( lua_istable(Ls, 2) )
		{
			lua_pushnil(Ls);
			while ( lua_next(Ls, 2) != 0 )
			{
				// Numbers only (truncated toward zero), as the JS twin reads them.
				if ( lua_type(Ls, -2) == LUA_TSTRING && lua_type(Ls, -1) == LUA_TNUMBER )
				{
					const std::string uk = samUpper(lua_tostring(Ls, -2));
					if ( SAMMonsterPatch::set(mtype, uk, (int)lua_tonumber(Ls, -1)) ) { ++applied; }
				}
				lua_pop(Ls, 1);
			}
		}
		SAM_INFO("SAM", "sam_patch_monster: type " + std::to_string(mtype) + " (" + std::to_string(applied) + " field override(s))");
		lua_pushboolean(Ls, applied > 0 ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	int lua_sam_add_class_passive(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const int classnum = samResolveClassArg(Ls, 1);
		const int eff = samResolvePassiveArg(Ls, 2);
		if ( classnum < 0 ) { SAM_ERROR("LUA", "sam_add_class_passive: unknown class."); lua_pushboolean(Ls, 0); return 1; }
		if ( eff < 0 || eff >= NUMEFFECTS ) { SAM_ERROR("LUA", "sam_add_class_passive: unknown effect."); lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMClasses::addClassPassive(classnum, eff) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	int lua_sam_remove_class_passive(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const int classnum = samResolveClassArg(Ls, 1);
		const int eff = samResolvePassiveArg(Ls, 2);
		if ( classnum < 0 || eff < 0 ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMClasses::removeClassPassive(classnum, eff) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// ---- spells: grant a spell to a player ---------------------------------------
	// sam_grant_spell(player, spell) -> boolean. `spell` is a vanilla internal name
	// ("spell_fireball", any case), a mod's "namespace:spell", or a numeric id (what
	// sam_get_tome_spell returns). Taught on the player's OWN machine through the engine's
	// addSpell (the spell list plus its spell item): in multiplayer the trampoline carries a
	// remote player's grant there, and the host's call returns true once it is sent. False
	// when the spell is unknown, or already known (the player is told, as in vanilla).
	int lua_sam_grant_spell(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* spellC = luaL_checkstring(Ls, 2);
		const std::string spell = spellC ? spellC : "";
		SAM_INFO("API", "sam_grant_spell(player=" + std::to_string(player) + ", " + spell + ")");
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] )
		{
			SAM_ERROR("LUA", "sam_grant_spell: invalid player index " + std::to_string(player) + ".");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		const int id = SAMSpells::resolveSpellRef(spell);
		if ( id < 0 )
		{
			SAM_ERROR("LUA", "sam_grant_spell: unknown spell '" + spell + "' (expected a SPELL_ name, \"namespace:spell\" or a spell id).");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		lua_pushboolean(Ls, SAMSpells::grantSpell(player, id) ? 1 : 0);
		return 1;
#else
		(void)player;
		lua_pushboolean(Ls, 0);
		return 1;
#endif
	}

#ifdef SAM_LUA_HAVE_BARONY
	static int samResolveSpellId(const std::string& spell);   // below, beside the other spell helpers
#endif
	// sam_cast_spell(player, spell) — fire a spell/bolt from a player in the direction
	// they face (host-authoritative). spell = a vanilla SPELL_ name or "namespace:spell".
	// Passes trap=true so the scripted cast is free (no mana/skill-up) and is never blocked
	// by the defend/animation guard — ideal for "shoot on block". Returns true if a
	// projectile spawned. Do NOT call from an on_spell_cast handler (infinite recursion).
	int lua_sam_cast_spell(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* spellC = luaL_checkstring(Ls, 2);
		const std::string spell = spellC ? spellC : "";
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_cast_spell refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{
			SAM_ERROR("LUA", "sam_cast_spell: invalid player index " + std::to_string(player) + ".");
			lua_pushboolean(Ls, 0); return 1;
		}
		// The shared resolver, like sam_cast_spell_at and _pos: it also takes the numeric id
		// sam_get_tome_spell returns, which this inline copy used to refuse.
		const int id = samResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_cast_spell: unknown spell '" + spell + "' (SPELL_ name, \"namespace:spell\" or a spell id)."); lua_pushboolean(Ls, 0); return 1; }
		spell_t* sp = getSpellFromID(id);
		if ( !sp ) { SAM_ERROR("LUA", "sam_cast_spell: spell '" + spell + "' (id " + std::to_string(id) + ") has no engine spell."); lua_pushboolean(Ls, 0); return 1; }
		Entity* missile = castSpell(players[player]->entity->getUID(), sp, false, true);
		SAM_INFO("SAM", "sam_cast_spell: player " + std::to_string(player) + " cast '" + spell + "'" + (missile ? "" : " (no projectile)"));
		lua_pushboolean(Ls, missile ? 1 : 0);
		return 1;
#else
		(void)player; (void)spell;
		lua_pushboolean(Ls, 0);
		return 1;
#endif
	}

#ifdef SAM_LUA_HAVE_BARONY
	// v1.5.0: resolve a spell reference (vanilla internalName or custom "ns:spell") to an engine id, or -1.
	static int samResolveSpellId(const std::string& spell)
	{
		// A numeric id as text. sam_get_tome_spell hands back a NUMBER, and every consumer here
		// is string-only, so "read what this spellbook teaches, then grant it" -- the stated
		// purpose of that function -- resolved nothing. Lua stringifies the integer to "4" on
		// the way in, which matched no internalName.
		{
			bool digits = !spell.empty();
			for ( char c : spell ) { if ( c < '0' || c > '9' ) { digits = false; break; } }
			if ( digits )
			{
				const int id = (int)strtol(spell.c_str(), nullptr, 10);
				if ( ItemTooltips.spellItems.find(id) != ItemTooltips.spellItems.end() ) { return id; }
				return -1;
			}
		}
		if ( spell.find(':') != std::string::npos )
		{
			const SAMSpellDef* d = SAMSpells::getSpellByName(spell);
			return d ? d->numericId : -1;
		}
		std::string lower = spell;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		for ( const auto& kv : ItemTooltips.spellItems ) { if ( kv.second.internalName == lower ) { return kv.first; } }
		return -1;
	}
	// Fire spell `id` from caster `e`, aimed by transiently pointing its yaw at (tx,ty) in pixels
	// (the missile reads yaw once at spawn). aim==false casts straight along the caster's own yaw.
	static Entity* samCastAimed(Entity* e, int id, bool aim, double tx, double ty)
	{
		spell_t* sp = getSpellFromID(id);
		if ( !e || !sp ) { return nullptr; }
		if ( !aim ) { return castSpell(e->getUID(), sp, false, true); }
		const real_t savedYaw = e->yaw;
		e->yaw = atan2(ty - e->y, tx - e->x);
		Entity* missile = castSpell(e->getUID(), sp, false, true);
		e->yaw = savedYaw;
		return missile;
	}
#endif

	// sam_cast_spell_at(player, target_uid, spell) -> missile uid | nil. Cast at an entity
	// (aims the bolt toward it) instead of straight ahead. Host-only, free cast.
	int lua_sam_cast_spell_at(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const long long targetUid = (long long)luaL_checkinteger(Ls, 2);
		const char* spellC = luaL_checkstring(Ls, 3);
		const std::string spell = spellC ? spellC : "";
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_cast_spell_at refused: host only."); lua_pushnil(Ls); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("LUA", "sam_cast_spell_at: invalid player index " + std::to_string(player) + "."); lua_pushnil(Ls); return 1; }
		Entity* target = samResolveEntityQuiet((long long)targetUid);
		if ( !target ) { SAM_WARN("LUA", "sam_cast_spell_at: no entity uid " + std::to_string(targetUid)); lua_pushnil(Ls); return 1; }
		const int id = samResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_cast_spell_at: unknown spell '" + spell + "'."); lua_pushnil(Ls); return 1; }
		Entity* missile = samCastAimed(players[player]->entity, id, true, target->x, target->y);
		if ( !missile ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)missile->getUID());
		return 1;
#else
		(void)player; (void)targetUid; (void)spell; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_cast_spell_pos(player, tileX, tileY, spell) -> missile uid | nil. Cast toward a map tile.
	int lua_sam_cast_spell_pos(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const int tx = (int)luaL_checkinteger(Ls, 2);
		const int ty = (int)luaL_checkinteger(Ls, 3);
		const char* spellC = luaL_checkstring(Ls, 4);
		const std::string spell = spellC ? spellC : "";
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_cast_spell_pos refused: host only."); lua_pushnil(Ls); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("LUA", "sam_cast_spell_pos: invalid player index " + std::to_string(player) + "."); lua_pushnil(Ls); return 1; }
		const int id = samResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_cast_spell_pos: unknown spell '" + spell + "'."); lua_pushnil(Ls); return 1; }
		Entity* missile = samCastAimed(players[player]->entity, id, true, (double)(tx * 16 + 8), (double)(ty * 16 + 8));
		if ( !missile ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)missile->getUID());
		return 1;
#else
		(void)player; (void)tx; (void)ty; (void)spell; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_monster_cast_spell(uid, spell) -> missile uid | nil. Make a monster cast a spell along
	// its own facing. Host-only, free cast.
	int lua_sam_monster_cast_spell(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* spellC = luaL_checkstring(Ls, 2);
		const std::string spell = spellC ? spellC : "";
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_monster_cast_spell refused: host only."); lua_pushnil(Ls); return 1; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("LUA", "sam_monster_cast_spell: no monster uid " + std::to_string(uid)); lua_pushnil(Ls); return 1; }
		const int id = samResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_monster_cast_spell: unknown spell '" + spell + "'."); lua_pushnil(Ls); return 1; }
		Entity* missile = samCastAimed(e, id, false, 0, 0);
		if ( !missile ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)missile->getUID());
		return 1;
#else
		(void)uid; (void)spell; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_spells(player) -> array of the spells the player knows: a mod's spell by the
	// "namespace:spell" its JSON declared, a vanilla one by its internal name. nil when this
	// machine cannot see that player's spells; on the host, a remote player's list is the one
	// their own machine reports (the host never holds one: addSpell refuses a non-local player).
	int lua_sam_get_spells(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		std::vector<std::string> names;
		if ( SAMMpInventory::spellNamesOf(player, "sam_get_spells", &names) == SAMMpInventory::Seen::No )
		{
			lua_pushnil(Ls);
			return 1;
		}
		lua_newtable(Ls);
		int n = 0;
		for ( const std::string& nm : names )
		{
			lua_pushstring(Ls, nm.c_str());
			lua_rawseti(Ls, -2, ++n);
		}
		return 1;
#else
		(void)player;
		lua_newtable(Ls);
		return 1;
#endif
	}

	// sam_player_knows_spell(player, spell) -> bool. Vanilla SPELL_ name or custom "ns:spell".
	int lua_sam_player_knows_spell(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* spellC = luaL_checkstring(Ls, 2);
		const std::string spell = spellC ? spellC : "";
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { lua_pushboolean(Ls, 0); return 1; }
		const int id = SAMSpells::resolveSpellRef(spell);
		if ( id < 0 ) { lua_pushboolean(Ls, 0); return 1; }
		// On the host a remote player's spells are the list their own machine reported. nil when
		// this machine cannot see them: false would make "if not knows then grant" misfire.
		std::vector<int> ids;
		if ( SAMMpInventory::spellIdsOf(player, "sam_player_knows_spell", &ids) == SAMMpInventory::Seen::No )
		{
			lua_pushnil(Ls);
			return 1;
		}
		bool known = false;
		for ( const int k : ids ) { if ( k == id ) { known = true; break; } }
		lua_pushboolean(Ls, known ? 1 : 0);
		return 1;
#else
		(void)player; (void)spell; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_remove_spell(player, spell) -> bool. Un-learn a spell and take away its spell item
	// (and a vanilla spell's shapeshift twin). Done on the player's own machine: in
	// multiplayer the trampoline carries a remote player's removal there, and the host's call
	// returns true once sent. Queued to the end of the tick (SAMSpells::queueRemoveSpell): the
	// engine may be casting that very spell when a script asks. False when the player does not
	// know it.
	int lua_sam_remove_spell(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* spellC = luaL_checkstring(Ls, 2);
		const std::string spell = spellC ? spellC : "";
#ifdef SAM_LUA_HAVE_BARONY
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("LUA", "sam_remove_spell refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { lua_pushboolean(Ls, 0); return 1; }
		const int id = SAMSpells::resolveSpellRef(spell);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_remove_spell: unknown spell '" + spell + "'."); lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMSpells::queueRemoveSpell(player, id) ? 1 : 0);
		return 1;
#else
		(void)player; (void)spell; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// =====================================================================================
	//  v2.8 batch 4 — COMBAT
	//
	//  Shapes decided by engine facts rather than by symmetry. The recurring one:
	//  Entity::getStats() answers nullptr for every behavior except actMonster, actPlayer and
	//  actPlayerLimb, and Entity::getHP() answers 0 rather than -1 in that case. A reader that
	//  passed that through would tell a script "this door is at zero health", which is a
	//  different and wronger claim than "a door has no health". So every reader here answers
	//  nil for anything that is not a creature.
	// =====================================================================================

#ifdef SAM_LUA_HAVE_BARONY
	// The seven damage classes the engine has (DamageTableType, monster.hpp). They are WEAPON
	// CLASSES, not elements — Barony has no fire/ice/lightning axis anywhere in the damage
	// funnel. The INDEX is the enum value, so the order of this array is load-bearing.
	static const char* const kSamDamageTypes[] = {
		"sword", "mace", "axe", "polearm", "ranged", "magic", "unarmed"
	};
	static const int kSamDamageTypeCount = 7;

	// The one place that turns a damage-type argument into an enum value, so the refusal is
	// written once and every function taking one says the same thing.
	static bool samDamageTypeArg(const char* nameIn, const char* who, int* out)
	{
		std::string want = nameIn ? nameIn : "";
		for ( char& c : want ) { c = (char)std::tolower((unsigned char)c); }
		for ( int i = 0; i < kSamDamageTypeCount; ++i )
		{
			if ( want == kSamDamageTypes[i] ) { *out = i; return true; }
		}
		SAM_ERROR("LUA", std::string(who) + ": '" + want + "' is not a damage type. Barony has"
			" exactly seven and they are weapon classes rather than elements: sword, mace, axe,"
			" polearm, ranged, magic, unarmed.");
		return false;
	}

	// Read-side resolve to an entity that HAS a Stat. Silent, like every other reader — see
	// samResolveEntityQuiet for why a reader must not warn on this API's own "no entity" value.
	static Entity* samResolveCombatant(long long uid, Stat** outStats)
	{
		Entity* e = samResolveEntityQuiet(uid);
		if ( !e ) { return nullptr; }
		Stat* s = e->getStats();
		if ( !s ) { return nullptr; }
		if ( outStats ) { *outStats = s; }
		return e;
	}

	// Write-side twin: refuses a client, a sentinel uid and a limb (samResolveWritable), then
	// says out loud that this particular uid has no health to change.
	static Entity* samResolveCombatantWritable(long long uid, const char* who, Stat** outStats)
	{
		Entity* e = samResolveWritable(uid, who);
		if ( !e ) { return nullptr; }
		Stat* s = e->getStats();
		if ( !s )
		{
			SAM_WARN("LUA", std::string(who) + ": uid " + std::to_string(uid) + " is not a creature."
				" Only players and monsters carry the stats this needs — a chest, a door, an arrow"
				" and a gib have none.");
			return nullptr;
		}
		if ( outStats ) { *outStats = s; }
		return e;
	}
#endif

	// sam_heal(uid, amount) -> the HP actually restored, or nil if the uid is not a creature.
	//
	// sam_deal_damage CANNOT do this, which is the whole reason this exists: it forces the sign
	// negative (`amount < 0 ? amount : -amount`), so BOTH signs damage. Healing was reachable
	// only through the absolute write sam_set_stat(player, "HP", n), which makes the caller
	// read, add and clamp by hand and does not exist at all for a creature that is not a
	// monster.
	//
	// Returns what LANDED, not what was asked. Entity::setHP clamps into [0, MAXHP], so a
	// 50-point heal on a creature three short of full restores three — and a mod building a
	// lifesteal effect needs the real number, not the one it hoped for.
	int lua_sam_heal(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int amount = (int)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatantWritable(uid, "sam_heal", &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		if ( amount <= 0 )
		{
			SAM_WARN("LUA", "sam_heal: the amount has to be positive. To hurt something use"
				" sam_deal_damage — passing a negative here would have healed it anyway.");
			lua_pushinteger(Ls, 0); return 1;
		}
		const int beforeHP = s->HP;
		e->modHP(amount);
		lua_pushinteger(Ls, (lua_Integer)(s->HP - beforeHP));
		return 1;
#else
		(void)uid; (void)amount; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_deal_damage_typed(uid, amount, type) -> the damage actually dealt, or nil.
	//
	// The same hit as sam_deal_damage, run through the target's resistance to that weapon class
	// first — so "10 magic damage" is 5 against something that halves magic and 20 against
	// something that doubles it, without the script needing to know which.
	//
	// It applies BOTH stages, in the engine's own order: the species damage table
	// (getDamageTableMultiplier) and then the live effect modifiers such as blood ward and
	// sanctuary (modifyDamageMultipliersFromEffects). actarrow.cpp is exactly this pair.
	// Applying only the first would silently ignore every defensive buff in the game and quietly
	// make those spells not work against scripted damage.
	//
	// Returns 0, honestly, when resistance eats the hit. A mod that wants the raw number applied
	// regardless should use sam_deal_damage.
	int lua_sam_deal_damage_typed(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int amount = (int)luaL_checkinteger(Ls, 2);
		const char* typeC = luaL_checkstring(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		int dtype = 0;
		if ( !samDamageTypeArg(typeC, "sam_deal_damage_typed", &dtype) ) { lua_pushnil(Ls); return 1; }
		Stat* s = nullptr;
		Entity* e = samResolveCombatantWritable(uid, "sam_deal_damage_typed", &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		const int asked = ( amount < 0 ) ? -amount : amount;
		real_t mult = Entity::getDamageTableMultiplier(e, *s, (DamageTableType)dtype);
		Entity::modifyDamageMultipliersFromEffects(e, nullptr, mult, (DamageTableType)dtype);
		int dealt = (int)(asked * mult);
		if ( dealt < 0 ) { dealt = 0; }
		if ( dealt > 0 ) { e->modHP(-dealt); }
		lua_pushinteger(Ls, (lua_Integer)dealt);
		return 1;
#else
		(void)uid; (void)amount; (void)typeC; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_hp(uid) / sam_get_max_hp(uid) -> int, or nil for anything without a Stat.
	//
	// Neither existing getter reaches here. sam_get_stat takes a PLAYER INDEX, and
	// sam_get_monster_stat refuses anything whose behavior is not actMonster — so a script
	// holding a uid out of sam_find_entities could not read the health of another PLAYER, or of
	// a companion, at all.
	int lua_sam_get_hp(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		if ( !samResolveCombatant(uid, &s) || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)s->HP);
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	int lua_sam_get_max_hp(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		if ( !samResolveCombatant(uid, &s) || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)s->MAXHP);
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_mp(uid) / sam_get_max_mp(uid) -> int, or nil for anything without a Stat.
	//
	// The twins of sam_get_hp, and they exist for the same reason plus one more: this batch added
	// three verbs that CHANGE mana and, without these, the only way to observe it on anything but
	// a player was to call one of those mutators and read what it returned. A mutator is not a
	// reader, and a test built on one cannot tell a working verb from a broken one.
	int lua_sam_get_mp(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		if ( !samResolveCombatant(uid, &s) || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)s->MP);
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	int lua_sam_get_max_mp(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		if ( !samResolveCombatant(uid, &s) || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)s->MAXMP);
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_attack(uid) -> the melee attack value the engine would use, or nil.
	int lua_sam_get_attack(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatant(uid, &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)Entity::getAttack(e, s, e->behavior == &actPlayer));
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_ranged_attack(uid [, quiver_bonus]) -> int, or nil.
	int lua_sam_get_ranged_attack(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int quiver = (int)luaL_optinteger(Ls, 2, 0);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatant(uid, &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)e->getRangedAttack(quiver));
		return 1;
#else
		(void)uid; (void)quiver; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_thrown_attack(uid) -> int, or nil.
	int lua_sam_get_thrown_attack(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatant(uid, &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)e->getThrownAttack());
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_bonus_attack_vs(uid, target_uid) -> the extra attack this attacker gets against
	// that particular target (slayer enchantments and the like), or nil.
	int lua_sam_get_bonus_attack_vs(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const long long tgt = (long long)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatant(uid, &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		Stat* ts = nullptr;
		if ( !samResolveCombatant(tgt, &ts) || !ts ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)e->getBonusAttackOnTarget(*ts));
		return 1;
#else
		(void)uid; (void)tgt; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_damage_resist(uid [, type]) -> the multiplier this creature takes that damage
	// class at (1.0 normal, 0.5 half, 2.0 double), or nil. Defaults to "magic", which is the
	// one the game itself puts on the character sheet.
	//
	// This is the FULL figure — equipment, effects and magic resistance included. It is the
	// same call the character sheet makes to draw the number a player sees.
	int lua_sam_get_damage_resist(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* typeC = luaL_optstring(Ls, 2, "magic");
#ifdef SAM_LUA_HAVE_BARONY
		int dtype = 0;
		if ( !samDamageTypeArg(typeC, "sam_get_damage_resist", &dtype) ) { lua_pushnil(Ls); return 1; }
		Stat* s = nullptr;
		Entity* e = samResolveCombatant(uid, &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushnumber(Ls, (lua_Number)Entity::getDamageTableMultiplier(e, *s, (DamageTableType)dtype));
		return 1;
#else
		(void)uid; (void)typeC; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_magic_resist(uid) -> the raw magic-resistance POINT count, or nil. Each point is
	// a separate reduction inside getDamageTableMultiplier; this is the input,
	// sam_get_damage_resist(uid, "magic") is the result.
	int lua_sam_get_magic_resist(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		if ( !samResolveCombatant(uid, &s) || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)Entity::getMagicResistance(s));
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_preview_damage(attacker_uid, target_uid) -> what a melee swing would deal right now,
	// dealing nothing. nil if either side is not a creature.
	//
	// Composed from the same three public terms the melee path combines, in the same
	// expression: the attacker's attack, the target's AC effectiveness, and its AC. It is a
	// PREVIEW — the real swing then folds in weapon multipliers, backstab and capstone bonuses,
	// so treat this as the floor rather than a promise.
	int lua_sam_preview_damage(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const long long tgt = (long long)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* as = nullptr;
		Entity* ae = samResolveCombatant(uid, &as);
		if ( !ae || !as ) { lua_pushnil(Ls); return 1; }
		Stat* ts = nullptr;
		Entity* te = samResolveCombatant(tgt, &ts);
		if ( !te || !ts ) { lua_pushnil(Ls); return 1; }

		const real_t myAttack = (real_t)Entity::getAttack(ae, as, ae->behavior == &actPlayer);
		int numBlessings = 0;
		const real_t acEff = Entity::getACEffectiveness(te, ts, te->behavior == &actPlayer,
			ae, as, numBlessings);
		const real_t enemyAC = (real_t)AC(ts);
		int out = (int)(std::max(0.0, ((myAttack * acEff - enemyAC))) + (1.0 - acEff) * myAttack);
		if ( out < 0 ) { out = 0; }
		lua_pushinteger(Ls, (lua_Integer)out);
		return 1;
#else
		(void)uid; (void)tgt; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_regen_interval(uid) -> ticks between natural HP regeneration ticks, or nil.
	// SMALLER is faster. Nothing in S.A.M exposed regeneration at all before this.
	int lua_sam_get_regen_interval(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatant(uid, &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)Entity::getHealthRegenInterval(e, *s, e->behavior == &actPlayer));
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_get_healring(uid) -> the regeneration bonus from equipment and effects combined, or
	// nil. This is what makes the interval above shorter.
	int lua_sam_get_healring(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatant(uid, &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		const int fromGear = Entity::getHealringFromEquipment(e, *s, e->behavior == &actPlayer);
		const int fromEff  = Entity::getHealringFromEffects(e, *s);
		lua_pushinteger(Ls, (lua_Integer)(fromGear + fromEff));
		return 1;
#else
		(void)uid; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_mod_mp(uid, amount) -> the MP after the change, or nil. Relative, and negative is
	// allowed. S.A.M had only the absolute write sam_set_stat(player,"MP",n) before this, which
	// meant every "spend 5 mana" had to read, subtract and clamp by hand — and could not reach
	// a creature that was not a monster.
	int lua_sam_mod_mp(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int amount = (int)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatantWritable(uid, "sam_mod_mp", &s);
		if ( !e || !s ) { lua_pushnil(Ls); return 1; }
		e->modMP(amount, true);   // true: setMP writes its own UPMP packet, so clients keep up
		lua_pushinteger(Ls, (lua_Integer)s->MP);
		return 1;
#else
		(void)uid; (void)amount; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_drain_mp(uid, amount [, notify]) -> true if it ran.
	//
	// The DANGEROUS one, deliberately: anything drained past the creature's remaining MP comes
	// out of its HEALTH instead. That overdraw is the point — it is how a blood-magic cost is
	// expressed — but it can kill, so it gets its own name rather than hiding inside sam_mod_mp
	// behind a negative number.
	int lua_sam_drain_mp(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int amount = (int)luaL_checkinteger(Ls, 2);
		const bool notify = samBoolArg(Ls, 3, true);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatantWritable(uid, "sam_drain_mp", &s);
		if ( !e || !s ) { lua_pushboolean(Ls, 0); return 1; }
		if ( amount <= 0 )
		{
			SAM_WARN("LUA", "sam_drain_mp: the amount has to be positive. To GIVE mana use sam_mod_mp.");
			lua_pushboolean(Ls, 0); return 1;
		}
		e->drainMP(amount, notify);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)amount; (void)notify; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_consume_mp(uid, amount) -> true if the cost was paid, false if it could not be.
	//
	// The safe counterpart to sam_drain_mp and the one a custom ability's cost should use --
	// with ONE engine exception worth knowing before relying on it. Entity::safeConsumeMP has a
	// VAMPIRE arm (entity.cpp:3894): a vampire PLAYER who cannot afford the cost has the
	// shortfall drained out of HEALTH and still gets true back. That is Barony's rule for
	// vampires, not ours, and overriding it here would make every vampire mod wrong instead.
	int lua_sam_consume_mp(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int amount = (int)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatantWritable(uid, "sam_consume_mp", &s);
		if ( !e || !s ) { lua_pushboolean(Ls, 0); return 1; }
		if ( amount < 0 )
		{
			SAM_WARN("LUA", "sam_consume_mp: the amount cannot be negative. To GIVE mana use sam_mod_mp.");
			lua_pushboolean(Ls, 0); return 1;
		}
		lua_pushboolean(Ls, e->safeConsumeMP(amount) ? 1 : 0);
		return 1;
#else
		(void)uid; (void)amount; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_defending(player, on) -> true if it changed anything.
	//
	// The read has existed since v1.2 (sam_is_defending) with no way to cause it.
	//
	// IT LASTS ONE FRAME, and that is not a defect in this function -- it is what the field is.
	// actHudShield writes stats[player]->defending from the block input EVERY frame
	// (acthudweapon.cpp:4460 and :4465), unconditionally, for whichever player that machine is
	// playing. A remote player's copy is refreshed only by their 'SHLD' packet, so the host puts
	// their own value back after one pass of game logic (SAMCombat::noteDefendingOverride). It
	// is a host-side combat override: the player's own screen never shows the stance. So
	// this is a same-frame override: useful immediately before reading combat maths, or from a
	// per-frame handler that re-applies it, and useless as a latch you set once. Said out loud
	// rather than left to be discovered, because a setter the engine quietly undoes is the exact
	// shape of bug this project keeps finding in its own work.
	int lua_sam_set_defending(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		bool on = true;
		if ( !samBoolReq(Ls, 2, "sam_set_defending", &on) ) { lua_pushboolean(Ls, 0); return 1; }
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_set_defending refused: host only.");
			lua_pushboolean(Ls, 0); return 1;
		}
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushboolean(Ls, 0); return 1; }
		const bool was = stats[player]->defending;
		stats[player]->defending = on;
		// A remote player's flag is otherwise rewritten only by their next 'SHLD', up to 2.4 s away.
		SAMCombat::noteDefendingOverride(player, was, on);
		lua_pushboolean(Ls, ( was != on ) ? 1 : 0);
		return 1;
#else
		(void)player; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_is_parrying(player) -> true while the parry window is open. Exposed nowhere before
	// this, though the engine consumes it in melee resolution to produce parried damage.
	int lua_sam_is_parrying(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, ( stats[player]->parrying > 0 ) ? 1 : 0);
		return 1;
#else
		(void)player; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_parry(player, ticks) -> true if set. 0 closes the window.
	int lua_sam_set_parry(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		long long ticks = (long long)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_set_parry refused: host only.");
			lua_pushboolean(Ls, 0); return 1;
		}
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushboolean(Ls, 0); return 1; }
		if ( ticks < 0 ) { ticks = 0; }
		// The field is a Uint32 counted down by the engine. An hour is already far longer than
		// any real window, and a script asking for a year is a units mistake rather than an
		// intention — clamping is the difference between a long parry and a permanent one.
		if ( ticks > 180000LL ) { ticks = 180000LL; }
		stats[player]->parrying = (Uint32)ticks;
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)player; (void)ticks; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_monster_target_uid(uid, target_uid [, was_hit]) -> true if the monster took it.
	//
	// sam_set_monster_target already exists and takes a PLAYER INDEX, hardcoding
	// `players[player]->entity` — so monster-versus-monster aggro, which the engine method
	// itself supports (it takes any Entity), was unreachable from a script. This is the same
	// call with that restriction removed: point a monster at another monster, at a companion,
	// or at anything else with a body.
	int lua_sam_set_monster_target_uid(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const long long tgt = (long long)luaL_checkinteger(Ls, 2);
		const bool wasHit = samBoolArg(Ls, 3, false);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_set_monster_target_uid refused: host only. AI lives on the host,"
				" and a client holds no stats for an ordinary monster.");
			lua_pushboolean(Ls, 0); return 1;
		}
		Entity* e = samResolveMonster(uid);
		if ( !e )
		{
			SAM_WARN("LUA", "sam_set_monster_target_uid: uid " + std::to_string(uid) + " is not a"
				" monster with stats, so it has no AI to point anywhere.");
			lua_pushboolean(Ls, 0); return 1;
		}
		Entity* t = samResolveEntityQuiet(tgt);
		if ( !t ) { lua_pushboolean(Ls, 0); return 1; }
		if ( t == e )
		{
			SAM_WARN("LUA", "sam_set_monster_target_uid: a monster cannot hunt itself.");
			lua_pushboolean(Ls, 0); return 1;
		}
		e->monsterAcquireAttackTarget(*t, MONSTER_STATE_PATH, wasHit);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)tgt; (void)wasHit; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_get_monster_target_uid(uid) -> the uid of whatever this monster is hunting, or 0.
	//
	// sam_get_monster_target answers a PLAYER INDEX and -1 for everything that is not a player,
	// so the moment sam_set_monster_target_uid could aim a monster at another monster the result
	// became unreadable. A writer with no reader cannot be verified by anyone, including its own
	// test. 0 means hunting nobody, which is the value the rest of this API already uses for
	// "no entity".
	//
	// The stored number is RESOLVED rather than returned raw: monsterTarget is not cleared when
	// its target dies, and the engine rolls the uid counter back for throwaway particles, so a
	// stale number can come to name something else entirely. Resolving it is the difference
	// between "hunting entity 4211" and "hunting whatever holds 4211 now".
	int lua_sam_get_monster_target_uid(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushinteger(Ls, 0); return 1; }
		Entity* t = uidToEntity((Sint32)e->monsterTarget);
		lua_pushinteger(Ls, t ? (lua_Integer)t->getUID() : 0);
		return 1;
#else
		(void)uid; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_clear_monster_target(uid [, force]) -> true if it let go.
	//
	// The engine method can REFUSE — a monster whose AI insists keeps its target unless `force`
	// is set — and that refusal is passed through rather than swallowed, so a script can tell
	// "it let go" from "it would not".
	int lua_sam_clear_monster_target(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const bool force = samBoolArg(Ls, 2, false);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_clear_monster_target refused: host only.");
			lua_pushboolean(Ls, 0); return 1;
		}
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, e->monsterReleaseAttackTarget(force) ? 1 : 0);
		return 1;
#else
		(void)uid; (void)force; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_alert_allies(uid [, attacker_uid]) -> true if the call ran.
	//
	// Wakes every ally near this monster onto the attacker, which is what the engine does when
	// something is hit in a room full of its friends. attacker_uid may be omitted for "alerted
	// by nothing in particular".
	int lua_sam_alert_allies(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const long long att = (long long)luaL_optinteger(Ls, 2, 0);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_alert_allies refused: host only.");
			lua_pushboolean(Ls, 0); return 1;
		}
		Entity* e = samResolveMonster(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		Entity* a = ( att != 0 ) ? samResolveEntityQuiet(att) : nullptr;
		e->alertAlliesOnBeingHit(a);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)uid; (void)att; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_break_armor(uid [, slot]) -> true if the piece degraded (and possibly broke).
	//
	// player.on_item_broken has existed as an event with no verb able to cause it.
	//
	// With no slot named, the engine's OWN picker chooses, so the odds and the exclusions match
	// what a real hit does. With a slot named, the number passed to degradeArmor is the
	// engine's equipment numbering (the one net.cpp's 'ARMR' packet uses): 0 helmet,
	// 1 breastplate, 2 gloves, 3 boots, 4 shield, 6 cloak, 9 mask. It is NOT the equipment-slot
	// enum and the two disagree, which is why this table is written out rather than cast.
	//
	// False is a real answer as well as a failure: degradeArmor refuses shadows and liches
	// outright, and refuses artifacts, quivers and anything preserved.
	int lua_sam_break_armor(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* slotC = luaL_optstring(Ls, 2, "");
#ifdef SAM_LUA_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = samResolveCombatantWritable(uid, "sam_break_armor", &s);
		if ( !e || !s ) { lua_pushboolean(Ls, 0); return 1; }

		std::string want = slotC ? slotC : "";
		for ( char& c : want ) { c = (char)std::tolower((unsigned char)c); }

		Item* armor = nullptr;
		int armornum = -1;

		if ( want.empty() )
		{
			// The engine's own picker, with the melee path's arguments: no weapon, no jewellery.
			// It also knows the exclusions a hand-written list would miss — a shapeshifted
			// creature has nothing to break, and a quiver or a spellbook in the shield slot is
			// not armour.
			armornum = s->pickRandomEquippedItemToDegradeOnHit(&armor, true, false, false, true);
			if ( armornum < 0 || !armor ) { lua_pushboolean(Ls, 0); return 1; }
		}
		else
		{
			struct SamArmorSlot { const char* name; int armornum; Item** item; };
			const SamArmorSlot slots[] = {
				{ "helmet",      0, &s->helmet      },
				{ "breastplate", 1, &s->breastplate },
				{ "armor",       1, &s->breastplate },
				{ "gloves",      2, &s->gloves      },
				{ "boots",       3, &s->shoes       },
				{ "shoes",       3, &s->shoes       },
				{ "shield",      4, &s->shield      },
				{ "cloak",       6, &s->cloak       },
				{ "mask",        9, &s->mask        },
			};
			const int slotCount = (int)(sizeof(slots) / sizeof(slots[0]));
			int chosen = -1;
			for ( int i = 0; i < slotCount; ++i )
			{
				if ( want == slots[i].name ) { chosen = i; break; }
			}
			if ( chosen < 0 )
			{
				SAM_ERROR("LUA", "sam_break_armor: '" + want + "' is not a slot that can degrade."
					" They are helmet, breastplate, gloves, boots, shield, cloak and mask.");
				lua_pushboolean(Ls, 0); return 1;
			}
			armor = *slots[chosen].item;
			armornum = slots[chosen].armornum;
			if ( !armor ) { lua_pushboolean(Ls, 0); return 1; }   // nothing worn there
		}
		lua_pushboolean(Ls, e->degradeArmor(*s, *armor, armornum) ? 1 : 0);
		return 1;
#else
		(void)uid; (void)slotC; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_gib(uid [, sprite]) -> true if a chunk was thrown.
	//
	// The one member of the spawn family sam_spawn_particle could not carry: bang, poof,
	// explosion and sleep all take a POSITION, and a gib takes a PARENT — it inherits the
	// creature's colour and flies off it. The optional sprite overrides the model.
	int lua_sam_gib(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int sprite = (int)luaL_optinteger(Ls, 2, -1);
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = samResolveWritable(uid, "sam_gib");
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		Entity* g = spawnGib(e, sprite);
		if ( g ) { SAMMpEntities::gibForClients(g); }   // the engine's SPGB, from the tick; stock clients handle it
		lua_pushboolean(Ls, g ? 1 : 0);
		return 1;
#else
		(void)uid; (void)sprite; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_obituary(killer_uid, victim_uid [, from_spell]) -> true if it was recorded.
	//
	// A scripted kill produces no attribution at all today: sam_kill_monster just sets HP to 0,
	// so the death message is the generic one and nobody gets credit. This writes the killer
	// and the death text the way the engine does for its own kills.
	//
	// ORDER MATTERS. Entity::setHP overwrites the obituary with the generic string on EVERY HP
	// change, so calling this before the killing blow would have it immediately overwritten.
	// Call it after.
	int lua_sam_obituary(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long killer = (long long)luaL_checkinteger(Ls, 1);
		const long long victim = (long long)luaL_checkinteger(Ls, 2);
		const bool fromSpell = samBoolArg(Ls, 3, false);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_obituary refused: host only.");
			lua_pushboolean(Ls, 0); return 1;
		}
		Entity* k = samResolveEntityQuiet(killer);
		Stat* vs = nullptr;
		Entity* v = samResolveCombatant(victim, &vs);
		if ( !k || !v || !vs )
		{
			SAM_WARN("LUA", "sam_obituary: both sides have to be live entities and the victim has"
				" to be a creature — the obituary is written into the victim's own stats.");
			lua_pushboolean(Ls, 0); return 1;
		}
		k->killedByMonsterObituary(v, fromSpell);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)killer; (void)victim; (void)fromSpell; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_revive_player(player [, x, y]) -> true if the player is back on their feet.
	//
	// Every slot. The host builds the body exactly as the engine's own 'REZZ' handler does; for a
	// player on another machine, that machine (S.A.M only) is told to take its ghost down. The
	// gear the death already copied out -- to the floor in singleplayer, to the loot bag in co-op
	// without keep-inventory -- is removed from the revived player, on every machine that holds a
	// copy, so nothing exists twice. A player whose game does not run S.A.M is refused: only their
	// machine can drop its ghost and its copy of the gear. See SAMCombat::revivePlayer.
	//
	// x and y are TILE coordinates. Omitted, the ghost's own tile is used, which is where the
	// player is looking from.
	int lua_sam_revive_player(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const int wantX = (int)luaL_optinteger(Ls, 2, -1);
		const int wantY = (int)luaL_optinteger(Ls, 3, -1);
#ifdef SAM_LUA_HAVE_BARONY
		// Shared with the JS twin: SAMCombat::revivePlayer (sam_combat.cpp).
		lua_pushboolean(Ls, SAMCombat::revivePlayer(player, wantX, wantY) ? 1 : 0);
		return 1;
#else
		(void)player; (void)wantX; (void)wantY; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_species_damage_resist(species, type, multiplier) -> true if it took.
	//
	// Species-wide, not per-creature: every skeleton in the game, now and later. 1.0 is normal,
	// 0.5 halves what they take, 2.0 doubles it.
	//
	// IT CANNOT GRANT IMMUNITY. This number only seeds the multiplier; the engine then runs its
	// bonus pool and floors the result at 0.1, so the least damage any species can be made to
	// take is a TENTH, not none. Pass 0 and you get 0.1. For real immunity use
	// sam_set_damage_immune, or veto on_before_damage or on_damage_multiplier.
	//
	// NOT A WRITE TO damagetables. That array is declared `static` inside monster.hpp, so every
	// translation unit owns a private copy and a write from here would be invisible to the
	// engine's read — reporting success and changing nothing. The override lives in a table
	// entity.cpp consults instead. See sam_combat.hpp.
	//
	// EVERY MACHINE KEEPS A COPY, AND THEY AGREE. The damage is computed on the host; each client's
	// character sheet reads its own copy. This is an `all` function (sam_mp_contracts.inc): a host
	// call runs on every client too, and a client that joins later is sent the host's whole table
	// (SAMCombat::sendResistSnapshot). A client cannot call it itself.
	int lua_sam_set_species_damage_resist(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* speciesC = luaL_checkstring(Ls, 1);
		const char* typeC = luaL_checkstring(Ls, 2);
		const double mult = (double)luaL_checknumber(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		const int species = samMonsterNameToId(speciesC);
		if ( species < 0 )
		{
			SAM_ERROR("LUA", std::string("sam_set_species_damage_resist: '")
				+ (speciesC ? speciesC : "") + "' is not a creature this game has.");
			lua_pushboolean(Ls, 0); return 1;
		}
		int dtype = 0;
		if ( !samDamageTypeArg(typeC, "sam_set_species_damage_resist", &dtype) )
		{
			lua_pushboolean(Ls, 0); return 1;
		}
		lua_pushboolean(Ls, SAMCombat::setSpeciesResist(species, dtype, mult) ? 1 : 0);
		return 1;
#else
		(void)speciesC; (void)typeC; (void)mult; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_clear_species_damage_resist([species [, type]]) -> how many overrides were removed.
	// No arguments clears everything, which is what a mod's teardown wants.
	int lua_sam_clear_species_damage_resist(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* speciesC = luaL_optstring(Ls, 1, "");
		const char* typeC = luaL_optstring(Ls, 2, "");
#ifdef SAM_LUA_HAVE_BARONY
		if ( !speciesC || !*speciesC )
		{
			lua_pushinteger(Ls, (lua_Integer)SAMCombat::clearAllSpeciesResist());
			return 1;
		}
		const int species = samMonsterNameToId(speciesC);
		if ( species < 0 )
		{
			SAM_ERROR("LUA", std::string("sam_clear_species_damage_resist: '") + speciesC
				+ "' is not a creature this game has.");
			lua_pushinteger(Ls, 0); return 1;
		}
		// A missing type means every type for that species, which clearSpeciesResist expresses
		// as an out-of-range type rather than a second entry point.
		int dtype = -1;
		if ( typeC && *typeC )
		{
			if ( !samDamageTypeArg(typeC, "sam_clear_species_damage_resist", &dtype) )
			{
				lua_pushinteger(Ls, 0); return 1;
			}
		}
		lua_pushinteger(Ls, (lua_Integer)SAMCombat::clearSpeciesResist(species, dtype));
		return 1;
#else
		(void)speciesC; (void)typeC; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_add_damage_multiplier(fraction) -> true if the contribution was taken.
	//
	// Valid ONLY inside an on_damage_multiplier handler, and it says so rather than silently
	// dropping the number — a contribution made anywhere else has no window to land in and
	// would otherwise vanish without a word.
	//
	// Positives ADD and negatives MULTIPLY, which is Barony's own rule for the bonus pool it
	// builds in getDamageTableMultiplier. Two mods each contributing +0.2 give +40%; two each
	// contributing -0.5 give a quarter rather than nothing.
	int lua_sam_add_damage_multiplier(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const double f = (double)luaL_checknumber(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		if ( !SAMCombat::multiplierHookActive() )
		{
			SAM_WARN("LUA", "sam_add_damage_multiplier: no damage is being resolved right now, so"
				" there is nothing to contribute to. It works inside an on_damage_multiplier"
				" handler; to change one specific hit use sam_modify_damage (player) or"
				" sam_modify_monster_damage (monster).");
			lua_pushboolean(Ls, 0); return 1;
		}
		if ( !std::isfinite(f) )
		{
			SAM_ERROR("LUA", "sam_add_damage_multiplier: the contribution has to be a finite number.");
			lua_pushboolean(Ls, 0); return 1;
		}
		SAMCombat::addMultiplier(f);
		lua_pushboolean(Ls, 1);
		return 1;
#else
		(void)f; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// =====================================================================================
	//  CAMERA (v3.0.0)
	//
	//  Every one of these is LOCAL. A camera belongs to the machine that draws it, so these
	//  are not host-only like most of the API -- a client running a camera mod for itself is
	//  the normal case, and naming another player's slot is refused rather than silently
	//  doing nothing.
	//
	//  Everything is in TILES. See sam_camera.hpp for why the vertical needed converting.
	// =====================================================================================

	// sam_set_camera_offset(player, back [, up [, right]]) -> boolean
	//
	// Put the camera behind the player and keep it there. `back` tiles behind them, `up`
	// tiles above, `right` tiles to their right. It follows their yaw and pitch, so looking
	// down swings the camera up and over rather than sliding it along the floor, and the
	// boom shortens when a wall is in the way.
	//
	// This is the whole of a third-person camera. Barony's own /thirdperson only sets a flag
	// that makes your body visible and stops the camera being updated at all, which is why it
	// stays where you turned it on.
	//
	// Pair it with sam_show_own_body(player, true), or you will be looking at the back of an
	// invisible character.
	int lua_sam_set_camera_offset(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const double back = (double)luaL_checknumber(Ls, 2);
		const double up = (double)luaL_optnumber(Ls, 3, 0.0);
		const double right = (double)luaL_optnumber(Ls, 4, 0.0);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, SAMCamera::setOffset(player, back, up, right) ? 1 : 0);
		return 1;
#else
		(void)player; (void)back; (void)up; (void)right; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_camera_position(player, x, y [, height]) -> boolean
	//
	// Pin the camera in the world instead of on the player: a security camera, a cutscene, a
	// fixed view of a room. x and y are tiles, height is tiles above the floor -- a standing
	// eye is about 0.64, and a tile-high room puts the ceiling at 1.
	//
	// It keeps looking wherever the player looks unless you also call sam_set_camera_angle or
	// sam_set_camera_target, which is usually what you want for a fixed camera.
	int lua_sam_set_camera_position(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const double x = (double)luaL_checknumber(Ls, 2);
		const double y = (double)luaL_checknumber(Ls, 3);
		const double h = (double)luaL_optnumber(Ls, 4, 0.64);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, SAMCamera::setPosition(player, x, y, h) ? 1 : 0);
		return 1;
#else
		(void)player; (void)x; (void)y; (void)h; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_camera_angle(player [, yaw, pitch]) -> boolean
	//
	// Point the camera somewhere fixed. Yaw is radians the same way sam_get_facing reports
	// them; pitch is positive DOWNWARD, which is Barony's convention, and is held inside a
	// right angle so the view cannot roll over.
	//
	// Called with no angle it goes back to following the player's own look, which is what the
	// mouse drives.
	int lua_sam_set_camera_angle(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		if ( lua_isnoneornil(Ls, 2) )
		{
			lua_pushboolean(Ls, SAMCamera::clearAngle(player) ? 1 : 0);
			return 1;
		}
		const double yaw = (double)luaL_checknumber(Ls, 2);
		const double pitch = (double)luaL_optnumber(Ls, 3, 0.0);
		lua_pushboolean(Ls, SAMCamera::setAngle(player, yaw, pitch) ? 1 : 0);
		return 1;
#else
		(void)player; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_camera_target(player, uid) -> boolean
	//
	// Keep the camera pointed at something, recomputed every frame so it tracks. Pass 0 to
	// stop. If the thing dies the camera holds its last angle rather than snapping, which is
	// what a camera operator would do, and stops tracking.
	int lua_sam_set_camera_target(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const long long uid = (long long)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, SAMCamera::setTarget(player, uid) ? 1 : 0);
		return 1;
#else
		(void)player; (void)uid; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_camera_collision(player, on) -> boolean
	//
	// Whether the boom shortens when a wall is between the player and the camera. On by
	// default, and it uses the engine's own line trace so it agrees with what the level
	// really blocks. Turn it off for a camera that is meant to pass through geometry.
	int lua_sam_set_camera_collision(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		bool on = true;
		if ( !samBoolReq(Ls, 2, "sam_set_camera_collision", &on) ) { lua_pushboolean(Ls, 0); return 1; }
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, SAMCamera::setCollision(player, on) ? 1 : 0);
		return 1;
#else
		(void)player; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_get_camera(player) -> table { x, y, height, yaw, pitch, mode } or nil
	//
	// Where the camera actually IS, not what was asked for. Those differ the moment the boom
	// hits a wall, and the difference is the whole reason this reader exists: a mod placing a
	// camera has no other way to find out whether it got there.
	//
	// mode is "vanilla", "orbit" or "absolute".
	int lua_sam_get_camera(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		double x = 0, y = 0, h = 0, yaw = 0, pitch = 0;
		int mode = 0;
		if ( !SAMCamera::get(player, &x, &y, &h, &yaw, &pitch, &mode) ) { lua_pushnil(Ls); return 1; }
		lua_newtable(Ls);
		lua_pushnumber(Ls, x);     lua_setfield(Ls, -2, "x");
		lua_pushnumber(Ls, y);     lua_setfield(Ls, -2, "y");
		lua_pushnumber(Ls, h);     lua_setfield(Ls, -2, "height");
		lua_pushnumber(Ls, yaw);   lua_setfield(Ls, -2, "yaw");
		lua_pushnumber(Ls, pitch); lua_setfield(Ls, -2, "pitch");
		lua_pushstring(Ls, mode == 1 ? "orbit" : ( mode == 2 ? "absolute" : "vanilla" ));
		lua_setfield(Ls, -2, "mode");
		return 1;
#else
		(void)player; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_reset_camera(player) -> true if anything was being overridden.
	// Hands the camera back to the engine. Your mod should call this when it unloads.
	int lua_sam_reset_camera(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, SAMCamera::reset(player) ? 1 : 0);
		return 1;
#else
		(void)player; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_show_own_body(player, on) -> boolean
	//
	// Whether you can see your own character. Normally you cannot: the camera is inside your
	// head, so the engine hides your body and draws a first-person weapon instead. Turning
	// this on shows the body and hides that weapon, which is what any camera outside the head
	// needs.
	//
	// It is the engine's own switch -- the one /thirdperson flips -- so it costs nothing and
	// behaves exactly as the game already does. What /thirdperson does NOT do is move the
	// camera; pair this with sam_set_camera_offset for that.
	//
	// Value 2 of that switch belongs to the death camera and the project-spirit effect, and
	// is refused here rather than overwritten: taking it would strand a dying player.
	int lua_sam_show_own_body(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		bool on = true;
		if ( !samBoolReq(Ls, 2, "sam_show_own_body", &on) ) { lua_pushboolean(Ls, 0); return 1; }
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushboolean(Ls, SAMCamera::showOwnBody(player, on) ? 1 : 0);
		return 1;
#else
		(void)player; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// =====================================================================================
	//  THE RULES (v3.0.0): stat modifiers, effect immunity, the XP curve
	//
	//  The idea these share: a value Barony computes for itself, which a mod should be able to
	//  influence WITHOUT overwriting what another mod did. Every modifier carries an id so a
	//  mod can take back its own contribution and nobody else's.
	// =====================================================================================

	// An integer argument that does not fit in 32 bits is refused here rather than wrapped.
	// `(int)luaL_checkinteger` quietly turned 4294967296 into player slot 0 and 5e9 EXP into
	// 705 million, while the JS twin turned the same numbers into INT_MIN -- two different wrong
	// answers. Raises like any other bad argument.
	static int samRuleInt(lua_State* Ls, int idx)
	{
		const lua_Integer v = luaL_checkinteger(Ls, idx);
		if ( v < -2147483647LL - 1 || v > 2147483647LL )
		{
			luaL_argerror(Ls, idx, "does not fit in a 32-bit integer");
		}
		return (int)v;
	}

#ifdef SAM_LUA_HAVE_BARONY
	// A stat name -> the kind SAMRules indexes by, with the refusal written once.
	static bool samStatKindArg(const char* name, const char* who, int* out)
	{
		const int k = SAMRules::statKindFromName(name);
		if ( k >= 0 ) { *out = k; return true; }
		SAM_ERROR("LUA", std::string(who) + ": '" + (name ? name : "") + "' is not a stat this can"
			" modify. The nine are STR, DEX, CON, INT, PER, CHR, AC, ATTACK and SPEED. The rest"
			" are stored numbers rather than computed ones, so a modifier on them would be"
			" overwritten by the next write -- use sam_set_stat for those.");
		return false;
	}
#endif

	// sam_add_stat_modifier(player, stat, id, add [, multiply]) -> boolean
	//
	// Contribute to one of the player's computed stats. Adds are summed and multipliers are
	// multiplied ACROSS EVERY MOD, then applied as (base + adds) * multipliers -- so two mods
	// each giving +2 STR give +4, and two each halving give a quarter. Neither mod has to know
	// the other exists, which is the whole point.
	//
	// The id is yours. Adding again with the same id REPLACES that contribution, which is what
	// makes a per-tick "recalculate my buff" loop safe; sam_remove_stat_modifier takes back
	// everything under that id and touches nothing else.
	//
	// Survives floors. A player's entity is rebuilt on the stairs and its uid changes, so these
	// are keyed by player slot rather than by uid.
	int lua_sam_add_stat_modifier(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = samRuleInt(Ls, 1);
		const char* statC = luaL_checkstring(Ls, 2);
		const char* idC = luaL_checkstring(Ls, 3);
		const double add = (double)luaL_optnumber(Ls, 4, 0.0);
		const double mult = (double)luaL_optnumber(Ls, 5, 1.0);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_add_stat_modifier refused: host only. The host owns the table and"
				" sends the totals on, so a client setting one would be overwritten by the next"
				" update and disagree with everyone else in the meantime.");
			lua_pushboolean(Ls, 0); return 1;
		}
		int kind = 0;
		if ( !samStatKindArg(statC, "sam_add_stat_modifier", &kind) ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMRules::addPlayerModifier(player, kind, g_currentNs.c_str(), idC, add, mult) ? 1 : 0);
		return 1;
#else
		(void)player; (void)statC; (void)idC; (void)add; (void)mult; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_add_monster_stat_modifier(uid, stat, id, add [, multiply]) -> boolean
	//
	// The same for one creature. DIES WITH THE FLOOR: monster tables are dropped on every level
	// change, because the engine reuses entity uids and a remembered one can come to name
	// something else entirely.
	//
	// All nine stats work on a monster. SPEED scales how fast it moves, through the one factor
	// every movement path in actMonster multiplies by.
	int lua_sam_add_monster_stat_modifier(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* statC = luaL_checkstring(Ls, 2);
		const char* idC = luaL_checkstring(Ls, 3);
		const double add = (double)luaL_optnumber(Ls, 4, 0.0);
		const double mult = (double)luaL_optnumber(Ls, 5, 1.0);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("LUA", "sam_add_monster_stat_modifier refused: host only.");
			lua_pushboolean(Ls, 0); return 1;
		}
		int kind = 0;
		if ( !samStatKindArg(statC, "sam_add_monster_stat_modifier", &kind) ) { lua_pushboolean(Ls, 0); return 1; }
		// A uid that names no living monster would be stored and never matched, so the call
		// would say yes and change nothing. A player's uid is refused too: players are keyed by
		// slot, and sam_add_stat_modifier is the function for them.
		if ( !samResolveMonster(uid) )
		{
			SAM_ERROR("LUA", "sam_add_monster_stat_modifier: uid " + std::to_string(uid)
				+ " is not a living monster. For a player, use sam_add_stat_modifier.");
			lua_pushboolean(Ls, 0); return 1;
		}
		lua_pushboolean(Ls, SAMRules::addMonsterModifier(uid, kind, g_currentNs.c_str(), idC, add, mult) ? 1 : 0);
		return 1;
#else
		(void)uid; (void)statC; (void)idC; (void)add; (void)mult; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_remove_stat_modifier(player, id) -> how many stats carried that id.
	int lua_sam_remove_stat_modifier(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = samRuleInt(Ls, 1);
		const char* idC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushinteger(Ls, (lua_Integer)SAMRules::removePlayerModifier(player, g_currentNs.c_str(), idC));
		return 1;
#else
		(void)player; (void)idC; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_remove_monster_stat_modifier(uid, id) -> how many stats carried that id.
	int lua_sam_remove_monster_stat_modifier(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* idC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushinteger(Ls, (lua_Integer)SAMRules::removeMonsterModifier(uid, g_currentNs.c_str(), idC));
		return 1;
#else
		(void)uid; (void)idC; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_clear_stat_modifiers([player]) -> how many were removed. Takes back THIS MOD's
	// modifiers only: from one player, or with no argument from every player and every monster,
	// which is what a mod's teardown wants. Another mod's contributions are not this mod's to take.
	int lua_sam_clear_stat_modifiers(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = lua_isnoneornil(Ls, 1) ? -1 : samRuleInt(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		int n = SAMRules::clearPlayerModifiers(player, g_currentNs.c_str());
		if ( player < 0 ) { n += SAMRules::clearMonsterModifiers(0, g_currentNs.c_str()); }
		lua_pushinteger(Ls, (lua_Integer)n);
		return 1;
#else
		(void)player; lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_get_stat_modifier(player, stat, id) -> table { add, multiply } or nil.
	// Reads back what YOUR id contributes, so a mod does not have to remember.
	int lua_sam_get_stat_modifier(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = samRuleInt(Ls, 1);
		const char* statC = luaL_checkstring(Ls, 2);
		const char* idC = luaL_checkstring(Ls, 3);
#ifdef SAM_LUA_HAVE_BARONY
		int kind = 0;
		if ( !samStatKindArg(statC, "sam_get_stat_modifier", &kind) ) { lua_pushnil(Ls); return 1; }
		double add = 0.0, mult = 1.0;
		if ( !SAMRules::getPlayerModifier(player, kind, g_currentNs.c_str(), idC, &add, &mult) ) { lua_pushnil(Ls); return 1; }
		lua_newtable(Ls);
		lua_pushnumber(Ls, add);  lua_setfield(Ls, -2, "add");
		lua_pushnumber(Ls, mult); lua_setfield(Ls, -2, "multiply");
		return 1;
#else
		(void)player; (void)statC; (void)idC; lua_pushnil(Ls); return 1;
#endif
	}

	// sam_set_immunity(player, effect [, on]) -> boolean
	//
	// Make a player immune to a named effect. Barony's own immunities are a hardcoded species
	// switch inside setEffect with no table and no way to ask; this is one a mod owns, checked
	// before that switch runs.
	int lua_sam_set_immunity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = samRuleInt(Ls, 1);
		const char* effC = luaL_checkstring(Ls, 2);
		const bool on = samBoolArg(Ls, 3, true);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_immunity refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(effC);
		if ( eff < 0 )
		{
			SAM_ERROR("LUA", std::string("sam_set_immunity: unknown effect '") + (effC ? effC : "")
				+ "'. Valid: " + samEffectNameHint());
			lua_pushboolean(Ls, 0); return 1;
		}
		lua_pushboolean(Ls, SAMRules::setPlayerImmunity(player, eff, on, g_currentNs.c_str()) ? 1 : 0);
		return 1;
#else
		(void)player; (void)effC; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_monster_immunity(uid, effect [, on]) -> boolean. Dies with the floor.
	int lua_sam_set_monster_immunity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* effC = luaL_checkstring(Ls, 2);
		const bool on = samBoolArg(Ls, 3, true);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_monster_immunity refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		const int eff = samEffectNameToId(effC);
		if ( eff < 0 )
		{
			SAM_ERROR("LUA", std::string("sam_set_monster_immunity: unknown effect '") + (effC ? effC : "")
				+ "'. Valid: " + samEffectNameHint());
			lua_pushboolean(Ls, 0); return 1;
		}
		// Only when turning it ON: a teardown switching off a creature that has since died is
		// harmless and should not be shouted at.
		if ( on && !samResolveMonster(uid) )
		{
			SAM_ERROR("LUA", "sam_set_monster_immunity: uid " + std::to_string(uid)
				+ " is not a living monster. For a player, use sam_set_immunity.");
			lua_pushboolean(Ls, 0); return 1;
		}
		lua_pushboolean(Ls, SAMRules::setMonsterImmunity(uid, eff, on, g_currentNs.c_str()) ? 1 : 0);
		return 1;
#else
		(void)uid; (void)effC; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_species_immunity(species, effect [, on]) -> boolean
	// Every creature of that kind, now and later. Unlike the per-creature form this survives
	// floors, because a species is not a uid.
	int lua_sam_set_species_immunity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* speciesC = luaL_checkstring(Ls, 1);
		const char* effC = luaL_checkstring(Ls, 2);
		const bool on = samBoolArg(Ls, 3, true);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_species_immunity refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		const int species = samMonsterNameToId(speciesC);
		if ( species < 0 )
		{
			SAM_ERROR("LUA", std::string("sam_set_species_immunity: '") + (speciesC ? speciesC : "")
				+ "' is not a creature this game has.");
			lua_pushboolean(Ls, 0); return 1;
		}
		const int eff = samEffectNameToId(effC);
		if ( eff < 0 )
		{
			SAM_ERROR("LUA", std::string("sam_set_species_immunity: unknown effect '") + (effC ? effC : "")
				+ "'. Valid: " + samEffectNameHint());
			lua_pushboolean(Ls, 0); return 1;
		}
		lua_pushboolean(Ls, SAMRules::setSpeciesImmunity(species, eff, on, g_currentNs.c_str()) ? 1 : 0);
		return 1;
#else
		(void)speciesC; (void)effC; (void)on; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_is_immune(uid, effect) -> boolean
	//
	// Whether a mod has made this creature immune, so a script can ask before wasting a cast --
	// which is impossible in vanilla. It answers about OUR table only: Barony's own immunities
	// are a switch with no way to query it, so a false here does not promise the effect will
	// land.
	int lua_sam_is_immune(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const char* effC = luaL_checkstring(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		const int eff = samEffectNameToId(effC);
		if ( eff < 0 ) { lua_pushboolean(Ls, 0); return 1; }
		Entity* e = samResolveEntityQuiet(uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMRules::isImmune(e->getStats(), e, eff) ? 1 : 0);
		return 1;
#else
		(void)uid; (void)effC; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_clear_immunities() -> how many were removed. This mod's immunities only: a creature
	// another mod also made immune stays immune.
	int lua_sam_clear_immunities(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushinteger(Ls, (lua_Integer)SAMRules::clearImmunities(g_currentNs.c_str()));
		return 1;
#else
		lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_set_xp_curve(level, threshold) -> boolean
	//
	// How much experience the next level costs. Barony's is a flat 100 at every level, with the
	// vanilla scaling literally commented out beside it in the engine, so every progression mod
	// has had to fake it by writing EXP directly.
	//
	// Level -1 sets the flat value for EVERY level, which is the one-line version of a slower or
	// faster game. Naming a level overrides just that one, so a curve is a handful of calls.
	int lua_sam_set_xp_curve(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int level = samRuleInt(Ls, 1);
		const int threshold = samRuleInt(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_xp_curve refused: host only. Only the host runs the level-up branch."); lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMRules::setXpThreshold(level, threshold, g_currentNs.c_str()) ? 1 : 0);
		return 1;
#else
		(void)level; (void)threshold; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_clear_xp_curve() -> how many levels were overridden. Back to a flat 100.
	int lua_sam_clear_xp_curve(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushinteger(Ls, (lua_Integer)SAMRules::clearXpCurve(g_currentNs.c_str()));
		return 1;
#else
		lua_pushinteger(Ls, 0); return 1;
#endif
	}

	// sam_get_xp_threshold(level) -> what that level costs right now. 100 unless a mod said
	// otherwise, so this is also how to read vanilla's number.
	int lua_sam_get_xp_threshold(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int level = samRuleInt(Ls, 1);
#ifdef SAM_LUA_HAVE_BARONY
		lua_pushinteger(Ls, (lua_Integer)SAMRules::xpThreshold(level));
		return 1;
#else
		(void)level; lua_pushinteger(Ls, 100); return 1;
#endif
	}

	// sam_grant_xp(player, amount) -> the player's EXP afterwards, or nil.
	//
	// Nothing granted an arbitrary amount before this: sam_level_up adds whole levels and
	// sam_set_stat("EXP") is an absolute write. Crossing the threshold levels the player up
	// naturally on the next tick, firing player.on_level_up, and respects a curve set above.
	int lua_sam_grant_xp(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = samRuleInt(Ls, 1);
		const int amount = samRuleInt(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_grant_xp refused: host only."); lua_pushnil(Ls); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] )
		{
			SAM_ERROR("LUA", "sam_grant_xp: invalid player index " + std::to_string(player) + ".");
			lua_pushnil(Ls); return 1;
		}
		long long v = (long long)stats[player]->EXP + (long long)amount;
		if ( v < 0 ) { v = 0; }
		if ( v > 2147483647LL ) { v = 2147483647LL; }
		stats[player]->EXP = (Sint32)v;
		// EXP and LVL both ride the existing 'ATTR' payload, so the owning client sees it
		// without a new packet.
		SAMLua::flushStatToClient(player);
		lua_pushinteger(Ls, (lua_Integer)stats[player]->EXP);
		return 1;
#else
		(void)player; (void)amount; lua_pushnil(Ls); return 1;
#endif
	}

	// ===== P_ITEMS: the loot pool (sam_loot.cpp) =============================================
	//
	// Categories are the names /sam_loot takes (WEAPON ARMOR AMULET POTION SCROLL MAGICSTAFF RING
	// SPELLBOOK GEM THROWN TOOL FOOD BOOK) or the numeric Category; contexts are floor / chest /
	// shop / monster / recipe / console / other. Items resolve as everywhere else: a numeric id,
	// a vanilla name or "ns:item". Every table writer is `all`: the host's call reaches every
	// S.A.M client, because each client rolls its own floor from the same seed.

#ifdef SAM_LUA_HAVE_BARONY
	// A category argument: a name or the numeric Category; -1 when it is neither. With allowAny,
	// "ANY" sets isAny and returns -1 (sam_set_loot_fallback takes it for the lockpick reward's
	// any-category draw).
	static int samLootCategoryArg(lua_State* Ls, int idx, bool allowAny, bool& isAny)
	{
		isAny = false;
		if ( lua_type(Ls, idx) == LUA_TNUMBER ) { return (int)lua_tointeger(Ls, idx); }
		const char* s = lua_isstring(Ls, idx) ? lua_tostring(Ls, idx) : nullptr;
		if ( !s ) { return -1; }
		if ( allowAny && SAMLoot::isAnyCategoryName(s) ) { isAny = true; return -1; }
		return SAMLoot::categoryFromName(s);
	}

	// A context argument: -1 when absent or nil (each function says what that means for it),
	// -2 for a word that is not a context (logged), else the kind.
	static int samLootContextArg(lua_State* Ls, int idx, const char* who)
	{
		if ( lua_isnoneornil(Ls, idx) ) { return -1; }
		const char* s = lua_isstring(Ls, idx) ? lua_tostring(Ls, idx) : nullptr;
		const int kind = s ? SAMLoot::kindFromName(s) : -1;
		if ( kind < 0 )
		{
			SAM_ERROR("LUA", std::string(who) + ": '" + (s ? s : "?")
				+ "' is not a loot context. Valid: floor chest shop monster recipe console other.");
			return -2;
		}
		return kind;
	}
#endif

	// sam_get_loot_pool(category, min_level, max_level [, context]) -> array of { type, name,
	// level, weight } | nil. The exact candidate set itemLevelCurve would draw from for that
	// window: the sheet's level test, sam_patch_item levels, mod items, and the loot tables
	// (weight 0, floor window, context) applied. Without a context the context rule is not
	// tested. nil for a category that is not one.
	int lua_sam_get_loot_pool(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		bool any = false;
		const int cat = samLootCategoryArg(Ls, 1, false, any);
		const int minLevel = (int)luaL_checkinteger(Ls, 2);
		const int maxLevel = (int)luaL_checkinteger(Ls, 3);
		const int kind = samLootContextArg(Ls, 4, "sam_get_loot_pool");
		if ( kind == -2 ) { lua_pushnil(Ls); return 1; }
		std::vector<SAMLoot::PoolEntry> out;
		if ( !SAMLoot::pool(cat, minLevel, maxLevel, kind, out) ) { lua_pushnil(Ls); return 1; }
		lua_newtable(Ls);
		int n = 0;
		for ( const SAMLoot::PoolEntry& e : out )
		{
			lua_newtable(Ls);
			lua_pushinteger(Ls, e.type);         lua_setfield(Ls, -2, "type");
			lua_pushstring(Ls, e.name.c_str());  lua_setfield(Ls, -2, "name");
			lua_pushinteger(Ls, e.level);        lua_setfield(Ls, -2, "level");
			lua_pushinteger(Ls, e.weight);       lua_setfield(Ls, -2, "weight");
			lua_rawseti(Ls, -2, ++n);
		}
		return 1;
#else
		lua_pushnil(Ls); return 1;
#endif
	}

	// sam_set_loot_weight(item, weight) -> boolean. 1 is vanilla, 0 is never, N is N times as
	// likely as a weight-1 item in the same draw. A table of only 0s and 1s keeps the vanilla
	// RNG stream (the pick stays rng.rand() % n); any weight above 1 makes it a weighted draw.
	int lua_sam_set_loot_weight(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const int id = samResolveItemArg(Ls, 1);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_set_loot_weight: unknown item."); lua_pushboolean(Ls, 0); return 1; }
		const int weight = (int)luaL_checkinteger(Ls, 2);
		lua_pushboolean(Ls, SAMLoot::setWeight(id, weight) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_loot_category_weight(category, weight) -> boolean. The "any category" draw a
	// floor item, a random chest, the general store and a troll's hoard make. With a weight set
	// that draw becomes ONE weighted pick and the vanilla THROWN/BOOK re-roll quirks are gone
	// for it; with none set it is the vanilla code, re-rolls included.
	int lua_sam_set_loot_category_weight(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		bool any = false;
		const int cat = samLootCategoryArg(Ls, 1, false, any);
		const int weight = (int)luaL_checkinteger(Ls, 2);
		lua_pushboolean(Ls, SAMLoot::setCategoryWeight(cat, weight) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_loot_floor_range(item, min_floor [, max_floor]) -> boolean. A window on the
	// CURRENT FLOOR checked beside the sheet's level: max_floor is the ceiling vanilla never had.
	// Leave max_floor out for none; (item, 0) removes the rule.
	int lua_sam_set_loot_floor_range(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const int id = samResolveItemArg(Ls, 1);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_set_loot_floor_range: unknown item."); lua_pushboolean(Ls, 0); return 1; }
		const int minFloor = (int)luaL_checkinteger(Ls, 2);
		const int maxFloor = (int)luaL_optinteger(Ls, 3, -1);
		lua_pushboolean(Ls, SAMLoot::setFloorRange(id, minFloor, maxFloor) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_loot_context(item, context, allowed) -> boolean. Keep an item out of (false) or
	// back in (true, the default) one kind of roll: floor chest shop monster recipe console other.
	int lua_sam_set_loot_context(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const int id = samResolveItemArg(Ls, 1);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_set_loot_context: unknown item."); lua_pushboolean(Ls, 0); return 1; }
		const int kind = samLootContextArg(Ls, 2, "sam_set_loot_context");
		if ( kind < 0 )
		{
			if ( kind == -1 ) { SAM_ERROR("LUA", "sam_set_loot_context: argument 2 (the context) is required."); }
			lua_pushboolean(Ls, 0); return 1;
		}
		bool allowed = false;
		if ( !samBoolReq(Ls, 3, "sam_set_loot_context", &allowed) ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMLoot::setContextAllowed(id, kind, allowed) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_spell_droppable(spell, allowed [, min_floor]) -> boolean. Overrides the sheet's
	// drop_table / hidden rule for one spell in the spellbook re-roll: its book (or tome) may
	// come out of a spellbook roll from min_floor (default 0), or never. A SPELL_ name,
	// "namespace:spell" or a spell id.
	int lua_sam_set_spell_droppable(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		int spell = -1;
		if ( lua_type(Ls, 1) == LUA_TNUMBER ) { spell = (int)lua_tointeger(Ls, 1); }
		else if ( lua_isstring(Ls, 1) ) { spell = SAMSpells::resolveSpellRef(lua_tostring(Ls, 1)); }
		if ( spell < 0 )
		{
			SAM_ERROR("LUA", "sam_set_spell_droppable: unknown spell (expected a SPELL_ name, \"namespace:spell\" or a spell id).");
			lua_pushboolean(Ls, 0); return 1;
		}
		bool allowed = false;
		if ( !samBoolReq(Ls, 2, "sam_set_spell_droppable", &allowed) ) { lua_pushboolean(Ls, 0); return 1; }
		const int minFloor = (int)luaL_optinteger(Ls, 3, 0);
		lua_pushboolean(Ls, SAMLoot::setSpellDroppable(spell, allowed, minFloor) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_loot_fallback(category, item [, context]) -> boolean. What an EMPTY pool returns
	// instead of GEM_ROCK, for that category ("ANY" is the any-category draw the lockpick reward
	// uses) and that context (nil = every context). item nil puts vanilla back.
	int lua_sam_set_loot_fallback(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		bool any = false;
		const int cat = samLootCategoryArg(Ls, 1, true, any);
		if ( cat < 0 && !any )
		{
			SAM_ERROR("LUA", "sam_set_loot_fallback: not a category. Valid: ANY WEAPON ARMOR AMULET POTION SCROLL MAGICSTAFF RING SPELLBOOK GEM THROWN TOOL FOOD BOOK");
			lua_pushboolean(Ls, 0); return 1;
		}
		int item = -1;
		if ( !lua_isnoneornil(Ls, 2) )
		{
			item = samResolveItemArg(Ls, 2);
			if ( item < 0 ) { SAM_ERROR("LUA", "sam_set_loot_fallback: unknown item."); lua_pushboolean(Ls, 0); return 1; }
		}
		const int kind = samLootContextArg(Ls, 3, "sam_set_loot_fallback");
		if ( kind == -2 ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, SAMLoot::setFallback(cat, item, kind) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_roll_loot(category, min_level, max_level [, context]) -> item type | nothing. ONE roll
	// of the engine's own curve, the tables, both loot events and the spellbook re-roll included,
	// from the game's own local generator, the gameplay stream: a seeded run's floors come from
	// the seed and are unchanged, an unseeded run's next floor seed moves on as after any attack
	// roll. Nothing for a bad category, on a client, or from inside a loot event handler (a
	// nested roll would wipe the outer handler's write-backs).
	int lua_sam_roll_loot(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		bool any = false;
		const int cat = samLootCategoryArg(Ls, 1, false, any);
		const int minLevel = (int)luaL_checkinteger(Ls, 2);
		const int maxLevel = (int)luaL_checkinteger(Ls, 3);
		const int kind = samLootContextArg(Ls, 4, "sam_roll_loot");
		if ( kind == -2 ) { return 0; }
		const int type = SAMLoot::roll(cat, minLevel, maxLevel, kind);
		if ( type < 0 ) { return 0; }
		lua_pushinteger(Ls, type);
		return 1;
#else
		return 0;
#endif
	}

	// sam_add_item_to_container(uid, item [, count [, status [, beatitude [, identified [, appearance]]]]])
	//   -> the new item's uid | nil. A chest (open or closed), a creature's pockets or a shop's
	// stock (laid out again, consumables kept). Defaults: 1, EXCELLENT, 0, identified, random.
	int lua_sam_add_item_to_container(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int type = samResolveItemArg(Ls, 2);
		if ( type < 0 ) { SAM_ERROR("LUA", "sam_add_item_to_container: unknown item."); lua_pushnil(Ls); return 1; }
		const int count = (int)luaL_optinteger(Ls, 3, 1);
		const int status = (int)luaL_optinteger(Ls, 4, (int)EXCELLENT);
		const int beatitude = (int)luaL_optinteger(Ls, 5, 0);
		const bool identified = samBoolArg(Ls, 6, true);
		const long long appearance = (long long)luaL_optinteger(Ls, 7, -1);
		const long long itemUid = SAMLoot::addToContainer((std::uint32_t)uid, type, count, status, beatitude, identified, appearance);
		if ( itemUid < 0 ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)itemUid);
		return 1;
#else
		lua_pushnil(Ls); return 1;
#endif
	}

	// sam_remove_item_from_container(uid, item_or_uid [, count]) -> boolean. An item uid from
	// the container's own list, or a type (name or id): the first stack of that type. count 0 or
	// omitted removes the whole stack. An open chest is closed first (there is no "the host took
	// this out" packet); an open shop window is closed the same way.
	int lua_sam_remove_item_from_container(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		long long what = -1;
		if ( lua_type(Ls, 2) == LUA_TNUMBER ) { what = (long long)lua_tointeger(Ls, 2); }
		else
		{
			what = samResolveItemArg(Ls, 2);
			if ( what < 0 ) { SAM_ERROR("LUA", "sam_remove_item_from_container: unknown item."); lua_pushboolean(Ls, 0); return 1; }
		}
		const int count = (int)luaL_optinteger(Ls, 3, 0);
		lua_pushboolean(Ls, SAMLoot::removeFromContainer((std::uint32_t)uid, what, count) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// Push t[key] with the key matched case-insensitively, nil when absent: the exact spelling
	// first (one lookup, the common case), then a walk of the table's string keys. This is the
	// rule every other table this file takes already follows (sam_patch_item and
	// sam_register_setting upper-case their keys) and the rule the JS twin applies through
	// samJsGetPropCI, so that { Item = "IRON_SWORD", Count = 5 } means the same thing in both
	// languages; it used to stock five swords in JS and resolve nothing in Lua. Leaves the stack
	// as it was plus the one pushed value, and needs three free slots.
	static void samLuaGetFieldCI(lua_State* Ls, int idx, const char* key)
	{
		idx = lua_absindex(Ls, idx);
		lua_getfield(Ls, idx, key);
		if ( !lua_isnil(Ls, -1) ) { return; }
		lua_pop(Ls, 1);
		const std::string want = samUpper(key);
		lua_pushnil(Ls);
		while ( lua_next(Ls, idx) != 0 )
		{
			if ( lua_type(Ls, -2) == LUA_TSTRING && samUpper(lua_tostring(Ls, -2)) == want )
			{
				lua_remove(Ls, -2);   // keep the value, drop the key
				return;
			}
			lua_pop(Ls, 1);
		}
		lua_pushnil(Ls);
	}

	// sam_set_shop_stock(shopkeeper_uid, items) -> boolean. Replaces the stock (the data-driven
	// consumables stay) with `items`: an array of { item, count, status, beatitude, identified }
	// tables or bare item names / ids, then runs the price-sorted layout again. The whole list is
	// checked first: a bad entry changes nothing and names itself.
	int lua_sam_set_shop_stock(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		luaL_checktype(Ls, 2, LUA_TTABLE);
		if ( !lua_checkstack(Ls, 6) ) { SAM_ERROR("LUA", "sam_set_shop_stock: Lua stack exhausted."); lua_pushboolean(Ls, 0); return 1; }
		std::vector<SAMLoot::StockEntry> entries;
		const lua_Integer n = (lua_Integer)lua_rawlen(Ls, 2);
		for ( lua_Integer i = 1; i <= n; ++i )
		{
			lua_rawgeti(Ls, 2, i);
			const int entryIdx = lua_gettop(Ls);
			SAMLoot::StockEntry en;
			if ( lua_istable(Ls, entryIdx) )
			{
				// Keys case-insensitive, as the JS twin reads them and as every other table here is read.
				samLuaGetFieldCI(Ls, entryIdx, "item");
				if ( lua_isnil(Ls, -1) ) { lua_pop(Ls, 1); samLuaGetFieldCI(Ls, entryIdx, "type"); }
				en.type = samResolveItemArg(Ls, lua_gettop(Ls));
				lua_pop(Ls, 1);
				samLuaGetFieldCI(Ls, entryIdx, "count");     if ( lua_type(Ls, -1) == LUA_TNUMBER ) { en.count = (int)lua_tointeger(Ls, -1); }     lua_pop(Ls, 1);
				samLuaGetFieldCI(Ls, entryIdx, "status");    if ( lua_type(Ls, -1) == LUA_TNUMBER ) { en.status = (int)lua_tointeger(Ls, -1); }    lua_pop(Ls, 1);
				samLuaGetFieldCI(Ls, entryIdx, "beatitude"); if ( lua_type(Ls, -1) == LUA_TNUMBER ) { en.beatitude = (int)lua_tointeger(Ls, -1); } lua_pop(Ls, 1);
				samLuaGetFieldCI(Ls, entryIdx, "identified"); en.identified = samBoolArg(Ls, lua_gettop(Ls), true); lua_pop(Ls, 1);
			}
			else
			{
				en.type = samResolveItemArg(Ls, entryIdx);
			}
			lua_pop(Ls, 1);
			if ( en.type < 0 )
			{
				SAM_ERROR("LUA", "sam_set_shop_stock: entry " + std::to_string((long long)i) + " is not an item this game has. Nothing was changed.");
				lua_pushboolean(Ls, 0); return 1;
			}
			entries.push_back(en);
		}
		lua_pushboolean(Ls, SAMLoot::setShopStock((std::uint32_t)uid, entries) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_set_shop_type(shopkeeper_uid, store_type) -> boolean. 0 arms, 1 hats, 2 jewelry, 3
	// books, 4 apothecary, 5 staves, 6 food, 7 hardware, 8 hunting, 9 general, 10 mysterious.
	// Only BEFORE the shopkeeper has stocked (the tick it was spawned); after that the type is
	// read, so use world.on_before_shop_stock, or sam_set_shop_stock for the stock itself.
	int lua_sam_set_shop_type(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_LUA_HAVE_BARONY
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int storeType = (int)luaL_checkinteger(Ls, 2);
		lua_pushboolean(Ls, SAMLoot::setShopType((std::uint32_t)uid, storeType) ? 1 : 0);
		return 1;
#else
		lua_pushboolean(Ls, 0); return 1;
#endif
	}

	int lua_panic(lua_State* Ls)
	{
		const char* msg = lua_tostring(Ls, -1);
		SAM_ERROR("LUA", std::string("PANIC (unprotected Lua error): ") + (msg ? msg : "?"));
		// Returning from a panic handler makes Lua abort(); in practice every
		// entry point here is protected by lua_pcall, so this should be dead code.
		return 0;
	}

	// ---- sandbox construction --------------------------------------------------

	void setupSandbox()
	{
		// Open ONLY pure-computation libraries. We never open io, os, package,
		// debug or coroutine.
		static const luaL_Reg safeLibs[] = {
			{ LUA_GNAME,       luaopen_base   },
			{ LUA_TABLIBNAME,  luaopen_table  },
			{ LUA_STRLIBNAME,  luaopen_string },
			{ LUA_MATHLIBNAME, luaopen_math   },
			{ LUA_UTF8LIBNAME, luaopen_utf8   },
			{ nullptr,         nullptr        }
		};
		for ( const luaL_Reg* lib = safeLibs; lib->func; ++lib )
		{
			luaL_requiref(L, lib->name, lib->func, 1);
			lua_pop(L, 1); // pop the module table requiref leaves on the stack
		}

		// Strip dangerous globals. os/io/package/require are already absent (we
		// never opened them); we nil them anyway so the contract is explicit and
		// robust against a future openlibs slip. dofile/loadfile/load/loadstring
		// come from the base lib and are the real ones that must go.
		static const char* stripped[] = {
			"dofile", "loadfile", "load", "loadstring",
			"os", "io", "require", "package",
			nullptr
		};
		for ( const char** name = stripped; *name; ++name )
		{
			lua_pushnil(L);
			lua_setglobal(L, *name);
		}

		// Install the sam.* host table.
		lua_newtable(L);
		lua_pushcfunction(L, lua_sam_log);
		lua_setfield(L, -2, "log");
		lua_setglobal(L, "sam");

		// Convenience: expose the same logger as a bare global sam_log(msg) so
		// scripts can call it without the table prefix.
		samLuaRegister(L, "sam_log", lua_sam_log);
#ifdef SAM_LUA_HAVE_BARONY
		SAMNet::setCallRunner('L', samLuaRunForwarded);   // calls the host carries to this machine
#endif

		// Persistent per-mod data (Part 3) — available in every build.
		samLuaRegister(L, "sam_save_data", lua_sam_save_data);
		samLuaRegister(L, "sam_load_data", lua_sam_load_data);
		samLuaRegister(L, "sam_delete_data", lua_sam_delete_data);

		// Timers (Part 4).
		samLuaRegister(L, "sam_set_timer", lua_sam_set_timer);
		samLuaRegister(L, "sam_set_repeating_timer", lua_sam_set_repeating_timer);
		samLuaRegister(L, "sam_cancel_timer", lua_sam_cancel_timer);

		// Custom hooks (Part 2).
		samLuaRegister(L, "sam_register_hook", lua_sam_register_hook);
		samLuaRegister(L, "sam_fire_hook", lua_sam_fire_hook);
		samLuaRegister(L, "sam_modify_damage", lua_sam_modify_damage);
		samLuaRegister(L, "sam_modify_monster_damage", lua_sam_modify_monster_damage);
		samLuaRegister(L, "sam_modify_value", lua_sam_modify_value);
		samLuaRegister(L, "sam_deal_damage", lua_sam_deal_damage);
		samLuaRegister(L, "sam_is_key_held", lua_sam_is_key_held);
		samLuaRegister(L, "sam_get_monster_stat", lua_sam_get_monster_stat);
		samLuaRegister(L, "sam_monster_path_to", lua_sam_monster_path_to);
		samLuaRegister(L, "sam_monster_face", lua_sam_monster_face);
		samLuaRegister(L, "sam_monster_attack", lua_sam_monster_attack);
		samLuaRegister(L, "sam_monster_charge", lua_sam_monster_charge);
		samLuaRegister(L, "sam_get_monster_type", lua_sam_get_monster_type);
		samLuaRegister(L, "sam_get_monster_name", lua_sam_get_monster_name);
		samLuaRegister(L, "sam_monster_has_effect", lua_sam_monster_has_effect);
		samLuaRegister(L, "sam_monster_has_trait", lua_sam_monster_has_trait);
		samLuaRegister(L, "sam_get_item_category", lua_sam_get_item_category);
		samLuaRegister(L, "sam_set_monster_stat", lua_sam_set_monster_stat);
		samLuaRegister(L, "sam_set_monster_name", lua_sam_set_monster_name);
		samLuaRegister(L, "sam_set_model", lua_sam_set_model);
		samLuaRegister(L, "sam_clear_model", lua_sam_clear_model);
		samLuaRegister(L, "sam_get_model", lua_sam_get_model);
		samLuaRegister(L, "sam_set_scale", lua_sam_set_scale);
		samLuaRegister(L, "sam_set_entity_size", lua_sam_set_entity_size);
		samLuaRegister(L, "sam_set_damage_immune", lua_sam_set_damage_immune);
		samLuaRegister(L, "sam_is_damage_immune", lua_sam_is_damage_immune);
		samLuaRegister(L, "sam_move_entity", lua_sam_move_entity);
		samLuaRegister(L, "sam_apply_force", lua_sam_apply_force);
		samLuaRegister(L, "sam_set_on_fire", lua_sam_set_on_fire);
		samLuaRegister(L, "sam_get_entity_flag", lua_sam_get_entity_flag);
		samLuaRegister(L, "sam_set_entity_flag", lua_sam_set_entity_flag);
		samLuaRegister(L, "sam_set_elevation", lua_sam_set_elevation);
		samLuaRegister(L, "sam_set_visible", lua_sam_set_visible);
		samLuaRegister(L, "sam_monster_equip", lua_sam_monster_equip);
		samLuaRegister(L, "sam_monster_unequip", lua_sam_monster_unequip);
		samLuaRegister(L, "sam_apply_monster_effect", lua_sam_apply_monster_effect);
		samLuaRegister(L, "sam_kill_monster", lua_sam_kill_monster);
		samLuaRegister(L, "sam_spawn_monsters", lua_sam_spawn_monsters);
		samLuaRegister(L, "sam_get_monster_target", lua_sam_get_monster_target);
		samLuaRegister(L, "sam_set_monster_target", lua_sam_set_monster_target);
		samLuaRegister(L, "sam_get_monster_data", lua_sam_get_monster_data);
		samLuaRegister(L, "sam_set_monster_data", lua_sam_set_monster_data);
		samLuaRegister(L, "sam_get_player_data", lua_sam_get_player_data);
		samLuaRegister(L, "sam_set_player_data", lua_sam_set_player_data);
		samLuaRegister(L, "sam_get_effect_duration", lua_sam_get_effect_duration);
		samLuaRegister(L, "sam_get_effect_strength", lua_sam_get_effect_strength);
		samLuaRegister(L, "sam_get_effects", lua_sam_get_effects);
		// v1.5.0 monster status-effect read/remove parity
		samLuaRegister(L, "sam_remove_monster_effect", lua_sam_remove_monster_effect);
		samLuaRegister(L, "sam_get_monster_effect_duration", lua_sam_get_monster_effect_duration);
		samLuaRegister(L, "sam_get_monster_effect_strength", lua_sam_get_monster_effect_strength);
		samLuaRegister(L, "sam_get_monster_effects", lua_sam_get_monster_effects);
		// v0.7.0 Feature 5: modify existing content (revert on unload)
		samLuaRegister(L, "sam_patch_class", lua_sam_patch_class);
		samLuaRegister(L, "sam_unpatch_class", lua_sam_unpatch_class);
		samLuaRegister(L, "sam_patch_item", lua_sam_patch_item);
		samLuaRegister(L, "sam_patch_monster", lua_sam_patch_monster);
		samLuaRegister(L, "sam_add_class_passive", lua_sam_add_class_passive);
		samLuaRegister(L, "sam_remove_class_passive", lua_sam_remove_class_passive);
		// Custom spells (Session 1: grant vanilla for real; custom recognized + deferred)
		samLuaRegister(L, "sam_grant_spell", lua_sam_grant_spell);
		samLuaRegister(L, "sam_cast_spell", lua_sam_cast_spell);
		// v1.5.0 spell freedom: aimed cast, monster cast, query + remove
		samLuaRegister(L, "sam_cast_spell_at", lua_sam_cast_spell_at);
		samLuaRegister(L, "sam_cast_spell_pos", lua_sam_cast_spell_pos);
		samLuaRegister(L, "sam_monster_cast_spell", lua_sam_monster_cast_spell);
		samLuaRegister(L, "sam_get_spells", lua_sam_get_spells);
		samLuaRegister(L, "sam_player_knows_spell", lua_sam_player_knows_spell);
		samLuaRegister(L, "sam_remove_spell", lua_sam_remove_spell);

#ifdef SAM_LUA_HAVE_BARONY
		// Host bindings that actually affect the game (engine build only).
		samLuaRegister(L, "sam_grant_item", lua_sam_grant_item);
		samLuaRegister(L, "sam_grant_gold", lua_sam_grant_gold);
		samLuaRegister(L, "sam_apply_effect", lua_sam_apply_effect);
		samLuaRegister(L, "sam_remove_effect", lua_sam_remove_effect);
		// v1.5.0 player effect control
		samLuaRegister(L, "sam_clear_effects", lua_sam_clear_effects);
		samLuaRegister(L, "sam_set_effect_duration", lua_sam_set_effect_duration);
		samLuaRegister(L, "sam_set_effect_strength", lua_sam_set_effect_strength);
		samLuaRegister(L, "sam_get_stat", lua_sam_get_stat);
		samLuaRegister(L, "sam_set_stat", lua_sam_set_stat);
		samLuaRegister(L, "sam_set_move_speed", lua_sam_set_move_speed);
		samLuaRegister(L, "sam_get_move_speed", lua_sam_get_move_speed);
		samLuaRegister(L, "sam_add_move_speed", lua_sam_add_move_speed);
		samLuaRegister(L, "sam_level_up", lua_sam_level_up);
		samLuaRegister(L, "sam_get_floor", lua_sam_get_floor);
		samLuaRegister(L, "sam_get_seed", lua_sam_get_seed);
		samLuaRegister(L, "sam_get_flag", lua_sam_get_flag);
		samLuaRegister(L, "sam_is_ghost", lua_sam_is_ghost);
		samLuaRegister(L, "sam_is_spirit_ghost", lua_sam_is_spirit_ghost);
		samLuaRegister(L, "sam_random", lua_sam_random);
		samLuaRegister(L, "sam_list_data_keys", lua_sam_list_data_keys);
		samLuaRegister(L, "sam_spawn_item", lua_sam_spawn_item);
		samLuaRegister(L, "sam_item_id", lua_sam_item_id);
		samLuaRegister(L, "sam_message", lua_sam_message);
		samLuaRegister(L, "sam_play_sound", lua_sam_play_sound);
		samLuaRegister(L, "sam_get_nearby_entities", lua_sam_get_nearby_entities);

		// Expanded player queries (Part 5).
		samLuaRegister(L, "sam_get_equipped_item", lua_sam_get_equipped_item);
		samLuaRegister(L, "sam_get_equipped_item_id", lua_sam_get_equipped_item_id);
		samLuaRegister(L, "sam_is_defending", lua_sam_is_defending);
		samLuaRegister(L, "sam_is_action_held", lua_sam_is_action_held);
		samLuaRegister(L, "sam_get_action_binding", lua_sam_get_action_binding);
		// P_SETTINGS: a mod's own rows in the Bindings page and the General tab (sam_settings.cpp).
		samLuaRegister(L, "sam_register_action", lua_sam_register_action);
		samLuaRegister(L, "sam_register_setting", lua_sam_register_setting);
		samLuaRegister(L, "sam_get_setting", lua_sam_get_setting);
		samLuaRegister(L, "sam_set_setting", lua_sam_set_setting);
		samLuaRegister(L, "sam_list_settings", lua_sam_list_settings);
		samLuaRegister(L, "sam_get_inventory_count", lua_sam_get_inventory_count);
		samLuaRegister(L, "sam_has_effect", lua_sam_has_effect);
		samLuaRegister(L, "sam_get_class", lua_sam_get_class);
		samLuaRegister(L, "sam_get_race", lua_sam_get_race);
		samLuaRegister(L, "sam_get_kills", lua_sam_get_kills);
		samLuaRegister(L, "sam_test_done", lua_sam_test_done);
		samLuaRegister(L, "sam_is_host", lua_sam_is_host);
		samLuaRegister(L, "sam_play_sound_at", lua_sam_play_sound_at);
		samLuaRegister(L, "sam_play_music", lua_sam_play_music);
		samLuaRegister(L, "sam_stop_music", lua_sam_stop_music);
		samLuaRegister(L, "sam_get_music", lua_sam_get_music);
		samLuaRegister(L, "sam_list_music", lua_sam_list_music);
		samLuaRegister(L, "sam_list_sounds", lua_sam_list_sounds);
		samLuaRegister(L, "sam_stop_sound", lua_sam_stop_sound);
		samLuaRegister(L, "sam_play_sound_entity", lua_sam_play_sound_entity);
		samLuaRegister(L, "sam_spawn_particle", lua_sam_spawn_particle);
		samLuaRegister(L, "sam_damage_number", lua_sam_damage_number);
		samLuaRegister(L, "sam_get_effective_stat", lua_sam_get_effective_stat);
		samLuaRegister(L, "sam_get_ac", lua_sam_get_ac);
		samLuaRegister(L, "sam_get_skill", lua_sam_get_skill);
		samLuaRegister(L, "sam_is_enemy", lua_sam_is_enemy);
		samLuaRegister(L, "sam_is_friend", lua_sam_is_friend);
		samLuaRegister(L, "sam_get_mods", lua_sam_get_mods);
		samLuaRegister(L, "sam_is_mod_loaded", lua_sam_is_mod_loaded);
		samLuaRegister(L, "sam_get_tile", lua_sam_get_tile);
		samLuaRegister(L, "sam_set_tile", lua_sam_set_tile);
		samLuaRegister(L, "sam_is_spawnable", lua_sam_is_spawnable);
		samLuaRegister(L, "sam_line_of_sight", lua_sam_line_of_sight);
		samLuaRegister(L, "sam_tiles_connected", lua_sam_tiles_connected);
		samLuaRegister(L, "sam_get_light_at", lua_sam_get_light_at);
		samLuaRegister(L, "sam_find_entities", lua_sam_find_entities);
		samLuaRegister(L, "sam_get_container_items", lua_sam_get_container_items);
		samLuaRegister(L, "sam_set_door", lua_sam_set_door);
		samLuaRegister(L, "sam_set_door_locked", lua_sam_set_door_locked);
		samLuaRegister(L, "sam_power_entity", lua_sam_power_entity);
		samLuaRegister(L, "sam_toggle_switch", lua_sam_toggle_switch);
		samLuaRegister(L, "sam_get_level_info", lua_sam_get_level_info);
		samLuaRegister(L, "sam_hud_text", lua_sam_hud_text);
		samLuaRegister(L, "sam_hud_bar", lua_sam_hud_bar);
		samLuaRegister(L, "sam_hud_clear", lua_sam_hud_clear);
		// v1.10.3 -- the mod's own pictures (overlay + HUD art).
		samLuaRegister(L, "sam_show_image", lua_sam_show_image);
		samLuaRegister(L, "sam_show_image_at", lua_sam_show_image_at);
		samLuaRegister(L, "sam_hide_image", lua_sam_hide_image);
		samLuaRegister(L, "sam_hud_image", lua_sam_hud_image);
		samLuaRegister(L, "sam_get_image_size", lua_sam_get_image_size);
		// v1.11.0 -- interactive panels.
		samLuaRegister(L, "sam_ui_open", lua_sam_ui_open);
		samLuaRegister(L, "sam_ui_close", lua_sam_ui_close);
		samLuaRegister(L, "sam_ui_is_open", lua_sam_ui_is_open);
		samLuaRegister(L, "sam_ui_clear", lua_sam_ui_clear);
		samLuaRegister(L, "sam_ui_label", lua_sam_ui_label);
		samLuaRegister(L, "sam_ui_button", lua_sam_ui_button);
		samLuaRegister(L, "sam_ui_image", lua_sam_ui_image);
		samLuaRegister(L, "sam_ui_list", lua_sam_ui_list);
		samLuaRegister(L, "sam_ui_list_add", lua_sam_ui_list_add);
		samLuaRegister(L, "sam_ui_list_clear", lua_sam_ui_list_clear);
		samLuaRegister(L, "sam_ui_input", lua_sam_ui_input);
		samLuaRegister(L, "sam_ui_input_text", lua_sam_ui_input_text);
		samLuaRegister(L, "sam_ui_panel_style", lua_sam_ui_panel_style);
		samLuaRegister(L, "sam_ui_font", lua_sam_ui_font);
		samLuaRegister(L, "sam_ui_list_row_height", lua_sam_ui_list_row_height);
		samLuaRegister(L, "sam_ui_text_size", lua_sam_ui_text_size);
		// v1.11.0 -- reading the game's own content.
		samLuaRegister(L, "sam_list_items", lua_sam_list_items);
		samLuaRegister(L, "sam_get_item_info", lua_sam_get_item_info);
		samLuaRegister(L, "sam_list_monsters", lua_sam_list_monsters);
		samLuaRegister(L, "sam_list_spells", lua_sam_list_spells);
		samLuaRegister(L, "sam_spawn_projectile", lua_sam_spawn_projectile);
		samLuaRegister(L, "sam_send_packet", lua_sam_send_packet);
		samLuaRegister(L, "sam_player_count", lua_sam_player_count);
		samLuaRegister(L, "sam_local_player", lua_sam_local_player);
		samLuaRegister(L, "sam_get_time_played", lua_sam_get_time_played);

		// v2 world-ops: position / teleport / spawn / inventory.
		samLuaRegister(L, "sam_get_player_uid", lua_sam_get_player_uid);
		samLuaRegister(L, "sam_get_position", lua_sam_get_position);
		samLuaRegister(L, "sam_can_stand", lua_sam_can_stand);
		samLuaRegister(L, "sam_set_position", lua_sam_set_position);
		samLuaRegister(L, "sam_spawn_monster", lua_sam_spawn_monster);
		samLuaRegister(L, "sam_spawn_portal", lua_sam_spawn_portal);
		samLuaRegister(L, "sam_remove_entity", lua_sam_remove_entity);
		samLuaRegister(L, "sam_set_entity_facing", lua_sam_set_entity_facing);
		samLuaRegister(L, "sam_look_at", lua_sam_look_at);
		samLuaRegister(L, "sam_get_entity_facing", lua_sam_get_entity_facing);
		samLuaRegister(L, "sam_register_behavior", lua_sam_register_behavior);
		samLuaRegister(L, "sam_attach_behavior", lua_sam_attach_behavior);
		samLuaRegister(L, "sam_detach_behavior", lua_sam_detach_behavior);
		samLuaRegister(L, "sam_spawn_entity", lua_sam_spawn_entity);
		samLuaRegister(L, "sam_set_chest_stash", lua_sam_set_chest_stash);
		samLuaRegister(L, "sam_travel_to_level", lua_sam_travel_to_level);
		samLuaRegister(L, "sam_world_save", lua_sam_world_save);
		samLuaRegister(L, "sam_world_load", lua_sam_world_load);
		samLuaRegister(L, "sam_world_clear", lua_sam_world_clear);
		samLuaRegister(L, "sam_world_keys", lua_sam_world_keys);
		samLuaRegister(L, "sam_get_inventory", lua_sam_get_inventory);
		samLuaRegister(L, "sam_remove_item", lua_sam_remove_item);
		// v1.4.0 — floating companion ("Stand") + facing reader.
		samLuaRegister(L, "sam_spawn_companion", lua_sam_spawn_companion);
		samLuaRegister(L, "sam_companion_punch", lua_sam_companion_punch);
		samLuaRegister(L, "sam_get_facing", lua_sam_get_facing);
		// v1.6.0 — impact frame: screen flash / manga burst / camera shake / hitstop.
		samLuaRegister(L, "sam_screen_flash", lua_sam_screen_flash);
		samLuaRegister(L, "sam_impact_frame", lua_sam_impact_frame);
		samLuaRegister(L, "sam_camera_shake", lua_sam_camera_shake);
		samLuaRegister(L, "sam_hitstop", lua_sam_hitstop);
		// The simulation speed (sam_speed.cpp).
		samLuaRegister(L, "sam_set_game_speed", lua_sam_set_game_speed);
		samLuaRegister(L, "sam_get_game_speed", lua_sam_get_game_speed);
		// P_ITEMS: the loot pool, its tables, the containers and the shops (sam_loot.cpp).
		samLuaRegister(L, "sam_get_loot_pool", lua_sam_get_loot_pool);
		samLuaRegister(L, "sam_set_loot_weight", lua_sam_set_loot_weight);
		samLuaRegister(L, "sam_set_loot_category_weight", lua_sam_set_loot_category_weight);
		samLuaRegister(L, "sam_set_loot_floor_range", lua_sam_set_loot_floor_range);
		samLuaRegister(L, "sam_set_loot_context", lua_sam_set_loot_context);
		samLuaRegister(L, "sam_set_spell_droppable", lua_sam_set_spell_droppable);
		samLuaRegister(L, "sam_set_loot_fallback", lua_sam_set_loot_fallback);
		samLuaRegister(L, "sam_roll_loot", lua_sam_roll_loot);
		samLuaRegister(L, "sam_add_item_to_container", lua_sam_add_item_to_container);
		samLuaRegister(L, "sam_remove_item_from_container", lua_sam_remove_item_from_container);
		samLuaRegister(L, "sam_set_shop_stock", lua_sam_set_shop_stock);
		samLuaRegister(L, "sam_set_shop_type", lua_sam_set_shop_type);
		// ---- the rules (v3.0.0) ----------------------------------------------
		samLuaRegister(L, "sam_add_stat_modifier", lua_sam_add_stat_modifier);
		samLuaRegister(L, "sam_add_monster_stat_modifier", lua_sam_add_monster_stat_modifier);
		samLuaRegister(L, "sam_remove_stat_modifier", lua_sam_remove_stat_modifier);
		samLuaRegister(L, "sam_remove_monster_stat_modifier", lua_sam_remove_monster_stat_modifier);
		samLuaRegister(L, "sam_clear_stat_modifiers", lua_sam_clear_stat_modifiers);
		samLuaRegister(L, "sam_get_stat_modifier", lua_sam_get_stat_modifier);
		samLuaRegister(L, "sam_set_immunity", lua_sam_set_immunity);
		samLuaRegister(L, "sam_set_monster_immunity", lua_sam_set_monster_immunity);
		samLuaRegister(L, "sam_set_species_immunity", lua_sam_set_species_immunity);
		samLuaRegister(L, "sam_is_immune", lua_sam_is_immune);
		samLuaRegister(L, "sam_clear_immunities", lua_sam_clear_immunities);
		samLuaRegister(L, "sam_set_xp_curve", lua_sam_set_xp_curve);
		samLuaRegister(L, "sam_clear_xp_curve", lua_sam_clear_xp_curve);
		samLuaRegister(L, "sam_get_xp_threshold", lua_sam_get_xp_threshold);
		samLuaRegister(L, "sam_grant_xp", lua_sam_grant_xp);
		// ---- the camera (v3.0.0) ---------------------------------------------
		samLuaRegister(L, "sam_set_camera_offset", lua_sam_set_camera_offset);
		samLuaRegister(L, "sam_set_camera_position", lua_sam_set_camera_position);
		samLuaRegister(L, "sam_set_camera_angle", lua_sam_set_camera_angle);
		samLuaRegister(L, "sam_set_camera_target", lua_sam_set_camera_target);
		samLuaRegister(L, "sam_set_camera_collision", lua_sam_set_camera_collision);
		samLuaRegister(L, "sam_get_camera", lua_sam_get_camera);
		samLuaRegister(L, "sam_reset_camera", lua_sam_reset_camera);
		samLuaRegister(L, "sam_show_own_body", lua_sam_show_own_body);
		// ---- v2.8 batch 4: combat ---------------------------------------------
		samLuaRegister(L, "sam_heal", lua_sam_heal);
		samLuaRegister(L, "sam_deal_damage_typed", lua_sam_deal_damage_typed);
		samLuaRegister(L, "sam_get_hp", lua_sam_get_hp);
		samLuaRegister(L, "sam_get_max_hp", lua_sam_get_max_hp);
		samLuaRegister(L, "sam_get_mp", lua_sam_get_mp);
		samLuaRegister(L, "sam_get_max_mp", lua_sam_get_max_mp);
		samLuaRegister(L, "sam_get_attack", lua_sam_get_attack);
		samLuaRegister(L, "sam_get_ranged_attack", lua_sam_get_ranged_attack);
		samLuaRegister(L, "sam_get_thrown_attack", lua_sam_get_thrown_attack);
		samLuaRegister(L, "sam_get_bonus_attack_vs", lua_sam_get_bonus_attack_vs);
		samLuaRegister(L, "sam_get_damage_resist", lua_sam_get_damage_resist);
		samLuaRegister(L, "sam_get_magic_resist", lua_sam_get_magic_resist);
		samLuaRegister(L, "sam_preview_damage", lua_sam_preview_damage);
		samLuaRegister(L, "sam_get_regen_interval", lua_sam_get_regen_interval);
		samLuaRegister(L, "sam_get_healring", lua_sam_get_healring);
		samLuaRegister(L, "sam_mod_mp", lua_sam_mod_mp);
		samLuaRegister(L, "sam_drain_mp", lua_sam_drain_mp);
		samLuaRegister(L, "sam_consume_mp", lua_sam_consume_mp);
		samLuaRegister(L, "sam_set_defending", lua_sam_set_defending);
		samLuaRegister(L, "sam_is_parrying", lua_sam_is_parrying);
		samLuaRegister(L, "sam_set_parry", lua_sam_set_parry);
		samLuaRegister(L, "sam_get_monster_target_uid", lua_sam_get_monster_target_uid);
		samLuaRegister(L, "sam_set_monster_target_uid", lua_sam_set_monster_target_uid);
		samLuaRegister(L, "sam_clear_monster_target", lua_sam_clear_monster_target);
		samLuaRegister(L, "sam_alert_allies", lua_sam_alert_allies);
		samLuaRegister(L, "sam_break_armor", lua_sam_break_armor);
		samLuaRegister(L, "sam_gib", lua_sam_gib);
		samLuaRegister(L, "sam_obituary", lua_sam_obituary);
		samLuaRegister(L, "sam_revive_player", lua_sam_revive_player);
		samLuaRegister(L, "sam_set_species_damage_resist", lua_sam_set_species_damage_resist);
		samLuaRegister(L, "sam_clear_species_damage_resist", lua_sam_clear_species_damage_resist);
		samLuaRegister(L, "sam_add_damage_multiplier", lua_sam_add_damage_multiplier);
		// ---- v2.6 batch 1: reads and dice -------------------------------------
		samLuaRegister(L, "sam_get_position_precise", lua_sam_get_position_precise);
		samLuaRegister(L, "sam_get_distance", lua_sam_get_distance);
		samLuaRegister(L, "sam_get_distance_to", lua_sam_get_distance_to);
		samLuaRegister(L, "sam_get_entity_type", lua_sam_get_entity_type);
		samLuaRegister(L, "sam_get_scale", lua_sam_get_scale);
		samLuaRegister(L, "sam_is_visible", lua_sam_is_visible);
		samLuaRegister(L, "sam_get_velocity", lua_sam_get_velocity);
		samLuaRegister(L, "sam_get_entity_size", lua_sam_get_entity_size);
		samLuaRegister(L, "sam_get_entity_sprite", lua_sam_get_entity_sprite);
		samLuaRegister(L, "sam_get_entity_ticks", lua_sam_get_entity_ticks);
		samLuaRegister(L, "sam_get_map_seed", lua_sam_get_map_seed);
		samLuaRegister(L, "sam_is_dark_level", lua_sam_is_dark_level);
		samLuaRegister(L, "sam_get_playable_bounds", lua_sam_get_playable_bounds);
		samLuaRegister(L, "sam_is_tile_diggable", lua_sam_is_tile_diggable);
		samLuaRegister(L, "sam_get_map_flags", lua_sam_get_map_flags);
		samLuaRegister(L, "sam_get_exit_position", lua_sam_get_exit_position);
		samLuaRegister(L, "sam_get_run_time", lua_sam_get_run_time);
		samLuaRegister(L, "sam_get_tick_rate", lua_sam_get_tick_rate);
		samLuaRegister(L, "sam_get_fps", lua_sam_get_fps);
		samLuaRegister(L, "sam_get_real_time", lua_sam_get_real_time);
		samLuaRegister(L, "sam_get_date", lua_sam_get_date);
		samLuaRegister(L, "sam_is_paused", lua_sam_is_paused);
		samLuaRegister(L, "sam_is_in_game", lua_sam_is_in_game);
		samLuaRegister(L, "sam_is_loading", lua_sam_is_loading);
		samLuaRegister(L, "sam_random_float", lua_sam_random_float);
		samLuaRegister(L, "sam_random_chance", lua_sam_random_chance);
		samLuaRegister(L, "sam_random_from_list", lua_sam_random_from_list);
		samLuaRegister(L, "sam_random_weighted", lua_sam_random_weighted);
		samLuaRegister(L, "sam_has_data", lua_sam_has_data);
		samLuaRegister(L, "sam_world_bytes", lua_sam_world_bytes);
		samLuaRegister(L, "sam_world_bytes_free", lua_sam_world_bytes_free);
		// ---- v2.6 batch 2: inventory and items --------------------------------
		samLuaRegister(L, "sam_get_item", lua_sam_get_item);
		samLuaRegister(L, "sam_get_item_name", lua_sam_get_item_name);
		samLuaRegister(L, "sam_get_item_value", lua_sam_get_item_value);
		samLuaRegister(L, "sam_get_item_weight", lua_sam_get_item_weight);
		samLuaRegister(L, "sam_get_item_attack", lua_sam_get_item_attack);
		samLuaRegister(L, "sam_get_item_ac", lua_sam_get_item_ac);
		samLuaRegister(L, "sam_get_tome_spell", lua_sam_get_tome_spell);
		samLuaRegister(L, "sam_get_food_satiation", lua_sam_get_food_satiation);
		samLuaRegister(L, "sam_set_item_beatitude", lua_sam_set_item_beatitude);
		samLuaRegister(L, "sam_set_item_status", lua_sam_set_item_status);
		samLuaRegister(L, "sam_set_item_count", lua_sam_set_item_count);
		samLuaRegister(L, "sam_identify_item", lua_sam_identify_item);
		samLuaRegister(L, "sam_set_item_appearance", lua_sam_set_item_appearance);
		samLuaRegister(L, "sam_set_item_droppable", lua_sam_set_item_droppable);
		samLuaRegister(L, "sam_get_item_owner", lua_sam_get_item_owner);
		samLuaRegister(L, "sam_set_item_owner", lua_sam_set_item_owner);
		samLuaRegister(L, "sam_is_ranged_weapon", lua_sam_is_ranged_weapon);
		samLuaRegister(L, "sam_is_melee_weapon", lua_sam_is_melee_weapon);
		samLuaRegister(L, "sam_is_shield", lua_sam_is_shield);
		samLuaRegister(L, "sam_is_potion_bad", lua_sam_is_potion_bad);
		samLuaRegister(L, "sam_item_has_trait", lua_sam_item_has_trait);
		samLuaRegister(L, "sam_get_item_slot", lua_sam_get_item_slot);
		samLuaRegister(L, "sam_is_better_weapon", lua_sam_is_better_weapon);
		samLuaRegister(L, "sam_is_better_armor", lua_sam_is_better_armor);
		samLuaRegister(L, "sam_is_item_equipped", lua_sam_is_item_equipped);
		samLuaRegister(L, "sam_can_unequip", lua_sam_can_unequip);
		samLuaRegister(L, "sam_inventory_has_space", lua_sam_inventory_has_space);
		samLuaRegister(L, "sam_get_max_stack", lua_sam_get_max_stack);
		samLuaRegister(L, "sam_can_items_stack", lua_sam_can_items_stack);
		samLuaRegister(L, "sam_monster_can_wield", lua_sam_monster_can_wield);
#endif
	}

	// Build a Lua table { name = ..., <k>=<v>, ... } from an Event and leave it
	// on top of the stack. Copies primitives only — no pointers cross over.
	void pushEventTable(const SAMLua::Event& ev)
	{
		lua_createtable(L, 0, (int)(ev.ints.size() + ev.strings.size() + 1));

		lua_pushstring(L, ev.name.c_str());
		lua_setfield(L, -2, "name");

		// Values come from the live write-back store, not from `ev` -- so a change made by an
		// earlier script is what the next script SEES. Two mods that each halve incoming
		// damage therefore both apply, instead of the second one silently overwriting the
		// first from the original number. The store is seeded from `ev` at the top of the
		// dispatch, so the first script still sees exactly what the engine sent.
		for ( const auto& kv : ev.ints )
		{
			auto it = g_lastEventNumbers.find(kv.first);
			const double v = ( it == g_lastEventNumbers.end() ) ? (double)kv.second : it->second;
			lua_pushinteger(L, (lua_Integer)v);
			lua_setfield(L, -2, kv.first.c_str());
		}
		for ( const auto& kv : ev.strings )
		{
			auto it = g_lastEventStrings.find(kv.first);
			// With its length: an on_packet payload may hold a zero byte, where lua_pushstring stopped.
			const std::string& v = ( it == g_lastEventStrings.end() ) ? kv.second : it->second;
			lua_pushlstring(L, v.data(), v.size());
			lua_setfield(L, -2, kv.first.c_str());
		}
	}

} // anonymous namespace

// ---------------------------------------------------------------------------
namespace SAMLua
{
	bool init(const SandboxConfig& cfg)
	{
		if ( L )
		{
			SAM_WARN("LUA", "init() called twice — ignoring the second call.");
			return true;
		}

		g_cfg = cfg;

		g_alloc = AllocState{};
		g_alloc.limit = cfg.memoryCapBytes;

		g_hook = HookState{};
		g_hook.budget   = cfg.instructionBudget;
		g_hook.interval = cfg.watchdogInterval;

		L = lua_newstate(luaAlloc, &g_alloc);
		if ( !L )
		{
			SAM_ERROR("LUA", "lua_newstate failed (allocator refused the initial state).");
			return false;
		}

		lua_atpanic(L, lua_panic);
		setupSandbox();

		SAM_INFO("LUA", "Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR " runtime initialized "
			"(mem cap " + std::to_string(cfg.memoryCapBytes / (1024u * 1024u)) + "MB, "
			"instr budget " + std::to_string(cfg.instructionBudget) + ", "
			"watchdog every " + std::to_string(cfg.watchdogInterval) + " instr).");
		return true;
	}

	bool loadScript(const std::string& path, const std::string& modNamespace)
	{
		if ( !L )
		{
			SAM_ERROR("LUA", "loadScript('" + path + "') called before init().");
			return false;
		}

		// Parse only (does not execute yet).
		if ( luaL_loadfile(L, path.c_str()) != LUA_OK )
		{
			const char* e = lua_tostring(L, -1);
			const std::string err = e ? e : "(no error message)";
			lua_pop(L, 1);
			SAM_ERROR("LUA", "Failed to parse '" + path + "': " + err);
			return false;
		}

		// Run the chunk under the sandbox. This is where a top-level infinite
		// loop or error would occur — the watchdog / pcall contain it. The namespace
		// is live during load so a script may sam_load_data() at startup.
		g_currentNs = modNamespace;
		const bool ranOk = protectedCall(0, 0, "load " + path);
		g_currentNs.clear();
		if ( !ranOk )
		{
			SAM_WARN("LUA", "Script '" + path + "' disabled (it failed while running).");
			return false;
		}

		// Capture its on_event and/or on_tick handlers (a script may define either
		// or both). on_tick (v0.7.0) fires every game tick.
		//
		// RAW access to _G -- lua_rawget/lua_rawset on the globals table, never
		// lua_getglobal/lua_setglobal. Those honour __index/__newindex, and this runs
		// OUTSIDE any protected call: the chunk that just executed is free to
		// setmetatable(_G, ...) (setmetatable ships in the base lib the sandbox opens), and
		// a strict-globals guard whose __index raises would longjmp into lua_panic -- which
		// abort()s the process -- the instant we asked for a handler the script did not
		// define. Same hazard, same fix, as collectEventWriteBacks below; it had been applied
		// to the event table and not to _G. lua_panic's own comment ("every entry point here
		// is protected by lua_pcall") was wrong here.
		if ( !lua_checkstack(L, 4) )
		{
			SAM_ERROR("LUA", "Script '" + path + "' disabled (no Lua stack headroom after load).");
			return false;
		}
		lua_pushglobaltable(L);                                   // _G
		lua_pushstring(L, "on_event"); lua_rawget(L, -2);
		int eventRef = LUA_NOREF;
		if ( lua_isfunction(L, -1) ) { eventRef = luaL_ref(L, LUA_REGISTRYINDEX); } // pops it
		else { lua_pop(L, 1); }

		lua_pushstring(L, "on_tick"); lua_rawget(L, -2);
		int tickRef = LUA_NOREF;
		if ( lua_isfunction(L, -1) ) { tickRef = luaL_ref(L, LUA_REGISTRYINDEX); }
		else { lua_pop(L, 1); }

		// Clear both so the next script can't inherit this one's handlers -- raw, for the
		// same reason (__newindex fires on an absent key, and the key IS absent whenever the
		// script defined only one of the two).
		lua_pushstring(L, "on_event"); lua_pushnil(L); lua_rawset(L, -3);
		lua_pushstring(L, "on_tick");  lua_pushnil(L); lua_rawset(L, -3);
		lua_pop(L, 1);                                            // _G

		Script s; s.path = path; s.ns = modNamespace;
		s.callbackRef = eventRef; s.tickRef = tickRef;
		s.enabled = ( eventRef != LUA_NOREF || tickRef != LUA_NOREF );
		g_scripts.push_back(s);

		if ( !s.enabled )
		{
			// Name the actual mistake instead of just saying something's missing. By far the
			// most common cause is defining a function named after an EVENT —
			// `function on_action_pressed(event)` — because on_tick IS its own function while
			// every other hook arrives as event.name through on_event. That asymmetry catches
			// people constantly, and the old warning ("defines neither...") didn't hint at it.
			// Any on_* global that isn't a handler is almost certainly this error.
			std::string strays;
			// Read the globals table from the registry, not via the name "_G" — a script
			// is free to shadow or nil that name, and this diagnostic must still work.
			lua_pushglobaltable(L);
			const int gt = lua_gettop(L); // absolute index; lua_next shifts the stack
			lua_pushnil(L);
			while ( lua_next(L, gt) != 0 )
			{
				// key at -2, value at -1. Only read STRING keys: lua_tostring on a number
				// key would coerce it in place and corrupt lua_next's iteration state.
				if ( lua_isfunction(L, -1) && lua_type(L, -2) == LUA_TSTRING )
				{
					const char* n = lua_tostring(L, -2);
					if ( n && strncmp(n, "on_", 3) == 0
						&& strcmp(n, "on_event") != 0 && strcmp(n, "on_tick") != 0 )
					{
						if ( !strays.empty() ) { strays += ", "; }
						strays += std::string(n) + "()";
					}
				}
				lua_pop(L, 1); // pop value, keep key for the next lua_next
			}
			lua_pop(L, 1); // pop the globals table

			if ( !strays.empty() )
			{
				SAM_WARN("LUA", "Script '" + path + "' defines " + strays + " — that is an EVENT NAME, not a handler, "
					"so S.A.M never calls it and this script does nothing. S.A.M only calls on_event(event) and "
					"on_tick(event). Write it as: function on_event(event) if event.name == \"<the event>\" then ... end end");
			}
			else
			{
				SAM_WARN("LUA", "Script '" + path + "' loaded but defines neither on_event(event) nor on_tick(event).");
			}
		}
		else
		{
			std::string handlers = (eventRef != LUA_NOREF) ? "on_event" : "";
			if ( tickRef != LUA_NOREF ) { handlers += (handlers.empty() ? "" : " + ") + std::string("on_tick"); }
			SAM_INFO("LUA", "Loaded script '" + path + "' (" + handlers + " registered).");
		}
		return true;
	}

	// Read a handler's changes off the event table and drop our reference to it.
	//
	// Only keys the engine supplied are considered. That is deliberate: it keeps a script
	// from inventing a field name that happens to match something a future site reads, and
	// it means the cost is bounded by the size of the event rather than by whatever the
	// handler decided to attach to it.
	void collectEventWriteBacks(const SAMLua::Event& ev, int evRef)
	{
		if ( evRef == LUA_NOREF ) { return; }
		lua_rawgeti(L, LUA_REGISTRYINDEX, evRef);
		if ( lua_istable(L, -1) )
		{
			// lua_rawget, NOT lua_getfield.
			//
			// getfield honours __index, and this runs OUTSIDE any protected call -- so a
			// script that put a metatable on the event table could raise a Lua error here
			// with no pcall on the C stack, which means lua_panic and abort(): the game dies
			// mid-frame with no save. A strict-mode debug idiom is enough to trigger it, so
			// this needs no malice. Raw access is also what we actually mean: we want what
			// the handler ASSIGNED, not what a metatable synthesises.
			for ( const auto& kv : ev.ints )
			{
				lua_pushstring(L, kv.first.c_str());
				lua_rawget(L, -2);
				// Strict type test, matching the JS side: lua_isnumber accepts a numeric
				// STRING, so the same assignment would be honoured in Lua and dropped in JS.
				if ( lua_type(L, -1) == LUA_TNUMBER )
				{
					const double d = (double)lua_tonumber(L, -1);
					// Only a real change is recorded, so a script that merely reads the event
					// cannot overwrite an earlier script's edit with the value it was handed.
					auto it = g_lastEventNumbers.find(kv.first);
					const double seen = ( it == g_lastEventNumbers.end() ) ? (double)kv.second : it->second;
					if ( d != seen ) { g_lastEventNumbers[kv.first] = d; }
				}
				lua_pop(L, 1);
			}
			for ( const auto& kv : ev.strings )
			{
				lua_pushstring(L, kv.first.c_str());
				lua_rawget(L, -2);
				if ( lua_type(L, -1) == LUA_TSTRING )
				{
					size_t cLen = 0;
					const char* c = lua_tolstring(L, -1, &cLen);
					auto it = g_lastEventStrings.find(kv.first);
					const std::string seen = ( it == g_lastEventStrings.end() ) ? kv.second : it->second;
					// Compared and stored WITH the length (see pushEventTable).
					if ( c ) { const std::string cs(c, cLen); if ( seen != cs ) { g_lastEventStrings[kv.first] = cs; } }
				}
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
		luaL_unref(L, LUA_REGISTRYINDEX, evRef);
	}

	// Free whichever handle a row currently holds. One place, so neither register path nor
	// the teardown can forget a language.
	void releaseBehaviorRow(ScriptedBehavior& b)
	{
		if ( b.luaRef >= 0 && L ) { luaL_unref(L, LUA_REGISTRYINDEX, b.luaRef); }
		b.luaRef = -2;
#ifndef SAM_LUA_NO_JS
		if ( b.jsFn ) { SAMJs::releaseBehaviorFn(b.jsFn); }
#endif
		b.jsFn = nullptr;
	}

	bool clearBehaviorFnIf(const std::string& fullName, void* jsFn)
	{
		// Only clear the row if it still holds THIS handle. The JS callback that errored may
		// have re-registered its own name (in either language) before failing; then the
		// row belongs to the replacement, registerBehavior already released the old value,
		// and wiping the row here would strand the new function -- a Lua ref pinned
		// forever, or a JS function the caller then frees a second time. The old version
		// also zeroed luaRef unconditionally, which is how a re-registered Lua behaviour
		// got silently disabled by an unrelated JS error.
		const int i = behaviorIndexFor(fullName);
		if ( i < 0 || g_behaviors[i].jsFn != jsFn ) { return false; }
		g_behaviors[i].jsFn = nullptr;
		return true;
	}

	int registerBehavior(const std::string& fullName, const std::string& ns, int luaFnRef)
	{
		const int existing = behaviorIndexFor(fullName);
		if ( existing >= 0 )
		{
			// Re-registering replaces the function but KEEPS the index, so entities already
			// alive in the world follow the new code instead of pointing at a dead row.
			// Release whatever the row held first -- including a value owned by the OTHER
			// language, which the first version dropped on the floor.
			releaseBehaviorRow(g_behaviors[existing]);
			g_behaviors[existing].luaRef = luaFnRef;
			g_behaviors[existing].jsFn = nullptr;
			g_behaviors[existing].ns = ns;
			return existing;
		}
		ScriptedBehavior b; b.name = fullName; b.ns = ns; b.luaRef = luaFnRef;
		g_behaviors.push_back(b);
		SAM_INFO("LUA", "Registered behaviour '" + fullName + "'.");
		return (int)g_behaviors.size() - 1;
	}

	int behaviorIndexFor(const std::string& fullName)
	{
		for ( size_t i = 0; i < g_behaviors.size(); ++i )
		{
			if ( g_behaviors[i].name == fullName ) { return (int)i; }
		}
		return -1;
	}

	bool g_anyMonsterBehaviors = false;
	std::map<unsigned long long, int> g_monsterBehaviors;

	int monsterBehaviorIndexFor(unsigned long long uid)
	{
		if ( g_monsterBehaviors.empty() ) { return -1; }
		auto it = g_monsterBehaviors.find(uid);
		return ( it != g_monsterBehaviors.end() ) ? it->second : -1;
	}
	void attachMonsterBehavior(unsigned long long uid, int index)
	{
		g_monsterBehaviors[uid] = index;
		g_anyMonsterBehaviors = true;
	}
	void detachMonsterBehavior(unsigned long long uid)
	{
		g_monsterBehaviors.erase(uid);
		g_anyMonsterBehaviors = !g_monsterBehaviors.empty();
	}
	void clearMonsterBehaviors()
	{
		g_monsterBehaviors.clear();
		g_anyMonsterBehaviors = false;
	}

	void runBehavior(int index, unsigned long long uid)
	{
		if ( index < 0 || index >= (int)g_behaviors.size() ) { return; }

		// COPY everything needed before calling the script. The callback can register a new
		// behaviour, which push_backs into g_behaviors and reallocates it -- a reference held
		// across the call would dangle, and the error path below would then read and write
		// freed memory. Re-look-up by index afterwards instead.
		const int         luaRef = g_behaviors[index].luaRef;
		const std::string name   = g_behaviors[index].name;
		const std::string ns     = g_behaviors[index].ns;
		void* const       jsFn   = g_behaviors[index].jsFn;

		if ( luaRef >= 0 && L )
		{
			if ( !luaHasHeadroom("a behaviour") ) { return; }
			lua_rawgeti(L, LUA_REGISTRYINDEX, luaRef);
			if ( !lua_isfunction(L, -1) ) { lua_pop(L, 1); return; }
			lua_pushinteger(L, (lua_Integer)uid);
			const std::string savedNs = g_currentNs;
			g_currentNs = ns;
			const bool ok = protectedCall(1, 0, "behaviour '" + name + "'");
			g_currentNs = savedNs;
			if ( !ok )
			{
				// One bad frame must not spin forever. Drop the function; the entity keeps
				// existing but stops thinking, which is visible and debuggable, where
				// per-frame error spam is neither.
				SAM_WARN("LUA", "Behaviour '" + name + "' errored and was disabled.");
				// Re-resolve FIRST: the vector may have moved while the script ran, and the
				// row may have changed hands -- the callback can sam_register_behavior its
				// own name before erroring, in which case registerBehavior already unref'd
				// our copy and installed a replacement. Unreffing the stale copy then frees
				// a registry slot that may since have been handed to another callback (a
				// timer, another behaviour), and the next two luaL_refs share one slot.
				// Only release what the row STILL holds.
				const int now = behaviorIndexFor(name);
				if ( now >= 0 && g_behaviors[now].luaRef == luaRef )
				{
					luaL_unref(L, LUA_REGISTRYINDEX, luaRef);
					g_behaviors[now].luaRef = -2;
				}
				// else: re-registered during the call (the replacement stays live), or the
				// registry was torn down meanwhile (clearBehaviors already released it).
			}
			return;
		}
#ifndef SAM_LUA_NO_JS
		if ( jsFn ) { SAMJs::runBehaviorJs(index, jsFn, uid, ns, name); }
#endif
	}

	bool setEntityFacing(unsigned long long uid, double radians)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("SAM", "sam_set_entity_facing refused: host only."); return false; }
		if ( !std::isfinite(radians) )
		{
			SAM_WARN("SAM", "sam_set_entity_facing: angle must be a real number.");
			return false;
		}
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e ) { return false; }
		if ( e->behavior == &actPlayer )
		{
			// A player's facing belongs to whoever is holding the mouse. Turning their head
			// from a script would fight their input every frame.
			SAM_WARN("SAM", "sam_set_entity_facing refused: cannot turn a player.");
			return false;
		}
		// Normalise into [0, 2pi) so a script that keeps adding to an angle does not drift
		// into a huge float, and so the value survives the wire (yaw is sent as yaw*256 in a
		// Sint16, which overflows outside roughly +/-128 radians).
		const double twoPi = 2.0 * PI;
		double y = fmod(radians, twoPi);
		if ( y < 0.0 ) { y += twoPi; }
		e->yaw = y;
		// ENTU carries yaw, and a client turns an entity toward it when it has a client behaviour --
		// which every S.A.M spawn now has (SAMMpEntities::adopt). An entity a client PINS (a ground
		// item, a torch) never takes ENTU, so a S.A.M client is told directly.
		e->flags[UPDATENEEDED] = true;
		SAMMpEntities::transformed(e, SAMMpEntities::Changed::FACING);
		return true;
#else
		(void)uid; (void)radians; return false;
#endif
	}

	bool lookAt(unsigned long long uid, unsigned long long targetUid)
	{
#ifdef SAM_LUA_HAVE_BARONY
		Entity* e = uidToEntity((Sint32)uid);
		Entity* t = uidToEntity((Sint32)targetUid);
		if ( !e || !t ) { return false; }
		return setEntityFacing(uid, atan2(t->y - e->y, t->x - e->x));
#else
		(void)uid; (void)targetUid; return false;
#endif
	}

	double entityFacing(unsigned long long uid)
	{
#ifdef SAM_LUA_HAVE_BARONY
		// The reader's resolver: a sentinel uid (0, or -3 written unsigned) names no one entity.
		Entity* e = samResolveEntityQuiet((long long)uid);
		if ( !e || !std::isfinite((double)e->yaw) ) { return -1.0; }
		// Wrapped into [0, 2pi), the range sam_set_entity_facing writes. A negative yaw is a valid
		// engine value, and returned raw both bindings read it as their "no entity" (-1 -> nil).
		double y = fmod((double)e->yaw, 2.0 * PI);
		if ( y < 0.0 ) { y += 2.0 * PI; }
		return y;
#else
		(void)uid; return -1.0;
#endif
	}

	unsigned long long spawnScriptedEntity(double tileX, double tileY,
		const std::string& behaviourName, const std::string& modelId, const std::string& ns)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("SAM", "sam_spawn_entity refused: host only."); return 0; }
		if ( !map.entities ) { return 0; }
		if ( !std::isfinite(tileX) || !std::isfinite(tileY) )
		{
			SAM_WARN("SAM", "sam_spawn_entity: position must be a real number.");
			return 0;
		}
		// Bounds BEFORE the double->int narrowing below, which is undefined for a value the
		// int cannot hold. Every other SAM spawner checks this; this one did not.
		if ( tileX < 0.0 || tileX >= (double)map.width || tileY < 0.0 || tileY >= (double)map.height )
		{
			SAM_ERROR("SAM", "sam_spawn_entity: tile (" + std::to_string((int)tileX) + ","
				+ std::to_string((int)tileY) + ") is outside the map.");
			return 0;
		}

		// A behaviour runs every frame and can spawn from inside itself, and the engine
		// appends new entities to the END of the list the host loop is still walking -- so a
		// one-line mistake becomes an unbounded spawn that never yields back to the frame.
		// Two bounds: how many can appear in a single tick, and how many can exist at once.
		{
			static Uint32 s_budgetTick = 0;
			static int    s_spawnedThisTick = 0;
			if ( s_budgetTick != ticks ) { s_budgetTick = ticks; s_spawnedThisTick = 0; }
			if ( ++s_spawnedThisTick > 64 )
			{
				SAM_WARN("SAM", "sam_spawn_entity: more than 64 in one tick - refused. A"
					" behaviour spawning every frame is almost always a loop that was meant"
					" to be gated.");
				return 0;
			}
			int live = 0;
			for ( node_t* n = map.entities->first; n != nullptr; n = n->next )
			{
				Entity* e = (Entity*)n->element;
				if ( e && e->behavior == &samScriptedBehavior && ++live > 2000 )
				{
					SAM_WARN("SAM", "sam_spawn_entity: 2000 scripted entities already exist"
						" - refused.");
					return 0;
				}
			}
		}

		std::string full = behaviourName;
		if ( full.find(':') == std::string::npos && !ns.empty() ) { full = ns + ":" + full; }
		const int idx = behaviorIndexFor(full);
		if ( idx < 0 )
		{
			SAM_ERROR("SAM", "sam_spawn_entity: no behaviour named '" + full + "'. Call"
				" sam_register_behavior first -- register at the top of your script rather"
				" than inside the handler that spawns, so the name exists when you use it.");
			return 0;
		}

		int sprite = 0;
		if ( !modelId.empty() )
		{
			sprite = samResolveModelAsset(modelId, ns);
			if ( sprite < 0 )
			{
				char* end = nullptr;
				const long n = std::strtol(modelId.c_str(), &end, 10);
				if ( end && *end == '\0' && n > 0 && n < (long)nummodels ) { sprite = (int)n; }
			}
			if ( sprite < 0 )
			{
				SAM_WARN("SAM", "sam_spawn_entity: model '" + modelId + "' is not registered."
					" Declare it in mod.json \"models\", or pass a vanilla model index."
					" Spawning it invisible.");
				sprite = 0;
			}
		}

		Entity* e = newEntity(sprite, 1, map.entities, nullptr);
		if ( !e ) { return 0; }
		e->x = tileX * 16.0 + 8.0;
		e->y = tileY * 16.0 + 8.0;
		e->z = 0.0;
		e->sizex = 1;
		e->sizey = 1;
		e->behavior = &samScriptedBehavior;
		e->flags[UPDATENEEDED] = true;
		e->flags[PASSABLE] = true;   // the script resolves its own collisions
		e->skill[19] = SAM_SCRIPTED_MARKER;
		e->skill[18] = idx;
		e->skill[17] = -1;
		// Multiplayer: skill[2] = -7 (actEmpty on every client, stock included), a bind code on the
		// ENTU tail that a S.A.M client honours before the sprite switch (so even a door or torch
		// model interpolates), and the mod model announced by NAME. Nothing reads skill[2] here.
		SAMMpEntities::adopt(e, SAMMpEntities::Kind::Scripted);
		SAM_INFO("SAM", "Spawned '" + full + "' at (" + std::to_string((int)tileX) + ","
			+ std::to_string((int)tileY) + ") uid " + std::to_string((unsigned long long)e->getUID()));
		return (unsigned long long)e->getUID();
#else
		(void)tileX; (void)tileY; (void)behaviourName; (void)modelId; (void)ns;
		return 0;
#endif
	}

	int registerBehaviorJs(const std::string& fullName, const std::string& ns, void* jsFn)
	{
		const int existing = behaviorIndexFor(fullName);
		if ( existing >= 0 )
		{
			releaseBehaviorRow(g_behaviors[existing]);   // free what it held, either language
			g_behaviors[existing].jsFn = jsFn;
			g_behaviors[existing].luaRef = -2;
			g_behaviors[existing].ns = ns;
			return existing;
		}
		ScriptedBehavior b; b.name = fullName; b.ns = ns; b.jsFn = jsFn;
		g_behaviors.push_back(b);
		SAM_INFO("JS", "Registered behaviour '" + fullName + "'.");
		return (int)g_behaviors.size() - 1;
	}

	void clearBehaviors()
	{
		clearMonsterBehaviors();   // uids from the old run mean nothing to the new one
		// Release the functions but KEEP the rows.
		//
		// An entity already in the world carries a behaviour INDEX, and it keeps carrying it
		// across a mod reload (/sam_reload works mid-game). If the vector were emptied, that
		// index would later be handed to whatever behaviour happened to register into that
		// slot next -- a turret would silently start running someone else's brain. Keeping
		// the rows means a stale index either finds its own name re-registered, in which case
		// the entity correctly follows the new code, or finds a dead row and simply stops
		// thinking. The table is bounded by (mods x behaviours), so it costs nothing.
		// Every row is released through the shared helper. The previous version nulled jsFn
		// and claimed "the JS runtime frees its own values on shutdown" -- it does not; there
		// is no JS-side registry of these allocations, so every JS behaviour function leaked
		// for the life of the process.
		for ( auto& b : g_behaviors ) { releaseBehaviorRow(b); }
	}

	int dispatchEvent(const Event& ev)
	{
		// dispatchEvent RE-ENTERS: a handler may call a host API that fires another event.
		// The inner dispatch owns the store while it runs, so stash the outer one's and put
		// it back on the way out. Without this, a site that fired an event whose handler
		// happened to trigger a second event would read the INNER event's numbers.
		const std::map<std::string, double>      savedNums = g_lastEventNumbers;
		const std::map<std::string, std::string> savedStrs = g_lastEventStrings;
		struct StoreRestore
		{
			const std::map<std::string, double>* n; const std::map<std::string, std::string>* s;
			int depth;
			~StoreRestore() { if ( depth > 0 ) { g_lastEventNumbers = *n; g_lastEventStrings = *s; } }
		};
		// Depth is what tells an inner dispatch to restore and an outer one to leave its
		// results in place for the engine site to read.
		static int s_dispatchDepth = 0;
		StoreRestore restore{ &savedNums, &savedStrs, s_dispatchDepth };
		++s_dispatchDepth;
		struct DepthPop { int* d; ~DepthPop() { --(*d); } } depthPop{ &s_dispatchDepth };

		// Seed the write-back store FIRST, ahead of every early return in this function.
		//
		// An engine site reads this immediately after dispatching. If a dispatch bailed out
		// early -- no Lua state yet, called before init -- without reseeding, the site would
		// read whatever the PREVIOUS event left behind and silently act on another event's
		// numbers. Seeding with the engine's own values also means an untouched field reads
		// back exactly as it went in.
		g_lastEventNumbers.clear();
		g_lastEventStrings.clear();
		for ( const auto& kv : ev.ints )    { g_lastEventNumbers[kv.first] = (double)kv.second; }
		for ( const auto& kv : ev.strings ) { g_lastEventStrings[kv.first] = kv.second; }

		// Reset BEFORE the early-out guard below. Doing it after meant a shutdown or a
		// pre-init dispatch left a stale `true` latched: every later veto-capable site
		// (itemPickup, castSpell, useItem) then saw a cancel nobody asked for, in a
		// session with no mods loaded at all.
		g_lastDispatchCancelled = false;

		if ( !L )
		{
			// Expected during the pre-mod main-menu/char-select carousel, which equips
			// preview loadouts before mods load. The guard drops the event harmlessly;
			// log it ONCE at info level instead of spamming ERROR on every equip.
			static bool warnedBeforeInit = false;
			if ( !warnedBeforeInit )
			{
				warnedBeforeInit = true;
				SAM_INFO("LUA", "dispatchEvent('" + ev.name + "') before init() — ignored (pre-mod menu; suppressing further notices).");
			}
			return 0;
		}
		// Store seeded and latch reset above, so an engine site that reads after this early
		// return sees its own numbers, exactly as it does for the pre-init return.
		if ( !luaHasHeadroom("on_event handlers") ) { return 0; }

		int delivered = 0;
		g_lastDispatchCancelled = false;
		bool cancelled = false;
		// Preserve the caller's namespace. dispatchEvent can RE-ENTER: a script's on_event
		// may call a host API (sam_apply_effect, sam_fire_hook, ...) that fires another hook,
		// nesting a dispatch inside this one. Restoring (not clearing) g_currentNs keeps the
		// outer script's namespace intact so a later sam_save_data still knows its mod.
		const std::string savedNs = g_currentNs;
#ifdef SAM_LUA_HAVE_BARONY
		// A screen function called with no player inside a handler shows to the player this
		// event is about (sam_net.hpp).
		long long evPlayer = -1;
		for ( const auto& kv : ev.ints ) { if ( kv.first == "player" ) { evPlayer = kv.second; break; } }
		SAMNet::ContextPlayerScope evWho(evPlayer);
#endif
		for ( auto& s : g_scripts )
		{
			if ( !s.enabled || s.callbackRef == LUA_NOREF )
			{
				continue;
			}

			lua_rawgeti(L, LUA_REGISTRYINDEX, s.callbackRef); // push on_event
			pushEventTable(ev);                                // push event table arg

			// Hold our own reference to that table so we can see what the handler wrote to
			// it. protectedCall consumes the argument, so without this the handler's changes
			// would be collected by the GC before we could read them.
			lua_pushvalue(L, -1);
			const int evRef = luaL_ref(L, LUA_REGISTRYINDEX); // pops the duplicate

			g_currentNs = s.ns;
			// ONE result, not zero. This is what lets a mod DECIDE rather than merely watch:
			// a handler that returns exactly `false` is saying "I handled this, skip what the
			// game would have done". Anything else -- true, nil, or no return at all, which is
			// what every existing script does -- means "carry on", so this is backwards
			// compatible with every script ever written for an older framework.
			//
			// Every handler still runs even after one cancels, so two mods watching the same
			// event both see it and neither can silently starve the other. The cancel is the
			// OR of all of them.
			const bool ok = protectedCall(1, 1, "on_event('" + ev.name + "') in " + s.path);
			g_currentNs = savedNs;
			if ( ok )
			{
				// lua_toboolean would treat nil as false and cancel everything by accident,
				// so require a real boolean false.
				if ( lua_isboolean(L, -1) && !lua_toboolean(L, -1) ) { cancelled = true; }
				lua_pop(L, 1); // discard the result (protectedCall left exactly one)
				collectEventWriteBacks(ev, evRef);
				++delivered;
			}
			else
			{
				luaL_unref(L, LUA_REGISTRYINDEX, evRef); // the handler failed; drop its table
				// Error isolation: disable ONLY this script; the rest keep running.
				s.enabled = false;
				SAMLogger::noteScriptError();
				SAM_WARN("LUA", "Script '" + s.path + "' disabled after an on_event error.");
			}
		}

		SAMLogger::noteHookFired(delivered, ev.name.c_str()); // count + open the GAMEPLAY section on the first hook
		// A dispatch that reached NOBODY carries no information -- half a real session's
		// log was "Dispatched 'X' to 0 script(s)". Keep it at DEBUG so it is still there
		// with SAM_DEBUG set when you are working out why a hook is not firing.
		// A hook firing is routine -- one line per dispatch was 80% of a real session's log.
		// The SESSION SUMMARY reports which hooks fired and how often, which is strictly more
		// useful. A CANCEL still logs at INFO: that one changed what the game did.
		if ( cancelled )
		{
			SAM_INFO("LUA", "Dispatched '" + ev.name + "' to " + std::to_string(delivered)
				+ " script(s). (a script cancelled the default behaviour)");
		}
		else
		{
			SAM_DEBUG("LUA", "Dispatched '" + ev.name + "' to " + std::to_string(delivered) + " script(s).");
		}
		g_lastDispatchCancelled = cancelled;
		return delivered;
	}

	// v0.7.0: fire on_tick(event) for every script that defines it, once per game tick
	// (host-only). Deliberately SILENT — no per-tick log line, no hook count — since this
	// runs ~50x/sec. Only scripts with an on_tick are touched; errors disable just that one.
	void dispatchTick(long long tickCount)
	{
		if ( !L ) { return; }
		if ( !luaHasHeadroom("on_tick handlers") ) { return; }
		const std::string savedNs = g_currentNs;
		for ( auto& s : g_scripts )
		{
			if ( !s.enabled || s.tickRef == LUA_NOREF ) { continue; }
			lua_rawgeti(L, LUA_REGISTRYINDEX, s.tickRef); // push on_tick
			lua_newtable(L);
			lua_pushinteger(L, (lua_Integer)tickCount); lua_setfield(L, -2, "tick_count");
			lua_pushinteger(L, 1);                        lua_setfield(L, -2, "delta_ticks");
			g_currentNs = s.ns;
			const bool ok = protectedCall(1, 0, "on_tick in " + s.path);
			g_currentNs = savedNs;
			if ( !ok )
			{
				s.enabled = false;
				SAMLogger::noteScriptError();
				SAM_WARN("LUA", "Script '" + s.path + "' disabled after an on_tick error.");
			}
		}
	}

	// v0.7.0 Feature 2 — damage-interception latch. The host brackets the
	// on_before_damage dispatch with begin()/end(); scripts call modify() (via
	// sam_modify_damage) in between. Shared by both runtimes + Entity::modHP.
	void beforeDamageBegin(int player, long long damage)   { g_bdActive = true; g_bdPlayer = player; g_bdValue = damage; }
	// Clamp to [0, INT_MAX] here rather than at each engine read-back. Every consumer
	// narrows to int, and a script returning something huge would wrap negative -- turning
	// damage into healing. One clamp at the write covers all three read sites.
	static long long samClampHookValue(long long v)
	{
		if ( v < 0 ) { return 0; }
		if ( v > 2147483647LL ) { return 2147483647LL; }
		return v;
	}
	void beforeDamageModify(int player, long long newValue) { if ( g_bdActive && player == g_bdPlayer ) { g_bdValue = samClampHookValue(newValue); } }
	long long beforeDamageEnd()                             { g_bdActive = false; return g_bdValue; }
	bool beforeDamageActive()                              { return g_bdActive; }

	void beforeMonsterDamageBegin(long long damage)         { g_bdmActive = true; g_bdmValue = damage; }
	void beforeMonsterDamageModify(long long newValue)      { if ( g_bdmActive ) { g_bdmValue = samClampHookValue(newValue); } }
	long long beforeMonsterDamageEnd()                      { g_bdmActive = false; return g_bdmValue; }
	bool beforeMonsterDamageActive()                        { return g_bdmActive; }

	void hookValueBegin(const char* hookName, long long value)
	{
		// NESTING IS REFUSED. dispatchEvent can re-enter: a script's handler may call a host
		// API that fires another hook. Without this, the inner hook would overwrite the
		// outer one's value and the outer engine site would then apply a number meant for
		// something else entirely. The engine sites all guard on hookValueActive() before
		// opening, so this is belt-and-braces -- but the guarantee should live here, not
		// depend on every future call site remembering it.
		if ( g_hvActive )
		{
			SAM_WARN("LUA", std::string("hookValueBegin('") + ( hookName ? hookName : "?" )
				+ "') while '" + g_hvName + "' is still open — the inner hook offers no value to rewrite.");
			return;
		}
		g_hvActive = true; g_hvValue = value; g_hvName = hookName ? hookName : "";
	}
	void hookValueModify(long long newValue)  { if ( g_hvActive ) { g_hvValue = samClampHookValue(newValue); } }
	long long hookValueEnd()                  { g_hvActive = false; return g_hvValue; }
	bool hookValueActive()                    { return g_hvActive; }
	const char* hookValueName()               { return g_hvName.c_str(); }

	void recordEventWriteBackNumber(const char* field, double v)
	{
		if ( field ) { g_lastEventNumbers[field] = v; }
	}

	void recordEventWriteBackString(const char* field, const std::string& v)
	{
		if ( field ) { g_lastEventStrings[field] = v; }
	}

	long long lastEventInt(const char* field, long long fallback)
	{
		auto it = g_lastEventNumbers.find(field ? field : "");
		if ( it == g_lastEventNumbers.end() ) { return fallback; }
		// A script can put anything in a number field, including inf and NaN. Narrowing
		// those to an integer is undefined behaviour, so refuse them here rather than let
		// each adopting site remember to. Same for a value no integer can hold.
		const double d = it->second;
		if ( !std::isfinite(d) || d > 9.2e18 || d < -9.2e18 )
		{
			SAM_WARN("SAM", std::string("event field '") + (field ? field : "?")
				+ "' was set to a value that is not a usable number; keeping the original.");
			return fallback;
		}
		return (long long)d;
	}

	double lastEventNumber(const char* field, double fallback)
	{
		auto it = g_lastEventNumbers.find(field ? field : "");
		return ( it == g_lastEventNumbers.end() ) ? fallback : it->second;
	}

	std::string lastEventString(const char* field, const std::string& fallback)
	{
		auto it = g_lastEventStrings.find(field ? field : "");
		return ( it == g_lastEventStrings.end() ) ? fallback : it->second;
	}

	// Did any handler of the last dispatchEvent return false? See the header.
	bool lastDispatchCancelled()                           { return g_lastDispatchCancelled; }
	std::string effectNameHint()                           { return samEffectNameHint(); }

	// ---- v0.7.0 Feature 3: input hooks ----------------------------------------
#ifdef SAM_LUA_HAVE_BARONY
	// Map a key name ("F", "a", "1", "F5") to its SDL keycode, or SDLK_UNKNOWN.
	SDL_Keycode samKeyNameToKeycode(const std::string& name)
	{
		if ( name.empty() ) { return SDLK_UNKNOWN; }
		if ( (name[0] == 'F' || name[0] == 'f') && name.size() >= 2 )
		{
			const int n = std::atoi(name.c_str() + 1);
			if ( n >= 1 && n <= 12 ) { return (SDL_Keycode)(SDLK_F1 + (n - 1)); }
			return SDLK_UNKNOWN;
		}
		if ( name.size() == 1 )
		{
			const char c = name[0];
			if ( c >= 'A' && c <= 'Z' ) { return (SDL_Keycode)(SDLK_a + (c - 'A')); }
			if ( c >= 'a' && c <= 'z' ) { return (SDL_Keycode)(SDLK_a + (c - 'a')); }
			if ( c >= '0' && c <= '9' ) { return (SDL_Keycode)(SDLK_0 + (c - '0')); }
		}
		// SDL already parses the whole keyboard vocabulary -- "Space", "Left Shift", "Return",
		// "Escape" -- which is exactly the set sam_get_action_binding hands back. Without this,
		// anything longer than one character that was not an F-key silently answered false, so
		// feeding one S.A.M call's output into another never worked for most bindings.
		{
			const SDL_Keycode sdl = SDL_GetKeyFromName(name.c_str());
			if ( sdl != SDLK_UNKNOWN ) { return sdl; }
		}
		// A MOUSE binding has no keycode at all, so this can never answer for one. Say that
		// rather than returning false forever: sam_is_action_held is the call that works, and it
		// is what sam_get_action_binding was meant to pair with.
		if ( name.size() > 5 && ( name.compare(0, 5, "Mouse") == 0 || name.compare(0, 5, "mouse") == 0 ) )
		{
			static bool toldOnce = false;
			if ( !toldOnce )
			{
				toldOnce = true;
				SAM_WARN("LUA", "sam_is_key_held cannot answer for a mouse binding ('" + name
					+ "'). Use sam_is_action_held(player, action), which handles every binding kind.");
			}
		}
		return SDLK_UNKNOWN;
	}

	// The supported keys (A-Z, 0-9, F1-F12) as (name, keycode), built once.
	const std::vector<std::pair<std::string, SDL_Keycode>>& samKeyList()
	{
		static const std::vector<std::pair<std::string, SDL_Keycode>> keys = []() {
			std::vector<std::pair<std::string, SDL_Keycode>> v;
			for ( char c = 'A'; c <= 'Z'; ++c ) { v.push_back({ std::string(1, c), (SDL_Keycode)(SDLK_a + (c - 'A')) }); }
			for ( char c = '0'; c <= '9'; ++c ) { v.push_back({ std::string(1, c), (SDL_Keycode)(SDLK_0 + (c - '0')) }); }
			for ( int n = 1; n <= 12; ++n ) { v.push_back({ "F" + std::to_string(n), (SDL_Keycode)(SDLK_F1 + (n - 1)) }); }
			return v;
		}();
		return keys;
	}
#endif

	// Poll supported keys once per game tick and fire on_key_pressed / on_key_released
	// on state transitions (host + gameplay only; the caller gates on !intro/!CLIENT).
	// Single-fire per press — sam_is_key_held covers continuous checks.
	void pollInput()
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( !L ) { return; }
		static std::unordered_map<SDL_Keycode, bool> prev;
		// The player the keyboard belongs to: in splitscreen the seat the game gave it to, otherwise
		// this machine's own. Only the host and singleplayer run this; a joiner's keys reach the host
		// through SAMMpInput and fire there with player = that joiner.
		const int player = SAMMpInput::keyboardPlayer();
		// Nothing counts as held while a text field has focus (the chat box, the console): the
		// engine sets keystatus on every key down regardless, so a mod bound to "F" used to fire
		// every time someone typed an f. Reading everything as released rather than skipping the
		// poll keeps press and release paired -- a key that was down when the box opened still
		// gets its release edge. SAMMpInput::clientTick and pollLocalActions do the same, so
		// every machine and every seat agrees.
		const bool typing = ( SDL_IsTextInputActive() == SDL_TRUE );
		for ( const auto& kv : samKeyList() )
		{
			auto it = keystatus.find(kv.second);
			const bool down = !typing && ( it != keystatus.end() && it->second );
			const bool was  = prev[kv.second];
			if ( down != was )
			{
				const char* evName = down ? "on_key_pressed" : "on_key_released";
				SAMLua::Event ev; ev.setName(evName);
				ev.i("player", player); ev.s("key_name", kv.first);
				if ( down ) { ev.i("held", 0); }
				SAMLua::dispatchEvent(ev);

				SAMJs::Event jsev; jsev.setName(evName);
				jsev.i("player", player); jsev.s("key_name", kv.first);
				if ( down ) { jsev.i("held", 0); }
				SAMJs::dispatchEvent(jsev);
			}
			prev[kv.second] = down;
		}
#endif
	}

	bool isKeyHeld(const std::string& name)
	{
#ifdef SAM_LUA_HAVE_BARONY
		const SDL_Keycode kc = samKeyNameToKeycode(name);
		if ( kc == SDLK_UNKNOWN ) { return false; }
		// Nothing is held while a text field has focus, the same rule pollInput applies to the
		// press/release events and clientTick applies to what a joiner reports. Without this the
		// READ disagrees with the EVENT: sam_is_key_held("F") answered true while this machine
		// typed an f in chat, and false for a joiner doing exactly the same thing, because their
		// machine gated it before reporting. Host and joiner must answer alike.
		if ( SDL_IsTextInputActive() == SDL_TRUE ) { return false; }
		auto it = keystatus.find(kc);
		return it != keystatus.end() && it->second;
#else
		(void)name; return false;
#endif
	}

	// ---- bound-action hooks ----------------------------------------------------
	//
	// The gameplay-relevant subset of Barony's own action names (ui/MainMenu.cpp
	// defaultBindings). We look them up BY NAME, so whatever the player rebound an
	// action to is what a script sees — that is the entire reason this exists instead
	// of the raw-key path above.
	//
	// THE ORDER IS THE 'SAMA' WIRE FORMAT: append only, never reorder or remove, or a
	// client and host on different builds would disagree about which action fired.
	//
	// Note "Hotbar Up / Select" is right-click by default: a mod CAN react to it, and
	// because we only observe, the hotbar keeps working normally.
	const std::vector<std::string>& samActionList()
	{
		static const std::vector<std::string> v = {
			"Attack", "Defend", "Use", "Cast Spell", "Sneak",
			"Hotbar Up / Select", "Hotbar Down / Cancel", "Hotbar Left", "Hotbar Right",
			"Call Out", "Command NPC", "Quick Turn",
		};
		return v;
	}

	const char* actionNameForIndex(int index)
	{
		const auto& v = samActionList();
		return ( index >= 0 && index < (int)v.size() ) ? v[index].c_str() : "";
	}

	bool isActionHeld(int player, const std::string& action)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS ) { return false; }
		// A player on another machine: from what that machine reported to the host (SAMMpInput);
		// false on a client. Input::inputs[] would answer with THIS machine's buttons.
		if ( SAMMpInput::onOtherMachine(player) ) { return SAMMpInput::remoteActionHeld(player, action); }
		// Nothing is held while a text field has focus -- pollLocalActions gates the events this
		// way and a joiner gates what it reports, so an ungated read here is the one place where
		// the local player and a remote player answer differently while both are typing.
		if ( SDL_IsTextInputActive() == SDL_TRUE ) { return false; }
		// const read — cannot touch binding_t::consumed, so vanilla is unaffected.
		return Input::inputs[player].binary(action.c_str());
#else
		(void)player; (void)action; return false;
#endif
	}

	const char* actionBinding(int player, const std::string& action)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS ) { return ""; }
		// The binding that player's own machine reported ("" until it has, and always on a client).
		if ( SAMMpInput::onOtherMachine(player) ) { return SAMMpInput::remoteActionBinding(player, action); }
		const char* b = Input::inputs[player].binding(action.c_str()); // "" when unbound
		// "[unbound]" and "[hidden]" are the engine's own sentinels for "nothing here"; the
		// reference promises nil for those, and a prompt must not print them.
		if ( b && (strcmp(b, MainMenu::emptyBinding) == 0 || strcmp(b, MainMenu::hiddenBinding) == 0) ) { return ""; }
		return b;
#else
		(void)player; (void)action; return "";
#endif
	}

	long long randomDraw(const std::string& ns, const std::string& stream, long long lo, long long hi)
	{
		return samRandomDraw(ns, stream, lo, hi);
	}

	void resetRandomStreams()
	{
		// Called when a run begins so every machine starts each named stream at draw 0.
		g_rngCounters.clear();
	}

	int lobbyFlag(const std::string& name, bool& ok)
	{
#ifdef SAM_LUA_HAVE_BARONY
		return samLobbyFlag(name, ok);
#else
		(void)name; ok = false; return 0;
#endif
	}

	const char* lobbyFlagNames()
	{
#ifdef SAM_LUA_HAVE_BARONY
		return samLobbyFlagNames();
#else
		return "";
#endif
	}

	Entity* resolveWritableEntity(long long uid, const char* who) { return samResolveWritable(uid, who); }
	int resolveEntityFlag(const char* name, bool forWrite, const char* who) { return samResolveEntityFlag(name, forWrite, who); }
	bool isCompanionEntity(const Entity* e)
	{
#ifdef SAM_LUA_HAVE_BARONY
		return e && e->behavior == &samCompanionBehavior;
#else
		(void)e; return false;
#endif
	}
	bool applyForceTo(Entity* e, double force, double angle, int ticks) { return e ? samApplyForce(e, force, angle, ticks) : false; }
	bool isDamageImmune(unsigned int uid)
	{
		if ( g_damageImmune.empty() ) { return false; }   // the vanilla path, one comparison
		return g_damageImmune.count((Uint32)uid) > 0;
	}
	void clearDamageImmune()
	{
		g_damageImmune.clear();
		// The behaviour map is keyed by uid too, and it does something stronger than block damage:
		// it runs mod script against whatever owns that uid, every frame. This batch taught one
		// uid-keyed store to clear on a floor boundary and left its neighbour alone.
		clearMonsterBehaviors();
		// The removal queue goes with it. It is normally drained on the very next frame, but a
		// level change between the queue and the drain would leave a uid naming whatever entity
		// inherits that number on the new floor -- and the engine hands numbers back out (see the
		// rollback sites named above). One line, and the window closes.
		g_pendingRemove.clear();
	}
	void setDamageImmune(unsigned int uid, bool on)
	{
		if ( on ) { g_damageImmune.insert((Uint32)uid); } else { g_damageImmune.erase((Uint32)uid); }
	}

	bool queueRemoveEntity(unsigned int uid, const char* who) { return samQueueRemoveEntity((Uint32)uid, who); }

	void drainRemoveQueue()
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( g_pendingRemove.empty() ) { return; }
		// Swapped out first: a removal can run engine code, and anything queued during this pass
		// belongs to the next frame rather than to an unbounded loop here.
		std::vector<Uint32> batch;
		batch.swap(g_pendingRemove);
		for ( Uint32 uid : batch )
		{
			Entity* e = uidToEntity((Sint32)uid);
			if ( !e ) { continue; }                       // ordinary gameplay got there first
			if ( e->behavior == &actPlayer ) { continue; } // became a player between queue and drain
			// A chest open RIGHT NOW is also in openedChest[], which neither list_RemoveNode nor
			// ~Entity clears, so deleting it left the still-open UI reading freed memory.
			if ( e->behavior == &actChest ) { e->closeChest(); }
			e->removeLightField();
			// Drop any script behaviour attached to this uid BEFORE the entity goes away. The map
			// is keyed by uid and the engine hands uids back out -- it rolls the counter back for
			// throwaway particles -- so leaving the row meant a later monster could inherit a dead
			// boss's per-frame script and run it once a tick against the wrong creature.
			detachMonsterBehavior((unsigned long long)uid);
			// A player's follower leaves stats[c]->FOLLOWERS, a remote leader is sent LDEL (before
			// ~Entity sends ENTD, as the engine's death path does), a local leader's follower menu lets
			// go of it -- the same steps actmonster.cpp takes when a follower dies.
			SAMMpEntities::beforeRemove(e);
			if ( e->mynode ) { list_RemoveNode(e->mynode); }
		}
#endif
	}
	Entity* resolveReadableEntity(long long uid, const char* who) { return samResolveEntityRead(uid, who); }
	Entity* resolveEntityQuiet(long long uid) { return samResolveEntityQuiet(uid); }

	// ---- batch 4, combat: one decision, both runtimes -------------------------------------
	Entity* resolveCombatant(long long uid, Stat** outStats)
	{
#ifdef SAM_LUA_HAVE_BARONY
		return samResolveCombatant(uid, outStats);
#else
		(void)uid; (void)outStats; return nullptr;
#endif
	}
	Entity* resolveCombatantWritable(long long uid, const char* who, Stat** outStats)
	{
#ifdef SAM_LUA_HAVE_BARONY
		return samResolveCombatantWritable(uid, who, outStats);
#else
		(void)uid; (void)who; (void)outStats; return nullptr;
#endif
	}
	bool damageTypeFromName(const char* name, const char* who, int* out)
	{
#ifdef SAM_LUA_HAVE_BARONY
		return samDamageTypeArg(name, who, out);
#else
		(void)name; (void)who; (void)out; return false;
#endif
	}

	void migrateLegacyDataFile(const std::string& dir, const std::string& key) { samMigrateLegacyDataFile(dir, key); }
	int resolveModelAssetIn(const std::string& id, const std::string& ns) { return samResolveModelAsset(id, ns); }
	int resolveSoundAssetIn(const std::string& id, const std::string& ns) { return samResolveSoundAsset(id, ns); }
	std::string encodeDataKey(const std::string& key) { return samEncodeKey(key); }
	std::string decodeDataKey(const std::string& fileStem) { return samDecodeKey(fileStem); }

	std::string modDataDir(const std::string& ns)
	{
		return samModDataDir(ns);
	}

	bool sendModPacket(int target, const std::string& tag, const std::string& payload)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == SINGLE ) { return false; }
		// In the lobby neither side's packet table has 'SAMP': the packet was dropped as a "mystery
		// packet" while this returned true. Say so instead; packets travel once the game has started.
		if ( intro )
		{
			SAMNet::warnOnce("packet:lobby", "sam_send_packet: not sent -- packets only travel once the game has"
				" started (the lobby drops them). Send it from game.on_game_start or later.");
			return false;
		}
		if ( tag.empty() || tag.size() > SAMLua::SAM_PACKET_MAX_TAG ) { return false; }
		if ( payload.size() > SAMLua::SAM_PACKET_MAX_PAYLOAD ) { return false; }
		if ( !net_packet ) { return false; }

		// [0..3] "SAMP" | [4] sender player | [5] tag length | tag | payload
		memcpy(net_packet->data, "SAMP", 4);
		net_packet->data[4] = (Uint8)((multiplayer == CLIENT) ? clientnum : 0);
		net_packet->data[5] = (Uint8)tag.size();
		memcpy(&net_packet->data[6], tag.data(), tag.size());
		memcpy(&net_packet->data[6 + tag.size()], payload.data(), payload.size());
		net_packet->len = (int)(6 + tag.size() + payload.size());

		if ( multiplayer == CLIENT )
		{
			// A client can only ever talk to the host.
			net_packet->address.host = net_server.host;
			net_packet->address.port = net_server.port;
			sendPacketSafe(net_sock, -1, net_packet, 0);
			return true;
		}

		// Host: one client, or all of them.
		bool sentAny = false;
		for ( int c = 1; c < MAXPLAYERS; ++c )
		{
			if ( client_disconnected[c] ) { continue; }
			if ( target >= 0 && target != c ) { continue; }
			net_packet->address.host = net_clients[c - 1].host;
			net_packet->address.port = net_clients[c - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, c - 1);
			sentAny = true;
		}
		return sentAny;
#else
		(void)target; (void)tag; (void)payload; return false;
#endif
	}

	bool travelToLevel(int target, bool secret, const char* tag)
	{
#ifdef SAM_LUA_HAVE_BARONY
		// Three preconditions the engine does not enforce, because until now nothing but a
		// ladder ever set these globals, and a ladder cannot be touched when they are unsafe.
		//
		// 1. Only the host may move the party: the block that consumes these globals lives
		//    inside `if ( multiplayer != CLIENT )`, so a client would arm a request that
		//    never fires and then leaks into its next game.
		if ( multiplayer == CLIENT )
		{
			SAM_WARN(tag, "sam_travel_to_level refused: host only.");
			return false;
		}
		// 2. A level change already in flight -- overwriting it would apply both offsets.
		if ( loadnextlevel )
		{
			SAM_WARN(tag, "sam_travel_to_level refused: a level change is already under way.");
			return false;
		}
		// 3. We must actually be in a game; firing this from a menu would run the level
		//    loader against a map that is not there.
		if ( intro || loading )
		{
			SAM_WARN(tag, "sam_travel_to_level refused: not in a game yet.");
			return false;
		}
		if ( target < 0 || target > 100 )
		{
			SAM_WARN(tag, "sam_travel_to_level: floor " + std::to_string(target)
				+ " is out of range (0-100).");
			return false;
		}

		// 4. And two destinations that work perfectly in single player but SPLIT THE PARTY
		//    in multiplayer. The host announces the move with an LVLC packet, and the
		//    client's own handler discards exactly these two cases before acting on it:
		//
		//        if ( currentlevel == data[13] && secretlevel == data[4] ) return;
		//        if ( data[13] == 0 ) return;   // "dont warp back to start level"
		//
		//    (net.cpp:5552-5560; the host sends data[13] = the NEW currentlevel and
		//    data[4] = the NEW secretlevel, game.cpp:2169/2172.)
		//
		//    So a mod sending everyone to floor 0 -- a perfectly natural home for a hub --
		//    or reloading the floor they are standing on would travel the host and leave
		//    every client behind on a map the host has left. Refuse loudly instead.
		if ( multiplayer != SINGLE )
		{
			if ( target == 0 )
			{
				SAM_WARN(tag, "sam_travel_to_level refused: floor 0 cannot be reached in"
					" multiplayer -- a connected client ignores a move to it and would be"
					" left behind. Put a hub on any other floor.");
				return false;
			}
			if ( target == currentlevel && secret == secretlevel )
			{
				SAM_WARN(tag, "sam_travel_to_level refused: reloading the floor the party is"
					" already on would move the host only, splitting the party. (Changing the"
					" secret flag at the same time is fine.)");
				return false;
			}
		}

		// The consuming code adds skipLevelsOnLoad and then adds one MORE unless the skip
		// was positive, so the arithmetic differs above and below the current floor:
		//     skip > 0  ->  currentlevel += skip
		//     otherwise ->  currentlevel += skip; ++currentlevel
		// Inverting that for an absolute destination gives exactly this.
		skipLevelsOnLoad = ( target > currentlevel ) ? ( target - currentlevel )
		                                             : ( target - currentlevel - 1 );
		secretlevel = secret;
		loadnextlevel = true;
		SAM_INFO(tag, "Travelling to floor " + std::to_string(target)
			+ (secret ? " (secret list)." : "."));
		return true;
#else
		(void)target; (void)secret; (void)tag;
		return false;
#endif
	}

	void dispatchUiEvent(const std::string& eventName, const std::string& ns,
		const std::string& panel, const std::string& widget, const std::string& value)
	{
		// Cross-runtime, same as on_packet: the click has to reach whichever language the
		// panel's owner wrote their mod in, and a mod may mix the two.
		Event ev;
		ev.setName(eventName.c_str()).s("mod", ns).s("panel", panel).s("widget", widget).s("value", value);
		dispatchEvent(ev);
#ifndef SAM_LUA_NO_JS
		SAMJs::Event jev;
		jev.setName(eventName.c_str()).s("mod", ns).s("panel", panel).s("widget", widget).s("value", value);
		SAMJs::dispatchEvent(jev);
#endif
	}

	void dispatchModPacket(int fromPlayer, const std::string& tag, const std::string& payload)
	{
		Event ev;
		ev.setName("on_packet").i("from", fromPlayer).s("tag", tag).s("payload", payload);
		dispatchEvent(ev);
#ifndef SAM_LUA_NO_JS
		SAMJs::Event jev;
		jev.setName("on_packet").i("from", fromPlayer).s("tag", tag).s("payload", payload);
		SAMJs::dispatchEvent(jev);
#endif
	}

	void dispatchAction(int player, int actionIndex, bool pressed)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( !L ) { return; }
		if ( player < 0 || player >= MAXPLAYERS ) { return; }
		const char* action = actionNameForIndex(actionIndex);
		if ( !action || !action[0] ) { return; }

		// The physical input behind the action, for prompts ("press Mouse3"). Only the
		// LOCAL player's bindings live on this machine — Input::inputs[] forwards every
		// remote index to inputs[0] in a netgame — so reporting it for a remote player
		// would be this machine's binding, not theirs. Send it only when it's really ours.
		const char* bindTo = ( player == clientnum ) ? actionBinding(player, action) : "";

		const char* evName = pressed ? "on_action_pressed" : "on_action_released";
		SAMLua::Event ev; ev.setName(evName);
		ev.i("player", player); ev.s("action", action); ev.s("binding", bindTo ? bindTo : "");
		SAMLua::dispatchEvent(ev);

		SAMJs::Event jsev; jsev.setName(evName);
		jsev.i("player", player); jsev.s("action", action); jsev.s("binding", bindTo ? bindTo : "");
		SAMJs::dispatchEvent(jsev);
#else
		(void)player; (void)actionIndex; (void)pressed;
#endif
	}

	void pollActions()
	{
#ifdef SAM_LUA_HAVE_BARONY
		// Every seat whose buttons are on this machine, each with its own edges and binding. A client
		// does nothing here: SAMMpInput's tick hook reports its buttons to the host in order, so no
		// 'SAMA' is sent any more (the host still accepts one from an older S.A.M client).
		SAMMpInput::pollLocalActions();
#endif
	}

	// v0.7.0 Feature 4 — per-monster scratch-data accessors (JSON-string values), shared
	// with the JS runtime and cleared on shutdown.
	void monsterDataSet(unsigned uid, const std::string& key, const std::string& jsonValue) { g_monsterData[uid][key] = jsonValue; }
	std::string monsterDataGet(unsigned uid, const std::string& key)
	{
		auto mit = g_monsterData.find(uid);
		if ( mit == g_monsterData.end() ) { return std::string(); }
		auto kit = mit->second.find(key);
		return ( kit == mit->second.end() ) ? std::string() : kit->second;
	}
	void monsterDataClear() { g_monsterData.clear(); }

	// v1.2.9 — per-player scratch-data accessors (JSON-string values), shared with the JS
	// runtime and cleared on new game/shutdown.
	void playerDataSet(int player, const std::string& key, const std::string& jsonValue)
	{
		if ( player >= 0 && player < MAXPLAYERS ) { g_playerData[player][key] = jsonValue; }
	}
	std::string playerDataGet(int player, const std::string& key)
	{
		if ( player < 0 || player >= MAXPLAYERS ) { return std::string(); }
		auto kit = g_playerData[player].find(key);
		return ( kit == g_playerData[player].end() ) ? std::string() : kit->second;
	}
	void playerDataClear() { for ( int i = 0; i < MAXPLAYERS; ++i ) { g_playerData[i].clear(); } }

	void tickTimers()
	{
		if ( !L || g_timers.empty() ) { return; }
		if ( !luaHasHeadroom("timer callbacks") ) { return; }
		struct Due { std::string ns; int ref; bool oneShot; };
		std::vector<Due> due;
		for ( size_t i = 0; i < g_timers.size(); )
		{
			Timer& t = g_timers[i];
			if ( --t.remaining > 0 ) { ++i; continue; }
			if ( t.repeating )
			{
				// Take an OWN reference for the due-list. If an earlier due callback
				// this tick cancels + re-registers this repeating timer, samRemoveTimer
				// luaL_unref's t.callbackRef and its registry slot can be recycled by a
				// new luaL_ref — a bare shared ref would then fetch the WRONG callback.
				// The dup is independent of t.callbackRef and is freed right after firing
				// (Due.oneShot == true). Matches the JS runtime's JS_DupValue approach.
				lua_rawgeti(L, LUA_REGISTRYINDEX, t.callbackRef);
				const int ownRef = luaL_ref(L, LUA_REGISTRYINDEX);
				due.push_back({ t.ns, ownRef, true });
				t.remaining = t.interval > 0 ? t.interval : 1;
				++i;
			}
			else
			{
				due.push_back({ t.ns, t.callbackRef, true }); // ownership transfers to `due`
				g_timers.erase(g_timers.begin() + i);
			}
		}
		const std::string savedNs = g_currentNs; // restore (not clear) for re-entrant safety
		for ( const Due& d : due )
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, d.ref);
			g_currentNs = d.ns;
			protectedCall(0, 0, "timer callback");
			g_currentNs = savedNs;
			if ( d.oneShot ) { luaL_unref(L, LUA_REGISTRYINDEX, d.ref); }
		}
	}

	// Drop every pending timer (used on a new game so a prior run's timers don't
	// carry over). Safe to call whether or not the VM is initialized.
	void resetTimers()
	{
		if ( L )
		{
			for ( auto& t : g_timers )
			{
				if ( t.callbackRef != LUA_NOREF ) { luaL_unref(L, LUA_REGISTRYINDEX, t.callbackRef); }
			}
		}
		g_timers.clear();
	}

	void shutdown()
	{
		if ( !L )
		{
			return;
		}
		for ( auto& t : g_timers )
		{
			if ( t.callbackRef != LUA_NOREF ) { luaL_unref(L, LUA_REGISTRYINDEX, t.callbackRef); }
		}
		g_timers.clear();
		g_monsterData.clear(); // v0.7.0 F4: drop per-monster scratch data on teardown
		for ( int i = 0; i < MAXPLAYERS; ++i ) { g_playerData[i].clear(); } // v1.2.9: per-player scratch
		for ( auto& s : g_scripts )
		{
			if ( s.callbackRef != LUA_NOREF )
			{
				luaL_unref(L, LUA_REGISTRYINDEX, s.callbackRef);
			}
		}
		g_scripts.clear();
		// Behaviours die with the scripts that registered them. Leaving them would point
		// entities in the world at a Lua ref belonging to a closed state -- the same class of
		// "teardown hooked to nothing" that let panels outlive a run. clearBehaviors runs
		// BEFORE lua_close so the refs are released against a live state.
		clearBehaviors();

		const std::size_t peak = g_alloc.peak;
		lua_close(L);
		L = nullptr;

		SAM_INFO("LUA", "Runtime shut down (peak Lua memory " + std::to_string(peak) + " bytes).");
	}

	std::size_t scriptCount() { return g_scripts.size(); }

	std::size_t enabledScriptCount()
	{
		std::size_t n = 0;
		for ( const auto& s : g_scripts ) { if ( s.enabled ) { ++n; } }
		return n;
	}

	std::size_t memoryUsedBytes() { return g_alloc.used; }
	std::size_t memoryPeakBytes() { return g_alloc.peak; }
	bool isInitialized() { return L != nullptr; }

	void noteKill(int player)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player >= 0 && player < MAXPLAYERS ) { ++g_samSessionKills[player]; }
#else
		(void)player;
#endif
	}
	long long getKills(int player)
	{
#ifdef SAM_LUA_HAVE_BARONY
		return ( player >= 0 && player < MAXPLAYERS ) ? (long long)g_samSessionKills[player] : 0;
#else
		(void)player; return 0;
#endif
	}
	void resetKills()
	{
#ifdef SAM_LUA_HAVE_BARONY
		for ( int i = 0; i < MAXPLAYERS; ++i ) { g_samSessionKills[i] = 0; }
#endif
	}

	// Public wrapper so the JS runtime resolves effect names through the SAME table
	// (see the header note — two copies is exactly how this got stuck at 14 of 135).
	int effectIdFromName(const char* name)
	{
#ifdef SAM_LUA_HAVE_BARONY
		return samEffectNameToId(name);
#else
		(void)name; return -1;
#endif
	}

	// Reverse of effectIdFromName: canonical name for an effect id (vanilla name, or
	// "CUSTOM:<id>" for the 135.. pseudo-effect slots), empty for an unnamed slot. Shared
	// so the JS runtime's sam_get_effects can label effects without its own table.
	std::string effectNameFromId(int id)
	{
		// The SAME strings the engine's effect_name event emits: lowercase for a vanilla effect,
		// the mod's "ns:effect" id for a custom one. This used to answer UPPERCASE "POISONED" and
		// "CUSTOM:135" while the event said "poisoned" and "mymod:frostbite". Both forms resolve
		// on the way back IN, so nothing ever failed -- but a script comparing its own
		// sam_get_effects() against the event it had just been handed never matched.
#ifdef SAM_LUA_HAVE_BARONY
		{
			const std::string custom = SAMEffects::nameForSlot(id);
			if ( !custom.empty() ) { return custom; }
		}
#endif
		for ( const auto& e : samEffectNames )
		{
			if ( e.id == id )
			{
				std::string n = e.name;
				for ( char& c : n ) { c = (char)std::tolower((unsigned char)c); }
				return n;
			}
		}
		return std::string();
	}

	double getMoveSpeedMult(int player)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS ) { return 1.0; }
		// Sanitize on the way OUT as well as in. This is called from the movement inner
		// loop, where the cost is a compare and the alternative is a NaN reaching
		// PLAYER_VELX — cheap insurance against any future writer that skips the setter.
		return samSanitizeSpeed(g_samMoveSpeed[player]);
#else
		(void)player; return 1.0;
#endif
	}

	void setMoveSpeedMult(int player, double mult)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS ) { return; }
		if ( multiplayer == CLIENT ) { return; } // host is authoritative; see the header
		g_samMoveSpeed[player] = samSanitizeSpeed(mult);
		samSendMoveSpeed(player);
#else
		(void)player; (void)mult;
#endif
	}

	void applyMoveSpeedMult(int player, double mult)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS ) { return; }
		// Receive side: store only. Re-sending here would bounce the value back at the
		// host, and on a listen server that is an infinite loop.
		g_samMoveSpeed[player] = samSanitizeSpeed(mult);
#else
		(void)player; (void)mult;
#endif
	}

	void resetMoveSpeed()
	{
#ifdef SAM_LUA_HAVE_BARONY
		// Deliberately local-only: a new game tears every client's session down anyway, and
		// sending here would touch net_packet from the menu, off the game thread's cadence.
		for ( int i = 0; i < MAXPLAYERS; ++i ) { g_samMoveSpeed[i] = 1.0; }
#endif
	}

	// ---- v1.4.0 floating companion ("Stand") — shared by both runtimes ----------
	// The behavior function samCompanionBehavior + its constants live in the anonymous
	// namespace above (they need Barony types); these public entry points just drive them.

	unsigned long long spawnCompanion(int player, const std::string& modelId, double scale,
		const std::string& assetNs)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("SAM", "spawnCompanion refused: host only."); return 0; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("SAM", "spawnCompanion: invalid player index " + std::to_string(player) + "."); return 0; }
		const int modelIdx = samResolveModelAsset(modelId, assetNs);
		if ( modelIdx < 0 )
		{ SAM_ERROR("SAM", "spawnCompanion: no registered model '" + modelId + "' (is it in the mod's models[]?)."); return 0; }
		Entity* owner = players[player]->entity;
		Entity* e = newEntity(modelIdx, 1, map.entities, nullptr);
		if ( !e ) { SAM_ERROR("SAM", "spawnCompanion: entity creation failed."); return 0; }
		e->sprite = modelIdx;                          // custom .vox model index (3D voxel path)
		e->x = owner->x;
		e->y = owner->y;
		e->z = owner->z - SAM_COMPANION_RISE;
		e->sizex = 2;
		e->sizey = 2;
		double s = scale;
		if ( !(s > 0.0) ) { s = 1.0; }                  // catches <=0 AND NaN (both make !(s>0) true)
		if ( s > 8.0 )    { s = 8.0; }                  // sane visual ceiling
		e->scalex = s; e->scaley = s; e->scalez = s;
		e->yaw = owner->yaw;
		e->flags[PASSABLE] = true;                      // walkable / never collide
		e->flags[BRIGHT]   = true;
		e->flags[SPRITE]   = false;                     // keep the 3D voxel path, not a flat billboard
		e->behavior = &samCompanionBehavior;
		e->skill[17] = player;                          // owner index; skill[2] is the client code (adopt)
		e->skill[18] = 0;                               // not punching
		e->skill[19] = 2;                               // S.A.M companion marker (portal uses 1)
		e->fskill[0] = 0.0;                             // hover-bob phase
		// Every player sees it: UPDATENEEDED, skill[2] = -7 plus a companion code on the ENTU tail
		// (a S.A.M client animates it from the owner itself), the model by NAME, and the scale
		// clamped to the 1.99 ENTU can carry (warned).
		SAMMpEntities::adopt(e, SAMMpEntities::Kind::Companion, player);
		SAM_INFO("SAM", "spawnCompanion: model '" + modelId + "' for player " + std::to_string(player)
		                + " uid " + std::to_string(e->getUID()));
		return (unsigned long long)e->getUID();
#else
		(void)player; (void)modelId; (void)scale; return 0;
#endif
	}

	unsigned long long spawnProjectile(int owner, double tileX, double tileY, double angle,
		double speed, int damage, int lifetimeTicks, const std::string& modelId,
		const std::string& assetNs)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("SAM", "sam_spawn_projectile refused: host only.");
			return 0;
		}
		if ( !map.entities ) { return 0; }

		// Reject non-finite geometry before it reaches an Entity. Lua raises a clean error
		// for a nil argument, but QuickJS turns `undefined` into NaN and reports SUCCESS --
		// so a JS mod passing the result of a failed lookup got a projectile whose position
		// was NaN, which then propagates into the entity list rather than failing loudly.
		// Checked here, in the shared spawner, so both runtimes behave the same way.
		if ( !std::isfinite(tileX) || !std::isfinite(tileY)
			|| !std::isfinite(angle) || !std::isfinite(speed) )
		{
			SAM_WARN("SAM", "sam_spawn_projectile: position, angle and speed must be real"
				" numbers (got a NaN or infinity -- check the values you passed in).");
			return 0;
		}
		// A projectile with no speed never moves and never expires by distance, so it would
		// sit in the world burning a slot until its lifetime ran out. Refuse it.
		if ( !(speed > 0.0) )
		{
			SAM_WARN("SAM", "sam_spawn_projectile: speed must be greater than 0.");
			return 0;
		}
		int life = lifetimeTicks;
		if ( life <= 0 ) { life = 100; }        // ~2 seconds at 50 ticks/sec
		if ( life > 1000 ) { life = 1000; }     // a projectile is a shot, not a pet

		int sprite = 0;
		if ( !modelId.empty() )
		{
			sprite = samResolveModelAsset(modelId, assetNs);
			if ( sprite < 0 )
			{
				char* end = nullptr;
				const long n = std::strtol(modelId.c_str(), &end, 10);
				if ( end && *end == '\0' && n > 0 && n < (long)nummodels ) { sprite = (int)n; }
			}
			if ( sprite < 0 )
			{
				SAM_WARN("SAM", "sam_spawn_projectile: model '" + modelId
					+ "' is not registered. Declare it in mod.json \"models\", or pass a vanilla"
					" model index. Firing an invisible projectile instead.");
				sprite = 0;
			}
		}

		Entity* e = newEntity(sprite, 1, map.entities, nullptr);
		if ( !e ) { return 0; }
		e->x = tileX * 16.0 + 8.0;
		e->y = tileY * 16.0 + 8.0;
		e->z = 0.0;
		e->yaw = angle;
		e->sizex = 1;
		e->sizey = 1;
		e->behavior = &samProjectileBehavior;
		e->flags[UPDATENEEDED] = true;
		e->flags[PASSABLE] = true;
		e->skill[19] = SAM_PROJECTILE_MARKER;
		e->skill[17] = damage < 0 ? 0 : damage;
		e->skill[18] = life;
		e->fskill[2] = cos(angle) * speed;
		e->fskill[3] = sin(angle) * speed;
		// parent is what fired it, so the shot cannot kill its owner on frame one.
		if ( owner >= 0 && owner < MAXPLAYERS && players[owner] && players[owner]->entity )
		{
			e->parent = players[owner]->entity->getUID();
		}
		// Multiplayer: skill[2] = -7 and a velocity from fskill[2]/[3], so every client binds plain
		// interpolation and dead-reckons the shot between the 8 Hz updates (SAMMpEntities::adopt).
		SAMMpEntities::adopt(e, SAMMpEntities::Kind::Projectile);
		return (unsigned long long)e->getUID();
#else
		(void)owner; (void)tileX; (void)tileY; (void)angle; (void)speed;
		(void)damage; (void)lifetimeTicks; (void)modelId;
		return 0;
#endif
	}

	void dispatchProjectileHit(unsigned long long projectile, unsigned long long target,
		int x, int y, int damage)
	{
		Event ev;
		ev.setName("on_projectile_hit").i("projectile", (long long)projectile)
			.i("target", (long long)target).i("x", x).i("y", y).i("damage", damage);
		dispatchEvent(ev);
#ifndef SAM_LUA_NO_JS
		SAMJs::Event jev;
		jev.setName("on_projectile_hit").i("projectile", (long long)projectile)
			.i("target", (long long)target).i("x", x).i("y", y).i("damage", damage);
		SAMJs::dispatchEvent(jev);
#endif
	}

	bool companionPunch(unsigned long long uid)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { return false; }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e || e->behavior != &samCompanionBehavior ) { return false; } // only a live companion
		e->skill[18] = SAM_COMPANION_PUNCH_TICKS;       // (re)start a forward thrust
		SAMMpEntities::companionPunched(e);             // ENTS 18 from the tick: S.A.M clients lunge too
		return true;
#else
		(void)uid; return false;
#endif
	}

	double getFacing(int player)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity ) { return -1.0; }
		return (double)players[player]->entity->yaw; // radians [0,2PI); engine keeps it wrapped
#else
		(void)player; return -1.0;
#endif
	}

	// ---- v1.6.0 impact frame: screen flash / camera shake / hitstop ------------
	// See the header for the design contract. All three are inert until a script triggers
	// them, and are cleared by resetImpact() on a new game.

	void triggerScreenFlash(int player, int r, int g, int b, double intensity, int durationMs, int style, int lines)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS ) { return; }
		if ( durationMs <= 0 ) { g_samFlash[player].maxAlpha = 0; return; } // 0/neg = clear
		double inten = intensity;
		if ( !(inten > 0.0) ) { g_samFlash[player].maxAlpha = 0; return; }  // <=0 or NaN = clear
		if ( inten > 1.0 ) { inten = 1.0; }
		auto clamp255 = [](int v) -> Uint8 { return (Uint8)(v < 0 ? 0 : (v > 255 ? 255 : v)); };
		int dur = durationMs;
		if ( dur > 5000 ) { dur = 5000; } // a flash is a flash, not a fade-to-white
		int ln = lines;
		if ( ln < 0 ) { ln = 0; }
		if ( ln > 400 ) { ln = 400; }     // sane ceiling on speed-line count
		SamFlashState& f = g_samFlash[player];
		f.startMs  = SDL_GetTicks();
		f.durMs    = (Uint32)dur;
		f.r = clamp255(r); f.g = clamp255(g); f.b = clamp255(b);
		f.maxAlpha = (Uint8)((int)(inten * 255.0 + 0.5));
		f.style    = (Uint8)(style == 1 ? 1 : 0);
		f.lines    = (Uint16)ln;
#else
		(void)player; (void)r; (void)g; (void)b; (void)intensity; (void)durationMs; (void)style; (void)lines;
#endif
	}

	bool screenFlashState(int player, int& r, int& g, int& b, int& alpha, int& style, int& lines, double& progress)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS ) { return false; }
		SamFlashState& f = g_samFlash[player];
		if ( f.maxAlpha == 0 || f.durMs == 0 ) { return false; }
		const Uint32 elapsed = SDL_GetTicks() - f.startMs; // unsigned; safe across wrap
		if ( elapsed >= f.durMs ) { f.maxAlpha = 0; return false; } // expired → mark idle
		const double t = (double)elapsed / (double)f.durMs;         // 0 → 1 through the fade
		int ai = (int)((double)f.maxAlpha * (1.0 - t) + 0.5);       // linear alpha fade
		if ( ai <= 0 ) { return false; }
		if ( ai > 255 ) { ai = 255; }
		r = f.r; g = f.g; b = f.b; alpha = ai;
		style = (int)f.style; lines = (int)f.lines; progress = t;
		return true;
#else
		(void)player; (void)r; (void)g; (void)b; (void)alpha; (void)style; (void)lines; (void)progress; return false;
#endif
	}

	void triggerCameraShake(int player, double magnitude)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return; }
		double m = magnitude;
		if ( !(m > 0.0) ) { return; }       // <=0 or NaN → nothing
		if ( m > 40.0 ) { m = 40.0; }       // gameLogic clamps the accumulator anyway
		if ( players[player]->isLocalPlayer() )
		{
			// Same channels vanilla's damage/explosion shakes write to (entity.cpp ~2426).
			// x is real_t (~.03-.3 typical), y is int (~3-30 typical); gameLogic decays both.
			cameravars[player].shakex += m * 0.02;
			cameravars[player].shakey += (int)(m * 2.0);
		}
		else if ( player > 0 && multiplayer == SERVER && !client_disconnected[player] )
		{
			// Owned by a remote client — forward the shake over vanilla's 'SHAK' packet.
			// The receiver (net.cpp) does shakex += byte/100.f and shakey += byte, so send
			// byte = m*2.0 to match the LOCAL mapping above (shakex += m*0.02, shakey += m*2).
			int mag = (int)(m * 2.0);
			if ( mag > 255 ) { mag = 255; }
			strcpy((char*)net_packet->data, "SHAK");
			net_packet->data[4] = (Uint8)mag;
			net_packet->data[5] = (Uint8)mag;
			net_packet->address.host = net_clients[player - 1].host;
			net_packet->address.port = net_clients[player - 1].port;
			net_packet->len = 6;
			sendPacketSafe(net_sock, -1, net_packet, player - 1);
		}
#else
		(void)player; (void)magnitude;
#endif
	}

	void triggerHitstop(int durationMs)
	{
#ifdef SAM_LUA_HAVE_BARONY
		// Singleplayer only: the freeze gates the host's entity logic, which in a netgame
		// would stall enemy motion for the host while clients keep simulating → desync.
		if ( multiplayer != SINGLE ) { return; }
		if ( durationMs <= 0 ) { return; }
		int ms = durationMs;
		if ( ms > 400 ) { ms = 400; } // a beat, not a stall
		const Uint32 until = SDL_GetTicks() + (Uint32)ms;
		if ( until > g_samHitstopUntilMs ) { g_samHitstopUntilMs = until; } // extend, never shorten
#else
		(void)durationMs;
#endif
	}

	bool hitstopActive()
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( g_samHitstopUntilMs == 0 ) { return false; }
		if ( SDL_GetTicks() >= g_samHitstopUntilMs ) { g_samHitstopUntilMs = 0; return false; }
		return true;
#else
		return false;
#endif
	}

	void resetImpact()
	{
#ifdef SAM_LUA_HAVE_BARONY
		for ( int i = 0; i < MAXPLAYERS; ++i ) { g_samFlash[i] = SamFlashState{}; }
		g_samHitstopUntilMs = 0;
#endif
	}

	// Push a player's attributes to the client that owns them.
	//
	// This is a sixth hand-inlined copy of vanilla's ATTR packet, by necessity: the engine
	// never factored it out — entity.cpp repeats the same 21 bytes at 2842/4817/7984/8174/
	// 18875 — so there is nothing to call. Kept field-for-field identical to those five.
	// The receiver (net.cpp ~5195) is stock and must not be bent to suit us: S.A.M clients
	// have to keep understanding stock hosts, and vice versa.
	//
	// Not used for HP/MP: Entity::setHP/setMP already emit UPHP/UPMP themselves.
	void flushStatToClient(int player)
	{
#ifdef SAM_LUA_HAVE_BARONY
		// Guarded exactly as vanilla guards its five sites. player 0 is not merely
		// pointless here — net_clients[player - 1] would read out of bounds.
		if ( multiplayer != SERVER ) { return; }
		if ( player <= 0 || player >= MAXPLAYERS ) { return; }
		if ( !players[player] || players[player]->isLocalPlayer() ) { return; }
		if ( client_disconnected[player] || !stats[player] ) { return; }
		Stat* s = stats[player];

		strcpy((char*)net_packet->data, "ATTR");
		net_packet->data[4] = clientnum;
		net_packet->data[5] = (Sint8)s->STR;
		net_packet->data[6] = (Sint8)s->DEX;
		net_packet->data[7] = (Sint8)s->CON;
		net_packet->data[8] = (Sint8)s->INT;
		net_packet->data[9] = (Sint8)s->PER;
		net_packet->data[10] = (Sint8)s->CHR;
		net_packet->data[11] = (Uint8)s->EXP;
		net_packet->data[12] = (Uint8)s->LVL;
		SDLNet_Write16((Sint16)s->HP, &net_packet->data[13]);
		SDLNet_Write16((Sint16)s->MAXHP, &net_packet->data[15]);
		SDLNet_Write16((Sint16)s->MP, &net_packet->data[17]);
		SDLNet_Write16((Sint16)s->MAXMP, &net_packet->data[19]);
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		net_packet->len = 21;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
#else
		(void)player;
#endif
	}

	// Gold rides its own packet (actgold.cpp ~141) — ATTR has no field for it, so without
	// this a scripted gold change stays host-side forever no matter how many ATTRs fire.
	void flushGoldToClient(int player)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer != SERVER ) { return; }
		if ( player <= 0 || player >= MAXPLAYERS ) { return; }
		if ( !players[player] || players[player]->isLocalPlayer() ) { return; }
		if ( client_disconnected[player] || !stats[player] ) { return; }

		strcpy((char*)net_packet->data, "GOLD");
		SDLNet_Write32((Uint32)stats[player]->GOLD, &net_packet->data[4]);
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		net_packet->len = 8;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
#else
		(void)player;
#endif
	}

	bool getGlobalInt(const std::string& name, long long& out)
	{
		if ( !L ) { return false; }
		lua_getglobal(L, name.c_str());
		bool ok = false;
		if ( lua_isinteger(L, -1) )      { out = (long long)lua_tointeger(L, -1); ok = true; }
		else if ( lua_isnumber(L, -1) )  { out = (long long)lua_tonumber(L, -1);  ok = true; }
		lua_pop(L, 1);
		return ok;
	}

	bool getGlobalString(const std::string& name, std::string& out)
	{
		if ( !L ) { return false; }
		lua_getglobal(L, name.c_str());
		bool ok = false;
		if ( lua_type(L, -1) == LUA_TSTRING )
		{
			out = lua_tostring(L, -1);
			ok = true;
		}
		lua_pop(L, 1);
		return ok;
	}

} // namespace SAMLua
