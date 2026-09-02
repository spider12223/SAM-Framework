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
#include "sam_errors.hpp"   // writeFileAtomic

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
#	include "sam_spells.hpp"  // custom-spell registry (sam_grant_spell)
#	include "sam_models.hpp"  // v1.4.0: SAMModels::modelIndexForId (companion custom .vox)
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
		// Resolve a custom "namespace:item" id first, else a vanilla name (case-insensitive),
		// matching sam_grant_item. Without the first tier a mod could not drop its OWN items,
		// which is the main thing scripts spawn.
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
	// Resolve a sound argument: a number is a raw engine id, a string is a mod's own
	// "ns:sound" registered by SAMSounds. Mirrors what sam_play_sound already accepts.
	static int samResolveSoundId(lua_State* Ls, int idx)
	{
		if ( lua_type(Ls, idx) == LUA_TSTRING )
		{
			const char* nm = lua_tostring(Ls, idx);
			return SAMSounds::soundIndexForId(nm ? nm : "");
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
		if ( eff < 0 ) { SAM_WARN("LUA", std::string("sam_has_effect: unknown effect '") + (nameC ? nameC : "") + "'. Valid: " + samEffectNameHint()); lua_pushboolean(Ls, 0); return 1; }
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

	// ---- multiplayer awareness ----------------------------------------------------------
	//
	// Most S.A.M functions are host-only and warn-and-return-false on a client, but until now a
	// script had no way to ASK. So a co-op mod either spammed the log with refusals or guessed.
	// These three are the cheap fix and are safe to call from anywhere.

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
	// told no. Send several small messages, or use sam_save_data for bulk state.

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
				+ " (one datagram). Packet not sent -- split it or use sam_save_data.");
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
		const bool modal = lua_isnoneornil(Ls, 7) ? false : (lua_toboolean(Ls, 7) != 0);
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

	// sam_ui_is_open(panel) -> boolean
	int lua_sam_ui_is_open(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		lua_pushboolean(Ls, SAMUi::isOpen(g_currentNs, panel ? panel : "") ? 1 : 0);
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

	// sam_ui_input_text(panel, id) -> string. What the player has typed so far, without
	// waiting for them to press enter.
	int lua_sam_ui_input_text(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const char* panel = luaL_checkstring(Ls, 1);
		const char* id = luaL_checkstring(Ls, 2);
		lua_pushstring(Ls, SAMUi::inputText(g_currentNs, panel ? panel : "", id ? id : "").c_str());
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
		const bool ents = lua_isnoneornil(Ls, 5) ? false : (lua_toboolean(Ls, 5) != 0);
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
		const bool fly = lua_isnoneornil(Ls, 5) ? false : (lua_toboolean(Ls, 5) != 0);
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
		lua_pushboolean(Ls, SAMWorld::setDoor((uint32_t)uid, lua_toboolean(Ls, 2) != 0) ? 1 : 0);
		return 1;
	}

	int lua_sam_set_door_locked(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		lua_pushboolean(Ls, SAMWorld::setDoorLocked((uint32_t)uid, lua_toboolean(Ls, 2) != 0) ? 1 : 0);
		return 1;
	}

	int lua_sam_power_entity(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const long long uid = (long long)luaL_checkinteger(Ls, 1);
		lua_pushboolean(Ls, SAMWorld::powerEntity((uint32_t)uid, lua_toboolean(Ls, 2) != 0) ? 1 : 0);
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

	static Entity* samEntityFromUid(long long uid)
	{
		return uidToEntity((Sint32)uid);
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
		const bool eff = lua_isnoneornil(Ls, 3) ? true : (lua_toboolean(Ls, 3) != 0);
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
		Entity* e = uidToEntity((Sint32)uid);
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
		if      ( k == "poof" )      { made = spawnPoof(px, py, pz, scale, true); }
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
		Entity* e = uidToEntity((Sint32)uid);
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
	// hook that offers one (XP gained, gold gained, and every future modifiable hook). The
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
		for ( const auto& en : samEffectNames )
		{
			const Uint8 st = s->getEffectActive(en.id);
			if ( st != 0 ) { pushEntry(en.name, en.id, st); }
		}
		for ( int id = 135; id < NUMEFFECTS; ++id )
		{
			const Uint8 st = s->getEffectActive(id);
			if ( st != 0 ) { pushEntry("CUSTOM:" + std::to_string(id), id, st); }
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
	constexpr double SAM_COMPANION_PI          = 3.14159265358979323846;
	constexpr int    SAM_COMPANION_PUNCH_TICKS = 8;      // one forward-thrust window
	constexpr double SAM_COMPANION_BACK        = 18.0;   // idle distance behind the owner (px)
	constexpr double SAM_COMPANION_REACH       = 30.0;   // forward lunge distance on a punch (px)
	constexpr double SAM_COMPANION_RISE        = 6.0;    // float height above owner (z is neg-up)

	// Per-frame follow behavior (host-authoritative). Skill layout — none aliased to any
	// vanilla use (the portal marker is skill[19]==1; a companion is skill[19]==2):
	//   skill[2]  = owner player index (players[skill[2]] convention)
	//   skill[18] = punch ticks remaining (0 = idle behind; >0 = thrusting forward)
	//   skill[19] = 2 (S.A.M companion marker)   fskill[0] = hover-bob phase
	void samCompanionBehavior(Entity* my)
	{
		my->flags[PASSABLE] = true;   // never collide with anything
		my->flags[BRIGHT]   = true;   // full-bright so the Stand reads in a dark dungeon
		const int owner = my->skill[2];
		if ( owner < 0 || owner >= MAXPLAYERS || !players[owner] || !players[owner]->entity )
		{
			// Owner gone (died / descended a level) -> despawn. Self-remove is safe: the
			// entity act loop advances a saved next-node first, exactly as spell missiles do.
			if ( my->mynode ) { list_RemoveNode(my->mynode); }
			return;
		}
		Entity* p = players[owner]->entity;

		double tx, ty, ease;
		if ( my->skill[18] > 0 )
		{
			// Thrust forward (in front of the player) then back — a crisp jab. reach traces
			// 0 -> REACH -> 0 across the punch window so the Stand lunges out and returns.
			const double phase = (double)(SAM_COMPANION_PUNCH_TICKS - my->skill[18])
			                     / (double)SAM_COMPANION_PUNCH_TICKS;
			const double reach = SAM_COMPANION_REACH * std::sin(phase * SAM_COMPANION_PI);
			tx = p->x + reach * std::cos(p->yaw);
			ty = p->y + reach * std::sin(p->yaw);
			ease = 0.6;                 // snap out fast for a punchy jab
			my->skill[18]--;
		}
		else
		{
			// Idle: float a set distance behind the player (yaw + PI = directly behind).
			tx = p->x + SAM_COMPANION_BACK * std::cos(p->yaw + SAM_COMPANION_PI);
			ty = p->y + SAM_COMPANION_BACK * std::sin(p->yaw + SAM_COMPANION_PI);
			ease = 0.25;                // smooth trail
		}
		my->x += (tx - my->x) * ease;
		my->y += (ty - my->y) * ease;
		my->yaw = p->yaw;               // face where the owner faces

		my->fskill[0] += 0.05;          // gentle vertical bob
		my->z = p->z - SAM_COMPANION_RISE + 1.5 * std::sin(my->fskill[0]);
		// Deliberately DON'T set UPDATENEEDED: the companion is host-authoritative and not
		// network-synced (clients never create it — behavior pointers aren't sent), so an
		// ENTU broadcast would only waste bandwidth / risk a behavior-less ghost on clients.
		// Host/SP local rendering doesn't use that flag anyway (the portal omits it too).
	}

	// ---- v2.0 script-owned entity behaviour ------------------------------------------------
	//
	// Slot layout for an entity whose brain is a script (marker 4, alongside portal=1,
	// companion=2, projectile=3):
	//   skill[19] = 4                  S.A.M scripted-behaviour marker
	//   skill[18] = behaviour index     into g_behaviors
	//   skill[17] = owning player, or -1
	// skill[0..15] are left entirely to the script as per-entity scratch, which is why the
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
	//   skill[2]  = owner player index, or -1 for an unowned/monster shot
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
		if ( multiplayer != SINGLE ) { lua_pushboolean(Ls, 0); return 1; }
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
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_remove_entity refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = uidToEntity((Sint32)uid);
		if ( !e ) { lua_pushboolean(Ls, 0); return 1; }
		if ( e->behavior == &actPlayer ) { SAM_WARN("LUA", "sam_remove_entity refused: cannot remove a player."); lua_pushboolean(Ls, 0); return 1; }
		// A chest that is open right now is ALSO stored in openedChest[], and neither
		// list_RemoveNode nor ~Entity clears that array -- so deleting the entity here left
		// a dangling pointer behind for the still-open chest UI to read on its next frame.
		// Reachable without trying: a script tidying up its hub while a player is standing
		// in it with the chest open. closeChest() is itself guarded by `if (chestStatus)`,
		// so this is a no-op for a chest nobody has open.
		if ( e->behavior == &actChest ) { e->closeChest(); }
		e->removeLightField();              // drop any light it owns (e.g. a decorative portal)
		if ( e->mynode ) { list_RemoveNode(e->mynode); }
		lua_pushboolean(Ls, 1);
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
		const bool on = lua_isnoneornil(Ls, 2) ? true : lua_toboolean(Ls, 2) != 0;
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_set_chest_stash refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		Entity* e = uidToEntity((Sint32)uid);
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
			secret = lua_toboolean(Ls, -1) != 0;
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

#ifdef SAM_LUA_HAVE_BARONY
	// v1.5.0: resolve a spell reference (vanilla internalName or custom "ns:spell") to an engine id, or -1.
	static int samResolveSpellId(const std::string& spell)
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
		Entity* target = uidToEntity((Sint32)targetUid);
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

	// sam_get_spells(player) -> array of spell internal-name strings the player knows.
	int lua_sam_get_spells(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		lua_newtable(Ls);
#ifdef SAM_LUA_HAVE_BARONY
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return 1; }
		int n = 0;
		for ( node_t* node = players[player]->magic.spellList.first; node; node = node->next )
		{
			spell_t* sp = (spell_t*)node->element;
			if ( sp ) { lua_pushstring(Ls, sp->spell_internal_name); lua_rawseti(Ls, -2, ++n); }
		}
		return 1;
#else
		(void)player; return 1;
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
		const int id = samResolveSpellId(spell);
		if ( id < 0 ) { lua_pushboolean(Ls, 0); return 1; }
		bool known = false;
		for ( node_t* node = players[player]->magic.spellList.first; node; node = node->next )
		{
			spell_t* sp = (spell_t*)node->element;
			if ( sp && sp->ID == id ) { known = true; break; }
		}
		lua_pushboolean(Ls, known ? 1 : 0);
		return 1;
#else
		(void)player; (void)spell; lua_pushboolean(Ls, 0); return 1;
#endif
	}

	// sam_remove_spell(player, spell) -> bool. Un-learn a spell (local player's list). Host-only.
	int lua_sam_remove_spell(lua_State* Ls)
	{
		SAMLogger::noteApiCall();
		const int player = (int)luaL_checkinteger(Ls, 1);
		const char* spellC = luaL_checkstring(Ls, 2);
		const std::string spell = spellC ? spellC : "";
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("LUA", "sam_remove_spell refused: host only."); lua_pushboolean(Ls, 0); return 1; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { lua_pushboolean(Ls, 0); return 1; }
		const int id = samResolveSpellId(spell);
		if ( id < 0 ) { SAM_ERROR("LUA", "sam_remove_spell: unknown spell '" + spell + "'."); lua_pushboolean(Ls, 0); return 1; }
		for ( node_t* node = players[player]->magic.spellList.first; node; )
		{
			node_t* next = node->next;
			spell_t* sp = (spell_t*)node->element;
			if ( sp && sp->ID == id ) { list_RemoveNode(node); lua_pushboolean(Ls, 1); return 1; }
			node = next;
		}
		lua_pushboolean(Ls, 0);
		return 1;
#else
		(void)player; (void)spell; lua_pushboolean(Ls, 0); return 1;
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
		lua_pushcfunction(L, lua_sam_modify_monster_damage);
		lua_setglobal(L, "sam_modify_monster_damage");
		lua_pushcfunction(L, lua_sam_modify_value);
		lua_setglobal(L, "sam_modify_value");
		lua_pushcfunction(L, lua_sam_deal_damage);
		lua_setglobal(L, "sam_deal_damage");
		lua_pushcfunction(L, lua_sam_is_key_held);
		lua_setglobal(L, "sam_is_key_held");
		lua_pushcfunction(L, lua_sam_get_monster_stat);    lua_setglobal(L, "sam_get_monster_stat");
		lua_pushcfunction(L, lua_sam_monster_path_to);     lua_setglobal(L, "sam_monster_path_to");
		lua_pushcfunction(L, lua_sam_monster_face);        lua_setglobal(L, "sam_monster_face");
		lua_pushcfunction(L, lua_sam_monster_attack);      lua_setglobal(L, "sam_monster_attack");
		lua_pushcfunction(L, lua_sam_monster_charge);      lua_setglobal(L, "sam_monster_charge");
		lua_pushcfunction(L, lua_sam_get_monster_type);    lua_setglobal(L, "sam_get_monster_type");
		lua_pushcfunction(L, lua_sam_get_monster_name);    lua_setglobal(L, "sam_get_monster_name");
		lua_pushcfunction(L, lua_sam_monster_has_effect);  lua_setglobal(L, "sam_monster_has_effect");
		lua_pushcfunction(L, lua_sam_monster_has_trait);   lua_setglobal(L, "sam_monster_has_trait");
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
		// v1.5.0 monster status-effect read/remove parity
		lua_pushcfunction(L, lua_sam_remove_monster_effect);       lua_setglobal(L, "sam_remove_monster_effect");
		lua_pushcfunction(L, lua_sam_get_monster_effect_duration); lua_setglobal(L, "sam_get_monster_effect_duration");
		lua_pushcfunction(L, lua_sam_get_monster_effect_strength); lua_setglobal(L, "sam_get_monster_effect_strength");
		lua_pushcfunction(L, lua_sam_get_monster_effects);         lua_setglobal(L, "sam_get_monster_effects");
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
		// v1.5.0 spell freedom: aimed cast, monster cast, query + remove
		lua_pushcfunction(L, lua_sam_cast_spell_at);       lua_setglobal(L, "sam_cast_spell_at");
		lua_pushcfunction(L, lua_sam_cast_spell_pos);      lua_setglobal(L, "sam_cast_spell_pos");
		lua_pushcfunction(L, lua_sam_monster_cast_spell);  lua_setglobal(L, "sam_monster_cast_spell");
		lua_pushcfunction(L, lua_sam_get_spells);          lua_setglobal(L, "sam_get_spells");
		lua_pushcfunction(L, lua_sam_player_knows_spell);  lua_setglobal(L, "sam_player_knows_spell");
		lua_pushcfunction(L, lua_sam_remove_spell);        lua_setglobal(L, "sam_remove_spell");

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
		// v1.5.0 player effect control
		lua_pushcfunction(L, lua_sam_clear_effects);       lua_setglobal(L, "sam_clear_effects");
		lua_pushcfunction(L, lua_sam_set_effect_duration); lua_setglobal(L, "sam_set_effect_duration");
		lua_pushcfunction(L, lua_sam_set_effect_strength); lua_setglobal(L, "sam_set_effect_strength");
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
		lua_pushcfunction(L, lua_sam_is_host);             lua_setglobal(L, "sam_is_host");
		lua_pushcfunction(L, lua_sam_play_sound_at);     lua_setglobal(L, "sam_play_sound_at");
		lua_pushcfunction(L, lua_sam_play_sound_entity); lua_setglobal(L, "sam_play_sound_entity");
		lua_pushcfunction(L, lua_sam_spawn_particle);    lua_setglobal(L, "sam_spawn_particle");
		lua_pushcfunction(L, lua_sam_damage_number);     lua_setglobal(L, "sam_damage_number");
		lua_pushcfunction(L, lua_sam_get_effective_stat); lua_setglobal(L, "sam_get_effective_stat");
		lua_pushcfunction(L, lua_sam_get_ac);            lua_setglobal(L, "sam_get_ac");
		lua_pushcfunction(L, lua_sam_get_skill);         lua_setglobal(L, "sam_get_skill");
		lua_pushcfunction(L, lua_sam_is_enemy);          lua_setglobal(L, "sam_is_enemy");
		lua_pushcfunction(L, lua_sam_is_friend);         lua_setglobal(L, "sam_is_friend");
		lua_pushcfunction(L, lua_sam_get_mods);          lua_setglobal(L, "sam_get_mods");
		lua_pushcfunction(L, lua_sam_is_mod_loaded);     lua_setglobal(L, "sam_is_mod_loaded");
		lua_pushcfunction(L, lua_sam_get_tile);          lua_setglobal(L, "sam_get_tile");
		lua_pushcfunction(L, lua_sam_set_tile);          lua_setglobal(L, "sam_set_tile");
		lua_pushcfunction(L, lua_sam_is_spawnable);      lua_setglobal(L, "sam_is_spawnable");
		lua_pushcfunction(L, lua_sam_line_of_sight);     lua_setglobal(L, "sam_line_of_sight");
		lua_pushcfunction(L, lua_sam_tiles_connected);   lua_setglobal(L, "sam_tiles_connected");
		lua_pushcfunction(L, lua_sam_get_light_at);      lua_setglobal(L, "sam_get_light_at");
		lua_pushcfunction(L, lua_sam_find_entities);     lua_setglobal(L, "sam_find_entities");
		lua_pushcfunction(L, lua_sam_get_container_items); lua_setglobal(L, "sam_get_container_items");
		lua_pushcfunction(L, lua_sam_set_door);          lua_setglobal(L, "sam_set_door");
		lua_pushcfunction(L, lua_sam_set_door_locked);   lua_setglobal(L, "sam_set_door_locked");
		lua_pushcfunction(L, lua_sam_power_entity);      lua_setglobal(L, "sam_power_entity");
		lua_pushcfunction(L, lua_sam_toggle_switch);     lua_setglobal(L, "sam_toggle_switch");
		lua_pushcfunction(L, lua_sam_get_level_info);    lua_setglobal(L, "sam_get_level_info");
		lua_pushcfunction(L, lua_sam_hud_text);            lua_setglobal(L, "sam_hud_text");
		lua_pushcfunction(L, lua_sam_hud_bar);             lua_setglobal(L, "sam_hud_bar");
		lua_pushcfunction(L, lua_sam_hud_clear);           lua_setglobal(L, "sam_hud_clear");
		// v1.10.3 -- the mod's own pictures (overlay + HUD art).
		lua_pushcfunction(L, lua_sam_show_image);          lua_setglobal(L, "sam_show_image");
		lua_pushcfunction(L, lua_sam_show_image_at);       lua_setglobal(L, "sam_show_image_at");
		lua_pushcfunction(L, lua_sam_hide_image);          lua_setglobal(L, "sam_hide_image");
		lua_pushcfunction(L, lua_sam_hud_image);           lua_setglobal(L, "sam_hud_image");
		lua_pushcfunction(L, lua_sam_get_image_size);      lua_setglobal(L, "sam_get_image_size");
		// v1.11.0 -- interactive panels.
		lua_pushcfunction(L, lua_sam_ui_open);             lua_setglobal(L, "sam_ui_open");
		lua_pushcfunction(L, lua_sam_ui_close);            lua_setglobal(L, "sam_ui_close");
		lua_pushcfunction(L, lua_sam_ui_is_open);          lua_setglobal(L, "sam_ui_is_open");
		lua_pushcfunction(L, lua_sam_ui_clear);            lua_setglobal(L, "sam_ui_clear");
		lua_pushcfunction(L, lua_sam_ui_label);            lua_setglobal(L, "sam_ui_label");
		lua_pushcfunction(L, lua_sam_ui_button);           lua_setglobal(L, "sam_ui_button");
		lua_pushcfunction(L, lua_sam_ui_image);            lua_setglobal(L, "sam_ui_image");
		lua_pushcfunction(L, lua_sam_ui_list);             lua_setglobal(L, "sam_ui_list");
		lua_pushcfunction(L, lua_sam_ui_list_add);         lua_setglobal(L, "sam_ui_list_add");
		lua_pushcfunction(L, lua_sam_ui_list_clear);       lua_setglobal(L, "sam_ui_list_clear");
		lua_pushcfunction(L, lua_sam_ui_input);            lua_setglobal(L, "sam_ui_input");
		lua_pushcfunction(L, lua_sam_ui_input_text);       lua_setglobal(L, "sam_ui_input_text");
		lua_pushcfunction(L, lua_sam_ui_panel_style);      lua_setglobal(L, "sam_ui_panel_style");
		lua_pushcfunction(L, lua_sam_ui_font);             lua_setglobal(L, "sam_ui_font");
		lua_pushcfunction(L, lua_sam_ui_list_row_height);  lua_setglobal(L, "sam_ui_list_row_height");
		lua_pushcfunction(L, lua_sam_ui_text_size);        lua_setglobal(L, "sam_ui_text_size");
		// v1.11.0 -- reading the game's own content.
		lua_pushcfunction(L, lua_sam_list_items);          lua_setglobal(L, "sam_list_items");
		lua_pushcfunction(L, lua_sam_get_item_info);       lua_setglobal(L, "sam_get_item_info");
		lua_pushcfunction(L, lua_sam_list_monsters);       lua_setglobal(L, "sam_list_monsters");
		lua_pushcfunction(L, lua_sam_list_spells);         lua_setglobal(L, "sam_list_spells");
		lua_pushcfunction(L, lua_sam_spawn_projectile);    lua_setglobal(L, "sam_spawn_projectile");
		lua_pushcfunction(L, lua_sam_send_packet);         lua_setglobal(L, "sam_send_packet");
		lua_pushcfunction(L, lua_sam_player_count);        lua_setglobal(L, "sam_player_count");
		lua_pushcfunction(L, lua_sam_local_player);        lua_setglobal(L, "sam_local_player");
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
		lua_pushcfunction(L, lua_sam_spawn_portal);
		lua_setglobal(L, "sam_spawn_portal");
		lua_pushcfunction(L, lua_sam_remove_entity);
		lua_setglobal(L, "sam_remove_entity");
		lua_pushcfunction(L, lua_sam_set_entity_facing);
		lua_setglobal(L, "sam_set_entity_facing");
		lua_pushcfunction(L, lua_sam_look_at);
		lua_setglobal(L, "sam_look_at");
		lua_pushcfunction(L, lua_sam_get_entity_facing);
		lua_setglobal(L, "sam_get_entity_facing");
		lua_pushcfunction(L, lua_sam_register_behavior);
		lua_setglobal(L, "sam_register_behavior");
		lua_pushcfunction(L, lua_sam_spawn_entity);
		lua_setglobal(L, "sam_spawn_entity");
		lua_pushcfunction(L, lua_sam_set_chest_stash);
		lua_setglobal(L, "sam_set_chest_stash");
		lua_pushcfunction(L, lua_sam_travel_to_level);
		lua_setglobal(L, "sam_travel_to_level");
		lua_pushcfunction(L, lua_sam_world_save);
		lua_setglobal(L, "sam_world_save");
		lua_pushcfunction(L, lua_sam_world_load);
		lua_setglobal(L, "sam_world_load");
		lua_pushcfunction(L, lua_sam_world_clear);
		lua_setglobal(L, "sam_world_clear");
		lua_pushcfunction(L, lua_sam_world_keys);
		lua_setglobal(L, "sam_world_keys");
		lua_pushcfunction(L, lua_sam_get_inventory);
		lua_setglobal(L, "sam_get_inventory");
		lua_pushcfunction(L, lua_sam_remove_item);
		lua_setglobal(L, "sam_remove_item");
		// v1.4.0 — floating companion ("Stand") + facing reader.
		lua_pushcfunction(L, lua_sam_spawn_companion);
		lua_setglobal(L, "sam_spawn_companion");
		lua_pushcfunction(L, lua_sam_companion_punch);
		lua_setglobal(L, "sam_companion_punch");
		lua_pushcfunction(L, lua_sam_get_facing);
		lua_setglobal(L, "sam_get_facing");
		// v1.6.0 — impact frame: screen flash / manga burst / camera shake / hitstop.
		lua_pushcfunction(L, lua_sam_screen_flash);
		lua_setglobal(L, "sam_screen_flash");
		lua_pushcfunction(L, lua_sam_impact_frame);
		lua_setglobal(L, "sam_impact_frame");
		lua_pushcfunction(L, lua_sam_camera_shake);
		lua_setglobal(L, "sam_camera_shake");
		lua_pushcfunction(L, lua_sam_hitstop);
		lua_setglobal(L, "sam_hitstop");
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
			lua_pushstring(L, ( it == g_lastEventStrings.end() ) ? kv.second.c_str() : it->second.c_str());
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
					const char* c = lua_tostring(L, -1);
					auto it = g_lastEventStrings.find(kv.first);
					const std::string seen = ( it == g_lastEventStrings.end() ) ? kv.second : it->second;
					if ( c && seen != c ) { g_lastEventStrings[kv.first] = c; }
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
		// The entity is already flagged UPDATENEEDED, and ENTU carries yaw, so clients pick
		// this up on the next entity update without a bespoke packet.
		e->flags[UPDATENEEDED] = true;
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
		Entity* e = uidToEntity((Sint32)uid);
		return e ? (double)e->yaw : -1.0;
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
			sprite = SAMModels::modelIndexForId(modelId);
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

	bool sendModPacket(int target, const std::string& tag, const std::string& payload)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == SINGLE ) { return false; }
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

	// ---- v1.4.0 floating companion ("Stand") — shared by both runtimes ----------
	// The behavior function samCompanionBehavior + its constants live in the anonymous
	// namespace above (they need Barony types); these public entry points just drive them.

	unsigned long long spawnCompanion(int player, const std::string& modelId, double scale)
	{
#ifdef SAM_LUA_HAVE_BARONY
		if ( multiplayer == CLIENT ) { SAM_WARN("SAM", "spawnCompanion refused: host only."); return 0; }
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->entity )
		{ SAM_ERROR("SAM", "spawnCompanion: invalid player index " + std::to_string(player) + "."); return 0; }
		const int modelIdx = SAMModels::modelIndexForId(modelId);
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
		e->skill[2]  = player;                          // owner index (players[skill[2]] convention)
		e->skill[18] = 0;                               // not punching
		e->skill[19] = 2;                               // S.A.M companion marker (portal uses 1)
		e->fskill[0] = 0.0;                             // hover-bob phase
		SAM_INFO("SAM", "spawnCompanion: model '" + modelId + "' for player " + std::to_string(player)
		                + " uid " + std::to_string(e->getUID()));
		return (unsigned long long)e->getUID();
#else
		(void)player; (void)modelId; (void)scale; return 0;
#endif
	}

	unsigned long long spawnProjectile(int owner, double tileX, double tileY, double angle,
		double speed, int damage, int lifetimeTicks, const std::string& modelId)
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
			sprite = SAMModels::modelIndexForId(modelId);
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
		e->skill[2] = ( owner >= 0 && owner < MAXPLAYERS ) ? owner : -1;
		e->skill[17] = damage < 0 ? 0 : damage;
		e->skill[18] = life;
		e->fskill[2] = cos(angle) * speed;
		e->fskill[3] = sin(angle) * speed;
		// parent is what fired it, so the shot cannot kill its owner on frame one.
		if ( owner >= 0 && owner < MAXPLAYERS && players[owner] && players[owner]->entity )
		{
			e->parent = players[owner]->entity->getUID();
		}
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
		else if ( player > 0 && multiplayer == SERVER )
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
