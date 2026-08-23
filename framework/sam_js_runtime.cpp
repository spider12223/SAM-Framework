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
#include "sam_logger.hpp"

extern "C" {
#include "quickjs.h"
}

#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>       // v1.5.0: std::atan2 for aimed spell casts
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
#	include "paths.hpp"     // GeneratePathTypes (monster movement bindings)
#	include "engine/audio/sound.hpp" // playSoundPlayer, numsounds
#	include "files.hpp"     // outputdir (savegames base dir for persistent mod data)
#	include "sam_items.hpp" // SAMItems::itemIdForIdString (custom item names in queries)
#	include "sam_classes.hpp" // v0.7.0 F5: SAMClasses::patchClass / addClassPassive
#	include "sam_monster_patches.hpp" // v0.7.0 F5: SAMMonsterPatch::set
#	include "sam_monsters.hpp" // SAMMonsters::traitBitForName (sam_monster_has_trait)
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
		const std::string tmp = path + ".tmp";
		{
			std::ofstream f(tmp, std::ios::binary);
			if ( !f ) { return false; }
			f.write(data.data(), (std::streamsize)data.size());
			if ( !f ) { return false; }
		}
		std::remove(path.c_str());
		return std::rename(tmp.c_str(), path.c_str()) == 0;
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
		for ( const auto& kv : ev.ints )
		{
			JS_SetPropertyStr(ctx, obj, kv.first.c_str(), JS_NewInt64(ctx, (int64_t)kv.second));
		}
		for ( const auto& kv : ev.strings )
		{
			JS_SetPropertyStr(ctx, obj, kv.first.c_str(), JS_NewString(ctx, kv.second.c_str()));
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

	// ---- host functions exposed to scripts ------------------------------------
	JSValue js_sam_log(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc >= 1 )
		{
			const char* s = JS_ToCString(ctx, argv[0]);
			if ( s ) { SAM_INFO("SCRIPT", s); JS_FreeCString(ctx, s); }
		}
		return JS_UNDEFINED;
	}

	JSValue js_sam_grant_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
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
		if ( resolvedType < 0 )
		{
			std::string lower = name;
			for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
			auto it = ItemTooltips.itemNameStringToItemID.find(lower);
			if ( it != ItemTooltips.itemNameStringToItemID.end() ) { resolvedType = it->second; }
		}
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
		if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &beatitudeArg, argv[2]); }
		if ( samHasArg(argc, argv, 3) ) { JS_ToInt32(ctx, &statusArg, argv[3]); }
		if ( samHasArg(argc, argv, 4) ) { JS_ToInt32(ctx, &countArg, argv[4]); }
		const Sint16 beatitude = (Sint16)beatitudeArg;
		if ( statusArg < (int)BROKEN ) { statusArg = (int)BROKEN; }
		if ( statusArg > (int)EXCELLENT ) { statusArg = (int)EXCELLENT; }
		const Status status = (Status)statusArg;
		const Sint16 count = (Sint16)(countArg < 1 ? 1 : countArg);
		Item* item = newItem(type, status, beatitude, count, 0, true, nullptr);
		if ( !item ) { return JS_NewBool(ctx, 0); }
		if ( players[player]->isLocalPlayer() )
		{
			itemPickup(player, item);
			free(item);
		}
		else
		{
			free(item);
			SAM_WARN("JS", "sam_grant_item: remote-player delivery not wired yet; '" + name + "' not given.");
			return JS_NewBool(ctx, 0);
		}
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
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToInt32(ctx, &amount, argv[1]); }
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
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &ticks, argv[2]); }
		int32_t strength = 0;
		if ( samHasArg(argc, argv, 3) ) { JS_ToInt32(ctx, &strength, argv[3]); } // optional tier/magnitude
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
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
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
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
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
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &ticks, argv[2]); }
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
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &strength, argv[2]); }
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
		int32_t player = -1;
		std::string name;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_get_stat refused: host only."); return JS_NewInt32(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("JS", "sam_get_stat: invalid player index " + std::to_string(player) + "."); return JS_NewInt32(ctx, 0); }
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
		else { SAM_ERROR("JS", "sam_get_stat: unknown stat '" + name + "'."); return JS_NewInt32(ctx, 0); }
		return JS_NewInt64(ctx, v);
	}

	JSValue js_sam_set_stat(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1, value = 0;
		std::string name;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &value, argv[2]); }
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
		SAM_INFO("JS", "Set stat " + n + " = " + std::to_string(value) + " on player " + std::to_string(player));
		return JS_NewBool(ctx, 1);
	}

	// sam_set_move_speed(player, mult) — host-only; syncs to the owning client.
	JSValue js_sam_set_move_speed(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		double mult = 1.0;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToFloat64(ctx, &mult, argv[1]); }
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
		int32_t player = -1;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		return JS_NewFloat64(ctx, SAMLua::getMoveSpeedMult(player));
	}

	// sam_add_move_speed(player, delta) -> new multiplier. Additive counterpart to the
	// set-only sam_set_move_speed: stacks onto whatever the multiplier already is.
	JSValue js_sam_add_move_speed(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		double delta = 0.0;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToFloat64(ctx, &delta, argv[1]); }
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
		int32_t player = -1, count = 1;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToInt32(ctx, &count, argv[1]); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_level_up refused: host only."); return JS_NewBool(ctx, 0); }
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("JS", "sam_level_up: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const int levels = samClampInt(count, 1, 255);
		stats[player]->EXP += 100 * levels;
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

	JSValue js_sam_spawn_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t x = 0, y = 0;
		std::string name;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &x, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToInt32(ctx, &y, argv[1]); }
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
		if ( resolvedType < 0 )
		{
			std::string lower = name;
			for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
			auto it = ItemTooltips.itemNameStringToItemID.find(lower);
			if ( it != ItemTooltips.itemNameStringToItemID.end() ) { resolvedType = it->second; }
		}
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
		if ( samHasArg(argc, argv, 3) ) { JS_ToInt32(ctx, &statusArg, argv[3]); }
		if ( samHasArg(argc, argv, 4) ) { JS_ToInt32(ctx, &beatitudeArg, argv[4]); }
		if ( samHasArg(argc, argv, 5) ) { JS_ToInt32(ctx, &countArg, argv[5]); }
		const Status st = (Status)samClampInt(statusArg, (int)BROKEN, (int)EXCELLENT);
		const Sint16 be = (Sint16)samClampInt(beatitudeArg, -100, 100);
		const Sint16 ct = (Sint16)samClampInt(countArg, 1, 1000);

		Entity* e = spawnGroundItem(static_cast<ItemType>(resolvedType), st, be, ct, x, y);
		if ( !e ) { SAM_ERROR("JS", "sam_spawn_item: invalid tile (" + std::to_string(x) + "," + std::to_string(y) + ")."); return JS_NULL; }
		SAM_INFO("JS", "Spawned item " + name + " at (" + std::to_string(x) + "," + std::to_string(y)
			+ ") uid " + std::to_string((unsigned long long)e->getUID()));
		return JS_NewInt64(ctx, (int64_t)e->getUID());
	}

	// sam_item_id("VANILLA_NAME" | "namespace:item") -> number|null. Resolve an item
	// type's numeric id, for matching against event fields like on_block's shield_type.
	// A name containing ':' resolves a custom S.A.M item; otherwise the vanilla tooltip
	// name map is used (case-insensitive). Returns null if the item is unknown.
	JSValue js_sam_item_id(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NULL; }
		std::string name;
		{ const char* s = JS_ToCString(ctx, argv[0]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		int id = -1;
		if ( name.find(':') != std::string::npos )
		{
			id = SAMItems::itemIdForIdString(name);
		}
		else
		{
			std::string lower = name;
			for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
			auto it = ItemTooltips.itemNameStringToItemID.find(lower);
			if ( it != ItemTooltips.itemNameStringToItemID.end() ) { id = it->second; }
		}
		if ( id < 0 ) { return JS_NULL; }
		return JS_NewInt32(ctx, id);
	}

	JSValue js_sam_message(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		std::string text;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { text = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_message refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS )
		{ SAM_ERROR("JS", "sam_message: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		messagePlayer(player, MESSAGE_MISC, "%s", text.c_str());
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_play_sound(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t soundId = -1, vol = 128;
		if ( argc >= 1 && JS_IsString(argv[0]) )
		{
			const char* nm = JS_ToCString(ctx, argv[0]);
			soundId = nm ? SAMSounds::soundIndexForId(nm) : -1;
			if ( nm ) { JS_FreeCString(ctx, nm); }
			if ( soundId < 0 ) { SAM_ERROR("JS", "sam_play_sound: unknown sound name."); return JS_NewBool(ctx, 0); }
		}
		else if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &soundId, argv[0]); }
		if ( argc >= 2 && !JS_IsUndefined(argv[1]) ) { JS_ToInt32(ctx, &vol, argv[1]); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_play_sound refused: host only."); return JS_NewBool(ctx, 0); }
		if ( soundId < 0 || (Uint32)soundId >= numsounds )
		{ SAM_ERROR("JS", "sam_play_sound: sound id " + std::to_string(soundId) + " out of range (0.." + std::to_string(numsounds) + ")."); return JS_NewBool(ctx, 0); }
		vol = samClampInt(vol, 0, 255);
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( players[i] && !client_disconnected[i] )
			{
				playSoundPlayer(i, (Uint16)soundId, (Uint8)vol);
			}
		}
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_get_nearby_entities(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		double radiusTiles = 0.0;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToFloat64(ctx, &radiusTiles, argv[1]); }
		JSValue arr = JS_NewArray(ctx);
		if ( multiplayer == CLIENT ) { return arr; }
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
		if ( slot == "HELMET" )                          { return s->helmet; }
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
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		std::string slot; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { slot = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NULL; }
		for ( char& c : slot ) { c = (char)std::toupper((unsigned char)c); }
		Item* it = samEquippedSlotJs(player, slot);
		if ( !it ) { return JS_NULL; }
		return JS_NewString(ctx, samItemNameJs((int)it->type).c_str());
	}

	// sam_get_equipped_item_id(player, slot) -> number|null. The NUMERIC item type, so it
	// can be compared against sam_item_id("ns:item"). js_sam_get_equipped_item above
	// returns a display NAME from the vanilla name table, which never contains custom
	// items — so it can never match a custom id.
	JSValue js_sam_get_equipped_item_id(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		std::string slot; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { slot = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NULL; }
		for ( char& c : slot ) { c = (char)std::toupper((unsigned char)c); }
		Item* it = samEquippedSlotJs(player, slot);
		if ( !it ) { return JS_NULL; }
		return JS_NewInt32(ctx, (int)it->type);
	}

	// sam_is_defending(player) -> boolean. Real engine blocking state, not just the button
	// being down. Correct for remote players too — vanilla syncs it with its 'SHLD' packet.
	JSValue js_sam_is_defending(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, stats[player]->defending ? 1 : 0);
	}

	// sam_is_action_held(player, "Use") -> boolean. Reads a BOUND action, so it follows
	// the player's keybinds. Local player only (input never leaves its machine).
	JSValue js_sam_is_action_held(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		std::string action; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { action = s; JS_FreeCString(ctx, s); } }
		return JS_NewBool(ctx, SAMLua::isActionHeld(player, action) ? 1 : 0);
	}

	// sam_get_action_binding(player, "Use") -> string|null. The physical input behind an
	// action ("Mouse3"), for prompts. null when the player has it unbound.
	JSValue js_sam_get_action_binding(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		std::string action; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { action = s; JS_FreeCString(ctx, s); } }
		const char* b = SAMLua::actionBinding(player, action);
		if ( !b || !b[0] ) { return JS_NULL; }
		return JS_NewString(ctx, b);
	}

	JSValue js_sam_get_inventory_count(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		std::string name; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NewInt32(ctx, 0); }
		std::string lower = name; for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		int wantType = -1;
		auto mit = ItemTooltips.itemNameStringToItemID.find(lower);
		if ( mit != ItemTooltips.itemNameStringToItemID.end() ) { wantType = mit->second; }
		else { const int cid = SAMItems::itemIdForIdString(name); if ( cid >= 0 ) { wantType = cid; } }
		if ( wantType < 0 ) { return JS_NewInt32(ctx, 0); }
		long long total = 0;
		for ( node_t* node = stats[player]->inventory.first; node != nullptr; node = node->next )
		{
			Item* it = (Item*)node->element;
			if ( it && (int)it->type == wantType ) { total += it->count; }
		}
		return JS_NewInt64(ctx, total);
	}

	JSValue js_sam_has_effect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		std::string name; if ( samHasArg(argc, argv, 1) ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NewBool(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 ) { SAM_WARN("JS", "sam_has_effect: unknown effect '" + name + "'. Valid: " + SAMLua::effectNameHint()); return JS_NewBool(ctx, 0); }
		return JS_NewBool(ctx, stats[player]->getEffectActive(eff) != 0 ? 1 : 0);
	}

	JSValue js_sam_get_class(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( player < 0 || player >= MAXPLAYERS ) { return JS_NULL; }
		// SAM-aware, mirroring the Lua binding: custom ids resolve from the registry, since
		// playerClassLangEntry returns a bogus string for them (see the Lua samClassName note).
		const int cls = client_classes[player];
		if ( cls >= SAM_CLASS_ID_BASE )
		{
			const SAMClassDef* def = SAMClasses::getClass(cls);
			return JS_NewString(ctx, def ? def->name.c_str() : "");
		}
		return JS_NewString(ctx, playerClassLangEntry(cls, player));
	}

	// The race identifier for a player: a custom race's "namespace:race" id, or the
	// vanilla race's name. Lets a race behavior script gate its logic by race.
	JSValue js_sam_get_race(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return JS_NULL; }
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
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
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
		return base + "/" + samSanitize(ns) + "/" + samSanitize(key) + ".json";
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
		JSValue jstr = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
		std::string json = "null";
		if ( !JS_IsException(jstr) && !JS_IsUndefined(jstr) )
		{
			const char* s = JS_ToCString(ctx, jstr);
			if ( s ) { json = s; JS_FreeCString(ctx, s); }
		}
		JS_FreeValue(ctx, jstr);
		const std::string path = samModDataFile(g_currentNs, key);
		try
		{
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
			std::ofstream f(path, std::ios::binary | std::ios::trunc);
			if ( !f.is_open() ) { SAM_ERROR("JS", "sam_save_data: cannot write " + path); return JS_NewBool(ctx, 0); }
			f << json;
		}
		catch ( ... ) { SAM_ERROR("JS", "sam_save_data: failed writing key '" + key + "'."); return JS_NewBool(ctx, 0); }
		SAM_INFO("SAM", "Saved data key '" + key + "' for [" + g_currentNs + "]");
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_load_data(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_UNDEFINED; }
		const char* keyC = JS_ToCString(ctx, argv[0]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( g_currentNs.empty() ) { return JS_UNDEFINED; }
		std::ifstream f(samModDataFile(g_currentNs, key), std::ios::binary);
		if ( !f.is_open() ) { return JS_UNDEFINED; }
		const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		JSValue v = JS_ParseJSON(ctx, text.c_str(), text.size(), "sam_mod_data");
		if ( JS_IsException(v) ) { JS_FreeValue(ctx, v); SAM_WARN("JS", "sam_load_data: corrupt data for key '" + key + "' — undefined."); return JS_UNDEFINED; }
		SAM_INFO("SAM", "Loaded data key '" + key + "' for [" + g_currentNs + "]");
		return v;
	}

	// ---- v1.11.0 persistent world state (Lua parity: sam_set_chest_stash /
	//      sam_travel_to_level / sam_world_save / _load / _clear / _keys) ------------------
	// Rationale and the engine details are documented once, on the Lua side.

	JSValue js_sam_set_chest_stash(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const bool on = samHasArg(argc, argv, 1) ? ( JS_ToBool(ctx, argv[1]) > 0 ) : true;
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_chest_stash refused: host only."); return JS_FALSE; }
		Entity* e = uidToEntity((Sint32)uid);
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
		if ( argc < 1 ) { return JS_FALSE; }
		int32_t target = 0; JS_ToInt32(ctx, &target, argv[0]);
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
		JSValue jstr = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
		std::string json = "null";
		if ( !JS_IsException(jstr) && !JS_IsUndefined(jstr) )
		{
			const char* c = JS_ToCString(ctx, jstr);
			if ( c ) { json = c; JS_FreeCString(ctx, c); }
		}
		JS_FreeValue(ctx, jstr);
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
		if ( argc < 3 ) { return JS_UNDEFINED; }
		const char* idC = JS_ToCString(ctx, argv[0]);
		const std::string id = idC ? idC : "";
		if ( idC ) { JS_FreeCString(ctx, idC); }
		int32_t ticks = 0; JS_ToInt32(ctx, &ticks, argv[1]);
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
		const int n = SAMJs::dispatchEvent(jsev) + SAMLua::dispatchEvent(ev);
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
		if ( argc < 2 ) { return JS_UNDEFINED; }
		int32_t player = 0; JS_ToInt32(ctx, &player, argv[0]);
		int64_t v = 0;      JS_ToInt64(ctx, &v, argv[1]);
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
		if ( argc < 1 ) { return JS_UNDEFINED; }
		int64_t v = 0; JS_ToInt64(ctx, &v, argv[0]);
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
		if ( argc < 1 ) { return JS_UNDEFINED; }
		int64_t v = 0; JS_ToInt64(ctx, &v, argv[0]);
		if ( !SAMLua::hookValueActive() )
		{
			SAM_WARN("JS", "sam_modify_value: only valid inside a hook that offers a value to rewrite — ignored.");
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
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0;    JS_ToInt64(ctx, &uid, argv[0]);
		int32_t amount = 0; JS_ToInt32(ctx, &amount, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_deal_damage refused: host only."); return JS_FALSE; }
		Entity* e = uidToEntity((Uint32)uid);
		if ( !e ) { SAM_WARN("JS", "sam_deal_damage: no entity with uid " + std::to_string(uid) + "."); return JS_FALSE; }
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
		const bool held = SAMLua::isKeyHeld(nameC ? nameC : "");
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
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

	// sam_get_player_uid(player) -> entity uid | null.
	JSValue js_sam_get_player_uid(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity ) { return JS_NULL; }
		return JS_NewInt64(ctx, (int64_t)players[player]->entity->getUID());
	}

	// sam_get_position(uid) -> [tileX, tileY] | null.
	JSValue js_sam_get_position(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; if ( samHasArg(argc, argv, 0) ) { JS_ToInt64(ctx, &uid, argv[0]); }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e ) { return JS_NULL; }
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
		int64_t uid = 0; int32_t tx = 0, ty = 0;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt64(ctx, &uid, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToInt32(ctx, &tx, argv[1]); }
		if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &ty, argv[2]); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_position refused: host only."); return JS_NewBool(ctx, 0); }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e ) { SAM_WARN("JS", "sam_set_position: no entity uid " + std::to_string(uid) + "."); return JS_NewBool(ctx, 0); }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("JS", "sam_set_position: tile out of bounds."); return JS_NewBool(ctx, 0); }
		if ( e->behavior == &actPlayer )
		{
			const bool ok = e->teleport(tx, ty);
			return JS_NewBool(ctx, ok ? 1 : 0);
		}
		e->x = (double)(tx * 16 + 8);
		e->y = (double)(ty * 16 + 8);
		e->flags[UPDATENEEDED] = true;
		e->flags[NOUPDATE] = false;
		TileEntityList.updateEntity(*e);
		return JS_NewBool(ctx, 1);
	}

	// sam_spawn_monster(tileX, tileY, "name" [, shopType]) -> uid | null. Host only.
	JSValue js_sam_spawn_monster(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t tx = 0, ty = 0; std::string monName;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &tx, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToInt32(ctx, &ty, argv[1]); }
		if ( samHasArg(argc, argv, 2) ) { const char* s = JS_ToCString(ctx, argv[2]); if ( s ) { monName = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_monster refused: host only."); return JS_NULL; }
		const int creature = samMonsterNameToId(monName.c_str());
		if ( creature <= 0 ) { SAM_ERROR("JS", "sam_spawn_monster: unknown monster '" + monName + "'."); return JS_NULL; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("JS", "sam_spawn_monster: tile out of bounds."); return JS_NULL; }
		Entity* e = summonMonster(static_cast<Monster>(creature), tx * 16 + 8, ty * 16 + 8);
		if ( !e ) { SAM_ERROR("JS", "sam_spawn_monster: spawn failed (blocked tile?)."); return JS_NULL; }
		if ( argc >= 4 && creature == SHOPKEEPER )
		{
			int32_t shopType = 0; JS_ToInt32(ctx, &shopType, argv[3]);
			if ( shopType < 0 ) { shopType = 0; }
			if ( shopType > 14 ) { shopType = 14; }
			if ( Stat* s = e->getStats() ) { s->MISC_FLAGS[STAT_FLAG_NPC] = 1 + shopType; }
		}
		SAM_INFO("JS", "Spawned monster " + monName + " at (" + std::to_string(tx) + "," + std::to_string(ty) + ")");
		return JS_NewInt64(ctx, (int64_t)e->getUID());
	}

	// sam_spawn_portal(tileX, tileY) -> uid | null. A purely-DECORATIVE, walkable portal
	// (swirling vortex, sprite 254): animates + glows but is never interactive and never
	// descends anyone (see the skill[15] guard in actPortal). Returns its uid so a script
	// can move it (sam_set_position) or clear it (sam_remove_entity). Host only. Twin of
	// the Lua binding — keep the two in lock-step.
	JSValue js_sam_spawn_portal(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t tx = 0, ty = 0;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &tx, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToInt32(ctx, &ty, argv[1]); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_portal refused: host only."); return JS_NULL; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("JS", "sam_spawn_portal: tile out of bounds."); return JS_NULL; }
		Entity* e = newEntity(254, 1, map.entities, nullptr);
		if ( !e ) { SAM_ERROR("JS", "sam_spawn_portal: entity creation failed."); return JS_NULL; }
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
		SAM_INFO("JS", "sam_spawn_portal: decorative portal at (" + std::to_string(tx) + "," + std::to_string(ty) + ")");
		return JS_NewInt64(ctx, (int64_t)e->getUID());
	}

	// sam_remove_entity(uid) -> bool. Remove a non-player world entity by uid (portal
	// marker, spawned monster, ground item...). Refuses players. Frees any light. Host only.
	JSValue js_sam_remove_entity(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_remove_entity refused: host only."); return JS_FALSE; }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e ) { return JS_FALSE; }
		if ( e->behavior == &actPlayer ) { SAM_WARN("JS", "sam_remove_entity refused: cannot remove a player."); return JS_FALSE; }
		// See the Lua binding: an open chest is also held in openedChest[], which nothing
		// here clears, so removing the entity left a dangling pointer for the still-open
		// chest UI. closeChest() is a no-op when the chest is not open.
		if ( e->behavior == &actChest ) { e->closeChest(); }
		e->removeLightField();
		if ( e->mynode ) { list_RemoveNode(e->mynode); }
		return JS_TRUE;
	}

	// sam_spawn_companion(player, model_id [, scale]) -> uid | null. Twin of the Lua binding:
	// spawns a floating companion that renders a registered custom .vox model and trails the
	// player, ready to thrust forward on sam_companion_punch. Remove with sam_remove_entity.
	JSValue js_sam_spawn_companion(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		const char* modelC = ( samHasArg(argc, argv, 1) ) ? JS_ToCString(ctx, argv[1]) : nullptr;
		double scale = 1.0;
		if ( samHasArg(argc, argv, 2) ) { JS_ToFloat64(ctx, &scale, argv[2]); }
		const unsigned long long uid = SAMLua::spawnCompanion(player, modelC ? modelC : "", scale);
		if ( modelC ) { JS_FreeCString(ctx, modelC); }
		if ( uid == 0 ) { return JS_NULL; }
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

	// sam_get_facing(player) -> yaw radians [0,2PI) | null. 0 = +x (east), increasing toward
	// +y; forward unit vector is (cos yaw, sin yaw). Host-authoritative for remote players.
	JSValue js_sam_get_facing(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		const double yaw = SAMLua::getFacing(player);
		if ( yaw < 0.0 ) { return JS_NULL; }
		return JS_NewFloat64(ctx, yaw);
	}

	// sam_screen_flash(player, r, g, b [, intensity=1.0] [, duration_ms=180]) -> bool.
	// JS twin of the Lua binding — the anime "impact frame" full-screen colour flash.
	JSValue js_sam_screen_flash(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1, r = 0, g = 0, b = 0, ms = 180;
		double inten = 1.0;
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToInt32(ctx, &r, argv[1]); }
		if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &g, argv[2]); }
		if ( samHasArg(argc, argv, 3) ) { JS_ToInt32(ctx, &b, argv[3]); }
		if ( samHasArg(argc, argv, 4) ) { JS_ToFloat64(ctx, &inten, argv[4]); }
		if ( samHasArg(argc, argv, 5) ) { JS_ToInt32(ctx, &ms, argv[5]); }
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
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToInt32(ctx, &r, argv[1]); }
		if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &g, argv[2]); }
		if ( samHasArg(argc, argv, 3) ) { JS_ToInt32(ctx, &b, argv[3]); }
		if ( samHasArg(argc, argv, 4) ) { JS_ToFloat64(ctx, &inten, argv[4]); }
		if ( samHasArg(argc, argv, 5) ) { JS_ToInt32(ctx, &ms, argv[5]); }
		if ( samHasArg(argc, argv, 6) ) { JS_ToInt32(ctx, &lines, argv[6]); }
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
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( samHasArg(argc, argv, 1) ) { JS_ToFloat64(ctx, &mag, argv[1]); }
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
		if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &ms, argv[0]); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer != SINGLE ) { return JS_FALSE; }
		SAMLua::triggerHitstop(ms);
		return JS_TRUE;
