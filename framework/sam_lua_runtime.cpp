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
#include "sam_logger.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstdlib>
#include <cstring>
#include <cmath>    // lround — move-speed fixed-point encoding
#include <string>
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
#	include "engine/audio/sound.hpp" // playSoundPlayer, numsounds
#	include "files.hpp"     // outputdir (savegames base dir for persistent mod data)
#	include "sam_items.hpp" // SAMItems::itemIdForIdString (custom item names in queries)
#	include "sam_effects.hpp" // custom status effects (resolve "ns:effect" ids)
#	include "sam_sounds.hpp" // custom sounds (resolve "ns:sound" ids in sam_play_sound)
#	include "sam_races.hpp" // custom races (sam_get_race id lookup)
#	include "sam_classes.hpp" // v0.7.0 F5: SAMClasses::patchClass / addClassPassive
#	include "sam_monster_patches.hpp" // v0.7.0 F5: SAMMonsterPatch::set
#	include "sam_spells.hpp"  // custom-spell registry (sam_grant_spell)
#	include "magic/magic.hpp" // addSpell (grant a spell to a player)
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
		if ( resolvedType < 0 )
		{
			std::string lower = itemName;
			for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
			auto it = ItemTooltips.itemNameStringToItemID.find(lower);
			if ( it != ItemTooltips.itemNameStringToItemID.end() ) { resolvedType = it->second; }
		}
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

		if ( players[player]->isLocalPlayer() )
		{
			itemPickup(player, item); // copies/merges into the player's inventory
			free(item);               // free our temp, mirroring applyLoadout
		}
		else
		{
			// Server -> remote client inventory needs a dedicated item packet; not
			// wired in this first pass. Discard the temp to avoid a leak.
			free(item);
			SAM_WARN("LUA", "sam_grant_item: remote-player delivery not wired yet; '"
				+ itemName + "' not given to player " + std::to_string(player) + ".");
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
	int samEffectNameToId(const char* nameIn)
	{
		const std::string n = samUpper(nameIn);
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
		if ( eff < 0 ) { SAM_ERROR("LUA", std::string("sam_apply_effect: unknown effect '") + (nameC ? nameC : "") + "'."); lua_pushboolean(Ls, 0); return 1; }
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
		if ( eff < 0 ) { SAM_ERROR("LUA", std::string("sam_remove_effect: unknown effect '") + (nameC ? nameC : "") + "'."); lua_pushboolean(Ls, 0); return 1; }
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
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_get_stat refused: host only."); lua_pushinteger(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("LUA", "sam_get_stat: invalid player index " + std::to_string(player) + "."); lua_pushinteger(Ls, 0); return 1; }
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
		else { SAM_ERROR("LUA", std::string("sam_get_stat: unknown stat '") + (nameC ? nameC : "") + "'."); lua_pushinteger(Ls, 0); return 1; }
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

	// sam_get_move_speed(player) -> number. Readable on clients too: a client needs to see
	// its own multiplier, and that is exactly the value its movement code is using.
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
	// Host-only. Adds 100*count EXP; the host's Entity::handleEffects drains 100/tick and
	// runs the FULL vanilla level-up (attribute rolls, HP/MP, level-up screen/sound, the
	// ATTR/LVLI packets, and one player.on_level_up per level). No code duplication, no
	// manual flush — the engine owns it. Since EXP is always 0..99 between levels, this
	// grants exactly `count` levels.
	int lua_sam_level_up(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const int count = lua_isnoneornil(Ls, 2) ? 1 : (int)luaL_checkinteger(Ls, 2);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_level_up refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] )
		{ SAM_ERROR("LUA", "sam_level_up: invalid player index " + std::to_string(player) + "."); lua_pushboolean(Ls, 0); return 1; }
		const int levels = samClampInt(count, 1, 255);
		stats[player]->EXP += 100 * levels;
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

	// sam_spawn_item(x, y, "ITEM_NAME") — spawn a ground item at map tile (x,y).
	int lua_sam_spawn_item(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int x = (int)luaL_checkinteger(Ls, 1);
		const int y = (int)luaL_checkinteger(Ls, 2);
		const char* nameC = luaL_checkstring(Ls, 3);
		const std::string itemName = nameC ? nameC : "";
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_spawn_item refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		std::string lower = itemName;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		auto it = ItemTooltips.itemNameStringToItemID.find(lower);
		if ( it == ItemTooltips.itemNameStringToItemID.end() )
		{ SAM_ERROR("LUA", "sam_spawn_item: unknown item type '" + itemName + "'."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = spawnGroundItem(static_cast<ItemType>(it->second), EXCELLENT, 0, 1, x, y);
		if ( !e ) { SAM_ERROR("LUA", "sam_spawn_item: invalid tile (" + std::to_string(x) + "," + std::to_string(y) + ")."); lua_pushboolean(Ls, 0); return 1; }
		SAM_INFO("LUA", "Spawned item " + itemName + " at (" + std::to_string(x) + "," + std::to_string(y) + ")");
		lua_pushboolean(Ls, 1);
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
		else
		{
			std::string lower = name;
			for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
			auto it = ItemTooltips.itemNameStringToItemID.find(lower);
			if ( it != ItemTooltips.itemNameStringToItemID.end() ) { id = it->second; }
		}
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
		messagePlayer(player, MESSAGE_MISC, "%s", text ? text : "");
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_play_sound(soundId[, vol]) — play a sound for all connected players. soundId is
	// a vanilla numeric index OR the "namespace:sound" id of a custom sound.
	int lua_sam_play_sound(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		int soundId = -1;
		if ( lua_type(Ls, 1) == LUA_TSTRING )
		{
			const char* nm = lua_tostring(Ls, 1);
			soundId = SAMSounds::soundIndexForId(nm ? nm : "");
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
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( players[i] && !client_disconnected[i] )
			{
				playSoundPlayer(i, (Uint16)soundId, (Uint8)vol);
			}
		}
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
		if ( multiplayer == CLIENT ) { return 1; }
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

	// Tell the client that owns `player` its multiplier changed.
	//
	// Host->client, in-game: the receiving entry lives in net.cpp's clientPacketHandlers.
	// Guarded the way every vanilla per-client send is: a local or splitscreen player
	// already reads g_samMoveSpeed directly, and player 0 would index net_clients[-1].
	void samSendMoveSpeed(int player)
	{
		if ( multiplayer != SERVER ) { return; }
		if ( player <= 0 || player >= MAXPLAYERS ) { return; }
		if ( !players[player] || players[player]->isLocalPlayer() ) { return; }
		if ( client_disconnected[player] ) { return; }

		strcpy((char*)net_packet->data, "SAMS");
		net_packet->data[4] = (Uint8)player;
		// Fixed point x1000 rather than a raw double: the wire stays endian-defined via
		// SDLNet_Write32, and 0.1..3.0 needs nowhere near the precision of 8 raw bytes.
		SDLNet_Write32((Uint32)(Sint32)lround(g_samMoveSpeed[player] * 1000.0), &net_packet->data[5]);
		net_packet->address.host = net_clients[player - 1].host;
		net_packet->address.port = net_clients[player - 1].port;
		net_packet->len = 9;
		sendPacketSafe(net_sock, -1, net_packet, player - 1);
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
	// button being down. Correct in multiplayer for remote players too: vanilla already
	// syncs it to the host with its own 'SHLD' packet.
	int lua_sam_is_defending(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, stats[player]->defending ? 1 : 0);
		return 1;
	}

	// sam_is_action_held(player, "Use") -> boolean. Reads a BOUND action, so it follows
	// the player's own keybinds. Local player only (input never leaves its machine).
	int lua_sam_is_action_held(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* action = luaL_checkstring(Ls, 2);
		lua_pushboolean(Ls, SAMLua::isActionHeld(player, action ? action : "") ? 1 : 0);
		return 1;
	}

	// sam_get_action_binding(player, "Use") -> string|nil. The physical input behind an
	// action ("Mouse3"), for prompts. nil when the player has it unbound.
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

	int lua_sam_get_inventory_count(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* nameC = luaL_checkstring(Ls, 2);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { lua_pushinteger(Ls, 0); return 1; }
		std::string lower = nameC ? nameC : "";
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		int wantType = -1;
		auto mit = ItemTooltips.itemNameStringToItemID.find(lower);
		if ( mit != ItemTooltips.itemNameStringToItemID.end() ) { wantType = mit->second; }
		else { const int cid = SAMItems::itemIdForIdString(nameC ? nameC : ""); if ( cid >= 0 ) { wantType = cid; } }
		if ( wantType < 0 ) { lua_pushinteger(Ls, 0); return 1; }
		long long total = 0;
		for ( node_t* node = stats[player]->inventory.first; node != nullptr; node = node->next )
		{
			Item* it = (Item*)node->element;
			if ( it && (int)it->type == wantType ) { total += it->count; }
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
		if ( eff < 0 ) { SAM_WARN("LUA", std::string("sam_has_effect: unknown effect '") + (nameC ? nameC : "") + "'."); lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, stats[player]->getEffectActive(eff) != 0 ? 1 : 0);
		return 1;
	}

	// sam_get_effect_duration(player, "EFFECT") -> remaining ticks (50 = 1s). 0 if the
	// effect is not active; -1 for a permanent effect (no timer). Readable on clients
	// (effects are synced), so a debuff can scale/decay by how much time is left.
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
		for ( const auto& e : samEffectNames )
		{
			const Uint8 s = stats[player]->getEffectActive(e.id);
			if ( s != 0 ) { pushEntry(e.name, e.id, s); }
		}
		for ( int id = 135; id < NUMEFFECTS; ++id ) // custom pseudo-effect slots, if any are live
		{
			const Uint8 s = stats[player]->getEffectActive(id);
			if ( s != 0 ) { pushEntry("CUSTOM:" + std::to_string(id), id, s); }
		}
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
			if ( type < 0 )
			{
				std::string lower = name;
				for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
				auto it = ItemTooltips.itemNameStringToItemID.find(lower);
				if ( it != ItemTooltips.itemNameStringToItemID.end() ) { type = it->second; }
			}
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
			return def ? def->name.c_str() : "";
		}
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
		if ( player < 0 || player >= MAXPLAYERS ) { lua_pushnil(Ls); return 1; }
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

	std::string samSanitize(const std::string& s)
	{
		std::string o;
		for ( char c : s ) { o += ( c == '/' || c == '\\' || c == ':' || c == '.' ) ? '_' : c; }
		return o.empty() ? std::string("_") : o;
	}

	std::string samModDataFile(const std::string& ns, const std::string& key)
	{
#ifdef SAM_LUA_HAVE_BARONY
		const std::string base = std::string(outputdir) + "/savegames/sam_mod_data";
#else
		const std::string base = "./sam_mod_data";
#endif
		return base + "/" + samSanitize(ns) + "/" + samSanitize(key) + ".json";
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
	int lua_sam_save_data(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* keyC = luaL_checkstring(Ls, 1);
		const std::string key = keyC ? keyC : "";
		if ( g_currentNs.empty() ) { SAM_WARN("LUA", "sam_save_data: no owning mod namespace — ignored."); lua_pushboolean(Ls, 0); return 1; }
		nlohmann::json j = luaToJson(Ls, 2, 0);
		const std::string path = samModDataFile(g_currentNs, key);
		try
		{
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
			std::ofstream f(path, std::ios::binary | std::ios::trunc);
			if ( !f.is_open() ) { SAM_ERROR("LUA", "sam_save_data: cannot write " + path); lua_pushboolean(Ls, 0); return 1; }
			// 'replace' handler: non-UTF-8 Lua-string bytes -> U+FFFD instead of a
			// thrown type_error (the try/catch already prevents a crash, but this
			// persists the data instead of dropping the whole save).
			f << j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
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
		t.remaining = ticks < 1 ? 1 : ticks;
		t.interval  = repeating ? (ticks < 1 ? 1 : ticks) : 0;
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
		const int n = SAMLua::dispatchEvent(ev) + SAMJs::dispatchEvent(jsev);
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

	// v0.7.0 Feature 2: sam_deal_damage(entity_uid, amount) — deal `amount` damage to
	// any entity by UID (host-only, UID-only, existence-validated). Positive = damage.
	int lua_sam_deal_damage(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int amount = (int)luaL_checkinteger(Ls, 2);
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_deal_damage refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = uidToEntity((Uint32)uid);
		if ( !e ) { SAM_WARN("LUA", "sam_deal_damage: no entity with uid " + std::to_string(uid) + "."); lua_pushboolean(Ls, 0); return 1; }
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

	// v0.7.0 Feature 3: sam_is_key_held(key_name) -> boolean.
	int lua_sam_is_key_held(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* nameC = luaL_checkstring(Ls, 1);
		lua_pushboolean(Ls, SAMLua::isKeyHeld(nameC ? nameC : "") ? 1 : 0);
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

	// sam_get_position(uid) -> tileX, tileY | nil. Any live entity (player/monster/item).
	int lua_sam_get_position(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e ) { lua_pushnil(Ls); return 1; }
		lua_pushinteger(Ls, (lua_Integer)((int)e->x >> 4)); // pixel -> tile
		lua_pushinteger(Ls, (lua_Integer)((int)e->y >> 4));
		return 2;
	}

	// sam_set_position(uid, tileX, tileY) -> boolean. Players go through the safe
	// teleport() path (obstacle + MFLAG_DISABLETELEPORT guards + TELE packet) so they
	// can't tunnel into walls; other entities move by x/y + UPDATENEEDED (the server's
	// per-frame broadcast picks it up — no new packet). Host only.
	int lua_sam_set_position(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		const int tx = (int)luaL_checkinteger(Ls, 2);
		const int ty = (int)luaL_checkinteger(Ls, 3);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_position refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e ) { SAM_WARN("LUA", "sam_set_position: no entity uid " + std::to_string(uid) + "."); lua_pushboolean(Ls, 0); return 1; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("LUA", "sam_set_position: tile (" + std::to_string(tx) + "," + std::to_string(ty) + ") out of bounds."); lua_pushboolean(Ls, 0); return 1; }
		if ( e->behavior == &actPlayer )
		{
			const bool ok = e->teleport(tx, ty); // may refuse (walls / minotaur level / map flag)
			lua_pushboolean(Ls, ok ? 1 : 0);
			return 1;
		}
		e->x = (double)(tx * 16 + 8);
		e->y = (double)(ty * 16 + 8);
		e->flags[UPDATENEEDED] = true;
		e->flags[NOUPDATE] = false;
		TileEntityList.updateEntity(*e); // re-bucket in the spatial grid, as teleport() does
		lua_pushboolean(Ls, 1);
		return 1;
	}

	// sam_spawn_monster(tileX, tileY, "name" [, shopType]) -> uid | nil. Whitelisted to
	// the Monster enum (name resolved case-insensitively). shopType (0-14) only applies
	// to "shopkeeper" and picks the store kind. Host only; net replication is done by
	// summonMonster itself (the SUMM packet). Returns the new monster's uid so scripts
	// can move / query it afterwards.
	int lua_sam_spawn_monster(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int tx = (int)luaL_checkinteger(Ls, 1);
		const int ty = (int)luaL_checkinteger(Ls, 2);
		const char* nameC = luaL_checkstring(Ls, 3);
		const std::string monName = nameC ? nameC : "";
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_spawn_monster refused: host only."); lua_pushnil(Ls); return 1; }
		const int creature = samMonsterNameToId(nameC);
		if ( creature <= 0 ) { SAM_ERROR("LUA", "sam_spawn_monster: unknown monster '" + monName + "'."); lua_pushnil(Ls); return 1; }
		if ( tx < 0 || tx >= (int)map.width || ty < 0 || ty >= (int)map.height )
		{ SAM_ERROR("LUA", "sam_spawn_monster: tile (" + std::to_string(tx) + "," + std::to_string(ty) + ") out of bounds."); lua_pushnil(Ls); return 1; }
		Entity* e = summonMonster(static_cast<Monster>(creature), tx * 16 + 8, ty * 16 + 8); // pixel coords
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
	// identified, equipped }. Returns an empty table for an invalid player. Reader.
	int lua_sam_get_inventory(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		lua_newtable(Ls);
		if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return 1; }
		int idx = 1;
		for ( node_t* node = stats[player]->inventory.first; node != nullptr; node = node->next )
		{
			Item* it = (Item*)node->element;
			if ( !it ) { continue; }
			lua_newtable(Ls);
			lua_pushinteger(Ls, (lua_Integer)it->uid);        lua_setfield(Ls, -2, "uid");
			lua_pushinteger(Ls, (lua_Integer)it->type);       lua_setfield(Ls, -2, "type");
			if ( (int)it->type >= 0 && (int)it->type < NUMITEMS ) { lua_pushstring(Ls, itemNameStrings[(int)it->type + 2]); }
			else { lua_pushstring(Ls, "custom"); }
			lua_setfield(Ls, -2, "name");
			lua_pushinteger(Ls, (lua_Integer)it->count);      lua_setfield(Ls, -2, "count");
			lua_pushinteger(Ls, (lua_Integer)it->beatitude);  lua_setfield(Ls, -2, "beatitude");
			lua_pushinteger(Ls, (lua_Integer)it->status);     lua_setfield(Ls, -2, "status");
			lua_pushboolean(Ls, it->identified ? 1 : 0);      lua_setfield(Ls, -2, "identified");
			lua_pushboolean(Ls, (itemSlot(stats[player], it) != nullptr) ? 1 : 0); lua_setfield(Ls, -2, "equipped");
			lua_rawseti(Ls, -2, idx++);
		}
		return 1;
	}

	// sam_remove_item(itemUid) -> boolean. Removes an inventory item by its uid. Refuses
	// an EQUIPPED item (freeing it would dangle stats[p]->weapon etc. -> crash) — unequip
	// first. Consumes the whole stack via consumeItem. Host only.
	int lua_sam_remove_item(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_remove_item refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Item* it = uidToItem((Uint32)uid);
		if ( !it ) { SAM_WARN("LUA", "sam_remove_item: no item uid " + std::to_string(uid) + "."); lua_pushboolean(Ls, 0); return 1; }
		int owner = -1;
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( !stats[p] ) { continue; }
			if ( itemSlot(stats[p], it) != nullptr )
			{ SAM_WARN("LUA", "sam_remove_item: item uid " + std::to_string(uid) + " is equipped; unequip first."); lua_pushboolean(Ls, 0); return 1; }
			for ( node_t* n = stats[p]->inventory.first; n; n = n->next ) { if ( (Item*)n->element == it ) { owner = p; break; } }
		}
		Item* ref = it;
		while ( ref ) { consumeItem(ref, owner >= 0 ? owner : 0); } // decrements + frees the whole stack
		SAM_INFO("LUA", "Removed item uid " + std::to_string(uid) + ".");
		lua_pushboolean(Ls, 1);
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
		if ( eff < 0 ) { SAM_WARN("LUA", std::string("sam_monster_has_effect: unknown effect '") + (nameC ? nameC : "") + "'."); lua_pushboolean(Ls, 0); return 1; }
		lua_pushboolean(Ls, e->getStats()->getEffectActive(eff) != 0 ? 1 : 0);
		return 1;
#else
		(void)uid; (void)nameC; lua_pushboolean(Ls, 0); return 1;
#endif
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
		if ( mtype < 0 ) { SAM_WARN("LUA", std::string("sam_spawn_monsters: unknown monster type '") + (typeC ? typeC : "") + "'"); lua_pushinteger(Ls, 0); return 1; }
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
		nlohmann::json j = luaToJson(Ls, 3, 0);
		// Lua strings are raw bytes; a non-UTF-8 value makes strict dump() throw
		// nlohmann::type_error. Since Lua is built as C (setjmp/longjmp), that C++
		// exception would unwind past lua_pcall -> std::terminate (host crash).
		// Use the 'replace' handler so invalid bytes become U+FFFD and dump()
		// never throws. (Matches the try/catch guard in sam_save_data.)
		SAMLua::monsterDataSet((unsigned)(Sint32)uid, keyC ? keyC : "",
			j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
		return 0;
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
							if ( lua_type(Ls, -2) == LUA_TSTRING ) { patch.skills[lua_tostring(Ls, -2)] = (int)lua_tointeger(Ls, -1); }
							lua_pop(Ls, 1);
						}
					}
					else if ( lua_isnumber(Ls, -1) ) { patch.stats[uk] = (int)lua_tointeger(Ls, -1); }
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
			lua_pushnil(Ls);
			while ( lua_next(Ls, 2) != 0 )
			{
				if ( lua_type(Ls, -2) == LUA_TSTRING )
				{
					const std::string uk = samUpper(lua_tostring(Ls, -2));
					if ( uk == "ATTRIBUTES" && lua_istable(Ls, -1) )
					{
						const int aIdx = lua_gettop(Ls);
						lua_pushnil(Ls);
						while ( lua_next(Ls, aIdx) != 0 )
						{
							if ( lua_type(Ls, -2) == LUA_TSTRING ) { patch.attributes[lua_tostring(Ls, -2)] = (int)lua_tointeger(Ls, -1); }
							lua_pop(Ls, 1);
						}
					}
					else if ( uk == "WEIGHT" ) { patch.hasWeight = true; patch.weight = (int)lua_tointeger(Ls, -1); }
					else if ( uk == "VALUE" || uk == "GOLD_VALUE" ) { patch.hasValue = true; patch.value = (int)lua_tointeger(Ls, -1); }
					else if ( uk == "LEVEL" ) { patch.hasLevel = true; patch.level = (int)lua_tointeger(Ls, -1); }
					else if ( uk == "CATEGORY" && lua_isstring(Ls, -1) ) { patch.hasCategory = true; patch.category = samUpper(lua_tostring(Ls, -1)); }
					else if ( uk == "SLOT" && lua_isstring(Ls, -1) ) { patch.hasSlot = true; patch.slot = lua_tostring(Ls, -1); }
					else if ( uk == "TOOLTIP" && lua_isstring(Ls, -1) ) { patch.hasTooltip = true; patch.tooltip = lua_tostring(Ls, -1); }
					else if ( ( uk == "NAME_IDENTIFIED" || uk == "NAME" ) && lua_isstring(Ls, -1) ) { patch.hasNameId = true; patch.nameIdentified = lua_tostring(Ls, -1); }
					else if ( uk == "NAME_UNIDENTIFIED" && lua_isstring(Ls, -1) ) { patch.hasNameUnid = true; patch.nameUnidentified = lua_tostring(Ls, -1); }
				}
				lua_pop(Ls, 1);
			}
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
		if ( lua_isnumber(Ls, 1) ) { mtype = (int)lua_tointeger(Ls, 1); }
		else if ( lua_isstring(Ls, 1) ) { mtype = samMonsterNameToId(lua_tostring(Ls, 1)); }
		if ( mtype <= 0 || mtype >= NUMMONSTERS ) { SAM_ERROR("LUA", "sam_patch_monster: unknown monster type."); lua_pushboolean(Ls, 0); return 1; }
		int applied = 0;
		if ( lua_istable(Ls, 2) )
		{
			lua_pushnil(Ls);
			while ( lua_next(Ls, 2) != 0 )
			{
				if ( lua_type(Ls, -2) == LUA_TSTRING && lua_isnumber(Ls, -1) )
				{
					const std::string uk = samUpper(lua_tostring(Ls, -2));
					if ( SAMMonsterPatch::set(mtype, uk, (int)lua_tointeger(Ls, -1)) ) { ++applied; }
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

	// ---- custom spells (Session 1): grant a spell to a player -------------------
	// sam_grant_spell(player, "namespace:spell" | vanilla SPELL_ name). Vanilla spells
	// are granted for real via addSpell(id, player, true); a custom spell id is
	// recognized + logged, but its in-engine grant + casting arrive in a later session
	// (no spell_t exists for it yet, so addSpell would assert).
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
		if ( spell.find(':') != std::string::npos )
		{
			// Custom spell — the engine spell_t is built at load, so grant it for real.
			const bool ok = SAMSpells::grantCustomSpell(player, spell);
			lua_pushboolean(Ls, ok ? 1 : 0);
			return 1;
		}
		std::string lower = spell;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		int id = -1;
		for ( const auto& kv : ItemTooltips.spellItems )
		{
			if ( kv.second.internalName == lower ) { id = kv.first; break; }
		}
		if ( id < 0 )
		{
			SAM_ERROR("LUA", "sam_grant_spell: unknown spell '" + spell + "' (expected a SPELL_ name or \"namespace:spell\").");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		const bool ok = addSpell(id, player, true);
		SAM_INFO("SAM", "sam_grant_spell: " + std::string(ok ? "granted" : "not granted (already known or non-local)")
			+ " vanilla spell '" + spell + "' (id " + std::to_string(id) + ") to player " + std::to_string(player) + ".");
		lua_pushboolean(Ls, ok ? 1 : 0);
		return 1;
#else
		(void)player;
		lua_pushboolean(Ls, 0);
		return 1;
#endif
	}

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
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_cast_spell: unknown spell '" + spell + "' (SPELL_ name or \"namespace:spell\")."); lua_pushboolean(Ls, 0); return 1; }
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
		lua_pushcfunction(L, lua_sam_log);
		lua_setglobal(L, "sam_log");

		// Persistent per-mod data (Part 3) — available in every build.
		lua_pushcfunction(L, lua_sam_save_data);
		lua_setglobal(L, "sam_save_data");
		lua_pushcfunction(L, lua_sam_load_data);
		lua_setglobal(L, "sam_load_data");
		lua_pushcfunction(L, lua_sam_delete_data);
		lua_setglobal(L, "sam_delete_data");

		// Timers (Part 4).
		lua_pushcfunction(L, lua_sam_set_timer);
		lua_setglobal(L, "sam_set_timer");
		lua_pushcfunction(L, lua_sam_set_repeating_timer);
		lua_setglobal(L, "sam_set_repeating_timer");
		lua_pushcfunction(L, lua_sam_cancel_timer);
		lua_setglobal(L, "sam_cancel_timer");

		// Custom hooks (Part 2).
		lua_pushcfunction(L, lua_sam_register_hook);
		lua_setglobal(L, "sam_register_hook");
		lua_pushcfunction(L, lua_sam_fire_hook);
		lua_setglobal(L, "sam_fire_hook");
		lua_pushcfunction(L, lua_sam_modify_damage);
		lua_setglobal(L, "sam_modify_damage");
		lua_pushcfunction(L, lua_sam_deal_damage);
		lua_setglobal(L, "sam_deal_damage");
		lua_pushcfunction(L, lua_sam_is_key_held);
		lua_setglobal(L, "sam_is_key_held");
		lua_pushcfunction(L, lua_sam_get_monster_stat);    lua_setglobal(L, "sam_get_monster_stat");
		lua_pushcfunction(L, lua_sam_monster_has_effect);  lua_setglobal(L, "sam_monster_has_effect");
		lua_pushcfunction(L, lua_sam_get_item_category);   lua_setglobal(L, "sam_get_item_category");
		lua_pushcfunction(L, lua_sam_set_monster_stat);    lua_setglobal(L, "sam_set_monster_stat");
		lua_pushcfunction(L, lua_sam_apply_monster_effect); lua_setglobal(L, "sam_apply_monster_effect");
		lua_pushcfunction(L, lua_sam_kill_monster);        lua_setglobal(L, "sam_kill_monster");
		lua_pushcfunction(L, lua_sam_spawn_monsters);      lua_setglobal(L, "sam_spawn_monsters");
		lua_pushcfunction(L, lua_sam_get_monster_target);  lua_setglobal(L, "sam_get_monster_target");
		lua_pushcfunction(L, lua_sam_set_monster_target);  lua_setglobal(L, "sam_set_monster_target");
		lua_pushcfunction(L, lua_sam_get_monster_data);    lua_setglobal(L, "sam_get_monster_data");
		lua_pushcfunction(L, lua_sam_set_monster_data);    lua_setglobal(L, "sam_set_monster_data");
		lua_pushcfunction(L, lua_sam_get_player_data);     lua_setglobal(L, "sam_get_player_data");
		lua_pushcfunction(L, lua_sam_set_player_data);     lua_setglobal(L, "sam_set_player_data");
		lua_pushcfunction(L, lua_sam_get_effect_duration); lua_setglobal(L, "sam_get_effect_duration");
		lua_pushcfunction(L, lua_sam_get_effect_strength); lua_setglobal(L, "sam_get_effect_strength");
		lua_pushcfunction(L, lua_sam_get_effects);         lua_setglobal(L, "sam_get_effects");
		// v0.7.0 Feature 5: modify existing content (revert on unload)
		lua_pushcfunction(L, lua_sam_patch_class);         lua_setglobal(L, "sam_patch_class");
		lua_pushcfunction(L, lua_sam_unpatch_class);       lua_setglobal(L, "sam_unpatch_class");
		lua_pushcfunction(L, lua_sam_patch_item);          lua_setglobal(L, "sam_patch_item");
		lua_pushcfunction(L, lua_sam_patch_monster);       lua_setglobal(L, "sam_patch_monster");
		lua_pushcfunction(L, lua_sam_add_class_passive);   lua_setglobal(L, "sam_add_class_passive");
		lua_pushcfunction(L, lua_sam_remove_class_passive); lua_setglobal(L, "sam_remove_class_passive");
		// Custom spells (Session 1: grant vanilla for real; custom recognized + deferred)
		lua_pushcfunction(L, lua_sam_grant_spell);         lua_setglobal(L, "sam_grant_spell");
		lua_pushcfunction(L, lua_sam_cast_spell);          lua_setglobal(L, "sam_cast_spell");

#ifdef SAM_LUA_HAVE_BARONY
		// Host bindings that actually affect the game (engine build only).
		lua_pushcfunction(L, lua_sam_grant_item);
		lua_setglobal(L, "sam_grant_item");
		lua_pushcfunction(L, lua_sam_grant_gold);
		lua_setglobal(L, "sam_grant_gold");
		lua_pushcfunction(L, lua_sam_apply_effect);
		lua_setglobal(L, "sam_apply_effect");
		lua_pushcfunction(L, lua_sam_remove_effect);
		lua_setglobal(L, "sam_remove_effect");
		lua_pushcfunction(L, lua_sam_get_stat);
		lua_setglobal(L, "sam_get_stat");
		lua_pushcfunction(L, lua_sam_set_stat);
		lua_setglobal(L, "sam_set_stat");
		lua_pushcfunction(L, lua_sam_set_move_speed);
		lua_setglobal(L, "sam_set_move_speed");
		lua_pushcfunction(L, lua_sam_get_move_speed);
		lua_setglobal(L, "sam_get_move_speed");
		lua_pushcfunction(L, lua_sam_add_move_speed);
		lua_setglobal(L, "sam_add_move_speed");
		lua_pushcfunction(L, lua_sam_level_up);
		lua_setglobal(L, "sam_level_up");
		lua_pushcfunction(L, lua_sam_get_floor);
		lua_setglobal(L, "sam_get_floor");
		lua_pushcfunction(L, lua_sam_spawn_item);
		lua_setglobal(L, "sam_spawn_item");
		lua_pushcfunction(L, lua_sam_item_id);
		lua_setglobal(L, "sam_item_id");
		lua_pushcfunction(L, lua_sam_message);
		lua_setglobal(L, "sam_message");
		lua_pushcfunction(L, lua_sam_play_sound);
		lua_setglobal(L, "sam_play_sound");
		lua_pushcfunction(L, lua_sam_get_nearby_entities);
		lua_setglobal(L, "sam_get_nearby_entities");

		// Expanded player queries (Part 5).
		lua_pushcfunction(L, lua_sam_get_equipped_item);
		lua_setglobal(L, "sam_get_equipped_item");
		lua_pushcfunction(L, lua_sam_get_equipped_item_id);
		lua_setglobal(L, "sam_get_equipped_item_id");
		lua_pushcfunction(L, lua_sam_is_defending);
		lua_setglobal(L, "sam_is_defending");
		lua_pushcfunction(L, lua_sam_is_action_held);
		lua_setglobal(L, "sam_is_action_held");
		lua_pushcfunction(L, lua_sam_get_action_binding);
		lua_setglobal(L, "sam_get_action_binding");
		lua_pushcfunction(L, lua_sam_get_inventory_count);
		lua_setglobal(L, "sam_get_inventory_count");
		lua_pushcfunction(L, lua_sam_has_effect);
		lua_setglobal(L, "sam_has_effect");
		lua_pushcfunction(L, lua_sam_get_class);
		lua_setglobal(L, "sam_get_class");
		lua_pushcfunction(L, lua_sam_get_race);
		lua_setglobal(L, "sam_get_race");
		lua_pushcfunction(L, lua_sam_get_kills);
		lua_setglobal(L, "sam_get_kills");
		lua_pushcfunction(L, lua_sam_get_time_played);
		lua_setglobal(L, "sam_get_time_played");

		// v2 world-ops: position / teleport / spawn / inventory.
		lua_pushcfunction(L, lua_sam_get_player_uid);
		lua_setglobal(L, "sam_get_player_uid");
		lua_pushcfunction(L, lua_sam_get_position);
		lua_setglobal(L, "sam_get_position");
		lua_pushcfunction(L, lua_sam_set_position);
		lua_setglobal(L, "sam_set_position");
		lua_pushcfunction(L, lua_sam_spawn_monster);
		lua_setglobal(L, "sam_spawn_monster");
		lua_pushcfunction(L, lua_sam_get_inventory);
		lua_setglobal(L, "sam_get_inventory");
		lua_pushcfunction(L, lua_sam_remove_item);
		lua_setglobal(L, "sam_remove_item");
#endif
	}

	// Build a Lua table { name = ..., <k>=<v>, ... } from an Event and leave it
	// on top of the stack. Copies primitives only — no pointers cross over.
	void pushEventTable(const SAMLua::Event& ev)
	{
		lua_createtable(L, 0, (int)(ev.ints.size() + ev.strings.size() + 1));

		lua_pushstring(L, ev.name.c_str());
		lua_setfield(L, -2, "name");

		for ( const auto& kv : ev.ints )
		{
			lua_pushinteger(L, (lua_Integer)kv.second);
			lua_setfield(L, -2, kv.first.c_str());
		}
		for ( const auto& kv : ev.strings )
		{
			lua_pushstring(L, kv.second.c_str());
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
		lua_getglobal(L, "on_event");
		int eventRef = LUA_NOREF;
		if ( lua_isfunction(L, -1) ) { eventRef = luaL_ref(L, LUA_REGISTRYINDEX); } // pops it
		else { lua_pop(L, 1); }

		lua_getglobal(L, "on_tick");
		int tickRef = LUA_NOREF;
		if ( lua_isfunction(L, -1) ) { tickRef = luaL_ref(L, LUA_REGISTRYINDEX); }
		else { lua_pop(L, 1); }

		// Clear the globals so the next script can't inherit this one's handlers.
		lua_pushnil(L); lua_setglobal(L, "on_event");
		lua_pushnil(L); lua_setglobal(L, "on_tick");

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

	int dispatchEvent(const Event& ev)
	{
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

		int delivered = 0;
		// Preserve the caller's namespace. dispatchEvent can RE-ENTER: a script's on_event
		// may call a host API (sam_apply_effect, sam_fire_hook, ...) that fires another hook,
		// nesting a dispatch inside this one. Restoring (not clearing) g_currentNs keeps the
		// outer script's namespace intact so a later sam_save_data still knows its mod.
		const std::string savedNs = g_currentNs;
		for ( auto& s : g_scripts )
		{
			if ( !s.enabled || s.callbackRef == LUA_NOREF )
			{
				continue;
			}

			lua_rawgeti(L, LUA_REGISTRYINDEX, s.callbackRef); // push on_event
			pushEventTable(ev);                                // push event table arg

			g_currentNs = s.ns;
			const bool ok = protectedCall(1, 0, "on_event('" + ev.name + "') in " + s.path);
			g_currentNs = savedNs;
			if ( ok )
			{
				++delivered;
			}
			else
			{
				// Error isolation: disable ONLY this script; the rest keep running.
				s.enabled = false;
				SAMLogger::noteScriptError();
				SAM_WARN("LUA", "Script '" + s.path + "' disabled after an on_event error.");
			}
		}

		SAMLogger::noteHookFired(delivered); // count + open the GAMEPLAY section on the first hook
		SAM_INFO("LUA", "Dispatched '" + ev.name + "' to " + std::to_string(delivered) + " script(s).");
		return delivered;
	}

	// v0.7.0: fire on_tick(event) for every script that defines it, once per game tick
	// (host-only). Deliberately SILENT — no per-tick log line, no hook count — since this
	// runs ~50x/sec. Only scripts with an on_tick are touched; errors disable just that one.
	void dispatchTick(long long tickCount)
	{
		if ( !L ) { return; }
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
	void beforeDamageModify(int player, long long newValue) { if ( g_bdActive && player == g_bdPlayer ) { g_bdValue = ( newValue < 0 ) ? 0 : newValue; } }
	long long beforeDamageEnd()                             { g_bdActive = false; return g_bdValue; }
	bool beforeDamageActive()                              { return g_bdActive; }

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
		const int player = clientnum;
		for ( const auto& kv : samKeyList() )
		{
			auto it = keystatus.find(kv.second);
			const bool down = ( it != keystatus.end() && it->second );
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
		return Input::inputs[player].binding(action.c_str()); // "" when unbound
#else
		(void)player; (void)action; return "";
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
		if ( !L ) { return; }
		const int player = clientnum;
		if ( player < 0 || player >= MAXPLAYERS ) { return; }

		static std::unordered_map<int, bool> prev; // action index -> last observed state
		const auto& actions = samActionList();
		for ( size_t i = 0; i < actions.size(); ++i )
		{
			// binary() is const: it reads binding_t::binary and never sets `consumed`,
			// which is the only thing that could starve vanilla's own readers. Do NOT
			// switch this to binaryToggle/consume* — blocking and attacking would break.
			const bool down = Input::inputs[player].binary(actions[i].c_str());
			auto prevIt = prev.find((int)i);
			const bool was = ( prevIt != prev.end() ) && prevIt->second;
			if ( down == was ) { continue; }
			prev[(int)i] = down;

			if ( multiplayer == CLIENT )
			{
				// Input is local, but the hooks must fire where host-only APIs work.
				// Forward the edge; the host dispatches it for this player index.
				strcpy((char*)net_packet->data, "SAMA");
				net_packet->data[4] = (Uint8)player;
				net_packet->data[5] = (Uint8)i;
				net_packet->data[6] = down ? 1 : 0;
				net_packet->address.host = net_server.host;
				net_packet->address.port = net_server.port;
				net_packet->len = 7;
				sendPacketSafe(net_sock, -1, net_packet, 0);
			}
			else
			{
				dispatchAction(player, (int)i, down);
			}
		}
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
		for ( const auto& e : samEffectNames ) { if ( e.id == id ) { return e.name; } }
		if ( id >= 135 && id < NUMEFFECTS ) { return "CUSTOM:" + std::to_string(id); }
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
