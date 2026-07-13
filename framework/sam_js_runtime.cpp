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
#	include "engine/audio/sound.hpp" // playSoundPlayer, numsounds
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
		std::string path;
		bool enabled = false;
	};
	std::vector<Script> g_scripts;

	// ---- wall-clock watchdog --------------------------------------------------
	long long g_deadlineMs = 0; // 0 = disabled

	long long nowMs()
	{
		return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	void setDeadline(long long budgetMs) { g_deadlineMs = nowMs() + budgetMs; }
	void clearDeadline() { g_deadlineMs = 0; }

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

	// ---- host functions exposed to scripts ------------------------------------
	JSValue js_sam_log(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		if ( argc >= 1 )
		{
			const char* s = JS_ToCString(ctx, argv[0]);
			if ( s ) { SAM_INFO("SCRIPT", s); JS_FreeCString(ctx, s); }
		}
		return JS_UNDEFINED;
	}

	JSValue js_sam_grant_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		int32_t player = -1;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &player, argv[0]); }
		std::string name;
		if ( argc >= 2 ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }

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
		std::string lower = name;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		auto it = ItemTooltips.itemNameStringToItemID.find(lower);
		if ( it == ItemTooltips.itemNameStringToItemID.end() )
		{
			SAM_ERROR("JS", "sam_grant_item: unknown item type '" + name + "' — nothing granted.");
			return JS_NewBool(ctx, 0);
		}
		const ItemType type = static_cast<ItemType>(it->second);
		Item* item = newItem(type, EXCELLENT, 0, 1, 0, true, nullptr);
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

	int samEffectNameToId(const char* nameIn)
	{
		const std::string n = samUpper(nameIn);
		if ( n == "LEVITATING" ) { return EFF_LEVITATING; }
		if ( n == "INVISIBLE" )  { return EFF_INVISIBLE;  }
		if ( n == "CONFUSED" )   { return EFF_CONFUSED;   }
		if ( n == "POISONED" )   { return EFF_POISONED;   }
		if ( n == "BLEEDING" )   { return EFF_BLEEDING;   }
		if ( n == "ASLEEP" )     { return EFF_ASLEEP;     }
		if ( n == "PARALYZED" )  { return EFF_PARALYZED;  }
		if ( n == "DRUNK" )      { return EFF_DRUNK;      }
		if ( n == "BLIND" )      { return EFF_BLIND;      }
		if ( n == "GREASY" )     { return EFF_GREASY;     }
		if ( n == "VOMITING" )   { return EFF_VOMITING;   }
		if ( n == "WEBBED" )     { return EFF_WEBBED;     }
		if ( n == "SLOW" )       { return EFF_SLOW;       }
		if ( n == "FAST" )       { return EFF_FAST;       }
		return -1;
	}

	JSValue js_sam_grant_gold(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		int32_t player = -1, amount = 0;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( argc >= 2 ) { JS_ToInt32(ctx, &amount, argv[1]); }
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
		int32_t player = -1, ticks = 0;
		std::string name;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( argc >= 2 ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( argc >= 3 ) { JS_ToInt32(ctx, &ticks, argv[2]); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_apply_effect refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("JS", "sam_apply_effect: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 ) { SAM_ERROR("JS", "sam_apply_effect: unknown effect '" + name + "'."); return JS_NewBool(ctx, 0); }
		const bool ok = players[player]->entity->setEffect(eff, true, ticks, true);
		SAM_INFO("JS", "Applied effect " + name + " to player " + std::to_string(player) + (ok ? "" : " (refused/immune)"));
		return JS_NewBool(ctx, ok ? 1 : 0);
	}

	JSValue js_sam_remove_effect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		int32_t player = -1;
		std::string name;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( argc >= 2 ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_remove_effect refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("JS", "sam_remove_effect: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const int eff = samEffectNameToId(name.c_str());
		if ( eff < 0 ) { SAM_ERROR("JS", "sam_remove_effect: unknown effect '" + name + "'."); return JS_NewBool(ctx, 0); }
		players[player]->entity->setEffect(eff, false, 0, true);
		SAM_INFO("JS", "Removed effect " + name + " from player " + std::to_string(player));
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_get_stat(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		int32_t player = -1;
		std::string name;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( argc >= 2 ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
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
		else if ( n == "LEVEL" || n == "LVL" ) { v = s->LVL; }
		else if ( n == "EXP" )   { v = s->EXP; }
		else { SAM_ERROR("JS", "sam_get_stat: unknown stat '" + name + "'."); return JS_NewInt32(ctx, 0); }
		return JS_NewInt64(ctx, v);
	}

	JSValue js_sam_set_stat(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		int32_t player = -1, value = 0;
		std::string name;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( argc >= 2 ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( argc >= 3 ) { JS_ToInt32(ctx, &value, argv[2]); }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_set_stat refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("JS", "sam_set_stat: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		const std::string n = samUpper(name.c_str());
		Stat* s = stats[player];
		Entity* e = players[player]->entity;
		if      ( n == "HP" )    { if ( e ) { e->setHP(value); } else { s->HP = samClampInt(value, 0, s->MAXHP); } }
		else if ( n == "MP" )    { if ( e ) { e->setMP(value); } else { s->MP = samClampInt(value, 0, s->MAXMP); } }
		else if ( n == "MAXHP" ) { s->MAXHP = (value < 1 ? 1 : value); if ( s->HP > s->MAXHP ) { s->HP = s->MAXHP; } }
		else if ( n == "MAXMP" ) { s->MAXMP = (value < 0 ? 0 : value); if ( s->MP > s->MAXMP ) { s->MP = s->MAXMP; } }
		else if ( n == "STR" )   { s->STR = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "DEX" )   { s->DEX = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "CON" )   { s->CON = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "INT" )   { s->INT = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "PER" )   { s->PER = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "CHR" )   { s->CHR = samClampInt(value, -128, MAX_PLAYER_STAT_VALUE); }
		else if ( n == "GOLD" )  { s->GOLD = (value < 0 ? 0 : value); }
		else if ( n == "LEVEL" || n == "LVL" ) { s->LVL = samClampInt(value, 1, 255); }
		else if ( n == "EXP" )   { s->EXP = samClampInt(value, 0, 99); }
		else { SAM_ERROR("JS", "sam_set_stat: unknown stat '" + name + "'."); return JS_NewBool(ctx, 0); }
		SAM_INFO("JS", "Set stat " + n + " = " + std::to_string(value) + " on player " + std::to_string(player));
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_get_floor(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
	{
		return JS_NewInt32(ctx, currentlevel);
	}

	JSValue js_sam_spawn_item(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		int32_t x = 0, y = 0;
		std::string name;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &x, argv[0]); }
		if ( argc >= 2 ) { JS_ToInt32(ctx, &y, argv[1]); }
		if ( argc >= 3 ) { const char* s = JS_ToCString(ctx, argv[2]); if ( s ) { name = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_spawn_item refused: host only."); return JS_NewBool(ctx, 0); }
		std::string lower = name;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		auto it = ItemTooltips.itemNameStringToItemID.find(lower);
		if ( it == ItemTooltips.itemNameStringToItemID.end() )
		{ SAM_ERROR("JS", "sam_spawn_item: unknown item type '" + name + "'."); return JS_NewBool(ctx, 0); }
		Entity* e = spawnGroundItem(static_cast<ItemType>(it->second), EXCELLENT, 0, 1, x, y);
		if ( !e ) { SAM_ERROR("JS", "sam_spawn_item: invalid tile (" + std::to_string(x) + "," + std::to_string(y) + ")."); return JS_NewBool(ctx, 0); }
		SAM_INFO("JS", "Spawned item " + name + " at (" + std::to_string(x) + "," + std::to_string(y) + ")");
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_message(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		int32_t player = -1;
		std::string text;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( argc >= 2 ) { const char* s = JS_ToCString(ctx, argv[1]); if ( s ) { text = s; JS_FreeCString(ctx, s); } }
		if ( multiplayer == CLIENT ) { SAM_WARN("JS", "sam_message refused: host only."); return JS_NewBool(ctx, 0); }
		if ( player < 0 || player >= MAXPLAYERS )
		{ SAM_ERROR("JS", "sam_message: invalid player index " + std::to_string(player) + "."); return JS_NewBool(ctx, 0); }
		messagePlayer(player, MESSAGE_MISC, "%s", text.c_str());
		return JS_NewBool(ctx, 1);
	}

	JSValue js_sam_play_sound(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
	{
		int32_t soundId = -1, vol = 128;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &soundId, argv[0]); }
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
		int32_t player = -1;
		double radiusTiles = 0.0;
		if ( argc >= 1 ) { JS_ToInt32(ctx, &player, argv[0]); }
		if ( argc >= 2 ) { JS_ToFloat64(ctx, &radiusTiles, argv[1]); }
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
#endif // SAM_JS_HAVE_BARONY

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
#ifdef SAM_JS_HAVE_BARONY
		JS_SetPropertyStr(ctx, g, "sam_grant_gold", JS_NewCFunction(ctx, js_sam_grant_gold, "sam_grant_gold", 2));
		JS_SetPropertyStr(ctx, g, "sam_apply_effect", JS_NewCFunction(ctx, js_sam_apply_effect, "sam_apply_effect", 3));
		JS_SetPropertyStr(ctx, g, "sam_remove_effect", JS_NewCFunction(ctx, js_sam_remove_effect, "sam_remove_effect", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_stat", JS_NewCFunction(ctx, js_sam_get_stat, "sam_get_stat", 2));
		JS_SetPropertyStr(ctx, g, "sam_set_stat", JS_NewCFunction(ctx, js_sam_set_stat, "sam_set_stat", 3));
		JS_SetPropertyStr(ctx, g, "sam_get_floor", JS_NewCFunction(ctx, js_sam_get_floor, "sam_get_floor", 0));
		JS_SetPropertyStr(ctx, g, "sam_spawn_item", JS_NewCFunction(ctx, js_sam_spawn_item, "sam_spawn_item", 3));
		JS_SetPropertyStr(ctx, g, "sam_message", JS_NewCFunction(ctx, js_sam_message, "sam_message", 2));
		JS_SetPropertyStr(ctx, g, "sam_play_sound", JS_NewCFunction(ctx, js_sam_play_sound, "sam_play_sound", 2));
		JS_SetPropertyStr(ctx, g, "sam_get_nearby_entities", JS_NewCFunction(ctx, js_sam_get_nearby_entities, "sam_get_nearby_entities", 2));
#endif
		JS_FreeValue(ctx, g);
	}

	// Run a JS source string in a fresh hardened context; capture its on_event.
	bool loadJSSource(const std::string& source, const std::string& label)
	{
		JSContext* ctx = newSandboxContext(g_rt);
		if ( !ctx ) { SAM_ERROR("JS", "failed to create sandbox context for " + label); return false; }
		registerHostApi(ctx);

		setDeadline(g_cfg.callbackBudgetMs); // bound the top-level eval (kills a top-level infinite loop)
		JSValue res = JS_Eval(ctx, source.c_str(), source.size(), label.c_str(), JS_EVAL_TYPE_GLOBAL);
		clearDeadline();
		if ( JS_IsException(res) )
		{
			SAM_ERROR("JS", "error running '" + label + "': " + exceptionToString(ctx));
			JS_FreeValue(ctx, res);
			JS_FreeContext(ctx);
			return false;
		}
		JS_FreeValue(ctx, res);

		JSValue g = JS_GetGlobalObject(ctx);
		JSValue fn = JS_GetPropertyStr(ctx, g, "on_event");
		JS_FreeValue(ctx, g);
		if ( !JS_IsFunction(ctx, fn) )
		{
			JS_FreeValue(ctx, fn);
			SAM_WARN("JS", "script '" + label + "' defines no on_event(event) — no handler registered.");
			Script sc; sc.ctx = ctx; sc.onEvent = JS_UNDEFINED; sc.path = label; sc.enabled = false;
			g_scripts.push_back(sc);
			return true;
		}
		Script sc; sc.ctx = ctx; sc.onEvent = fn; sc.path = label; sc.enabled = true;
		g_scripts.push_back(sc);
		SAM_INFO("JS", "Loaded script '" + label + "' (on_event registered).");
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
		if ( !g_tsCtx ) { SAM_ERROR("JS", "transpile context alloc failed"); return false; }

		setDeadline(g_cfg.transpileBudgetMs);
		JSValue r = JS_Eval(g_tsCtx, tsLib.c_str(), tsLib.size(), "typescript.js", JS_EVAL_TYPE_GLOBAL);
		clearDeadline();
		if ( JS_IsException(r) )
		{
			SAM_ERROR("JS", "failed to load typescript.js: " + exceptionToString(g_tsCtx));
			JS_FreeValue(g_tsCtx, r);
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

	bool loadScriptJS(const std::string& path)
	{
		if ( !g_rt ) { SAM_ERROR("JS", "loadScriptJS before init()"); return false; }
		std::string src;
		if ( !readFile(path, src) ) { SAM_ERROR("JS", "cannot read JS file: " + path); return false; }
		return loadJSSource(src, path);
	}

	bool loadScriptTS(const std::string& path, const std::string& cacheDir, const std::string& tsCompilerJsPath)
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
			return loadJSSource(js, path);
		}

		SAM_INFO("JS", "TS cache miss for '" + path + "' — transpiling...");
		if ( !ensureTranspiler(tsCompilerJsPath) ) { return false; }
		if ( !transpile(src, js) ) { SAM_ERROR("JS", "TypeScript transpile failed for: " + path); return false; }
		if ( !writeFileAtomic(cachePath, js) )
		{
			SAM_WARN("JS", "could not write TS cache to " + cachePath + " (continuing without cache).");
		}
		SAM_INFO("JS", "Transpiled '" + path + "' -> " + std::to_string(js.size()) + " bytes JS (cached " + key + ".js).");
		return loadJSSource(js, path);
	}

	int dispatchEvent(const Event& ev)
	{
		if ( !g_rt ) { SAM_ERROR("JS", "dispatchEvent before init()"); return 0; }
		int delivered = 0;
		for ( auto& sc : g_scripts )
		{
			if ( !sc.enabled ) { continue; }
			JSValue evObj = makeEventObject(sc.ctx, ev);
			JSValue argv[1] = { evObj };
			setDeadline(g_cfg.callbackBudgetMs);
			JSValue ret = JS_Call(sc.ctx, sc.onEvent, JS_UNDEFINED, 1, argv);
			clearDeadline();
			JS_FreeValue(sc.ctx, evObj);
			if ( JS_IsException(ret) )
			{
				SAM_ERROR("JS", "on_event error in '" + sc.path + "': " + exceptionToString(sc.ctx));
				SAM_WARN("JS", "script '" + sc.path + "' disabled after an on_event error.");
				sc.enabled = false;
			}
			else
			{
				++delivered;
			}
			JS_FreeValue(sc.ctx, ret);
		}
		SAM_INFO("JS", "Dispatched '" + ev.name + "' to " + std::to_string(delivered) + " script(s).");
		return delivered;
	}

	void releaseTranspiler()
	{
		if ( g_tsCtx ) { JS_FreeContext(g_tsCtx); g_tsCtx = nullptr; }
		if ( g_tsRt )  { JS_FreeRuntime(g_tsRt);  g_tsRt = nullptr; }
	}

	void shutdown()
	{
		for ( auto& sc : g_scripts )
		{
			if ( sc.ctx )
			{
				JS_FreeValue(sc.ctx, sc.onEvent);
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

} // namespace SAMJs