#else
		(void)ms;
		return JS_FALSE;
#endif
	}

	// sam_get_inventory(player) -> array of { uid, type, name, count, beatitude, status,
	// identified, equipped }. Empty array for an invalid player. Reader.
	JSValue js_sam_get_inventory(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		JSValue arr = JS_NewArray(ctx);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return arr; }
		uint32_t idx = 0;
		for ( node_t* node = stats[player]->inventory.first; node != nullptr; node = node->next )
		{
			Item* it = (Item*)node->element;
			if ( !it ) { continue; }
			JSValue o = JS_NewObject(ctx);
			JS_SetPropertyStr(ctx, o, "uid", JS_NewInt64(ctx, (int64_t)it->uid));
			JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, (int32_t)it->type));
			if ( (int)it->type >= 0 && (int)it->type < NUMITEMS ) { JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, itemNameStrings[(int)it->type + 2])); }
			else { JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, "custom")); }
			JS_SetPropertyStr(ctx, o, "count", JS_NewInt32(ctx, (int32_t)it->count));
			JS_SetPropertyStr(ctx, o, "beatitude", JS_NewInt32(ctx, (int32_t)it->beatitude));
			JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, (int32_t)it->status));
			JS_SetPropertyStr(ctx, o, "identified", JS_NewBool(ctx, it->identified ? 1 : 0));
			JS_SetPropertyStr(ctx, o, "equipped", JS_NewBool(ctx, (itemSlot(stats[player], it) != nullptr) ? 1 : 0));
			JS_SetPropertyUint32(ctx, arr, idx++, o);
		}
		return arr;
	}

	// sam_remove_item(itemUid) -> boolean. Refuses an equipped item. Host only.
	JSValue js_sam_remove_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int64_t uid = 0; if ( samHasArg(argc, argv, 0) ) { JS_ToInt64(ctx, &uid, argv[0]); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_remove_item refused: host only."); return JS_NewBool(ctx, 0); }
		Item* it = uidToItem((Uint32)uid);
		if ( !it ) { SAM_WARN("JS", "sam_remove_item: no item uid " + std::to_string(uid) + "."); return JS_NewBool(ctx, 0); }
		int owner = -1;
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( !stats[p] ) { continue; }
			if ( itemSlot(stats[p], it) != nullptr )
			{ SAM_WARN("JS", "sam_remove_item: item uid " + std::to_string(uid) + " is equipped; unequip first."); return JS_NewBool(ctx, 0); }
			for ( node_t* n = stats[p]->inventory.first; n; n = n->next ) { if ( (Item*)n->element == it ) { owner = p; break; } }
		}
		Item* ref = it;
		while ( ref ) { consumeItem(ref, owner >= 0 ? owner : 0); }
		SAM_INFO("JS", "Removed item uid " + std::to_string(uid) + ".");
		return JS_NewBool(ctx, 1);
	}
