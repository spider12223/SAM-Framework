/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_js_runtime.hpp
	Desc: sandboxed JavaScript + TypeScript scripting runtime, built on quickjs-ng.

	This is the JS/TS sibling of sam_lua_runtime — SAME contract, SAME sandbox,
	SAME event/API surface, so the loader can treat .lua / .js / .ts uniformly:

	  1. Engine is quickjs-ng (vendored amalgam, MIT), no JIT, no libc/os/fs.
	  2. JS NEVER receives a raw Entity/Item pointer. Events carry only COPIED
	     primitives — numeric UIDs, integers, strings.
	  3. Every script is sandboxed: a hard memory cap, a per-callback wall-clock
	     watchdog that kills infinite loops, only pure intrinsics in the global
	     (no filesystem/network/os), and per-script error isolation — a broken
	     script disables only itself, never the host.
	  4. TypeScript: a .ts file is transpiled to JS ONCE at load (typescript.js
	     running under a privileged QuickJS context), cached by content hash, and
	     the resulting JS runs in the same hardened sandbox as a .js script.

	Engine-agnostic like the Lua header: no Barony dependency here; the .cpp adds
	the vendored QuickJS headers and (in the engine build) the SAM host bindings.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstddef>

namespace SAMJs
{
	// An event handed from C++ to a script — COPIED primitives only, never a
	// pointer. Identical shape to SAMLua::Event so events map 1:1 across engines.
	// Becomes a plain JS object: { name: <name>, <k>: <v>, ... }.
	struct Event
	{
		std::string name;
		std::vector<std::pair<std::string, long long>>   ints;
		std::vector<std::pair<std::string, std::string>> strings;

		Event& setName(const std::string& n) { name = n; return *this; }
		Event& i(const std::string& key, long long v) { ints.emplace_back(key, v); return *this; }
		Event& s(const std::string& key, const std::string& v) { strings.emplace_back(key, v); return *this; }
	};

	// Sandbox limits — enforced per runtime and (for the watchdog) per callback.
	struct SandboxConfig
	{
		std::size_t memoryCapBytes = 10u * 1024u * 1024u; // 10 MB hard cap (JS_SetMemoryLimit)
		std::size_t maxStackBytes  = 512u * 1024u;        // native C-stack guard (JS_SetMaxStackSize)
		long long   callbackBudgetMs = 250;               // per-callback wall-clock deadline (watchdog)
		long long   transpileBudgetMs = 8000;             // relaxed deadline for the one-time TS compile
	};

	// Create the QuickJS runtime + sandbox. Returns false on failure.
	bool init(const SandboxConfig& cfg = SandboxConfig{});

	// Load + run a JavaScript script (defines a global on_event(event)). Runs in
	// the hardened per-script sandbox. Returns true if it loaded and ran.
	bool loadScriptJS(const std::string& path, const std::string& modNamespace = "");

	// Load a TypeScript script: transpile .ts -> .js (cached), then load the JS.
	// cacheDir is where compiled .js is written (must be a writable real-FS dir,
	// NOT the read-only mod folder). tsCompilerJsPath points at the vendored
	// typescript.js. Returns true if it transpiled + loaded.
	bool loadScriptTS(const std::string& path, const std::string& cacheDir, const std::string& tsCompilerJsPath, const std::string& modNamespace = "");

	// Call every enabled script's on_event(event) with a fresh JS object built
	// from `ev`. A script that errors / blows its budget is disabled; others run.
	// Returns the number of scripts the event reached.
	int dispatchEvent(const Event& ev);

	// Tear down the runtime and release all script references.
	// Advance + fire due per-script timers (Part 4). Call once per game tick, host only.
	void tickTimers();

	// Drop all pending timers (call on a new game so a prior run's timers don't leak
	// into the next). Safe before init.
	void resetTimers();

	void shutdown();

	// Free the privileged TypeScript transpile runtime (and its ~9MB compiler).
	// Call after all .ts files are loaded so the compiler isn't resident during
	// gameplay; ensureTranspiler() lazily recreates it if another .ts loads.
	// shutdown() also frees it, so calling both is safe.
	void releaseTranspiler();

	// ---- introspection (for tests / diagnostics) -----------------------------
	std::size_t scriptCount();
	std::size_t enabledScriptCount();
	bool isInitialized();

	// Read back a global set by a script — used by the test harness to confirm a
	// script actually observed an event (copies primitives out only).
	bool getGlobalInt(const std::string& name, long long& out);
	bool getGlobalString(const std::string& name, std::string& out);

} // namespace SAMJs
