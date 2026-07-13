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
#include "sam_logger.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
#	include "net.hpp"
#	include "mod_tools.hpp" // ItemTooltips.itemNameStringToItemID
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

	// One loaded behavior script.
	struct Script
	{
		std::string path;
		int  callbackRef = LUA_NOREF; // registry ref to its on_event function
		bool enabled     = false;
	};
	std::vector<Script> g_scripts;

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
		armWatchdog();
		const int rc = lua_pcall(L, nargs, nresults, 0);
		disarmWatchdog();

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

		// Resolve "IRON_DAGGER" -> ItemType via Barony's name map (case-insensitive),
		// exactly as the class starting-loadout does.
		std::string lower = itemName;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		auto it = ItemTooltips.itemNameStringToItemID.find(lower);
		if ( it == ItemTooltips.itemNameStringToItemID.end() )
		{
			SAM_ERROR("LUA", "sam_grant_item: unknown item type '" + itemName
				+ "' (expected a vanilla name like \"IRON_DAGGER\") — nothing granted.");
			lua_pushboolean(Ls, 0);
			return 1;
		}
		const ItemType type = static_cast<ItemType>(it->second);

		Item* item = newItem(type, EXCELLENT, 0, 1, 0, true, nullptr);
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
#endif // SAM_LUA_HAVE_BARONY

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

#ifdef SAM_LUA_HAVE_BARONY
		// Host binding that actually affects the game (engine build only).
		lua_pushcfunction(L, lua_sam_grant_item);
		lua_setglobal(L, "sam_grant_item");
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

	bool loadScript(const std::string& path)
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
		// loop or error would occur — the watchdog / pcall contain it.
		if ( !protectedCall(0, 0, "load " + path) )
		{
			SAM_WARN("LUA", "Script '" + path + "' disabled (it failed while running).");
			return false;
		}

		// Capture its on_event handler, if it defined one.
		lua_getglobal(L, "on_event");
		if ( !lua_isfunction(L, -1) )
		{
			lua_pop(L, 1);
			SAM_WARN("LUA", "Script '" + path + "' loaded but defines no on_event(event) — no handler registered.");
			Script s; s.path = path; s.callbackRef = LUA_NOREF; s.enabled = false;
			g_scripts.push_back(s);
			return true;
		}

		const int ref = luaL_ref(L, LUA_REGISTRYINDEX); // pops the function, stores a ref
		Script s; s.path = path; s.callbackRef = ref; s.enabled = true;
		g_scripts.push_back(s);

		// Clear the global so the next script can't accidentally inherit this
		// script's on_event.
		lua_pushnil(L);
		lua_setglobal(L, "on_event");

		SAM_INFO("LUA", "Loaded script '" + path + "' (on_event registered).");
		return true;
	}

	int dispatchEvent(const Event& ev)
	{
		if ( !L )
		{
			SAM_ERROR("LUA", "dispatchEvent('" + ev.name + "') called before init().");
			return 0;
		}

		int delivered = 0;
		for ( auto& s : g_scripts )
		{
			if ( !s.enabled || s.callbackRef == LUA_NOREF )
			{
				continue;
			}

			lua_rawgeti(L, LUA_REGISTRYINDEX, s.callbackRef); // push on_event
			pushEventTable(ev);                                // push event table arg

			if ( protectedCall(1, 0, "on_event('" + ev.name + "') in " + s.path) )
			{
				++delivered;
			}
			else
			{
				// Error isolation: disable ONLY this script; the rest keep running.
				s.enabled = false;
				SAM_WARN("LUA", "Script '" + s.path + "' disabled after an on_event error.");
			}
		}

		SAM_INFO("LUA", "Dispatched '" + ev.name + "' to " + std::to_string(delivered) + " script(s).");
		return delivered;
	}

	void shutdown()
	{
		if ( !L )
		{
			return;
		}
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