#endif

	// sam_get_monster_type(uid) -> "rat" / "skeleton" / ... or null. The creature's SPECIES,
	// as the lowercase name the engine uses in monstertypename[]. This is the BASE type: a
	// custom monster is a variant of a vanilla species, so a mod's "Rathalos" built on a bat
	// answers "bat". Lua parity: lua_sam_get_monster_type.
	JSValue js_sam_get_monster_type(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NULL; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_NULL; }
		const int t = (int)e->getStats()->type;
		if ( t < 0 || t >= NUMMONSTERS ) { return JS_NULL; }
		return JS_NewString(ctx, monstertypename[t]);
#else
		(void)uid; return JS_NULL;
#endif
	}

	// sam_get_monster_name(uid) -> the creature's DISPLAY name, or null. For a custom monster
	// this is its variant name ("Rathalos"); vanilla creatures carry an empty variant name, so
	// fall back to the species. Lua parity: lua_sam_get_monster_name.
	JSValue js_sam_get_monster_name(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NULL; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_NULL; }
		Stat* st = e->getStats();
		if ( st->name[0] ) { return JS_NewString(ctx, st->name); }
		const int t = (int)st->type;
		if ( t < 0 || t >= NUMMONSTERS ) { return JS_NULL; }
		return JS_NewString(ctx, monstertypename[t]);
#else
		(void)uid; return JS_NULL;
