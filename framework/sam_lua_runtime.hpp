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

	// Bound-ACTION hooks. Unlike pollInput above (which reads raw SDL keycodes and so
	// ignores the player's keybinds entirely), these read Barony's own named actions —
	// "Attack", "Defend", "Use" — so a script reacting to "Use" automatically follows
	// whatever the player rebound it to, and can never collide with a binding because it
	// claims none of its own. Mouse buttons work here; the raw-key path can't see them.
	//
	// STRICTLY READ-ONLY: we only ever call the const Input::binary()/binding(). The
	// consume* calls are non-const and are what would starve vanilla's own readers
	// (blocking, attacking, the hotbar), so they must never appear in this framework.
	//
	// pollActions() runs on EVERY machine (input only exists locally — Input::inputs[]
	// holds live data for local players only). The host dispatches directly; a client
	// forwards each edge to the host via the 'SAMA' packet so the hooks fire host-side,
	// where host-only APIs like sam_cast_spell actually work.
	void pollActions();
	// Fire on_action_pressed/on_action_released for a player. Called by pollActions on
	// the host, and by the 'SAMA' packet handler for a remote client's edges.
	void dispatchAction(int player, int actionIndex, bool pressed);
	// Wire format for 'SAMA' — index into the action table. Returns "" if out of range.
	const char* actionNameForIndex(int index);
	// Backs sam_is_action_held / sam_get_action_binding.
	bool isActionHeld(int player, const std::string& action);
	const char* actionBinding(int player, const std::string& action);

	// Per-player move-speed multiplier (sam_set_move_speed / sam_get_move_speed).
	//
	// Movement is computed by the machine that OWNS the player, so a host-set multiplier
	// for a remote player has to travel to that client's exe or it does nothing at all.
	// setMoveSpeedMult is host-only and syncs via the 'SAMS' packet; applyMoveSpeedMult is
	// the receive side (it stores without re-sending, so there is no echo).
	//
	// Range is [0.1, 3.0]. The ceiling is not a safety limit: the engine already clamps
	// final velocity magnitude to 5.0 unconditionally (actplayer.cpp ~4809), so a
	// multiplier physically cannot carry a player past a speed vanilla itself allows.
	// 3.0 is where the useful range ends — a max-speed character's terminal velocity is
	// ~1.75, so the engine's clamp starts swallowing the difference around 2.8x, while a
	// slow or heavily-laden character still gains the full amount.
	double getMoveSpeedMult(int player);
	void setMoveSpeedMult(int player, double mult);
	void applyMoveSpeedMult(int player, double mult);
	// Drop every multiplier back to 1.0 (call on a new game, so a prior run's speed
	// effects don't leak into the next). Safe before init.
	void resetMoveSpeed();

	// Resolve a status-effect name to its engine id, or -1 if there's no such effect.
	// Accepts any of the engine's named effects ("PARALYZED", "STUNNED", ...), plus a
	// custom slot as a raw number ("135") or "CUSTOM:135".
	//
	// Lives here so the JS runtime shares it instead of keeping a second table. The two
	// WERE separate copies, and they drifted to the same wrong place: 14 names out of the
	// engine's 135, so most real effects were unreachable from either language.
	int effectIdFromName(const char* name);
	std::string effectNameFromId(int id); // reverse: id -> canonical name ("CUSTOM:<id>" for 135..)

	// ---- scripted stat writes: making them real on the client -------------------
	//
	// A raw write to stats[player] only exists on the host. Vanilla never factored its
	// stat-sync packet out — entity.cpp hand-inlines the same 21 bytes at five sites — so
	// there is no engine helper to call and S.A.M has to carry its own. These live here,
	// rather than once per runtime, precisely so the Lua and JS paths cannot drift apart.
	// Both are host-only and no-op for local/splitscreen players, who read stats[] direct.
	void flushStatToClient(int player);   // ATTR: STR/DEX/CON/INT/PER/CHR/EXP/LVL/HP/MAXHP/MP/MAXMP
	void flushGoldToClient(int player);   // GOLD rides its own packet; ATTR has no field for it

	// Bounds a scripted stat write must respect to survive the wire. These are protocol
	// limits, not design choices — see the definitions in the .cpp for why.
	//   STAT_WIRE_MAX: MAXHP/MAXMP go over ATTR as Sint16, so >32767 desyncs permanently.
	//   ATTR_WIRE_MIN: attributes get one byte, and the stock receiver's decode hack reads
	//                  raw 128..248 as positive — so only -7..-1 survive as negatives.
	constexpr int STAT_WIRE_MAX = 32767;
	constexpr int ATTR_WIRE_MIN = -7;

	// v0.7.0 Feature 4 — per-monster scratch data (JSON-string values), shared with the
	// JS runtime through these accessors. Cleared on shutdown.
	void monsterDataSet(unsigned uid, const std::string& key, const std::string& jsonValue);
	std::string monsterDataGet(unsigned uid, const std::string& key);
	void monsterDataClear();

	// v1.2.9 — per-player scratch data (cooldowns/flags/stacks), shared with the JS runtime.
	// In-memory, per session, cleared on new game/shutdown.
	void playerDataSet(int player, const std::string& key, const std::string& jsonValue);
	std::string playerDataGet(int player, const std::string& key);
	void playerDataClear();

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

	// v1.4.0 — floating "companion" entity (a JoJo-style Stand / familiar). A decorative
	// follower that renders a registered custom .vox model, trails its owner player a short
	// distance behind with a gentle hover, and thrusts forward on demand (the punch motion).
	// Backs sam_spawn_companion / sam_companion_punch and lives here (rather than once per
	// runtime) so the Lua and JS paths share ONE behavior pointer and cannot drift.
	//
	// Host-authoritative: only the host runs entity behaviors, and it moves the companion
	// every frame so its position replicates to clients like any mover. All calls no-op
	// (return 0/false) off-host, on an invalid player, for an unregistered model, or in the
	// standalone sandbox that has no engine linked. A companion adds a brand-new behavior
	// function and touches no vanilla code path — pure no-op unless a mod spawns one.
	//   spawnCompanion(player 0..3, "ns:model", scale>0) -> new entity uid, or 0 on failure.
	//   companionPunch(uid) -> false unless uid is a live companion; else starts a thrust.
	unsigned long long spawnCompanion(int player, const std::string& modelId, double scale);
	bool companionPunch(unsigned long long uid);

	// v1.4.0 — sam_get_facing(player) reader. Returns the player's facing yaw in radians,
	// wrapped to [0, 2*PI): 0 = +x (east), increasing toward +y (so the forward unit vector
	// is (cos yaw, sin yaw)). Returns a negative sentinel for an invalid/absent player.
	// Host-authoritative for remote players; a client always sees its own facing correctly.
	double getFacing(int player);

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
