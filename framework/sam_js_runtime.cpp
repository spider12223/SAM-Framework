/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_js_runtime.cpp
	Desc: sandboxed JavaScript + TypeScript runtime on quickjs-ng.

	Sibling of sam_lua_runtime — same sandbox guarantees, same on_event / sam_*
	surface. Lua -> QuickJS mechanism mapping:
	  * memory cap    JS_SetMemoryLimit(rt, 10MB)         (out-of-memory exception)
	  * native stack  JS_SetMaxStackSize(rt, 512KB)       (clean RangeError, no segfault)
	  * watchdog      JS_SetInterruptHandler + a wall-clock deadline; a bare
	                  `while(true){}` is aborted with an UNCATCHABLE exception.
	  * minimal env   JS_NewContextRaw + only pure intrinsics; quickjs-libc is
	                  never linked, so there is NO fs/net/os/print in scripts.
	  * host API      JS_NewCFunction + JS_SetPropertyStr (sam_log/sam_grant_item)
	  * primitives    JS_ToInt32/JS_ToCString in, JS_NewInt64/JS_NewString out —
	                  never an Entity or Item pointer across the boundary.
	  * isolation     each script runs in its OWN JSContext on a shared runtime.

	TypeScript: a .ts is transpiled ONCE at load by typescript.js running in a
	separate, privileged QuickJS context (relaxed budget), cached by content hash,
	and the emitted .js runs in the same hardened sandbox as a .js script.

-------------------------------------------------------------------------------*/

#ifndef NOMINMAX
#define NOMINMAX // keep windows.h min/max macros away from Barony/quickjs headers
#endif

#include "sam_js_runtime.hpp"
#include "sam_lua_runtime.hpp" // Part 2: sam_fire_hook cross-dispatches to Lua scripts too
#include "sam_test.hpp"   // sam_test_done: end an unattended -samtest run
#include "sam_speed.hpp"  // sam_set_game_speed / sam_get_game_speed: the simulation speed
#include "sam_loot.hpp"   // P_ITEMS: the loot pool, its tables, the containers and the shops
#include "sam_settings.hpp"  // sam_register_action / sam_register_setting / sam_get_setting / sam_set_setting / sam_list_settings
#include "sam_logger.hpp"
#include "sam_errors.hpp"   // writeFileAtomic

extern "C" {
#include "quickjs.h"
}

#include <string>
#include <type_traits>   // the ItemType assertion on the appearance guard
#include <cctype>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm> // std::sort: deterministic key order in sam_random_weighted
#include <cmath>       // v1.5.0: std::atan2 for aimed spell casts
#include <set>       // one error line per `all` call whose arguments cannot cross the wire
#include <filesystem>

// Barony bindings for sam_grant_item are enabled only inside the engine build
// (mirrors sam_lua_runtime). Standalone (this PoC) uses a logging stub.
#if defined(__has_include) && __has_include("items.hpp")
#	define SAM_JS_HAVE_BARONY 1
#	include "main.hpp"
#	include "game.hpp"
#	include "items.hpp"
#	include "player.hpp"
#	include "net.hpp"
#	include "mod_tools.hpp"
#	include "stat.hpp"      // Stat members, EFF_* effect ids, stats[], MAX_PLAYER_STAT_VALUE
#	include "entity.hpp"    // Entity::setEffect/setHP/setMP/getUID, act* behaviors, map iteration
#	include "monster.hpp"   // actMonster, Monster enum
#	include "collision.hpp" // entityDist
#	include "scores.hpp"    // completionTime (sam_get_run_time)
#	include "paths.hpp"     // GeneratePathTypes (monster movement bindings)
#	include "engine/audio/sound.hpp" // playSoundPlayer, numsounds
#	include "files.hpp"     // outputdir (savegames base dir for persistent mod data)
#	include "sam_items.hpp" // SAMItems::itemIdForIdString (custom item names in queries)
#	include "sam_classes.hpp" // v0.7.0 F5: SAMClasses::patchClass / addClassPassive
#	include "sam_monster_patches.hpp" // v0.7.0 F5: SAMMonsterPatch::set
#	include "sam_monsters.hpp" // SAMMonsters::traitBitForName (sam_monster_has_trait)
#	include "sam_combat.hpp" // species damage resistance + the on_damage_multiplier hook
#	include "sam_camera.hpp" // script control of where a player's camera is
#	include "sam_rules.hpp"  // stat modifiers, effect immunity, the XP curve
#	include "sam_music.hpp"  // mod music: sam_play_music and friends
#	include "sam_bodies.hpp"   // runtime model control (sam_set_model)
#	include "sam_spells.hpp"  // custom-spell registry (sam_grant_spell)
#	include "sam_sounds.hpp"  // custom sounds (resolve "ns:sound" ids in sam_play_sound)
#	include "sam_races.hpp"   // custom races (sam_get_race id lookup)
#	include "sam_hud.hpp"     // script-driven HUD layer
#	include "sam_images.hpp"  // the mod's own pictures (overlay + HUD art)
#	include "sam_ui.hpp"      // interactive mod panels
#	include "sam_catalog.hpp"  // reading the game content registries
#	include "sam_world.hpp"   // world queries, terrain, mechanisms
#	include "sam_world_state.hpp" // per-character mod state carried in the savegame
#	include "sam_workshop.hpp" // SAMModManifest (sam_get_mods)
#	include "sam_net.hpp"     // multiplayer contracts + the channel that carries a call to its player
#	include "sam_mp_inventory.hpp" // remote players' backpacks and spells, as the host mirrors them
#	include "sam_mp_entities.hpp" // multiplayer: script-made entities, pinned props, terrain, gibs
#	include "sam_mp_input.hpp" // keys, bound actions and client-raised events in multiplayer
#	include "magic/magic.hpp" // addSpell (grant a spell to a player)
#	include <cctype>
#endif

namespace
{
	// ---- runtime state --------------------------------------------------------
	JSRuntime* g_rt   = nullptr;   // sandbox runtime (mod scripts run here)
	JSRuntime* g_tsRt = nullptr;   // privileged transpile runtime (typescript.js)
	JSContext* g_tsCtx = nullptr;
	SAMJs::SandboxConfig g_cfg;

	struct Script
	{
		JSContext* ctx = nullptr;
		JSValue onEvent = JS_UNDEFINED; // owned ref, or JS_UNDEFINED
		JSValue onTick  = JS_UNDEFINED; // owned ref to on_tick (v0.7.0), or JS_UNDEFINED
		std::string path;
		std::string ns;                 // owning mod namespace (per-mod data / custom hooks / timers)
		bool enabled = false;
	};
	std::vector<Script> g_scripts;

	// Namespace of the script currently executing — set around every callback and
	// top-level eval so host APIs (sam_save_data, ...) can attribute a call to its mod.
	std::string g_currentNs;

	// ---- multiplayer: every sam_* function goes through one trampoline ----------------------
	//
	// The JS twin of samLuaRegister (sam_lua_runtime.cpp); the contract kinds are explained in
	// sam_net.hpp. Each function is registered as a "magic" C function whose magic number
	// indexes g_jsFns, so one trampoline serves all of them.
	struct SAMJsFn
	{
		std::string name;
		JSCFunction* fn = nullptr;
		int length = 0;
#ifdef SAM_JS_HAVE_BARONY
		const SAMNet::Contract* contract = nullptr;
#endif
	};
	std::vector<SAMJsFn> g_jsFns;
	std::map<std::string, int> g_jsFnIndex;

#ifdef SAM_JS_HAVE_BARONY
	bool samJsEncode(JSContext* ctx, JSValueConst v, SAMNet::Writer& w, int depth, std::string& err)
	{
		if ( JS_IsUndefined(v) || JS_IsNull(v) ) { w.u8(SAMNet::Tag::Nil); return true; }
		if ( JS_IsBool(v) ) { w.u8(JS_ToBool(ctx, v) ? SAMNet::Tag::True : SAMNet::Tag::False); return true; }
		if ( JS_IsNumber(v) )
		{
			double d = 0.0;
			JS_ToFloat64(ctx, &d, v);
			// Whole numbers cross as integers so a Lua runner on the far side reads them the
			// same way luaL_checkinteger would read a literal.
			if ( std::isfinite(d) && d == std::floor(d) && std::fabs(d) <= 9007199254740991.0 ) { w.u8(SAMNet::Tag::Int); w.i64((long long)d); }
			else { w.u8(SAMNet::Tag::Num); w.f64(d); }
			return true;
		}
		if ( JS_IsString(v) )
		{
			std::size_t n = 0;
			const char* s = JS_ToCStringLen(ctx, &n, v);
			if ( !s ) { err = "a string that could not be read"; return false; }
			if ( n > 65535 ) { JS_FreeCString(ctx, s); err = "a string over 64 KB"; return false; }
			w.u8(SAMNet::Tag::Str);
			w.str16(std::string(s, n));
			JS_FreeCString(ctx, s);
			return true;
		}
		if ( JS_IsFunction(ctx, v) ) { err = "a function"; return false; }
		if ( JS_IsArray(v) )
		{
			if ( depth >= SAMNet::MAX_DEPTH ) { err = "arrays nested more than 4 deep"; return false; }
			JSValue lenV = JS_GetPropertyStr(ctx, v, "length");
			std::uint32_t n = 0;
			JS_ToUint32(ctx, &n, lenV);
			JS_FreeValue(ctx, lenV);
			if ( n > 4096 ) { err = "an array of more than 4096 entries"; return false; }
			w.u8(SAMNet::Tag::Arr);
			w.u16((std::uint16_t)n);
			for ( std::uint32_t i = 0; i < n; ++i )
			{
				JSValue e = JS_GetPropertyUint32(ctx, v, i);
				const bool ok = samJsEncode(ctx, e, w, depth + 1, err);
				JS_FreeValue(ctx, e);
				if ( !ok ) { return false; }
			}
			return true;
		}
		if ( JS_IsObject(v) )
		{
			if ( depth >= SAMNet::MAX_DEPTH ) { err = "objects nested more than 4 deep"; return false; }
			JSPropertyEnum* props = nullptr;
			std::uint32_t n = 0;
			if ( JS_GetOwnPropertyNames(ctx, &props, &n, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0 ) { err = "an object that could not be read"; return false; }
			if ( n > 4096 ) { JS_FreePropertyEnum(ctx, props, n); err = "an object of more than 4096 entries"; return false; }
			w.u8(SAMNet::Tag::Map);
			w.u16((std::uint16_t)n);
			bool ok = true;
			for ( std::uint32_t i = 0; i < n && ok; ++i )
			{
				const char* k = JS_AtomToCString(ctx, props[i].atom);
				w.u8(SAMNet::Tag::Str);
				w.str16(k ? std::string(k) : std::string());
				if ( k ) { JS_FreeCString(ctx, k); }
				JSValue pv = JS_GetProperty(ctx, v, props[i].atom);
				ok = samJsEncode(ctx, pv, w, depth + 1, err);
				JS_FreeValue(ctx, pv);
			}
			JS_FreePropertyEnum(ctx, props, n);
			return ok;
		}
		err = "a value that cannot be sent";
		return false;
	}

	// One value read back off the wire. A nil comes back as undefined, never null: the host API
	// tests optional arguments with JS_IsUndefined, and a null there would read as 0.
	JSValue samJsDecode(JSContext* ctx, SAMNet::Reader& r, int depth, bool& ok)
	{
		const std::uint8_t t = r.u8();
		if ( !r.ok ) { ok = false; return JS_UNDEFINED; }
		switch ( t )
		{
			case SAMNet::Tag::Nil:   return JS_UNDEFINED;
			case SAMNet::Tag::False: return JS_FALSE;
			case SAMNet::Tag::True:  return JS_TRUE;
			case SAMNet::Tag::Int:   { const long long v = r.i64(); if ( !r.ok ) { ok = false; } return JS_NewInt64(ctx, (int64_t)v); }
			case SAMNet::Tag::Num:   { const double v = r.f64(); if ( !r.ok ) { ok = false; } return JS_NewFloat64(ctx, v); }
			case SAMNet::Tag::Str:
			{
				const std::string s = r.str16();
				if ( !r.ok ) { ok = false; return JS_UNDEFINED; }
				return JS_NewStringLen(ctx, s.data(), s.size());
			}
			case SAMNet::Tag::Arr:
			{
				if ( depth >= SAMNet::MAX_DEPTH ) { ok = false; return JS_UNDEFINED; }
				const int n = r.u16();
				JSValue a = JS_NewArray(ctx);
				for ( int i = 0; i < n && ok; ++i )
				{
					JSValue e = samJsDecode(ctx, r, depth + 1, ok);
					JS_SetPropertyUint32(ctx, a, (std::uint32_t)i, e);   // takes e
				}
				if ( !ok ) { JS_FreeValue(ctx, a); return JS_UNDEFINED; }
				return a;
			}
			case SAMNet::Tag::Map:
			{
				if ( depth >= SAMNet::MAX_DEPTH ) { ok = false; return JS_UNDEFINED; }
				const int n = r.u16();
				JSValue o = JS_NewObject(ctx);
				for ( int i = 0; i < n && ok; ++i )
				{
					const std::uint8_t kt = r.u8();
					std::string key;
					if ( kt == SAMNet::Tag::Str ) { key = r.str16(); }
					else if ( kt == SAMNet::Tag::Int ) { key = std::to_string(r.i64()); }
					else { ok = false; break; }
					if ( !r.ok ) { ok = false; break; }
					JSValue e = samJsDecode(ctx, r, depth + 1, ok);
					JS_SetPropertyStr(ctx, o, key.c_str(), e);   // takes e
				}
				if ( !ok ) { JS_FreeValue(ctx, o); return JS_UNDEFINED; }
				return o;
			}
			default:
				ok = false;
				return JS_UNDEFINED;
		}
	}

	// The Lua twin is samLuaPushRefusal. Nil answers `undefined`, not `null`, for the same
	// reason samJsDecode does (see the comment there): a JavaScript mod that writes
	// `if ( sam_get_ac(uid) < 10 )` gets `false` from undefined, the way `nil < 10` raises in
	// Lua, while `null < 10` is true -- so a refused read would take the branch for every
	// monster in the dungeon and say nothing. Nil and None are therefore the same value in
	// JavaScript; they differ only in Lua, where None returns no value at all.
	JSValue samJsRefusal(JSContext* ctx, SAMNet::Refusal r, bool forwarded)
	{
		switch ( r )
		{
			case SAMNet::Refusal::False: return JS_NewBool(ctx, forwarded);
			case SAMNet::Refusal::Nil:   return JS_UNDEFINED;
			case SAMNet::Refusal::Zero:  return JS_NewInt32(ctx, 0);
			case SAMNet::Refusal::Empty: return JS_NewArray(ctx);
			case SAMNet::Refusal::None:  return JS_UNDEFINED;
		}
		return JS_UNDEFINED;
	}

	// See samLuaEncodeArgs.
	bool samJsEncodeArgs(JSContext* ctx, int argc, JSValueConst* argv, int dropFrom, int replaceArg, long long replaceValue, std::string& out, std::string& err)
	{
		SAMNet::Writer w;
		int top = argc;
		if ( dropFrom > 0 && top >= dropFrom ) { top = dropFrom - 1; }
		if ( top > 255 ) { err = "more than 255 arguments"; return false; }
		w.u8((std::uint8_t)top);
		for ( int i = 1; i <= top; ++i )
		{
			if ( i == replaceArg ) { w.u8(SAMNet::Tag::Int); w.i64(replaceValue); continue; }
			std::string why;
			if ( !samJsEncode(ctx, argv[i - 1], w, 0, why) ) { err = "argument " + std::to_string(i) + " is " + why; return false; }
		}
		out = std::move(w.buf);
		return true;
	}

	// Defined with the other argument helpers further down; the trampoline needs it up here so it
	// reads a player argument exactly the way the body will. It accepts a numeric string, which
	// is what Lua's lua_isnumber does, so the two runtimes route the same call to the same seat.
	static bool samJsNum(JSContext* ctx, JSValueConst v, double* out);

	// See the Lua twin: a screen function that NAMES its player in argument `arg`, which is the
	// family a script may write with -1 for "every player". Each of them draws into one player's
	// own viewport, so -1 has to become a real seat before the body sees it.
	bool samScreenNamesPlayer(const SAMNet::Contract* c)
	{
		return c->kind == SAMNet::Kind::Screen && c->target == SAMNet::Target::Player && c->arg > 0;
	}

	// The seats sitting at THIS machine; see the Lua twin.
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

	// One error line per `all` function whose arguments could not be carried; see the Lua twin.
	std::set<std::string> g_allEncodeSaid;

	JSValue samJsTrampoline(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
	{
		if ( magic < 0 || magic >= (int)g_jsFns.size() ) { return JS_UNDEFINED; }
		JSCFunction* fn = g_jsFns[magic].fn;
		const SAMNet::Contract* c = g_jsFns[magic].contract;
		if ( !c ) { return fn(ctx, this_val, argc, argv); }

		// A trailing player the body never had: hidden from it, in singleplayer too.
		const bool opt = (c->target == SAMNet::Target::OptPlayer);
		const int bodyArgc = (opt && argc >= (int)c->arg) ? (int)c->arg - 1 : argc;

		// See the Lua twin for the whole reason: a player argument that is PRESENT but names no
		// seat is a mistake, not "no player given", and it is refused here rather than mapped to
		// -1 and read as "this machine". Before the singleplayer branch, so the same call is
		// refused in a solo game and in a co-op game.
		if ( c->target == SAMNet::Target::Player || c->target == SAMNet::Target::OptPlayer )
		{
			const int pi = (int)c->arg - 1;
			if ( c->arg > 0 && argc > pi && !JS_IsUndefined(argv[pi]) && !JS_IsNull(argv[pi]) )
			{
				const std::string& fname = g_jsFns[magic].name;
				const std::string seats = "0.." + std::to_string(MAXPLAYERS - 1) + ", or -1 for every player";
				double pn = 0.0;
				if ( !samJsNum(ctx, argv[pi], &pn) )
				{
					SAMNet::warnOnce(fname + "|argplayer", fname + ": argument " + std::to_string((int)c->arg)
						+ " names the player and has to be a number (" + seats + "). Nothing was done.");
					return samJsRefusal(ctx, c->refusal, false);
				}
				// samJsNum has already ruled out a NaN and anything past integer precision, so
				// the only thing left to reject is a fractional seat: 2.7 is not player 2.
				const bool integral = ( (double)(long long)pn == pn );
				const long long pv = integral ? (long long)pn : 0;
				if ( !integral || pv < -1 || pv >= MAXPLAYERS )
				{
					SAMNet::warnOnce(fname + "|argplayer", fname + ": there is no player "
						+ ( integral ? std::to_string(pv) : std::to_string(pn) )
						+ " (" + seats + "). Nothing was done.");
					return samJsRefusal(ctx, c->refusal, false);
				}
			}
		}

		if ( multiplayer == SINGLE || SAMNet::inForwardedCall() )
		{
			// "Every player" is every player on this machine when there is nobody else; see the
			// Lua twin for why a couch with more than one seat needs all of them, and why one
			// seat -- every ordinary singleplayer game -- behaves exactly as it always did.
			const int soloAi = (int)c->arg - 1;
			if ( samScreenNamesPlayer(c) && argc > soloAi )
			{
				// samJsNum, not JS_IsNumber: the Lua twin goes through lua_isnumber, which reads
				// the string "-1" as the number -1, and a seat that came out of a config file or
				// a text box arrives as a string. Without this the same mod expanded "every
				// player" in Lua and drew on one screen in JavaScript.
				double solo = 0.0;
				if ( samJsNum(ctx, argv[soloAi], &solo) && solo == -1.0 )
				{
					int seats[MAXPLAYERS];
					const int nSeats = samLocalSeats(seats);
					std::vector<JSValue> mine(argv, argv + argc);
					if ( nSeats == 1 )
					{
						mine[soloAi] = JS_NewInt32(ctx, seats[0]);
						return fn(ctx, this_val, bodyArgc, mine.data());
					}
					// The body runs once per seat and the script is told whether any of them
					// took it: every one of these functions answers with a single true/false.
					bool any = false;
					for ( int i = 0; i < nSeats; ++i )
					{
						mine[soloAi] = JS_NewInt32(ctx, seats[i]);
						JSValue r = fn(ctx, this_val, bodyArgc, mine.data());
						if ( JS_IsException(r) ) { return r; }
						if ( JS_ToBool(ctx, r) > 0 ) { any = true; }
						JS_FreeValue(ctx, r);
					}
					return JS_NewBool(ctx, any ? 1 : 0);
				}
			}
			return fn(ctx, this_val, bodyArgc, argv);
		}

		int player = -1;
		bool remoteItem = false;
		std::uint32_t ownerUid = 0;
		const int ai = (int)c->arg - 1;
		const bool hasArg = c->arg > 0 && argc > ai && !JS_IsUndefined(argv[ai]) && !JS_IsNull(argv[ai]);
		double num = 0.0;
		// samJsNum, not JS_IsNumber: it is the same reader every body uses for a number, so the
		// machine this call is routed to is the one the argument actually names. JS_IsNumber says
		// false for the string "2" where lua_isnumber says true, and that one difference sent the
		// same mod's call to player 2 in Lua and to the host's own screen in JavaScript.
		const bool isNum = hasArg && samJsNum(ctx, argv[ai], &num);
		switch ( c->target )
		{
			case SAMNet::Target::Player:
			case SAMNet::Target::OptPlayer:
				if ( isNum )
				{
					const long long v = (long long)num;
					player = (v == -1) ? SAMNet::EVERYONE : ((v >= 0 && v < MAXPLAYERS) ? (int)v : -1);
				}
				else if ( !hasArg && (c->kind == SAMNet::Kind::Screen || c->kind == SAMNet::Kind::Read) )
				{
					player = SAMNet::defaultScreenPlayer();
				}
				break;
			case SAMNet::Target::Uid:
				if ( isNum ) { player = SAMNet::playerOfUid((std::uint32_t)(long long)num); }
				break;
			case SAMNet::Target::Item:
				if ( isNum )
				{
					int p = -1;
					std::uint32_t ou = 0;
					if ( SAMNet::routeItem((std::uint32_t)(long long)num, p, ou) ) { remoteItem = true; player = p; ownerUid = ou; }
				}
				break;
			default:
				break;
		}

		const SAMNet::Decision d = SAMNet::decide(*c, player, remoteItem);
		const std::string name = g_jsFns[magic].name;
		switch ( d.route )
		{
			case SAMNet::Route::Here:
				// Only a client reaches Here with "every player" still in the arguments, and
				// there it means this machine's own screen: expanding -1 to the other machines
				// is the host's job. See the Lua twin.
				if ( samScreenNamesPlayer(c) && player == SAMNet::EVERYONE )
				{
					std::vector<JSValue> mine(argv, argv + argc);
					mine[ai] = JS_NewInt32(ctx, clientnum);
					return fn(ctx, this_val, bodyArgc, mine.data());
				}
				return fn(ctx, this_val, bodyArgc, argv);

			case SAMNet::Route::Refuse:
				SAMNet::warnOnce(name + "|" + d.why, name + ": " + d.why);
				return samJsRefusal(ctx, c->refusal, false);

			case SAMNet::Route::Forward:
			{
				std::string args, err;
				if ( !samJsEncodeArgs(ctx, argc, argv, opt ? (int)c->arg : 0, remoteItem ? (int)c->arg : 0, (long long)ownerUid, args, err) )
				{
					SAMNet::warnOnce(name + "|encode", name + ": cannot be sent to player " + std::to_string(d.player) + "'s machine: " + err + ".");
					return samJsRefusal(ctx, c->refusal, false);
				}
				// What this answers is what the reference promises: true once the call really is
				// on its way. See the Lua twin.
				const bool sent = SAMNet::forwardCall(d.player, 'J', g_currentNs, name, args);
				return samJsRefusal(ctx, c->refusal, sent);
			}

			case SAMNet::Route::ForwardAndHere:
			{
				if ( c->target == SAMNet::Target::Player && player == SAMNet::EVERYONE )
				{
					for ( int p = 1; p < MAXPLAYERS; ++p )
					{
						if ( !SAMNet::isRemotePlayer(p) ) { continue; }
						if ( !SAMNet::peerMayHaveSam(p) )
						{
							// Naming that player gets a warning; "every player" used to skip them
							// in silence. Keyed on the player, and on the same key
							// forwardCallToAll uses, so it stays one line per player.
							SAMNet::warnOnce("net:stock:" + std::to_string(p), "Player " + std::to_string(p)
								+ "'s game is not running S.A.M scripts, so it sees none of what the mod shows or changes for everyone.");
							continue;
						}
						std::string args, err;
						if ( samJsEncodeArgs(ctx, argc, argv, 0, (int)c->arg, p, args, err) ) { SAMNet::forwardCall(p, 'J', g_currentNs, name, args); }
					}
					// This machine runs it for its own player: the same arguments with the
					// player swapped for ours.
					std::vector<JSValue> mine(argv, argv + argc);
					mine[ai] = JS_NewInt32(ctx, clientnum);
					return fn(ctx, this_val, argc, mine.data());
				}
				std::string args, err;
				if ( samJsEncodeArgs(ctx, argc, argv, opt ? (int)c->arg : 0, 0, 0, args, err) )
				{
					SAMNet::forwardCallToAll('J', g_currentNs, name, args, c->kind == SAMNet::Kind::All);
				}
				else if ( c->kind == SAMNet::Kind::All )
				{
					// Refused on this machine too, so the host's copy of the table cannot end up
					// different from everybody else's. See the Lua twin for the full reasoning.
					if ( g_allEncodeSaid.insert(name).second )
					{
						SAM_ERROR("JS", name + ": " + err + ", so it cannot be sent to the other players -- and it was"
							" NOT applied on this machine either, because a table every machine keeps its own copy of has"
							" to change on all of them or on none. Keep functions and anything else that cannot cross the"
							" network out of the object you pass.");
					}
					return samJsRefusal(ctx, c->refusal, false);
				}
				else
				{
					SAMNet::warnOnce(name + "|encode", name + ": cannot be sent to the other players: " + err + ".");
				}
				return fn(ctx, this_val, bodyArgc, argv);
			}
		}
		return fn(ctx, this_val, bodyArgc, argv);
	}

	// Runs on a client: a call the host carried here. Arguments are padded with undefined up to
	// the function's declared length, as QuickJS does for a script's own call, because the
	// host API reads argv[i] below its length without checking argc first.
	void samJsRunForwarded(const std::string& ns, const std::string& name, const std::string& args)
	{
		auto it = g_jsFnIndex.find(name);
		if ( it == g_jsFnIndex.end() ) { return; }
		const SAMJsFn f = g_jsFns[it->second];
		JSContext* ctx = nullptr;
		for ( auto& sc : g_scripts ) { if ( sc.ctx && sc.ns == ns ) { ctx = sc.ctx; break; } }
		if ( !ctx ) { for ( auto& sc : g_scripts ) { if ( sc.ctx ) { ctx = sc.ctx; break; } } }
		if ( !ctx ) { return; }
		SAMNet::Reader r(args);
		const int n = r.u8();
		if ( !r.ok ) { return; }
		std::vector<JSValue> argv((std::size_t)std::max(n, f.length), JS_UNDEFINED);
		bool ok = true;
		for ( int i = 0; i < n && ok; ++i ) { argv[i] = samJsDecode(ctx, r, 0, ok); }
		if ( !ok )
		{
			for ( auto& v : argv ) { JS_FreeValue(ctx, v); }
			SAMNet::warnOnce("net:decode:" + name, "The host sent '" + name + "' with arguments this machine could not read; ignored.");
			return;
		}
		const std::string savedNs = g_currentNs;
		g_currentNs = ns;
		JSValue res = f.fn(ctx, JS_UNDEFINED, n, argv.data());
		if ( JS_IsException(res) )
		{
			JSValue ex = JS_GetException(ctx);
			const char* m = JS_ToCString(ctx, ex);
			SAM_WARN("NET", name + " (sent by the host) failed on this machine: " + std::string(m ? m : "?"));
			if ( m ) { JS_FreeCString(ctx, m); }
			JS_FreeValue(ctx, ex);
		}
		JS_FreeValue(ctx, res);
		g_currentNs = savedNs;
		for ( auto& v : argv ) { JS_FreeValue(ctx, v); }
	}
#endif

	// Register one sam_* function on a script's global object.
	void samJsRegister(JSContext* ctx, JSValueConst g, const char* name, JSCFunction* fn, int length)
	{
#ifdef SAM_JS_HAVE_BARONY
		int idx = 0;
		auto it = g_jsFnIndex.find(name);
		if ( it == g_jsFnIndex.end() )
		{
			idx = (int)g_jsFns.size();
			SAMJsFn f;
			f.name = name;
			f.fn = fn;
			f.length = length;
			f.contract = SAMNet::contractFor(name);
			if ( !f.contract )
			{
				SAM_WARN("NET", std::string(name) + " has no multiplayer contract (sam_mp_contracts.inc); it runs unchecked on every machine.");
			}
			g_jsFns.push_back(std::move(f));
			g_jsFnIndex[name] = idx;
		}
		else
		{
			idx = it->second;
			g_jsFns[idx].fn = fn;
			g_jsFns[idx].length = length;
		}
		JS_SetPropertyStr(ctx, g, name, JS_NewCFunctionMagic(ctx, samJsTrampoline, name, length, JS_CFUNC_generic_magic, idx));
#else
		JS_SetPropertyStr(ctx, g, name, JS_NewCFunction(ctx, fn, name, length));
#endif
	}

	// Part 4 timers — per-script, keyed by (ns,id). Callback + its context are owned.
	struct JsTimer
	{
		std::string id;
		std::string ns;
		JSContext* ctx = nullptr;
		JSValue callback = JS_UNDEFINED; // owned ref
		long long remaining = 0;
		long long interval  = 0;
		bool repeating = false;
	};
	std::vector<JsTimer> g_jsTimers;

	// Part 2 custom hooks — registered names + a recursion guard.
	std::vector<std::string> g_customHooks;
	int g_fireDepth = 0;

	// ---- wall-clock watchdog --------------------------------------------------
	long long g_deadlineMs = 0; // 0 = disabled

	long long nowMs()
	{
		return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	int g_deadlineDepth = 0; // reentrancy depth (nesting-aware watchdog)
	void setDeadline(long long budgetMs) { if ( g_deadlineDepth++ == 0 ) { g_deadlineMs = nowMs() + budgetMs; } }
	void clearDeadline() { if ( --g_deadlineDepth <= 0 ) { g_deadlineDepth = 0; g_deadlineMs = 0; } }

	int js_interrupt(JSRuntime* /*rt*/, void* /*opaque*/)
	{
		return (g_deadlineMs != 0 && nowMs() > g_deadlineMs) ? 1 : 0; // nonzero = abort
	}

	// ---- small fs helpers -----------------------------------------------------
	bool readFile(const std::string& path, std::string& out)
	{
		std::ifstream f(path, std::ios::binary);
		if ( !f ) { return false; }
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}
	bool writeFileAtomic(const std::string& path, const std::string& data)
	{
		// One implementation for every file the framework writes and reads back.
		return SAMErrors::writeFileAtomic(path, data);
	}
	std::string hashKey(const std::string& src)
	{
		// FNV-1a 64 over the source + a salt that encodes transpiler id/opts so a
		// compiler/option change auto-invalidates every cache entry.
		uint64_t h = 1469598103934665603ULL;
		const std::string salt = "|ts5.8.3|target=ES2020|isolatedModules|fmt1";
		auto mix = [&](const std::string& s) {
			for ( unsigned char c : s ) { h ^= c; h *= 1099511628211ULL; }
		};
		mix(src);
		mix(salt);
		// 16-hex-digit key. Built by hand (not snprintf) because Barony's headers
		// macro-remap snprintf -> _snprintf, which breaks std::snprintf.
		static const char* const HEX = "0123456789abcdef";
		std::string out(16, '0');
		for ( int i = 15; i >= 0; --i ) { out[(std::size_t)i] = HEX[h & 0xFULL]; h >>= 4; }
		return out;
	}

	// ---- JS value helpers -----------------------------------------------------
	std::string exceptionToString(JSContext* ctx)
	{
		JSValue exc = JS_GetException(ctx);
		std::string out = "(unknown JS error)";
		const char* s = JS_ToCString(ctx, exc);
		if ( s ) { out = s; JS_FreeCString(ctx, s); }
		if ( JS_IsObject(exc) )
		{
			JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
			const char* st = JS_ToCString(ctx, stack);
			if ( st )
			{
				std::string sstr(st);
				if ( !sstr.empty() && sstr != "undefined" ) { out += " | "; out += sstr; }
				JS_FreeCString(ctx, st);
			}
			JS_FreeValue(ctx, stack);
		}
		JS_FreeValue(ctx, exc);
		return out;
	}

	JSValue makeEventObject(JSContext* ctx, const SAMJs::Event& ev)
	{
		JSValue obj = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, ev.name.c_str()));
		// Seed from the SHARED write-back store, exactly as the Lua side does. Building this
		// from the raw engine event instead was a real bug: a Lua mod would set damage to 5,
		// then any JS script at all -- including one whose on_event is empty -- would be
		// handed the original 10 and record it straight back over the 5. In practice that
		// meant a single JS mod anywhere in the load order silently reverted every Lua mod's
		// change, and two JS mods could never chain either.
		for ( const auto& kv : ev.ints )
		{
			const double v = SAMLua::lastEventNumber(kv.first.c_str(), (double)kv.second);
			JS_SetPropertyStr(ctx, obj, kv.first.c_str(), JS_NewInt64(ctx, (int64_t)v));
		}
		for ( const auto& kv : ev.strings )
		{
			const std::string v = SAMLua::lastEventString(kv.first.c_str(), kv.second);
			JS_SetPropertyStr(ctx, obj, kv.first.c_str(), JS_NewStringLen(ctx, v.data(), v.size()));   // with its length
		}
		return obj;
	}
	// An optional argument is "given" only if it is present AND not undefined/null.
	//
	// argc alone is not enough. QuickJS converts undefined to 0 and reports SUCCESS
	// (JS_ToInt32Free, JS_TAG_UNDEFINED -> 0), and JS_ToBool(undefined) is false -- so a
	// caller writing the perfectly ordinary `sam_show_image(p, img, ms, opts.alpha)` with no
	// alpha in opts passes argc=4 with argv[3]=undefined, silently overriding a default of
	// 255 with 0. The Lua side gets this right for free because lua_isnoneornil covers both.
	static bool samHasArg(int argc, JSValueConst* argv, int i)
	{
		return ( argc > i && !JS_IsUndefined(argv[i]) && !JS_IsNull(argv[i]) );
	}

	// What counts as a number, since QuickJS itself will not say.
	//
	// JS_ToInt32 and JS_ToInt64 apply JavaScript's coercion rules and report SUCCESS for
	// every one of them: "sword" and {} become NaN and then 0, true becomes 1, [] becomes 0.
	// Nothing downstream can tell that apart from a deliberate 0, and 0 is the destructive
	// value in more than one place -- sam_set_item_count(uid, "two") deleted the item, and
	// sam_set_item_beatitude(uid, obj) stripped a curse. Lua's luaL_checkinteger raises on
	// all of these, so the same typo behaved differently in the two runtimes.
	//
	// A numeric STRING is accepted, because Lua's own coercion accepts it and mods do read
	// numbers out of text. Everything that is not a finite number is refused.
	static bool samJsNum(JSContext* ctx, JSValueConst v, double* out)
	{
		if ( JS_IsBool(v) ) { return false; }   // luaL_checkinteger raises on a boolean too
		double d = 0.0;
		if ( JS_ToFloat64(ctx, &d, v) < 0 )
		{
			// "sword" does not throw, but an object with a throwing valueOf does, and a
			// pending exception left on the context would surface at the next unrelated call.
			JS_FreeValue(ctx, JS_GetException(ctx));
			return false;
		}
		if ( d != d ) { return false; }                        // NaN: "sword", {}, [1,2]
		if ( d > 9.0e15 || d < -9.0e15 ) { return false; }     // infinity, and past integer precision
		*out = d;
		return true;
	}

	// Optional numeric argument: leaves the caller's default in place when the argument is
	// absent, and warns rather than substituting a wrong number when it is unreadable.
	static void samJsOptI32(JSContext* ctx, int argc, JSValueConst* argv, int i, int32_t* io, const char* who)
	{
		if ( !samHasArg(argc, argv, i) ) { return; }
		double d = 0.0;
		if ( !samJsNum(ctx, argv[i], &d) )
		{
			SAM_WARN("JS", std::string(who) + ": argument " + std::to_string(i + 1)
				+ " is not a number; ignoring it.");
			return;
		}
		*io = (int32_t)d;
	}

	static void samJsOptI64(JSContext* ctx, int argc, JSValueConst* argv, int i, int64_t* io, const char* who)
	{
		if ( !samHasArg(argc, argv, i) ) { return; }
		double d = 0.0;
		if ( !samJsNum(ctx, argv[i], &d) )
		{
			SAM_WARN("JS", std::string(who) + ": argument " + std::to_string(i + 1)
				+ " is not a number; ignoring it.");
			return;
		}
		*io = (int64_t)d;
	}

	// Required numeric argument: refuses the whole call rather than guessing.
	static bool samJsReqI32(JSContext* ctx, int argc, JSValueConst* argv, int i, int32_t* out, const char* who)
	{
		if ( !samHasArg(argc, argv, i) )
		{
			SAM_ERROR("JS", std::string(who) + ": argument " + std::to_string(i + 1) + " is required.");
			return false;
		}
		double d = 0.0;
		if ( !samJsNum(ctx, argv[i], &d) )
		{
			SAM_ERROR("JS", std::string(who) + ": argument " + std::to_string(i + 1) + " must be a number.");
			return false;
		}
		*out = (int32_t)d;
		return true;
	}


	// The same two, for REAL arguments. Batch 3 is the first batch whose arguments are mostly
	// reals -- a force, an angle, an elevation -- and the helper set above stops at integers, so
	// without these every float argument would go back to a bare JS_ToFloat64 and back to
	// silently accepting "hard" as 0.0.
	static void samJsOptF64(JSContext* ctx, int argc, JSValueConst* argv, int i, double* io, const char* who)
	{
		if ( !samHasArg(argc, argv, i) ) { return; }
		double d = 0.0;
		if ( !samJsNum(ctx, argv[i], &d) )
		{
			SAM_WARN("JS", std::string(who) + ": argument " + std::to_string(i + 1)
				+ " is not a number; ignoring it.");
			return;
		}
		*io = d;
	}

	static bool samJsReqF64(JSContext* ctx, int argc, JSValueConst* argv, int i, double* out, const char* who)
	{
		if ( !samHasArg(argc, argv, i) )
		{
			SAM_ERROR("JS", std::string(who) + ": argument " + std::to_string(i + 1) + " is required.");
			return false;
		}
		double d = 0.0;
		if ( !samJsNum(ctx, argv[i], &d) )
		{
			SAM_ERROR("JS", std::string(who) + ": argument " + std::to_string(i + 1) + " must be a number.");
			return false;
		}
		*out = d;
		return true;
	}

	// A REQUIRED uid. samJsReqI32 truncates to 32 bits, and a uid is wider than that.
	static bool samJsReqI32Wide(JSContext* ctx, int argc, JSValueConst* argv, int i, int64_t* out, const char* who)
	{
		double d = 0.0;
		if ( !samJsReqF64(ctx, argc, argv, i, &d, who) ) { return false; }
		*out = (int64_t)d;
		return true;
	}

	// A REQUIRED flag; see the Lua twin for why a missing one is refused rather than guessed.
	static bool samBoolReqJs(JSContext* ctx, int argc, JSValueConst* argv, int i, const char* who, bool* out)
	{
		if ( !samHasArg(argc, argv, i) )
		{
			SAM_ERROR("JS", std::string(who) + ": argument " + std::to_string(i + 1)
				+ " (true or false) is required.");
			return false;
		}
		*out = JS_ToBool(ctx, argv[i]) != 0;
		return true;
	}

	// A flag argument, with ONE rule across both runtimes.
	//
	// lua_toboolean says TRUE for the number 0; JS_ToBool says FALSE for it. So sam_set_visible(
	// uid, 0) already means opposite things in Lua and JS today, in shipped code. JavaScript's
	// rule is the one a modder expects from a 0, so it wins, and Lua is brought to it.
	static bool samBoolArgJs(JSContext* ctx, int argc, JSValueConst* argv, int i, bool dflt)
	{
		if ( !samHasArg(argc, argv, i) ) { return dflt; }
		return JS_ToBool(ctx, argv[i]) != 0;
	}

	// ---- host functions exposed to scripts ------------------------------------
	JSValue js_sam_log(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// A string or a number, as luaL_checkstring takes in the Lua twin. Nothing, or a value of
		// any other kind, is refused with an error there too; this used to log "[object Object]"
		// for a table-like value and nothing at all when called with no argument.
		if ( !samHasArg(argc, argv, 0) ) { SAM_ERROR("JS", "sam_log: argument 1 (the message) is required."); return JS_UNDEFINED; }
		if ( !JS_IsString(argv[0]) && !JS_IsNumber(argv[0]) )
		{
			SAM_ERROR("JS", "sam_log: argument 1 must be a string or a number.");
			return JS_UNDEFINED;
		}
		const char* s = JS_ToCString(ctx, argv[0]);
		if ( s ) { SAM_INFO("SCRIPT", s); JS_FreeCString(ctx, s); }
		return JS_UNDEFINED;
	}

	JSValue js_sam_grant_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_grant_item") ) { return JS_NewBool(ctx, 0); }
		std::string name;
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }

#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_grant_item refused: host only (multiplayer == CLIENT).");
			return JS_NewBool(ctx, 0);
		}
		if ( player < 0 || player >= MAXPLAYERS || !players[player] )
		{
			SAM_ERROR("JS", "sam_grant_item: invalid player index " + std::to_string(player) + ".");
			return JS_NewBool(ctx, 0);
		}
		// Resolve a custom "namespace:item" id first, else a vanilla name (case-insensitive).
		int resolvedType = -1;
		if ( name.find(':') != std::string::npos ) { resolvedType = SAMItems::itemIdForIdString(name); }
		// One resolver, shared with every other name-taking call: digits, "ns:id", the internal
		// name, then the DISPLAYED name -- which is what sam_list_items and sam_get_container_items
		// hand out, and what this used to refuse.
		if ( resolvedType < 0 ) { resolvedType = SAMCatalog::itemTypeFor(name); }
		if ( resolvedType < 0 )
		{
			SAM_ERROR("JS", "sam_grant_item: unknown item '" + name
				+ "' (expected a vanilla name like \"IRON_DAGGER\" or a custom \"namespace:item\") — nothing granted.");
			return JS_NewBool(ctx, 0);
		}
		const ItemType type = static_cast<ItemType>(resolvedType);

		// Optional trailing args: beatitude (blessed +N / cursed -N), status (0=BROKEN .. 4=
		// EXCELLENT), count. The 2-arg call (plain, uncursed, one item) is unchanged.
		int32_t beatitudeArg = 0, statusArg = (int)EXCELLENT, countArg = 1;
		samJsOptI32(ctx, argc, argv, 2, &beatitudeArg, __func__);
		samJsOptI32(ctx, argc, argv, 3, &statusArg, __func__);
		samJsOptI32(ctx, argc, argv, 4, &countArg, __func__);
		const Sint16 beatitude = (Sint16)beatitudeArg;
		if ( statusArg < (int)BROKEN ) { statusArg = (int)BROKEN; }
		if ( statusArg > (int)EXCELLENT ) { statusArg = (int)EXCELLENT; }
		const Status status = (Status)statusArg;
		const Sint16 count = (Sint16)(countArg < 1 ? 1 : countArg);
		Item* item = newItem(type, status, beatitude, count, 0, true, nullptr);
		if ( !item ) { return JS_NewBool(ctx, 0); }
		// See the Lua twin: the engine's own pickup, local or (vanilla ITEM packet) remote.
		if ( !SAMMpInventory::deliverItem(player, item, "sam_grant_item") ) { return JS_NewBool(ctx, 0); }
		SAM_INFO("JS", "Granted item " + name + " to player " + std::to_string(player));
		return JS_NewBool(ctx, 1);
#else
		// Standalone PoC: prove the primitive boundary works without Barony.
		SAM_INFO("SCRIPT", "sam_grant_item (stub): grant '" + name + "' to player " + std::to_string(player));
		return JS_NewBool(ctx, 1);
#endif
	}

#ifdef SAM_JS_HAVE_BARONY
	// ---- shared helpers for the host API (primitives only) --------------------
	inline int samClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

	inline std::string samUpper(const char* in)
	{
		std::string o = in ? in : "";
		for ( char& c : o ) { c = (char)std::toupper((unsigned char)c); }
		return o;
	}

	// Resolve an effect name via the shared SAMLua table — all of the engine's named
	// effects plus custom slots. This used to be a second hand-written copy of the same
	// if-chain, and both copies drifted to the same wrong place: 14 of the engine's 135
	// effects, so STUNNED/FEAR/ROOTED/etc. were unreachable from either language.
	// One table now, so Lua and JS cannot disagree about what an effect is called.
	int samEffectNameToId(const char* nameIn)
	{
		return SAMLua::effectIdFromName(nameIn);
	}

	JSValue js_sam_grant_gold(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1, amount = 0;
		// Both required, as luaL_checkinteger makes them in Lua. A forgotten amount used to keep
		// the default 0: it granted nothing, still sent the vanilla GOLD packet to that client,
		// and reported success -- a reward that silently pays nothing.
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_grant_gold") ) { return JS_NewBool(ctx, 0); }
		if ( !samJsReqI32(ctx, argc, argv, 1, &amount, "sam_grant_gold") ) { return JS_NewBool(ctx, 0); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_grant_gold refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("JS", "sam_grant_gold: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
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
		SAM_INFO("JS", "Granted " + std::to_string(amount) + " gold to player " + std::to_string(player));
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_apply_effect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1, ticks = 0;
		std::string name;
		// Player and ticks are required, as in Lua. A missing tick count used to become 0, and the
		// engine counts down only positive timers, so the effect was applied PERMANENTLY.
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_apply_effect") ) { return JS_NewBool(ctx, 0); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( !samJsReqI32(ctx, argc, argv, 2, &ticks, "sam_apply_effect") ) { return JS_NewBool(ctx, 0); }
		int32_t strength = 0;
		samJsOptI32(ctx, argc, argv, 3, &strength, __func__); // optional tier/magnitude
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_apply_effect refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("JS", "sam_apply_effect: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 ) { SAM_ERROR("JS", "sam_apply_effect: unknown effect '" + name + "'. Valid: " + SAMLua::effectNameHint()); return JS_NewBool(ctx, 0); }
		bool ok;
		if ( strength > 0 )
		{
			const Uint8 st = (Uint8)(strength > 255 ? 255 : strength);
			ok = players[player]->entity->setEffect(eff, st, ticks, true, true, true); // overrideEffectStrength
		}
		else
		{
			ok = players[player]->entity->setEffect(eff, true, ticks, true);
		}
		SAM_INFO("JS", "Applied effect " + name + " to player " + std::to_string(player) + (ok ? "" : " (refused/immune)"));
		return JS_NewBool(ctx, ok ? 1 : 0);
	}

	JSValue js_sam_remove_effect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		std::string name;
		// Required, as luaL_checkinteger makes it in Lua.
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_remove_effect") ) { return JS_NewBool(ctx, 0); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_remove_effect refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("JS", "sam_remove_effect: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 ) { SAM_ERROR("JS", "sam_remove_effect: unknown effect '" + name + "'. Valid: " + SAMLua::effectNameHint()); return JS_NewBool(ctx, 0); }
		players[player]->entity->setEffect(eff, false, 0, true);
		SAM_INFO("JS", "Removed effect " + name + " from player " + std::to_string(player));
		return JS_NewBool(ctx, 1);
	}

	// v1.5.0 — twins of the Lua player effect-control bindings.
	JSValue js_sam_clear_effects(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_clear_effects") ) { return JS_NewInt32(ctx, 0); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_clear_effects refused: host only."); return JS_NewInt32(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity || !stats[player] )
		{ SAM_ERROR("JS", "sam_clear_effects: invalid player index " + std::to_string(player) + "."); return JS_NewInt32(ctx, 0); }
		int cleared = 0;
		for ( int eff = 0; eff < NUMEFFECTS; ++eff )
		{
			if ( stats[player]->getEffectActive(eff) != 0 ) { players[player]->entity->setEffect(eff, false, 0, true); ++cleared; }
		}
		return JS_NewInt32(ctx, cleared);
	}

	JSValue js_sam_set_effect_duration(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1, ticks = 0; std::string name;
		// Required, as in Lua: a missing tick count made an active effect permanent.
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_set_effect_duration") ) { return JS_NewBool(ctx, 0); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( !samJsReqI32(ctx, argc, argv, 2, &ticks, "sam_set_effect_duration") ) { return JS_NewBool(ctx, 0); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_effect_duration refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity || !stats[player] )
		{ SAM_ERROR("JS", "sam_set_effect_duration: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 ) { SAM_ERROR("JS", "sam_set_effect_duration: unknown effect '" + name + "'. Valid: " + SAMLua::effectNameHint()); return JS_NewBool(ctx, 0); }
		if ( stats[player]->getEffectActive(eff) == 0 ) { return JS_NewBool(ctx, 0); }
		players[player]->entity->setEffect(eff, true, ticks, true, true, false, true);
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_set_effect_strength(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1, strength = 0; std::string name;
		// Player and strength are required, as luaL_checkinteger makes them in Lua: a missing
		// strength used to become 0, was raised to 1, and quietly reset the effect's tier.
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_set_effect_strength") ) { return JS_NewBool(ctx, 0); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( !samJsReqI32(ctx, argc, argv, 2, &strength, "sam_set_effect_strength") ) { return JS_NewBool(ctx, 0); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_effect_strength refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity || !stats[player] )
		{ SAM_ERROR("JS", "sam_set_effect_strength: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 ) { SAM_ERROR("JS", "sam_set_effect_strength: unknown effect '" + name + "'. Valid: " + SAMLua::effectNameHint()); return JS_NewBool(ctx, 0); }
		if ( stats[player]->getEffectActive(eff) == 0 ) { return JS_NewBool(ctx, 0); }
		const Uint8 st = (Uint8)(strength < 1 ? 1 : (strength > 255 ? 255 : strength));
		const int keepDur = stats[player]->EFFECTS_TIMERS[eff];
		players[player]->entity->setEffect(eff, st, keepDur, true, true, true, true);
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_get_stat(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// The player is required here, exactly as luaL_checkinteger requires it in the Lua twin.
		// It used to default to -1 and the body then answered a plausible number, so a mod that
		// read the player out of an event field a particular event does not carry got a real
		// looking answer and nothing in the log, while the byte-identical Lua mod stopped with
		// an error naming the line. The failure value is undefined rather than null or 0:
		// JavaScript compares null to 0 as equal, so null would still read as a real answer in
		// `if (hp <= 0)` -- the same trap the refusal values avoid.
		int32_t player = -1;
		std::string name;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_stat") ) { return JS_UNDEFINED; }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		// A CLIENT may read ITS OWN player, and nobody else's. Same rule and same reason as the
		// Lua twin: stats[clientnum] is kept current by the 'UPHP' and 'UPMP' handlers, so a
		// client-side HUD mod was being refused data its own machine was already holding, while
		// another player's slot really is not replicated and would read as zeroes.
		//
		// The trampoline enforces this first, so nothing reaches the branch below today; it
		// stays as a backstop and now answers what a refusal answers, which for this contract
		// is undefined. It used to answer 0 -- the one value this function's refusal was chosen
		// not to be. See the Lua twin.
		if ( multiplayer == CLIENT && player != clientnum )
		{
			SAM_WARN("JS", "sam_get_stat: on a client you can read your own player ("
				+ std::to_string(clientnum) + ") only. Another player's stats are not sent to"
				" your machine, so the number here would be invented rather than stale.");
			return JS_UNDEFINED;
		}
		// Same rule for the two argument errors below: undefined, not 0.
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("JS", "sam_get_stat: invalid player index " + std::to_string(player) + "."); return JS_UNDEFINED; }
		const std::string n = samUpper(name.c_str());
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
		else { SAM_ERROR("JS", "sam_get_stat: unknown stat '" + name + "'."); return JS_UNDEFINED; }
		return JS_NewInt64(ctx, v);
	}

	JSValue js_sam_set_stat(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1, value = 0;
		std::string name;
		// Required, as luaL_checkinteger makes them in Lua: a missing value used to become 0, so
		// sam_set_stat(p, "HP") killed the player in JS and raised in Lua.
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_set_stat") ) { return JS_NewBool(ctx, 0); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( !samJsReqI32(ctx, argc, argv, 2, &value, "sam_set_stat") ) { return JS_NewBool(ctx, 0); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_stat refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("JS", "sam_set_stat: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const std::string n = samUpper(name.c_str());
		Stat* s = stats[player];
		Entity* e = players[player]->entity;
		// Clamps and flush are kept in lockstep with the Lua sam_set_stat by using the same
		// shared SAMLua constants and helpers — the two runtimes having quietly different
		// bounds is a bug class this framework has already shipped once.
		enum { JS_SYNC_NONE, JS_SYNC_ATTR, JS_SYNC_GOLD, JS_SYNC_HUNGER } sync = JS_SYNC_NONE;
		// setHP/setMP self-emit UPHP/UPMP only when an entity exists; with none (dead player
		// awaiting respawn) the raw write needs the ATTR flush, same as the Lua path.
		if      ( n == "HP" )    { if ( e ) { e->setHP(value); } else { s->HP = samClampInt(value, 0, s->MAXHP); sync = JS_SYNC_ATTR; } }
		else if ( n == "MP" )    { if ( e ) { e->setMP(value); } else { s->MP = samClampInt(value, 0, s->MAXMP); sync = JS_SYNC_ATTR; } }
		else if ( n == "MAXHP" ) { s->MAXHP = samClampInt(value, 1, SAMLua::STAT_WIRE_MAX); if ( s->HP > s->MAXHP ) { s->HP = s->MAXHP; } sync = JS_SYNC_ATTR; }
		else if ( n == "MAXMP" ) { s->MAXMP = samClampInt(value, 0, SAMLua::STAT_WIRE_MAX); if ( s->MP > s->MAXMP ) { s->MP = s->MAXMP; } sync = JS_SYNC_ATTR; }
		else if ( n == "STR" )   { s->STR = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = JS_SYNC_ATTR; }
		else if ( n == "DEX" )   { s->DEX = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = JS_SYNC_ATTR; }
		else if ( n == "CON" )   { s->CON = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = JS_SYNC_ATTR; }
		else if ( n == "INT" )   { s->INT = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = JS_SYNC_ATTR; }
		else if ( n == "PER" )   { s->PER = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = JS_SYNC_ATTR; }
		else if ( n == "CHR" )   { s->CHR = samClampInt(value, SAMLua::ATTR_WIRE_MIN, MAX_PLAYER_STAT_VALUE); sync = JS_SYNC_ATTR; }
		else if ( n == "GOLD" )  { s->GOLD = (value < 0 ? 0 : value); sync = JS_SYNC_GOLD; }
		else if ( n == "HUNGER" ) { s->HUNGER = samClampInt(value, 0, 1500); sync = JS_SYNC_HUNGER; } // engine clamps 0..1500
		else if ( n == "LEVEL" || n == "LVL" ) { s->LVL = samClampInt(value, 1, 255); sync = JS_SYNC_ATTR; }
		// EXP up to 255 (its wire byte): 100+ triggers the engine's real level-up on the
		// host's next tick. The old 0..99 cap silently made leveling-by-EXP impossible.
		else if ( n == "EXP" )   { s->EXP = samClampInt(value, 0, 255); sync = JS_SYNC_ATTR; }
		else { SAM_ERROR("JS", "sam_set_stat: unknown stat '" + name + "'."); return JS_NewBool(ctx, 0); }
		if      ( sync == JS_SYNC_ATTR ) { SAMLua::flushStatToClient(player); }
		else if ( sync == JS_SYNC_GOLD ) { SAMLua::flushGoldToClient(player); }
		else if ( sync == JS_SYNC_HUNGER ) { serverUpdateHunger(player); } // engine's own 'HNGR' sender
		// ATTR reaches only the owner, and nothing at all for the host's own player. Every other
		// machine's party level comes from 'UPLV', which the engine sends only at a real level-up.
		if ( n == "LEVEL" || n == "LVL" ) { serverUpdatePlayerLVL(); }
		SAM_INFO("JS", "Set stat " + n + " = " + std::to_string(value) + " on player " + std::to_string(player));
		return JS_NewBool(ctx, 1);
	}

	// sam_set_move_speed(player, mult) — host-only; syncs to the owning client.
	JSValue js_sam_set_move_speed(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		double mult = 1.0;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_set_move_speed") ) { return JS_NewBool(ctx, 0); }
		if ( !samJsReqF64(ctx, argc, argv, 1, &mult, "sam_set_move_speed") ) { return JS_NewBool(ctx, 0); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_move_speed refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS )
		{ SAM_ERROR("JS", "sam_set_move_speed: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		SAMLua::setMoveSpeedMult(player, mult);
		return JS_NewBool(ctx, 1);
	}

	// sam_get_move_speed(player) -> number. Readable on clients too — a client's own
	// multiplier is exactly what its movement code is using.
	JSValue js_sam_get_move_speed(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua; see sam_get_stat. Without it a missing player fell through to
		// getMoveSpeedMult, which clamps an out-of-range index to 1.0 -- "normal speed" -- so a
		// speed feature silently never fired and there was nothing in the log to find.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_move_speed") ) { return JS_UNDEFINED; }
		return JS_NewFloat64(ctx, SAMLua::getMoveSpeedMult(player));
	}

	// sam_add_move_speed(player, delta) -> new multiplier. Additive counterpart to the
	// set-only sam_set_move_speed: stacks onto whatever the multiplier already is.
	JSValue js_sam_add_move_speed(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		double delta = 0.0;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_add_move_speed") ) { return JS_NewBool(ctx, 0); }
		if ( !samJsReqF64(ctx, argc, argv, 1, &delta, "sam_add_move_speed") ) { return JS_NewBool(ctx, 0); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_add_move_speed refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS )
		{ SAM_ERROR("JS", "sam_add_move_speed: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		SAMLua::setMoveSpeedMult(player, SAMLua::getMoveSpeedMult(player) + delta);
		return JS_NewFloat64(ctx, SAMLua::getMoveSpeedMult(player));
	}

	// sam_level_up(player[, count]) — queue count real engine level-ups (default 1) by
	// crediting EXP; the host's handleEffects grants them one/tick with full benefits.
	JSValue js_sam_level_up(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// The player is required, as luaL_checkinteger makes it in Lua; the count is not.
		int32_t player = -1, count = 1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_level_up") ) { return JS_NewBool(ctx, 0); }
		samJsOptI32(ctx, argc, argv, 1, &count, __func__);
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_level_up refused: host only."); return JS_NewBool(ctx, 0); }
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("JS", "sam_level_up: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const int levels = samClampInt(count, 1, 255);
		// What the next `count` levels cost under the XP curve, not a flat 100 each.
		long long exp = (long long)stats[player]->EXP;
		for ( int k = 0; k < levels; ++k ) { exp += SAMRules::xpThreshold((int)stats[player]->LVL + k); }
		if ( exp > 2147483647LL ) { exp = 2147483647LL; }
		stats[player]->EXP = (Sint32)exp;
		SAM_INFO("JS", "Queued " + std::to_string(levels) + " level-up(s) for player " + std::to_string(player) + ".");
		return JS_NewBool(ctx, 1);
#else
		(void)player; (void)count;
		return JS_NewBool(ctx, 1);
#endif
	}

	JSValue js_sam_get_floor(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		SAMLogger::noteApiCall();
		return JS_NewInt32(ctx, currentlevel);
	}

	// sam_get_seed() -> the run seed (uniqueGameKey). Int64, NOT Int32: uniqueGameKey is a
	// Uint32, and JS_NewInt32 would hand JavaScript a negative number for the same run that
	// Lua reports as positive — a parity bug that only shows on half the seeds.
	JSValue js_sam_get_seed(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		SAMLogger::noteApiCall();
		return JS_NewInt64(ctx, (int64_t)(uint64_t)uniqueGameKey);
	}

	// sam_get_flag("minotaurs") -> boolean, or undefined for an unknown name.
	JSValue js_sam_get_flag(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( !samHasArg(argc, argv, 0) ) { return JS_UNDEFINED; }
		const char* nameC = JS_ToCString(ctx, argv[0]);
		const std::string name = nameC ? nameC : "";
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		bool ok = false;
		const int v = SAMLua::lobbyFlag(name, ok);
		if ( !ok )
		{
			SAM_WARN("JS", "sam_get_flag: unknown flag '" + name + "'. Valid: " + SAMLua::lobbyFlagNames());
			return JS_UNDEFINED;
		}
		return JS_NewBool(ctx, v ? 1 : 0);
	}

	// sam_is_ghost(player) -> boolean. True while the player is a ghost that can act.
	JSValue js_sam_is_ghost(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua. false is a real answer here, so a
		// missing argument answers nothing at all instead.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_is_ghost") ) { return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, players[player]->ghost.isActive() ? 1 : 0);
	}

	// sam_is_spirit_ghost(player) -> boolean. Project Spirit (player still alive) vs death.
	JSValue js_sam_is_spirit_ghost(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua. false is a real answer here, so a
		// missing argument answers nothing at all instead.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_is_spirit_ghost") ) { return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, players[player]->ghost.isSpiritGhost() ? 1 : 0);
	}

	// sam_random("stream", lo, hi) -> integer in [lo, hi], deterministic per run.
	// Routed through SAMLua::randomDraw so Lua and JS share ONE counter per stream.
	JSValue js_sam_random(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( !samHasArg(argc, argv, 0) ) { SAM_ERROR("JS", "sam_random: argument 1 (the stream name) is required."); return JS_UNDEFINED; }
		const char* streamC = JS_ToCString(ctx, argv[0]);
		const std::string stream = streamC ? streamC : "";
		if ( streamC ) { JS_FreeCString(ctx, streamC); }
		int64_t lo = 0, hi = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &lo, "sam_random") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 2, &hi, "sam_random") ) { return JS_UNDEFINED; }
		return JS_NewInt64(ctx, (int64_t)SAMLua::randomDraw(g_currentNs, stream, (long long)lo, (long long)hi));
	}

	// sam_list_data_keys() -> array of this mod's saved key names.
	JSValue js_sam_list_data_keys(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		SAMLogger::noteApiCall();
		JSValue arr = JS_NewArray(ctx);
		const std::string dir = SAMLua::modDataDir(g_currentNs);
		uint32_t idx = 0;
		std::error_code ec;
		if ( !std::filesystem::exists(dir, ec) || ec ) { return arr; }
		std::filesystem::directory_iterator it(dir, ec);
		if ( ec ) { return arr; }
		for ( const auto& entry : it )
		{
			std::error_code ec2;
			if ( !entry.is_regular_file(ec2) || ec2 ) { continue; }
			std::string fn = entry.path().filename().string();
			if ( fn.size() <= 5 || fn.substr(fn.size() - 5) != ".json" ) { continue; }
			fn.erase(fn.size() - 5);
			// Decoded, so this returns the key sam_save_data was CALLED with. See the Lua twin.
			const std::string decoded = SAMLua::decodeDataKey(fn);
			JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, decoded.c_str()));
		}
		return arr;
	}

	JSValue js_sam_spawn_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t x = 0, y = 0;
		std::string name;
		// Required, as in Lua: a missing tile used to spawn the item at 0,0.
		if ( !samJsReqI32(ctx, argc, argv, 0, &x, "sam_spawn_item") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &y, "sam_spawn_item") ) { return JS_UNDEFINED; }
		if ( samHasArg(argc, argv, 2) ) { const char* s = JS_ToCString(ctx, argv[2]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_item refused: host only."); return JS_NewBool(ctx, 0); }
		// Resolve a custom "namespace:item" id first, else a vanilla name (case-insensitive),
		// matching sam_grant_item. Without the first tier a mod could not drop its OWN items,
		// which is the main thing scripts spawn.
		int resolvedType = -1;
		if ( name.find(':') != std::string::npos )
		{
			resolvedType = SAMItems::itemIdForIdString(name);
		}
		// One resolver, shared with every other name-taking call: digits, "ns:id", the internal
		// name, then the DISPLAYED name -- which is what sam_list_items and sam_get_container_items
		// hand out, and what this used to refuse.
		if ( resolvedType < 0 ) { resolvedType = SAMCatalog::itemTypeFor(name); }
		if ( resolvedType < 0 )
		{
			SAM_ERROR("JS", "sam_spawn_item: unknown item '" + name
				+ "' (expected a vanilla name like \"IRON_DAGGER\" or a custom \"namespace:item\").");
			return JS_NewBool(ctx, 0);
		}
		// Lua parity: status/beatitude/count settable, uid returned. See the Lua binding for
		// why -- restoring saved world state needs the item to come back as it was, and a
		// script needs a handle to what it placed.
		int32_t statusArg = (int)EXCELLENT, beatitudeArg = 0, countArg = 1;
		samJsOptI32(ctx, argc, argv, 3, &statusArg, __func__);
		samJsOptI32(ctx, argc, argv, 4, &beatitudeArg, __func__);
		samJsOptI32(ctx, argc, argv, 5, &countArg, __func__);
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
		if ( !e ) { SAM_ERROR("JS", "sam_spawn_item: invalid tile (" + std::to_string(x) + "," + std::to_string(y) + ")."); return JS_UNDEFINED; }
		SAM_INFO("JS", "Spawned item " + name + " at (" + std::to_string(x) + "," + std::to_string(y)
			+ ") uid " + std::to_string((unsigned long long)e->getUID()));
		return JS_NewInt64(ctx, (int64_t)e->getUID());
	}

	// sam_item_id("VANILLA_NAME" | "namespace:item") -> number|undefined. Resolve an item
	// type's numeric id, for matching against event fields like on_block's shield_type.
	// A name containing ':' resolves a custom S.A.M item; otherwise the vanilla tooltip
	// name map is used (case-insensitive). Returns undefined if the item is unknown.
	JSValue js_sam_item_id(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		std::string name;
		{ const char* s = JS_ToCString(ctx, argv[0]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		int id = -1;
		if ( name.find(':') != std::string::npos )
		{
			id = SAMItems::itemIdForIdString(name);
		}
		else { id = SAMCatalog::itemTypeFor(name); }   // shared resolver; accepts a displayed name too
		if ( id < 0 ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, id);
	}

	JSValue js_sam_message(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua; see sam_get_stat. This one answers false rather than undefined,
		// because every other way it can fail already answers false.
		int32_t player = -1;
		std::string text;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_message") ) { return JS_NewBool(ctx, 0); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { text = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_message refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS )
		{ SAM_ERROR("JS", "sam_message: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		SAMMpInput::scriptMessage(player, text);   // a joiner's game reads some texts as orders
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_play_sound(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t soundId = -1, vol = 128;
		if ( argc >= 1 && JS_IsString(argv[0]) )
		{
			const char* nm = JS_ToCString(ctx, argv[0]);
			// Through the shared resolver, so a bare "boom" means "one of MINE" here exactly as it
			// does in Lua. This called SAMSounds directly and so had no bare-id fallback at all.
			soundId = nm ? SAMLua::resolveSoundAssetIn(nm, g_currentNs) : -1;
			const std::string asked = nm ? nm : "";
			if ( nm ) { JS_FreeCString(ctx, nm); }
			if ( soundId < 0 ) { SAM_ERROR("JS", "sam_play_sound: unknown sound name '" + asked + "'."); return JS_NewBool(ctx, 0); }
		}
		// Required when it is not a name, as luaL_checkinteger makes it in Lua.
		else if ( !samJsReqI32(ctx, argc, argv, 0, &soundId, "sam_play_sound") ) { return JS_NewBool(ctx, 0); }
		// samHasArg, not a bare undefined test: an explicit null used to overwrite the 128
		// default with 0 and the sound played inaudibly, where the Lua call played normally.
		samJsOptI32(ctx, argc, argv, 1, &vol, __func__);
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_play_sound refused: host only."); return JS_NewBool(ctx, 0); }
		if ( soundId < 0 || (Uint32)soundId >= numsounds )
		{ SAM_ERROR("JS", "sam_play_sound: sound id " + std::to_string(soundId) + " out of range (0.." + std::to_string(numsounds) + ")."); return JS_NewBool(ctx, 0); }
		vol = samClampInt(vol, 0, 255);
		// Once on this machine, and by name to remote players for a mod sound (Lua parity).
		SAMSounds::playForEveryone(soundId, (Uint8)vol);
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_get_nearby_entities(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		double radiusTiles = 0.0;
		JSValue arr = JS_NewArray(ctx);
		// REQUIRED, as luaL_check* makes them in Lua. Readable on a client too; see the Lua twin.
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_nearby_entities") ) { return arr; }
		if ( !samJsReqF64(ctx, argc, argv, 1, &radiusTiles, "sam_get_nearby_entities") ) { return arr; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity || !map.entities ) { return arr; }
		Entity* pe = players[player]->entity;
		const double thresholdPx = radiusTiles * 16.0;
		uint32_t idx = 0;
		for ( node_t* node = map.entities->first; node != nullptr; node = node->next )
		{
			Entity* ent = (Entity*)node->element;
			if ( !ent || ent == pe ) { continue; }
			if ( !(ent->behavior == &actMonster || ent->behavior == &actPlayer) ) { continue; }
			if ( entityDist(pe, ent) <= thresholdPx )
			{
				JS_SetPropertyUint32(ctx, arr, idx++, JS_NewInt64(ctx, (int64_t)ent->getUID()));
				if ( idx >= 32 ) { break; }
			}
		}
		return arr;
	}

	// ---- expanded player queries (Part 5) --------------------------------------

	std::string samItemNameJs(int type)
	{
		for ( const auto& kv : ItemTooltips.itemNameStringToItemID )
		{
			if ( kv.second == type ) { std::string n = kv.first; for ( char& c : n ) { c = (char)std::toupper((unsigned char)c); } return n; }
		}
		if ( type >= 0 && type < NUM_ITEM_SLOTS ) { return std::string(items[type].getIdentifiedName()); }
		return "";
	}

	Item* samEquippedSlotJs(int player, const std::string& slot)
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

	JSValue js_sam_get_equipped_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Both required, as luaL_checkinteger / luaL_checkstring require them in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_equipped_item") ) { return JS_UNDEFINED; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_get_equipped_item: argument 2 (the slot) is required."); return JS_UNDEFINED; }
		std::string slot; { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { slot = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_UNDEFINED; }
		for ( char& c : slot ) { c = (char)std::toupper((unsigned char)c); }
		Item* it = samEquippedSlotJs(player, slot);
		if ( !it ) { return JS_UNDEFINED; }
		return JS_NewString(ctx, samItemNameJs((int)it->type).c_str());
	}

	// sam_get_equipped_item_id(player, slot) -> number|undefined. The NUMERIC item type, so it
	// can be compared against sam_item_id("ns:item"). js_sam_get_equipped_item above
	// returns a display NAME from the vanilla name table, which never contains custom
	// items — so it can never match a custom id.
	JSValue js_sam_get_equipped_item_id(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Both required, as luaL_checkinteger / luaL_checkstring require them in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_equipped_item_id") ) { return JS_UNDEFINED; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_get_equipped_item_id: argument 2 (the slot) is required."); return JS_UNDEFINED; }
		std::string slot; { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { slot = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_UNDEFINED; }
		for ( char& c : slot ) { c = (char)std::toupper((unsigned char)c); }
		Item* it = samEquippedSlotJs(player, slot);
		if ( !it ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, (int)it->type);
	}

	// sam_is_defending(player) -> boolean. Real engine blocking state, not just the button
	// being down. Correct for remote players too — vanilla syncs it with its 'SHLD' packet.
	JSValue js_sam_is_defending(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua. false is a real answer here, so a
		// missing argument answers nothing at all instead.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_is_defending") ) { return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, stats[player]->defending ? 1 : 0);
	}

	// sam_is_action_held(player, "Use") -> boolean. Reads a BOUND action, so it follows
	// the player's keybinds. Local player only (input never leaves its machine).
	JSValue js_sam_is_action_held(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua; see sam_get_stat.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_is_action_held") ) { return JS_UNDEFINED; }
		std::string action; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { action = s; JS_FreeCString(ctx, s); } }
		return JS_NewBool(ctx, SAMLua::isActionHeld(player, action) ? 1 : 0);
	}

	// sam_get_action_binding(player, "Use") -> string|undefined. The physical input behind an
	// action ("Mouse3"), for prompts. undefined when the player has it unbound.
	JSValue js_sam_get_action_binding(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua; see sam_get_stat. A missing argument is not an answer, so it
		// is refused with an error line of its own instead of reading as "unbound".
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_action_binding") ) { return JS_UNDEFINED; }
		std::string action; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { action = s; JS_FreeCString(ctx, s); } }
		const char* b = SAMLua::actionBinding(player, action);
		if ( !b || !b[0] ) { return JS_UNDEFINED; }
		return JS_NewString(ctx, b);
	}

	JSValue js_sam_get_inventory_count(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua; see sam_get_stat. undefined, not the 0 this answers for a player
		// who really carries none: 0 is a real count.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_inventory_count") ) { return JS_UNDEFINED; }
		std::string name; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NewInt32(ctx, 0); }
		// Shared resolver, and an unresolvable name says so rather than answering 0 in silence.
		const int wantType = SAMCatalog::itemTypeFor(name);
		if ( wantType < 0 )
		{
			SAM_ERROR("JS", "sam_get_inventory_count: unknown item '" + name
				+ "'. Returning 0, which is NOT the same as owning none -- check the name.");
		}
		if ( wantType < 0 ) { return JS_NewInt32(ctx, 0); }
		// Through the inventory mirror; undefined when this machine cannot see that backpack. See the Lua twin.
		long long total = 0;
		if ( SAMMpInventory::countOf((int)player, wantType, "sam_get_inventory_count", &total) == SAMMpInventory::Seen::No )
		{
			return JS_UNDEFINED;
		}
		return JS_NewInt64(ctx, total);
	}

	JSValue js_sam_has_effect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua; see sam_get_stat. undefined, not the false this answers for a
		// player who really does not have the effect: false is a real answer.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_has_effect") ) { return JS_UNDEFINED; }
		std::string name; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NewBool(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 ) { SAM_WARN("JS", "sam_has_effect: unknown effect '" + name + "'. Valid: " + SAMLua::effectNameHint()); return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, stats[player]->getEffectActive(eff) != 0 ? 1 : 0);
	}

	JSValue js_sam_get_class(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_class") ) { return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS ) { return JS_UNDEFINED; }
		// SAM-aware, mirroring the Lua binding: custom ids resolve from the registry, since
		// playerClassLangEntry returns a bogus string for them (see the Lua samClassName note).
		//
		// def->id, NOT def->name. This said `name` while the Lua twin was changed to `id`, so the
		// two runtimes returned different strings for the same call and only ONE of them is
		// accepted by anything: SAMClasses::classIdForIdString matches against the id, so
		// sam_patch_class(sam_get_class(0), ...) -- the read-then-modify pattern the docs
		// advertise -- worked in Lua and was refused in JavaScript. The display name resolves
		// nowhere and cannot be fed back into any function that takes a class.
		const int cls = client_classes[player];
		if ( cls >= SAM_CLASS_ID_BASE )
		{
			const SAMClassDef* def = SAMClasses::getClass(cls);
			return JS_NewString(ctx, def ? def->id.c_str() : "");
		}
		return JS_NewString(ctx, playerClassLangEntry(cls, player));
	}

	// The race identifier for a player: a custom race's "namespace:race" id, or the
	// vanilla race's name. Lets a race behavior script gate its logic by race.
	JSValue js_sam_get_race(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua: a missing player used to be answered
		// in silence. A slot with nobody in it answers undefined here, nil in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_race") ) { return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_UNDEFINED; }
		const int race = stats[player]->playerRace;
		if ( race >= SAM_RACE_ID_BASE )
		{
			const SAMRaceDef* def = SAMRaces::get(race);
			return JS_NewString(ctx, def ? def->id.c_str() : "");
		}
		const int mon = (int)getMonsterFromPlayerRace(race);
		if ( mon >= 0 && mon < NUMMONSTERS ) { return JS_NewString(ctx, monstertypename[mon]); }
		return JS_NewString(ctx, "");
	}

	JSValue js_sam_get_kills(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua. A kill count of 0 is a real answer,
		// so a missing player answers nothing at all instead.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_kills") ) { return JS_UNDEFINED; }
		return JS_NewInt64(ctx, SAMLua::getKills(player)); // shared session counter
	}

	JSValue js_sam_get_time_played(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		SAMLogger::noteApiCall();
		return JS_NewInt64(ctx, (int64_t)ticks);
	}
#endif // SAM_JS_HAVE_BARONY

	// ---- persistent per-mod data (Part 3) --------------------------------------
	// JSON under <savegames>/sam_mod_data/<namespace>/<key>.json via QuickJS's
	// built-in JSON. Namespace comes from the currently-executing script (g_currentNs).

	// The key codec lives in the Lua runtime and is exported, so both runtimes map a key to the
	// SAME file. It used to be duplicated byte-for-byte here, which is how two copies of one
	// resolver drift apart.
	static std::string samEncodeKey(const std::string& k) { return SAMLua::encodeDataKey(k); }
	static std::string samDecodeKey(const std::string& k) { return SAMLua::decodeDataKey(k); }

	std::string samSanitize(const std::string& s)
	{
		std::string o;
		for ( char c : s ) { o += ( c == '/' || c == '\\' || c == ':' || c == '.' ) ? '_' : c; }
		return o.empty() ? std::string("_") : o;
	}

	std::string samModDataFile(const std::string& ns, const std::string& key)
	{
#ifdef SAM_JS_HAVE_BARONY
		const std::string base = std::string(outputdir) + "/savegames/sam_mod_data";
#else
		const std::string base = "./sam_mod_data";
#endif
		const std::string dir = base + "/" + samSanitize(ns);
		// See the Lua twin: rescue anything an older build wrote under the previous filename.
		SAMLua::migrateLegacyDataFile(dir, key);
		return dir + "/" + samEncodeKey(key) + ".json";
	}

	// JSON.stringify for the four "store this value" bindings. A value that cannot be
	// stringified -- a cycle, a BigInt, a throwing toJSON -- used to be stored as the literal
	// "null" while the call reported success, destroying whatever the mod had saved under
	// that key; and the exception QuickJS raised was left pending on the context to surface
	// somewhere unrelated. Consume it, say what happened, refuse the write.
	bool samJsStringifyForStore(JSContext* ctx, JSValueConst v, const char* what, std::string& out)
	{
		JSValue jstr = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
		if ( JS_IsException(jstr) )
		{
			SAM_ERROR("JS", std::string(what) + ": value cannot be converted to JSON ("
				+ exceptionToString(ctx) + ") - nothing written.");
			return false;
		}
		if ( JS_IsUndefined(jstr) )
		{
			// JSON.stringify(undefined) and stringify(function) yield undefined, not "null".
			JS_FreeValue(ctx, jstr);
			SAM_WARN("JS", std::string(what) + ": value has no JSON form (undefined or a function) - nothing written.");
			return false;
		}
		const char* c = JS_ToCString(ctx, jstr);
		out = c ? c : "null";
		if ( c ) { JS_FreeCString(ctx, c); }
		JS_FreeValue(ctx, jstr);
		return true;
	}

	JSValue js_sam_save_data(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NewBool(ctx, 0); }
		const char* keyC = JS_ToCString(ctx, argv[0]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( g_currentNs.empty() ) { SAM_WARN("JS", "sam_save_data: no owning mod namespace — ignored."); return JS_NewBool(ctx, 0); }
		JSValueConst v = ( samHasArg(argc, argv, 1) ) ? argv[1] : JS_NULL;
		std::string json;
		if ( !samJsStringifyForStore(ctx, v, "sam_save_data", json) ) { return JS_NewBool(ctx, 0); }
		const std::string path = samModDataFile(g_currentNs, key);
		try
		{
			// Temp file + rename: a crash mid-write keeps the previous value on disk
			// instead of leaving a truncated file that will not parse next launch.
			if ( !SAMErrors::writeFileAtomic(path, json) ) { SAM_ERROR("JS", "sam_save_data: cannot write " + path); return JS_NewBool(ctx, 0); }
		}
		catch ( ... ) { SAM_ERROR("JS", "sam_save_data: failed writing key '" + key + "'."); return JS_NewBool(ctx, 0); }
		SAM_INFO("SAM", "Saved data key '" + key + "' for [" + g_currentNs + "]");
		return JS_NewBool(ctx, 1);
	}

	// The Lua twin's jsonDepthWithinLimit (sam_lua_runtime.cpp): text that nests deeper than
	// `limit` is refused BEFORE it is parsed, so both runtimes load exactly the same files.
	// String contents are skipped so brackets inside strings do not count.
	static const int SAM_JS_JSON_MAX_DEPTH = 64;   // the Lua twin's SAM_JSON_MAX_DEPTH
	static bool samJsJsonDepthWithinLimit(const std::string& text, int limit)
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

	// sam_load_data(key) -> the stored value, or undefined. Undefined wherever the Lua twin
	// answers nil, and data nested deeper than 64 levels is refused as it is in Lua. A value
	// that was stored AS null still reads back as null, so "never stored" is tellable apart.
	JSValue js_sam_load_data(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkstring makes it in Lua.
		if ( !samHasArg(argc, argv, 0) ) { SAM_ERROR("JS", "sam_load_data: argument 1 (the key) is required."); return JS_UNDEFINED; }
		const char* keyC = JS_ToCString(ctx, argv[0]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( g_currentNs.empty() ) { return JS_UNDEFINED; }
		std::ifstream f(samModDataFile(g_currentNs, key), std::ios::binary);
		if ( !f.is_open() ) { return JS_UNDEFINED; }
		const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if ( !samJsJsonDepthWithinLimit(text, SAM_JS_JSON_MAX_DEPTH) )
		{
			SAM_WARN("JS", "sam_load_data: data for key '" + key + "' nests too deep — undefined.");
			return JS_UNDEFINED;
		}
		JSValue v = JS_ParseJSON(ctx, text.c_str(), text.size(), "sam_mod_data");
		if ( JS_IsException(v) )
		{
			// Consume the parse error: left pending on the context it would surface at the next
			// unrelated call.
			JS_FreeValue(ctx, JS_GetException(ctx));
			SAM_WARN("JS", "sam_load_data: corrupt data for key '" + key + "' — undefined.");
			return JS_UNDEFINED;
		}
		SAM_INFO("SAM", "Loaded data key '" + key + "' for [" + g_currentNs + "]");
		return v;
	}

	// ---- v1.11.0 persistent world state (Lua parity: sam_set_chest_stash /
	//      sam_travel_to_level / sam_world_save / _load / _clear / _keys) ------------------
	// Rationale and the engine details are documented once, on the Lua side.

	// ---- v2.0 owned behaviours (Lua parity: sam_register_behavior / sam_spawn_entity) ----

	JSValue js_sam_register_behavior(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		const char* nameC = JS_ToCString(ctx, argv[0]);
		std::string full = nameC ? nameC : "";
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		if ( !JS_IsFunction(ctx, argv[1]) )
		{
			SAM_ERROR("JS", "sam_register_behavior: second argument must be a function.");
			return JS_FALSE;
		}
		if ( g_currentNs.empty() )
		{
			SAM_WARN("JS", "sam_register_behavior: no owning mod namespace - ignored.");
			return JS_FALSE;
		}
		if ( full.find(':') == std::string::npos ) { full = g_currentNs + ":" + full; }

		// QuickJS values are refcounted. Without this dup the engine would call a collected
		// function every frame the moment the script's own reference went away.
		JSValue* held = new JSValue(JS_DupValue(ctx, argv[1]));
		if ( SAMLua::registerBehaviorJs(full, g_currentNs, (void*)held) < 0 )
		{
			JS_FreeValue(ctx, *held); delete held; return JS_FALSE;
		}
		return JS_TRUE;
	}

	JSValue js_sam_set_entity_facing(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		double rad = 0; JS_ToFloat64(ctx, &rad, argv[1]);   // required: undefined -> NaN -> refused
		return JS_NewBool(ctx, SAMLua::setEntityFacing((unsigned long long)uid, rad) ? 1 : 0);
	}

	JSValue js_sam_look_at(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0, tgt = 0;
		JS_ToInt64(ctx, &uid, argv[0]);
		JS_ToInt64(ctx, &tgt, argv[1]);
		return JS_NewBool(ctx, SAMLua::lookAt((unsigned long long)uid, (unsigned long long)tgt) ? 1 : 0);
	}

	JSValue js_sam_get_entity_facing(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const double y = SAMLua::entityFacing((unsigned long long)uid);
		if ( y < 0.0 ) { return JS_UNDEFINED; }
		return JS_NewFloat64(ctx, y);
	}

	JSValue js_sam_spawn_entity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_UNDEFINED; }
		// NO samHasArg on these two: they are REQUIRED, and the helper would turn a missing
		// argument into a silent 0 -- a legal tile -- instead of letting undefined become NaN
		// and be caught by the isfinite guard in the shared spawner.
		double x = 0, y = 0;
		JS_ToFloat64(ctx, &x, argv[0]);
		JS_ToFloat64(ctx, &y, argv[1]);
		const char* behC = JS_ToCString(ctx, argv[2]);
		const char* modelC = samHasArg(argc, argv, 3) ? JS_ToCString(ctx, argv[3]) : nullptr;
		const unsigned long long uid = SAMLua::spawnScriptedEntity(
			x, y, behC ? behC : "", modelC ? modelC : "", g_currentNs);
		if ( behC ) { JS_FreeCString(ctx, behC); }
		if ( modelC ) { JS_FreeCString(ctx, modelC); }
		if ( uid == 0 ) { return JS_UNDEFINED; }
		return JS_NewInt64(ctx, (int64_t)uid);
	}

	JSValue js_sam_set_chest_stash(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const bool on = samHasArg(argc, argv, 1) ? ( JS_ToBool(ctx, argv[1]) > 0 ) : true;
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_chest_stash refused: host only."); return JS_FALSE; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { SAM_WARN("JS", "sam_set_chest_stash: no entity with uid " + std::to_string(uid) + "."); return JS_FALSE; }
		if ( e->behavior != &actChest )
		{
			SAM_WARN("JS", "sam_set_chest_stash: uid " + std::to_string(uid) + " is not a chest.");
			return JS_FALSE;
		}
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
				SAM_WARN("JS", "sam_set_chest_stash: this chest already holds "
					+ std::to_string(held) + " item stack(s). They are not destroyed, but they"
					" are hidden while it is a stash; turn the stash off to reach them again."
					" Prefer converting an empty chest.");
			}
		}
		e->chestVoidState = on ? -1 : 0;
		serverUpdateEntitySkill(e, 17);
		SAM_INFO("JS", std::string("Chest ") + std::to_string(uid)
			+ (on ? " is now a stash." : " is a normal chest again."));
		return JS_TRUE;
	}

	JSValue js_sam_travel_to_level(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// An undefined floor used to convert to 0 and silently send the party to floor 0;
		// the Lua binding raises. Refuse, and say so.
		int32_t target = 0;
		if ( !samHasArg(argc, argv, 0) || JS_ToInt32(ctx, &target, argv[0]) < 0 )
		{
			SAM_ERROR("JS", "sam_travel_to_level: needs a floor number.");
			return JS_FALSE;
		}
		bool secret = false;
		if ( samHasArg(argc, argv, 1) && JS_IsObject(argv[1]) )
		{
			JSValue sv = JS_GetPropertyStr(ctx, argv[1], "secret");
			secret = ( JS_ToBool(ctx, sv) > 0 );
			JS_FreeValue(ctx, sv);
		}
		// Same shared implementation the Lua binding calls -- see SAMLua::travelToLevel.
		return JS_NewBool(ctx, SAMLua::travelToLevel(target, secret, "JS") ? 1 : 0);
	}

	JSValue js_sam_world_save(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NewBool(ctx, 0); }
		const char* keyC = JS_ToCString(ctx, argv[0]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( g_currentNs.empty() ) { SAM_WARN("JS", "sam_world_save: no owning mod namespace - ignored."); return JS_NewBool(ctx, 0); }
		JSValueConst v = ( samHasArg(argc, argv, 1) ) ? argv[1] : JS_NULL;
		std::string json;
		if ( !samJsStringifyForStore(ctx, v, "sam_world_save", json) ) { return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, SAMWorldState::set(g_currentNs, key, json) ? 1 : 0);
	}

	JSValue js_sam_world_load(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		const char* keyC = JS_ToCString(ctx, argv[0]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( g_currentNs.empty() ) { return JS_UNDEFINED; }
		std::string raw;
		if ( !SAMWorldState::get(g_currentNs, key, raw) ) { return JS_UNDEFINED; }
		JSValue v = JS_ParseJSON(ctx, raw.c_str(), raw.size(), "sam_world_state");
		if ( JS_IsException(v) )
		{
			JS_FreeValue(ctx, v);
			SAM_WARN("JS", "sam_world_load: saved value for '" + key + "' is corrupt - ignoring it.");
			return JS_UNDEFINED;
		}
		return v;
	}

	JSValue js_sam_world_clear(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NewBool(ctx, 0); }
		const char* keyC = JS_ToCString(ctx, argv[0]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( g_currentNs.empty() ) { return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, SAMWorldState::erase(g_currentNs, key) ? 1 : 0);
	}

	JSValue js_sam_world_keys(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		JSValue arr = JS_NewArray(ctx);
		if ( g_currentNs.empty() ) { return arr; }
		uint32_t n = 0;
		for ( const std::string& k : SAMWorldState::keys(g_currentNs) )
		{
			JS_SetPropertyUint32(ctx, arr, n++, JS_NewString(ctx, k.c_str()));
		}
		return arr;
	}

	JSValue js_sam_delete_data(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NewBool(ctx, 0); }
		const char* keyC = JS_ToCString(ctx, argv[0]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( g_currentNs.empty() ) { return JS_NewBool(ctx, 0); }
		std::error_code ec;
		const bool removed = std::filesystem::remove(std::filesystem::path(samModDataFile(g_currentNs, key)), ec);
		SAM_INFO("SAM", "Deleted data key '" + key + "' for [" + g_currentNs + "]" + (removed ? "" : " (was absent)"));
		return JS_NewBool(ctx, 1);
	}

	// ---- timers (Part 4) -------------------------------------------------------

	void samRemoveJsTimer(const std::string& ns, const std::string& id)
	{
		for ( size_t i = 0; i < g_jsTimers.size(); ++i )
		{
			if ( g_jsTimers[i].ns == ns && g_jsTimers[i].id == id )
			{
				if ( g_jsTimers[i].ctx ) { JS_FreeValue(g_jsTimers[i].ctx, g_jsTimers[i].callback); }
				g_jsTimers.erase(g_jsTimers.begin() + i);
				return;
			}
		}
	}

	JSValue samSetJsTimer(JSContext* ctx, int argc, JSValueConst* argv, bool repeating)
	{
		const char* who = repeating ? "sam_set_repeating_timer" : "sam_set_timer";
		if ( argc < 3 ) { SAM_ERROR("JS", std::string(who) + ": needs (id, ticks, callback)."); return JS_UNDEFINED; }
		const char* idC = JS_ToCString(ctx, argv[0]);
		const std::string id = idC ? idC : "";
		if ( idC ) { JS_FreeCString(ctx, idC); }
		int64_t ticks64 = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &ticks64, who) ) { return JS_UNDEFINED; }
		// 1..INT32_MAX, the same bounds as the Lua twin.
		const int32_t ticks = (int32_t)( ticks64 < 1 ? 1 : ( ticks64 > 2147483647LL ? 2147483647LL : ticks64 ) );
		if ( !JS_IsFunction(ctx, argv[2]) ) { SAM_WARN("JS", "sam_set_timer: callback must be a function."); return JS_UNDEFINED; }
		samRemoveJsTimer(g_currentNs, id);
		JsTimer t;
		t.id = id; t.ns = g_currentNs; t.ctx = ctx; t.callback = JS_DupValue(ctx, argv[2]);
		t.remaining = ticks < 1 ? 1 : ticks;
		t.interval  = repeating ? (ticks < 1 ? 1 : ticks) : 0;
		t.repeating = repeating;
		g_jsTimers.push_back(t);
		SAM_INFO("SAM", std::string("Timer '") + id + "' set for " + std::to_string(ticks) + " ticks" + (repeating ? " (repeating)" : ""));
		return JS_UNDEFINED;
	}

	JSValue js_sam_set_timer(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)           { return samSetJsTimer(ctx, argc, argv, false); }
	JSValue js_sam_set_repeating_timer(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) { return samSetJsTimer(ctx, argc, argv, true); }

	JSValue js_sam_cancel_timer(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		const char* idC = JS_ToCString(ctx, argv[0]);
		samRemoveJsTimer(g_currentNs, idC ? idC : "");
		if ( idC ) { JS_FreeCString(ctx, idC); }
		return JS_UNDEFINED;
	}

	// ---- custom hooks (Part 2) -------------------------------------------------

	JSValue js_sam_register_hook(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		const char* nameC = JS_ToCString(ctx, argv[0]);
		const std::string name = nameC ? nameC : "";
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		if ( name.find(':') == std::string::npos )
		{
			SAM_WARN("JS", "sam_register_hook: name '" + name + "' must be namespaced (\"namespace:hook_name\").");
			return JS_UNDEFINED;
		}
		g_customHooks.push_back(name);
		SAM_INFO("JS", "Registered custom hook: " + name);
		return JS_UNDEFINED;
	}

	// sam_fire_hook("ns:name", event_object) — dispatch to ALL JS + Lua scripts
	// (cross-runtime), host-authoritative. Only primitive fields cross over.
	JSValue js_sam_fire_hook(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		const char* nameC = JS_ToCString(ctx, argv[0]);
		const std::string name = nameC ? nameC : "";
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_fire_hook refused: host only."); return JS_UNDEFINED; }
#endif
		if ( g_fireDepth >= 8 ) { SAM_WARN("JS", "sam_fire_hook: recursion too deep — '" + name + "' not fired."); return JS_UNDEFINED; }

		SAMJs::Event  jsev; jsev.setName(name);
		SAMLua::Event ev;   ev.setName(name);
		if ( argc >= 2 && JS_IsObject(argv[1]) )
		{
			JSPropertyEnum* tab = nullptr;
			uint32_t plen = 0;
			if ( JS_GetOwnPropertyNames(ctx, &tab, &plen, argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 )
			{
				for ( uint32_t i = 0; i < plen; ++i )
				{
					const char* keyC = JS_AtomToCString(ctx, tab[i].atom);
					const std::string k = keyC ? keyC : "";
					if ( keyC ) { JS_FreeCString(ctx, keyC); }
					JSValue val = JS_GetProperty(ctx, argv[1], tab[i].atom);
					if ( JS_IsNumber(val) )      { double d = 0; JS_ToFloat64(ctx, &d, val); ev.i(k, (long long)d); jsev.i(k, (long long)d); }
					else if ( JS_IsBool(val) )   { const long long b = JS_ToBool(ctx, val) ? 1 : 0; ev.i(k, b); jsev.i(k, b); }
					else if ( JS_IsString(val) ) { const char* s = JS_ToCString(ctx, val); if ( s ) { ev.s(k, s); jsev.s(k, s); JS_FreeCString(ctx, s); } }
					JS_FreeValue(ctx, val);
					JS_FreeAtom(ctx, tab[i].atom);
				}
				js_free(ctx, tab);
			}
		}

		++g_fireDepth;
		const std::string savedNs = g_currentNs;
		// Lua FIRST, then JS -- the order every engine site and SamEvent::fire use, and it
		// matters twice over: SAMLua::dispatchEvent is what clears and re-seeds the shared
		// write-back store from THIS hook's payload, and the JS pass reads the live store so
		// edits chain. Written the other way round (as it was), JS listeners ran against
		// whatever the ENCLOSING event had left in the store -- a "player" or "damage" from a
		// different hook -- and everything they wrote back was wiped a line later. `+` has no
		// sequencing guarantee either; two statements do.
		int n = SAMLua::dispatchEvent(ev);
		n += SAMJs::dispatchEvent(jsev);
		g_currentNs = savedNs; // the nested dispatch cleared g_currentNs; restore the firer's
		--g_fireDepth;
		SAM_INFO("SAM", "Fired custom hook: " + name + " to " + std::to_string(n) + " script(s)");
		return JS_NewInt32(ctx, n); // return the count of scripts reached
	}

	// v0.7.0 Feature 2: sam_modify_damage(player, new_value) — rewrite incoming damage
	// from inside an on_before_damage callback (routes to the shared SAMLua latch).
	JSValue js_sam_modify_damage(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Checked, as in Lua. Raw coercion wrote 0 for undefined, NaN or "abc" into the latch, so a
		// mistyped field made the hit do nothing in JS while the same script raised in Lua.
		int32_t player = 0; int64_t v = 0;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_modify_damage") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &v, "sam_modify_damage") ) { return JS_UNDEFINED; }
		if ( !SAMLua::beforeDamageActive() )
		{
			SAM_WARN("JS", "sam_modify_damage: only valid inside an on_before_damage callback — ignored.");
			return JS_UNDEFINED;
		}
		SAMLua::beforeDamageModify(player, (long long)v);
		return JS_UNDEFINED;
	}

	// sam_modify_monster_damage(new_value) — the monster-side counterpart. See the Lua
	// runtime; no subject argument because only one monster is ever mid-dispatch.
	JSValue js_sam_modify_monster_damage(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t v = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &v, "sam_modify_monster_damage") ) { return JS_UNDEFINED; }
		if ( !SAMLua::beforeMonsterDamageActive() )
		{
			SAM_WARN("JS", "sam_modify_monster_damage: only valid inside an on_before_monster_damage callback — ignored.");
			return JS_UNDEFINED;
		}
		SAMLua::beforeMonsterDamageModify((long long)v);
		return JS_UNDEFINED;
	}

	// sam_modify_value(new_value) — see the Lua runtime. Parity matters: the same script
	// text must work in both.
	JSValue js_sam_modify_value(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t v = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &v, "sam_modify_value") ) { return JS_UNDEFINED; }
		if ( !SAMLua::hookValueActive() )
		{
			SAM_WARN("JS", "sam_modify_value: no hook is offering a value to rewrite right now — ignored. "
				"It works inside player.on_xp_gained; for damage use sam_modify_damage (player) "
				"or sam_modify_monster_damage (monster).");
			return JS_UNDEFINED;
		}
		SAMLua::hookValueModify((long long)v);
		return JS_UNDEFINED;
	}

	// v0.7.0 Feature 2: sam_deal_damage(entity_uid, amount) — deal damage to any entity
	// by UID (host-only, UID-only, existence-validated). Positive amount = damage.
	JSValue js_sam_deal_damage(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// The checked helpers, not raw JS_ToInt32. This batch repaired this function's entity
		// resolution and left its ARGUMENTS on the coercion the batch exists to remove:
		// JS_ToInt32 reports SUCCESS for "ten", {}, [] and NaN, every one of which becomes 0, so
		// sam_deal_damage(boss, somethingUnreadable) logged "0 damage", returned true and left
		// the boss untouched -- while the Lua twin's luaL_checkinteger raises on all of them.
		int64_t uid = 0; int32_t amount = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_deal_damage") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &amount, "sam_deal_damage") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_deal_damage");
		if ( !e ) { return JS_FALSE; }
		const int dmg = ( amount < 0 ) ? amount : -amount;
		e->modHP(dmg);
		SAM_INFO("SAM", "sam_deal_damage: " + std::to_string(-dmg) + " damage to uid " + std::to_string(uid));
		return JS_TRUE;
#else
		(void)uid; (void)amount;
		return JS_FALSE;
#endif
	}

	// v0.7.0 Feature 3: sam_is_key_held(key_name) -> boolean.
	JSValue js_sam_is_key_held(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		const char* nameC = JS_ToCString(ctx, argv[0]);
		const std::string name = nameC ? nameC : "";
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		// The optional player, as in Lua: absent/null/undefined = the default player.
		int32_t player = -1; samJsOptI32(ctx, argc, argv, 1, &player, __func__);
#ifdef SAM_JS_HAVE_BARONY
		const bool held = SAMMpInput::keyHeld(name, player);
#else
		(void)player;
		const bool held = SAMLua::isKeyHeld(name);
#endif
		return held ? JS_TRUE : JS_FALSE;
	}

	// ---- v0.7.0 Feature 4: monster / NPC scripting (UID-based) -----------------
#ifdef SAM_JS_HAVE_BARONY
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

	// ---- v2 world-ops: position / teleport / spawn / inventory (JS twins) -------
	// Positions are MAP TILE coordinates (integers). Mirrors the Lua bindings.

	// sam_get_player_uid(player) -> entity uid | undefined.
	JSValue js_sam_get_player_uid(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua: a missing player used to be answered
		// in silence, which reads as "that player has no body".
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_player_uid") ) { return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity ) { return JS_UNDEFINED; }
		return JS_NewInt64(ctx, (int64_t)players[player]->entity->getUID());
	}

	// sam_get_position(uid) -> [tileX, tileY] | undefined.
	
	// ============================================================================
	// v2.6 batch 1: reads and dice. Twins of the Lua bindings; see sam_lua_runtime.cpp
	// for the reasoning behind each. Two deliberate language differences:
	//   - Lua returns several values; JS returns an array.
	//   - sam_random_from_list indexes from 0 here and from 1 there, because each follows
	//     its own language. Porting one literally is the parity bug this project has hit
	//     before, so they are written separately on purpose.
	// ============================================================================

	JSValue js_sam_get_position_precise(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_position_precise") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
		JSValue a = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, a, 0, JS_NewFloat64(ctx, (double)e->x));
		JS_SetPropertyUint32(ctx, a, 1, JS_NewFloat64(ctx, (double)e->y));
		JS_SetPropertyUint32(ctx, a, 2, JS_NewFloat64(ctx, (double)e->z));
		return a;
	}

	JSValue js_sam_get_distance(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t ua = 0, ub = 0;
		// Both required, as luaL_checkinteger makes them in Lua.
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &ua, "sam_get_distance") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &ub, "sam_get_distance") ) { return JS_UNDEFINED; }
		Entity* a = SAMLua::resolveEntityQuiet((long long)ua);
		Entity* b = SAMLua::resolveEntityQuiet((long long)ub);
		if ( !a || !b ) { return JS_UNDEFINED; }
		return JS_NewFloat64(ctx, (double)(entityDist(a, b) / 16.0));
	}

	JSValue js_sam_get_distance_to(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t tx = 0, ty = 0;
		// All three are required, as luaL_checkinteger makes them in Lua. A forgotten y used to
		// keep the default 0 and measure the distance to tile (x, 0) -- a perfectly plausible
		// number, which is worse than no answer at all.
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_distance_to") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &tx, "sam_get_distance_to") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &ty, "sam_get_distance_to") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
		const double dx = e->x - ((double)tx * 16.0 + 8.0);
		const double dy = e->y - ((double)ty * 16.0 + 8.0);
		return JS_NewFloat64(ctx, std::sqrt(dx * dx + dy * dy) / 16.0);
	}

	JSValue js_sam_get_entity_type(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_entity_type") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
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
		// See the Lua twin: "gold" was accepted by sam_find_entities and produced by nothing.
		else if ( e->behavior == &actGoldBag )      { kind = "gold"; }
		return JS_NewString(ctx, kind);
	}

	JSValue js_sam_get_scale(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_scale") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
		JSValue a = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, a, 0, JS_NewFloat64(ctx, (double)e->scalex));
		JS_SetPropertyUint32(ctx, a, 1, JS_NewFloat64(ctx, (double)e->scaley));
		JS_SetPropertyUint32(ctx, a, 2, JS_NewFloat64(ctx, (double)e->scalez));
		return a;
	}

	JSValue js_sam_is_visible(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_is_visible") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
		return JS_NewBool(ctx, e->flags[INVISIBLE] ? 0 : 1);
	}

	JSValue js_sam_get_velocity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_velocity") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
		JSValue a = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, a, 0, JS_NewFloat64(ctx, (double)e->vel_x));
		JS_SetPropertyUint32(ctx, a, 1, JS_NewFloat64(ctx, (double)e->vel_y));
		JS_SetPropertyUint32(ctx, a, 2, JS_NewFloat64(ctx, (double)e->vel_z));
		return a;
	}

	JSValue js_sam_get_entity_size(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_entity_size") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
		JSValue a = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, (int)e->sizex));
		JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, (int)e->sizey));
		return a;
	}

	JSValue js_sam_get_entity_sprite(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_entity_sprite") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, (int)e->sprite);
	}

	JSValue js_sam_get_entity_ticks(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_entity_ticks") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
		return JS_NewInt64(ctx, (int64_t)e->ticks);
	}

	JSValue js_sam_get_map_seed(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewInt64(ctx, (int64_t)(uint64_t)mapseed);
	}

	JSValue js_sam_is_dark_level(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewBool(ctx, darkmap ? 1 : 0);
	}

	JSValue js_sam_get_playable_bounds(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		JSValue a = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, getMapPossibleLocationX1()));
		JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, getMapPossibleLocationY1()));
		JS_SetPropertyUint32(ctx, a, 2, JS_NewInt32(ctx, getMapPossibleLocationX2()));
		JS_SetPropertyUint32(ctx, a, 3, JS_NewInt32(ctx, getMapPossibleLocationY2()));
		return a;
	}

	JSValue js_sam_is_tile_diggable(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t x = 0, y = 0;
		// Both required, as luaL_checkinteger makes them in Lua. A forgotten y used to keep the
		// default 0 and answer about tile (x, 0), which is in bounds, so a real yes or no came
		// back about a tile the mod never asked about.
		if ( !samJsReqI32(ctx, argc, argv, 0, &x, "sam_is_tile_diggable") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &y, "sam_is_tile_diggable") ) { return JS_UNDEFINED; }
		// See the Lua twin: mapTileDiggable validates nothing, because every engine caller
		// hands it an in-bounds raycast hit. map.tiles is also null before a level loads.
		if ( !map.tiles || x < 0 || x >= (int)map.width || y < 0 || y >= (int)map.height )
		{
			return JS_FALSE;
		}
		return JS_NewBool(ctx, mapTileDiggable((int)x, (int)y) ? 1 : 0);
	}

	JSValue js_sam_get_map_flags(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		// MFLAG_* already extract their byte out of map.flags (main.hpp:536); using them to
		// index map.flags a second time made every field read map.flags[0]. See the Lua twin.
		JSValue o = JS_NewObject(ctx);
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
			JS_SetPropertyStr(ctx, o, f.name, JS_NewBool(ctx, f.value != 0 ? 1 : 0));
		}
		JS_SetPropertyStr(ctx, o, "perimeter_gap", JS_NewInt32(ctx, (int)MFLAG_PERIMETER_GAP));
		return o;
	}

	JSValue js_sam_get_exit_position(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		// No map yet (the title screen, a floor still loading): nothing to find. The other map
		// readers test this; this one dereferenced it.
		if ( !map.entities ) { return JS_UNDEFINED; }
		for ( node_t* node = map.entities->first; node; node = node->next )
		{
			Entity* e = (Entity*)node->element;
			if ( !e ) { continue; }
			// Mirrors drawminimap.cpp:1348-1359, same as the Lua twin: skip secret ladders,
			// Mages Guild's decorative portal, and the framework's own sam_spawn_portal.
			bool isExit = false;
			if ( e->behavior == &actLadder )            { isExit = ( e->skill[3] != 1 ); }
			else if ( e->behavior == &actPortal )       { isExit = ( e->skill[19] != 1 ) && ( e->portalNotSecret == 1 ); }
			else if ( e->behavior == &actCustomPortal ) { isExit = true; }
			if ( isExit )
			{
				JSValue a = JS_NewArray(ctx);
				JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, (int)e->x >> 4));
				JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, (int)e->y >> 4));
				return a;
			}
		}
		return JS_UNDEFINED;
	}

	JSValue js_sam_get_run_time(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewFloat64(ctx, (double)completionTime / (double)TICKS_PER_SECOND);
	}

	JSValue js_sam_get_tick_rate(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewInt32(ctx, (int)TICKS_PER_SECOND);
	}

	JSValue js_sam_get_fps(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewFloat64(ctx, (double)fps);
	}

	JSValue js_sam_get_real_time(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewInt64(ctx, (int64_t)getTime());
	}

	JSValue js_sam_get_date(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
		getTimeAndDate(getTime(), &y, &mo, &d, &h, &mi, &sec);
		JSValue o = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, o, "year",  JS_NewInt32(ctx, y));
		JS_SetPropertyStr(ctx, o, "month", JS_NewInt32(ctx, mo));
		JS_SetPropertyStr(ctx, o, "day",   JS_NewInt32(ctx, d));
		JS_SetPropertyStr(ctx, o, "hour",  JS_NewInt32(ctx, h));
		JS_SetPropertyStr(ctx, o, "min",   JS_NewInt32(ctx, mi));
		JS_SetPropertyStr(ctx, o, "sec",   JS_NewInt32(ctx, sec));
		return o;
	}

	JSValue js_sam_is_paused(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewBool(ctx, gamePaused ? 1 : 0);
	}

	JSValue js_sam_is_in_game(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewBool(ctx, intro ? 0 : 1);
	}

	JSValue js_sam_is_loading(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewBool(ctx, loading ? 1 : 0);
	}

	JSValue js_sam_random_float(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( !samHasArg(argc, argv, 0) ) { SAM_ERROR("JS", "sam_random_float: argument 1 (the stream name) is required."); return JS_UNDEFINED; }
		const char* streamC = JS_ToCString(ctx, argv[0]);
		const std::string stream = streamC ? streamC : "";
		if ( streamC ) { JS_FreeCString(ctx, streamC); }
		const long long v = SAMLua::randomDraw(g_currentNs, stream, 0, 1000000);
		return JS_NewFloat64(ctx, (double)v / 1000000.0);
	}

	JSValue js_sam_random_chance(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( !samHasArg(argc, argv, 0) ) { SAM_ERROR("JS", "sam_random_chance: argument 1 (the stream name) is required."); return JS_FALSE; }
		const char* streamC = JS_ToCString(ctx, argv[0]);
		const std::string stream = streamC ? streamC : "";
		if ( streamC ) { JS_FreeCString(ctx, streamC); }
		double pct = 0.0;
		if ( !samJsReqF64(ctx, argc, argv, 1, &pct, "sam_random_chance") ) { return JS_FALSE; }
		if ( pct <= 0.0 ) { return JS_FALSE; }
		if ( pct >= 100.0 ) { return JS_TRUE; }
		const long long v = SAMLua::randomDraw(g_currentNs, stream, 1, 1000000);
		return JS_NewBool(ctx, ((double)v <= pct * 10000.0) ? 1 : 0);
	}

	// Indexes from 0 here and from 1 in Lua. See the header comment: each follows its own
	// language rather than one being a literal port of the other.
	JSValue js_sam_random_from_list(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( !samHasArg(argc, argv, 0) ) { SAM_ERROR("JS", "sam_random_from_list: argument 1 (the stream name) is required."); return JS_UNDEFINED; }
		const char* streamC = JS_ToCString(ctx, argv[0]);
		const std::string stream = streamC ? streamC : "";
		if ( streamC ) { JS_FreeCString(ctx, streamC); }
		if ( !samHasArg(argc, argv, 1) || !JS_IsObject(argv[1]) ) { return JS_UNDEFINED; }
		uint32_t n = 0;
		JSValue lenv = JS_GetPropertyStr(ctx, argv[1], "length");
		JS_ToUint32(ctx, &n, lenv);
		JS_FreeValue(ctx, lenv);
		if ( n == 0 ) { return JS_UNDEFINED; }
		const long long pick = SAMLua::randomDraw(g_currentNs, stream, 0, (long long)n - 1);
		return JS_GetPropertyUint32(ctx, argv[1], (uint32_t)pick);
	}

	JSValue js_sam_random_weighted(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( !samHasArg(argc, argv, 0) ) { SAM_ERROR("JS", "sam_random_weighted: argument 1 (the stream name) is required."); return JS_UNDEFINED; }
		const char* streamC = JS_ToCString(ctx, argv[0]);
		const std::string stream = streamC ? streamC : "";
		if ( streamC ) { JS_FreeCString(ctx, streamC); }
		if ( !samHasArg(argc, argv, 1) || !JS_IsObject(argv[1]) ) { return JS_UNDEFINED; }

		JSPropertyEnum* props = nullptr;
		uint32_t count = 0;
		if ( JS_GetOwnPropertyNames(ctx, &props, &count, argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0 )
		{
			return JS_UNDEFINED;
		}
		// Frees every atom and the table itself, on every exit path below.
		auto releaseProps = [&]() {
			for ( uint32_t i = 0; i < count; ++i ) { JS_FreeAtom(ctx, props[i].atom); }
			js_free(ctx, props);
		};

		// SORTED, like the Lua twin. JS property order is insertion order and therefore
		// stable within one process, but it is not the same order Lua would produce, and the
		// two runtimes must agree with each other as well as with themselves.
		std::vector<std::pair<std::string, double>> entries;
		double total = 0.0;
		for ( uint32_t i = 0; i < count; ++i )
		{
			JSValue v = JS_GetProperty(ctx, argv[1], props[i].atom);
			double w = 0.0; JS_ToFloat64(ctx, &w, v);
			JS_FreeValue(ctx, v);
			if ( w > 0.0 )
			{
				const char* k = JS_AtomToCString(ctx, props[i].atom);
				if ( k ) { entries.emplace_back(k, w); JS_FreeCString(ctx, k); total += w; }
			}
		}
		releaseProps();     // the atoms are no longer needed; the keys are copied out
		if ( entries.empty() || total <= 0.0 ) { return JS_UNDEFINED; }
		std::sort(entries.begin(), entries.end(),
			[](const std::pair<std::string, double>& a, const std::pair<std::string, double>& b)
			{ return a.first < b.first; });

		// [1, 999999], never the inclusive top: at exactly total, float residue left every
		// branch untaken and a valid table returned undefined.
		const long long draw = SAMLua::randomDraw(g_currentNs, stream, 1, 999999);
		double target = total * ((double)draw / 1000000.0);
		for ( const auto& e : entries )
		{
			target -= e.second;
			if ( target <= 0.0 ) { return JS_NewString(ctx, e.first.c_str()); }
		}
		return JS_NewString(ctx, entries.back().first.c_str());
	}

	JSValue js_sam_has_data(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		const char* keyC = samHasArg(argc, argv, 0) ? JS_ToCString(ctx, argv[0]) : nullptr;
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( g_currentNs.empty() || key.empty() ) { return JS_FALSE; }
		// Through samModDataFile, which sanitizes, exactly as every writer does. Building the
		// path by hand looked for a file no writer ever creates. See the Lua twin.
		const std::string path = samModDataFile(g_currentNs, key);
		std::error_code ec;
		return JS_NewBool(ctx, (std::filesystem::exists(path, ec) && !ec) ? 1 : 0);
	}

	JSValue js_sam_world_bytes(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		return JS_NewInt64(ctx, (int64_t)SAMWorldState::totalBytes());
	}

	JSValue js_sam_world_bytes_free(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		const long long used = (long long)SAMWorldState::totalBytes();
		const long long cap = (long long)SAMWorldState::kMaxTotalBytes;
		return JS_NewInt64(ctx, (int64_t)(used >= cap ? 0 : cap - used));
	}

	
	// ============================================================================
	// v2.6 batch 2: inventory and items. Twins of the Lua bindings; the reasoning for
	// each lives in sam_lua_runtime.cpp. The same two rules apply:
	//   Every uid goes through SAMMpInventory::resolveItem (this machine's own players' items,
	//   plus, for a reader on the host, the mirror of each remote player's backpack).
	//   Every write is an `owner` call: done here for this machine's items, carried to the
	//   owner's machine for anyone else's, and echoed to the host's worn copy from there.
	// The only language difference is that a Lua table comes back as a JS object.
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

	static const char* samJsItemStatusName(int st)
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

	static bool samJsItemStatusFromName(const std::string& n, int& out)
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

	static const char* samJsItemSlotName(int slot)
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

	// Resolve the item uid in argv[idx]. See the Lua twins samItemFromUid / samItemForWrite:
	// this machine's own players' items, plus, for a READER on the host, the mirror of each
	// remote player's backpack. A missing or non-numeric argument is refused before any lookup
	// (uid 0 used to be a real lookup that could return a real item), the way the Lua twin
	// refuses when luaL_checkinteger raises.
	static Item* samJsItemResolve(JSContext* ctx, int argc, JSValueConst* argv, int idx,
		SAMMpInventory::Use use, const char* fn, int* holder)
	{
		if ( holder ) { *holder = -1; }
		double d = 0.0;
		if ( !samHasArg(argc, argv, idx) ) { return nullptr; }
		if ( !samJsNum(ctx, argv[idx], &d) ) { return nullptr; }
		return SAMMpInventory::resolveItem((long long)d, use, fn, holder);
	}
	static Item* samJsItemFromUid(JSContext* ctx, int argc, JSValueConst* argv, int idx, const char* fn, int* holder = nullptr)
	{
		return samJsItemResolve(ctx, argc, argv, idx, SAMMpInventory::Use::Read, fn, holder);
	}
	static Item* samJsItemForWrite(JSContext* ctx, int argc, JSValueConst* argv, int idx, const char* fn, int* holder = nullptr)
	{
		return samJsItemResolve(ctx, argc, argv, idx, SAMMpInventory::Use::Write, fn, holder);
	}

	JSValue js_sam_get_item(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_get_item");
		if ( !it ) { return JS_UNDEFINED; }
		JSValue o = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, o, "type",        JS_NewInt32(ctx, (int)it->type));
		JS_SetPropertyStr(ctx, o, "count",       JS_NewInt32(ctx, (int)it->count));
		JS_SetPropertyStr(ctx, o, "beatitude",   JS_NewInt32(ctx, (int)it->beatitude));
		JS_SetPropertyStr(ctx, o, "status",      JS_NewInt32(ctx, (int)it->status));
		JS_SetPropertyStr(ctx, o, "status_name", JS_NewString(ctx, samJsItemStatusName((int)it->status)));
		JS_SetPropertyStr(ctx, o, "identified",  JS_NewBool(ctx, it->identified ? 1 : 0));
		JS_SetPropertyStr(ctx, o, "appearance",  JS_NewInt64(ctx, (int64_t)it->appearance));
		JS_SetPropertyStr(ctx, o, "owner_uid",   JS_NewInt64(ctx, (int64_t)it->ownerUid));
		JS_SetPropertyStr(ctx, o, "droppable",   JS_NewBool(ctx, it->isDroppable ? 1 : 0));
		JS_SetPropertyStr(ctx, o, "grid_x",      JS_NewInt32(ctx, (int)it->x));
		JS_SetPropertyStr(ctx, o, "grid_y",      JS_NewInt32(ctx, (int)it->y));
		return o;
	}

	JSValue js_sam_get_item_name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_get_item_name");
		if ( !it ) { return JS_UNDEFINED; }
		// getName writes into a shared buffer; JS_NewString copies it here and now.
		const char* n = it->getName();
		return JS_NewString(ctx, n ? n : "");
	}

	JSValue js_sam_get_item_value(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_get_item_value");
		if ( !it ) { return JS_UNDEFINED; }
		// Per unit in the engine; multiplied here. See the Lua twin.
		return JS_NewInt64(ctx, (int64_t)it->getGoldValue() * (int64_t)(it->count > 0 ? it->count : 1));
	}

	JSValue js_sam_get_item_weight(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_get_item_weight");
		if ( !it ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, (int)it->getWeight());
	}

	JSValue js_sam_get_item_attack(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_get_item_attack");
		if ( !it ) { return JS_UNDEFINED; }
		const Stat* w = nullptr;
		int32_t p = -1;
		// A player that is given but is not a number is refused, as luaL_checkinteger raises on
		// it in Lua. It used to be ignored here, and the base value came back as if no player
		// had been named.
		if ( samHasArg(argc, argv, 1) && !samJsReqI32(ctx, argc, argv, 1, &p, "sam_get_item_attack") ) { return JS_UNDEFINED; }
		if ( p >= 0 && p < MAXPLAYERS ) { w = stats[p]; }
		return JS_NewInt32(ctx, (int)it->weaponGetAttack(w));
	}

	JSValue js_sam_get_item_ac(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_get_item_ac");
		if ( !it ) { return JS_UNDEFINED; }
		const Stat* w = nullptr;
		int32_t p = -1;
		// See sam_get_item_attack: a non-numeric player is refused, not ignored.
		if ( samHasArg(argc, argv, 1) && !samJsReqI32(ctx, argc, argv, 1, &p, "sam_get_item_ac") ) { return JS_UNDEFINED; }
		if ( p >= 0 && p < MAXPLAYERS ) { w = stats[p]; }
		return JS_NewInt32(ctx, (int)it->armorGetAC(w));
	}

	JSValue js_sam_get_tome_spell(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_get_tome_spell");
		if ( !it ) { return JS_FALSE; }   // cannot see it: not the same as "teaches no spell"
		// See the Lua twin: branch on category, and undefined for no spell so both runtimes agree.
		int spellID = SPELL_NONE;
		if ( itemCategory(it) == SPELLBOOK )        { spellID = getSpellIDFromSpellbook(it->type); }
		else if ( itemCategory(it) == TOME_SPELL )  { spellID = it->getTomeSpellID(); }
		if ( spellID == SPELL_NONE ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, spellID);
	}

	JSValue js_sam_get_food_satiation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t t = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &t, "sam_get_food_satiation") ) { return JS_UNDEFINED; }
		if ( t < 0 || t >= NUM_ITEM_SLOTS ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, Item::getBaseFoodSatiation((ItemType)t));
	}

	// ---- writes (host only) ----------------------------------------------------

	JSValue js_sam_set_item_beatitude(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, and it must really be a number: a forgotten argument wrote 0 and stripped
		// a curse or a blessing, and so did any value QuickJS coerced to 0 for us.
		int32_t v = 0;
		if ( !samJsReqI32(ctx, argc, argv, 1, &v, "sam_set_item_beatitude") ) { return JS_FALSE; }
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("JS", "sam_set_item_beatitude refused: host only."); return JS_FALSE; }
		Item* it = samJsItemForWrite(ctx, argc, argv, 0, "sam_set_item_beatitude");
		if ( !it ) { return JS_FALSE; }
		// -100..100, matching newItem and sam_spawn_item.
		it->beatitude = (Sint16)((v < -100) ? -100 : ((v > 100) ? 100 : v));
		SAMMpInventory::afterOwnerWrite(it);   // a worn item's copy on the host: see the Lua twin
		return JS_TRUE;
	}

	JSValue js_sam_set_item_status(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Seeded OUTSIDE the enum. 0 is BROKEN (items.hpp:588), so a forgotten argument used
		// to break the item and a `st < BROKEN` clamp could never notice.
		int st = -1;
		if ( !samHasArg(argc, argv, 1) )
		{
			SAM_ERROR("JS", "sam_set_item_status: the status argument is required.");
			return JS_FALSE;
		}
		if ( JS_IsString(argv[1]) )
		{
			const char* c = JS_ToCString(ctx, argv[1]);
			const std::string name = c ? c : "";
			if ( c ) { JS_FreeCString(ctx, c); }
			if ( !samJsItemStatusFromName(name, st) )
			{
				SAM_ERROR("JS", "sam_set_item_status: unknown status. Valid: BROKEN, DECREPIT, WORN, SERVICABLE, EXCELLENT.");
				return JS_FALSE;
			}
		}
		else
		{
			int32_t n = 0;
			if ( !samJsReqI32(ctx, argc, argv, 1, &n, "sam_set_item_status") ) { return JS_FALSE; }
			st = (int)n;
		}
		// Refused, not clamped, and refused in BOTH runtimes: the Lua twin used to clamp a
		// negative up to BROKEN and answer true while this one answered false, so the same
		// script reported two different outcomes depending on the language it was written in.
		if ( st < BROKEN || st > EXCELLENT )
		{
			SAM_ERROR("JS", "sam_set_item_status: status must be 0 (BROKEN) to 4 (EXCELLENT), or a name.");
			return JS_FALSE;
		}
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("JS", "sam_set_item_status refused: host only."); return JS_FALSE; }
		Item* it = samJsItemForWrite(ctx, argc, argv, 0, "sam_set_item_status");
		if ( !it ) { return JS_FALSE; }
		it->status = (Status)st;
		SAMMpInventory::afterOwnerWrite(it);
		return JS_TRUE;
	}

	JSValue js_sam_set_item_count(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Zero is the DESTRUCTIVE branch here, so anything that silently becomes zero deletes
		// the item: a forgotten argument, and equally "two" or {} or NaN, all of which
		// JS_ToInt32 would have converted for us without complaint. Lua raises on every one
		// of them; JS has to be told.
		int32_t n = 0;
		if ( !samJsReqI32(ctx, argc, argv, 1, &n, "sam_set_item_count") ) { return JS_FALSE; }
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("JS", "sam_set_item_count refused: host only."); return JS_FALSE; }
		int holder = -1;
		Item* it = samJsItemForWrite(ctx, argc, argv, 0, "sam_set_item_count", &holder);
		if ( !it ) { return JS_FALSE; }
		const int owner = holder >= 0 ? holder : clientnum;
		// Queued, never immediate: the engine frame that dispatched this event still holds
		// the pointer. See SAMItems::queueDestroy, and the Lua twin for where it drains.
		if ( n <= 0 )
		{
			const bool queued = SAMItems::queueDestroy((Uint32)it->uid, owner);
			if ( !queued ) { SAMMpInventory::noteOwnerRefusal("sam_set_item_count", "that item is equipped; unequip it before destroying it.", false, "equipped"); }
			// A destroy has no item left to echo: see the Lua twin.
			else { SAMMpInventory::nudgeReport(); }
			return JS_NewBool(ctx, queued ? 1 : 0);
		}
		const int cap = it->getMaxStackLimit(owner);
		if ( n > cap )
		{
			const std::string why = std::to_string((int)n) + " exceeds this item's"
				" stack limit of " + std::to_string(cap) + "; refused. Use sam_get_max_stack to check first.";
			SAM_WARN("JS", "sam_set_item_count: " + why);
			SAMMpInventory::noteOwnerRefusal("sam_set_item_count", why, false, "stacklimit");
			return JS_FALSE;
		}
		it->count = (Sint16)n;
		SAMMpInventory::afterOwnerWrite(it);
		return JS_TRUE;
	}

	JSValue js_sam_identify_item(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger requires it: a missing player used to fall to -1 and
		// fail silently.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_identify_item") ) { return JS_FALSE; }
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("JS", "sam_identify_item refused: host only."); return JS_FALSE; }
		if ( player < 0 || player >= MAXPLAYERS ) { return JS_FALSE; }
		int holder = -1;
		Item* it = samJsItemForWrite(ctx, argc, argv, 1, "sam_identify_item", &holder);
		if ( !it ) { return JS_FALSE; }
		// `player` must be the holder, but only when the item HAS one. holder == -1 means the
		// item is on the floor, in a chest or in a shop, not that it is somebody else's. See
		// the Lua twin for the whole reason.
		if ( holder >= 0 && holder != (int)player )
		{
			double d = 0.0;
			samJsNum(ctx, argv[1], &d);
			SAMMpInventory::noteOwnerRefusal("sam_identify_item", "uid " + std::to_string((long long)d) + " is player "
				+ std::to_string(holder) + "'s item, not player " + std::to_string((int)player) + "'s; nothing identified.", true, "notyours");
			return JS_FALSE;
		}
		if ( it->identified ) { return JS_TRUE; }
		it->identified = true;
		Item::onItemIdentified((int)player, it);
		SAMMpInventory::afterOwnerWrite(it);
		return JS_TRUE;
	}

	JSValue js_sam_set_item_appearance(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t ap = -1;
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &ap, "sam_set_item_appearance") ) { return JS_FALSE; }
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("JS", "sam_set_item_appearance refused: host only."); return JS_FALSE; }
		Item* it = samJsItemForWrite(ctx, argc, argv, 0, "sam_set_item_appearance");
		if ( !it || ap < 0 ) { return JS_FALSE; }
		// See the Lua twin: appearance carries gameplay state on several types and is saved.
		if ( samItemAppearanceIsGameplay((int)it->type) )
		{
			SAM_ERROR("JS", "sam_set_item_appearance: this item type stores gameplay data in"
				" its appearance (a tome's spell, a loot bag's contents, a robot's HP, a"
				" scepter's charges). Refused, because the change would be saved.");
			SAMMpInventory::noteOwnerRefusal("sam_set_item_appearance", "this item type stores gameplay data in its appearance; refused.", false, "appearance");
			return JS_FALSE;
		}
		it->appearance = (Uint32)ap;
		SAMMpInventory::afterOwnerWrite(it);
		return JS_TRUE;
	}

	JSValue js_sam_set_item_droppable(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required: defaulting to false made a forgotten argument silently pin the item in
		// place forever. See the Lua twin.
		if ( !samHasArg(argc, argv, 1) )
		{
			SAM_ERROR("JS", "sam_set_item_droppable: the true/false argument is required.");
			return JS_FALSE;
		}
		const bool d = ( JS_ToBool(ctx, argv[1]) != 0 );
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("JS", "sam_set_item_droppable refused: host only."); return JS_FALSE; }
		Item* it = samJsItemForWrite(ctx, argc, argv, 0, "sam_set_item_droppable");
		if ( !it ) { return JS_FALSE; }
		it->isDroppable = d;
		SAMMpInventory::afterOwnerWrite(it);   // the host's worn copy: see the Lua twin
		return JS_TRUE;
	}

	JSValue js_sam_get_item_owner(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_get_item_owner");
		if ( !it ) { return JS_FALSE; }                  // cannot see it: not "nobody owns it"
		if ( it->ownerUid == 0 ) { return JS_UNDEFINED; }   // matches the Lua twin
		return JS_NewInt64(ctx, (int64_t)it->ownerUid);
	}

	JSValue js_sam_set_item_owner(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t owner = -1;
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &owner, "sam_set_item_owner") ) { return JS_FALSE; }
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("JS", "sam_set_item_owner refused: host only."); return JS_FALSE; }
		Item* it = samJsItemForWrite(ctx, argc, argv, 0, "sam_set_item_owner");
		if ( !it || owner < 0 ) { return JS_FALSE; }
		it->ownerUid = (Uint32)owner;
		SAMMpInventory::afterOwnerWrite(it);   // on the owner's machine: re-report the backpack now
		return JS_TRUE;
	}

	// ---- predicates ------------------------------------------------------------

	JSValue js_sam_is_ranged_weapon(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua. false is a real answer here, so a
		// missing argument answers nothing at all instead.
		int32_t t = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &t, "sam_is_ranged_weapon") ) { return JS_UNDEFINED; }
		if ( t < 0 || t >= NUM_ITEM_SLOTS ) { return JS_FALSE; }
		return JS_NewBool(ctx, isRangedWeapon((ItemType)t) ? 1 : 0);
	}

	JSValue js_sam_is_melee_weapon(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_is_melee_weapon");
		if ( !it ) { return JS_UNDEFINED; }   // cannot see it: neither yes nor no
		return JS_NewBool(ctx, isMeleeWeapon(*it) ? 1 : 0);
	}

	JSValue js_sam_is_shield(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_is_shield");
		if ( !it ) { return JS_UNDEFINED; }   // cannot see it: neither yes nor no
		// isItemEquippableInShieldSlot: isShield excludes lanterns, torches, quivers and
		// spellbooks, which are exactly the offhand cases a mod asks about.
		return JS_NewBool(ctx, isItemEquippableInShieldSlot(it) ? 1 : 0);
	}

	JSValue js_sam_is_potion_bad(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* it = samJsItemFromUid(ctx, argc, argv, 0, "sam_is_potion_bad");
		if ( !it ) { return JS_UNDEFINED; }   // cannot see it: neither yes nor no
		return JS_NewBool(ctx, isPotionBad(*it) ? 1 : 0);
	}

	JSValue js_sam_item_has_trait(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua. false is a real answer here, so a
		// missing argument answers nothing at all instead.
		int32_t t = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &t, "sam_item_has_trait") ) { return JS_UNDEFINED; }
		const char* c = samHasArg(argc, argv, 1) ? JS_ToCString(ctx, argv[1]) : nullptr;
		std::string u;
		for ( const char* p = c; p && *p; ++p ) { u += (char)toupper((unsigned char)*p); }
		if ( c ) { JS_FreeCString(ctx, c); }
		if ( t < 0 || t >= NUM_ITEM_SLOTS ) { return JS_FALSE; }
		const ItemType ty = (ItemType)t;
		// All ELEVEN traits a mod may declare. See the Lua twin: the bit a custom item declared
		// (ItemGeneric::samTraits) OR the engine's predicate for a vanilla one. Four of these had
		// no reader anywhere before.
		const Uint64 declared = ::items[t].samTraits;
		if ( u == "RANGED" )           { return JS_NewBool(ctx, (( declared & SAMItemTrait::RANGED ) || isRangedWeapon(ty)) ? 1 : 0); }
		if ( u == "QUIVER" )           { return JS_NewBool(ctx, (( declared & SAMItemTrait::QUIVER ) || itemTypeIsQuiver(ty)) ? 1 : 0); }
		if ( u == "FOCI" )             { return JS_NewBool(ctx, (( declared & SAMItemTrait::FOCI ) || itemTypeIsFoci(ty)) ? 1 : 0); }
		if ( u == "INSTRUMENT" )       { return JS_NewBool(ctx, (( declared & SAMItemTrait::INSTRUMENT ) || itemTypeIsInstrument(ty)) ? 1 : 0); }
		if ( u == "THROWN_BALL" )      { return JS_NewBool(ctx, (( declared & SAMItemTrait::THROWN_BALL ) || itemTypeIsThrownBall(ty)) ? 1 : 0); }
		if ( u == "SHIELD_SLOT" )      { return JS_NewBool(ctx, ( declared & SAMItemTrait::SHIELD_SLOT ) ? 1 : 0); }
		if ( u == "POTION_BAD" )       { return JS_NewBool(ctx, ( declared & SAMItemTrait::POTION_BAD ) ? 1 : 0); }
		if ( u == "AUTOMATON_FOOD" )   { return JS_NewBool(ctx, ( declared & SAMItemTrait::AUTOMATON_FOOD ) ? 1 : 0); }
		if ( u == "TINKER_THROWABLE" ) { return JS_NewBool(ctx, ( declared & SAMItemTrait::TINKER_THROWABLE ) ? 1 : 0); }
		if ( u == "USABLE" )           { return JS_NewBool(ctx, ( declared & SAMItemTrait::USABLE ) ? 1 : 0); }
		if ( u == "BEATITUDE_AC" )     { return JS_NewBool(ctx, ( declared & SAMItemTrait::BEATITUDE_AC ) ? 1 : 0); }
		SAM_ERROR("JS", "sam_item_has_trait: unknown trait '" + u + "'. Valid: RANGED, QUIVER, FOCI,"
			" INSTRUMENT, THROWN_BALL, SHIELD_SLOT, POTION_BAD, AUTOMATON_FOOD, TINKER_THROWABLE,"
			" USABLE, BEATITUDE_AC.");
		return JS_FALSE;
	}

	JSValue js_sam_get_item_slot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t t = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &t, "sam_get_item_slot") ) { return JS_UNDEFINED; }
		if ( t < 0 || t >= NUM_ITEM_SLOTS ) { return JS_UNDEFINED; }
		return JS_NewString(ctx, samJsItemSlotName((int)items[t].item_slot));
	}

	JSValue js_sam_is_better_weapon(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* a = samJsItemFromUid(ctx, argc, argv, 0, "sam_is_better_weapon");
		const bool hasB = samHasArg(argc, argv, 1);
		Item* b = hasB ? samJsItemFromUid(ctx, argc, argv, 1, "sam_is_better_weapon") : nullptr;
		// See the Lua twin: a given-but-unresolvable second uid is not "nothing".
		if ( !a || ( hasB && !b ) ) { return JS_UNDEFINED; }
		// See the Lua twin: without a category test this says yes to anything.
		if ( !b && itemCategory(a) != WEAPON ) { return JS_FALSE; }
		return JS_NewBool(ctx, Item::isThisABetterWeapon(*a, b) ? 1 : 0);
	}

	JSValue js_sam_is_better_armor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		Item* a = samJsItemFromUid(ctx, argc, argv, 0, "sam_is_better_armor");
		const bool hasB = samHasArg(argc, argv, 1);
		Item* b = hasB ? samJsItemFromUid(ctx, argc, argv, 1, "sam_is_better_armor") : nullptr;
		// See the Lua twin: a given-but-unresolvable second uid is not "nothing".
		if ( !a || ( hasB && !b ) ) { return JS_UNDEFINED; }
		// isThisABetterArmor short-circuits to TRUE the moment there is nothing to compare
		// against (items.cpp:7215) and never asks whether the new item is armour at all, so
		// the documented "is this worth wearing" form said yes to a potion. The engine only
		// ever reaches that function through checkEquipType, which has already routed the
		// item into one of these seven slots (entity.cpp:26149-26228). That routing is the
		// guard, and it is the same short-circuit already fixed in sam_is_better_weapon.
		if ( !b )
		{
			const int t = (int)a->type;
			if ( t < 0 || t >= NUM_ITEM_SLOTS ) { return JS_FALSE; }
			const int slot = (int)items[t].item_slot;
			if ( slot != EQUIPPABLE_IN_SLOT_SHIELD && slot != EQUIPPABLE_IN_SLOT_MASK
				&& slot != EQUIPPABLE_IN_SLOT_HELM && slot != EQUIPPABLE_IN_SLOT_GLOVES
				&& slot != EQUIPPABLE_IN_SLOT_BOOTS && slot != EQUIPPABLE_IN_SLOT_BREASTPLATE
				&& slot != EQUIPPABLE_IN_SLOT_CLOAK )
			{
				return JS_FALSE;
			}
		}
		return JS_NewBool(ctx, Item::isThisABetterArmor(*a, b) ? 1 : 0);
	}

	// ---- queries ---------------------------------------------------------------

	JSValue js_sam_is_item_equipped(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_is_item_equipped") ) { return JS_FALSE; }
		Item* it = samJsItemFromUid(ctx, argc, argv, 1, "sam_is_item_equipped");
		if ( !it ) { return JS_UNDEFINED; }   // cannot see it: not the same as "not worn"
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_FALSE; }
		// By POINTER, through the helper sam_get_inventory's `equipped` uses. See the Lua twin.
		return JS_NewBool(ctx, SAMMpInventory::isEquipped((int)player, it) ? 1 : 0);
	}

	JSValue js_sam_can_unequip(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_can_unequip") ) { return JS_FALSE; }
		Item* it = samJsItemFromUid(ctx, argc, argv, 1, "sam_can_unequip");
		if ( !it ) { return JS_UNDEFINED; }   // cannot see it: false would read as "cursed"
		if ( player < 0 || player >= MAXPLAYERS ) { return JS_FALSE; }
		// NOT Item::canUnequip: it identifies the item as a side effect. See the Lua twin.
		return JS_NewBool(ctx, samItemCanUnequipQuiet(it, stats[player]) ? 1 : 0);
	}

	JSValue js_sam_inventory_has_space(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_inventory_has_space") ) { return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_UNDEFINED; }
		if ( !players[player]->isLocalPlayer() )
		{
			SAM_WARN("JS", "sam_inventory_has_space: the inventory grid is local to each machine;"
				" a remote player cannot be asked. Returning undefined.");
			return JS_UNDEFINED;
		}
		return JS_NewBool(ctx, players[player]->inventoryUI.bItemInventoryHasFreeSlot() ? 1 : 0);
	}

	JSValue js_sam_get_max_stack(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_max_stack") ) { return JS_UNDEFINED; }
		Item* it = samJsItemFromUid(ctx, argc, argv, 1, "sam_get_max_stack");
		if ( !it || player < 0 || player >= MAXPLAYERS ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, it->getMaxStackLimit((int)player));
	}

	JSValue js_sam_can_items_stack(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_can_items_stack") ) { return JS_FALSE; }
		Item* a = samJsItemFromUid(ctx, argc, argv, 1, "sam_can_items_stack");
		Item* b = samJsItemFromUid(ctx, argc, argv, 2, "sam_can_items_stack");
		if ( !a || !b ) { return JS_UNDEFINED; }   // cannot see one: not "they would not merge"
		if ( player < 0 || player >= MAXPLAYERS ) { return JS_FALSE; }
		// The engine's own comparison, a != b, and the ceiling on the DESTINATION.
		return JS_NewBool(ctx, ( a != b && itemCompare(a, b, false) == 0
			&& b->shouldItemStack((int)player) ) ? 1 : 0);
	}

	JSValue js_sam_monster_can_wield(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Both required, as luaL_checkinteger makes them in Lua. false is a real answer here,
		// so a missing argument answers nothing at all instead.
		int64_t muid = 0; int32_t t = -1;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &muid, "sam_monster_can_wield") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &t, "sam_monster_can_wield") ) { return JS_UNDEFINED; }
		// See the Lua twin: a backstop behind the trampoline, answering what the refusal answers.
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_monster_can_wield refused: host only."); return JS_UNDEFINED; }
		Entity* e = samResolveMonster((long long)muid);
		if ( !e || t < 0 || t >= NUM_ITEM_SLOTS ) { return JS_FALSE; }
		// Value-initialised: Item has no constructor, so a plain declaration would leave
		// several members holding whatever was on the stack.
		Item probe{};
		probe.type = (ItemType)t;
		probe.status = EXCELLENT;
		probe.beatitude = 0;
		probe.count = 1;
		probe.appearance = 0;
		probe.identified = true;
		return JS_NewBool(ctx, e->canWieldItem(probe) ? 1 : 0);
	}

	JSValue js_sam_get_position(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua: a missing uid used to be answered in silence.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_position") ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_UNDEFINED; }
		JSValue arr = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, (int)e->x >> 4));
		JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, (int)e->y >> 4));
		return arr;
	}

	// sam_set_position(uid, tileX, tileY) -> boolean. Players via the safe teleport()
	// path; other entities via x/y + UPDATENEEDED. Host only.
	JSValue js_sam_set_position(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// All three arguments are REQUIRED. They were read with the optional helpers, so
		// sam_set_position(uid) quietly meant tile (0, 0) -- a real destination, in the corner of
		// every map.
		int64_t uid = 0; int32_t tx = 0, ty = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_position") ) { return JS_NewBool(ctx, 0); }
		if ( !samJsReqI32(ctx, argc, argv, 1, &tx, "sam_set_position") ) { return JS_NewBool(ctx, 0); }
		if ( !samJsReqI32(ctx, argc, argv, 2, &ty, "sam_set_position") ) { return JS_NewBool(ctx, 0); }
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_set_position");
		if ( !e ) { return JS_NewBool(ctx, 0); }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("JS", "sam_set_position: tile out of bounds."); return JS_NewBool(ctx, 0); }
		if ( e->behavior == &actPlayer )
		{
			const bool ok = e->teleport(tx, ty);
			if ( ok ) { SAMMpEntities::teleported(e); }   // see the Lua twin
			return JS_NewBool(ctx, ok ? 1 : 0);
		}
		// Warns and still places; see the Lua twin for why refusing would break the deliberate case.
		{
			const hit_t samSavedHit = hit;
			const bool blocked = ( checkObstacle((tx << 4) + 8, (ty << 4) + 8, e, nullptr) != 0 );
			hit = samSavedHit;
			if ( blocked )
			{
				SAM_WARN("JS", "sam_set_position: that tile is blocked for this entity, so it is"
					" being placed inside something. Fine for a decoration; a monster put there"
					" cannot get out. sam_can_stand(uid, x, y) answers this before you move"
					" anything.");
			}
		}
		e->x = (double)(tx * 16 + 8);
		e->y = (double)(ty * 16 + 8);
		e->flags[UPDATENEEDED] = true;
		e->flags[NOUPDATE] = false;
		TileEntityList.updateEntity(*e);
		SAMMpEntities::moved(e);   // see the Lua twin
		return JS_NewBool(ctx, 1);
	}

	// sam_spawn_monster(tileX, tileY, "name" [, shopType]) -> uid | undefined. Host only.
	// Summon a mod-declared monster variant by its "ns:slug" id; nullptr if never declared.
	// Hands off to createMonsterFromFile -- the SAME routine level generation uses
	// (maps.cpp:8003) -- so stats, equipment, traits, body model and followers are applied
	// exactly as on a generated one. Before this a script could only summon a vanilla
	// species and then hand-patch it, which never reproduced the body or the followers.
	Entity* samSummonCustomVariantJs(const std::string& id, int tx, int ty)
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

	JSValue js_sam_spawn_monster(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t tx = 0, ty = 0; std::string monName;
		// REQUIRED, as luaL_checkinteger makes them in Lua: a missing tile used to mean (0, 0).
		if ( !samJsReqI32(ctx, argc, argv, 0, &tx, "sam_spawn_monster") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &ty, "sam_spawn_monster") ) { return JS_UNDEFINED; }
		if ( samHasArg(argc, argv, 2) ) { const char* s = JS_ToCString(ctx, argv[2]); if ( s ) { monName = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_monster refused: host only."); return JS_UNDEFINED; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("JS", "sam_spawn_monster: tile out of bounds."); return JS_UNDEFINED; }

		// A name containing ':' is a mod's OWN monster; anything else is a vanilla species.
		Entity* e = nullptr;
		int creature = 0;
		if ( monName.find(':') != std::string::npos )
		{
			e = samSummonCustomVariantJs(monName, tx, ty);
			if ( !e )
			{
				SAM_ERROR("JS", "sam_spawn_monster: no monster declared as '" + monName
					+ "' (check the id against the mod's monster JSON, and that the mod loaded).");
				return JS_UNDEFINED;
			}
		}
		else
		{
			creature = samMonsterNameToId(monName.c_str());
			if ( creature <= 0 ) { SAM_ERROR("JS", "sam_spawn_monster: unknown monster '" + monName + "'."); return JS_UNDEFINED; }
			e = summonMonster(static_cast<Monster>(creature), tx * 16 + 8, ty * 16 + 8);
		}
		if ( !e ) { SAM_ERROR("JS", "sam_spawn_monster: spawn failed (blocked tile?)."); return JS_UNDEFINED; }
		if ( samHasArg(argc, argv, 3) && creature == SHOPKEEPER )
		{
			int32_t shopType = 0; JS_ToInt32(ctx, &shopType, argv[3]);
			if ( shopType < 0 ) { shopType = 0; }
			if ( shopType > 14 ) { shopType = 14; }
			if ( Stat* s = e->getStats() ) { s->MISC_FLAGS[STAT_FLAG_NPC] = 1 + shopType; }
		}
		SAM_INFO("JS", "Spawned monster " + monName + " at (" + std::to_string(tx) + "," + std::to_string(ty) + ")");
		return JS_NewInt64(ctx, (int64_t)e->getUID());
	}

	// sam_spawn_portal(tileX, tileY) -> uid | undefined. A purely-DECORATIVE, walkable portal
	// (swirling vortex, sprite 254): animates + glows but is never interactive and never
	// descends anyone (see the skill[15] guard in actPortal). Returns its uid so a script
	// can move it (sam_set_position) or clear it (sam_remove_entity). Host only. Twin of
	// the Lua binding — keep the two in lock-step.
	JSValue js_sam_spawn_portal(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t tx = 0, ty = 0;
		// REQUIRED, as luaL_checkinteger makes them in Lua: a missing tile used to mean (0, 0).
		if ( !samJsReqI32(ctx, argc, argv, 0, &tx, "sam_spawn_portal") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &ty, "sam_spawn_portal") ) { return JS_UNDEFINED; }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_portal refused: host only."); return JS_UNDEFINED; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("JS", "sam_spawn_portal: tile out of bounds."); return JS_UNDEFINED; }
		Entity* e = newEntity(254, 1, map.entities, nullptr);
		if ( !e ) { SAM_ERROR("JS", "sam_spawn_portal: entity creation failed."); return JS_UNDEFINED; }
		e->x = tx * 16 + 8;
		e->y = ty * 16 + 8;
		e->z = 0;
		e->sprite = 254;
		e->sizex = 4;
		e->sizey = 4;
		e->yaw = 1.5707963267948966;   // PI/2
		e->flags[PASSABLE] = true;
		e->behavior = &actPortal;
		e->skill[19] = 1;              // S.A.M decorative marker (guard in actPortal); skill[19]/[20] are outside the portal alias range
		SAMMpEntities::adopt(e, SAMMpEntities::Kind::Portal);   // every player sees it; see the Lua twin
		SAM_INFO("JS", "sam_spawn_portal: decorative portal at (" + std::to_string(tx) + "," + std::to_string(ty) + ")");
		return JS_NewInt64(ctx, (int64_t)e->getUID());
	}

	// sam_remove_entity(uid) -> bool. Remove a non-player world entity by uid (portal
	// marker, spawned monster, ground item...). Refuses players. Frees any light. Host only.
	// sam_can_stand(uid, tileX, tileY) -> boolean. See the Lua twin for why the bounds test
	// comes first: checkObstacle answers CLEAR for every coordinate outside the map.
	JSValue js_sam_can_stand(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t tx = 0, ty = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_can_stand") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &tx, "sam_can_stand") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &ty, "sam_can_stand") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveReadableEntity((long long)uid, "sam_can_stand");
		if ( !e ) { return JS_FALSE; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height ) { return JS_FALSE; }
		// See the Lua twin: a client does not keep the entity grid this test reads.
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_can_stand: on a connected client this only sees walls and floor,"
				" not creatures, because the client does not keep the entity grid the test reads."
				" Ask the host if the answer has to include who is standing there.");
		}
		const hit_t samSavedHit = hit;
		const bool ok = ( checkObstacle((tx << 4) + 8, (ty << 4) + 8, e, nullptr) == 0 );
		hit = samSavedHit;   // never leave the engine's global perturbed by our probe
		return JS_NewBool(ctx, ok ? 1 : 0);
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_remove_entity(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// QUEUED, not freed here -- see the Lua twin. Freeing an Entity mid-event is a
		// use-after-free: Entity::attack keeps dereferencing hit.entity for ~160 lines after the
		// modHP call that fired the damage event a mod is handling. Shared queue, one drain.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_remove_entity") ) { return JS_FALSE; }
		return JS_NewBool(ctx, SAMLua::queueRemoveEntity((unsigned int)uid, "sam_remove_entity") ? 1 : 0);
	}

	// sam_spawn_companion(player, model_id [, scale]) -> uid | undefined. Twin of the Lua binding:
	// spawns a floating companion that renders a registered custom .vox model and trails the
	// player, ready to thrust forward on sam_companion_punch. Remove with sam_remove_entity.
	JSValue js_sam_spawn_companion(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_spawn_companion") ) { return JS_UNDEFINED; }
		const char* modelC = ( samHasArg(argc, argv, 1) ) ? JS_ToCString(ctx, argv[1]) : nullptr;
		double scale = 1.0;
		samJsOptF64(ctx, argc, argv, 2, &scale, __func__);
		// g_currentNs is OURS. The shared spawn functions default to the Lua runtime's namespace,
		// which is empty during a JS callback and is the wrong mod's when a Lua event fired us.
		const unsigned long long uid = SAMLua::spawnCompanion(player, modelC ? modelC : "", scale, g_currentNs);
		if ( modelC ) { JS_FreeCString(ctx, modelC); }
		if ( uid == 0 ) { return JS_UNDEFINED; }
		return JS_NewInt64(ctx, (int64_t)uid);
	}

	// sam_companion_punch(uid) -> bool. Trigger the forward punch thrust on a companion.
	JSValue js_sam_companion_punch(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		return SAMLua::companionPunch((unsigned long long)uid) ? JS_TRUE : JS_FALSE;
	}

	// sam_get_facing(player) -> yaw radians [0,2PI) | undefined. 0 = +x (east), increasing toward
	// +y; forward unit vector is (cos yaw, sin yaw). Host-authoritative for remote players.
	JSValue js_sam_get_facing(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_facing") ) { return JS_UNDEFINED; }
		const double yaw = SAMLua::getFacing(player);
		if ( yaw < 0.0 ) { return JS_UNDEFINED; }
		return JS_NewFloat64(ctx, yaw);
	}

	// Defined further down with the rules and batch-4 twins; declared here for the presentation
	// bindings, which refuse a missing or malformed argument the way their Lua twins' luaL_check*
	// raise, instead of turning it into a 0.
	static bool samJsRuleInt(JSContext* ctx, int argc, JSValueConst* argv, int i, int32_t* io, const char* who, bool required);
	static bool samJsRuleAmount(JSContext* ctx, int argc, JSValueConst* argv, int i, double* io, const char* who);
	static bool samJsReqStr(JSContext* ctx, int argc, JSValueConst* argv, int i, const char* who, std::string* out);

	// sam_screen_flash(player, r, g, b [, intensity=1.0] [, duration_ms=180]) -> bool.
	// JS twin of the Lua binding — the anime "impact frame" full-screen colour flash.
	JSValue js_sam_screen_flash(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1, r = 0, g = 0, b = 0, ms = 180;
		double inten = 1.0;
		// player, r, g and b are required, as luaL_checkinteger makes them in Lua: a missing colour
		// used to become 0 and flash the screen black.
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_screen_flash", true) ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 1, &r, "sam_screen_flash", true) ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 2, &g, "sam_screen_flash", true) ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 3, &b, "sam_screen_flash", true) ) { return JS_FALSE; }
		if ( !samJsRuleAmount(ctx, argc, argv, 4, &inten, "sam_screen_flash") ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 5, &ms, "sam_screen_flash", false) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_FALSE; }
		SAMLua::triggerScreenFlash(player, r, g, b, inten, ms, 0, 0); // style 0 = plain fill
		return JS_TRUE;
#else
		(void)player; (void)r; (void)g; (void)b; (void)inten; (void)ms;
		return JS_FALSE;
#endif
	}

	// sam_impact_frame(player, r, g, b [, intensity=1.0] [, duration_ms=220] [, lines=110]) -> bool.
	// JS twin — the exaggerated anime burst (colour pop + manga speed lines + core flare).
	JSValue js_sam_impact_frame(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1, r = 0, g = 0, b = 0, ms = 220, lines = 110;
		double inten = 1.0;
		// Required as in Lua (luaL_checkinteger); a missing colour used to burst black.
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_impact_frame", true) ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 1, &r, "sam_impact_frame", true) ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 2, &g, "sam_impact_frame", true) ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 3, &b, "sam_impact_frame", true) ) { return JS_FALSE; }
		if ( !samJsRuleAmount(ctx, argc, argv, 4, &inten, "sam_impact_frame") ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 5, &ms, "sam_impact_frame", false) ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 6, &lines, "sam_impact_frame", false) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_FALSE; }
		SAMLua::triggerScreenFlash(player, r, g, b, inten, ms, 1, lines); // style 1 = manga burst
		return JS_TRUE;
#else
		(void)player; (void)r; (void)g; (void)b; (void)inten; (void)ms; (void)lines;
		return JS_FALSE;
#endif
	}

	// sam_camera_shake(player, magnitude) -> bool. JS twin — shakes the player's camera.
	JSValue js_sam_camera_shake(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		double mag = 0.0;
		// Both required, as luaL_checkinteger / luaL_checknumber make them in Lua: a missing
		// magnitude used to return true and shake nothing.
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_camera_shake", true) ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 1, &mag, "sam_camera_shake") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_FALSE; }
		SAMLua::triggerCameraShake(player, mag);
		return JS_TRUE;
#else
		(void)player; (void)mag;
		return JS_FALSE;
#endif
	}

	// sam_hitstop(duration_ms) -> bool. JS twin — brief freeze-frame. Singleplayer only.
	JSValue js_sam_hitstop(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t ms = 0;
		// Required, as luaL_checkinteger makes it in Lua: no argument used to return true and do nothing.
		if ( !samJsRuleInt(ctx, argc, argv, 0, &ms, "sam_hitstop", true) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer != SINGLE )
		{
			SAMNet::warnOnce("sam_hitstop|singleplayer", "sam_hitstop: singleplayer only, so it did nothing."
				" A freeze-frame would stop the host's monsters while every client's kept moving.");
			return JS_FALSE;
		}
		SAMLua::triggerHitstop(ms);
		return JS_TRUE;
#else
		(void)ms;
		return JS_FALSE;
#endif
	}

	// sam_get_inventory(player) -> array of { uid, type, name, count, beatitude, status,
	// identified, equipped }. Empty array for an invalid player, undefined for a player whose
	// backpack this machine cannot see. See the Lua twin: both build the rows with
	// SAMMpInventory::inventoryRows, so they cannot disagree. Reader.
	JSValue js_sam_get_inventory(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua; see sam_get_stat. A missing argument gets an error line of its
		// own: without it, it would read as the empty answer this returns for a backpack this
		// machine has not been told about, which is a real answer a script is meant to test for.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_inventory") ) { return JS_UNDEFINED; }
		std::vector<SAMMpInventory::InventoryRow> rows;
		if ( SAMMpInventory::inventoryRows((int)player, "sam_get_inventory", &rows) == SAMMpInventory::Seen::No )
		{
			return JS_UNDEFINED;
		}
		JSValue arr = JS_NewArray(ctx);
		uint32_t idx = 0;
		for ( const auto& row : rows )
		{
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "uid", JS_NewInt64(ctx, (int64_t)row.uid));
			JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, (int32_t)row.type));
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, row.name.c_str()));
			JS_SetPropertyStr(ctx, o, "count", JS_NewInt32(ctx, (int32_t)row.count));
			JS_SetPropertyStr(ctx, o, "beatitude", JS_NewInt32(ctx, (int32_t)row.beatitude));
			JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, (int32_t)row.status));
			JS_SetPropertyStr(ctx, o, "identified", JS_NewBool(ctx, row.identified ? 1 : 0));
			JS_SetPropertyStr(ctx, o, "equipped", JS_NewBool(ctx, row.equipped ? 1 : 0));
			JS_SetPropertyUint32(ctx, arr, idx++, o);
		}
		return arr;
	}

	// sam_remove_item(itemUid) -> boolean. Refuses an equipped item. An `owner` function: see
	// the Lua twin.
	JSValue js_sam_remove_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required: a missing uid used to become uid 0 and a lookup. Lua's luaL_checkinteger raises.
		if ( !samHasArg(argc, argv, 0) ) { SAM_ERROR("JS", "sam_remove_item: argument 1 (the item uid) is required."); return JS_NewBool(ctx, 0); }
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("JS", "sam_remove_item refused: host only."); return JS_NewBool(ctx, 0); }
		int holder = -1;
		Item* it = samJsItemForWrite(ctx, argc, argv, 0, "sam_remove_item", &holder);
		if ( !it ) { return JS_NewBool(ctx, 0); }
		// Through the queue, like the Lua twin: this used to free inline, which is a
		// use-after-free when a mod destroys the item inside its own on_item_use handler.
		const bool queued = SAMItems::queueDestroy((Uint32)it->uid, holder >= 0 ? holder : clientnum);
		if ( !queued ) { SAMMpInventory::noteOwnerRefusal("sam_remove_item", "that item is equipped; unequip it first.", false, "equipped"); }
		else { SAMMpInventory::nudgeReport(); }   // see the Lua twin
		return JS_NewBool(ctx, queued ? 1 : 0);
	}
#endif

	// sam_get_monster_type(uid) -> "rat" / "skeleton" / ... or undefined. The creature's SPECIES,
	// as the lowercase name the engine uses in monstertypename[]. This is the BASE type: a
	// custom monster is a variant of a vanilla species, so a mod's "Rathalos" built on a bat
	// answers "bat". Lua parity: lua_sam_get_monster_type.
	JSValue js_sam_get_monster_type(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_UNDEFINED; }
		const int t = (int)e->getStats()->type;
		if ( t < 0 || t >= NUMMONSTERS ) { return JS_UNDEFINED; }
		return JS_NewString(ctx, monstertypename[t]);
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_get_monster_name(uid) -> the creature's DISPLAY name, or undefined. For a custom monster
	// this is its variant name ("Rathalos"); vanilla creatures carry an empty variant name, so
	// fall back to the species. Lua parity: lua_sam_get_monster_name.
	JSValue js_sam_get_monster_name(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_UNDEFINED; }
		Stat* st = e->getStats();
		if ( st->name[0] ) { return JS_NewString(ctx, st->name); }
		const int t = (int)st->type;
		if ( t < 0 || t >= NUMMONSTERS ) { return JS_UNDEFINED; }
		return JS_NewString(ctx, monstertypename[t]);
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// ---- monster movement (Lua parity: lua_sam_monster_path_to / _face / _attack) --------
	// Tile coordinates throughout, matching sam_get_position.

	JSValue js_sam_monster_path_to(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t tx = 0, ty = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_monster_path_to") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &tx, "sam_monster_path_to") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &ty, "sam_monster_path_to") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_monster_path_to refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_FALSE; }
		const bool ok = e->monsterSetPathToLocation(tx, ty, 1,
			GeneratePathTypes::GENERATE_PATH_PLAYER_ALLY_MOVETO);
		if ( ok ) { e->monsterState = MONSTER_STATE_HUNT; }
		return ok ? JS_TRUE : JS_FALSE;
#else
		(void)uid; (void)tx; (void)ty; return JS_FALSE;
#endif
	}

	JSValue js_sam_monster_face(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t tx = 0, ty = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_monster_face") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &tx, "sam_monster_face") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &ty, "sam_monster_face") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_monster_face refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_FALSE; }
		const real_t wx = (real_t)(tx * 16 + 8);
		const real_t wy = (real_t)(ty * 16 + 8);
		e->yaw = atan2(wy - e->y, wx - e->x);
		return JS_TRUE;
#else
		(void)uid; (void)tx; (void)ty; return JS_FALSE;
#endif
	}

	JSValue js_sam_monster_attack(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Checked, as luaL_checkinteger checks it in Lua: undefined used to become uid 0.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_monster_attack") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_monster_attack refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_FALSE; }
		e->attack(e->getAttackPose(), 0, nullptr);
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	// ---- multiplayer awareness (Lua parity: sam_is_host / _player_count / _local_player) --

	// sam_test_done(passed, failed) -> boolean. The JavaScript twin of the Lua binding: ends
	// an unattended -samtest run and sets the process exit code from `failed`. False outside
	// a test run, so a published mod may keep the call.
	JSValue js_sam_test_done(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t passed = 0, failed = 0;
		if ( !samJsReqI32(ctx, argc, argv, 0, &passed, "sam_test_done") ) { return JS_NewBool(ctx, 0); }
		if ( !samJsReqI32(ctx, argc, argv, 1, &failed, "sam_test_done") ) { return JS_NewBool(ctx, 0); }
#ifdef SAM_JS_HAVE_BARONY
		return JS_NewBool(ctx, SAMTest::done((int)passed, (int)failed) ? 1 : 0);
#else
		return JS_NewBool(ctx, 0);
#endif
	}

	// sam_set_game_speed(multiplier [, ticks]) -> bool. JS twin. Singleplayer only.
	JSValue js_sam_set_game_speed(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		double mult = 1.0;
		int64_t forTicks = 0;
		// Required, as luaL_checknumber makes it in Lua: a missing multiplier is refused, not read as 1x.
		if ( !samJsReqF64(ctx, argc, argv, 0, &mult, "sam_set_game_speed") ) { return JS_FALSE; }
		// Read wide and clamp as the Lua twin does: past INT_MAX ticks means "for the rest of the
		// run". The plain int32 cast in samJsOptI32 turned 1e10 into a negative number and then
		// into 0 = not a window, so the same script ended at a different speed in JavaScript.
		samJsOptI64(ctx, argc, argv, 1, &forTicks, "sam_set_game_speed");
#ifdef SAM_JS_HAVE_BARONY
		const int window = forTicks > 2147483647LL ? 2147483647 : ( forTicks < 0 ? 0 : (int)forTicks );
		return JS_NewBool(ctx, SAMSpeed::set(mult, window) ? 1 : 0);
#else
		(void)mult; (void)forTicks;
		return JS_FALSE;
#endif
	}

	// sam_get_game_speed() -> number. JS twin.
	JSValue js_sam_get_game_speed(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		return JS_NewFloat64(ctx, SAMSpeed::multiplier());
#else
		return JS_NewFloat64(ctx, 1.0);
#endif
	}

	JSValue js_sam_is_host(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		return (multiplayer != CLIENT) ? JS_TRUE : JS_FALSE;
#else
		return JS_TRUE;
#endif
	}

	JSValue js_sam_player_count(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		int n = 0;
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( !client_disconnected[i] ) { ++n; }
		}
		return JS_NewInt32(ctx, n);
#else
		return JS_NewInt32(ctx, 1);
#endif
	}

	JSValue js_sam_local_player(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		return JS_NewInt32(ctx, clientnum);
#else
		return JS_NewInt32(ctx, 0);
#endif
	}

	// sam_monster_charge(uid, ticks) -> boolean. Lua parity: lua_sam_monster_charge.
	// Drives MONSTER_STATE_GENERIC_CHARGE, an implemented-but-never-triggered engine behaviour.
	JSValue js_sam_monster_charge(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		int32_t ticks = 50;
		samJsOptI32(ctx, argc, argv, 1, &ticks, __func__);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_monster_charge refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_FALSE; }
		if ( ticks < 1 ) { ticks = 1; }
		if ( ticks > 500 ) { ticks = 500; }
		e->monsterState = MONSTER_STATE_GENERIC_CHARGE;
		e->monsterSpecialTimer = ticks;
		return JS_TRUE;
#else
		(void)uid; (void)ticks; return JS_FALSE;
#endif
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
	// told no. Send several small messages. Delivery is reliable but NOT ordered: number them if order matters.

	// sam_send_packet(target, tag, payload) -> boolean. Lua parity: lua_sam_send_packet.
	JSValue js_sam_send_packet(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { SAM_ERROR("JS", "sam_send_packet: needs (target, tag[, payload])."); return JS_FALSE; }
		int32_t target = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &target, "sam_send_packet") ) { return JS_FALSE; }
		// With their lengths, like the Lua twin: a string holding a zero byte used to be cut there.
		size_t tagLen = 0, payLen = 0;
		const char* tagC = JS_ToCStringLen(ctx, &tagLen, argv[1]);
		const char* payC = ( samHasArg(argc, argv, 2) ) ? JS_ToCStringLen(ctx, &payLen, argv[2]) : nullptr;
		const std::string tag = tagC ? std::string(tagC, tagLen) : std::string();
		const std::string pay = payC ? std::string(payC, payLen) : std::string();
		if ( tagC ) { JS_FreeCString(ctx, tagC); }
		if ( payC ) { JS_FreeCString(ctx, payC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == SINGLE ) { return JS_FALSE; }
		if ( tag.empty() || tag.size() > SAMLua::SAM_PACKET_MAX_TAG )
		{
			SAM_ERROR("JS", "sam_send_packet: tag must be 1.." + std::to_string(SAMLua::SAM_PACKET_MAX_TAG)
				+ " characters. Packet not sent.");
			return JS_FALSE;
		}
		if ( pay.size() > SAMLua::SAM_PACKET_MAX_PAYLOAD )
		{
			SAM_ERROR("JS", "sam_send_packet: payload is " + std::to_string(pay.size())
				+ " bytes, the limit is " + std::to_string(SAMLua::SAM_PACKET_MAX_PAYLOAD)
				+ " (one datagram). Packet not sent.");
			return JS_FALSE;
		}
		return SAMLua::sendModPacket(target, tag, pay) ? JS_TRUE : JS_FALSE;
#else
		(void)target; return JS_FALSE;
#endif
	}

	// ---- script HUD (Lua parity: sam_hud_text / _bar / _clear) ---------------------------
	static Uint32 samHudColorJS(JSContext* ctx, int argc, JSValueConst* argv, int idx, Uint32 dflt)
	{
		// samHasArg, not argc: an explicit undefined here would convert to 0, i.e.
		// makeColor(0,0,0,0) -- a fully transparent widget rather than the default colour.
		if ( !samHasArg(argc, argv, idx) ) { return dflt; }
		int64_t v = 0; JS_ToInt64(ctx, &v, argv[idx]);
		const unsigned long long u = (unsigned long long)v;
		return makeColor((Uint8)((u >> 24) & 0xFF), (Uint8)((u >> 16) & 0xFF),
		                 (Uint8)((u >> 8) & 0xFF),  (Uint8)(u & 0xFF));
	}

	JSValue js_sam_hud_text(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Every argument the Lua twin checks is required here too: too few used to return false
		// with nothing in the log, where Lua raised and said which one.
		std::string id, val;
		int32_t x = 0, y = 0;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_hud_text", &id) ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &x, "sam_hud_text") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &y, "sam_hud_text") ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 3, "sam_hud_text", &val) ) { return JS_FALSE; }
		const Uint32 col = samHudColorJS(ctx, argc, argv, 4, makeColor(255, 255, 255, 255));
		return SAMHud::text(g_currentNs, id, x, y, val, col) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_hud_bar(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required as in Lua (luaL_checkstring / luaL_checkinteger / luaL_checknumber).
		std::string id;
		int32_t x = 0, y = 0, w = 0, h = 0;
		double frac = 0.0;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_hud_bar", &id) ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &x, "sam_hud_bar") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &y, "sam_hud_bar") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 3, &w, "sam_hud_bar") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 4, &h, "sam_hud_bar") ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 5, &frac, "sam_hud_bar") ) { return JS_FALSE; }
		const Uint32 col = samHudColorJS(ctx, argc, argv, 6, makeColor(200, 40, 40, 255));
		return SAMHud::bar(g_currentNs, id, x, y, w, h, frac, col) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_hud_clear(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// No id means "clear MY HUD", not everybody's (Lua parity).
		if ( !samHasArg(argc, argv, 0) ) { SAMHud::clearNamespace(g_currentNs); return JS_TRUE; }
		const char* id = JS_ToCString(ctx, argv[0]);
		const bool ok = SAMHud::clear(g_currentNs, id ? id : "");
		if ( id ) { JS_FreeCString(ctx, id); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	// ---- the mod's own pictures (Lua parity: sam_show_image / _at / sam_hide_image /
	//      sam_hud_image / sam_get_image_size) ---------------------------------------------
	// Same naming rules as the Lua side: "ns:name" from a manifest, a bare "name" meaning one
	// of this mod's declared images, or a path inside this mod's folder.

	static int samImageFitJS(JSContext* ctx, int argc, JSValueConst* argv, int idx)
	{
		if ( !samHasArg(argc, argv, idx) ) { return SAMImages::FIT_STRETCH; }
		if ( JS_IsNumber(argv[idx]) )
		{
			int32_t v = 0; JS_ToInt32(ctx, &v, argv[idx]);
			return (int)v;
		}
		const char* fs = JS_ToCString(ctx, argv[idx]);
		const bool contain = ( fs && (strcmp(fs, "contain") == 0 || strcmp(fs, "fit") == 0) );
		if ( fs ) { JS_FreeCString(ctx, fs); }
		return contain ? SAMImages::FIT_CONTAIN : SAMImages::FIT_STRETCH;
	}

	// sam_show_image(player, image [, durationMs [, alpha [, "stretch"|"contain" ]]]) -> boolean
	JSValue js_sam_show_image(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int32_t player = 0, ms = 0, alpha = 255;
		JS_ToInt32(ctx, &player, argv[0]);
		const char* img = JS_ToCString(ctx, argv[1]);
		samJsOptI32(ctx, argc, argv, 2, &ms, __func__);
		samJsOptI32(ctx, argc, argv, 3, &alpha, __func__);
		const int fit = samImageFitJS(ctx, argc, argv, 4);
		const bool ok = SAMImages::show(player, g_currentNs, img ? img : "",
			ms, alpha, fit, 0, 0, 0, 0);
		if ( img ) { JS_FreeCString(ctx, img); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	// sam_show_image_at(player, image, x, y, w, h [, durationMs [, alpha ]]) -> boolean
	JSValue js_sam_show_image_at(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 6 ) { return JS_FALSE; }
		int32_t player = 0, x = 0, y = 0, w = 0, h = 0, ms = 0, alpha = 255;
		JS_ToInt32(ctx, &player, argv[0]);
		const char* img = JS_ToCString(ctx, argv[1]);
		JS_ToInt32(ctx, &x, argv[2]); JS_ToInt32(ctx, &y, argv[3]);
		JS_ToInt32(ctx, &w, argv[4]); JS_ToInt32(ctx, &h, argv[5]);
		samJsOptI32(ctx, argc, argv, 6, &ms, __func__);
		samJsOptI32(ctx, argc, argv, 7, &alpha, __func__);
		const bool ok = SAMImages::show(player, g_currentNs, img ? img : "",
			ms, alpha, SAMImages::FIT_RECT, x, y, w, h);
		if ( img ) { JS_FreeCString(ctx, img); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	// sam_hide_image([player]) -> boolean. No player clears every player's overlay.
	JSValue js_sam_hide_image(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// "no player given" includes an explicit undefined/null, as it does in Lua; on argc
		// alone, sam_hide_image(x) with x undefined cleared player 0's overlay and left the
		// other three on screen.
		if ( !samHasArg(argc, argv, 0) )
		{
			bool any = false;
			for ( int c = 0; c < MAXPLAYERS; ++c ) { if ( SAMImages::hide(c) ) { any = true; } }
			return any ? JS_TRUE : JS_FALSE;
		}
		int32_t player = 0; JS_ToInt32(ctx, &player, argv[0]);
		return SAMImages::hide(player) ? JS_TRUE : JS_FALSE;
	}

	// sam_hud_image(id, x, y, w, h, image [, 0xRRGGBBAA]) -> boolean
	JSValue js_sam_hud_image(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 6 ) { return JS_FALSE; }
		const char* id = JS_ToCString(ctx, argv[0]);
		int32_t x = 0, y = 0, w = 0, h = 0;
		JS_ToInt32(ctx, &x, argv[1]); JS_ToInt32(ctx, &y, argv[2]);
		JS_ToInt32(ctx, &w, argv[3]); JS_ToInt32(ctx, &h, argv[4]);
		const char* img = JS_ToCString(ctx, argv[5]);
		const Uint32 col = samHudColorJS(ctx, argc, argv, 6, makeColor(255, 255, 255, 255));
		const std::string path = SAMImages::resolve(g_currentNs, img ? img : "");
		const bool ok = !path.empty() && SAMHud::image(g_currentNs, id ? id : "", x, y, w, h, path, col);
		if ( id ) { JS_FreeCString(ctx, id); }
		if ( img ) { JS_FreeCString(ctx, img); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	// ---- interactive panels (Lua parity: sam_ui_open / _close / _is_open / _clear /
	//      _label / _button / _image) ------------------------------------------------------
	// A click fires the ui.on_click EVENT rather than invoking a callback, so it reaches Lua
	// and JS identically and cannot leave a dangling function reference in a C callback.

	JSValue js_sam_ui_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 5 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		int32_t x = 0, y = 0, w = 0, h = 0;
		JS_ToInt32(ctx, &x, argv[1]); JS_ToInt32(ctx, &y, argv[2]);
		JS_ToInt32(ctx, &w, argv[3]); JS_ToInt32(ctx, &h, argv[4]);
		const char* title = samHasArg(argc, argv, 5) ? JS_ToCString(ctx, argv[5]) : nullptr;
		const bool modal = samHasArg(argc, argv, 6) ? (JS_ToBool(ctx, argv[6]) > 0) : false;
		const bool ok = SAMUi::open(g_currentNs, panel ? panel : "", x, y, w, h,
			title ? title : "", modal);
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( title ) { JS_FreeCString(ctx, title); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_close(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( !samHasArg(argc, argv, 0) ) { SAMUi::closeNamespace(g_currentNs); return JS_TRUE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const bool ok = SAMUi::close(g_currentNs, panel ? panel : "");
		if ( panel ) { JS_FreeCString(ctx, panel); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_is_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string panel;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_ui_is_open", &panel) ) { return JS_FALSE; }
		int32_t player = -1;   // absent: the event's player, else this machine's own (Lua parity)
		if ( !samJsRuleInt(ctx, argc, argv, 1, &player, "sam_ui_is_open", false) ) { return JS_FALSE; }
		return SAMUi::isOpenFor((int)player, g_currentNs, panel) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_clear(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const bool ok = SAMUi::clearWidgets(g_currentNs, panel ? panel : "");
		if ( panel ) { JS_FreeCString(ctx, panel); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_label(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 6 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		int32_t x = 0, y = 0, w = 0;
		JS_ToInt32(ctx, &x, argv[2]); JS_ToInt32(ctx, &y, argv[3]); JS_ToInt32(ctx, &w, argv[4]);
		const char* text = JS_ToCString(ctx, argv[5]);
		const Uint32 col = samHudColorJS(ctx, argc, argv, 6, makeColor(220, 210, 190, 255));
		const bool ok = SAMUi::label(g_currentNs, panel ? panel : "", id ? id : "",
			x, y, w, text ? text : "", col);
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		if ( text ) { JS_FreeCString(ctx, text); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_button(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 7 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		int32_t x = 0, y = 0, w = 0, h = 0;
		JS_ToInt32(ctx, &x, argv[2]); JS_ToInt32(ctx, &y, argv[3]);
		JS_ToInt32(ctx, &w, argv[4]); JS_ToInt32(ctx, &h, argv[5]);
		const char* text = JS_ToCString(ctx, argv[6]);
		const bool ok = SAMUi::button(g_currentNs, panel ? panel : "", id ? id : "",
			x, y, w, h, text ? text : "");
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		if ( text ) { JS_FreeCString(ctx, text); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_image(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 7 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		int32_t x = 0, y = 0, w = 0, h = 0;
		JS_ToInt32(ctx, &x, argv[2]); JS_ToInt32(ctx, &y, argv[3]);
		JS_ToInt32(ctx, &w, argv[4]); JS_ToInt32(ctx, &h, argv[5]);
		const char* img = JS_ToCString(ctx, argv[6]);
		const Uint32 col = samHudColorJS(ctx, argc, argv, 7, makeColor(255, 255, 255, 255));
		const std::string path = SAMImages::resolve(g_currentNs, img ? img : "");
		const bool ok = !path.empty() && SAMUi::image(g_currentNs, panel ? panel : "",
			id ? id : "", x, y, w, h, path, col);
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		if ( img ) { JS_FreeCString(ctx, img); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_list(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 6 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		int32_t x = 0, y = 0, w = 0, h = 0;
		JS_ToInt32(ctx, &x, argv[2]); JS_ToInt32(ctx, &y, argv[3]);
		JS_ToInt32(ctx, &w, argv[4]); JS_ToInt32(ctx, &h, argv[5]);
		const bool ok = SAMUi::list(g_currentNs, panel ? panel : "", id ? id : "", x, y, w, h);
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_list_add(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 4 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		const char* rowId = JS_ToCString(ctx, argv[2]);
		const char* text = JS_ToCString(ctx, argv[3]);
		const Uint32 col = samHudColorJS(ctx, argc, argv, 4, makeColor(220, 210, 190, 255));
		const bool ok = SAMUi::listAdd(g_currentNs, panel ? panel : "", id ? id : "",
			rowId ? rowId : "", text ? text : "", col);
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		if ( rowId ) { JS_FreeCString(ctx, rowId); }
		if ( text ) { JS_FreeCString(ctx, text); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_list_clear(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		const bool ok = SAMUi::listClear(g_currentNs, panel ? panel : "", id ? id : "");
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_input(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 6 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		int32_t x = 0, y = 0, w = 0, h = 0;
		JS_ToInt32(ctx, &x, argv[2]); JS_ToInt32(ctx, &y, argv[3]);
		JS_ToInt32(ctx, &w, argv[4]); JS_ToInt32(ctx, &h, argv[5]);
		const char* text = samHasArg(argc, argv, 6) ? JS_ToCString(ctx, argv[6]) : nullptr;
		const bool ok = SAMUi::input(g_currentNs, panel ? panel : "", id ? id : "",
			x, y, w, h, text ? text : "");
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		if ( text ) { JS_FreeCString(ctx, text); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_input_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string panel, id;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_ui_input_text", &panel) ) { return JS_NewString(ctx, ""); }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_ui_input_text", &id) ) { return JS_NewString(ctx, ""); }
		int32_t player = -1;
		if ( !samJsRuleInt(ctx, argc, argv, 2, &player, "sam_ui_input_text", false) ) { return JS_NewString(ctx, ""); }
		return JS_NewString(ctx, SAMUi::inputTextFor((int)player, g_currentNs, panel, id).c_str());
	}

	JSValue js_sam_ui_panel_style(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const Uint32 bg = samHudColorJS(ctx, argc, argv, 1, 0);
		const Uint32 border = samHudColorJS(ctx, argc, argv, 2, 0);
		int32_t bw = -1; samJsOptI32(ctx, argc, argv, 3, &bw, __func__);
		const bool ok = SAMUi::panelStyle(g_currentNs, panel ? panel : "", bg, border, bw);
		if ( panel ) { JS_FreeCString(ctx, panel); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_font(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		const char* f = JS_ToCString(ctx, argv[2]);
		const bool ok = SAMUi::font(g_currentNs, panel ? panel : "", id ? id : "", f ? f : "");
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		if ( f ) { JS_FreeCString(ctx, f); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_ui_list_row_height(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		int32_t px = 0; JS_ToInt32(ctx, &px, argv[2]);
		const bool ok = SAMUi::listRowHeight(g_currentNs, panel ? panel : "", id ? id : "", px);
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	// [width, height] or undefined -- array form, matching sam_get_image_size.
	JSValue js_sam_ui_text_size(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		const char* text = JS_ToCString(ctx, argv[0]);
		const char* f = samHasArg(argc, argv, 1) ? JS_ToCString(ctx, argv[1]) : nullptr;
		int w = 0, h = 0;
		const bool ok = SAMUi::textSize(text ? text : "", f ? f : "", w, h);
		if ( text ) { JS_FreeCString(ctx, text); }
		if ( f ) { JS_FreeCString(ctx, f); }
		if ( !ok ) { return JS_UNDEFINED; }
		JSValue arr = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, w));
		JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, h));
		return arr;
	}

	// ---- reading the game's own content (Lua parity: sam_list_items / sam_get_item_info /
	//      sam_list_monsters / sam_list_spells) --------------------------------------------

	JSValue js_sam_list_items(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		const char* cat = samHasArg(argc, argv, 0) ? JS_ToCString(ctx, argv[0]) : nullptr;
		const std::vector<SAMCatalog::ItemEntry> list = SAMCatalog::items(cat ? cat : "");
		if ( cat ) { JS_FreeCString(ctx, cat); }
		JSValue arr = JS_NewArray(ctx);
		uint32_t n = 0;
		for ( const SAMCatalog::ItemEntry& e : list )
		{
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, e.type));
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e.name.c_str()));
			JS_SetPropertyStr(ctx, o, "unidentified", JS_NewString(ctx, e.unidName.c_str()));
			JS_SetPropertyStr(ctx, o, "category", JS_NewString(ctx, e.category.c_str()));
			JS_SetPropertyStr(ctx, o, "level", JS_NewInt32(ctx, e.level));
			JS_SetPropertyStr(ctx, o, "weight", JS_NewInt32(ctx, e.weight));
			JS_SetPropertyStr(ctx, o, "value", JS_NewInt32(ctx, e.value));
			JS_SetPropertyStr(ctx, o, "custom", JS_NewBool(ctx, e.custom));
			JS_SetPropertyUint32(ctx, arr, n++, o);
		}
		return arr;
	}

	JSValue js_sam_get_item_info(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		int type = -1;
		if ( JS_IsNumber(argv[0]) )
		{
			int32_t t = -1; JS_ToInt32(ctx, &t, argv[0]); type = t;
		}
		else
		{
			const char* nm = JS_ToCString(ctx, argv[0]);
			type = SAMCatalog::itemTypeFor(nm ? nm : "");
			if ( nm ) { JS_FreeCString(ctx, nm); }
		}
		SAMCatalog::ItemEntry e;
		std::map<std::string, int> attrs;
		if ( !SAMCatalog::itemInfo(type, e, attrs) ) { return JS_UNDEFINED; }

		JSValue o = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, e.type));
		JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e.name.c_str()));
		JS_SetPropertyStr(ctx, o, "unidentified", JS_NewString(ctx, e.unidName.c_str()));
		JS_SetPropertyStr(ctx, o, "category", JS_NewString(ctx, e.category.c_str()));
		JS_SetPropertyStr(ctx, o, "level", JS_NewInt32(ctx, e.level));
		JS_SetPropertyStr(ctx, o, "weight", JS_NewInt32(ctx, e.weight));
		JS_SetPropertyStr(ctx, o, "value", JS_NewInt32(ctx, e.value));
		JS_SetPropertyStr(ctx, o, "custom", JS_NewBool(ctx, e.custom));
		JSValue a = JS_NewObject(ctx);
		for ( const auto& kv : attrs )
		{
			JS_SetPropertyStr(ctx, a, kv.first.c_str(), JS_NewInt32(ctx, kv.second));
		}
		JS_SetPropertyStr(ctx, o, "attributes", a);
		return o;
	}

	JSValue js_sam_list_monsters(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		const std::vector<SAMCatalog::MonsterEntry> list = SAMCatalog::monsters();
		JSValue arr = JS_NewArray(ctx);
		uint32_t n = 0;
		for ( const SAMCatalog::MonsterEntry& e : list )
		{
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, e.type));
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e.name.c_str()));
			JS_SetPropertyUint32(ctx, arr, n++, o);
		}
		return arr;
	}

	JSValue js_sam_list_spells(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		const std::vector<SAMCatalog::SpellEntry> list = SAMCatalog::spells();
		JSValue arr = JS_NewArray(ctx);
		uint32_t n = 0;
		for ( const SAMCatalog::SpellEntry& e : list )
		{
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "id", JS_NewInt32(ctx, e.id));
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e.name.c_str()));
			JS_SetPropertyStr(ctx, o, "cost", JS_NewInt32(ctx, e.cost));
			JS_SetPropertyUint32(ctx, arr, n++, o);
		}
		return arr;
	}

	JSValue js_sam_spawn_projectile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 4 ) { return JS_UNDEFINED; }
		double x = 0, y = 0, angle = 0, speed = 0;
		JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
		JS_ToFloat64(ctx, &angle, argv[2]); JS_ToFloat64(ctx, &speed, argv[3]);
		int32_t dmg = 0, life = 100, owner = -1;
		samJsOptI32(ctx, argc, argv, 4, &dmg, __func__);
		samJsOptI32(ctx, argc, argv, 5, &life, __func__);
		const char* model = samHasArg(argc, argv, 6) ? JS_ToCString(ctx, argv[6]) : nullptr;
		samJsOptI32(ctx, argc, argv, 7, &owner, __func__);
		const unsigned long long uid = SAMLua::spawnProjectile(owner, x, y, angle, speed,
			dmg, life, model ? model : "", g_currentNs);   // our namespace, not the Lua runtime's
		if ( model ) { JS_FreeCString(ctx, model); }
		if ( uid == 0 ) { return JS_UNDEFINED; }
		return JS_NewInt64(ctx, (int64_t)uid);
	}

	// sam_get_image_size(image) -> [width, height], or undefined. Array rather than two returns:
	// the JS side mirrors every multi-value Lua function this way (see sam_get_position).
	JSValue js_sam_get_image_size(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		const char* img = JS_ToCString(ctx, argv[0]);
		int w = 0, h = 0;
		const bool ok = SAMImages::size(g_currentNs, img ? img : "", w, h);
		if ( img ) { JS_FreeCString(ctx, img); }
		if ( !ok ) { return JS_UNDEFINED; }
		JSValue arr = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, w));
		JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, h));
		return arr;
	}

	// ===== world, perception and truth (Lua parity) =======================================
	// Same argument convention as the Lua side: everything here takes a UID, never a player
	// index, because the split convention in the older API fails silently.

	static int samSkillFromNameJS(const char* nameC)
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

	JSValue js_sam_get_tile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_UNDEFINED; }
		int32_t x = 0, y = 0; JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
		const SAMWorld::TileInfo t = SAMWorld::tile(x, y);
		if ( !t.valid ) { return JS_UNDEFINED; }
		JSValue o = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, o, "wall", JS_NewInt32(ctx, t.wall));
		JS_SetPropertyStr(ctx, o, "floor", JS_NewInt32(ctx, t.floor));
		JS_SetPropertyStr(ctx, o, "ceiling", JS_NewInt32(ctx, t.ceiling));
		JS_SetPropertyStr(ctx, o, "solid", JS_NewBool(ctx, t.solid));
		JS_SetPropertyStr(ctx, o, "water", JS_NewBool(ctx, t.water));
		JS_SetPropertyStr(ctx, o, "lava", JS_NewBool(ctx, t.lava));
		JS_SetPropertyStr(ctx, o, "walkable", JS_NewBool(ctx, t.walkable));
		return o;
	}

	JSValue js_sam_set_tile(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 4 ) { return JS_FALSE; }
		int32_t x = 0, y = 0, l = 0, id = 0;
		JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
		JS_ToInt32(ctx, &l, argv[2]); JS_ToInt32(ctx, &id, argv[3]);
		return SAMWorld::setTile(x, y, l, id) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_is_spawnable(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int32_t x = 0, y = 0; JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
		return SAMWorld::spawnable(x, y) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_line_of_sight(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 4 ) { return JS_FALSE; }
		double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
		JS_ToFloat64(ctx, &x1, argv[0]); JS_ToFloat64(ctx, &y1, argv[1]);
		JS_ToFloat64(ctx, &x2, argv[2]); JS_ToFloat64(ctx, &y2, argv[3]);
		const bool ents = ( samHasArg(argc, argv, 4) ) ? (JS_ToBool(ctx, argv[4]) > 0) : false;
		int bx = -1, by = -1;
		const bool ok = SAMWorld::lineOfSight(x1, y1, x2, y2, ents, bx, by);
		// JS gets an object rather than multiple returns.
		JSValue o = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, o, "visible", JS_NewBool(ctx, ok));
		JS_SetPropertyStr(ctx, o, "blocked_x", JS_NewInt32(ctx, bx));
		JS_SetPropertyStr(ctx, o, "blocked_y", JS_NewInt32(ctx, by));
		return o;
	}

	JSValue js_sam_tiles_connected(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 4 ) { return JS_FALSE; }
		int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
		JS_ToInt32(ctx, &x1, argv[0]); JS_ToInt32(ctx, &y1, argv[1]);
		JS_ToInt32(ctx, &x2, argv[2]); JS_ToInt32(ctx, &y2, argv[3]);
		const bool fly = ( samHasArg(argc, argv, 4) ) ? (JS_ToBool(ctx, argv[4]) > 0) : false;
		return SAMWorld::connected(x1, y1, x2, y2, fly) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_get_light_at(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_NewInt32(ctx, 0); }
		int32_t x = 0, y = 0; JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
		// -1 default, matching Lua: the shared lightmap the monster AI reads.
		int32_t pl = -1; samJsOptI32(ctx, argc, argv, 2, &pl, __func__);
		return JS_NewInt32(ctx, SAMWorld::lightAt(x, y, pl));
	}

	JSValue js_sam_find_entities(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_NewArray(ctx); }
		int32_t x = 0, y = 0; JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
		double r = 0; JS_ToFloat64(ctx, &r, argv[2]);
		const char* kindC = ( samHasArg(argc, argv, 3) ) ? JS_ToCString(ctx, argv[3]) : nullptr;
		const std::string kind = kindC ? kindC : "any";
		if ( kindC ) { JS_FreeCString(ctx, kindC); }
		const std::vector<uint32_t> ids = SAMWorld::findEntities(x, y, r, kind);
		JSValue arr = JS_NewArray(ctx);
		uint32_t i = 0;
		for ( uint32_t u : ids ) { JS_SetPropertyUint32(ctx, arr, i++, JS_NewInt64(ctx, (int64_t)u)); }
		return arr;
	}

	JSValue js_sam_get_container_items(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		std::vector<SAMWorld::ItemInfo> found;
		if ( !SAMWorld::containerItems((uint32_t)uid, found) ) { return JS_UNDEFINED; }
		JSValue arr = JS_NewArray(ctx);
		uint32_t i = 0;
		for ( const auto& it : found )
		{
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, it.type));
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, it.name.c_str()));
			JS_SetPropertyStr(ctx, o, "count", JS_NewInt32(ctx, it.count));
			JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, it.status));
			JS_SetPropertyStr(ctx, o, "beatitude", JS_NewInt32(ctx, it.beatitude));
			JS_SetPropertyStr(ctx, o, "identified", JS_NewBool(ctx, it.identified));
			JS_SetPropertyUint32(ctx, arr, i++, o);
		}
		return arr;
	}

	JSValue js_sam_set_door(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// The uid is required, as luaL_checkinteger makes it in Lua. A missing flag is false, as
		// samBoolArg makes it there, so sam_set_door(uid) CLOSES the door in both runtimes (this
		// used to return false and do nothing).
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_door") ) { return JS_FALSE; }
		return SAMWorld::setDoor((uint32_t)uid, samBoolArgJs(ctx, argc, argv, 1, false)) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_set_door_locked(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// The uid is required, as luaL_checkinteger makes it in Lua. A missing flag is false, as
		// samBoolArg makes it there, so sam_set_door_locked(uid) UNLOCKS it in both runtimes (this
		// used to return false and do nothing).
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_door_locked") ) { return JS_FALSE; }
		return SAMWorld::setDoorLocked((uint32_t)uid, samBoolArgJs(ctx, argc, argv, 1, false)) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_power_entity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// The uid is required, as luaL_checkinteger makes it in Lua. A missing flag is false, as
		// samBoolArg makes it there, so sam_power_entity(uid) powers it OFF in both runtimes (this
		// used to return false and do nothing).
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_power_entity") ) { return JS_FALSE; }
		return SAMWorld::powerEntity((uint32_t)uid, samBoolArgJs(ctx, argc, argv, 1, false)) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_toggle_switch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		return SAMWorld::toggleSwitch((uint32_t)uid) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_get_level_info(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		const SAMWorld::LevelInfo l = SAMWorld::level();
		JSValue o = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, o, "floor", JS_NewInt32(ctx, l.floor));
		JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, l.name.c_str()));
		JS_SetPropertyStr(ctx, o, "author", JS_NewString(ctx, l.author.c_str()));
		JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, l.width));
		JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, l.height));
		JS_SetPropertyStr(ctx, o, "secret", JS_NewBool(ctx, l.secret));
		JS_SetPropertyStr(ctx, o, "skybox", JS_NewInt32(ctx, l.skybox));
		JS_SetPropertyStr(ctx, o, "no_digging", JS_NewBool(ctx, l.noDigging));
		JS_SetPropertyStr(ctx, o, "no_teleport", JS_NewBool(ctx, l.noTeleport));
		JS_SetPropertyStr(ctx, o, "no_levitation", JS_NewBool(ctx, l.noLevitation));
		return o;
	}

	JSValue js_sam_get_effective_stat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_UNDEFINED; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
		const std::string n = samUpper(nameC ? nameC : "");
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e || !e->getStats() ) { return JS_UNDEFINED; }
		Stat* st = e->getStats();
		long long v = 0;
		if      ( n == "STR" ) { v = statGetSTR(st, e); }
		else if ( n == "DEX" ) { v = statGetDEX(st, e); }
		else if ( n == "CON" ) { v = statGetCON(st, e); }
		else if ( n == "INT" ) { v = statGetINT(st, e); }
		else if ( n == "PER" ) { v = statGetPER(st, e); }
		else if ( n == "CHR" ) { v = statGetCHR(st, e); }
		else { SAM_WARN("JS", "sam_get_effective_stat: unknown stat '" + n + "'."); return JS_UNDEFINED; }
		return JS_NewInt64(ctx, v);
#else
		return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_get_ac(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e || !e->getStats() ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, AC(e->getStats()));
#else
		return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_get_skill(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_UNDEFINED; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
		const int skill = samSkillFromNameJS(nameC);
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		const bool eff = ( samHasArg(argc, argv, 2) ) ? (JS_ToBool(ctx, argv[2]) > 0) : true;
#ifdef SAM_JS_HAVE_BARONY
		if ( skill < 0 ) { SAM_WARN("JS", "sam_get_skill: unknown skill."); return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e || !e->getStats() ) { return JS_UNDEFINED; }
		Stat* st = e->getStats();
		return JS_NewInt32(ctx, eff ? st->getModifiedProficiency(skill) : st->getProficiency(skill));
#else
		(void)eff; return JS_UNDEFINED;
#endif
	}

	static JSValue samFactionCheckJS(JSContext* ctx, int argc, JSValueConst* argv, bool wantEnemy)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t a = 0, b = 0; JS_ToInt64(ctx, &a, argv[0]); JS_ToInt64(ctx, &b, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* ea = SAMLua::resolveEntityQuiet((long long)a);
		Entity* eb = SAMLua::resolveEntityQuiet((long long)b);
		if ( !ea || !eb ) { return JS_FALSE; }
		return (wantEnemy ? ea->checkEnemy(eb) : ea->checkFriend(eb)) ? JS_TRUE : JS_FALSE;
#else
		(void)wantEnemy; return JS_FALSE;
#endif
	}
	JSValue js_sam_is_enemy(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{ return samFactionCheckJS(ctx, argc, argv, true); }
	JSValue js_sam_is_friend(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{ return samFactionCheckJS(ctx, argc, argv, false); }

	JSValue js_sam_get_mods(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		JSValue arr = JS_NewArray(ctx);
#ifdef SAM_JS_HAVE_BARONY
		uint32_t i = 0;
		for ( const SAMModManifest& m : SAMWorkshop::manifests() )
		{
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "ns", JS_NewString(ctx, m.ns.c_str()));
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, m.name.c_str()));
			JS_SetPropertyStr(ctx, o, "version", JS_NewString(ctx, m.version.c_str()));
			JS_SetPropertyStr(ctx, o, "author", JS_NewString(ctx, m.author.c_str()));
			JS_SetPropertyUint32(ctx, arr, i++, o);
		}
#endif
		return arr;
	}

	JSValue js_sam_is_mod_loaded(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		const char* nsC = JS_ToCString(ctx, argv[0]);
		const std::string want = nsC ? nsC : "";
		if ( nsC ) { JS_FreeCString(ctx, nsC); }
#ifdef SAM_JS_HAVE_BARONY
		for ( const SAMModManifest& m : SAMWorkshop::manifests() )
		{
			if ( m.ns == want ) { return JS_TRUE; }
		}
#endif
		return JS_FALSE;
	}

	static int samResolveSoundIdJS(JSContext* ctx, JSValueConst v)
	{
		if ( JS_IsString(v) )
		{
			const char* nm = JS_ToCString(ctx, v);
			const int id = SAMLua::resolveSoundAssetIn(nm ? nm : "", g_currentNs);   // bare id = one of mine
			if ( id < 0 )
			{
				SAM_WARN("JS", std::string("unknown sound '") + (nm ? nm : "") + "'. A mod's sound is \"namespace:name\""
					" (a file sounds/name.ogg is \"<namespace>:name\"); sam_list_sounds() lists every one loaded.");
			}
			if ( nm ) { JS_FreeCString(ctx, nm); }
			return id;
		}
		// A number has to be a whole one, as luaL_checkinteger insists in the Lua twin: a missing
		// sound, null or 2.5 used to become sound 0 or 2 and play it.
		double d = 0.0;
		if ( JS_IsUndefined(v) || JS_IsNull(v) || !samJsNum(ctx, v, &d) || d != std::floor(d) )
		{
			SAM_ERROR("JS", "a sound is a \"namespace:name\" string or a whole vanilla sound number.");
			return -1;
		}
		return (int)d;
	}

	// ---- world-space presentation (Lua parity) --------------------------------------------

	JSValue js_sam_play_sound_at(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		const int snd = samResolveSoundIdJS(ctx, samHasArg(argc, argv, 0) ? argv[0] : JS_UNDEFINED);
		double tx = 0, ty = 0;
		if ( !samJsReqF64(ctx, argc, argv, 1, &tx, "sam_play_sound_at") ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 2, &ty, "sam_play_sound_at") ) { return JS_FALSE; }
		int32_t vol = 128;
		if ( !samJsRuleInt(ctx, argc, argv, 3, &vol, "sam_play_sound_at", false) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( snd < 0 || snd >= (int)numsounds ) { return JS_FALSE; }
		if ( vol < 0 ) { vol = 0; } if ( vol > 255 ) { vol = 255; }
		playSoundPos(tx * 16.0 + 8.0, ty * 16.0 + 8.0, (Uint16)snd, (Uint8)vol);
		return JS_TRUE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_play_sound_entity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		const int snd = samResolveSoundIdJS(ctx, samHasArg(argc, argv, 0) ? argv[0] : JS_UNDEFINED);
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &uid, "sam_play_sound_entity") ) { return JS_FALSE; }
		int32_t vol = 128;
		if ( !samJsRuleInt(ctx, argc, argv, 2, &vol, "sam_play_sound_entity", false) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( snd < 0 || snd >= (int)numsounds ) { return JS_FALSE; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_FALSE; }
		if ( vol < 0 ) { vol = 0; } if ( vol > 255 ) { vol = 255; }
		playSoundEntity(e, (Uint16)snd, (Uint8)vol);
		return JS_TRUE;
#else
		return JS_FALSE;
#endif
	}

	// ---- music, and controlling sounds (Lua parity) ----------------------------------------

	// Defined with the batch-4 helpers further down; declared here so these can refuse a
	// missing name the same way.
	static bool samJsReqStr(JSContext* ctx, int argc, JSValueConst* argv, int i,
		const char* who, std::string* out);

	static std::string samJsMusicId(const std::string& in)
	{
		if ( in.find(':') == std::string::npos && !g_currentNs.empty() ) { return g_currentNs + ":" + in; }
		return in;
	}

	JSValue js_sam_play_music(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string id;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_play_music", &id) ) { return JS_FALSE; }
		double fade = 1.5;
		samJsOptF64(ctx, argc, argv, 1, &fade, "sam_play_music");
		const bool loop = samBoolArgJs(ctx, argc, argv, 2, true);
		const bool persist = samBoolArgJs(ctx, argc, argv, 3, false);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_play_music refused: host only. The host picks forced music and sends it to everyone.");
			return JS_FALSE;
		}
		std::string err;
		if ( !SAMMusic::play(samJsMusicId(id), fade, loop, persist, &err) )
		{
			SAM_ERROR("JS", "sam_play_music: " + err + ". A track is a file in the mod's music/ folder,"
				" \"<namespace>:<file name>\"; sam_list_music() lists every one loaded.");
			return JS_FALSE;
		}
		return JS_TRUE;
#else
		(void)loop; (void)persist; return JS_FALSE;
#endif
	}

	JSValue js_sam_stop_music(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_stop_music refused: host only."); return JS_FALSE; }
		return SAMMusic::stop() ? JS_TRUE : JS_FALSE;
#else
		(void)ctx; return JS_FALSE;
#endif
	}

	JSValue js_sam_get_music(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		const std::string n = SAMMusic::nowPlaying();
		if ( n.empty() ) { return JS_UNDEFINED; }
		return JS_NewString(ctx, n.c_str());
#else
		(void)ctx; return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_list_music(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		JSValue arr = JS_NewArray(ctx);
#ifdef SAM_JS_HAVE_BARONY
		uint32_t i = 0;
		for ( const std::string& id : SAMMusic::listIds() ) { JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, id.c_str())); }
#endif
		return arr;
	}

	JSValue js_sam_list_sounds(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
		JSValue arr = JS_NewArray(ctx);
#ifdef SAM_JS_HAVE_BARONY
		uint32_t i = 0;
		for ( const std::string& id : SAMSounds::listIds() ) { JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, id.c_str())); }
#endif
		return arr;
	}

	JSValue js_sam_stop_sound(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string in;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_stop_sound", &in) ) { return JS_NewInt32(ctx, 0); }
#ifdef SAM_JS_HAVE_BARONY
		std::string id = in;
		if ( !SAMSounds::isRegistered(id) ) { id = samJsMusicId(in); }
		if ( !SAMSounds::isRegistered(id) )
		{
			SAM_WARN("JS", "sam_stop_sound: unknown sound '" + in + "'.");
			return JS_NewInt32(ctx, 0);
		}
		const int n = SAMSounds::stopById(id);
		SAMSounds::broadcastStop(id);
		return JS_NewInt32(ctx, n);
#else
		return JS_NewInt32(ctx, 0);
#endif
	}

	JSValue js_sam_spawn_particle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_FALSE; }
		const char* kindC = JS_ToCString(ctx, argv[0]);
		const std::string k = kindC ? kindC : "";
		if ( kindC ) { JS_FreeCString(ctx, kindC); }
		double tx = 0, ty = 0, z = 0, scale = 1.0;
		JS_ToFloat64(ctx, &tx, argv[1]); JS_ToFloat64(ctx, &ty, argv[2]);
		samJsOptF64(ctx, argc, argv, 3, &z, __func__);
		samJsOptF64(ctx, argc, argv, 4, &scale, __func__);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_particle refused: host only."); return JS_FALSE; }
		const Sint16 px = (Sint16)(tx * 16.0 + 8.0), py = (Sint16)(ty * 16.0 + 8.0), pz = (Sint16)z;
		Entity* made = nullptr;
		if      ( k == "poof" )      { made = spawnPoof(px, py, pz, SAMMpEntities::wirePoofScale(scale, "sam_spawn_particle"), true); }
		else if ( k == "explosion" ) { made = spawnExplosion(px, py, pz); }
		else if ( k == "bang" )      { made = spawnBang(px, py, pz); }
		else if ( k == "sleep" )     { made = spawnSleepZ(px, py, pz); }
		else { SAM_WARN("JS", "sam_spawn_particle: unknown kind '" + k + "'."); return JS_FALSE; }
		return made ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_damage_number(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		int32_t amount = 0; JS_ToInt32(ctx, &amount, argv[1]);
		int32_t gibType = 0; samJsOptI32(ctx, argc, argv, 2, &gibType, __func__);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_damage_number refused: host only."); return JS_FALSE; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_FALSE; }
		spawnDamageGib(e, amount, gibType, 0, true);
		return JS_TRUE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_get_monster_stat(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_monster_stat") ) { return JS_NewInt32(ctx, 0); }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_get_monster_stat: argument 2 is required."); return JS_NewInt32(ctx, 0); }
		const char* nameC = JS_ToCString(ctx, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("JS", "sam_get_monster_stat: no monster uid " + std::to_string(uid)); if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_NewInt32(ctx, 0); }
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
		else { SAM_WARN("JS", std::string("sam_get_monster_stat: unknown stat '") + (nameC ? nameC : "") + "'"); }
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		return JS_NewInt64(ctx, v);
#else
		(void)uid; if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_NewInt32(ctx, 0);
#endif
	}

	// sam_monster_has_effect(uid, "EFFECT") -> boolean. Monster counterpart of sam_has_effect.
	JSValue js_sam_monster_has_effect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_monster_has_effect") ) { return JS_NewBool(ctx, 0); }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_monster_has_effect: argument 2 is required."); return JS_NewBool(ctx, 0); }
		const char* nameC = JS_ToCString(ctx, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_NewBool(ctx, 0); }
		const int eff = samEffectNameToId(nameC ? nameC : "");
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		if ( eff < 0 ) { return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, e->getStats()->getEffectActive(eff) != 0 ? 1 : 0);
#else
		(void)uid; if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_NewBool(ctx, 0);
#endif
	}

	// sam_monster_has_trait(uid, "undead") -> boolean. Reads back what the mod declared in
	// JSON. False for every vanilla monster (mask is 0), so it is a no-op without a mod.
	JSValue js_sam_monster_has_trait(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_NewBool(ctx, 0); }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		const unsigned long long bit = SAMMonsters::traitBitForName(nameC ? nameC : "");
		if ( bit == 0 )
		{
			SAM_WARN("JS", std::string("sam_monster_has_trait: unknown trait '") + (nameC ? nameC : "")
				+ "'. Valid: boss, trader, untargetable, immobile_turret, never_retreat, "
				  "water_walking, undead, ally_recolour, tinker_construct, no_digestion, pass_through.");
			if ( nameC ) { JS_FreeCString(ctx, nameC); }
			return JS_NewBool(ctx, 0);
		}
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_NewBool(ctx, 0); }
		Stat* st = e->getStats();
		if ( !st ) { return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, samMonsterHasTrait(st, bit) ? 1 : 0);
#else
		(void)uid; if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_NewBool(ctx, 0);
#endif
	}

	// sam_get_item_category(item) -> category name string, or undefined (Lua: nil). `item` is an int id
	// (e.g. an event's item_type) or a name (vanilla or "ns:item").
	JSValue js_sam_get_item_category(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		int type = -1;
		if ( JS_IsNumber(argv[0]) )
		{
			int32_t t = -1; JS_ToInt32(ctx, &t, argv[0]); type = t;
		}
		else
		{
			const char* s = JS_ToCString(ctx, argv[0]);
			std::string name = s ? s : "";
			if ( s ) { JS_FreeCString(ctx, s); }
			if ( name.find(':') != std::string::npos ) { type = SAMItems::itemIdForIdString(name); }
			if ( type < 0 ) { type = SAMCatalog::itemTypeFor(name); }   // shared resolver
		}
		if ( type < 0 || type >= NUM_ITEM_SLOTS ) { return JS_UNDEFINED; }
		const std::string cat = SAMItems::categoryName((int)items[type].category);
		if ( cat.empty() ) { return JS_UNDEFINED; }
		return JS_NewString(ctx, cat.c_str());
#else
		return JS_UNDEFINED;
#endif
	}

	// sam_monster_equip(uid, "slot", "item" [, beatitude [, status [, count]]]) -> boolean.
	// See the Lua twin for why no dirty flag is needed.
	// Own copy of the slot table: the Lua one is internal to its translation unit.
	// Kept in the same order and with the same aliases so the two cannot drift.
	Item** samMonsterSlotJs(Stat* st, const std::string& slotIn)
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
	const char* samMonsterSlotNamesJs()
	{
		return "helmet, breastplate, gloves, shoes, shield, weapon, cloak, amulet, ring, mask";
	}

	// sam_set_monster_name(uid, "name") -> boolean. See the Lua twin for the host-only note
	// and the generic-name trap.
	// ---- v2.5 runtime model control (twins of the Lua bindings) ----------------------
	JSValue js_sam_set_model(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* idC = JS_ToCString(ctx, argv[1]);
		const std::string modelId = idC ? idC : "";
		if ( idC ) { JS_FreeCString(ctx, idC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_model refused: host only."); return JS_FALSE; }
		if ( !SAMBodies::setBodyById((uint32_t)uid, modelId) )
		{
			SAM_ERROR("JS", "sam_set_model: no model registered as '" + modelId + "'.");
			return JS_FALSE;
		}
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	JSValue js_sam_clear_model(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_clear_model refused: host only."); return JS_FALSE; }
		SAMBodies::clearBodyById((uint32_t)uid);
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	JSValue js_sam_get_model(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
#ifdef SAM_JS_HAVE_BARONY
		const std::string id = SAMBodies::bodyIdFor((uint32_t)uid);
		if ( id.empty() ) { return JS_UNDEFINED; }
		return JS_NewString(ctx, id.c_str());
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_set_scale(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; double sc = 1.0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_scale") ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 1, &sc, "sam_set_scale") ) { return JS_FALSE; }
		if ( !std::isfinite(sc) )
		{
			SAM_ERROR("JS", "sam_set_scale: scale must be a finite number.");
			return JS_FALSE;
		}
#ifdef SAM_JS_HAVE_BARONY
		// The SHARED resolver, not uidToEntity: this twin still accepted a sentinel uid and a
		// limb long after the Lua side stopped, because the ship gate compares names, not bodies.
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_set_scale");
		if ( !e ) { return JS_FALSE; }
		if ( !SAMMpEntities::scaleSticks(e, "sam_set_scale") ) { return JS_FALSE; }   // see the Lua twin
		// See the Lua twin: 0 used to become FULL SIZE silently, and the wire cannot carry
		// anything under 1/128.
		if ( !(sc > 0.0) )
		{
			SAM_ERROR("JS", "sam_set_scale: scale must be greater than 0 (was "
				+ std::to_string(sc) + "). To make something disappear use sam_set_visible.");
			return JS_FALSE;
		}
		if ( sc < 1.0 / 128.0 )
		{
			SAM_WARN("JS", "sam_set_scale: " + std::to_string(sc) + " is below the 1/128 the"
				" network can carry, so other players would see it disappear. Clamped.");
			sc = 1.0 / 128.0;
		}
		// Clamped ALWAYS, matching the Lua twin: the wire carries scale as (Uint8)(scale * 128),
		// so a mod authored in singleplayer at 3.0 was silently wrong for everybody else.
		if ( sc > 1.99 )
		{
			SAM_WARN("JS", "sam_set_scale: past the 1.99 the network can carry; clamped.");
			sc = 1.99;
		}
		e->scalex = sc; e->scaley = sc; e->scalez = sc;
		e->flags[UPDATENEEDED] = true;   // without this the 8 Hz sweep never tells a client
		SAMMpEntities::transformed(e, SAMMpEntities::Changed::SCALE);   // pinned entities; see the Lua twin
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	// sam_set_damage_immune(uid, on) -> boolean. See the Lua twin; the two runtimes share one set,
	// so a mod written half in each language sees one answer.
	JSValue js_sam_set_damage_immune(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; bool on = false;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_damage_immune") ) { return JS_FALSE; }
		if ( !samBoolReqJs(ctx, argc, argv, 1, "sam_set_damage_immune", &on) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_set_damage_immune");
		if ( !e ) { return JS_FALSE; }
		// See the Lua twin: only a player or a monster takes damage through Entity::modHP.
		if ( !(e->behavior == &actPlayer || e->behavior == &actMonster) )
		{
			SAM_WARN("JS", "sam_set_damage_immune refused: uid " + std::to_string(uid) + " is not"
				" a player or a monster. Only those take damage through the one place this"
				" immunity lives. Chests, doors, furniture and breakable props carry their own"
				" health that nothing here can reach.");
			return JS_FALSE;
		}
		SAMLua::setDamageImmune((unsigned int)uid, on);
		return JS_TRUE;
#else
		(void)uid; (void)on; return JS_FALSE;
#endif
	}

	// sam_is_damage_immune(uid) -> boolean.
	JSValue js_sam_is_damage_immune(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_is_damage_immune") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		return JS_NewBool(ctx, ( uid > 0 && SAMLua::isDamageImmune((unsigned int)uid) ) ? 1 : 0);
#else
		(void)uid; return JS_FALSE;
#endif
	}

	// sam_move_entity(uid, dxTiles, dyTiles) -> tiles actually moved | undefined. See the Lua twin:
	// clipMove slides along walls and reports the distance it managed, which is the answer a
	// script pushing something needs.
	JSValue js_sam_move_entity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; double dx = 0.0, dy = 0.0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_move_entity") ) { return JS_UNDEFINED; }
		if ( !samJsReqF64(ctx, argc, argv, 1, &dx, "sam_move_entity") ) { return JS_UNDEFINED; }
		if ( !samJsReqF64(ctx, argc, argv, 2, &dy, "sam_move_entity") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_move_entity");
		if ( !e ) { return JS_UNDEFINED; }
		if ( !std::isfinite(dx) || !std::isfinite(dy) )
		{
			SAM_ERROR("JS", "sam_move_entity: the distance must be two finite numbers.");
			return JS_UNDEFINED;
		}
		// See the Lua twin: SAMMpEntities::beginMove / endMove carry the move to the other machines.
		// SUB-STEPPED; see the Lua twin. clipMove tests only the destination point, so one big
		// step walks straight over a wall and reports the full distance as moved.
		real_t samVx = (real_t)(dx * 16.0), samVy = (real_t)(dy * 16.0);
		if ( std::fabs(samVx) > 16.0 * 1024.0 || std::fabs(samVy) > 16.0 * 1024.0 )
		{
			SAM_ERROR("JS", "sam_move_entity: that distance is past the end of any map. Use"
				" sam_set_position to place something somewhere far away.");
			return JS_UNDEFINED;
		}
		const int samSteps = std::max(1, (int)std::ceil(std::sqrt(samVx * samVx + samVy * samVy) / 7.0));
		samVx /= (real_t)samSteps;
		samVy /= (real_t)samSteps;
		double samStartX = 0.0, samStartY = 0.0;
		SAMMpEntities::beginMove(e, samStartX, samStartY);
		const hit_t samSavedHit = hit;   // clipMove's first statement is `hit.entity = NULL;`
		real_t moved = 0.0;
		for ( int samI = 0; samI < samSteps; ++samI )
		{
			const real_t samGot = clipMove(&e->x, &e->y, samVx, samVy, e);
			moved += samGot;
			if ( samGot <= 0.0 ) { break; }
		}
		hit = samSavedHit;
		e->flags[UPDATENEEDED] = true;
		e->flags[NOUPDATE] = false;
		TileEntityList.updateEntity(*e);
		SAMMpEntities::endMove(e, samStartX, samStartY);
		return JS_NewFloat64(ctx, (double)(moved / 16.0));
#else
		(void)uid; (void)dx; (void)dy; return JS_UNDEFINED;
#endif
	}

	// sam_apply_force(uid, force, angle [, ticks]) -> boolean. Calls the SAME shared body as Lua,
	// because which field a shove goes into differs between a player and a monster and that is
	// not knowledge worth having two copies of.
	JSValue js_sam_apply_force(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; double force = 0.0, angle = 0.0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_apply_force") ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 1, &force, "sam_apply_force") ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 2, &angle, "sam_apply_force") ) { return JS_FALSE; }
		int32_t ticks = 30;
		samJsOptI32(ctx, argc, argv, 3, &ticks, "sam_apply_force");
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_apply_force");
		if ( !e ) { return JS_FALSE; }
		if ( !std::isfinite(force) || !std::isfinite(angle) )
		{
			SAM_ERROR("JS", "sam_apply_force: force and angle must be finite numbers.");
			return JS_FALSE;
		}
		if ( force > 7.0 || force < -7.0 )
		{
			SAM_WARN("JS", "sam_apply_force: force is clamped to 7, past which a single step can"
				" jump clean over a wall instead of hitting it.");
			force = ( force > 0.0 ) ? 7.0 : -7.0;
		}
		if ( ticks < 1 ) { ticks = 1; }
		if ( ticks > 3600 ) { ticks = 3600; }
		// See the Lua twin: ENFS packs the angle as (Sint16)(value * 256), so degrees passed by
		// mistake wrap and shove a remote player the wrong way.
		angle = std::fmod(angle, 2.0 * PI);
		if ( angle < 0.0 ) { angle += 2.0 * PI; }
		return JS_NewBool(ctx, SAMLua::applyForceTo(e, force, angle, (int)ticks) ? 1 : 0);
#else
		(void)uid; (void)force; (void)angle; return JS_FALSE;
#endif
	}

	// sam_set_on_fire(uid [, on]) -> boolean. Answers "is it on fire now", which is deliberately
	// NOT what Entity::SetEntityOnFire returns; see the Lua twin for the three meanings hiding in
	// its false, and for why putting fires out belongs in the same function.
	JSValue js_sam_set_on_fire(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_on_fire") ) { return JS_FALSE; }
		const bool on = samBoolArgJs(ctx, argc, argv, 1, true);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_set_on_fire");
		if ( !e ) { return JS_FALSE; }

		if ( !on )
		{
			if ( e->flags[BURNING] )
			{
				e->flags[BURNING] = false;
				e->char_fire = 0;
				if ( multiplayer == SERVER ) { serverUpdateEntityFlag(e, BURNING); }
				if ( e->behavior == &actPlayer )
				{
					messagePlayer(e->skill[2], MESSAGE_STATUS, "%s", Language::get(647));
				}
			}
			return JS_FALSE;   // asked for not-burning, and it is not burning
		}

		if ( e->flags[BURNING] ) { return JS_TRUE; }
		if ( !e->flags[BURNABLE] )
		{
			// See the Lua twin: a monster spawned this frame has not run its species init yet.
			if ( e->behavior == &actMonster && e->skill[3] == 0 )
			{
				SAM_WARN("JS", "sam_set_on_fire: this monster was spawned this frame and has not run"
					" its own setup yet, so it is not burnable YET. Wait a frame (sam_set_timer with"
					" a short delay) and it will light normally.");
			}
			else
			{
				SAM_WARN("JS", "sam_set_on_fire: this entity is not BURNABLE, so the engine will"
					" never light it. sam_set_entity_flag(uid, \"BURNABLE\", true) first if that is"
					" what you want.");
			}
			return JS_FALSE;
		}
		const bool lit = e->SetEntityOnFire(nullptr);
		if ( !lit )
		{
			SAM_WARN("JS", "sam_set_on_fire: this creature resists fire. Skeletons and automatons"
				" never burn, and neither does anyone wearing a machinist apron or an amulet of"
				" burning resistance.");
		}
		else if ( !e->getStats() )
		{
			SAM_WARN("JS", "sam_set_on_fire: this entity has no stats, and the burn timer only runs"
				" for players and monsters, so it will burn for ever and hurt nothing. That is fine"
				" for a brazier. Call sam_set_on_fire(uid, false) to put it out.");
		}
		return JS_NewBool(ctx, lit ? 1 : 0);
#else
		(void)uid; (void)on; return JS_FALSE;
#endif
	}

	// sam_get_entity_flag(uid, "FLAG") -> boolean | undefined. undefined, not false, for an unknown name:
	// false is a real answer, so returning it for a typo hides the mistake. See the Lua twin.
	JSValue js_sam_get_entity_flag(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_entity_flag") ) { return JS_UNDEFINED; }
		if ( !samHasArg(argc, argv, 1) )
		{
			SAM_ERROR("JS", "sam_get_entity_flag: argument 2 (the flag name) is required.");
			return JS_UNDEFINED;
		}
		const char* flagName = JS_ToCString(ctx, argv[1]);
		if ( !flagName ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		const int idx = SAMLua::resolveEntityFlag(flagName, false, "sam_get_entity_flag");
		JS_FreeCString(ctx, flagName);
		if ( idx < 0 ) { return JS_UNDEFINED; }
		Entity* e = SAMLua::resolveReadableEntity((long long)uid, "sam_get_entity_flag");
		if ( !e ) { return JS_UNDEFINED; }
		return JS_NewBool(ctx, e->flags[idx] ? 1 : 0);
#else
		JS_FreeCString(ctx, flagName);
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_set_entity_flag(uid, "FLAG", on) -> boolean. Same names, same refusals, same reasons as
	// the Lua twin, because both call one shared table.
	JSValue js_sam_set_entity_flag(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; bool on = false;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_entity_flag") ) { return JS_FALSE; }
		if ( !samHasArg(argc, argv, 1) )
		{
			SAM_ERROR("JS", "sam_set_entity_flag: argument 2 (the flag name) is required.");
			return JS_FALSE;
		}
		if ( !samBoolReqJs(ctx, argc, argv, 2, "sam_set_entity_flag", &on) ) { return JS_FALSE; }
		const char* flagName = JS_ToCString(ctx, argv[1]);
		if ( !flagName ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		const int idx = SAMLua::resolveEntityFlag(flagName, true, "sam_set_entity_flag");
		JS_FreeCString(ctx, flagName);
		if ( idx < 0 ) { return JS_FALSE; }
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_set_entity_flag");
		if ( !e ) { return JS_FALSE; }
		if ( !SAMMpEntities::flagSticks(e, idx, "sam_set_entity_flag") ) { return JS_FALSE; }   // see the Lua twin
		e->flags[idx] = on;
		if ( multiplayer == SERVER ) { serverUpdateEntityFlag(e, idx); }
		return JS_TRUE;
#else
		JS_FreeCString(ctx, flagName);
		(void)uid; (void)on; return JS_FALSE;
#endif
	}

	// sam_set_entity_size(uid, size [, sizeY]) -> boolean. See the Lua twin for the whole story:
	// the clamp to 0..127 exists because the wire carries the size as one signed byte, and a
	// negative half-extent inverts the client's hitbox test.
	JSValue js_sam_set_entity_size(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Read WIDE and clamp wide. samJsReqI32 casts to int32 before returning, so 2^31 arrived
		// here as INT32_MIN and the clamp below turned it into 0 -- the "nothing collides with
		// this" value -- while the Lua twin, which keeps the number in a long long all the way to
		// its clamp, produced 127 from the same input. Same warning printed, opposite hitboxes.
		int64_t uid = 0, sx64 = 0, sy64 = 0;
		double samSxD = 0.0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_entity_size") ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 1, &samSxD, "sam_set_entity_size") ) { return JS_FALSE; }
		double samSyD = samSxD;
		samJsOptF64(ctx, argc, argv, 2, &samSyD, "sam_set_entity_size");
		// Whole numbers only, as in Lua: luaL_checkinteger / luaL_optinteger RAISE on 2.5, and this
		// twin quietly truncated it to 2 and reported success.
		if ( samSxD != std::floor(samSxD) || samSyD != std::floor(samSyD) )
		{
			SAM_ERROR("JS", "sam_set_entity_size: sizes must be whole numbers.");
			return JS_FALSE;
		}
		sx64 = (int64_t)samSxD;
		sy64 = (int64_t)samSyD;
		int32_t sx = (int32_t)std::max<int64_t>(-1, std::min<int64_t>(128, sx64));
		int32_t sy = (int32_t)std::max<int64_t>(-1, std::min<int64_t>(128, sy64));
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_set_entity_size");
		if ( !e ) { return JS_FALSE; }
		if ( !SAMMpEntities::sizeReachesOwner(e, "sam_set_entity_size") ) { return JS_FALSE; }   // see the Lua twin
		if ( sx < 0 || sx > 127 || sy < 0 || sy > 127 )
		{
			SAM_WARN("JS", "sam_set_entity_size: sizes are clamped to 0..127, because the network"
				" carries them as one signed byte and a larger number arrives negative, which turns"
				" the hitbox inside-out on every other player's machine.");
		}
		if ( sx < 0 ) { sx = 0; } if ( sx > 127 ) { sx = 127; }
		if ( sy < 0 ) { sy = 0; } if ( sy > 127 ) { sy = 127; }
		e->sizex = (Sint32)sx;
		e->sizey = (Sint32)sy;
		e->flags[UPDATENEEDED] = true;
		SAMMpEntities::transformed(e, SAMMpEntities::Changed::SIZE);   // see the Lua twin
		return JS_TRUE;
#else
		(void)uid; (void)sx; (void)sy; return JS_FALSE;
#endif
	}

	// sam_set_elevation(uid, z) -> boolean. Same z sam_get_position_precise returns, and in
	// Barony that axis points DOWN, so negative is up. Refuses players and monsters because their
	// species code rewrites z every frame; see the Lua twin.
	JSValue js_sam_set_elevation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; double z = 0.0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_elevation") ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 1, &z, "sam_set_elevation") ) { return JS_FALSE; }
		if ( !std::isfinite(z) )
		{
			SAM_ERROR("JS", "sam_set_elevation: z must be a finite number.");
			return JS_FALSE;
		}
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_set_elevation");
		if ( !e ) { return JS_FALSE; }
		if ( e->behavior == &actPlayer || e->behavior == &actMonster || SAMLua::isCompanionEntity(e) )
		{
			SAM_WARN("JS", "sam_set_elevation refused: a player's, a monster's or a companion's"
				" height is rewritten every frame by code that owns it (a companion by its own"
				" hover curve), so this would be erased before the next frame drew. Use a"
				" levitation effect for a creature; a companion's float height is fixed.");
			return JS_FALSE;
		}
		if ( z < -1023.0 || z > 1023.0 )
		{
			SAM_WARN("JS", "sam_set_elevation: z is clamped to -1023..1023, which is what the"
				" network can carry. Negative is up.");
			z = ( z < 0.0 ) ? -1023.0 : 1023.0;
		}
		e->z = z;
		e->new_z = z;
		e->flags[UPDATENEEDED] = true;
		SAMMpEntities::transformed(e, SAMMpEntities::Changed::HEIGHT);   // see the Lua twin
		return JS_TRUE;
#else
		(void)uid; (void)z; return JS_FALSE;
#endif
	}

	JSValue js_sam_set_visible(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// A missing flag is REFUSED in both runtimes now, rather than read as "hide". See the
		// Lua twin: guessing wrong on a flag stays invisible until the wrong thing is on screen.
		int64_t uid = 0; bool vis = false;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_visible") ) { return JS_FALSE; }
		if ( !samBoolReqJs(ctx, argc, argv, 1, "sam_set_visible", &vis) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_set_visible");
		if ( !e ) { return JS_FALSE; }
		// See the Lua twin: creatures and ground items rewrite INVISIBLE every frame on the host.
		if ( !SAMMpEntities::visibilitySticks(e, "sam_set_visible") ) { return JS_FALSE; }
		e->flags[INVISIBLE] = !vis;
		// See the Lua twin: a custom model is drawn at the draw site, so the draw pass has to be
		// told this hide came from a script rather than from an empty equipment slot.
		SAMBodies::noteScriptVisibility((uint32_t)e->getUID(), vis);
		if ( multiplayer == SERVER ) { serverUpdateEntityFlag(e, INVISIBLE); }
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	JSValue js_sam_set_monster_name(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
		const std::string newName = nameC ? nameC : "";
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_monster_name refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("JS", "sam_set_monster_name: no monster uid " + std::to_string((long long)uid)); return JS_FALSE; }
		Stat* st = e->getStats();
		if ( !st ) { return JS_FALSE; }
		stringCopy(st->name, newName.c_str(), sizeof(st->name), newName.size());
		SAMMonsters::syncFollowerName(e);   // see the Lua twin
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	JSValue js_sam_monster_equip(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* slotC = JS_ToCString(ctx, argv[1]);
		const char* itemC = JS_ToCString(ctx, argv[2]);
		const std::string slotName = slotC ? slotC : "";
		const std::string itemName = itemC ? itemC : "";
		if ( slotC ) { JS_FreeCString(ctx, slotC); }
		if ( itemC ) { JS_FreeCString(ctx, itemC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_monster_equip refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("JS", "sam_monster_equip: no monster uid " + std::to_string((long long)uid)); return JS_FALSE; }
		Stat* st = e->getStats();
		Item** slot = samMonsterSlotJs(st, slotName);
		if ( !slot )
		{
			SAM_ERROR("JS", "sam_monster_equip: unknown slot '" + slotName + "'. Valid: " + samMonsterSlotNamesJs());
			return JS_FALSE;
		}
		int resolvedType = -1;
		if ( itemName.find(':') != std::string::npos ) { resolvedType = SAMItems::itemIdForIdString(itemName); }
		// One resolver, shared with every other name-taking call and with the Lua twin: digits,
		// "ns:id", the internal name, then the DISPLAYED name.
		if ( resolvedType < 0 ) { resolvedType = SAMCatalog::itemTypeFor(itemName); }
		if ( resolvedType < 0 )
		{
			SAM_ERROR("JS", "sam_monster_equip: unknown item '" + itemName
				+ "' (expected a vanilla name like \"IRON_DAGGER\" or a custom \"namespace:item\").");
			return JS_FALSE;
		}
		int32_t beatitude = 0; samJsOptI32(ctx, argc, argv, 3, &beatitude, __func__);
		int32_t statusArg = (int32_t)EXCELLENT; samJsOptI32(ctx, argc, argv, 4, &statusArg, __func__);
		statusArg = samClampInt(statusArg, (int)BROKEN, (int)EXCELLENT);
		int32_t count = 1; samJsOptI32(ctx, argc, argv, 5, &count, __func__);
		if ( count < 1 ) { count = 1; }
		Item* item = newItem(static_cast<ItemType>(resolvedType), static_cast<Status>(statusArg),
			(Sint16)beatitude, count, 0, true, nullptr);
		if ( !item ) { return JS_FALSE; }
		e->monsterEquipItem(*item, slot);
		SAM_INFO("SAM", "sam_monster_equip: " + itemName + " -> " + slotName + " on uid " + std::to_string((long long)uid));
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	// sam_monster_unequip(uid, "slot") -> boolean.
	JSValue js_sam_monster_unequip(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* slotC = JS_ToCString(ctx, argv[1]);
		const std::string slotName = slotC ? slotC : "";
		if ( slotC ) { JS_FreeCString(ctx, slotC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_monster_unequip refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_FALSE; }
		Stat* st = e->getStats();
		Item** slot = samMonsterSlotJs(st, slotName);
		if ( !slot )
		{
			SAM_ERROR("JS", "sam_monster_unequip: unknown slot '" + slotName + "'. Valid: " + samMonsterSlotNamesJs());
			return JS_FALSE;
		}
		if ( !*slot ) { return JS_FALSE; }
		// The WHOLE stack. dropItemMonster drops one unit unless told otherwise (items.cpp), and
		// clearing the slot after that orphaned the rest of a stack of throwing weapons. Given the
		// full count it empties the item, clears the slot and frees it itself.
		dropItemMonster(*slot, e, st, (*slot)->count);
		*slot = nullptr;
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	// sam_attach_behavior(uid, "name") -> boolean. See the Lua twin.
	JSValue js_sam_attach_behavior(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
		const std::string name = nameC ? nameC : "";
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_attach_behavior refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("JS", "sam_attach_behavior: no monster uid " + std::to_string((long long)uid)); return JS_FALSE; }
		// The same ':' rule sam_register_behavior uses. Prefixing unconditionally meant the
		// "mymod:sentry" the docs tell you to register with became "mymod:mymod:sentry" here, so
		// only the bare form ever survived the round trip.
		std::string full = name;
		if ( full.find(':') == std::string::npos ) { full = g_currentNs + ":" + full; }
		const int idx = SAMLua::behaviorIndexFor(full);
		if ( idx < 0 )
		{
			SAM_ERROR("JS", "sam_attach_behavior: no behavior named '" + full
				+ "' — register it with sam_register_behavior first.");
			return JS_FALSE;
		}
		SAMLua::attachMonsterBehavior((unsigned long long)uid, idx);
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	// sam_detach_behavior(uid) -> boolean.
	JSValue js_sam_detach_behavior(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		SAMLua::detachMonsterBehavior((unsigned long long)uid);
		return JS_TRUE;
	}

	JSValue js_sam_set_monster_stat(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Checked, as luaL_checkinteger checks them in Lua: an undefined value used to become 0,
		// so sam_set_monster_stat(uid, "HP", undefined) killed the monster in JS.
		int64_t uid = 0; int32_t value = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_monster_stat") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &value, "sam_set_monster_stat") ) { return JS_FALSE; }
		const char* nameC = JS_ToCString(ctx, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_monster_stat refused: host only."); if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("JS", "sam_set_monster_stat: no monster uid " + std::to_string(uid)); if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		Stat* s = e->getStats();
		const std::string n = samUpper(nameC);
		bool ok = true;
		if      ( n == "HP" )    { e->setHP(value); }
		else if ( n == "MAXHP" ) { s->MAXHP = samClampInt(value, 1, SAMLua::STAT_WIRE_MAX); if ( s->HP > s->MAXHP ) { e->setHP(s->MAXHP); } SAMMonsters::syncFollowerSheet(e); }   // see the Lua twin
		else if ( n == "MP" )    { e->setMP(value); }
		else if ( n == "MAXMP" ) { s->MAXMP = (value < 0 ? 0 : value); if ( s->MP > s->MAXMP ) { s->MP = s->MAXMP; } }
		else if ( n == "STR" )   { s->STR = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "DEX" || n == "SPEED" ) { s->DEX = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "CON" )   { s->CON = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "INT" )   { s->INT = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "PER" )   { s->PER = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "CHR" )   { s->CHR = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "LEVEL" || n == "LVL" ) { s->LVL = samClampInt(value, 1, 255); SAMMonsters::syncFollowerSheet(e); }
		else { SAM_WARN("JS", std::string("sam_set_monster_stat: unknown stat '") + (nameC ? nameC : "") + "'"); ok = false; }
		if ( ok ) { SAM_INFO("SAM", "sam_set_monster_stat: " + n + "=" + std::to_string(value) + " on uid " + std::to_string(uid)); }
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		return ok ? JS_TRUE : JS_FALSE;
#else
		(void)uid; (void)value; if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE;
#endif
	}

	JSValue js_sam_apply_monster_effect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Checked, as luaL_checkinteger / luaL_checkstring check them in Lua: an undefined tick
		// count used to become 0 and was applied as such.
		int64_t uid = 0; int32_t ticks = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_apply_monster_effect") ) { return JS_FALSE; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_apply_monster_effect: argument 2 is required."); return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &ticks, "sam_apply_monster_effect") ) { return JS_FALSE; }
		const char* nameC = JS_ToCString(ctx, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_apply_monster_effect refused: host only."); if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("JS", "sam_apply_monster_effect: no monster uid " + std::to_string(uid)); if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { SAM_WARN("JS", std::string("sam_apply_monster_effect: unknown effect '") + (nameC ? nameC : "") + "'"); if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		const bool ok = e->setEffect(eff, true, ticks, true);
		SAM_INFO("SAM", std::string("sam_apply_monster_effect: ") + (nameC ? nameC : "") + " to uid " + std::to_string(uid) + (ok ? "" : " (immune)"));
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		return ok ? JS_TRUE : JS_FALSE;
#else
		(void)uid; (void)ticks; if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE;
#endif
	}

	// v1.5.0 — monster status-effect read/remove parity (twins of the Lua bindings).
	JSValue js_sam_remove_monster_effect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_remove_monster_effect refused: host only."); if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		const int eff = samEffectNameToId(nameC);
		if ( eff < 0 ) { if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		e->setEffect(eff, false, 0, true);
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		return JS_TRUE;
#else
		(void)uid; if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE;
#endif
	}

	JSValue js_sam_get_monster_effect_duration(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_NewInt32(ctx, 0); }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		int out = 0;
		if ( e ) { const int eff = samEffectNameToId(nameC); if ( eff >= 0 && e->getStats()->getEffectActive(eff) != 0 ) { out = (int)e->getStats()->EFFECTS_TIMERS[eff]; } }
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		return JS_NewInt32(ctx, out);
#else
		(void)uid; if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_NewInt32(ctx, 0);
#endif
	}

	JSValue js_sam_get_monster_effect_strength(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_NewInt32(ctx, 0); }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		int out = 0;
		if ( e ) { const int eff = samEffectNameToId(nameC); if ( eff >= 0 ) { out = (int)e->getStats()->getEffectActive(eff); } }
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		return JS_NewInt32(ctx, out);
#else
		(void)uid; if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_NewInt32(ctx, 0);
#endif
	}

	JSValue js_sam_get_monster_effects(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_monster_effects") ) { return JS_NewArray(ctx); }
		JSValue arr = JS_NewArray(ctx);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return arr; }
		Stat* s = e->getStats();
		uint32_t n = 0;
		for ( int id = 0; id < NUMEFFECTS; ++id )
		{
			const Uint8 strength = s->getEffectActive(id);
			if ( strength == 0 ) { continue; }
			std::string name = SAMLua::effectNameFromId(id);
			if ( name.empty() ) { name = "CUSTOM:" + std::to_string(id); }
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name.c_str()));
			JS_SetPropertyStr(ctx, o, "ticks", JS_NewInt32(ctx, (int32_t)s->EFFECTS_TIMERS[id]));
			JS_SetPropertyStr(ctx, o, "strength", JS_NewInt32(ctx, (int32_t)strength));
			JS_SetPropertyUint32(ctx, arr, n++, o);
		}
		return arr;
#else
		(void)uid; return arr;
#endif
	}

	JSValue js_sam_kill_monster(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_kill_monster refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("JS", "sam_kill_monster: no monster uid " + std::to_string(uid)); return JS_FALSE; }
		e->setHP(0); // actMonster runs death + drops on its next tick; fires on_monster_died
		SAM_INFO("SAM", "sam_kill_monster: uid " + std::to_string(uid));
		return JS_TRUE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	JSValue js_sam_spawn_monsters(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Checked, as luaL_checkinteger / luaL_checkstring check them in Lua.
		int64_t nearUid = 0; int32_t count = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &nearUid, "sam_spawn_monsters") ) { return JS_NewInt32(ctx, 0); }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_spawn_monsters: argument 2 is required."); return JS_NewInt32(ctx, 0); }
		if ( !samJsReqI32(ctx, argc, argv, 2, &count, "sam_spawn_monsters") ) { return JS_NewInt32(ctx, 0); }
		const char* typeC = JS_ToCString(ctx, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_monsters refused: host only."); if ( typeC ) { JS_FreeCString(ctx, typeC); } return JS_NewInt32(ctx, 0); }
		Entity* anchor = uidToEntity((Sint32)nearUid);
		if ( !anchor ) { SAM_WARN("JS", "sam_spawn_monsters: no anchor entity uid " + std::to_string(nearUid)); if ( typeC ) { JS_FreeCString(ctx, typeC); } return JS_NewInt32(ctx, 0); }
		const int mtype = samMonsterNameToId(typeC);
		// <= 0, as sam_spawn_monster refuses: id 0 is "nothing", not a creature.
		if ( mtype <= 0 ) { SAM_WARN("JS", std::string("sam_spawn_monsters: unknown monster type '") + (typeC ? typeC : "") + "'"); if ( typeC ) { JS_FreeCString(ctx, typeC); } return JS_NewInt32(ctx, 0); }
		if ( count < 1 ) { count = 1; }
		if ( count > 8 ) { count = 8; } // hard cap per spec
		int spawned = 0;
		for ( int i = 0; i < count; ++i )
		{
			Entity* m = summonMonster((Monster)mtype, anchor->x, anchor->y); // finds a free adjacent tile itself
			if ( m ) { ++spawned; }
		}
		SAM_INFO("SAM", "sam_spawn_monsters: " + std::to_string(spawned) + "x " + (typeC ? typeC : "") + " near uid " + std::to_string(nearUid));
		if ( typeC ) { JS_FreeCString(ctx, typeC); }
		return JS_NewInt32(ctx, spawned);
#else
		(void)nearUid; (void)count; if ( typeC ) { JS_FreeCString(ctx, typeC); } return JS_NewInt32(ctx, 0);
#endif
	}

	JSValue js_sam_get_monster_target(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_monster_target") ) { return JS_NewInt32(ctx, -1); }
#ifdef SAM_JS_HAVE_BARONY
		int idx = -1;
		if ( Entity* e = samResolveMonster(uid) )
		{
			Entity* t = uidToEntity((Sint32)e->monsterTarget);
			if ( t && t->behavior == &actPlayer ) { idx = t->skill[2]; }
		}
		return JS_NewInt32(ctx, idx);
#else
		(void)uid; return JS_NewInt32(ctx, -1);
#endif
	}

	JSValue js_sam_set_monster_target(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Checked: a bare JS_ToInt32 turned undefined or NaN into player 0, so the monster silently
		// hunted the HOST -- invisible in singleplayer, where 0 is the only player.
		int64_t uid = 0; int32_t player = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_monster_target") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &player, "sam_set_monster_target") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_monster_target refused: host only."); return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("JS", "sam_set_monster_target: no monster uid " + std::to_string(uid)); return JS_FALSE; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_WARN("JS", "sam_set_monster_target: invalid player " + std::to_string(player)); return JS_FALSE; }
		e->monsterAcquireAttackTarget(*players[player]->entity, MONSTER_STATE_PATH);
		SAM_INFO("SAM", "sam_set_monster_target: uid " + std::to_string(uid) + " -> player " + std::to_string(player));
		return JS_TRUE;
#else
		(void)uid; (void)player; return JS_FALSE;
#endif
	}

	// sam_get_monster_data(uid, key) -> value (undefined if unset). Per-monster scratch store,
	// shared with the Lua runtime via SAMLua::monsterDataGet.
	JSValue js_sam_get_monster_data(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_UNDEFINED; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* keyC = JS_ToCString(ctx, argv[1]);
		const std::string js = SAMLua::monsterDataGet((unsigned)(Sint32)uid, keyC ? keyC : "");
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( js.empty() ) { return JS_UNDEFINED; }
		JSValue v = JS_ParseJSON(ctx, js.c_str(), js.size(), "sam_monster_data");
		if ( JS_IsException(v) ) { JS_FreeValue(ctx, v); return JS_UNDEFINED; }
		return v;
	}

	// sam_set_monster_data(uid, key, value) — store any JSON-able value for a monster.
	JSValue js_sam_set_monster_data(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* keyC = JS_ToCString(ctx, argv[1]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		std::string json;
		if ( !samJsStringifyForStore(ctx, argv[2], "sam_set_monster_data", json) ) { return JS_FALSE; }
		SAMLua::monsterDataSet((unsigned)(Sint32)uid, key, json);
		return JS_TRUE;
	}

	// sam_get_player_data(player, key) / sam_set_player_data(player, key, value) — per-player,
	// in-memory, per-session scratch (cooldowns/flags/stacks). Shares SAMLua's store with Lua.
	JSValue js_sam_get_player_data(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_player_data") ) { return JS_UNDEFINED; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_get_player_data: argument 2 is required."); return JS_UNDEFINED; }
		const char* keyC = JS_ToCString(ctx, argv[1]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( player < 0 || player >= MAXPLAYERS ) { return JS_UNDEFINED; }
		const std::string js = SAMLua::playerDataGet(player, key);
		if ( js.empty() ) { return JS_UNDEFINED; }
		JSValue v = JS_ParseJSON(ctx, js.c_str(), js.size(), "sam_player_data");
		if ( JS_IsException(v) ) { JS_FreeValue(ctx, v); return JS_UNDEFINED; }
		return v;
	}

	JSValue js_sam_set_player_data(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Returns nothing, like the Lua twin and the docs. A missing or unreadable player used to
		// become player 0 here while Lua raised.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_set_player_data") ) { return JS_UNDEFINED; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_set_player_data: argument 2 is required."); return JS_UNDEFINED; }
		const char* keyC = JS_ToCString(ctx, argv[1]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( player < 0 || player >= MAXPLAYERS ) { return JS_UNDEFINED; }
		std::string json;
		if ( !samJsStringifyForStore(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "sam_set_player_data", json) ) { return JS_UNDEFINED; }
		SAMLua::playerDataSet(player, key, json);
		return JS_UNDEFINED;
	}

	// sam_get_effect_duration(player, "EFFECT") -> remaining ticks (0 if inactive, -1 permanent).
	JSValue js_sam_get_effect_duration(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua. 0 ticks is the real answer for "not
		// affected", so a missing player must not borrow it.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_effect_duration") ) { return JS_UNDEFINED; }
		std::string name; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NewInt32(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 || stats[player]->getEffectActive(eff) == 0 ) { return JS_NewInt32(ctx, 0); }
		return JS_NewInt32(ctx, (int32_t)stats[player]->EFFECTS_TIMERS[eff]);
	}

	// sam_get_effect_strength(player, "EFFECT") -> strength/tier (0 if inactive).
	JSValue js_sam_get_effect_strength(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua; see sam_get_stat. undefined, not the 0 this answers for an effect
		// that is not active: 0 is a real strength.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_effect_strength") ) { return JS_UNDEFINED; }
		std::string name; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NewInt32(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 ) { return JS_NewInt32(ctx, 0); }
		return JS_NewInt32(ctx, (int32_t)stats[player]->getEffectActive(eff));
	}

	// sam_get_effects(player) -> array of { name, ticks, strength } for every active effect.
	JSValue js_sam_get_effects(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as luaL_checkinteger makes it in Lua.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_effects") ) { return JS_NewArray(ctx); }
		JSValue arr = JS_NewArray(ctx);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return arr; }
		uint32_t n = 0;
		for ( int id = 0; id < NUMEFFECTS; ++id )
		{
			const Uint8 strength = stats[player]->getEffectActive(id);
			if ( strength == 0 ) { continue; }
			std::string name = SAMLua::effectNameFromId(id);
			if ( name.empty() ) { name = "CUSTOM:" + std::to_string(id); }
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name.c_str()));
			JS_SetPropertyStr(ctx, o, "ticks", JS_NewInt32(ctx, (int32_t)stats[player]->EFFECTS_TIMERS[id]));
			JS_SetPropertyStr(ctx, o, "strength", JS_NewInt32(ctx, (int32_t)strength));
			JS_SetPropertyUint32(ctx, arr, n++, o);
		}
		return arr;
	}

	// ---- v0.7.0 Feature 5: modify existing content (patch class/item/monster) -----
#ifdef SAM_JS_HAVE_BARONY
	int samJsResolveClass(JSContext* ctx, JSValueConst v)
	{
		if ( JS_IsNumber(v) )
		{
			int32_t n = 0; JS_ToInt32(ctx, &n, v);
			if ( (n >= 0 && n < NUMCLASSES) || (n >= SAM_CLASS_ID_BASE && SAMClasses::getClass(n)) ) { return n; }
			return -1;
		}
		if ( JS_IsString(v) )
		{
			const char* s = JS_ToCString(ctx, v);
			const int id = s ? SAMClasses::classIdForIdString(s) : -1;
			if ( s ) { JS_FreeCString(ctx, s); }
			return id;
		}
		return -1;
	}
	int samJsResolveItem(JSContext* ctx, JSValueConst v)
	{
		if ( JS_IsNumber(v) ) { int32_t n = 0; JS_ToInt32(ctx, &n, v); return n; }
		if ( JS_IsString(v) )
		{
			const char* s = JS_ToCString(ctx, v);
			int id = -1;
			if ( s )
			{
				std::string lower = s;
				for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
				auto it = ItemTooltips.itemNameStringToItemID.find(lower);
				id = (it != ItemTooltips.itemNameStringToItemID.end()) ? it->second : SAMItems::itemIdForIdString(s);
				JS_FreeCString(ctx, s);
			}
			return id;
		}
		return -1;
	}
	int samJsResolvePassive(JSContext* ctx, JSValueConst v)
	{
		if ( JS_IsNumber(v) ) { int32_t n = 0; JS_ToInt32(ctx, &n, v); return n; }
		if ( JS_IsString(v) )
		{
			const char* s = JS_ToCString(ctx, v);
			const int id = s ? samEffectNameToId(s) : -1;
			if ( s ) { JS_FreeCString(ctx, s); }
			return id;
		}
		return -1;
	}
	bool samJsKeyEq(const char* a, const char* b)
	{
		for ( ; *a && *b; ++a, ++b ) { if ( std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b) ) { return false; } }
		return *a == '\0' && *b == '\0';
	}
	// Exact key first; then a case-insensitive walk of the object's own keys, so that
	// { Weight: 3 } and { WEIGHT: 3 } work the way they already do in the Lua bindings,
	// which upper-case every key before comparing. Before this the JS side silently
	// ignored any key that was not exactly lower-case and the call still reported success.
	JSValue samJsGetPropCI(JSContext* ctx, JSValueConst obj, const char* key)
	{
		JSValue v = JS_GetPropertyStr(ctx, obj, key);
		if ( !JS_IsUndefined(v) ) { return v; }
		JS_FreeValue(ctx, v);
		JSValue found = JS_UNDEFINED;
		JSPropertyEnum* tab = nullptr; uint32_t plen = 0;
		if ( JS_GetOwnPropertyNames(ctx, &tab, &plen, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 )
		{
			for ( uint32_t i = 0; i < plen; ++i )
			{
				if ( JS_IsUndefined(found) )
				{
					const char* kc = JS_AtomToCString(ctx, tab[i].atom);
					if ( kc && samJsKeyEq(kc, key) ) { found = JS_GetProperty(ctx, obj, tab[i].atom); }
					if ( kc ) { JS_FreeCString(ctx, kc); }
				}
				JS_FreeAtom(ctx, tab[i].atom);
			}
			js_free(ctx, tab);
		}
		return found;
	}
	bool samJsGetIntProp(JSContext* ctx, JSValueConst obj, const char* key, int& out)
	{
		JSValue v = samJsGetPropCI(ctx, obj, key);
		bool ok = false;
		if ( JS_IsNumber(v) ) { int32_t n = 0; JS_ToInt32(ctx, &n, v); out = n; ok = true; }
		JS_FreeValue(ctx, v);
		return ok;
	}
	bool samJsGetStrProp(JSContext* ctx, JSValueConst obj, const char* key, std::string& out)
	{
		JSValue v = samJsGetPropCI(ctx, obj, key);
		bool ok = false;
		if ( JS_IsString(v) ) { const char* s = JS_ToCString(ctx, v); if ( s ) { out = s; JS_FreeCString(ctx, s); ok = true; } }
		JS_FreeValue(ctx, v);
		return ok;
	}
#endif

	// ===== mod actions and mod settings (sam_settings.cpp) -- the Lua twins' rules, one for one =====

#ifdef SAM_JS_HAVE_BARONY
	// A JS value as a setting value: a boolean, a number or a string, and nothing else.
	static void samJsToSettingValue(JSContext* ctx, JSValueConst v, SAMSettings::Value& out)
	{
		out = SAMSettings::Value();
		if ( JS_IsBool(v) ) { out.kind = SAMSettings::Kind::Bool; out.flag = ( JS_ToBool(ctx, v) > 0 ); }
		else if ( JS_IsNumber(v) )
		{
			double d = 0.0;
			if ( JS_ToFloat64(ctx, &d, v) == 0 ) { out.kind = SAMSettings::Kind::Number; out.number = d; }
		}
		else if ( JS_IsString(v) )
		{
			const char* s = JS_ToCString(ctx, v);
			if ( s ) { out.kind = SAMSettings::Kind::String; out.text = s; JS_FreeCString(ctx, s); }
		}
	}

	static JSValue samJsFromSettingValue(JSContext* ctx, const SAMSettings::Value& v)
	{
		switch ( v.kind )
		{
			case SAMSettings::Kind::Number: return JS_NewFloat64(ctx, v.number);   // a whole number prints as 2, not 2.0
			case SAMSettings::Kind::Bool:   return JS_NewBool(ctx, v.flag ? 1 : 0);
			case SAMSettings::Kind::String: return JS_NewString(ctx, v.text.c_str());
			default: return JS_UNDEFINED;
		}
	}

	// samJsGetIntProp's twin for a double: min, max and step are not integers.
	static bool samJsGetNumProp(JSContext* ctx, JSValueConst obj, const char* key, double& out)
	{
		JSValue v = samJsGetPropCI(ctx, obj, key);
		bool ok = false;
		if ( JS_IsNumber(v) ) { double d = 0.0; if ( JS_ToFloat64(ctx, &d, v) == 0 ) { out = d; ok = true; } }
		JS_FreeValue(ctx, v);
		return ok;
	}

	// The object sam_register_setting takes: the Lua twin's keys, case-insensitive, a number
	// only from a number and a string only from a string.
	static bool samJsReadSettingSpec(JSContext* ctx, JSValueConst obj, SAMSettings::Spec& spec, std::string& why)
	{
		std::string sv;
		if ( !samJsGetStrProp(ctx, obj, "type", sv) )
		{
			why = "'" + spec.id + "': the object needs type: \"slider\" | \"toggle\" | \"dropdown\" | \"number\" | \"text\"";
			return false;
		}
		if ( !SAMSettings::typeFromName(sv, spec.type) )
		{
			why = "'" + spec.id + "': unknown type \"" + sv + "\" (slider, toggle, dropdown, number or text)";
			return false;
		}
		if ( samJsGetStrProp(ctx, obj, "label", sv) ) { spec.label = sv; }
		if ( samJsGetStrProp(ctx, obj, "tip", sv) || samJsGetStrProp(ctx, obj, "tooltip", sv) ) { spec.tooltip = sv; }
		double d = 0.0;
		if ( samJsGetNumProp(ctx, obj, "min", d) ) { spec.hasMin = true; spec.min = d; }
		if ( samJsGetNumProp(ctx, obj, "max", d) ) { spec.hasMax = true; spec.max = d; }
		if ( samJsGetNumProp(ctx, obj, "step", d) ) { spec.step = d; }
		JSValue def = samJsGetPropCI(ctx, obj, "default");
		samJsToSettingValue(ctx, def, spec.def);
		JS_FreeValue(ctx, def);
		JSValue opts = samJsGetPropCI(ctx, obj, "options");
		if ( JS_IsArray(opts) )
		{
			int64_t len = 0;
			JSValue lenv = JS_GetPropertyStr(ctx, opts, "length");
			JS_ToInt64(ctx, &len, lenv);
			JS_FreeValue(ctx, lenv);
			for ( int64_t i = 0; i < len && i < 256; ++i )
			{
				JSValue o = JS_GetPropertyUint32(ctx, opts, (uint32_t)i);
				if ( JS_IsString(o) ) { const char* s = JS_ToCString(ctx, o); if ( s ) { spec.options.push_back(s); JS_FreeCString(ctx, s); } }
				JS_FreeValue(ctx, o);
			}
		}
		JS_FreeValue(ctx, opts);
		return true;
	}
#endif

	// An optional string argument, or `fallback` when it is missing, null or undefined.
	static std::string samJsStrArgOr(JSContext* ctx, int argc, JSValueConst* argv, int i, const char* fallback)
	{
		std::string out = fallback;
		if ( samHasArg(argc, argv, i) )
		{
			const char* s = JS_ToCString(ctx, argv[i]);
			if ( s ) { out = s; JS_FreeCString(ctx, s); }
		}
		return out;
	}

	// sam_register_action(id, label, default_key [, default_pad]) -> bool. JS twin.
	JSValue js_sam_register_action(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string id;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_register_action", &id) ) { return JS_FALSE; }
		const std::string label = samJsStrArgOr(ctx, argc, argv, 1, "");
		const std::string key = samJsStrArgOr(ctx, argc, argv, 2, "");
		const std::string pad = samJsStrArgOr(ctx, argc, argv, 3, "");
#ifdef SAM_JS_HAVE_BARONY
		if ( g_currentNs.empty() ) { SAM_WARN("JS", "sam_register_action: no owning mod namespace; ignored."); return JS_FALSE; }
		std::string why;
		if ( !SAMSettings::registerAction(g_currentNs, id, label, key, pad, &why) )
		{
			SAM_WARN("JS", "sam_register_action refused: " + why);
			return JS_FALSE;
		}
		return JS_TRUE;
#else
		(void)label; (void)key; (void)pad;
		return JS_FALSE;
#endif
	}

	// sam_register_setting(id, { type, label, tip, default, min, max, step, options }) -> bool. JS twin.
	JSValue js_sam_register_setting(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string id;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_register_setting", &id) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( g_currentNs.empty() ) { SAM_WARN("JS", "sam_register_setting: no owning mod namespace; ignored."); return JS_FALSE; }
		if ( !samHasArg(argc, argv, 1) || !JS_IsObject(argv[1]) )
		{
			SAM_WARN("JS", "sam_register_setting('" + id + "'): the second argument must be an object { type, label, default, ... }.");
			return JS_FALSE;
		}
		SAMSettings::Spec spec;
		spec.id = id;
		std::string why;
		if ( !samJsReadSettingSpec(ctx, argv[1], spec, why) || !SAMSettings::registerSetting(g_currentNs, spec, &why) )
		{
			SAM_WARN("JS", "sam_register_setting refused: " + why);
			return JS_FALSE;
		}
		return JS_TRUE;
#else
		return JS_FALSE;
#endif
	}

	// sam_get_setting(id) -> number | boolean | string | undefined. JS twin.
	JSValue js_sam_get_setting(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string id;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_get_setting", &id) ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		const SAMSettings::Setting* s = SAMSettings::find(g_currentNs, id);
		if ( !s ) { return JS_UNDEFINED; }
		return samJsFromSettingValue(ctx, s->value);
#else
		return JS_UNDEFINED;
#endif
	}

	// sam_set_setting(id, value) -> bool. JS twin.
	JSValue js_sam_set_setting(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string id;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_set_setting", &id) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		SAMSettings::Value v;
		samJsToSettingValue(ctx, samHasArg(argc, argv, 1) ? argv[1] : JS_UNDEFINED, v);
		std::string why;
		if ( !SAMSettings::set(g_currentNs, id, v, SAMSettings::Source::Script, &why) )
		{
			SAM_WARN("JS", "sam_set_setting refused: " + why);
			return JS_FALSE;
		}
		return JS_TRUE;
#else
		return JS_FALSE;
#endif
	}

	// sam_list_settings() -> array of { id, type, label, value, default, min?, max?, step?, options? }. JS twin.
	JSValue js_sam_list_settings(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		SAMLogger::noteApiCall();
		JSValue arr = JS_NewArray(ctx);
#ifdef SAM_JS_HAVE_BARONY
		uint32_t n = 0;
		for ( const SAMSettings::Setting& s : SAMSettings::settings() )
		{
			if ( s.ns != g_currentNs ) { continue; }
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "id", JS_NewString(ctx, s.id.c_str()));
			JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, SAMSettings::typeName(s.type)));
			JS_SetPropertyStr(ctx, o, "label", JS_NewString(ctx, s.label.c_str()));
			JS_SetPropertyStr(ctx, o, "value", samJsFromSettingValue(ctx, s.value));
			JS_SetPropertyStr(ctx, o, "default", samJsFromSettingValue(ctx, s.def));
			if ( s.hasMin ) { JS_SetPropertyStr(ctx, o, "min", JS_NewFloat64(ctx, s.min)); }
			if ( s.hasMax ) { JS_SetPropertyStr(ctx, o, "max", JS_NewFloat64(ctx, s.max)); }
			if ( s.type == SAMSettings::Type::Slider ) { JS_SetPropertyStr(ctx, o, "step", JS_NewFloat64(ctx, s.step)); }
			if ( s.type == SAMSettings::Type::Dropdown )
			{
				JSValue opts = JS_NewArray(ctx);
				uint32_t k = 0;
				for ( const std::string& opt : s.options ) { JS_SetPropertyUint32(ctx, opts, k++, JS_NewString(ctx, opt.c_str())); }
				JS_SetPropertyStr(ctx, o, "options", opts);
			}
			JS_SetPropertyUint32(ctx, arr, n++, o);
		}
#endif
		return arr;
	}

	JSValue js_sam_patch_class(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		const int classnum = samJsResolveClass(ctx, argv[0]);
		if ( classnum < 0 ) { SAM_ERROR("JS", "sam_patch_class: unknown class."); return JS_FALSE; }
		SAMClassStatPatch patch;
		if ( argc >= 2 && JS_IsObject(argv[1]) )
		{
			JSPropertyEnum* tab = nullptr; uint32_t plen = 0;
			if ( JS_GetOwnPropertyNames(ctx, &tab, &plen, argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 )
			{
				for ( uint32_t i = 0; i < plen; ++i )
				{
					const char* keyC = JS_AtomToCString(ctx, tab[i].atom);
					const std::string uk = samUpper(keyC ? keyC : "");
					JSValue val = JS_GetProperty(ctx, argv[1], tab[i].atom);
					if ( uk == "SKILLS" && JS_IsObject(val) )
					{
						JSPropertyEnum* st = nullptr; uint32_t sl = 0;
						if ( JS_GetOwnPropertyNames(ctx, &st, &sl, val, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 )
						{
							for ( uint32_t j = 0; j < sl; ++j )
							{
								const char* sk = JS_AtomToCString(ctx, st[j].atom);
								JSValue sv = JS_GetProperty(ctx, val, st[j].atom);
								if ( sk && JS_IsNumber(sv) ) { int32_t n = 0; JS_ToInt32(ctx, &n, sv); patch.skills[sk] = n; }
								if ( sk ) { JS_FreeCString(ctx, sk); }
								JS_FreeValue(ctx, sv);
								JS_FreeAtom(ctx, st[j].atom);
							}
							js_free(ctx, st);
						}
					}
					else if ( JS_IsNumber(val) ) { int32_t n = 0; JS_ToInt32(ctx, &n, val); patch.stats[uk] = n; }
					if ( keyC ) { JS_FreeCString(ctx, keyC); }
					JS_FreeValue(ctx, val);
					JS_FreeAtom(ctx, tab[i].atom);
				}
				js_free(ctx, tab);
			}
		}
		return SAMClasses::patchClass(classnum, patch) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_unpatch_class(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		const int classnum = samJsResolveClass(ctx, argv[0]);
		if ( classnum < 0 ) { return JS_FALSE; }
		SAMClasses::unpatchClass(classnum);
		return JS_TRUE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_patch_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		const int id = samJsResolveItem(ctx, argv[0]);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_patch_item: unknown item."); return JS_FALSE; }
		SAMItemPatch patch;
		if ( argc >= 2 && JS_IsObject(argv[1]) )
		{
			int iv; std::string sv;
			if ( samJsGetIntProp(ctx, argv[1], "weight", iv) ) { patch.hasWeight = true; patch.weight = iv; }
			if ( samJsGetIntProp(ctx, argv[1], "value", iv) ) { patch.hasValue = true; patch.value = iv; }
			else if ( samJsGetIntProp(ctx, argv[1], "gold_value", iv) ) { patch.hasValue = true; patch.value = iv; }
			if ( samJsGetIntProp(ctx, argv[1], "level", iv) ) { patch.hasLevel = true; patch.level = iv; }
			if ( samJsGetStrProp(ctx, argv[1], "category", sv) ) { patch.hasCategory = true; patch.category = samUpper(sv.c_str()); }
			if ( samJsGetStrProp(ctx, argv[1], "slot", sv) ) { patch.hasSlot = true; patch.slot = sv; }
			if ( samJsGetStrProp(ctx, argv[1], "tooltip", sv) ) { patch.hasTooltip = true; patch.tooltip = sv; }
			if ( samJsGetStrProp(ctx, argv[1], "name_identified", sv) ) { patch.hasNameId = true; patch.nameIdentified = sv; }
			else if ( samJsGetStrProp(ctx, argv[1], "name", sv) ) { patch.hasNameId = true; patch.nameIdentified = sv; }
			if ( samJsGetStrProp(ctx, argv[1], "name_unidentified", sv) ) { patch.hasNameUnid = true; patch.nameUnidentified = sv; }
			// Case-insensitive like every other key here and in Lua ({ ATTRIBUTES: ... } was ignored).
			JSValue attrs = samJsGetPropCI(ctx, argv[1], "attributes");
			if ( JS_IsObject(attrs) )
			{
				JSPropertyEnum* tab = nullptr; uint32_t plen = 0;
				if ( JS_GetOwnPropertyNames(ctx, &tab, &plen, attrs, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 )
				{
					for ( uint32_t i = 0; i < plen; ++i )
					{
						const char* ak = JS_AtomToCString(ctx, tab[i].atom);
						JSValue av = JS_GetProperty(ctx, attrs, tab[i].atom);
						if ( ak && JS_IsNumber(av) ) { int32_t n = 0; JS_ToInt32(ctx, &n, av); patch.attributes[ak] = n; }
						if ( ak ) { JS_FreeCString(ctx, ak); }
						JS_FreeValue(ctx, av);
						JS_FreeAtom(ctx, tab[i].atom);
					}
					js_free(ctx, tab);
				}
			}
			JS_FreeValue(ctx, attrs);
		}
		return SAMItems::patchItem(id, patch) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_patch_monster(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_patch_monster refused: host only."); return JS_FALSE; }
		int mtype = -1;
		if ( JS_IsNumber(argv[0]) ) { int32_t n = 0; JS_ToInt32(ctx, &n, argv[0]); mtype = n; }
		else if ( JS_IsString(argv[0]) ) { const char* s = JS_ToCString(ctx, argv[0]); if ( s ) { mtype = samMonsterNameToId(s); JS_FreeCString(ctx, s); } }
		if ( mtype <= 0 || mtype >= NUMMONSTERS ) { SAM_ERROR("JS", "sam_patch_monster: unknown monster type."); return JS_FALSE; }
		int applied = 0;
		if ( argc >= 2 && JS_IsObject(argv[1]) )
		{
			JSPropertyEnum* tab = nullptr; uint32_t plen = 0;
			if ( JS_GetOwnPropertyNames(ctx, &tab, &plen, argv[1], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 )
			{
				for ( uint32_t i = 0; i < plen; ++i )
				{
					const char* keyC = JS_AtomToCString(ctx, tab[i].atom);
					JSValue val = JS_GetProperty(ctx, argv[1], tab[i].atom);
					if ( keyC && JS_IsNumber(val) ) { int32_t n = 0; JS_ToInt32(ctx, &n, val); if ( SAMMonsterPatch::set(mtype, samUpper(keyC), n) ) { ++applied; } }
					if ( keyC ) { JS_FreeCString(ctx, keyC); }
					JS_FreeValue(ctx, val);
					JS_FreeAtom(ctx, tab[i].atom);
				}
				js_free(ctx, tab);
			}
		}
		SAM_INFO("SAM", "sam_patch_monster: type " + std::to_string(mtype) + " (" + std::to_string(applied) + " field override(s))");
		return applied > 0 ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_add_class_passive(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		const int classnum = samJsResolveClass(ctx, argv[0]);
		const int eff = samJsResolvePassive(ctx, argv[1]);
		if ( classnum < 0 ) { SAM_ERROR("JS", "sam_add_class_passive: unknown class."); return JS_FALSE; }
		if ( eff < 0 || eff >= NUMEFFECTS ) { SAM_ERROR("JS", "sam_add_class_passive: unknown effect."); return JS_FALSE; }
		return SAMClasses::addClassPassive(classnum, eff) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_remove_class_passive(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		const int classnum = samJsResolveClass(ctx, argv[0]);
		const int eff = samJsResolvePassive(ctx, argv[1]);
		if ( classnum < 0 || eff < 0 ) { return JS_FALSE; }
		return SAMClasses::removeClassPassive(classnum, eff) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// ---- spells: grant a spell to a player (see the Lua twin) -------------------------
	JSValue js_sam_grant_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Both required, as luaL_checkinteger / luaL_checkstring require them. A bare JS_ToInt32
		// here turned a missing or misspelt player into 0, and the host's player got the spell.
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_grant_spell") ) { return JS_FALSE; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_grant_spell: argument 2 (the spell) is required."); return JS_FALSE; }
		const char* spellC = JS_ToCString(ctx, argv[1]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
		SAM_INFO("API", "sam_grant_spell(player=" + std::to_string(player) + ", " + spell + ")");
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] )
		{
			SAM_ERROR("JS", "sam_grant_spell: invalid player index " + std::to_string(player) + ".");
			return JS_FALSE;
		}
		const int id = SAMSpells::resolveSpellRef(spell);
		if ( id < 0 )
		{
			SAM_ERROR("JS", "sam_grant_spell: unknown spell '" + spell + "' (expected a SPELL_ name, \"namespace:spell\" or a spell id).");
			return JS_FALSE;
		}
		return SAMSpells::grantSpell((int)player, id) ? JS_TRUE : JS_FALSE;
#else
		(void)player;
		return JS_FALSE;
#endif
	}

#ifdef SAM_JS_HAVE_BARONY
	static int samJsResolveSpellId(const std::string& spell);   // below, beside the other spell helpers
#endif
	// sam_cast_spell(player, spell) — mirror of the Lua binding: fire a spell/bolt from a
	// player in their facing direction (host-only, trap=true so it's free + never blocked
	// by the defend guard). Returns true if a projectile spawned.
	JSValue js_sam_cast_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required and checked, as in Lua. A bare JS_ToInt32 turned undefined into player 0, so a
		// handler reading a missing event field cast the spell from the HOST's character.
		int32_t player = 0;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_cast_spell") ) { return JS_FALSE; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_cast_spell: argument 2 is required."); return JS_FALSE; }
		const char* spellC = JS_ToCString(ctx, argv[1]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_cast_spell refused: host only."); return JS_FALSE; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{
			SAM_ERROR("JS", "sam_cast_spell: invalid player index " + std::to_string(player) + ".");
			return JS_FALSE;
		}
		const int id = samJsResolveSpellId(spell);   // the shared resolver, as _at and _pos use
		if ( id < 0 ) { SAM_ERROR("JS", "sam_cast_spell: unknown spell '" + spell + "' (SPELL_ name, \"namespace:spell\" or a spell id)."); return JS_FALSE; }
		spell_t* sp = getSpellFromID(id);
		if ( !sp ) { SAM_ERROR("JS", "sam_cast_spell: spell '" + spell + "' (id " + std::to_string(id) + ") has no engine spell."); return JS_FALSE; }
		Entity* missile = castSpell(players[player]->entity->getUID(), sp, false, true);
		SAM_INFO("SAM", "sam_cast_spell: player " + std::to_string(player) + " cast '" + spell + "'" + (missile ? "" : " (no projectile)"));
		return missile ? JS_TRUE : JS_FALSE;
#else
		(void)player; (void)spell;
		return JS_FALSE;
#endif
	}

#ifdef SAM_JS_HAVE_BARONY
	// v1.5.0 spell helpers (JS twins of the Lua ones).
	static int samJsResolveSpellId(const std::string& spell)
	{
		// A numeric id as text. See the Lua twin: sam_get_tome_spell hands back a NUMBER and
		// every consumer here was string-only, so "read what this spellbook teaches, then grant
		// it" resolved nothing.
		{
			bool digits = !spell.empty();
			for ( char c : spell ) { if ( c < '0' || c > '9' ) { digits = false; break; } }
			if ( digits )
			{
				const int nid = (int)strtol(spell.c_str(), nullptr, 10);
				if ( ItemTooltips.spellItems.find(nid) != ItemTooltips.spellItems.end() ) { return nid; }
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
	static Entity* samJsCastAimed(Entity* e, int id, bool aim, double tx, double ty)
	{
		spell_t* sp = getSpellFromID(id);
		if ( !e || !sp ) { return nullptr; }
		if ( !aim ) { return castSpell(e->getUID(), sp, false, true); }
		const real_t savedYaw = e->yaw;
		e->yaw = std::atan2(ty - e->y, tx - e->x);
		Entity* missile = castSpell(e->getUID(), sp, false, true);
		e->yaw = savedYaw;
		return missile;
	}
#endif

	JSValue js_sam_cast_spell_at(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; int64_t targetUid = 0;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_cast_spell_at") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &targetUid, "sam_cast_spell_at") ) { return JS_UNDEFINED; }
		if ( !samHasArg(argc, argv, 2) ) { SAM_ERROR("JS", "sam_cast_spell_at: argument 3 is required."); return JS_UNDEFINED; }
		const char* spellC = JS_ToCString(ctx, argv[2]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_cast_spell_at refused: host only."); return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("JS", "sam_cast_spell_at: invalid player index " + std::to_string(player) + "."); return JS_UNDEFINED; }
		Entity* target = SAMLua::resolveEntityQuiet((long long)targetUid);
		if ( !target ) { SAM_WARN("JS", "sam_cast_spell_at: no entity uid " + std::to_string((long long)targetUid)); return JS_UNDEFINED; }
		const int id = samJsResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_cast_spell_at: unknown spell '" + spell + "'."); return JS_UNDEFINED; }
		Entity* missile = samJsCastAimed(players[player]->entity, id, true, target->x, target->y);
		return missile ? JS_NewInt64(ctx, (int64_t)missile->getUID()) : JS_UNDEFINED;
#else
		(void)player; (void)targetUid; (void)spell; return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_cast_spell_pos(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0, tx = 0, ty = 0;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_cast_spell_pos") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &tx, "sam_cast_spell_pos") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &ty, "sam_cast_spell_pos") ) { return JS_UNDEFINED; }
		if ( !samHasArg(argc, argv, 3) ) { SAM_ERROR("JS", "sam_cast_spell_pos: argument 4 is required."); return JS_UNDEFINED; }
		const char* spellC = JS_ToCString(ctx, argv[3]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_cast_spell_pos refused: host only."); return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("JS", "sam_cast_spell_pos: invalid player index " + std::to_string(player) + "."); return JS_UNDEFINED; }
		const int id = samJsResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_cast_spell_pos: unknown spell '" + spell + "'."); return JS_UNDEFINED; }
		Entity* missile = samJsCastAimed(players[player]->entity, id, true, (double)(tx * 16 + 8), (double)(ty * 16 + 8));
		return missile ? JS_NewInt64(ctx, (int64_t)missile->getUID()) : JS_UNDEFINED;
#else
		(void)player; (void)tx; (void)ty; (void)spell; return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_monster_cast_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Checked, as luaL_checkinteger / luaL_checkstring check them in Lua: undefined used to
		// become uid 0 and a lookup.
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_monster_cast_spell") ) { return JS_UNDEFINED; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_monster_cast_spell: argument 2 is required."); return JS_UNDEFINED; }
		const char* spellC = JS_ToCString(ctx, argv[1]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_monster_cast_spell refused: host only."); return JS_UNDEFINED; }
		Entity* e = samResolveMonster(uid);
		// Said out loud, as the Lua twin always did; this used to fail in silence.
		if ( !e ) { SAM_WARN("JS", "sam_monster_cast_spell: no monster uid " + std::to_string((long long)uid)); return JS_UNDEFINED; }
		const int id = samJsResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_monster_cast_spell: unknown spell '" + spell + "'."); return JS_UNDEFINED; }
		Entity* missile = samJsCastAimed(e, id, false, 0, 0);
		return missile ? JS_NewInt64(ctx, (int64_t)missile->getUID()) : JS_UNDEFINED;
#else
		(void)uid; (void)spell; return JS_UNDEFINED;
#endif
	}

	// sam_get_spells(player). See the Lua twin: one helper names the spells for both runtimes,
	// so a mod's spell is its declared "namespace:spell" here too (this used to hand back the
	// mangled internal name while Lua handed back the id).
	JSValue js_sam_get_spells(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_get_spells") ) { return JS_NewArray(ctx); }
#ifdef SAM_JS_HAVE_BARONY
		std::vector<std::string> names;
		if ( SAMMpInventory::spellNamesOf((int)player, "sam_get_spells", &names) == SAMMpInventory::Seen::No )
		{
			return JS_UNDEFINED;
		}
		JSValue arr = JS_NewArray(ctx);
		uint32_t n = 0;
		for ( const std::string& nm : names ) { JS_SetPropertyUint32(ctx, arr, n++, JS_NewString(ctx, nm.c_str())); }
		return arr;
#else
		(void)player;
		return JS_NewArray(ctx);
#endif
	}

	JSValue js_sam_player_knows_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua: a bare JS_ToInt32 turned a missing player into 0 (the host's).
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_player_knows_spell") ) { return JS_FALSE; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_player_knows_spell: argument 2 (the spell) is required."); return JS_FALSE; }
		const char* spellC = JS_ToCString(ctx, argv[1]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_FALSE; }
		const int id = SAMSpells::resolveSpellRef(spell);
		if ( id < 0 ) { return JS_FALSE; }
		std::vector<int> ids;
		if ( SAMMpInventory::spellIdsOf((int)player, "sam_player_knows_spell", &ids) == SAMMpInventory::Seen::No )
		{
			return JS_UNDEFINED;
		}
		for ( const int k : ids ) { if ( k == id ) { return JS_TRUE; } }
		return JS_FALSE;
#else
		(void)player; (void)spell; return JS_FALSE;
#endif
	}

	// =====================================================================================
	//  v2.8 batch 4 — COMBAT, the JS twins.
	//
	//  Same order, same refusals, same reasons. Where a decision could drift — what counts as
	//  a creature, which seven words name a damage type — both runtimes call ONE shared
	//  function in SAMLua rather than each keeping a copy. Lua returns nil where these return
	//  undefined, which is the same answer in each language's own vocabulary.
	// =====================================================================================

	// A required string argument, refused rather than guessed. The Lua twins get this from
	// luaL_checkstring, which raises; here it has to be spelled out.
	static bool samJsReqStr(JSContext* ctx, int argc, JSValueConst* argv, int i,
		const char* who, std::string* out)
	{
		if ( !samHasArg(argc, argv, i) )
		{
			SAM_ERROR("JS", std::string(who) + ": argument " + std::to_string(i + 1) + " is required.");
			return false;
		}
		const char* s = JS_ToCString(ctx, argv[i]);
		if ( !s ) { return false; }
		*out = s;
		JS_FreeCString(ctx, s);
		return true;
	}

	static void samJsOptStr(JSContext* ctx, int argc, JSValueConst* argv, int i, std::string* io)
	{
		if ( !samHasArg(argc, argv, i) ) { return; }
		const char* s = JS_ToCString(ctx, argv[i]);
		if ( !s ) { return; }
		*io = s;
		JS_FreeCString(ctx, s);
	}

	// sam_heal(uid, amount) -> the HP actually restored, or undefined. See the Lua twin: sam_deal_damage
	// forces its sign negative, so before this there was no relative heal at all.
	JSValue js_sam_heal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t amount = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_heal") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &amount, "sam_heal") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatantWritable((long long)uid, "sam_heal", &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		if ( amount <= 0 )
		{
			SAM_WARN("JS", "sam_heal: the amount has to be positive. To hurt something use"
				" sam_deal_damage — passing a negative here would have healed it anyway.");
			return JS_NewInt32(ctx, 0);
		}
		const int beforeHP = s->HP;
		e->modHP(amount);
		return JS_NewInt32(ctx, s->HP - beforeHP);
#else
		(void)uid; (void)amount; return JS_UNDEFINED;
#endif
	}

	// sam_deal_damage_typed(uid, amount, type) -> the damage actually dealt, or undefined.
	JSValue js_sam_deal_damage_typed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t amount = 0; std::string type;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_deal_damage_typed") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &amount, "sam_deal_damage_typed") ) { return JS_UNDEFINED; }
		if ( !samJsReqStr(ctx, argc, argv, 2, "sam_deal_damage_typed", &type) ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		int dtype = 0;
		if ( !SAMLua::damageTypeFromName(type.c_str(), "sam_deal_damage_typed", &dtype) ) { return JS_UNDEFINED; }
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatantWritable((long long)uid, "sam_deal_damage_typed", &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		const int asked = ( amount < 0 ) ? -amount : amount;
		real_t mult = Entity::getDamageTableMultiplier(e, *s, (DamageTableType)dtype);
		Entity::modifyDamageMultipliersFromEffects(e, nullptr, mult, (DamageTableType)dtype);
		int dealt = (int)(asked * mult);
		if ( dealt < 0 ) { dealt = 0; }
		if ( dealt > 0 ) { e->modHP(-dealt); }
		return JS_NewInt32(ctx, dealt);
#else
		(void)uid; (void)amount; return JS_UNDEFINED;
#endif
	}

	// sam_get_hp(uid) / sam_get_max_hp(uid) -> number, or undefined for anything without a Stat.
	JSValue js_sam_get_hp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_hp") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		if ( !SAMLua::resolveCombatant((long long)uid, &s) || !s ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, s->HP);
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_get_max_hp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_max_hp") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		if ( !SAMLua::resolveCombatant((long long)uid, &s) || !s ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, s->MAXHP);
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_get_mp(uid) / sam_get_max_mp(uid) -> number, or undefined. See the Lua twins: without these
	// the three mana verbs this batch added had no reader at all on anything but a player.
	JSValue js_sam_get_mp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_mp") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		if ( !SAMLua::resolveCombatant((long long)uid, &s) || !s ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, s->MP);
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_get_max_mp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_max_mp") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		if ( !SAMLua::resolveCombatant((long long)uid, &s) || !s ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, s->MAXMP);
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_get_attack(uid) -> the melee attack value the engine would use, or undefined.
	JSValue js_sam_get_attack(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_attack") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatant((long long)uid, &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, (int)Entity::getAttack(e, s, e->behavior == &actPlayer));
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_get_ranged_attack(uid [, quiver_bonus]) -> number, or undefined.
	JSValue js_sam_get_ranged_attack(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t quiver = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_ranged_attack") ) { return JS_UNDEFINED; }
		samJsOptI32(ctx, argc, argv, 1, &quiver, "sam_get_ranged_attack");
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatant((long long)uid, &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, (int)e->getRangedAttack((int)quiver));
#else
		(void)uid; (void)quiver; return JS_UNDEFINED;
#endif
	}

	// sam_get_thrown_attack(uid) -> number, or undefined.
	JSValue js_sam_get_thrown_attack(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_thrown_attack") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatant((long long)uid, &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, (int)e->getThrownAttack());
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_get_bonus_attack_vs(uid, target_uid) -> number, or undefined.
	JSValue js_sam_get_bonus_attack_vs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0, tgt = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_bonus_attack_vs") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &tgt, "sam_get_bonus_attack_vs") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatant((long long)uid, &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		Stat* ts = nullptr;
		if ( !SAMLua::resolveCombatant((long long)tgt, &ts) || !ts ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, (int)e->getBonusAttackOnTarget(*ts));
#else
		(void)uid; (void)tgt; return JS_UNDEFINED;
#endif
	}

	// sam_get_damage_resist(uid [, type]) -> the multiplier, or undefined. Defaults to "magic".
	JSValue js_sam_get_damage_resist(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; std::string type = "magic";
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_damage_resist") ) { return JS_UNDEFINED; }
		samJsOptStr(ctx, argc, argv, 1, &type);
#ifdef SAM_JS_HAVE_BARONY
		int dtype = 0;
		if ( !SAMLua::damageTypeFromName(type.c_str(), "sam_get_damage_resist", &dtype) ) { return JS_UNDEFINED; }
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatant((long long)uid, &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		return JS_NewFloat64(ctx, (double)Entity::getDamageTableMultiplier(e, *s, (DamageTableType)dtype));
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_get_magic_resist(uid) -> the raw magic-resistance POINT count, or undefined.
	JSValue js_sam_get_magic_resist(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_magic_resist") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		if ( !SAMLua::resolveCombatant((long long)uid, &s) || !s ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, Entity::getMagicResistance(s));
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_preview_damage(attacker_uid, target_uid) -> what a melee swing would deal, dealing
	// nothing. undefined if either side is not a creature.
	JSValue js_sam_preview_damage(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0, tgt = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_preview_damage") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &tgt, "sam_preview_damage") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* as = nullptr;
		Entity* ae = SAMLua::resolveCombatant((long long)uid, &as);
		if ( !ae || !as ) { return JS_UNDEFINED; }
		Stat* ts = nullptr;
		Entity* te = SAMLua::resolveCombatant((long long)tgt, &ts);
		if ( !te || !ts ) { return JS_UNDEFINED; }

		const real_t myAttack = (real_t)Entity::getAttack(ae, as, ae->behavior == &actPlayer);
		int numBlessings = 0;
		const real_t acEff = Entity::getACEffectiveness(te, ts, te->behavior == &actPlayer,
			ae, as, numBlessings);
		const real_t enemyAC = (real_t)AC(ts);
		int out = (int)(std::max(0.0, ((myAttack * acEff - enemyAC))) + (1.0 - acEff) * myAttack);
		if ( out < 0 ) { out = 0; }
		return JS_NewInt32(ctx, out);
#else
		(void)uid; (void)tgt; return JS_UNDEFINED;
#endif
	}

	// sam_get_regen_interval(uid) -> ticks between natural HP regeneration ticks, or undefined.
	JSValue js_sam_get_regen_interval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_regen_interval") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatant((long long)uid, &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, Entity::getHealthRegenInterval(e, *s, e->behavior == &actPlayer));
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_get_healring(uid) -> the regeneration bonus from equipment and effects, or undefined.
	JSValue js_sam_get_healring(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_healring") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatant((long long)uid, &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		const int fromGear = Entity::getHealringFromEquipment(e, *s, e->behavior == &actPlayer);
		const int fromEff  = Entity::getHealringFromEffects(e, *s);
		return JS_NewInt32(ctx, fromGear + fromEff);
#else
		(void)uid; return JS_UNDEFINED;
#endif
	}

	// sam_mod_mp(uid, amount) -> the MP after the change, or undefined.
	JSValue js_sam_mod_mp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t amount = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_mod_mp") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &amount, "sam_mod_mp") ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatantWritable((long long)uid, "sam_mod_mp", &s);
		if ( !e || !s ) { return JS_UNDEFINED; }
		e->modMP((int)amount, true);
		return JS_NewInt32(ctx, s->MP);
#else
		(void)uid; (void)amount; return JS_UNDEFINED;
#endif
	}

	// sam_drain_mp(uid, amount [, notify]) -> boolean. Overdraw comes out of HEALTH.
	JSValue js_sam_drain_mp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t amount = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_drain_mp") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &amount, "sam_drain_mp") ) { return JS_FALSE; }
		const bool notify = samBoolArgJs(ctx, argc, argv, 2, true);
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatantWritable((long long)uid, "sam_drain_mp", &s);
		if ( !e || !s ) { return JS_FALSE; }
		if ( amount <= 0 )
		{
			SAM_WARN("JS", "sam_drain_mp: the amount has to be positive. To GIVE mana use sam_mod_mp.");
			return JS_FALSE;
		}
		e->drainMP((int)amount, notify);
		return JS_TRUE;
#else
		(void)uid; (void)amount; (void)notify; return JS_FALSE;
#endif
	}

	// sam_consume_mp(uid, amount) -> boolean. Spends only if affordable -- except for a vampire
	// PLAYER, whose shortfall the engine takes out of health instead; see the Lua twin.
	JSValue js_sam_consume_mp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t amount = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_consume_mp") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &amount, "sam_consume_mp") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatantWritable((long long)uid, "sam_consume_mp", &s);
		if ( !e || !s ) { return JS_FALSE; }
		if ( amount < 0 )
		{
			SAM_WARN("JS", "sam_consume_mp: the amount cannot be negative. To GIVE mana use sam_mod_mp.");
			return JS_FALSE;
		}
		return e->safeConsumeMP((int)amount) ? JS_TRUE : JS_FALSE;
#else
		(void)uid; (void)amount; return JS_FALSE;
#endif
	}

	// sam_set_defending(player, on) -> boolean (true if it changed anything).
	// SAME-FRAME ONLY -- see the Lua twin: actHudShield rewrites this field from the block
	// input every frame, so it is an override for the current frame and never a latch.
	JSValue js_sam_set_defending(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; bool on = true;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_set_defending") ) { return JS_FALSE; }
		if ( !samBoolReqJs(ctx, argc, argv, 1, "sam_set_defending", &on) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_set_defending refused: host only.");
			return JS_FALSE;
		}
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_FALSE; }
		const bool was = stats[player]->defending;
		stats[player]->defending = on;
		SAMCombat::noteDefendingOverride(player, was, on);   // see the Lua twin
		return ( was != on ) ? JS_TRUE : JS_FALSE;
#else
		(void)player; (void)on; return JS_FALSE;
#endif
	}

	// sam_is_parrying(player) -> boolean.
	JSValue js_sam_is_parrying(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_is_parrying") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_FALSE; }
		return ( stats[player]->parrying > 0 ) ? JS_TRUE : JS_FALSE;
#else
		(void)player; return JS_FALSE;
#endif
	}

	// sam_set_parry(player, ticks) -> boolean. 0 closes the window.
	JSValue js_sam_set_parry(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; int64_t ticks = 0;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_set_parry") ) { return JS_FALSE; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &ticks, "sam_set_parry") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_set_parry refused: host only.");
			return JS_FALSE;
		}
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_FALSE; }
		if ( ticks < 0 ) { ticks = 0; }
		if ( ticks > 180000LL ) { ticks = 180000LL; }
		stats[player]->parrying = (Uint32)ticks;
		return JS_TRUE;
#else
		(void)player; (void)ticks; return JS_FALSE;
#endif
	}

	// sam_set_monster_target_uid(uid, target_uid [, was_hit]) -> boolean. Monster-versus-monster
	// aggro, which sam_set_monster_target cannot express because it takes a player index.
	JSValue js_sam_set_monster_target_uid(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0, tgt = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_monster_target_uid") ) { return JS_FALSE; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &tgt, "sam_set_monster_target_uid") ) { return JS_FALSE; }
		const bool wasHit = samBoolArgJs(ctx, argc, argv, 2, false);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_set_monster_target_uid refused: host only. AI lives on the host,"
				" and a client holds no stats for an ordinary monster.");
			return JS_FALSE;
		}
		Entity* e = samResolveMonster(uid);
		if ( !e )
		{
			SAM_WARN("JS", "sam_set_monster_target_uid: uid " + std::to_string(uid) + " is not a"
				" monster with stats, so it has no AI to point anywhere.");
			return JS_FALSE;
		}
		Entity* t = SAMLua::resolveEntityQuiet((long long)tgt);
		if ( !t ) { return JS_FALSE; }
		if ( t == e )
		{
			SAM_WARN("JS", "sam_set_monster_target_uid: a monster cannot hunt itself.");
			return JS_FALSE;
		}
		e->monsterAcquireAttackTarget(*t, MONSTER_STATE_PATH, wasHit);
		return JS_TRUE;
#else
		(void)uid; (void)tgt; (void)wasHit; return JS_FALSE;
#endif
	}

	// sam_get_monster_target_uid(uid) -> the uid of whatever this monster is hunting, or 0.
	// See the Lua twin: sam_get_monster_target answers a player index, so monster-versus-monster
	// aggro had no reader at all.
	JSValue js_sam_get_monster_target_uid(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_get_monster_target_uid") ) { return JS_NewInt32(ctx, 0); }
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_NewInt32(ctx, 0); }
		Entity* t = uidToEntity((Sint32)e->monsterTarget);
		return JS_NewInt32(ctx, t ? (int)t->getUID() : 0);
#else
		(void)uid; return JS_NewInt32(ctx, 0);
#endif
	}

	// sam_clear_monster_target(uid [, force]) -> boolean. The engine's refusal is passed through.
	JSValue js_sam_clear_monster_target(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_clear_monster_target") ) { return JS_FALSE; }
		const bool force = samBoolArgJs(ctx, argc, argv, 1, false);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_clear_monster_target refused: host only.");
			return JS_FALSE;
		}
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_FALSE; }
		return e->monsterReleaseAttackTarget(force) ? JS_TRUE : JS_FALSE;
#else
		(void)uid; (void)force; return JS_FALSE;
#endif
	}

	// sam_alert_allies(uid [, attacker_uid]) -> boolean.
	JSValue js_sam_alert_allies(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0, att = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_alert_allies") ) { return JS_FALSE; }
		samJsOptI64(ctx, argc, argv, 1, &att, "sam_alert_allies");
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_alert_allies refused: host only.");
			return JS_FALSE;
		}
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_FALSE; }
		Entity* a = ( att != 0 ) ? SAMLua::resolveEntityQuiet((long long)att) : nullptr;
		e->alertAlliesOnBeingHit(a);
		return JS_TRUE;
#else
		(void)uid; (void)att; return JS_FALSE;
#endif
	}

	// sam_break_armor(uid [, slot]) -> boolean. With no slot, the engine's own picker chooses,
	// so the odds and the exclusions match a real hit. The numbers below are the engine's
	// equipment numbering (the one the 'ARMR' packet uses), NOT the equipment-slot enum.
	JSValue js_sam_break_armor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; std::string want;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_break_armor") ) { return JS_FALSE; }
		samJsOptStr(ctx, argc, argv, 1, &want);
#ifdef SAM_JS_HAVE_BARONY
		Stat* s = nullptr;
		Entity* e = SAMLua::resolveCombatantWritable((long long)uid, "sam_break_armor", &s);
		if ( !e || !s ) { return JS_FALSE; }
		for ( char& c : want ) { c = (char)std::tolower((unsigned char)c); }

		Item* armor = nullptr;
		int armornum = -1;
		if ( want.empty() )
		{
			armornum = s->pickRandomEquippedItemToDegradeOnHit(&armor, true, false, false, true);
			if ( armornum < 0 || !armor ) { return JS_FALSE; }
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
				SAM_ERROR("JS", "sam_break_armor: '" + want + "' is not a slot that can degrade."
					" They are helmet, breastplate, gloves, boots, shield, cloak and mask.");
				return JS_FALSE;
			}
			armor = *slots[chosen].item;
			armornum = slots[chosen].armornum;
			if ( !armor ) { return JS_FALSE; }
		}
		return e->degradeArmor(*s, *armor, armornum) ? JS_TRUE : JS_FALSE;
#else
		(void)uid; return JS_FALSE;
#endif
	}

	// sam_gib(uid [, sprite]) -> boolean. A gib takes a PARENT, which is why it could not be
	// another kind for sam_spawn_particle.
	JSValue js_sam_gib(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; int32_t sprite = -1;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_gib") ) { return JS_FALSE; }
		samJsOptI32(ctx, argc, argv, 1, &sprite, "sam_gib");
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = SAMLua::resolveWritableEntity((long long)uid, "sam_gib");
		if ( !e ) { return JS_FALSE; }
		Entity* g = spawnGib(e, (int)sprite);
		if ( g ) { SAMMpEntities::gibForClients(g); }   // see the Lua twin
		return g ? JS_TRUE : JS_FALSE;
#else
		(void)uid; (void)sprite; return JS_FALSE;
#endif
	}

	// sam_obituary(killer_uid, victim_uid [, from_spell]) -> boolean. Call it AFTER the killing
	// blow: setHP rewrites the obituary to the generic string on every HP change.
	JSValue js_sam_obituary(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t killer = 0, victim = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &killer, "sam_obituary") ) { return JS_FALSE; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &victim, "sam_obituary") ) { return JS_FALSE; }
		const bool fromSpell = samBoolArgJs(ctx, argc, argv, 2, false);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_obituary refused: host only.");
			return JS_FALSE;
		}
		Entity* k = SAMLua::resolveEntityQuiet((long long)killer);
		Stat* vs = nullptr;
		Entity* v = SAMLua::resolveCombatant((long long)victim, &vs);
		if ( !k || !v || !vs )
		{
			SAM_WARN("JS", "sam_obituary: both sides have to be live entities and the victim has"
				" to be a creature — the obituary is written into the victim's own stats.");
			return JS_FALSE;
		}
		k->killedByMonsterObituary(v, fromSpell);
		return JS_TRUE;
#else
		(void)killer; (void)victim; (void)fromSpell; return JS_FALSE;
#endif
	}

	// sam_revive_player(player [, x, y]) -> boolean. Every slot; see the Lua twin and
	// SAMCombat::revivePlayer, which both runtimes share.
	JSValue js_sam_revive_player(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0, wantX = -1, wantY = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_revive_player") ) { return JS_FALSE; }
		samJsOptI32(ctx, argc, argv, 1, &wantX, "sam_revive_player");
		samJsOptI32(ctx, argc, argv, 2, &wantY, "sam_revive_player");
#ifdef SAM_JS_HAVE_BARONY
		return SAMCombat::revivePlayer((int)player, (int)wantX, (int)wantY) ? JS_TRUE : JS_FALSE;   // see the Lua twin
#else
		(void)player; (void)wantX; (void)wantY; return JS_FALSE;
#endif
	}

	// sam_set_species_damage_resist(species, type, multiplier) -> boolean. Species-wide.
	// 0 does NOT mean immune: the engine floors the final multiplier at 0.1, so the least any
	// species can be made to take is a tenth. See sam_combat.hpp.
	// Not a write to damagetables — see sam_combat.hpp for why that could not work.
	JSValue js_sam_set_species_damage_resist(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string speciesName, typeName; double mult = 1.0;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_set_species_damage_resist", &speciesName) ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_set_species_damage_resist", &typeName) ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 2, &mult, "sam_set_species_damage_resist") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		const int species = samMonsterNameToId(speciesName.c_str());
		if ( species < 0 )
		{
			SAM_ERROR("JS", "sam_set_species_damage_resist: '" + speciesName
				+ "' is not a creature this game has.");
			return JS_FALSE;
		}
		int dtype = 0;
		if ( !SAMLua::damageTypeFromName(typeName.c_str(), "sam_set_species_damage_resist", &dtype) )
		{
			return JS_FALSE;
		}
		return SAMCombat::setSpeciesResist(species, dtype, mult) ? JS_TRUE : JS_FALSE;
#else
		(void)mult; return JS_FALSE;
#endif
	}

	// sam_clear_species_damage_resist([species [, type]]) -> how many were removed.
	JSValue js_sam_clear_species_damage_resist(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string speciesName, typeName;
		samJsOptStr(ctx, argc, argv, 0, &speciesName);
		samJsOptStr(ctx, argc, argv, 1, &typeName);
#ifdef SAM_JS_HAVE_BARONY
		if ( speciesName.empty() ) { return JS_NewInt32(ctx, SAMCombat::clearAllSpeciesResist()); }
		const int species = samMonsterNameToId(speciesName.c_str());
		if ( species < 0 )
		{
			SAM_ERROR("JS", "sam_clear_species_damage_resist: '" + speciesName
				+ "' is not a creature this game has.");
			return JS_NewInt32(ctx, 0);
		}
		int dtype = -1;
		if ( !typeName.empty() )
		{
			if ( !SAMLua::damageTypeFromName(typeName.c_str(), "sam_clear_species_damage_resist", &dtype) )
			{
				return JS_NewInt32(ctx, 0);
			}
		}
		return JS_NewInt32(ctx, SAMCombat::clearSpeciesResist(species, dtype));
#else
		return JS_NewInt32(ctx, 0);
#endif
	}

	// sam_add_damage_multiplier(fraction) -> boolean. Valid only inside an on_damage_multiplier
	// handler, and it says so rather than dropping the number in silence.
	JSValue js_sam_add_damage_multiplier(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		double f = 0.0;
		if ( !samJsReqF64(ctx, argc, argv, 0, &f, "sam_add_damage_multiplier") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( !SAMCombat::multiplierHookActive() )
		{
			SAM_WARN("JS", "sam_add_damage_multiplier: no damage is being resolved right now, so"
				" there is nothing to contribute to. It works inside an on_damage_multiplier"
				" handler; to change one specific hit use sam_modify_damage (player) or"
				" sam_modify_monster_damage (monster).");
			return JS_FALSE;
		}
		SAMCombat::addMultiplier(f);
		return JS_TRUE;
#else
		(void)f; return JS_FALSE;
#endif
	}

	// =====================================================================================
	//  CAMERA (v3.0.0), the JS twins. Same units, same refusals, same reasons; both runtimes
	//  call one shared SAMCamera so the two cannot drift apart.
	// =====================================================================================

	// sam_set_camera_offset(player, back [, up [, right]]) -> boolean. The third-person rig.
	// Pair with sam_show_own_body or you are looking at an invisible character.
	JSValue js_sam_set_camera_offset(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; double back = 0.0, up = 0.0, right = 0.0;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_set_camera_offset", true) ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 1, &back, "sam_set_camera_offset") ) { return JS_FALSE; }
		samJsOptF64(ctx, argc, argv, 2, &up, "sam_set_camera_offset");
		samJsOptF64(ctx, argc, argv, 3, &right, "sam_set_camera_offset");
#ifdef SAM_JS_HAVE_BARONY
		return SAMCamera::setOffset((int)player, back, up, right) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_camera_position(player, x, y [, height]) -> boolean. Tiles; height above the floor.
	JSValue js_sam_set_camera_position(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; double x = 0.0, y = 0.0, h = 0.64;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_set_camera_position", true) ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 1, &x, "sam_set_camera_position") ) { return JS_FALSE; }
		if ( !samJsReqF64(ctx, argc, argv, 2, &y, "sam_set_camera_position") ) { return JS_FALSE; }
		samJsOptF64(ctx, argc, argv, 3, &h, "sam_set_camera_position");
#ifdef SAM_JS_HAVE_BARONY
		return SAMCamera::setPosition((int)player, x, y, h) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_camera_angle(player [, yaw, pitch]) -> boolean. No angle goes back to the
	// player's own look, which is what the mouse drives.
	JSValue js_sam_set_camera_angle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; double yaw = 0.0, pitch = 0.0;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_set_camera_angle", true) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( !samHasArg(argc, argv, 1) )
		{
			return SAMCamera::clearAngle((int)player) ? JS_TRUE : JS_FALSE;
		}
		if ( !samJsReqF64(ctx, argc, argv, 1, &yaw, "sam_set_camera_angle") ) { return JS_FALSE; }
		samJsOptF64(ctx, argc, argv, 2, &pitch, "sam_set_camera_angle");
		return SAMCamera::setAngle((int)player, yaw, pitch) ? JS_TRUE : JS_FALSE;
#else
		(void)yaw; (void)pitch; return JS_FALSE;
#endif
	}

	// sam_set_camera_target(player, uid) -> boolean. 0 stops tracking.
	JSValue js_sam_set_camera_target(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; int64_t uid = 0;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_set_camera_target", true) ) { return JS_FALSE; }
		if ( !samJsReqI32Wide(ctx, argc, argv, 1, &uid, "sam_set_camera_target") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		return SAMCamera::setTarget((int)player, (long long)uid) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_camera_collision(player, on) -> boolean.
	JSValue js_sam_set_camera_collision(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; bool on = true;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_set_camera_collision", true) ) { return JS_FALSE; }
		if ( !samBoolReqJs(ctx, argc, argv, 1, "sam_set_camera_collision", &on) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		return SAMCamera::setCollision((int)player, on) ? JS_TRUE : JS_FALSE;
#else
		(void)on; return JS_FALSE;
#endif
	}

	// sam_get_camera(player) -> { x, y, height, yaw, pitch, mode } or undefined. Where the camera
	// actually IS, which differs from what was asked for the moment the boom hits a wall.
	JSValue js_sam_get_camera(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_get_camera", true) ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		double x = 0, y = 0, h = 0, yaw = 0, pitch = 0;
		int mode = 0;
		if ( !SAMCamera::get((int)player, &x, &y, &h, &yaw, &pitch, &mode) ) { return JS_UNDEFINED; }
		JSValue o = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, x));
		JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, y));
		JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, h));
		JS_SetPropertyStr(ctx, o, "yaw", JS_NewFloat64(ctx, yaw));
		JS_SetPropertyStr(ctx, o, "pitch", JS_NewFloat64(ctx, pitch));
		JS_SetPropertyStr(ctx, o, "mode",
			JS_NewString(ctx, mode == 1 ? "orbit" : ( mode == 2 ? "absolute" : "vanilla" )));
		return o;
#else
		return JS_UNDEFINED;
#endif
	}

	// sam_reset_camera(player) -> boolean. Hands the camera back to the engine.
	JSValue js_sam_reset_camera(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_reset_camera", true) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		return SAMCamera::reset((int)player) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_show_own_body(player, on) -> boolean. The engine's own switch, the one /thirdperson
	// flips: shows your character and hides the first-person weapon. What /thirdperson does
	// NOT do is move the camera -- sam_set_camera_offset is that half.
	JSValue js_sam_show_own_body(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; bool on = true;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_show_own_body", true) ) { return JS_FALSE; }
		if ( !samBoolReqJs(ctx, argc, argv, 1, "sam_show_own_body", &on) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		return SAMCamera::showOwnBody((int)player, on) ? JS_TRUE : JS_FALSE;
#else
		(void)on; return JS_FALSE;
#endif
	}

	// =====================================================================================
	//  THE RULES (v3.0.0), the JS twins. Same tables, same refusals; both runtimes call one
	//  shared SAMRules so the two cannot drift apart.
	// =====================================================================================

#ifdef SAM_JS_HAVE_BARONY
	static bool samJsStatKind(const char* name, const char* who, int* out)
	{
		const int k = SAMRules::statKindFromName(name);
		if ( k >= 0 ) { *out = k; return true; }
		SAM_ERROR("JS", std::string(who) + ": '" + (name ? name : "") + "' is not a stat this can"
			" modify. The nine are STR, DEX, CON, INT, PER, CHR, AC, ATTACK and SPEED.");
		return false;
	}
#endif

	// The integer twin. samJsReqI32 casts with (int32_t), which turns 5e9 into INT_MIN on MSVC and
	// 2.5 into 2, and samJsOptI32 ignores a non-number and keeps the default -- so
	// sam_clear_stat_modifiers(true) cleared every player where Lua raised. This refuses all three,
	// the same things luaL_checkinteger refuses. `required` false lets an absent argument keep *io.
	static bool samJsRuleInt(JSContext* ctx, int argc, JSValueConst* argv, int i, int32_t* io, const char* who, bool required)
	{
		if ( !samHasArg(argc, argv, i) )
		{
			if ( !required ) { return true; }
			SAM_ERROR("JS", std::string(who) + ": argument " + std::to_string(i + 1) + " is required.");
			return false;
		}
		double d = 0.0;
		if ( !samJsNum(ctx, argv[i], &d) || !std::isfinite(d) || d != std::floor(d)
			|| d < -2147483648.0 || d > 2147483647.0 )
		{
			SAM_ERROR("JS", std::string(who) + ": argument " + std::to_string(i + 1)
				+ " must be a whole number that fits in 32 bits.");
			return false;
		}
		*io = (int32_t)d;
		return true;
	}

	// An amount that is present but is not a number is REFUSED, not ignored. The shared
	// samJsOptF64 falls back to the default with a warning, which for a writer means a mod asking
	// for "+eleven" is told yes and handed +0. Lua raises on the same call, so this keeps the two
	// runtimes giving the same answer.
	static bool samJsRuleAmount(JSContext* ctx, int argc, JSValueConst* argv, int i, double* io, const char* who)
	{
		if ( !samHasArg(argc, argv, i) ) { return true; }
		double d = 0.0;
		if ( !samJsNum(ctx, argv[i], &d) )
		{
			SAM_ERROR("JS", std::string(who) + ": argument " + std::to_string(i + 1) + " must be a number.");
			return false;
		}
		*io = d;
		return true;
	}

	// sam_add_stat_modifier(player, stat, id, add [, multiply]) -> boolean.
	// Adds sum and multipliers multiply ACROSS EVERY MOD, so two mods stack instead of fighting.
	JSValue js_sam_add_stat_modifier(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; std::string stat, id; double add = 0.0, mult = 1.0;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_add_stat_modifier", true) ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_add_stat_modifier", &stat) ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 2, "sam_add_stat_modifier", &id) ) { return JS_FALSE; }
		if ( !samJsRuleAmount(ctx, argc, argv, 3, &add, "sam_add_stat_modifier") ) { return JS_FALSE; }
		if ( !samJsRuleAmount(ctx, argc, argv, 4, &mult, "sam_add_stat_modifier") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT )
		{
			SAM_WARN("JS", "sam_add_stat_modifier refused: host only. The host owns the table and"
				" sends the totals on.");
			return JS_FALSE;
		}
		int kind = 0;
		if ( !samJsStatKind(stat.c_str(), "sam_add_stat_modifier", &kind) ) { return JS_FALSE; }
		return SAMRules::addPlayerModifier((int)player, kind, g_currentNs.c_str(), id.c_str(), add, mult) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_add_monster_stat_modifier(uid, stat, id, add [, multiply]) -> boolean. Dies with the floor.
	JSValue js_sam_add_monster_stat_modifier(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; std::string stat, id; double add = 0.0, mult = 1.0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_add_monster_stat_modifier") ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_add_monster_stat_modifier", &stat) ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 2, "sam_add_monster_stat_modifier", &id) ) { return JS_FALSE; }
		if ( !samJsRuleAmount(ctx, argc, argv, 3, &add, "sam_add_monster_stat_modifier") ) { return JS_FALSE; }
		if ( !samJsRuleAmount(ctx, argc, argv, 4, &mult, "sam_add_monster_stat_modifier") ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_add_monster_stat_modifier refused: host only."); return JS_FALSE; }
		int kind = 0;
		if ( !samJsStatKind(stat.c_str(), "sam_add_monster_stat_modifier", &kind) ) { return JS_FALSE; }
		if ( !samResolveMonster((long long)uid) )
		{
			SAM_ERROR("JS", "sam_add_monster_stat_modifier: uid " + std::to_string((long long)uid)
				+ " is not a living monster. For a player, use sam_add_stat_modifier.");
			return JS_FALSE;
		}
		return SAMRules::addMonsterModifier((long long)uid, kind, g_currentNs.c_str(), id.c_str(), add, mult) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_remove_stat_modifier(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; std::string id;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_remove_stat_modifier", true) ) { return JS_NewInt32(ctx, 0); }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_remove_stat_modifier", &id) ) { return JS_NewInt32(ctx, 0); }
#ifdef SAM_JS_HAVE_BARONY
		return JS_NewInt32(ctx, SAMRules::removePlayerModifier((int)player, g_currentNs.c_str(), id.c_str()));
#else
		return JS_NewInt32(ctx, 0);
#endif
	}

	JSValue js_sam_remove_monster_stat_modifier(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; std::string id;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_remove_monster_stat_modifier") ) { return JS_NewInt32(ctx, 0); }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_remove_monster_stat_modifier", &id) ) { return JS_NewInt32(ctx, 0); }
#ifdef SAM_JS_HAVE_BARONY
		return JS_NewInt32(ctx, SAMRules::removeMonsterModifier((long long)uid, g_currentNs.c_str(), id.c_str()));
#else
		return JS_NewInt32(ctx, 0);
#endif
	}

	// sam_clear_stat_modifiers([player]) -> count. No argument clears every player AND monster.
	JSValue js_sam_clear_stat_modifiers(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_clear_stat_modifiers", false) ) { return JS_NewInt32(ctx, 0); }
#ifdef SAM_JS_HAVE_BARONY
		int n = SAMRules::clearPlayerModifiers((int)player, g_currentNs.c_str());
		if ( player < 0 ) { n += SAMRules::clearMonsterModifiers(0, g_currentNs.c_str()); }
		return JS_NewInt32(ctx, n);
#else
		return JS_NewInt32(ctx, 0);
#endif
	}

	// sam_get_stat_modifier(player, stat, id) -> { add, multiply } or undefined.
	JSValue js_sam_get_stat_modifier(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; std::string stat, id;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_get_stat_modifier", true) ) { return JS_UNDEFINED; }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_get_stat_modifier", &stat) ) { return JS_UNDEFINED; }
		if ( !samJsReqStr(ctx, argc, argv, 2, "sam_get_stat_modifier", &id) ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		int kind = 0;
		if ( !samJsStatKind(stat.c_str(), "sam_get_stat_modifier", &kind) ) { return JS_UNDEFINED; }
		double add = 0.0, mult = 1.0;
		if ( !SAMRules::getPlayerModifier((int)player, kind, g_currentNs.c_str(), id.c_str(), &add, &mult) ) { return JS_UNDEFINED; }
		JSValue o = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, o, "add", JS_NewFloat64(ctx, add));
		JS_SetPropertyStr(ctx, o, "multiply", JS_NewFloat64(ctx, mult));
		return o;
#else
		return JS_UNDEFINED;
#endif
	}

	// sam_set_immunity(player, effect [, on]) -> boolean.
	JSValue js_sam_set_immunity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0; std::string eff;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_set_immunity", true) ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_set_immunity", &eff) ) { return JS_FALSE; }
		const bool on = samBoolArgJs(ctx, argc, argv, 2, true);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_immunity refused: host only."); return JS_FALSE; }
		const int id = samEffectNameToId(eff.c_str());
		if ( id < 0 ) { SAM_ERROR("JS", "sam_set_immunity: unknown effect '" + eff + "'."); return JS_FALSE; }
		return SAMRules::setPlayerImmunity((int)player, id, on, g_currentNs.c_str()) ? JS_TRUE : JS_FALSE;
#else
		(void)on; return JS_FALSE;
#endif
	}

	JSValue js_sam_set_monster_immunity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; std::string eff;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_monster_immunity") ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_set_monster_immunity", &eff) ) { return JS_FALSE; }
		const bool on = samBoolArgJs(ctx, argc, argv, 2, true);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_monster_immunity refused: host only."); return JS_FALSE; }
		const int id = samEffectNameToId(eff.c_str());
		if ( id < 0 ) { SAM_ERROR("JS", "sam_set_monster_immunity: unknown effect '" + eff + "'."); return JS_FALSE; }
		if ( on && !samResolveMonster((long long)uid) )
		{
			SAM_ERROR("JS", "sam_set_monster_immunity: uid " + std::to_string((long long)uid)
				+ " is not a living monster. For a player, use sam_set_immunity.");
			return JS_FALSE;
		}
		return SAMRules::setMonsterImmunity((long long)uid, id, on, g_currentNs.c_str()) ? JS_TRUE : JS_FALSE;
#else
		(void)on; return JS_FALSE;
#endif
	}

	JSValue js_sam_set_species_immunity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		std::string species, eff;
		if ( !samJsReqStr(ctx, argc, argv, 0, "sam_set_species_immunity", &species) ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_set_species_immunity", &eff) ) { return JS_FALSE; }
		const bool on = samBoolArgJs(ctx, argc, argv, 2, true);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_species_immunity refused: host only."); return JS_FALSE; }
		const int sp = samMonsterNameToId(species.c_str());
		if ( sp < 0 ) { SAM_ERROR("JS", "sam_set_species_immunity: '" + species + "' is not a creature this game has."); return JS_FALSE; }
		const int id = samEffectNameToId(eff.c_str());
		if ( id < 0 ) { SAM_ERROR("JS", "sam_set_species_immunity: unknown effect '" + eff + "'."); return JS_FALSE; }
		return SAMRules::setSpeciesImmunity(sp, id, on, g_currentNs.c_str()) ? JS_TRUE : JS_FALSE;
#else
		(void)on; return JS_FALSE;
#endif
	}

	// sam_is_immune(uid, effect) -> boolean. OUR table only; vanilla's own immunities cannot be
	// queried, so a false does not promise the effect will land.
	JSValue js_sam_is_immune(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; std::string eff;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_is_immune") ) { return JS_FALSE; }
		if ( !samJsReqStr(ctx, argc, argv, 1, "sam_is_immune", &eff) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		const int id = samEffectNameToId(eff.c_str());
		if ( id < 0 ) { return JS_FALSE; }
		Entity* e = SAMLua::resolveEntityQuiet((long long)uid);
		if ( !e ) { return JS_FALSE; }
		return SAMRules::isImmune(e->getStats(), e, id) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_clear_immunities(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		return JS_NewInt32(ctx, SAMRules::clearImmunities(g_currentNs.c_str()));
#else
		return JS_NewInt32(ctx, 0);
#endif
	}

	// sam_set_xp_curve(level, threshold) -> boolean. Level -1 sets the flat value for every level.
	JSValue js_sam_set_xp_curve(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t level = 0, threshold = 100;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &level, "sam_set_xp_curve", true) ) { return JS_FALSE; }
		if ( !samJsRuleInt(ctx, argc, argv, 1, &threshold, "sam_set_xp_curve", true) ) { return JS_FALSE; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_xp_curve refused: host only."); return JS_FALSE; }
		return SAMRules::setXpThreshold((int)level, (int)threshold, g_currentNs.c_str()) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	JSValue js_sam_clear_xp_curve(JSContext* ctx, JSValueConst, int, JSValueConst*)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		return JS_NewInt32(ctx, SAMRules::clearXpCurve(g_currentNs.c_str()));
#else
		return JS_NewInt32(ctx, 0);
#endif
	}

	JSValue js_sam_get_xp_threshold(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t level = 0;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &level, "sam_get_xp_threshold", true) ) { return JS_NewInt32(ctx, 100); }
#ifdef SAM_JS_HAVE_BARONY
		return JS_NewInt32(ctx, SAMRules::xpThreshold((int)level));
#else
		return JS_NewInt32(ctx, 100);
#endif
	}

	// sam_grant_xp(player, amount) -> the EXP afterwards, or undefined.
	JSValue js_sam_grant_xp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = 0, amount = 0;
		if ( !samJsRuleInt(ctx, argc, argv, 0, &player, "sam_grant_xp", true) ) { return JS_UNDEFINED; }
		if ( !samJsRuleInt(ctx, argc, argv, 1, &amount, "sam_grant_xp", true) ) { return JS_UNDEFINED; }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_grant_xp refused: host only."); return JS_UNDEFINED; }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] )
		{
			SAM_ERROR("JS", "sam_grant_xp: invalid player index " + std::to_string(player) + ".");
			return JS_UNDEFINED;
		}
		long long v = (long long)stats[player]->EXP + (long long)amount;
		if ( v < 0 ) { v = 0; }
		if ( v > 2147483647LL ) { v = 2147483647LL; }
		stats[player]->EXP = (Sint32)v;
		SAMLua::flushStatToClient((int)player);
		return JS_NewInt32(ctx, stats[player]->EXP);
#else
		return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_remove_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		// Required, as in Lua (see sam_grant_spell).
		int32_t player = -1;
		if ( !samJsReqI32(ctx, argc, argv, 0, &player, "sam_remove_spell") ) { return JS_FALSE; }
		if ( !samHasArg(argc, argv, 1) ) { SAM_ERROR("JS", "sam_remove_spell: argument 2 (the spell) is required."); return JS_FALSE; }
		const char* spellC = JS_ToCString(ctx, argv[1]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( SAM_CLIENT_REFUSES ) { SAM_WARN("JS", "sam_remove_spell refused: host only."); return JS_FALSE; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_FALSE; }
		const int id = SAMSpells::resolveSpellRef(spell);
		// Said out loud, as the Lua twin always did; this used to fail in silence.
		if ( id < 0 ) { SAM_ERROR("JS", "sam_remove_spell: unknown spell '" + spell + "'."); return JS_FALSE; }
		return SAMSpells::queueRemoveSpell((int)player, id) ? JS_TRUE : JS_FALSE;
#else
		(void)player; (void)spell; return JS_FALSE;
#endif
	}

	// ===== P_ITEMS: the loot pool (sam_loot.cpp) -- JS twins of the Lua bindings ==============

#ifdef SAM_JS_HAVE_BARONY
	// A category argument: a name or the numeric Category; -1 when it is neither. With allowAny,
	// "ANY" sets isAny and returns -1.
	static int samJsLootCategoryArg(JSContext* ctx, int argc, JSValueConst* argv, int i, bool allowAny, bool& isAny)
	{
		isAny = false;
		if ( !samHasArg(argc, argv, i) ) { return -1; }
		if ( JS_IsNumber(argv[i]) ) { int32_t n = 0; JS_ToInt32(ctx, &n, argv[i]); return n; }
		if ( !JS_IsString(argv[i]) ) { return -1; }
		const char* s = JS_ToCString(ctx, argv[i]);
		if ( !s ) { return -1; }
		int cat = -1;
		if ( allowAny && SAMLoot::isAnyCategoryName(s) ) { isAny = true; }
		else { cat = SAMLoot::categoryFromName(s); }
		JS_FreeCString(ctx, s);
		return cat;
	}

	// A context argument: -1 absent (undefined or null), -2 not a context (logged), else the kind.
	static int samJsLootContextArg(JSContext* ctx, int argc, JSValueConst* argv, int i, const char* who)
	{
		if ( !samHasArg(argc, argv, i) ) { return -1; }
		std::string s;
		samJsOptStr(ctx, argc, argv, i, &s);
		const int kind = SAMLoot::kindFromName(s);
		if ( kind < 0 )
		{
			SAM_ERROR("JS", std::string(who) + ": '" + s + "' is not a loot context. Valid: floor chest shop monster recipe console other.");
			return -2;
		}
		return kind;
	}
#endif

	// sam_get_loot_pool(category, min_level, max_level [, context]) -> array | undefined. JS twin.
	JSValue js_sam_get_loot_pool(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		bool any = false;
		const int cat = samJsLootCategoryArg(ctx, argc, argv, 0, false, any);
		int32_t minLevel = 0, maxLevel = 0;
		if ( !samJsReqI32(ctx, argc, argv, 1, &minLevel, "sam_get_loot_pool") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &maxLevel, "sam_get_loot_pool") ) { return JS_UNDEFINED; }
		const int kind = samJsLootContextArg(ctx, argc, argv, 3, "sam_get_loot_pool");
		if ( kind == -2 ) { return JS_UNDEFINED; }
		std::vector<SAMLoot::PoolEntry> out;
		if ( !SAMLoot::pool(cat, minLevel, maxLevel, kind, out) ) { return JS_UNDEFINED; }
		JSValue arr = JS_NewArray(ctx);
		uint32_t n = 0;
		for ( const SAMLoot::PoolEntry& e : out )
		{
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, e.type));
			JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e.name.c_str()));
			JS_SetPropertyStr(ctx, o, "level", JS_NewInt32(ctx, e.level));
			JS_SetPropertyStr(ctx, o, "weight", JS_NewInt32(ctx, e.weight));
			JS_SetPropertyUint32(ctx, arr, n++, o);
		}
		return arr;
#else
		return JS_UNDEFINED;
#endif
	}

	// sam_set_loot_weight(item, weight) -> bool. JS twin.
	JSValue js_sam_set_loot_weight(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		// A call with no arguments names itself in the log, as the Lua twin's "unknown item" does;
		// a bare false left a data-driven table silently short.
		if ( argc < 1 ) { SAM_ERROR("JS", "sam_set_loot_weight: argument 1 (the item) is required."); return JS_FALSE; }
		const int id = samJsResolveItem(ctx, argv[0]);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_set_loot_weight: unknown item."); return JS_FALSE; }
		int32_t weight = 1;
		if ( !samJsReqI32(ctx, argc, argv, 1, &weight, "sam_set_loot_weight") ) { return JS_FALSE; }
		return SAMLoot::setWeight(id, weight) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_loot_category_weight(category, weight) -> bool. JS twin.
	JSValue js_sam_set_loot_category_weight(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		bool any = false;
		const int cat = samJsLootCategoryArg(ctx, argc, argv, 0, false, any);
		int32_t weight = 1;
		if ( !samJsReqI32(ctx, argc, argv, 1, &weight, "sam_set_loot_category_weight") ) { return JS_FALSE; }
		return SAMLoot::setCategoryWeight(cat, weight) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_loot_floor_range(item, min_floor [, max_floor]) -> bool. JS twin.
	JSValue js_sam_set_loot_floor_range(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		if ( argc < 1 ) { SAM_ERROR("JS", "sam_set_loot_floor_range: argument 1 (the item) is required."); return JS_FALSE; }
		const int id = samJsResolveItem(ctx, argv[0]);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_set_loot_floor_range: unknown item."); return JS_FALSE; }
		int32_t minFloor = 0, maxFloor = -1;
		if ( !samJsReqI32(ctx, argc, argv, 1, &minFloor, "sam_set_loot_floor_range") ) { return JS_FALSE; }
		samJsOptI32(ctx, argc, argv, 2, &maxFloor, "sam_set_loot_floor_range");
		return SAMLoot::setFloorRange(id, minFloor, maxFloor) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_loot_context(item, context, allowed) -> bool. JS twin.
	JSValue js_sam_set_loot_context(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		if ( argc < 1 ) { SAM_ERROR("JS", "sam_set_loot_context: argument 1 (the item) is required."); return JS_FALSE; }
		const int id = samJsResolveItem(ctx, argv[0]);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_set_loot_context: unknown item."); return JS_FALSE; }
		const int kind = samJsLootContextArg(ctx, argc, argv, 1, "sam_set_loot_context");
		if ( kind < 0 )
		{
			if ( kind == -1 ) { SAM_ERROR("JS", "sam_set_loot_context: argument 2 (the context) is required."); }
			return JS_FALSE;
		}
		bool allowed = false;
		if ( !samBoolReqJs(ctx, argc, argv, 2, "sam_set_loot_context", &allowed) ) { return JS_FALSE; }
		return SAMLoot::setContextAllowed(id, kind, allowed) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_spell_droppable(spell, allowed [, min_floor]) -> bool. JS twin.
	JSValue js_sam_set_spell_droppable(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		if ( argc < 1 ) { SAM_ERROR("JS", "sam_set_spell_droppable: argument 1 (the spell) is required."); return JS_FALSE; }
		int spell = -1;
		if ( JS_IsNumber(argv[0]) ) { int32_t n = 0; JS_ToInt32(ctx, &n, argv[0]); spell = n; }
		else if ( JS_IsString(argv[0]) )
		{
			const char* s = JS_ToCString(ctx, argv[0]);
			if ( s ) { spell = SAMSpells::resolveSpellRef(s); JS_FreeCString(ctx, s); }
		}
		if ( spell < 0 )
		{
			SAM_ERROR("JS", "sam_set_spell_droppable: unknown spell (expected a SPELL_ name, \"namespace:spell\" or a spell id).");
			return JS_FALSE;
		}
		bool allowed = false;
		if ( !samBoolReqJs(ctx, argc, argv, 1, "sam_set_spell_droppable", &allowed) ) { return JS_FALSE; }
		int32_t minFloor = 0;
		samJsOptI32(ctx, argc, argv, 2, &minFloor, "sam_set_spell_droppable");
		return SAMLoot::setSpellDroppable(spell, allowed, minFloor) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_loot_fallback(category, item [, context]) -> bool. JS twin (item null/undefined =
	// vanilla again).
	JSValue js_sam_set_loot_fallback(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		bool any = false;
		const int cat = samJsLootCategoryArg(ctx, argc, argv, 0, true, any);
		if ( cat < 0 && !any )
		{
			SAM_ERROR("JS", "sam_set_loot_fallback: not a category. Valid: ANY WEAPON ARMOR AMULET POTION SCROLL MAGICSTAFF RING SPELLBOOK GEM THROWN TOOL FOOD BOOK");
			return JS_FALSE;
		}
		int item = -1;
		if ( samHasArg(argc, argv, 1) )
		{
			item = samJsResolveItem(ctx, argv[1]);
			if ( item < 0 ) { SAM_ERROR("JS", "sam_set_loot_fallback: unknown item."); return JS_FALSE; }
		}
		const int kind = samJsLootContextArg(ctx, argc, argv, 2, "sam_set_loot_fallback");
		if ( kind == -2 ) { return JS_FALSE; }
		return SAMLoot::setFallback(cat, item, kind) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_roll_loot(category, min_level, max_level [, context]) -> item type | undefined. JS twin.
	JSValue js_sam_roll_loot(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		bool any = false;
		const int cat = samJsLootCategoryArg(ctx, argc, argv, 0, false, any);
		int32_t minLevel = 0, maxLevel = 0;
		if ( !samJsReqI32(ctx, argc, argv, 1, &minLevel, "sam_roll_loot") ) { return JS_UNDEFINED; }
		if ( !samJsReqI32(ctx, argc, argv, 2, &maxLevel, "sam_roll_loot") ) { return JS_UNDEFINED; }
		const int kind = samJsLootContextArg(ctx, argc, argv, 3, "sam_roll_loot");
		if ( kind == -2 ) { return JS_UNDEFINED; }
		const int type = SAMLoot::roll(cat, minLevel, maxLevel, kind);
		if ( type < 0 ) { return JS_UNDEFINED; }
		return JS_NewInt32(ctx, type);
#else
		return JS_UNDEFINED;
#endif
	}

	// sam_add_item_to_container(uid, item [, count [, status [, beatitude [, identified [, appearance]]]]])
	//   -> item uid | undefined. JS twin.
	JSValue js_sam_add_item_to_container(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_add_item_to_container") ) { return JS_UNDEFINED; }
		if ( argc < 2 ) { SAM_ERROR("JS", "sam_add_item_to_container: argument 2 (the item) is required."); return JS_UNDEFINED; }
		const int type = samJsResolveItem(ctx, argv[1]);
		if ( type < 0 ) { SAM_ERROR("JS", "sam_add_item_to_container: unknown item."); return JS_UNDEFINED; }
		int32_t count = 1, status = (int32_t)EXCELLENT, beatitude = 0;
		int64_t appearance = -1;
		samJsOptI32(ctx, argc, argv, 2, &count, "sam_add_item_to_container");
		samJsOptI32(ctx, argc, argv, 3, &status, "sam_add_item_to_container");
		samJsOptI32(ctx, argc, argv, 4, &beatitude, "sam_add_item_to_container");
		const bool identified = samBoolArgJs(ctx, argc, argv, 5, true);
		samJsOptI64(ctx, argc, argv, 6, &appearance, "sam_add_item_to_container");
		const long long itemUid = SAMLoot::addToContainer((std::uint32_t)uid, type, count, status, beatitude, identified, (long long)appearance);
		if ( itemUid < 0 ) { return JS_UNDEFINED; }
		return JS_NewInt64(ctx, (int64_t)itemUid);
#else
		return JS_UNDEFINED;
#endif
	}

	// sam_remove_item_from_container(uid, item_or_uid [, count]) -> bool. JS twin.
	JSValue js_sam_remove_item_from_container(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_remove_item_from_container") ) { return JS_FALSE; }
		if ( argc < 2 ) { SAM_ERROR("JS", "sam_remove_item_from_container: argument 2 (an item uid or type) is required."); return JS_FALSE; }
		long long what = -1;
		if ( JS_IsNumber(argv[1]) ) { double d = 0.0; if ( !samJsNum(ctx, argv[1], &d) ) { return JS_FALSE; } what = (long long)d; }
		else
		{
			what = samJsResolveItem(ctx, argv[1]);
			if ( what < 0 ) { SAM_ERROR("JS", "sam_remove_item_from_container: unknown item."); return JS_FALSE; }
		}
		int32_t count = 0;
		samJsOptI32(ctx, argc, argv, 2, &count, "sam_remove_item_from_container");
		return SAMLoot::removeFromContainer((std::uint32_t)uid, what, count) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_shop_stock(shopkeeper_uid, items) -> bool. JS twin: an array of { item, count,
	// status, beatitude, identified } objects or bare item names / ids.
	JSValue js_sam_set_shop_stock(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		int64_t uid = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_shop_stock") ) { return JS_FALSE; }
		if ( argc < 2 || !JS_IsArray(argv[1]) )
		{
			SAM_ERROR("JS", "sam_set_shop_stock: argument 2 must be an array of items.");
			return JS_FALSE;
		}
		JSValue lenV = JS_GetPropertyStr(ctx, argv[1], "length");
		uint32_t n = 0;
		JS_ToUint32(ctx, &n, lenV);
		JS_FreeValue(ctx, lenV);
		std::vector<SAMLoot::StockEntry> entries;
		for ( uint32_t i = 0; i < n; ++i )
		{
			JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
			SAMLoot::StockEntry en;
			if ( JS_IsObject(v) && !JS_IsArray(v) )
			{
				JSValue it = samJsGetPropCI(ctx, v, "item");
				if ( JS_IsUndefined(it) ) { JS_FreeValue(ctx, it); it = samJsGetPropCI(ctx, v, "type"); }
				en.type = samJsResolveItem(ctx, it);
				JS_FreeValue(ctx, it);
				int iv = 0;
				if ( samJsGetIntProp(ctx, v, "count", iv) ) { en.count = iv; }
				if ( samJsGetIntProp(ctx, v, "status", iv) ) { en.status = iv; }
				if ( samJsGetIntProp(ctx, v, "beatitude", iv) ) { en.beatitude = iv; }
				JSValue idv = samJsGetPropCI(ctx, v, "identified");
				if ( !JS_IsUndefined(idv) ) { en.identified = ( JS_ToBool(ctx, idv) != 0 ); }
				JS_FreeValue(ctx, idv);
			}
			else
			{
				en.type = samJsResolveItem(ctx, v);
			}
			JS_FreeValue(ctx, v);
			if ( en.type < 0 )
			{
				SAM_ERROR("JS", "sam_set_shop_stock: entry " + std::to_string(i + 1) + " is not an item this game has. Nothing was changed.");
				return JS_FALSE;
			}
			entries.push_back(en);
		}
		return SAMLoot::setShopStock((std::uint32_t)uid, entries) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// sam_set_shop_type(shopkeeper_uid, store_type) -> bool. JS twin.
	JSValue js_sam_set_shop_type(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
#ifdef SAM_JS_HAVE_BARONY
		int64_t uid = 0;
		int32_t storeType = 0;
		if ( !samJsReqI32Wide(ctx, argc, argv, 0, &uid, "sam_set_shop_type") ) { return JS_FALSE; }
		if ( !samJsReqI32(ctx, argc, argv, 1, &storeType, "sam_set_shop_type") ) { return JS_FALSE; }
		return SAMLoot::setShopType((std::uint32_t)uid, storeType) ? JS_TRUE : JS_FALSE;
#else
		return JS_FALSE;
#endif
	}

	// ---- sandbox construction -------------------------------------------------
	JSContext* newSandboxContext(JSRuntime* rt)
	{
		// JS_NewContext adds ONLY the standard ECMAScript intrinsics (Object/Array/
		// String/Math/JSON/Map/Promise/RegExp/TypedArrays/BigInt/eval/...). It does
		// NOT add quickjs-libc's std/os modules — those require an explicit
		// js_init_module_std/os call, which we never make and never link. So the
		// sandbox is complete by default: no fs/net/os, no print/console/require.
		// (We use the standard JS_NewContext rather than JS_NewContextRaw + hand-
		// picked intrinsics: same sandbox surface, but the well-trodden teardown
		// path that leaves gc_obj_list empty at JS_FreeRuntime.)
		return JS_NewContext(rt);
	}

	void registerHostApi(JSContext* ctx)
	{
		JSValue g = JS_GetGlobalObject(ctx);
		samJsRegister(ctx, g, "sam_log", js_sam_log, 1);
		// ---- the rules (v3.0.0) -----------------------------------------------
		samJsRegister(ctx, g, "sam_add_stat_modifier", js_sam_add_stat_modifier, 5);
		samJsRegister(ctx, g, "sam_add_monster_stat_modifier", js_sam_add_monster_stat_modifier, 5);
		samJsRegister(ctx, g, "sam_remove_stat_modifier", js_sam_remove_stat_modifier, 2);
		samJsRegister(ctx, g, "sam_remove_monster_stat_modifier", js_sam_remove_monster_stat_modifier, 2);
		samJsRegister(ctx, g, "sam_clear_stat_modifiers", js_sam_clear_stat_modifiers, 1);
		samJsRegister(ctx, g, "sam_get_stat_modifier", js_sam_get_stat_modifier, 3);
		samJsRegister(ctx, g, "sam_set_immunity", js_sam_set_immunity, 3);
		samJsRegister(ctx, g, "sam_set_monster_immunity", js_sam_set_monster_immunity, 3);
		samJsRegister(ctx, g, "sam_set_species_immunity", js_sam_set_species_immunity, 3);
		samJsRegister(ctx, g, "sam_is_immune", js_sam_is_immune, 2);
		samJsRegister(ctx, g, "sam_clear_immunities", js_sam_clear_immunities, 0);
		samJsRegister(ctx, g, "sam_set_xp_curve", js_sam_set_xp_curve, 2);
		samJsRegister(ctx, g, "sam_clear_xp_curve", js_sam_clear_xp_curve, 0);
		samJsRegister(ctx, g, "sam_get_xp_threshold", js_sam_get_xp_threshold, 1);
		samJsRegister(ctx, g, "sam_grant_xp", js_sam_grant_xp, 2);
		// ---- the camera (v3.0.0) ----------------------------------------------
		samJsRegister(ctx, g, "sam_set_camera_offset", js_sam_set_camera_offset, 4);
		samJsRegister(ctx, g, "sam_set_camera_position", js_sam_set_camera_position, 4);
		samJsRegister(ctx, g, "sam_set_camera_angle", js_sam_set_camera_angle, 3);
		samJsRegister(ctx, g, "sam_set_camera_target", js_sam_set_camera_target, 2);
		samJsRegister(ctx, g, "sam_set_camera_collision", js_sam_set_camera_collision, 2);
		samJsRegister(ctx, g, "sam_get_camera", js_sam_get_camera, 1);
		samJsRegister(ctx, g, "sam_reset_camera", js_sam_reset_camera, 1);
		samJsRegister(ctx, g, "sam_show_own_body", js_sam_show_own_body, 2);
		// ---- v2.8 batch 4: combat ----------------------------------------------
		samJsRegister(ctx, g, "sam_heal", js_sam_heal, 2);
		samJsRegister(ctx, g, "sam_deal_damage_typed", js_sam_deal_damage_typed, 3);
		samJsRegister(ctx, g, "sam_get_hp", js_sam_get_hp, 1);
		samJsRegister(ctx, g, "sam_get_max_hp", js_sam_get_max_hp, 1);
		samJsRegister(ctx, g, "sam_get_mp", js_sam_get_mp, 1);
		samJsRegister(ctx, g, "sam_get_max_mp", js_sam_get_max_mp, 1);
		samJsRegister(ctx, g, "sam_get_attack", js_sam_get_attack, 1);
		samJsRegister(ctx, g, "sam_get_ranged_attack", js_sam_get_ranged_attack, 2);
		samJsRegister(ctx, g, "sam_get_thrown_attack", js_sam_get_thrown_attack, 1);
		samJsRegister(ctx, g, "sam_get_bonus_attack_vs", js_sam_get_bonus_attack_vs, 2);
		samJsRegister(ctx, g, "sam_get_damage_resist", js_sam_get_damage_resist, 2);
		samJsRegister(ctx, g, "sam_get_magic_resist", js_sam_get_magic_resist, 1);
		samJsRegister(ctx, g, "sam_preview_damage", js_sam_preview_damage, 2);
		samJsRegister(ctx, g, "sam_get_regen_interval", js_sam_get_regen_interval, 1);
		samJsRegister(ctx, g, "sam_get_healring", js_sam_get_healring, 1);
		samJsRegister(ctx, g, "sam_mod_mp", js_sam_mod_mp, 2);
		samJsRegister(ctx, g, "sam_drain_mp", js_sam_drain_mp, 3);
		samJsRegister(ctx, g, "sam_consume_mp", js_sam_consume_mp, 2);
		samJsRegister(ctx, g, "sam_set_defending", js_sam_set_defending, 2);
		samJsRegister(ctx, g, "sam_is_parrying", js_sam_is_parrying, 1);
		samJsRegister(ctx, g, "sam_set_parry", js_sam_set_parry, 2);
		samJsRegister(ctx, g, "sam_get_monster_target_uid", js_sam_get_monster_target_uid, 1);
		samJsRegister(ctx, g, "sam_set_monster_target_uid", js_sam_set_monster_target_uid, 3);
		samJsRegister(ctx, g, "sam_clear_monster_target", js_sam_clear_monster_target, 2);
		samJsRegister(ctx, g, "sam_alert_allies", js_sam_alert_allies, 2);
		samJsRegister(ctx, g, "sam_break_armor", js_sam_break_armor, 2);
		samJsRegister(ctx, g, "sam_gib", js_sam_gib, 2);
		samJsRegister(ctx, g, "sam_obituary", js_sam_obituary, 3);
		samJsRegister(ctx, g, "sam_revive_player", js_sam_revive_player, 3);
		samJsRegister(ctx, g, "sam_set_species_damage_resist", js_sam_set_species_damage_resist, 3);
		samJsRegister(ctx, g, "sam_clear_species_damage_resist", js_sam_clear_species_damage_resist, 2);
		samJsRegister(ctx, g, "sam_add_damage_multiplier", js_sam_add_damage_multiplier, 1);
		samJsRegister(ctx, g, "sam_grant_item", js_sam_grant_item, 5);
		samJsRegister(ctx, g, "sam_save_data", js_sam_save_data, 2);
		samJsRegister(ctx, g, "sam_load_data", js_sam_load_data, 1);
		samJsRegister(ctx, g, "sam_delete_data", js_sam_delete_data, 1);
		samJsRegister(ctx, g, "sam_set_timer", js_sam_set_timer, 3);
		samJsRegister(ctx, g, "sam_set_repeating_timer", js_sam_set_repeating_timer, 3);
		samJsRegister(ctx, g, "sam_cancel_timer", js_sam_cancel_timer, 1);
		samJsRegister(ctx, g, "sam_register_hook", js_sam_register_hook, 1);
		samJsRegister(ctx, g, "sam_fire_hook", js_sam_fire_hook, 2);
		samJsRegister(ctx, g, "sam_modify_damage", js_sam_modify_damage, 2);
		samJsRegister(ctx, g, "sam_modify_monster_damage", js_sam_modify_monster_damage, 1);
		samJsRegister(ctx, g, "sam_modify_value", js_sam_modify_value, 1);
		samJsRegister(ctx, g, "sam_deal_damage", js_sam_deal_damage, 2);
		samJsRegister(ctx, g, "sam_is_key_held", js_sam_is_key_held, 2);
		// v0.7.0 Feature 4: monster / NPC scripting (UID-based, host-authoritative)
		samJsRegister(ctx, g, "sam_get_player_data", js_sam_get_player_data, 2);
		samJsRegister(ctx, g, "sam_set_player_data", js_sam_set_player_data, 3);
		samJsRegister(ctx, g, "sam_get_effect_duration", js_sam_get_effect_duration, 2);
		samJsRegister(ctx, g, "sam_get_effect_strength", js_sam_get_effect_strength, 2);
		samJsRegister(ctx, g, "sam_get_effects", js_sam_get_effects, 1);
		samJsRegister(ctx, g, "sam_get_monster_stat", js_sam_get_monster_stat, 2);
		samJsRegister(ctx, g, "sam_test_done", js_sam_test_done, 2);
		samJsRegister(ctx, g, "sam_is_host", js_sam_is_host, 0);
		samJsRegister(ctx, g, "sam_play_sound_at", js_sam_play_sound_at, 4);
		samJsRegister(ctx, g, "sam_play_music", js_sam_play_music, 4);
		samJsRegister(ctx, g, "sam_stop_music", js_sam_stop_music, 0);
		samJsRegister(ctx, g, "sam_get_music", js_sam_get_music, 0);
		samJsRegister(ctx, g, "sam_list_music", js_sam_list_music, 0);
		samJsRegister(ctx, g, "sam_list_sounds", js_sam_list_sounds, 0);
		samJsRegister(ctx, g, "sam_stop_sound", js_sam_stop_sound, 1);
		samJsRegister(ctx, g, "sam_play_sound_entity", js_sam_play_sound_entity, 3);
		samJsRegister(ctx, g, "sam_spawn_particle", js_sam_spawn_particle, 5);
		samJsRegister(ctx, g, "sam_damage_number", js_sam_damage_number, 3);
		samJsRegister(ctx, g, "sam_get_tile", js_sam_get_tile, 2);
		samJsRegister(ctx, g, "sam_set_tile", js_sam_set_tile, 4);
		samJsRegister(ctx, g, "sam_is_spawnable", js_sam_is_spawnable, 2);
		samJsRegister(ctx, g, "sam_line_of_sight", js_sam_line_of_sight, 5);
		samJsRegister(ctx, g, "sam_tiles_connected", js_sam_tiles_connected, 5);
		samJsRegister(ctx, g, "sam_get_light_at", js_sam_get_light_at, 3);
		samJsRegister(ctx, g, "sam_find_entities", js_sam_find_entities, 4);
		samJsRegister(ctx, g, "sam_get_container_items", js_sam_get_container_items, 1);
		samJsRegister(ctx, g, "sam_set_door", js_sam_set_door, 2);
		samJsRegister(ctx, g, "sam_set_door_locked", js_sam_set_door_locked, 2);
		samJsRegister(ctx, g, "sam_power_entity", js_sam_power_entity, 2);
		samJsRegister(ctx, g, "sam_toggle_switch", js_sam_toggle_switch, 1);
		samJsRegister(ctx, g, "sam_get_level_info", js_sam_get_level_info, 0);
		samJsRegister(ctx, g, "sam_get_effective_stat", js_sam_get_effective_stat, 2);
		samJsRegister(ctx, g, "sam_get_ac", js_sam_get_ac, 1);
		samJsRegister(ctx, g, "sam_get_skill", js_sam_get_skill, 3);
		samJsRegister(ctx, g, "sam_is_enemy", js_sam_is_enemy, 2);
		samJsRegister(ctx, g, "sam_is_friend", js_sam_is_friend, 2);
		samJsRegister(ctx, g, "sam_get_mods", js_sam_get_mods, 0);
		samJsRegister(ctx, g, "sam_is_mod_loaded", js_sam_is_mod_loaded, 1);
		samJsRegister(ctx, g, "sam_hud_text", js_sam_hud_text, 5);
		samJsRegister(ctx, g, "sam_hud_bar", js_sam_hud_bar, 7);
		samJsRegister(ctx, g, "sam_hud_clear", js_sam_hud_clear, 1);
		// v1.10.3 -- the mod's own pictures (overlay + HUD art).
		samJsRegister(ctx, g, "sam_show_image", js_sam_show_image, 5);
		samJsRegister(ctx, g, "sam_show_image_at", js_sam_show_image_at, 8);
		samJsRegister(ctx, g, "sam_hide_image", js_sam_hide_image, 1);
		samJsRegister(ctx, g, "sam_hud_image", js_sam_hud_image, 7);
		samJsRegister(ctx, g, "sam_get_image_size", js_sam_get_image_size, 1);
		// v1.11.0 -- interactive panels.
		samJsRegister(ctx, g, "sam_ui_open", js_sam_ui_open, 7);
		samJsRegister(ctx, g, "sam_ui_close", js_sam_ui_close, 1);
		samJsRegister(ctx, g, "sam_ui_is_open", js_sam_ui_is_open, 2);
		samJsRegister(ctx, g, "sam_ui_clear", js_sam_ui_clear, 1);
		samJsRegister(ctx, g, "sam_ui_label", js_sam_ui_label, 7);
		samJsRegister(ctx, g, "sam_ui_button", js_sam_ui_button, 7);
		samJsRegister(ctx, g, "sam_ui_image", js_sam_ui_image, 8);
		samJsRegister(ctx, g, "sam_ui_list", js_sam_ui_list, 6);
		samJsRegister(ctx, g, "sam_ui_list_add", js_sam_ui_list_add, 5);
		samJsRegister(ctx, g, "sam_ui_list_clear", js_sam_ui_list_clear, 2);
		samJsRegister(ctx, g, "sam_ui_input", js_sam_ui_input, 7);
		samJsRegister(ctx, g, "sam_ui_input_text", js_sam_ui_input_text, 3);
		samJsRegister(ctx, g, "sam_ui_panel_style", js_sam_ui_panel_style, 4);
		samJsRegister(ctx, g, "sam_ui_font", js_sam_ui_font, 3);
		samJsRegister(ctx, g, "sam_ui_list_row_height", js_sam_ui_list_row_height, 3);
		samJsRegister(ctx, g, "sam_ui_text_size", js_sam_ui_text_size, 2);
		samJsRegister(ctx, g, "sam_list_items", js_sam_list_items, 1);
		samJsRegister(ctx, g, "sam_get_item_info", js_sam_get_item_info, 1);
		samJsRegister(ctx, g, "sam_list_monsters", js_sam_list_monsters, 0);
		samJsRegister(ctx, g, "sam_list_spells", js_sam_list_spells, 0);
		samJsRegister(ctx, g, "sam_spawn_projectile", js_sam_spawn_projectile, 8);
		samJsRegister(ctx, g, "sam_send_packet", js_sam_send_packet, 3);
		samJsRegister(ctx, g, "sam_player_count", js_sam_player_count, 0);
		samJsRegister(ctx, g, "sam_local_player", js_sam_local_player, 0);
		samJsRegister(ctx, g, "sam_monster_path_to", js_sam_monster_path_to, 3);
		samJsRegister(ctx, g, "sam_monster_face", js_sam_monster_face, 3);
		samJsRegister(ctx, g, "sam_monster_attack", js_sam_monster_attack, 1);
		samJsRegister(ctx, g, "sam_monster_charge", js_sam_monster_charge, 2);
		samJsRegister(ctx, g, "sam_get_monster_type", js_sam_get_monster_type, 1);
		samJsRegister(ctx, g, "sam_get_monster_name", js_sam_get_monster_name, 1);
		samJsRegister(ctx, g, "sam_monster_has_effect", js_sam_monster_has_effect, 2);
		samJsRegister(ctx, g, "sam_monster_has_trait", js_sam_monster_has_trait, 2);
		samJsRegister(ctx, g, "sam_get_item_category", js_sam_get_item_category, 1);
		samJsRegister(ctx, g, "sam_set_monster_stat", js_sam_set_monster_stat, 3);
		samJsRegister(ctx, g, "sam_set_monster_name", js_sam_set_monster_name, 2);
		samJsRegister(ctx, g, "sam_set_model", js_sam_set_model, 2);
		samJsRegister(ctx, g, "sam_clear_model", js_sam_clear_model, 1);
		samJsRegister(ctx, g, "sam_get_model", js_sam_get_model, 1);
		samJsRegister(ctx, g, "sam_set_scale", js_sam_set_scale, 2);
		samJsRegister(ctx, g, "sam_set_entity_size", js_sam_set_entity_size, 3);
		samJsRegister(ctx, g, "sam_set_damage_immune", js_sam_set_damage_immune, 2);
		samJsRegister(ctx, g, "sam_is_damage_immune", js_sam_is_damage_immune, 1);
		samJsRegister(ctx, g, "sam_move_entity", js_sam_move_entity, 3);
		samJsRegister(ctx, g, "sam_apply_force", js_sam_apply_force, 4);
		samJsRegister(ctx, g, "sam_set_on_fire", js_sam_set_on_fire, 2);
		samJsRegister(ctx, g, "sam_get_entity_flag", js_sam_get_entity_flag, 2);
		samJsRegister(ctx, g, "sam_set_entity_flag", js_sam_set_entity_flag, 3);
		samJsRegister(ctx, g, "sam_set_elevation", js_sam_set_elevation, 2);
		samJsRegister(ctx, g, "sam_set_visible", js_sam_set_visible, 2);
		samJsRegister(ctx, g, "sam_monster_equip", js_sam_monster_equip, 6);
		samJsRegister(ctx, g, "sam_monster_unequip", js_sam_monster_unequip, 2);
		samJsRegister(ctx, g, "sam_apply_monster_effect", js_sam_apply_monster_effect, 3);
		samJsRegister(ctx, g, "sam_kill_monster", js_sam_kill_monster, 1);
		samJsRegister(ctx, g, "sam_spawn_monsters", js_sam_spawn_monsters, 3);
		samJsRegister(ctx, g, "sam_get_monster_target", js_sam_get_monster_target, 1);
		samJsRegister(ctx, g, "sam_set_monster_target", js_sam_set_monster_target, 2);
		samJsRegister(ctx, g, "sam_get_monster_data", js_sam_get_monster_data, 2);
		samJsRegister(ctx, g, "sam_set_monster_data", js_sam_set_monster_data, 3);
		// v0.7.0 Feature 5: modify existing content (revert on unload)
		samJsRegister(ctx, g, "sam_patch_class", js_sam_patch_class, 2);
		samJsRegister(ctx, g, "sam_unpatch_class", js_sam_unpatch_class, 1);
		samJsRegister(ctx, g, "sam_patch_item", js_sam_patch_item, 2);
		samJsRegister(ctx, g, "sam_patch_monster", js_sam_patch_monster, 2);
		samJsRegister(ctx, g, "sam_add_class_passive", js_sam_add_class_passive, 2);
		samJsRegister(ctx, g, "sam_remove_class_passive", js_sam_remove_class_passive, 2);
		// Custom spells (Session 1)
		samJsRegister(ctx, g, "sam_grant_spell", js_sam_grant_spell, 2);
		samJsRegister(ctx, g, "sam_cast_spell", js_sam_cast_spell, 2);
		// v1.5.0 spell freedom (twins of the Lua bindings)
		samJsRegister(ctx, g, "sam_cast_spell_at", js_sam_cast_spell_at, 3);
		samJsRegister(ctx, g, "sam_cast_spell_pos", js_sam_cast_spell_pos, 4);
		samJsRegister(ctx, g, "sam_monster_cast_spell", js_sam_monster_cast_spell, 2);
		samJsRegister(ctx, g, "sam_get_spells", js_sam_get_spells, 1);
		samJsRegister(ctx, g, "sam_player_knows_spell", js_sam_player_knows_spell, 2);
		samJsRegister(ctx, g, "sam_remove_spell", js_sam_remove_spell, 2);
#ifdef SAM_JS_HAVE_BARONY
		samJsRegister(ctx, g, "sam_grant_gold", js_sam_grant_gold, 2);
		samJsRegister(ctx, g, "sam_apply_effect", js_sam_apply_effect, 4);
		samJsRegister(ctx, g, "sam_remove_effect", js_sam_remove_effect, 2);
		// v1.5.0 player effect control + monster effect read/remove parity
		samJsRegister(ctx, g, "sam_clear_effects", js_sam_clear_effects, 1);
		samJsRegister(ctx, g, "sam_set_effect_duration", js_sam_set_effect_duration, 3);
		samJsRegister(ctx, g, "sam_set_effect_strength", js_sam_set_effect_strength, 3);
		samJsRegister(ctx, g, "sam_remove_monster_effect", js_sam_remove_monster_effect, 2);
		samJsRegister(ctx, g, "sam_get_monster_effect_duration", js_sam_get_monster_effect_duration, 2);
		samJsRegister(ctx, g, "sam_get_monster_effect_strength", js_sam_get_monster_effect_strength, 2);
		samJsRegister(ctx, g, "sam_get_monster_effects", js_sam_get_monster_effects, 1);
		samJsRegister(ctx, g, "sam_get_stat", js_sam_get_stat, 2);
		samJsRegister(ctx, g, "sam_set_stat", js_sam_set_stat, 3);
		samJsRegister(ctx, g, "sam_set_move_speed", js_sam_set_move_speed, 2);
		samJsRegister(ctx, g, "sam_add_move_speed", js_sam_add_move_speed, 2);
		samJsRegister(ctx, g, "sam_get_move_speed", js_sam_get_move_speed, 1);
		samJsRegister(ctx, g, "sam_level_up", js_sam_level_up, 2);
		samJsRegister(ctx, g, "sam_get_floor", js_sam_get_floor, 0);
		samJsRegister(ctx, g, "sam_get_seed", js_sam_get_seed, 0);
		samJsRegister(ctx, g, "sam_get_flag", js_sam_get_flag, 1);
		samJsRegister(ctx, g, "sam_is_ghost", js_sam_is_ghost, 1);
		samJsRegister(ctx, g, "sam_is_spirit_ghost", js_sam_is_spirit_ghost, 1);
		samJsRegister(ctx, g, "sam_random", js_sam_random, 3);
		samJsRegister(ctx, g, "sam_list_data_keys", js_sam_list_data_keys, 0);
		samJsRegister(ctx, g, "sam_spawn_item", js_sam_spawn_item, 6);
		samJsRegister(ctx, g, "sam_item_id", js_sam_item_id, 1);
		samJsRegister(ctx, g, "sam_message", js_sam_message, 2);
		samJsRegister(ctx, g, "sam_play_sound", js_sam_play_sound, 2);
		samJsRegister(ctx, g, "sam_get_nearby_entities", js_sam_get_nearby_entities, 2);
		samJsRegister(ctx, g, "sam_get_equipped_item", js_sam_get_equipped_item, 2);
		samJsRegister(ctx, g, "sam_get_equipped_item_id", js_sam_get_equipped_item_id, 2);
		samJsRegister(ctx, g, "sam_is_defending", js_sam_is_defending, 1);
		samJsRegister(ctx, g, "sam_is_action_held", js_sam_is_action_held, 2);
		samJsRegister(ctx, g, "sam_get_action_binding", js_sam_get_action_binding, 2);
		// P_SETTINGS: a mod's own rows in the Bindings page and the General tab (sam_settings.cpp).
		samJsRegister(ctx, g, "sam_register_action", js_sam_register_action, 4);
		samJsRegister(ctx, g, "sam_register_setting", js_sam_register_setting, 2);
		samJsRegister(ctx, g, "sam_get_setting", js_sam_get_setting, 1);
		samJsRegister(ctx, g, "sam_set_setting", js_sam_set_setting, 2);
		samJsRegister(ctx, g, "sam_list_settings", js_sam_list_settings, 0);
		samJsRegister(ctx, g, "sam_get_inventory_count", js_sam_get_inventory_count, 2);
		samJsRegister(ctx, g, "sam_has_effect", js_sam_has_effect, 2);
		samJsRegister(ctx, g, "sam_get_class", js_sam_get_class, 1);
		samJsRegister(ctx, g, "sam_get_race", js_sam_get_race, 1);
		samJsRegister(ctx, g, "sam_get_kills", js_sam_get_kills, 1);
		samJsRegister(ctx, g, "sam_get_time_played", js_sam_get_time_played, 0);
		// v2 world-ops: position / teleport / spawn / inventory.
		samJsRegister(ctx, g, "sam_get_player_uid", js_sam_get_player_uid, 1);
		samJsRegister(ctx, g, "sam_get_position", js_sam_get_position, 1);
		samJsRegister(ctx, g, "sam_can_stand", js_sam_can_stand, 3);
		samJsRegister(ctx, g, "sam_set_position", js_sam_set_position, 3);
		samJsRegister(ctx, g, "sam_spawn_monster", js_sam_spawn_monster, 4);
		samJsRegister(ctx, g, "sam_spawn_portal", js_sam_spawn_portal, 2);
		samJsRegister(ctx, g, "sam_remove_entity", js_sam_remove_entity, 1);
		samJsRegister(ctx, g, "sam_set_entity_facing", js_sam_set_entity_facing, 2);
		samJsRegister(ctx, g, "sam_look_at", js_sam_look_at, 2);
		samJsRegister(ctx, g, "sam_get_entity_facing", js_sam_get_entity_facing, 1);
		samJsRegister(ctx, g, "sam_register_behavior", js_sam_register_behavior, 2);
		samJsRegister(ctx, g, "sam_attach_behavior", js_sam_attach_behavior, 2);
		samJsRegister(ctx, g, "sam_detach_behavior", js_sam_detach_behavior, 1);
		samJsRegister(ctx, g, "sam_spawn_entity", js_sam_spawn_entity, 4);
		samJsRegister(ctx, g, "sam_set_chest_stash", js_sam_set_chest_stash, 2);
		samJsRegister(ctx, g, "sam_travel_to_level", js_sam_travel_to_level, 2);
		samJsRegister(ctx, g, "sam_world_save", js_sam_world_save, 2);
		samJsRegister(ctx, g, "sam_world_load", js_sam_world_load, 1);
		samJsRegister(ctx, g, "sam_world_clear", js_sam_world_clear, 1);
		samJsRegister(ctx, g, "sam_world_keys", js_sam_world_keys, 0);
		samJsRegister(ctx, g, "sam_get_inventory", js_sam_get_inventory, 1);
		samJsRegister(ctx, g, "sam_remove_item", js_sam_remove_item, 1);
		// v1.4.0 — floating companion ("Stand") + facing reader.
		samJsRegister(ctx, g, "sam_spawn_companion", js_sam_spawn_companion, 3);
		samJsRegister(ctx, g, "sam_companion_punch", js_sam_companion_punch, 1);
		samJsRegister(ctx, g, "sam_get_facing", js_sam_get_facing, 1);
		// v1.6.0 — impact frame: screen flash / manga burst / camera shake / hitstop.
		samJsRegister(ctx, g, "sam_screen_flash", js_sam_screen_flash, 6);
		samJsRegister(ctx, g, "sam_impact_frame", js_sam_impact_frame, 7);
		samJsRegister(ctx, g, "sam_camera_shake", js_sam_camera_shake, 2);
		samJsRegister(ctx, g, "sam_hitstop", js_sam_hitstop, 1);
		// The simulation speed (sam_speed.cpp).
		samJsRegister(ctx, g, "sam_set_game_speed", js_sam_set_game_speed, 2);
		samJsRegister(ctx, g, "sam_get_game_speed", js_sam_get_game_speed, 0);
		// P_ITEMS: the loot pool, its tables, the containers and the shops (sam_loot.cpp).
		samJsRegister(ctx, g, "sam_get_loot_pool", js_sam_get_loot_pool, 4);
		samJsRegister(ctx, g, "sam_set_loot_weight", js_sam_set_loot_weight, 2);
		samJsRegister(ctx, g, "sam_set_loot_category_weight", js_sam_set_loot_category_weight, 2);
		samJsRegister(ctx, g, "sam_set_loot_floor_range", js_sam_set_loot_floor_range, 3);
		samJsRegister(ctx, g, "sam_set_loot_context", js_sam_set_loot_context, 3);
		samJsRegister(ctx, g, "sam_set_spell_droppable", js_sam_set_spell_droppable, 3);
		samJsRegister(ctx, g, "sam_set_loot_fallback", js_sam_set_loot_fallback, 3);
		samJsRegister(ctx, g, "sam_roll_loot", js_sam_roll_loot, 4);
		samJsRegister(ctx, g, "sam_add_item_to_container", js_sam_add_item_to_container, 7);
		samJsRegister(ctx, g, "sam_remove_item_from_container", js_sam_remove_item_from_container, 3);
		samJsRegister(ctx, g, "sam_set_shop_stock", js_sam_set_shop_stock, 2);
		samJsRegister(ctx, g, "sam_set_shop_type", js_sam_set_shop_type, 2);
		// ---- v2.6 batch 1: reads and dice -------------------------------------
		samJsRegister(ctx, g, "sam_get_position_precise", js_sam_get_position_precise, 1);
		samJsRegister(ctx, g, "sam_get_distance", js_sam_get_distance, 2);
		samJsRegister(ctx, g, "sam_get_distance_to", js_sam_get_distance_to, 3);
		samJsRegister(ctx, g, "sam_get_entity_type", js_sam_get_entity_type, 1);
		samJsRegister(ctx, g, "sam_get_scale", js_sam_get_scale, 1);
		samJsRegister(ctx, g, "sam_is_visible", js_sam_is_visible, 1);
		samJsRegister(ctx, g, "sam_get_velocity", js_sam_get_velocity, 1);
		samJsRegister(ctx, g, "sam_get_entity_size", js_sam_get_entity_size, 1);
		samJsRegister(ctx, g, "sam_get_entity_sprite", js_sam_get_entity_sprite, 1);
		samJsRegister(ctx, g, "sam_get_entity_ticks", js_sam_get_entity_ticks, 1);
		samJsRegister(ctx, g, "sam_get_map_seed", js_sam_get_map_seed, 0);
		samJsRegister(ctx, g, "sam_is_dark_level", js_sam_is_dark_level, 0);
		samJsRegister(ctx, g, "sam_get_playable_bounds", js_sam_get_playable_bounds, 0);
		samJsRegister(ctx, g, "sam_is_tile_diggable", js_sam_is_tile_diggable, 2);
		samJsRegister(ctx, g, "sam_get_map_flags", js_sam_get_map_flags, 0);
		samJsRegister(ctx, g, "sam_get_exit_position", js_sam_get_exit_position, 0);
		samJsRegister(ctx, g, "sam_get_run_time", js_sam_get_run_time, 0);
		samJsRegister(ctx, g, "sam_get_tick_rate", js_sam_get_tick_rate, 0);
		samJsRegister(ctx, g, "sam_get_fps", js_sam_get_fps, 0);
		samJsRegister(ctx, g, "sam_get_real_time", js_sam_get_real_time, 0);
		samJsRegister(ctx, g, "sam_get_date", js_sam_get_date, 0);
		samJsRegister(ctx, g, "sam_is_paused", js_sam_is_paused, 0);
		samJsRegister(ctx, g, "sam_is_in_game", js_sam_is_in_game, 0);
		samJsRegister(ctx, g, "sam_is_loading", js_sam_is_loading, 0);
		samJsRegister(ctx, g, "sam_random_float", js_sam_random_float, 1);
		samJsRegister(ctx, g, "sam_random_chance", js_sam_random_chance, 2);
		samJsRegister(ctx, g, "sam_random_from_list", js_sam_random_from_list, 2);
		samJsRegister(ctx, g, "sam_random_weighted", js_sam_random_weighted, 2);
		samJsRegister(ctx, g, "sam_has_data", js_sam_has_data, 1);
		samJsRegister(ctx, g, "sam_world_bytes", js_sam_world_bytes, 0);
		samJsRegister(ctx, g, "sam_world_bytes_free", js_sam_world_bytes_free, 0);
		// ---- v2.6 batch 2: inventory and items --------------------------------
		samJsRegister(ctx, g, "sam_get_item", js_sam_get_item, 1);
		samJsRegister(ctx, g, "sam_get_item_name", js_sam_get_item_name, 1);
		samJsRegister(ctx, g, "sam_get_item_value", js_sam_get_item_value, 1);
		samJsRegister(ctx, g, "sam_get_item_weight", js_sam_get_item_weight, 1);
		samJsRegister(ctx, g, "sam_get_item_attack", js_sam_get_item_attack, 2);
		samJsRegister(ctx, g, "sam_get_item_ac", js_sam_get_item_ac, 2);
		samJsRegister(ctx, g, "sam_get_tome_spell", js_sam_get_tome_spell, 1);
		samJsRegister(ctx, g, "sam_get_food_satiation", js_sam_get_food_satiation, 1);
		samJsRegister(ctx, g, "sam_set_item_beatitude", js_sam_set_item_beatitude, 2);
		samJsRegister(ctx, g, "sam_set_item_status", js_sam_set_item_status, 2);
		samJsRegister(ctx, g, "sam_set_item_count", js_sam_set_item_count, 2);
		samJsRegister(ctx, g, "sam_identify_item", js_sam_identify_item, 2);
		samJsRegister(ctx, g, "sam_set_item_appearance", js_sam_set_item_appearance, 2);
		samJsRegister(ctx, g, "sam_set_item_droppable", js_sam_set_item_droppable, 2);
		samJsRegister(ctx, g, "sam_get_item_owner", js_sam_get_item_owner, 1);
		samJsRegister(ctx, g, "sam_set_item_owner", js_sam_set_item_owner, 2);
		samJsRegister(ctx, g, "sam_is_ranged_weapon", js_sam_is_ranged_weapon, 1);
		samJsRegister(ctx, g, "sam_is_melee_weapon", js_sam_is_melee_weapon, 1);
		samJsRegister(ctx, g, "sam_is_shield", js_sam_is_shield, 1);
		samJsRegister(ctx, g, "sam_is_potion_bad", js_sam_is_potion_bad, 1);
		samJsRegister(ctx, g, "sam_item_has_trait", js_sam_item_has_trait, 2);
		samJsRegister(ctx, g, "sam_get_item_slot", js_sam_get_item_slot, 1);
		samJsRegister(ctx, g, "sam_is_better_weapon", js_sam_is_better_weapon, 2);
		samJsRegister(ctx, g, "sam_is_better_armor", js_sam_is_better_armor, 2);
		samJsRegister(ctx, g, "sam_is_item_equipped", js_sam_is_item_equipped, 2);
		samJsRegister(ctx, g, "sam_can_unequip", js_sam_can_unequip, 2);
		samJsRegister(ctx, g, "sam_inventory_has_space", js_sam_inventory_has_space, 1);
		samJsRegister(ctx, g, "sam_get_max_stack", js_sam_get_max_stack, 2);
		samJsRegister(ctx, g, "sam_can_items_stack", js_sam_can_items_stack, 3);
		samJsRegister(ctx, g, "sam_monster_can_wield", js_sam_monster_can_wield, 2);
#endif
		JS_FreeValue(ctx, g);
	}

	// Run a JS source string in a fresh hardened context; capture its on_event.
	bool loadJSSource(const std::string& source, const std::string& label, const std::string& ns)
	{
		JSContext* ctx = newSandboxContext(g_rt);
		if ( !ctx ) { SAM_ERROR("JS", "failed to create sandbox context for " + label); return false; }
		registerHostApi(ctx);

		g_currentNs = ns; // live during eval so a script may sam_load_data() at startup
		setDeadline(g_cfg.callbackBudgetMs); // bound the top-level eval (kills a top-level infinite loop)
		JSValue res = JS_Eval(ctx, source.c_str(), source.size(), label.c_str(), JS_EVAL_TYPE_GLOBAL);
		clearDeadline();
		g_currentNs.clear();
		if ( JS_IsException(res) )
		{
			SAM_ERROR("JS", "error running '" + label + "': " + exceptionToString(ctx));
			JS_FreeValue(ctx, res);
			JS_FreeContext(ctx);
			return false;
		}
		JS_FreeValue(ctx, res);

		JSValue g = JS_GetGlobalObject(ctx);
		JSValue fn     = JS_GetPropertyStr(ctx, g, "on_event");
		JSValue tickFn = JS_GetPropertyStr(ctx, g, "on_tick"); // v0.7.0
		JS_FreeValue(ctx, g);

		const bool hasEvent = JS_IsFunction(ctx, fn);
		const bool hasTick  = JS_IsFunction(ctx, tickFn);
		if ( !hasEvent ) { JS_FreeValue(ctx, fn);     fn = JS_UNDEFINED; }
		if ( !hasTick )  { JS_FreeValue(ctx, tickFn); tickFn = JS_UNDEFINED; }

		Script sc; sc.ctx = ctx; sc.onEvent = fn; sc.onTick = tickFn; sc.path = label; sc.ns = ns;
		sc.enabled = ( hasEvent || hasTick );
		g_scripts.push_back(sc);

		if ( !sc.enabled )
		{
			// Mirror of the Lua diagnostic: name the mistake. Defining a function after an
			// EVENT name (on_action_pressed, on_hit, ...) registers nothing, because only
			// on_event/on_tick are ever called. See the Lua note for why this trips people.
			std::string strays;
			JSValue gobj = JS_GetGlobalObject(ctx);
			JSPropertyEnum* props = nullptr;
			uint32_t count = 0;
			if ( JS_GetOwnPropertyNames(ctx, &props, &count, gobj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 )
			{
				for ( uint32_t i = 0; i < count; ++i )
				{
					const char* n = JS_AtomToCString(ctx, props[i].atom);
					if ( n && strncmp(n, "on_", 3) == 0 && strcmp(n, "on_event") != 0 && strcmp(n, "on_tick") != 0 )
					{
						JSValue v = JS_GetProperty(ctx, gobj, props[i].atom);
						if ( JS_IsFunction(ctx, v) )
						{
							if ( !strays.empty() ) { strays += ", "; }
							strays += std::string(n) + "()";
						}
						JS_FreeValue(ctx, v);
					}
					if ( n ) { JS_FreeCString(ctx, n); }
					JS_FreeAtom(ctx, props[i].atom);
				}
				js_free(ctx, props);
			}
			JS_FreeValue(ctx, gobj);

			if ( !strays.empty() )
			{
				SAM_WARN("JS", "script '" + label + "' defines " + strays + " — that is an EVENT NAME, not a handler, "
					"so S.A.M never calls it and this script does nothing. S.A.M only calls on_event(event) and "
					"on_tick(event). Write it as: function on_event(event) { if (event.name === \"<the event>\") { ... } }");
			}
			else
			{
				SAM_WARN("JS", "script '" + label + "' defines neither on_event(event) nor on_tick(event) — no handler registered.");
			}
		}
		else
		{
			std::string handlers = hasEvent ? "on_event" : "";
			if ( hasTick ) { handlers += (handlers.empty() ? "" : " + ") + std::string("on_tick"); }
			SAM_INFO("JS", "Loaded script '" + label + "' (" + handlers + " registered).");
		}
		return true;
	}

	// ---- transpiler (typescript.js under a privileged QuickJS context) --------
	bool ensureTranspiler(const std::string& tsLibPath)
	{
		if ( g_tsCtx ) { return true; }
		std::string tsLib;
		if ( !readFile(tsLibPath, tsLib) )
		{
			SAM_ERROR("JS", "cannot read typescript.js at: " + tsLibPath);
			return false;
		}
		g_tsRt = JS_NewRuntime();
		if ( !g_tsRt ) { SAM_ERROR("JS", "transpile runtime alloc failed"); return false; }
		JS_SetMemoryLimit(g_tsRt, 256u * 1024u * 1024u); // generous but bounded (untrusted .ts input)
		JS_SetMaxStackSize(g_tsRt, 4u * 1024u * 1024u);  // typescript.js recurses deeply
		JS_SetInterruptHandler(g_tsRt, js_interrupt, nullptr);
		g_tsCtx = JS_NewContext(g_tsRt); // full standard context (still no libc/os — never linked)
		if ( !g_tsCtx )
		{
			// Free the runtime we just allocated, else the next call re-runs
			// JS_NewRuntime() (the g_tsCtx guard above is still null) and leaks it.
			SAM_ERROR("JS", "transpile context alloc failed");
			JS_FreeRuntime(g_tsRt); g_tsRt = nullptr;
			return false;
		}

		setDeadline(g_cfg.transpileBudgetMs);
		JSValue r = JS_Eval(g_tsCtx, tsLib.c_str(), tsLib.size(), "typescript.js", JS_EVAL_TYPE_GLOBAL);
		clearDeadline();
		if ( JS_IsException(r) )
		{
			// Tear down fully: leaving g_tsCtx/g_tsRt set would make the `if (g_tsCtx)`
			// fast-path above wrongly report the (broken) transpiler as ready, so every
			// later transpile() would run against a context where `ts` never attached.
			SAM_ERROR("JS", "failed to load typescript.js: " + exceptionToString(g_tsCtx));
			JS_FreeValue(g_tsCtx, r);
			JS_FreeContext(g_tsCtx); g_tsCtx = nullptr;
			JS_FreeRuntime(g_tsRt);  g_tsRt = nullptr;
			return false;
		}
		JS_FreeValue(g_tsCtx, r);

		JSValue g = JS_GetGlobalObject(g_tsCtx);
		JSValue tsv = JS_GetPropertyStr(g_tsCtx, g, "ts");
		const bool ok = JS_IsObject(tsv);
		JS_FreeValue(g_tsCtx, tsv);
		JS_FreeValue(g_tsCtx, g);
		if ( !ok )
		{
			SAM_ERROR("JS", "typescript.js loaded but global 'ts' is missing (UMD did not attach).");
			JS_FreeContext(g_tsCtx); g_tsCtx = nullptr;
			JS_FreeRuntime(g_tsRt);  g_tsRt = nullptr;
			return false;
		}
		SAM_INFO("JS", "TypeScript compiler loaded under QuickJS (" + std::to_string(tsLib.size() / 1024) + " KB).");
		return true;
	}

	bool transpile(const std::string& src, std::string& out)
	{
		JSValue g = JS_GetGlobalObject(g_tsCtx);
		JS_SetPropertyStr(g_tsCtx, g, "__sam_src", JS_NewStringLen(g_tsCtx, src.data(), src.size()));
		JS_FreeValue(g_tsCtx, g);

		static const char* EXPR =
			"ts.transpileModule(__sam_src, { compilerOptions: { target: ts.ScriptTarget.ES2020, "
			"isolatedModules: true } }).outputText";
		setDeadline(g_cfg.transpileBudgetMs);
		JSValue r = JS_Eval(g_tsCtx, EXPR, std::strlen(EXPR), "<transpile>", JS_EVAL_TYPE_GLOBAL);
		clearDeadline();
		if ( JS_IsException(r) )
		{
			SAM_ERROR("JS", "transpileModule threw: " + exceptionToString(g_tsCtx));
			JS_FreeValue(g_tsCtx, r);
			return false;
		}
		bool ok = false;
		const char* s = JS_ToCString(g_tsCtx, r);
		if ( s ) { out = s; JS_FreeCString(g_tsCtx, s); ok = !out.empty(); }
		JS_FreeValue(g_tsCtx, r);
		return ok;
	}

} // anonymous namespace

// ---------------------------------------------------------------------------
namespace SAMJs
{
	bool init(const SandboxConfig& cfg)
	{
		if ( g_rt ) { SAM_WARN("JS", "init() called twice — ignoring."); return true; }
		g_cfg = cfg;
		g_rt = JS_NewRuntime();
		if ( !g_rt ) { SAM_ERROR("JS", "JS_NewRuntime failed"); return false; }
		JS_SetMemoryLimit(g_rt, cfg.memoryCapBytes);
		JS_SetMaxStackSize(g_rt, cfg.maxStackBytes);
		JS_SetInterruptHandler(g_rt, js_interrupt, nullptr);
#ifdef SAM_JS_HAVE_BARONY
		SAMNet::setCallRunner('J', samJsRunForwarded);   // calls the host carries to this machine
#endif
		SAM_INFO("JS", "QuickJS runtime initialized (mem cap "
			+ std::to_string(cfg.memoryCapBytes / (1024u * 1024u)) + "MB, callback budget "
			+ std::to_string(cfg.callbackBudgetMs) + "ms, watchdog on).");
		return true;
	}

	bool loadScriptJS(const std::string& path, const std::string& ns)
	{
		if ( !g_rt ) { SAM_ERROR("JS", "loadScriptJS before init()"); return false; }
		std::string src;
		if ( !readFile(path, src) ) { SAM_ERROR("JS", "cannot read JS file: " + path); return false; }
		return loadJSSource(src, path, ns);
	}

	bool loadScriptTS(const std::string& path, const std::string& cacheDir, const std::string& tsCompilerJsPath, const std::string& ns)
	{
		if ( !g_rt ) { SAM_ERROR("JS", "loadScriptTS before init()"); return false; }
		std::string src;
		if ( !readFile(path, src) ) { SAM_ERROR("JS", "cannot read TS file: " + path); return false; }

		const std::string key = hashKey(src);
		const std::string cachePath = cacheDir + "/" + key + ".js";

		std::string js;
		if ( readFile(cachePath, js) )
		{
			SAM_INFO("JS", "TS cache HIT for '" + path + "' -> " + key + ".js");
			return loadJSSource(js, path, ns);
		}

		SAM_INFO("JS", "TS cache miss for '" + path + "' — transpiling...");
		if ( !ensureTranspiler(tsCompilerJsPath) ) { return false; }
		if ( !transpile(src, js) ) { SAM_ERROR("JS", "TypeScript transpile failed for: " + path); return false; }
		if ( !writeFileAtomic(cachePath, js) )
		{
			SAM_WARN("JS", "could not write TS cache to " + cachePath + " (continuing without cache).");
		}
		SAM_INFO("JS", "Transpiled '" + path + "' -> " + std::to_string(js.size()) + " bytes JS (cached " + key + ".js).");
		return loadJSSource(js, path, ns);
	}


// Set by the most recent dispatchEvent: did any handler return false? Mirrors the Lua
// runtime's flag so an engine site can ask one question and get both runtimes' answer.
bool g_lastDispatchCancelled = false;

	void releaseBehaviorFn(void* jsFn)
	{
		if ( !jsFn ) { return; }
		JSValue* fn = (JSValue*)jsFn;
		JSContext* ctx = nullptr;
		for ( auto& sc : g_scripts ) { if ( sc.ctx ) { ctx = sc.ctx; break; } }
		// If the runtime is already gone the JSValue died with it; just reclaim the wrapper.
		if ( ctx ) { JS_FreeValue(ctx, *fn); }
		delete fn;
	}

	void runBehaviorJs(int index, void* jsFn, unsigned long long uid,
		const std::string& ns, const std::string& name)
	{
		(void)index;
		if ( !jsFn || g_scripts.empty() ) { return; }
		JSValue* fn = (JSValue*)jsFn;
		// Every JS script shares one runtime; use the first live context to call through.
		JSContext* ctx = nullptr;
		for ( auto& sc : g_scripts ) { if ( sc.ctx ) { ctx = sc.ctx; break; } }
		if ( !ctx ) { return; }

		const std::string savedNs = g_currentNs;
		g_currentNs = ns;
		JSValue arg = JS_NewInt64(ctx, (int64_t)uid);
		setDeadline(g_cfg.callbackBudgetMs);
		JSValue ret = JS_Call(ctx, *fn, JS_UNDEFINED, 1, &arg);
		clearDeadline();
		g_currentNs = savedNs;
		JS_FreeValue(ctx, arg);
		if ( JS_IsException(ret) )
		{
			// One bad frame must not spam forever: report once and stop calling it. The
			// entity survives but stops thinking, which is visible and debuggable.
			SAM_ERROR("JS", "behaviour '" + name + "' errored: " + exceptionToString(ctx));
			SAM_WARN("JS", "behaviour '" + name + "' disabled.");
			SAMLogger::noteScriptError();
			// Clear the registry's pointer FIRST, then free -- and only if the row still holds
			// THIS function. Freeing first left a window in which the row pointed at a dead
			// JSValue; and the callback may have re-registered its own name before erroring,
			// in which case registerBehavior already released our pointer and installed a
			// replacement -- freeing here again would be a double free, and the old helper
			// would also have wiped the replacement out of the row.
			if ( SAMLua::clearBehaviorFnIf(name, jsFn) ) { releaseBehaviorFn(jsFn); }
		}
		JS_FreeValue(ctx, ret);
	}

	int dispatchEvent(const Event& ev)
	{
		// Reset BEFORE the early-out guard below. Doing it after meant a shutdown or a
		// pre-init dispatch left a stale `true` latched: every later veto-capable site
		// (itemPickup, castSpell, useItem) then saw a cancel nobody asked for, in a
		// session with no mods loaded at all.
		g_lastDispatchCancelled = false;

		if ( !g_rt )
		{
			// Expected during the pre-mod menu/char-select carousel (equips fire before
			// mods load). Drop harmlessly; log ONCE at info instead of spamming ERROR.
			static bool warnedBeforeInit = false;
			if ( !warnedBeforeInit )
			{
				warnedBeforeInit = true;
				SAM_INFO("JS", "dispatchEvent('" + ev.name + "') before init() — ignored (pre-mod menu; suppressing further notices).");
			}
			return 0;
		}
		int delivered = 0;
		g_lastDispatchCancelled = false;
		bool cancelled = false;
		// Preserve the caller's namespace across a possibly RE-ENTRANT dispatch (a script's
		// on_event may call a host API that fires another hook). Restore, don't clear, so an
		// outer script's sam_save_data still resolves its owning mod. See the Lua mirror.
		const std::string savedNs = g_currentNs;
#ifdef SAM_JS_HAVE_BARONY
		// A screen function called with no player inside a handler shows to the player this
		// event is about (sam_net.hpp).
		long long evPlayer = -1;
		for ( const auto& kv : ev.ints ) { if ( kv.first == "player" ) { evPlayer = kv.second; break; } }
		SAMNet::ContextPlayerScope evWho(evPlayer);
#endif
		for ( auto& sc : g_scripts )
		{
			if ( !sc.enabled ) { continue; }
			// A script may define only on_tick (no on_event); its onEvent is
			// JS_UNDEFINED. Calling JS_Call on undefined throws a TypeError that the
			// catch below would treat as a script error and permanently disable the
			// script (killing its on_tick too). Skip it here, mirroring the Lua
			// dispatchEvent's `callbackRef == LUA_NOREF` guard.
			if ( JS_IsUndefined(sc.onEvent) ) { continue; }
			JSValue evObj = makeEventObject(sc.ctx, ev);
			JSValue argv[1] = { evObj };
			g_currentNs = sc.ns;
			setDeadline(g_cfg.callbackBudgetMs);
			JSValue ret = JS_Call(sc.ctx, sc.onEvent, JS_UNDEFINED, 1, argv);
			clearDeadline();
			g_currentNs = savedNs;

			// Read back what the handler CHANGED before the object is freed. The event
			// object is an in/out parameter in JS exactly as it is in Lua: assigning to one
			// of the event's own fields proposes a new value to the engine site. Only keys
			// the engine supplied are considered, so a script cannot invent a field name.
			// Both runtimes write into the same store (SAMLua's), and JS dispatch runs after
			// Lua's, so a change made in either language reaches the site.
			if ( !JS_IsException(ret) )
			{
				for ( const auto& kv : ev.ints )
				{
					JSValue f = JS_GetPropertyStr(sc.ctx, evObj, kv.first.c_str());
					double d = 0;
					if ( JS_IsNumber(f) && JS_ToFloat64(sc.ctx, &d, f) == 0 )
					{
						// Record only a real CHANGE. Re-recording an untouched field would let
						// a script that merely reads the event overwrite an earlier script's
						// edit with the value it happened to be handed.
						const double seen = SAMLua::lastEventNumber(kv.first.c_str(), (double)kv.second);
						if ( d != seen ) { SAMLua::recordEventWriteBackNumber(kv.first.c_str(), d); }
					}
					JS_FreeValue(sc.ctx, f);
				}
				for ( const auto& kv : ev.strings )
				{
					JSValue f = JS_GetPropertyStr(sc.ctx, evObj, kv.first.c_str());
					if ( JS_IsString(f) )
					{
						size_t cLen = 0;
						const char* c = JS_ToCStringLen(sc.ctx, &cLen, f);
						if ( c )
						{
							const std::string cs(c, cLen);   // with its length, as the Lua side
							const std::string seen = SAMLua::lastEventString(kv.first.c_str(), kv.second);
							if ( seen != cs ) { SAMLua::recordEventWriteBackString(kv.first.c_str(), cs); }
							JS_FreeCString(sc.ctx, c);
						}
					}
					JS_FreeValue(sc.ctx, f);
				}
			}

			JS_FreeValue(sc.ctx, evObj);
			if ( JS_IsException(ret) )
			{
				SAM_ERROR("JS", "on_event error in '" + sc.path + "': " + exceptionToString(sc.ctx));
				SAM_WARN("JS", "script '" + sc.path + "' disabled after an on_event error.");
				sc.enabled = false;
				SAMLogger::noteScriptError();
			}
			else
			{
				// A host-API argument conversion (e.g. a throwing valueOf on a passed
				// object) can leave an exception pending even though the callback
				// returned a normal value. Surface + clear it so it neither lingers on
				// the shared runtime nor is silently swallowed.
				JSValue pend = JS_GetException(sc.ctx);
				// JS_GetException returns JS_UNINITIALIZED -- not JS_NULL -- when nothing is
				// pending; QuickJS's own JS_HasException is literally
				// !JS_IsUninitialized(current_exception). Testing for null alone was never
				// true, so this warned on EVERY dispatch of every JS script and printed the
				// sentinel itself as the message ("[uninitialized]"). 1169 lines in a
				// five-minute session, none of them a real error.
				if ( !JS_IsNull(pend) && !JS_IsUninitialized(pend) )
				{
					const char* pc = JS_ToCString(sc.ctx, pend);
					SAM_WARN("JS", "on_event in '" + sc.path + "' left a pending host-API error: "
						+ std::string(pc ? pc : "?"));
					if ( pc ) { JS_FreeCString(sc.ctx, pc); }
				}
				JS_FreeValue(sc.ctx, pend);
				// A handler returning exactly `false` asks the game not to do what it was
				// about to do. Strict check: undefined (no return, what every existing
				// script does) and null must NOT cancel, so this stays compatible.
				if ( JS_IsBool(ret) && !JS_ToBool(sc.ctx, ret) ) { cancelled = true; }
				++delivered;
			}
			JS_FreeValue(sc.ctx, ret);
		}
		SAMLogger::noteHookFired(delivered, ev.name.c_str()); // count + open the GAMEPLAY section on the first hook
		g_lastDispatchCancelled = cancelled;
		// A dispatch that reached NOBODY carries no information -- half a real session's
		// log was "Dispatched 'X' to 0 script(s)". Keep it at DEBUG so it is still there
		// with SAM_DEBUG set when you are working out why a hook is not firing.
		// Routine: counted in the SESSION SUMMARY rather than one line each. See the Lua side.
		SAM_DEBUG("JS", "Dispatched '" + ev.name + "' to " + std::to_string(delivered) + " script(s).");
		return delivered;
	}

	// v0.7.0: fire on_tick(event) for every script defining it, once per game tick
	// (host-only). Silent — no per-tick log, no hook count — since this runs ~50x/sec.
	void dispatchTick(long long tickCount)
	{
		if ( !g_rt ) { return; }
		const std::string savedNs = g_currentNs;
		for ( auto& sc : g_scripts )
		{
			if ( !sc.enabled || JS_IsUndefined(sc.onTick) ) { continue; }
			JSValue ev = JS_NewObject(sc.ctx);
			JS_SetPropertyStr(sc.ctx, ev, "tick_count", JS_NewInt64(sc.ctx, tickCount));
			JS_SetPropertyStr(sc.ctx, ev, "delta_ticks", JS_NewInt32(sc.ctx, 1));
			JSValue argv[1] = { ev };
			g_currentNs = sc.ns;
			setDeadline(g_cfg.callbackBudgetMs);
			JSValue ret = JS_Call(sc.ctx, sc.onTick, JS_UNDEFINED, 1, argv);
			clearDeadline();
			g_currentNs = savedNs;
			JS_FreeValue(sc.ctx, ev);
			if ( JS_IsException(ret) )
			{
				SAM_WARN("JS", "script '" + sc.path + "' disabled after an on_tick error.");
				sc.enabled = false;
				SAMLogger::noteScriptError();
			}
			else
			{
				// Clear any exception a host-API conversion left pending (see
				// dispatchEvent). Silent here — this runs ~50x/sec.
				JSValue pend = JS_GetException(sc.ctx);
				JS_FreeValue(sc.ctx, pend);
			}
			JS_FreeValue(sc.ctx, ret);
		}
	}

	void tickTimers()
	{
		if ( !g_rt || g_jsTimers.empty() ) { return; }
		struct Due { JSContext* ctx; JSValue cb; std::string ns; };
		std::vector<Due> due;
		for ( size_t i = 0; i < g_jsTimers.size(); )
		{
			JsTimer& t = g_jsTimers[i];
			if ( --t.remaining > 0 ) { ++i; continue; }
			if ( t.repeating )
			{
				due.push_back({ t.ctx, JS_DupValue(t.ctx, t.callback), t.ns });
				t.remaining = t.interval > 0 ? t.interval : 1;
				++i;
			}
			else
			{
				due.push_back({ t.ctx, t.callback, t.ns }); // transfer ownership out of g_jsTimers
				g_jsTimers.erase(g_jsTimers.begin() + i);
			}
		}
		const std::string savedNs = g_currentNs; // restore (not clear) for re-entrant safety
		for ( Due& d : due )
		{
			g_currentNs = d.ns;
			setDeadline(g_cfg.callbackBudgetMs);
			JSValue ret = JS_Call(d.ctx, d.cb, JS_UNDEFINED, 0, nullptr);
			clearDeadline();
			g_currentNs = savedNs;
			if ( JS_IsException(ret) ) { SAM_WARN("JS", "timer callback error: " + exceptionToString(d.ctx)); }
			else { JSValue pend = JS_GetException(d.ctx); JS_FreeValue(d.ctx, pend); } // clear a swallowed host-API error
			JS_FreeValue(d.ctx, ret);
			JS_FreeValue(d.ctx, d.cb);
		}
	}

	void releaseTranspiler()
	{
		if ( g_tsCtx ) { JS_FreeContext(g_tsCtx); g_tsCtx = nullptr; }
		if ( g_tsRt )  { JS_FreeRuntime(g_tsRt);  g_tsRt = nullptr; }
	}

	// Drop every pending timer (used on a new game so a prior run's timers don't
	// carry over). Callbacks are freed against their owning context first.
	void resetTimers()
	{
		for ( auto& t : g_jsTimers )
		{
			if ( t.ctx ) { JS_FreeValue(t.ctx, t.callback); }
		}
		g_jsTimers.clear();
	}

	void shutdown()
	{
		for ( auto& t : g_jsTimers )
		{
			if ( t.ctx ) { JS_FreeValue(t.ctx, t.callback); } // free before the owning contexts
		}
		g_jsTimers.clear();
		for ( auto& sc : g_scripts )
		{
			if ( sc.ctx )
			{
				JS_FreeValue(sc.ctx, sc.onEvent);
				JS_FreeValue(sc.ctx, sc.onTick);
				JS_FreeContext(sc.ctx);
			}
		}
		g_scripts.clear();
		if ( g_tsCtx ) { JS_FreeContext(g_tsCtx); g_tsCtx = nullptr; }
		if ( g_tsRt )  { JS_FreeRuntime(g_tsRt); g_tsRt = nullptr; }
		if ( g_rt )    { JS_FreeRuntime(g_rt);   g_rt = nullptr; }
		SAM_INFO("JS", "runtime shut down.");
	}

	std::size_t scriptCount() { return g_scripts.size(); }
	std::size_t enabledScriptCount()
	{
		std::size_t n = 0;
		for ( const auto& sc : g_scripts ) { if ( sc.enabled ) { ++n; } }
		return n;
	}
	bool isInitialized() { return g_rt != nullptr; }

	bool getGlobalInt(const std::string& name, long long& out)
	{
		for ( auto& sc : g_scripts )
		{
			if ( !sc.ctx ) { continue; }
			JSValue g = JS_GetGlobalObject(sc.ctx);
			JSValue v = JS_GetPropertyStr(sc.ctx, g, name.c_str());
			JS_FreeValue(sc.ctx, g);
			bool got = false;
			if ( JS_IsNumber(v) )
			{
				int64_t i = 0;
				if ( JS_ToInt64(sc.ctx, &i, v) == 0 ) { out = (long long)i; got = true; }
			}
			JS_FreeValue(sc.ctx, v);
			if ( got ) { return true; }
		}
		return false;
	}

	bool getGlobalString(const std::string& name, std::string& out)
	{
		for ( auto& sc : g_scripts )
		{
			if ( !sc.ctx ) { continue; }
			JSValue g = JS_GetGlobalObject(sc.ctx);
			JSValue v = JS_GetPropertyStr(sc.ctx, g, name.c_str());
			JS_FreeValue(sc.ctx, g);
			bool got = false;
			if ( JS_IsString(v) )
			{
				const char* s = JS_ToCString(sc.ctx, v);
				if ( s ) { out = s; JS_FreeCString(sc.ctx, s); got = true; }
			}
			JS_FreeValue(sc.ctx, v);
			if ( got ) { return true; }
		}
		return false;
	}


	bool lastDispatchCancelled() { return g_lastDispatchCancelled; }

} // namespace SAMJs