#endif
	}

	// ---- monster movement (Lua parity: lua_sam_monster_path_to / _face / _attack) --------
	// Tile coordinates throughout, matching sam_get_position.

	JSValue js_sam_monster_path_to(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		int32_t tx = 0, ty = 0; JS_ToInt32(ctx, &tx, argv[1]); JS_ToInt32(ctx, &ty, argv[2]);
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
		if ( argc < 3 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		int32_t tx = 0, ty = 0; JS_ToInt32(ctx, &tx, argv[1]); JS_ToInt32(ctx, &ty, argv[2]);
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
		if ( argc < 1 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
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
		if ( samHasArg(argc, argv, 1) ) { JS_ToInt32(ctx, &ticks, argv[1]); }
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
	// told no. Send several small messages, or use sam_save_data for bulk state.

	// sam_send_packet(target, tag, payload) -> boolean. Lua parity: lua_sam_send_packet.
	JSValue js_sam_send_packet(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int32_t target = -1; JS_ToInt32(ctx, &target, argv[0]);
		const char* tagC = JS_ToCString(ctx, argv[1]);
		const char* payC = ( samHasArg(argc, argv, 2) ) ? JS_ToCString(ctx, argv[2]) : nullptr;
		const std::string tag = tagC ? tagC : "";
		const std::string pay = payC ? payC : "";
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
		if ( argc < 4 ) { return JS_FALSE; }
		const char* id = JS_ToCString(ctx, argv[0]);
		int32_t x = 0, y = 0; JS_ToInt32(ctx, &x, argv[1]); JS_ToInt32(ctx, &y, argv[2]);
		const char* val = JS_ToCString(ctx, argv[3]);
		const Uint32 col = samHudColorJS(ctx, argc, argv, 4, makeColor(255, 255, 255, 255));
		const bool ok = SAMHud::text(g_currentNs, id ? id : "", x, y, val ? val : "", col);
		if ( id ) { JS_FreeCString(ctx, id); }
		if ( val ) { JS_FreeCString(ctx, val); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_hud_bar(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 6 ) { return JS_FALSE; }
		const char* id = JS_ToCString(ctx, argv[0]);
		int32_t x = 0, y = 0, w = 0, h = 0;
		JS_ToInt32(ctx, &x, argv[1]); JS_ToInt32(ctx, &y, argv[2]);
		JS_ToInt32(ctx, &w, argv[3]); JS_ToInt32(ctx, &h, argv[4]);
		double frac = 0.0; JS_ToFloat64(ctx, &frac, argv[5]);
		const Uint32 col = samHudColorJS(ctx, argc, argv, 6, makeColor(200, 40, 40, 255));
		const bool ok = SAMHud::bar(g_currentNs, id ? id : "", x, y, w, h, frac, col);
		if ( id ) { JS_FreeCString(ctx, id); }
		return ok ? JS_TRUE : JS_FALSE;
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
		if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &ms, argv[2]); }
		if ( samHasArg(argc, argv, 3) ) { JS_ToInt32(ctx, &alpha, argv[3]); }
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
		if ( samHasArg(argc, argv, 6) ) { JS_ToInt32(ctx, &ms, argv[6]); }
		if ( samHasArg(argc, argv, 7) ) { JS_ToInt32(ctx, &alpha, argv[7]); }
		const bool ok = SAMImages::show(player, g_currentNs, img ? img : "",
			ms, alpha, SAMImages::FIT_RECT, x, y, w, h);
		if ( img ) { JS_FreeCString(ctx, img); }
		return ok ? JS_TRUE : JS_FALSE;
	}

	// sam_hide_image([player]) -> boolean. No player clears every player's overlay.
	JSValue js_sam_hide_image(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 )
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
		if ( argc < 1 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const bool ok = SAMUi::isOpen(g_currentNs, panel ? panel : "");
		if ( panel ) { JS_FreeCString(ctx, panel); }
		return ok ? JS_TRUE : JS_FALSE;
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
		if ( argc < 2 ) { return JS_NewString(ctx, ""); }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const char* id = JS_ToCString(ctx, argv[1]);
		const std::string t = SAMUi::inputText(g_currentNs, panel ? panel : "", id ? id : "");
		if ( panel ) { JS_FreeCString(ctx, panel); }
		if ( id ) { JS_FreeCString(ctx, id); }
		return JS_NewString(ctx, t.c_str());
	}

	JSValue js_sam_ui_panel_style(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_FALSE; }
		const char* panel = JS_ToCString(ctx, argv[0]);
		const Uint32 bg = samHudColorJS(ctx, argc, argv, 1, 0);
		const Uint32 border = samHudColorJS(ctx, argc, argv, 2, 0);
		int32_t bw = -1; if ( samHasArg(argc, argv, 3) ) { JS_ToInt32(ctx, &bw, argv[3]); }
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

	// [width, height] or null -- array form, matching sam_get_image_size.
	JSValue js_sam_ui_text_size(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NULL; }
		const char* text = JS_ToCString(ctx, argv[0]);
		const char* f = samHasArg(argc, argv, 1) ? JS_ToCString(ctx, argv[1]) : nullptr;
		int w = 0, h = 0;
		const bool ok = SAMUi::textSize(text ? text : "", f ? f : "", w, h);
		if ( text ) { JS_FreeCString(ctx, text); }
		if ( f ) { JS_FreeCString(ctx, f); }
		if ( !ok ) { return JS_NULL; }
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
		if ( argc < 1 ) { return JS_NULL; }
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
		if ( !SAMCatalog::itemInfo(type, e, attrs) ) { return JS_NULL; }

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
		if ( argc < 4 ) { return JS_NULL; }
		double x = 0, y = 0, angle = 0, speed = 0;
		JS_ToFloat64(ctx, &x, argv[0]); JS_ToFloat64(ctx, &y, argv[1]);
		JS_ToFloat64(ctx, &angle, argv[2]); JS_ToFloat64(ctx, &speed, argv[3]);
		int32_t dmg = 0, life = 100, owner = -1;
		if ( samHasArg(argc, argv, 4) ) { JS_ToInt32(ctx, &dmg, argv[4]); }
		if ( samHasArg(argc, argv, 5) ) { JS_ToInt32(ctx, &life, argv[5]); }
		const char* model = samHasArg(argc, argv, 6) ? JS_ToCString(ctx, argv[6]) : nullptr;
		if ( samHasArg(argc, argv, 7) ) { JS_ToInt32(ctx, &owner, argv[7]); }
		const unsigned long long uid = SAMLua::spawnProjectile(owner, x, y, angle, speed,
			dmg, life, model ? model : "");
		if ( model ) { JS_FreeCString(ctx, model); }
		if ( uid == 0 ) { return JS_NULL; }
		return JS_NewInt64(ctx, (int64_t)uid);
	}

	// sam_get_image_size(image) -> [width, height], or null. Array rather than two returns:
	// the JS side mirrors every multi-value Lua function this way (see sam_get_position).
	JSValue js_sam_get_image_size(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NULL; }
		const char* img = JS_ToCString(ctx, argv[0]);
		int w = 0, h = 0;
		const bool ok = SAMImages::size(g_currentNs, img ? img : "", w, h);
		if ( img ) { JS_FreeCString(ctx, img); }
		if ( !ok ) { return JS_NULL; }
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
		if ( argc < 2 ) { return JS_NULL; }
		int32_t x = 0, y = 0; JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
		const SAMWorld::TileInfo t = SAMWorld::tile(x, y);
		if ( !t.valid ) { return JS_NULL; }
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
		int32_t pl = -1; if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &pl, argv[2]); }
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
		if ( argc < 1 ) { return JS_NULL; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		std::vector<SAMWorld::ItemInfo> found;
		if ( !SAMWorld::containerItems((uint32_t)uid, found) ) { return JS_NULL; }
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
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		return SAMWorld::setDoor((uint32_t)uid, JS_ToBool(ctx, argv[1]) > 0) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_set_door_locked(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		return SAMWorld::setDoorLocked((uint32_t)uid, JS_ToBool(ctx, argv[1]) > 0) ? JS_TRUE : JS_FALSE;
	}

	JSValue js_sam_power_entity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		return SAMWorld::powerEntity((uint32_t)uid, JS_ToBool(ctx, argv[1]) > 0) ? JS_TRUE : JS_FALSE;
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
		if ( argc < 2 ) { return JS_NULL; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
		const std::string n = samUpper(nameC ? nameC : "");
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e || !e->getStats() ) { return JS_NULL; }
		Stat* st = e->getStats();
		long long v = 0;
		if      ( n == "STR" ) { v = statGetSTR(st, e); }
		else if ( n == "DEX" ) { v = statGetDEX(st, e); }
		else if ( n == "CON" ) { v = statGetCON(st, e); }
		else if ( n == "INT" ) { v = statGetINT(st, e); }
		else if ( n == "PER" ) { v = statGetPER(st, e); }
		else if ( n == "CHR" ) { v = statGetCHR(st, e); }
		else { SAM_WARN("JS", "sam_get_effective_stat: unknown stat '" + n + "'."); return JS_NULL; }
		return JS_NewInt64(ctx, v);
#else
		return JS_NULL;
#endif
	}

	JSValue js_sam_get_ac(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 1 ) { return JS_NULL; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e || !e->getStats() ) { return JS_NULL; }
		return JS_NewInt32(ctx, AC(e->getStats()));
#else
		return JS_NULL;
#endif
	}

	JSValue js_sam_get_skill(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_NULL; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
		const int skill = samSkillFromNameJS(nameC);
		if ( nameC ) { JS_FreeCString(ctx, nameC); }
		const bool eff = ( samHasArg(argc, argv, 2) ) ? (JS_ToBool(ctx, argv[2]) > 0) : true;
#ifdef SAM_JS_HAVE_BARONY
		if ( skill < 0 ) { SAM_WARN("JS", "sam_get_skill: unknown skill."); return JS_NULL; }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e || !e->getStats() ) { return JS_NULL; }
		Stat* st = e->getStats();
		return JS_NewInt32(ctx, eff ? st->getModifiedProficiency(skill) : st->getProficiency(skill));
#else
		(void)eff; return JS_NULL;
#endif
	}

	static JSValue samFactionCheckJS(JSContext* ctx, int argc, JSValueConst* argv, bool wantEnemy)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t a = 0, b = 0; JS_ToInt64(ctx, &a, argv[0]); JS_ToInt64(ctx, &b, argv[1]);
#ifdef SAM_JS_HAVE_BARONY
		Entity* ea = uidToEntity((Sint32)a);
		Entity* eb = uidToEntity((Sint32)b);
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
			const int id = SAMSounds::soundIndexForId(nm ? nm : "");
			if ( nm ) { JS_FreeCString(ctx, nm); }
			return id;
		}
		int32_t n = -1; JS_ToInt32(ctx, &n, v); return n;
	}

	// ---- world-space presentation (Lua parity) --------------------------------------------

	JSValue js_sam_play_sound_at(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_FALSE; }
		const int snd = samResolveSoundIdJS(ctx, argv[0]);
		double tx = 0, ty = 0; JS_ToFloat64(ctx, &tx, argv[1]); JS_ToFloat64(ctx, &ty, argv[2]);
		int32_t vol = 128; if ( samHasArg(argc, argv, 3) ) { JS_ToInt32(ctx, &vol, argv[3]); }
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
		if ( argc < 2 ) { return JS_FALSE; }
		const int snd = samResolveSoundIdJS(ctx, argv[0]);
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[1]);
		int32_t vol = 128; if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &vol, argv[2]); }
