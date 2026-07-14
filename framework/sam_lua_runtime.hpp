/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_lua_runtime.hpp
	Desc: sandboxed Lua 5.4 scripting runtime for S.A.M behavior scripts.

	DESIGN CONTRACT (non-negotiable):
	  1. Lua library is vanilla Lua 5.4 (vendored, no vcpkg DLL).
	  2. Lua NEVER receives a raw Entity or Item pointer. Events carry only
	     copied primitives — numeric UIDs, integers, strings. This is what
	     makes the runtime immune to use-after-free from freed game objects.
	  3. Every Lua state is sandboxed: hard memory cap, per-callback
	     instruction budget enforced by a count watchdog, dangerous stdlib
	     functions stripped, and a script error only disables THAT script —
	     it never crashes the host.
	  4. (Integration-time) hooks fire host-authoritative only; this PoC file
	     is engine-agnostic and only knows about the Event struct.

	This header has no dependency on Barony — only the C++ std lib and (in the
	.cpp) the vendored Lua headers and SAMLogger. It is drop-in for the real
	SAM build later.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstddef>
#include <cstdint>

namespace SAMLua
{
	// An event handed from C++ to a Lua script. It is a bag of COPIED primitives
	// only — there is deliberately no way to store an Entity/Item pointer here. When
	// dispatched it becomes a plain Lua table: { name = <name>, <k> = <v>, ... }.
	struct Event
	{
		std::string name; // e.g. "player.on_level_up"
		std::vector<std::pair<std::string, long long>>   ints;    // integer fields (UIDs, amounts, ...)
		std::vector<std::pair<std::string, std::string>> strings; // string fields

		Event& setName(const std::string& n) { name = n; return *this; }
		Event& i(const std::string& key, long long v) { ints.emplace_back(key, v); return *this; }
		Event& s(const std::string& key, const std::string& v) { strings.emplace_back(key, v); return *this; }
	};

	// Sandbox limits — enforced per Lua state and (for the instruction budget)
	// reset per callback invocation.
	struct SandboxConfig
	{
		std::size_t memoryCapBytes   = 10u * 1024u * 1024u; // 10 MB hard allocator cap
		long long   instructionBudget = 500000;             // per-callback instruction budget
		int         watchdogInterval  = 1000;               // count hook fires every N instructions
	};

	// Create the VM and install the sandbox. Returns false on failure (e.g. OOM).
	bool init(const SandboxConfig& cfg = SandboxConfig{});

	// Parse + run a script file, then capture its global on_event function (if
	// any). A top-level infinite loop / error here is caught by the sandbox and
	// the script is simply not enabled — the host is never harmed. Returns true
	// if the script loaded and ran to completion (regardless of whether it
	// defined on_event).
	bool loadScript(const std::string& path, const std::string& modNamespace = "");

	// Call every enabled script's on_event(event) with a fresh Lua table built
	// from `ev`. A script that errors (or blows its instruction/memory budget)
	// is disabled; the others still run. Returns the number of scripts the event
	// was successfully delivered to.
	int dispatchEvent(const Event& ev);

	// v0.7.0: call on_tick(event) for every script that defines it, once per game
	// tick (host-authoritative, silent — no per-tick logging). `tickCount` is the
	// current per-game tick.
	void dispatchTick(long long tickCount);

	// v0.7.0 Feature 2 — damage interception. Entity::modHP brackets the
	// on_before_damage dispatch with beforeDamageBegin/End; scripts rewrite the
	// incoming value via beforeDamageModify (sam_modify_damage). One shared latch is
	// used by both runtimes and the engine, so these live on SAMLua.
	void beforeDamageBegin(int player, long long damage);
	void beforeDamageModify(int player, long long newValue);
	long long beforeDamageEnd();
	bool beforeDamageActive();

	// v0.7.0 Feature 3 — input hooks. pollInput() is called once per game tick (host +
	// gameplay) to fire on_key_pressed / on_key_released on key-state transitions;
	// isKeyHeld backs sam_is_key_held. Supported keys: A-Z, 0-9, F1-F12.
	void pollInput();
	bool isKeyHeld(const std::string& name);

	// v0.7.0 Feature 4 — per-monster scratch data (JSON-string values), shared with the
	// JS runtime through these accessors. Cleared on shutdown.
	void monsterDataSet(unsigned uid, const std::string& key, const std::string& jsonValue);
	std::string monsterDataGet(unsigned uid, const std::string& key);
	void monsterDataClear();

	// Advance + fire any due per-script timers (Part 4). Call once per game tick,
	// host-authoritative only.
	void tickTimers();

	// Drop all pending timers (call on a new game so a prior run's timers don't leak
	// into the next). Safe before init.
	void resetTimers();

	// Part 5 session kill counter (Barony has no per-player one). noteKill is called
	// from the on_kill hook; resetKills on a new game; getKills backs sam_get_kills.
	void noteKill(int player);
	long long getKills(int player);
	void resetKills();

	// Tear down the VM and release all script references.
	void shutdown();

	// ---- introspection (for tests / diagnostics) -------------------------------
	std::size_t scriptCount();          // total scripts loaded
	std::size_t enabledScriptCount();   // scripts still enabled (not disabled by an error)
	std::size_t memoryUsedBytes();      // current Lua allocation
	std::size_t memoryPeakBytes();      // high-water mark
	bool isInitialized();

	// Read back a global set by a script — used by the test harness to confirm a
	// script actually observed an event. Safe: only copies primitives out.
	bool getGlobalInt(const std::string& name, long long& out);
	bool getGlobalString(const std::string& name, std::string& out);

} // namespace SAMLua