#ifdef SAM_JS_HAVE_BARONY
		if ( snd < 0 || snd >= (int)numsounds ) { return JS_FALSE; }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e ) { return JS_FALSE; }
		if ( vol < 0 ) { vol = 0; } if ( vol > 255 ) { vol = 255; }
		playSoundEntity(e, (Uint16)snd, (Uint8)vol);
		return JS_TRUE;
#else
		return JS_FALSE;
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
		if ( samHasArg(argc, argv, 3) ) { JS_ToFloat64(ctx, &z, argv[3]); }
		if ( samHasArg(argc, argv, 4) ) { JS_ToFloat64(ctx, &scale, argv[4]); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_particle refused: host only."); return JS_FALSE; }
		const Sint16 px = (Sint16)(tx * 16.0 + 8.0), py = (Sint16)(ty * 16.0 + 8.0), pz = (Sint16)z;
		Entity* made = nullptr;
		if      ( k == "poof" )      { made = spawnPoof(px, py, pz, scale, true); }
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
		int32_t gibType = 0; if ( samHasArg(argc, argv, 2) ) { JS_ToInt32(ctx, &gibType, argv[2]); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_damage_number refused: host only."); return JS_FALSE; }
		Entity* e = uidToEntity((Sint32)uid);
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
		if ( argc < 2 ) { return JS_NewInt32(ctx, 0); }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
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
		if ( argc < 2 ) { return JS_NewBool(ctx, 0); }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
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

	// sam_get_item_category(item) -> category name string, or undefined. `item` is an int id
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
			if ( type < 0 )
			{
				std::string lower = name;
				for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
				auto it = ItemTooltips.itemNameStringToItemID.find(lower);
				if ( it != ItemTooltips.itemNameStringToItemID.end() ) { type = it->second; }
			}
		}
		if ( type < 0 || type >= NUM_ITEM_SLOTS ) { return JS_UNDEFINED; }
		const std::string cat = SAMItems::categoryName((int)items[type].category);
		if ( cat.empty() ) { return JS_UNDEFINED; }
		return JS_NewString(ctx, cat.c_str());
#else
		return JS_UNDEFINED;
#endif
	}

	JSValue js_sam_set_monster_stat(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 3 ) { return JS_FALSE; }
		int64_t uid = 0;    JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
		int32_t value = 0;  JS_ToInt32(ctx, &value, argv[2]);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_monster_stat refused: host only."); if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { SAM_WARN("JS", "sam_set_monster_stat: no monster uid " + std::to_string(uid)); if ( nameC ) { JS_FreeCString(ctx, nameC); } return JS_FALSE; }
		Stat* s = e->getStats();
		const std::string n = samUpper(nameC);
		bool ok = true;
		if      ( n == "HP" )    { e->setHP(value); }
		else if ( n == "MAXHP" ) { s->MAXHP = (value < 1 ? 1 : value); if ( s->HP > s->MAXHP ) { s->HP = s->MAXHP; } }
		else if ( n == "MP" )    { e->setMP(value); }
		else if ( n == "MAXMP" ) { s->MAXMP = (value < 0 ? 0 : value); if ( s->MP > s->MAXMP ) { s->MP = s->MAXMP; } }
		else if ( n == "STR" )   { s->STR = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "DEX" || n == "SPEED" ) { s->DEX = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "CON" )   { s->CON = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "INT" )   { s->INT = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "PER" )   { s->PER = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "CHR" )   { s->CHR = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "LEVEL" || n == "LVL" ) { s->LVL = samClampInt(value, 1, 255); }
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
		if ( argc < 3 ) { return JS_FALSE; }
		int64_t uid = 0;    JS_ToInt64(ctx, &uid, argv[0]);
		const char* nameC = JS_ToCString(ctx, argv[1]);
		int32_t ticks = 0;  JS_ToInt32(ctx, &ticks, argv[2]);
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
		int64_t uid = 0; if ( samHasArg(argc, argv, 0) ) { JS_ToInt64(ctx, &uid, argv[0]); }
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
		if ( argc < 3 ) { return JS_NewInt32(ctx, 0); }
		int64_t nearUid = 0; JS_ToInt64(ctx, &nearUid, argv[0]);
		const char* typeC = JS_ToCString(ctx, argv[1]);
		int32_t count = 0;   JS_ToInt32(ctx, &count, argv[2]);
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_monsters refused: host only."); if ( typeC ) { JS_FreeCString(ctx, typeC); } return JS_NewInt32(ctx, 0); }
		Entity* anchor = uidToEntity((Sint32)nearUid);
		if ( !anchor ) { SAM_WARN("JS", "sam_spawn_monsters: no anchor entity uid " + std::to_string(nearUid)); if ( typeC ) { JS_FreeCString(ctx, typeC); } return JS_NewInt32(ctx, 0); }
		const int mtype = samMonsterNameToId(typeC);
		if ( mtype < 0 ) { SAM_WARN("JS", std::string("sam_spawn_monsters: unknown monster type '") + (typeC ? typeC : "") + "'"); if ( typeC ) { JS_FreeCString(ctx, typeC); } return JS_NewInt32(ctx, 0); }
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
		if ( argc < 1 ) { return JS_NewInt32(ctx, -1); }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
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
		if ( argc < 2 ) { return JS_FALSE; }
		int64_t uid = 0;    JS_ToInt64(ctx, &uid, argv[0]);
		int32_t player = 0; JS_ToInt32(ctx, &player, argv[1]);
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
		JSValue jstr = JS_JSONStringify(ctx, argv[2], JS_UNDEFINED, JS_UNDEFINED);
		std::string json = "null";
		if ( !JS_IsException(jstr) && !JS_IsUndefined(jstr) )
		{
			const char* s = JS_ToCString(ctx, jstr);
			if ( s ) { json = s; JS_FreeCString(ctx, s); }
		}
		JS_FreeValue(ctx, jstr);
		SAMLua::monsterDataSet((unsigned)(Sint32)uid, key, json);
		return JS_TRUE;
	}

	// sam_get_player_data(player, key) / sam_set_player_data(player, key, value) — per-player,
	// in-memory, per-session scratch (cooldowns/flags/stacks). Shares SAMLua's store with Lua.
	JSValue js_sam_get_player_data(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_UNDEFINED; }
		int32_t player = -1; JS_ToInt32(ctx, &player, argv[0]);
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
		if ( argc < 3 ) { return JS_FALSE; }
		int32_t player = -1; JS_ToInt32(ctx, &player, argv[0]);
		const char* keyC = JS_ToCString(ctx, argv[1]);
		const std::string key = keyC ? keyC : "";
		if ( keyC ) { JS_FreeCString(ctx, keyC); }
		if ( player < 0 || player >= MAXPLAYERS ) { return JS_FALSE; }
		JSValue jstr = JS_JSONStringify(ctx, argv[2], JS_UNDEFINED, JS_UNDEFINED);
		std::string json = "null";
		if ( !JS_IsException(jstr) && !JS_IsUndefined(jstr) )
		{
			const char* s = JS_ToCString(ctx, jstr);
			if ( s ) { json = s; JS_FreeCString(ctx, s); }
		}
		JS_FreeValue(ctx, jstr);
		SAMLua::playerDataSet(player, key, json);
		return JS_TRUE;
	}

	// sam_get_effect_duration(player, "EFFECT") -> remaining ticks (0 if inactive, -1 permanent).
	JSValue js_sam_get_effect_duration(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
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
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
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
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
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
	bool samJsGetIntProp(JSContext* ctx, JSValueConst obj, const char* key, int& out)
	{
		JSValue v = JS_GetPropertyStr(ctx, obj, key);
		bool ok = false;
		if ( JS_IsNumber(v) ) { int32_t n = 0; JS_ToInt32(ctx, &n, v); out = n; ok = true; }
		JS_FreeValue(ctx, v);
		return ok;
	}
	bool samJsGetStrProp(JSContext* ctx, JSValueConst obj, const char* key, std::string& out)
	{
		JSValue v = JS_GetPropertyStr(ctx, obj, key);
		bool ok = false;
		if ( JS_IsString(v) ) { const char* s = JS_ToCString(ctx, v); if ( s ) { out = s; JS_FreeCString(ctx, s); ok = true; } }
		JS_FreeValue(ctx, v);
		return ok;
	}
#endif

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
			JSValue attrs = JS_GetPropertyStr(ctx, argv[1], "attributes");
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

	// ---- custom spells (Session 1): grant a spell to a player -------------------
	JSValue js_sam_grant_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int32_t player = 0; JS_ToInt32(ctx, &player, argv[0]);
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
		if ( spell.find(':') != std::string::npos )
		{
			// Custom spell — the engine spell_t is built at load, so grant it for real.
			return SAMSpells::grantCustomSpell(player, spell) ? JS_TRUE : JS_FALSE;
		}
		std::string lower = spell;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		int id = -1;
		for ( const auto& kv : ItemTooltips.spellItems )
		{
			if ( kv.second.internalName == lower ) { id = kv.first; break; }
		}
		if ( id < 0 ) { SAM_ERROR("JS", "sam_grant_spell: unknown spell '" + spell + "' (expected a SPELL_ name or \"namespace:spell\")."); return JS_FALSE; }
		const bool ok = addSpell(id, player, true);
		SAM_INFO("SAM", "sam_grant_spell: " + std::string(ok ? "granted" : "not granted (already known or non-local)")
			+ " vanilla spell '" + spell + "' (id " + std::to_string(id) + ") to player " + std::to_string(player) + ".");
		return ok ? JS_TRUE : JS_FALSE;
#else
		(void)player;
		return JS_FALSE;
#endif
	}

	// sam_cast_spell(player, spell) — mirror of the Lua binding: fire a spell/bolt from a
	// player in their facing direction (host-only, trap=true so it's free + never blocked
	// by the defend guard). Returns true if a projectile spawned.
	JSValue js_sam_cast_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int32_t player = 0; JS_ToInt32(ctx, &player, argv[0]);
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
		int id = -1;
		if ( spell.find(':') != std::string::npos )
		{
			const SAMSpellDef* d = SAMSpells::getSpellByName(spell);
			if ( d ) { id = d->numericId; }
		}
		else
		{
			std::string lower = spell;
			for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
			for ( const auto& kv : ItemTooltips.spellItems ) { if ( kv.second.internalName == lower ) { id = kv.first; break; } }
		}
		if ( id < 0 ) { SAM_ERROR("JS", "sam_cast_spell: unknown spell '" + spell + "' (SPELL_ name or \"namespace:spell\")."); return JS_FALSE; }
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
		if ( argc < 3 ) { return JS_NULL; }
		int32_t player = 0; JS_ToInt32(ctx, &player, argv[0]);
		int64_t targetUid = 0; JS_ToInt64(ctx, &targetUid, argv[1]);
		const char* spellC = JS_ToCString(ctx, argv[2]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_cast_spell_at refused: host only."); return JS_NULL; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity ) { return JS_NULL; }
		Entity* target = uidToEntity((Sint32)targetUid);
		if ( !target ) { return JS_NULL; }
		const int id = samJsResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_cast_spell_at: unknown spell '" + spell + "'."); return JS_NULL; }
		Entity* missile = samJsCastAimed(players[player]->entity, id, true, target->x, target->y);
		return missile ? JS_NewInt64(ctx, (int64_t)missile->getUID()) : JS_NULL;
#else
		(void)player; (void)targetUid; (void)spell; return JS_NULL;
#endif
	}

	JSValue js_sam_cast_spell_pos(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 4 ) { return JS_NULL; }
		int32_t player = 0, tx = 0, ty = 0;
		JS_ToInt32(ctx, &player, argv[0]); JS_ToInt32(ctx, &tx, argv[1]); JS_ToInt32(ctx, &ty, argv[2]);
		const char* spellC = JS_ToCString(ctx, argv[3]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_cast_spell_pos refused: host only."); return JS_NULL; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity ) { return JS_NULL; }
		const int id = samJsResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_cast_spell_pos: unknown spell '" + spell + "'."); return JS_NULL; }
		Entity* missile = samJsCastAimed(players[player]->entity, id, true, (double)(tx * 16 + 8), (double)(ty * 16 + 8));
		return missile ? JS_NewInt64(ctx, (int64_t)missile->getUID()) : JS_NULL;
#else
		(void)player; (void)tx; (void)ty; (void)spell; return JS_NULL;
#endif
	}

	JSValue js_sam_monster_cast_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_NULL; }
		int64_t uid = 0; JS_ToInt64(ctx, &uid, argv[0]);
		const char* spellC = JS_ToCString(ctx, argv[1]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_monster_cast_spell refused: host only."); return JS_NULL; }
		Entity* e = samResolveMonster(uid);
		if ( !e ) { return JS_NULL; }
		const int id = samJsResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("JS", "sam_monster_cast_spell: unknown spell '" + spell + "'."); return JS_NULL; }
		Entity* missile = samJsCastAimed(e, id, false, 0, 0);
		return missile ? JS_NewInt64(ctx, (int64_t)missile->getUID()) : JS_NULL;
#else
		(void)uid; (void)spell; return JS_NULL;
#endif
	}

	JSValue js_sam_get_spells(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		int32_t player = -1; if ( samHasArg(argc, argv, 0) ) { JS_ToInt32(ctx, &player, argv[0]); }
		JSValue arr = JS_NewArray(ctx);
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return arr; }
		uint32_t n = 0;
		for ( node_t* node = players[player]->magic.spellList.first; node; node = node->next )
		{
			spell_t* sp = (spell_t*)node->element;
			if ( sp ) { JS_SetPropertyUint32(ctx, arr, n++, JS_NewString(ctx, sp->spell_internal_name)); }
		}
		return arr;
#else
		(void)player; return arr;
#endif
	}

	JSValue js_sam_player_knows_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int32_t player = -1; JS_ToInt32(ctx, &player, argv[0]);
		const char* spellC = JS_ToCString(ctx, argv[1]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_FALSE; }
		const int id = samJsResolveSpellId(spell);
		if ( id < 0 ) { return JS_FALSE; }
		for ( node_t* node = players[player]->magic.spellList.first; node; node = node->next )
		{
			spell_t* sp = (spell_t*)node->element;
			if ( sp && sp->ID == id ) { return JS_TRUE; }
		}
		return JS_FALSE;
#else
		(void)player; (void)spell; return JS_FALSE;
#endif
	}

	JSValue js_sam_remove_spell(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		SAMLogger::noteApiCall();
		if ( argc < 2 ) { return JS_FALSE; }
		int32_t player = -1; JS_ToInt32(ctx, &player, argv[0]);
		const char* spellC = JS_ToCString(ctx, argv[1]);
		const std::string spell = spellC ? spellC : "";
		if ( spellC ) { JS_FreeCString(ctx, spellC); }
#ifdef SAM_JS_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_remove_spell refused: host only."); return JS_FALSE; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return JS_FALSE; }
		const int id = samJsResolveSpellId(spell);
		if ( id < 0 ) { return JS_FALSE; }
		for ( node_t* node = players[player]->magic.spellList.first; node; )
		{
			node_t* next = node->next;
			spell_t* sp = (spell_t*)node->element;
			if ( sp && sp->ID == id ) { list_RemoveNode(node); return JS_TRUE; }
			node = next;
		}
		return JS_FALSE;
#else
		(void)player; (void)spell; return JS_FALSE;
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
		JS_SetPropertyStr(ctx, g, "sam_log", JS_NewCFunction(ctx, js_sam_log, "sam_log", 1));
		JS_SetPropertyStr(ctx, g, "sam_grant_item", JS_NewCFunction(ctx, js_sam_grant_item, "sam_grant_item", 2));
		JS_SetPropertyStr(ctx, g, "sam_save_data", JS_NewCFunction(ctx, js_sam_save_data, "sam_save_data", 2));
		JS_SetPropertyStr(ctx, g, "sam_load_data", JS_NewCFunction(ctx, js_sam_load_data, "sam_load_data", 1));
		JS_SetPropertyStr(ctx, g, "sam_delete_data", JS_NewCFunction(ctx, js_sam_delete_data, "sam_delete_data", 1));
		JS_SetPropertyStr(ctx, g, "sam_set_timer", JS_NewCFunction(ctx, js_sam_set_timer, "sam_set_timer", 3));
		JS_SetPropertyStr(ctx, g, "sam_set_repeating_timer", JS_NewCFunction(ctx, js_sam_set_repeating_timer, "sam_set_repeating_timer", 3));
		JS_SetPropertyStr(ctx, g, "sam_cancel_timer", JS_NewCFunction(ctx, js_sam_cancel_timer, "sam_cancel_timer", 1));
		JS_SetPropertyStr(ctx, g, "sam_register_hook", JS_NewCFunction(ctx, js_sam_register_hook, "sam_register_hook", 2));
		JS_SetPropertyStr(ctx, g, "sam_fire_hook", JS_NewCFunction(ctx, js_sam_fire_hook, "sam_fire_hook", 2));
		JS_SetPropertyStr(ctx, g, "sam_modify_damage", JS_NewCFunction(ctx, js_sam_modify_damage, "sam_modify_damage", 2));
		JS_SetPropertyStr(ctx, g, "sam_modify_monster_damage", JS_NewCFunction(ctx, js_sam_modify_monster_damage, "sam_modify_monster_damage", 1));
		JS_SetPropertyStr(ctx, g, "sam_modify_value", JS_NewCFunction(ctx, js_sam_modify_value, "sam_modify_value", 1));
		JS_SetPropertyStr(ctx, g, "sam_deal_damage", JS_NewCFunction(ctx, js_sam_deal_damage, "sam_deal_damage", 2));
		JS_SetPropertyStr(ctx, g, "sam_is_key_held", JS_NewCFunction(ctx, js_sam_is_key_held, "sam_is_key_held", 1));
		// v0.7.0 Feature 4: monster / NPC scripting (UID-based, host-authoritative)
		JS_SetPropertyStr(ctx, g, "sam_get_player_data", JS_NewCFunction(ctx, js_sam_get_player_data, "sam_get_player_data", 2));
		JS_SetPropertyStr(ctx, g, "sam_set_player_data", JS_NewCFunction(ctx, js_sam_set_player_data, "sam_set_player_data", 3));
		JS_SetPropertyStr(ctx, g, "sam_get_effect_duration", JS_NewCFunction(ctx, js_sam_get_effect_duration, "sam_get_effect_duration", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_effect_strength", JS_NewCFunction(ctx, js_sam_get_effect_strength, "sam_get_effect_strength", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_effects", JS_NewCFunction(ctx, js_sam_get_effects, "sam_get_effects", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_monster_stat", JS_NewCFunction(ctx, js_sam_get_monster_stat, "sam_get_monster_stat", 2));
		JS_SetPropertyStr(ctx, g, "sam_is_host", JS_NewCFunction(ctx, js_sam_is_host, "sam_is_host", 0));
		JS_SetPropertyStr(ctx, g, "sam_play_sound_at", JS_NewCFunction(ctx, js_sam_play_sound_at, "sam_play_sound_at", 4));
		JS_SetPropertyStr(ctx, g, "sam_play_sound_entity", JS_NewCFunction(ctx, js_sam_play_sound_entity, "sam_play_sound_entity", 3));
		JS_SetPropertyStr(ctx, g, "sam_spawn_particle", JS_NewCFunction(ctx, js_sam_spawn_particle, "sam_spawn_particle", 5));
		JS_SetPropertyStr(ctx, g, "sam_damage_number", JS_NewCFunction(ctx, js_sam_damage_number, "sam_damage_number", 3));
		JS_SetPropertyStr(ctx, g, "sam_get_tile", JS_NewCFunction(ctx, js_sam_get_tile, "sam_get_tile", 2));
		JS_SetPropertyStr(ctx, g, "sam_set_tile", JS_NewCFunction(ctx, js_sam_set_tile, "sam_set_tile", 4));
		JS_SetPropertyStr(ctx, g, "sam_is_spawnable", JS_NewCFunction(ctx, js_sam_is_spawnable, "sam_is_spawnable", 2));
		JS_SetPropertyStr(ctx, g, "sam_line_of_sight", JS_NewCFunction(ctx, js_sam_line_of_sight, "sam_line_of_sight", 5));
		JS_SetPropertyStr(ctx, g, "sam_tiles_connected", JS_NewCFunction(ctx, js_sam_tiles_connected, "sam_tiles_connected", 5));
		JS_SetPropertyStr(ctx, g, "sam_get_light_at", JS_NewCFunction(ctx, js_sam_get_light_at, "sam_get_light_at", 2));
		JS_SetPropertyStr(ctx, g, "sam_find_entities", JS_NewCFunction(ctx, js_sam_find_entities, "sam_find_entities", 4));
		JS_SetPropertyStr(ctx, g, "sam_get_container_items", JS_NewCFunction(ctx, js_sam_get_container_items, "sam_get_container_items", 1));
		JS_SetPropertyStr(ctx, g, "sam_set_door", JS_NewCFunction(ctx, js_sam_set_door, "sam_set_door", 2));
		JS_SetPropertyStr(ctx, g, "sam_set_door_locked", JS_NewCFunction(ctx, js_sam_set_door_locked, "sam_set_door_locked", 2));
		JS_SetPropertyStr(ctx, g, "sam_power_entity", JS_NewCFunction(ctx, js_sam_power_entity, "sam_power_entity", 2));
		JS_SetPropertyStr(ctx, g, "sam_toggle_switch", JS_NewCFunction(ctx, js_sam_toggle_switch, "sam_toggle_switch", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_level_info", JS_NewCFunction(ctx, js_sam_get_level_info, "sam_get_level_info", 0));
		JS_SetPropertyStr(ctx, g, "sam_get_effective_stat", JS_NewCFunction(ctx, js_sam_get_effective_stat, "sam_get_effective_stat", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_ac", JS_NewCFunction(ctx, js_sam_get_ac, "sam_get_ac", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_skill", JS_NewCFunction(ctx, js_sam_get_skill, "sam_get_skill", 3));
		JS_SetPropertyStr(ctx, g, "sam_is_enemy", JS_NewCFunction(ctx, js_sam_is_enemy, "sam_is_enemy", 2));
		JS_SetPropertyStr(ctx, g, "sam_is_friend", JS_NewCFunction(ctx, js_sam_is_friend, "sam_is_friend", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_mods", JS_NewCFunction(ctx, js_sam_get_mods, "sam_get_mods", 0));
		JS_SetPropertyStr(ctx, g, "sam_is_mod_loaded", JS_NewCFunction(ctx, js_sam_is_mod_loaded, "sam_is_mod_loaded", 1));
		JS_SetPropertyStr(ctx, g, "sam_hud_text", JS_NewCFunction(ctx, js_sam_hud_text, "sam_hud_text", 5));
		JS_SetPropertyStr(ctx, g, "sam_hud_bar", JS_NewCFunction(ctx, js_sam_hud_bar, "sam_hud_bar", 7));
		JS_SetPropertyStr(ctx, g, "sam_hud_clear", JS_NewCFunction(ctx, js_sam_hud_clear, "sam_hud_clear", 1));
		// v1.10.3 -- the mod's own pictures (overlay + HUD art).
		JS_SetPropertyStr(ctx, g, "sam_show_image", JS_NewCFunction(ctx, js_sam_show_image, "sam_show_image", 5));
		JS_SetPropertyStr(ctx, g, "sam_show_image_at", JS_NewCFunction(ctx, js_sam_show_image_at, "sam_show_image_at", 8));
		JS_SetPropertyStr(ctx, g, "sam_hide_image", JS_NewCFunction(ctx, js_sam_hide_image, "sam_hide_image", 1));
		JS_SetPropertyStr(ctx, g, "sam_hud_image", JS_NewCFunction(ctx, js_sam_hud_image, "sam_hud_image", 7));
		JS_SetPropertyStr(ctx, g, "sam_get_image_size", JS_NewCFunction(ctx, js_sam_get_image_size, "sam_get_image_size", 1));
		// v1.11.0 -- interactive panels.
		JS_SetPropertyStr(ctx, g, "sam_ui_open", JS_NewCFunction(ctx, js_sam_ui_open, "sam_ui_open", 7));
		JS_SetPropertyStr(ctx, g, "sam_ui_close", JS_NewCFunction(ctx, js_sam_ui_close, "sam_ui_close", 1));
		JS_SetPropertyStr(ctx, g, "sam_ui_is_open", JS_NewCFunction(ctx, js_sam_ui_is_open, "sam_ui_is_open", 1));
		JS_SetPropertyStr(ctx, g, "sam_ui_clear", JS_NewCFunction(ctx, js_sam_ui_clear, "sam_ui_clear", 1));
		JS_SetPropertyStr(ctx, g, "sam_ui_label", JS_NewCFunction(ctx, js_sam_ui_label, "sam_ui_label", 7));
		JS_SetPropertyStr(ctx, g, "sam_ui_button", JS_NewCFunction(ctx, js_sam_ui_button, "sam_ui_button", 7));
		JS_SetPropertyStr(ctx, g, "sam_ui_image", JS_NewCFunction(ctx, js_sam_ui_image, "sam_ui_image", 8));
		JS_SetPropertyStr(ctx, g, "sam_ui_list", JS_NewCFunction(ctx, js_sam_ui_list, "sam_ui_list", 6));
		JS_SetPropertyStr(ctx, g, "sam_ui_list_add", JS_NewCFunction(ctx, js_sam_ui_list_add, "sam_ui_list_add", 5));
		JS_SetPropertyStr(ctx, g, "sam_ui_list_clear", JS_NewCFunction(ctx, js_sam_ui_list_clear, "sam_ui_list_clear", 2));
		JS_SetPropertyStr(ctx, g, "sam_ui_input", JS_NewCFunction(ctx, js_sam_ui_input, "sam_ui_input", 7));
		JS_SetPropertyStr(ctx, g, "sam_ui_input_text", JS_NewCFunction(ctx, js_sam_ui_input_text, "sam_ui_input_text", 2));
		JS_SetPropertyStr(ctx, g, "sam_ui_panel_style", JS_NewCFunction(ctx, js_sam_ui_panel_style, "sam_ui_panel_style", 4));
		JS_SetPropertyStr(ctx, g, "sam_ui_font", JS_NewCFunction(ctx, js_sam_ui_font, "sam_ui_font", 3));
		JS_SetPropertyStr(ctx, g, "sam_ui_list_row_height", JS_NewCFunction(ctx, js_sam_ui_list_row_height, "sam_ui_list_row_height", 3));
		JS_SetPropertyStr(ctx, g, "sam_ui_text_size", JS_NewCFunction(ctx, js_sam_ui_text_size, "sam_ui_text_size", 2));
		JS_SetPropertyStr(ctx, g, "sam_list_items", JS_NewCFunction(ctx, js_sam_list_items, "sam_list_items", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_item_info", JS_NewCFunction(ctx, js_sam_get_item_info, "sam_get_item_info", 1));
		JS_SetPropertyStr(ctx, g, "sam_list_monsters", JS_NewCFunction(ctx, js_sam_list_monsters, "sam_list_monsters", 0));
		JS_SetPropertyStr(ctx, g, "sam_list_spells", JS_NewCFunction(ctx, js_sam_list_spells, "sam_list_spells", 0));
		JS_SetPropertyStr(ctx, g, "sam_spawn_projectile", JS_NewCFunction(ctx, js_sam_spawn_projectile, "sam_spawn_projectile", 8));
		JS_SetPropertyStr(ctx, g, "sam_send_packet", JS_NewCFunction(ctx, js_sam_send_packet, "sam_send_packet", 3));
		JS_SetPropertyStr(ctx, g, "sam_player_count", JS_NewCFunction(ctx, js_sam_player_count, "sam_player_count", 0));
		JS_SetPropertyStr(ctx, g, "sam_local_player", JS_NewCFunction(ctx, js_sam_local_player, "sam_local_player", 0));
		JS_SetPropertyStr(ctx, g, "sam_monster_path_to", JS_NewCFunction(ctx, js_sam_monster_path_to, "sam_monster_path_to", 3));
		JS_SetPropertyStr(ctx, g, "sam_monster_face", JS_NewCFunction(ctx, js_sam_monster_face, "sam_monster_face", 3));
		JS_SetPropertyStr(ctx, g, "sam_monster_attack", JS_NewCFunction(ctx, js_sam_monster_attack, "sam_monster_attack", 1));
		JS_SetPropertyStr(ctx, g, "sam_monster_charge", JS_NewCFunction(ctx, js_sam_monster_charge, "sam_monster_charge", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_monster_type", JS_NewCFunction(ctx, js_sam_get_monster_type, "sam_get_monster_type", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_monster_name", JS_NewCFunction(ctx, js_sam_get_monster_name, "sam_get_monster_name", 1));
		JS_SetPropertyStr(ctx, g, "sam_monster_has_effect", JS_NewCFunction(ctx, js_sam_monster_has_effect, "sam_monster_has_effect", 2));
		JS_SetPropertyStr(ctx, g, "sam_monster_has_trait", JS_NewCFunction(ctx, js_sam_monster_has_trait, "sam_monster_has_trait", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_item_category", JS_NewCFunction(ctx, js_sam_get_item_category, "sam_get_item_category", 1));
		JS_SetPropertyStr(ctx, g, "sam_set_monster_stat", JS_NewCFunction(ctx, js_sam_set_monster_stat, "sam_set_monster_stat", 3));
		JS_SetPropertyStr(ctx, g, "sam_apply_monster_effect", JS_NewCFunction(ctx, js_sam_apply_monster_effect, "sam_apply_monster_effect", 3));
		JS_SetPropertyStr(ctx, g, "sam_kill_monster", JS_NewCFunction(ctx, js_sam_kill_monster, "sam_kill_monster", 1));
		JS_SetPropertyStr(ctx, g, "sam_spawn_monsters", JS_NewCFunction(ctx, js_sam_spawn_monsters, "sam_spawn_monsters", 3));
		JS_SetPropertyStr(ctx, g, "sam_get_monster_target", JS_NewCFunction(ctx, js_sam_get_monster_target, "sam_get_monster_target", 1));
		JS_SetPropertyStr(ctx, g, "sam_set_monster_target", JS_NewCFunction(ctx, js_sam_set_monster_target, "sam_set_monster_target", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_monster_data", JS_NewCFunction(ctx, js_sam_get_monster_data, "sam_get_monster_data", 2));
		JS_SetPropertyStr(ctx, g, "sam_set_monster_data", JS_NewCFunction(ctx, js_sam_set_monster_data, "sam_set_monster_data", 3));
		// v0.7.0 Feature 5: modify existing content (revert on unload)
		JS_SetPropertyStr(ctx, g, "sam_patch_class", JS_NewCFunction(ctx, js_sam_patch_class, "sam_patch_class", 2));
		JS_SetPropertyStr(ctx, g, "sam_unpatch_class", JS_NewCFunction(ctx, js_sam_unpatch_class, "sam_unpatch_class", 1));
		JS_SetPropertyStr(ctx, g, "sam_patch_item", JS_NewCFunction(ctx, js_sam_patch_item, "sam_patch_item", 2));
		JS_SetPropertyStr(ctx, g, "sam_patch_monster", JS_NewCFunction(ctx, js_sam_patch_monster, "sam_patch_monster", 2));
		JS_SetPropertyStr(ctx, g, "sam_add_class_passive", JS_NewCFunction(ctx, js_sam_add_class_passive, "sam_add_class_passive", 2));
		JS_SetPropertyStr(ctx, g, "sam_remove_class_passive", JS_NewCFunction(ctx, js_sam_remove_class_passive, "sam_remove_class_passive", 2));
		// Custom spells (Session 1)
		JS_SetPropertyStr(ctx, g, "sam_grant_spell", JS_NewCFunction(ctx, js_sam_grant_spell, "sam_grant_spell", 2));
		JS_SetPropertyStr(ctx, g, "sam_cast_spell", JS_NewCFunction(ctx, js_sam_cast_spell, "sam_cast_spell", 2));
		// v1.5.0 spell freedom (twins of the Lua bindings)
		JS_SetPropertyStr(ctx, g, "sam_cast_spell_at", JS_NewCFunction(ctx, js_sam_cast_spell_at, "sam_cast_spell_at", 3));
		JS_SetPropertyStr(ctx, g, "sam_cast_spell_pos", JS_NewCFunction(ctx, js_sam_cast_spell_pos, "sam_cast_spell_pos", 4));
		JS_SetPropertyStr(ctx, g, "sam_monster_cast_spell", JS_NewCFunction(ctx, js_sam_monster_cast_spell, "sam_monster_cast_spell", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_spells", JS_NewCFunction(ctx, js_sam_get_spells, "sam_get_spells", 1));
		JS_SetPropertyStr(ctx, g, "sam_player_knows_spell", JS_NewCFunction(ctx, js_sam_player_knows_spell, "sam_player_knows_spell", 2));
		JS_SetPropertyStr(ctx, g, "sam_remove_spell", JS_NewCFunction(ctx, js_sam_remove_spell, "sam_remove_spell", 2));
#ifdef SAM_JS_HAVE_BARONY
		JS_SetPropertyStr(ctx, g, "sam_grant_gold", JS_NewCFunction(ctx, js_sam_grant_gold, "sam_grant_gold", 2));
		JS_SetPropertyStr(ctx, g, "sam_apply_effect", JS_NewCFunction(ctx, js_sam_apply_effect, "sam_apply_effect", 3));
		JS_SetPropertyStr(ctx, g, "sam_remove_effect", JS_NewCFunction(ctx, js_sam_remove_effect, "sam_remove_effect", 2));
		// v1.5.0 player effect control + monster effect read/remove parity
		JS_SetPropertyStr(ctx, g, "sam_clear_effects", JS_NewCFunction(ctx, js_sam_clear_effects, "sam_clear_effects", 1));
		JS_SetPropertyStr(ctx, g, "sam_set_effect_duration", JS_NewCFunction(ctx, js_sam_set_effect_duration, "sam_set_effect_duration", 3));
		JS_SetPropertyStr(ctx, g, "sam_set_effect_strength", JS_NewCFunction(ctx, js_sam_set_effect_strength, "sam_set_effect_strength", 3));
		JS_SetPropertyStr(ctx, g, "sam_remove_monster_effect", JS_NewCFunction(ctx, js_sam_remove_monster_effect, "sam_remove_monster_effect", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_monster_effect_duration", JS_NewCFunction(ctx, js_sam_get_monster_effect_duration, "sam_get_monster_effect_duration", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_monster_effect_strength", JS_NewCFunction(ctx, js_sam_get_monster_effect_strength, "sam_get_monster_effect_strength", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_monster_effects", JS_NewCFunction(ctx, js_sam_get_monster_effects, "sam_get_monster_effects", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_stat", JS_NewCFunction(ctx, js_sam_get_stat, "sam_get_stat", 2));
		JS_SetPropertyStr(ctx, g, "sam_set_stat", JS_NewCFunction(ctx, js_sam_set_stat, "sam_set_stat", 3));
		JS_SetPropertyStr(ctx, g, "sam_set_move_speed", JS_NewCFunction(ctx, js_sam_set_move_speed, "sam_set_move_speed", 2));
		JS_SetPropertyStr(ctx, g, "sam_add_move_speed", JS_NewCFunction(ctx, js_sam_add_move_speed, "sam_add_move_speed", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_move_speed", JS_NewCFunction(ctx, js_sam_get_move_speed, "sam_get_move_speed", 1));
		JS_SetPropertyStr(ctx, g, "sam_level_up", JS_NewCFunction(ctx, js_sam_level_up, "sam_level_up", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_floor", JS_NewCFunction(ctx, js_sam_get_floor, "sam_get_floor", 0));
		JS_SetPropertyStr(ctx, g, "sam_spawn_item", JS_NewCFunction(ctx, js_sam_spawn_item, "sam_spawn_item", 6));
		JS_SetPropertyStr(ctx, g, "sam_item_id", JS_NewCFunction(ctx, js_sam_item_id, "sam_item_id", 1));
		JS_SetPropertyStr(ctx, g, "sam_message", JS_NewCFunction(ctx, js_sam_message, "sam_message", 2));
		JS_SetPropertyStr(ctx, g, "sam_play_sound", JS_NewCFunction(ctx, js_sam_play_sound, "sam_play_sound", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_nearby_entities", JS_NewCFunction(ctx, js_sam_get_nearby_entities, "sam_get_nearby_entities", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_equipped_item", JS_NewCFunction(ctx, js_sam_get_equipped_item, "sam_get_equipped_item", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_equipped_item_id", JS_NewCFunction(ctx, js_sam_get_equipped_item_id, "sam_get_equipped_item_id", 2));
		JS_SetPropertyStr(ctx, g, "sam_is_defending", JS_NewCFunction(ctx, js_sam_is_defending, "sam_is_defending", 1));
		JS_SetPropertyStr(ctx, g, "sam_is_action_held", JS_NewCFunction(ctx, js_sam_is_action_held, "sam_is_action_held", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_action_binding", JS_NewCFunction(ctx, js_sam_get_action_binding, "sam_get_action_binding", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_inventory_count", JS_NewCFunction(ctx, js_sam_get_inventory_count, "sam_get_inventory_count", 2));
		JS_SetPropertyStr(ctx, g, "sam_has_effect", JS_NewCFunction(ctx, js_sam_has_effect, "sam_has_effect", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_class", JS_NewCFunction(ctx, js_sam_get_class, "sam_get_class", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_race", JS_NewCFunction(ctx, js_sam_get_race, "sam_get_race", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_kills", JS_NewCFunction(ctx, js_sam_get_kills, "sam_get_kills", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_time_played", JS_NewCFunction(ctx, js_sam_get_time_played, "sam_get_time_played", 0));
		// v2 world-ops: position / teleport / spawn / inventory.
		JS_SetPropertyStr(ctx, g, "sam_get_player_uid", JS_NewCFunction(ctx, js_sam_get_player_uid, "sam_get_player_uid", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_position", JS_NewCFunction(ctx, js_sam_get_position, "sam_get_position", 1));
		JS_SetPropertyStr(ctx, g, "sam_set_position", JS_NewCFunction(ctx, js_sam_set_position, "sam_set_position", 3));
		JS_SetPropertyStr(ctx, g, "sam_spawn_monster", JS_NewCFunction(ctx, js_sam_spawn_monster, "sam_spawn_monster", 4));
		JS_SetPropertyStr(ctx, g, "sam_spawn_portal", JS_NewCFunction(ctx, js_sam_spawn_portal, "sam_spawn_portal", 2));
		JS_SetPropertyStr(ctx, g, "sam_remove_entity", JS_NewCFunction(ctx, js_sam_remove_entity, "sam_remove_entity", 1));
		JS_SetPropertyStr(ctx, g, "sam_set_chest_stash", JS_NewCFunction(ctx, js_sam_set_chest_stash, "sam_set_chest_stash", 2));
		JS_SetPropertyStr(ctx, g, "sam_travel_to_level", JS_NewCFunction(ctx, js_sam_travel_to_level, "sam_travel_to_level", 2));
		JS_SetPropertyStr(ctx, g, "sam_world_save", JS_NewCFunction(ctx, js_sam_world_save, "sam_world_save", 2));
		JS_SetPropertyStr(ctx, g, "sam_world_load", JS_NewCFunction(ctx, js_sam_world_load, "sam_world_load", 1));
		JS_SetPropertyStr(ctx, g, "sam_world_clear", JS_NewCFunction(ctx, js_sam_world_clear, "sam_world_clear", 1));
		JS_SetPropertyStr(ctx, g, "sam_world_keys", JS_NewCFunction(ctx, js_sam_world_keys, "sam_world_keys", 0));
		JS_SetPropertyStr(ctx, g, "sam_get_inventory", JS_NewCFunction(ctx, js_sam_get_inventory, "sam_get_inventory", 1));
		JS_SetPropertyStr(ctx, g, "sam_remove_item", JS_NewCFunction(ctx, js_sam_remove_item, "sam_remove_item", 1));
		// v1.4.0 — floating companion ("Stand") + facing reader.
		JS_SetPropertyStr(ctx, g, "sam_spawn_companion", JS_NewCFunction(ctx, js_sam_spawn_companion, "sam_spawn_companion", 3));
		JS_SetPropertyStr(ctx, g, "sam_companion_punch", JS_NewCFunction(ctx, js_sam_companion_punch, "sam_companion_punch", 1));
		JS_SetPropertyStr(ctx, g, "sam_get_facing", JS_NewCFunction(ctx, js_sam_get_facing, "sam_get_facing", 1));
		// v1.6.0 — impact frame: screen flash / manga burst / camera shake / hitstop.
		JS_SetPropertyStr(ctx, g, "sam_screen_flash", JS_NewCFunction(ctx, js_sam_screen_flash, "sam_screen_flash", 6));
		JS_SetPropertyStr(ctx, g, "sam_impact_frame", JS_NewCFunction(ctx, js_sam_impact_frame, "sam_impact_frame", 7));
		JS_SetPropertyStr(ctx, g, "sam_camera_shake", JS_NewCFunction(ctx, js_sam_camera_shake, "sam_camera_shake", 2));
		JS_SetPropertyStr(ctx, g, "sam_hitstop", JS_NewCFunction(ctx, js_sam_hitstop, "sam_hitstop", 1));
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
