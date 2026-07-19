/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_logger.hpp
	Desc: console + file logger for the S.A.M Barony modding framework.
	      This is the first system built — every other S.A.M system depends
	      on it, so it has no dependency on any other S.A.M file and only a
	      minimal dependency on the C++ standard library.

	v0.7.0 — structured session logs: a boxed session header, phase sections
	(INIT / MOD LOAD / GAMEPLAY), a load-summary and session-summary block,
	5-session rotation, a persistent session counter, ERROR "!!!" highlighting,
	and session-relative timestamps during gameplay.

	Usage:
	    SAMLogger::init(outputdir);            // once, from initApp()
	    SAM_INFO("CORE", "S.A.M initializing...");
	    SAM_WARN("ITEMS", "No model found, using fallback");
	    SAM_ERROR("SYNC", "Mod list mismatch");
	    SAM_DEBUG("LOADER", "verbose detail");  // only shown when debug mode on

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>

// S.A.M framework version — stamped into the session banner in the log file.
#define SAM_FRAMEWORK_VERSION "1.3.4"

// Barony release this build is patched against — shown in the session banner.
#define SAM_BARONY_TARGET "5.0.2"

// NOTE: members are CamelCase on purpose. Barony's src/game.hpp does
// `#define DEBUG 1` and <windows.h> (wingdi.h) does `#define ERROR 0`, so
// all-caps enum members (DEBUG/ERROR) get macro-expanded and break the enum
// when this header is included inside a Barony translation unit. CamelCase
// names can't collide with those all-caps macros.
enum class SAMLogLevel
{
	Info,
	Warn,
	Error,
	Debug
};

// Counts handed to logLoadSummary() after all mods finish loading. The loader
// fills these from its registries; the logger owns only the presentation.
struct SAMLoadStats
{
	int mods = 0;
	int classesRegistered = 0, classesDeclared = 0;
	int itemsRegistered = 0,   itemsDeclared = 0;
	int monstersRegistered = 0, monstersDeclared = 0;
	int spawnLevels = 0;
	int scriptsLua = 0, scriptsJs = 0; // scriptsJs counts .js + .ts
	int patchOps = 0,   patchFiles = 0;
	int plugins = 0;
};

class SAMLogger
{
public:
	// Opens <outputDir>/sam_log.txt, rotates old sessions (keeps the most recent
	// 5), bumps the persistent session counter (<outputDir>/sam_session.txt),
	// enables ANSI colour on Windows consoles, and writes the boxed session
	// header followed by the INIT section. Idempotent. If the log file cannot be
	// opened, logging still proceeds to stdout.
	static void init(const std::string& outputDir, bool debugModeEnabled = false);

	// Closes the log file. Optional — mainly for a clean standalone test run.
	static void shutdown();

	// Core logging entry point. DEBUG messages are suppressed unless debug
	// mode is enabled. Thread-safe.
	static void log(SAMLogLevel level, const std::string& module, const std::string& message);

	// Convenience wrappers around log().
	static void info(const std::string& module, const std::string& message);
	static void warn(const std::string& module, const std::string& message);
	static void error(const std::string& module, const std::string& message);
	static void debug(const std::string& module, const std::string& message);

	// ---- structured sections (v0.7.0) -----------------------------------------

	// Print a "── NAME ─────" phase divider. Used for INIT / MOD LOAD / GAMEPLAY.
	static void beginSection(const std::string& name);

	// Mark the start of mod loading (opens the MOD LOAD section + starts the
	// load-time clock). Called by the loader before it scans mods.
	static void beginModLoad();

	// Print the LOAD SUMMARY block from the loader's final counts. Ends the load
	// clock. Called once, after all mods are registered.
	static void logLoadSummary(const SAMLoadStats& stats);

	// Print the SESSION SUMMARY block + closing box. Called from the loader's
	// unload path when SAM tears down (Mods::unloadMods).
	static void logSessionSummary();

	// ---- gameplay counters (v0.7.0) -------------------------------------------

	// A hook was dispatched to `scriptsReached` scripts. The FIRST call opens the
	// GAMEPLAY section. Called from each runtime's dispatchEvent().
	static void noteHookFired(int scriptsReached);
	// A host API function was invoked (from a script). Called by sam_* bindings.
	static void noteApiCall();
	// A script was disabled by an on_event error. Called from dispatchEvent().
	static void noteScriptError();

	// Legacy rule/separator (kept for compatibility; now draws a section divider).
	static void separator(const std::string& label = "");

	// Toggle verbose DEBUG output at runtime (SAM_DEBUG=1 style flag).
	static void setDebugMode(bool enabled) { debugMode = enabled; }
	static bool isDebugMode() { return debugMode; }
	static bool isInitialized() { return initialized; }

private:
	// Which phase we are in — controls the relative-time suffix on gameplay lines.
	enum class Phase { Init, ModLoad, Gameplay };

	static std::ofstream logFile;
	static std::mutex logMutex;
	static bool debugMode;
	static bool initialized;
	static bool summaryWritten; // guards the session summary against a double write

	// Session state.
	static int         sessionNumber;
	static Phase       phase;
	static std::chrono::steady_clock::time_point sessionStart;
	static std::chrono::steady_clock::time_point modLoadStart;
	static long long   loadMillis;      // measured mod-load time
	// Cumulative counters.
	static long long   warnCount, errorCount, hookCount, hookScriptsTotal, apiCallCount, scriptErrorCount;
	// Snapshots taken at logLoadSummary(), so the session summary reports the
	// gameplay-phase deltas.
	static long long   warnAtLoadEnd, errorAtLoadEnd;

	// Emit a raw line (already fully formatted) to both sinks. Caller holds lock.
	static void emitRaw(const std::string& fileLine, const char* color, const std::string& stdoutLine);

	static std::string getTimestamp();         // "HH:MM:SS"
	static std::string getDateTimeStamp();     // "YYYY-MM-DD HH:MM:SS"
	static std::string relativeStamp();        // "+H:MM:SS" since session start
	static std::string levelToString(SAMLogLevel level);  // padded to 5 chars
	static const char* levelToColor(SAMLogLevel level);   // ANSI colour code
	static std::string padModule(const std::string& module);  // padded to 8 chars

	static void rotateAndOpen(const std::string& path);   // keep last 5 sessions
	static int  bumpSessionCounter(const std::string& dir);
	static void writeSessionHeader();
};

/*-------------------------------------------------------------------------------
	Convenience macros — the intended way to call the logger everywhere in SAM.
-------------------------------------------------------------------------------*/
#define SAM_INFO(module, msg)  SAMLogger::info((module), (msg))
#define SAM_WARN(module, msg)  SAMLogger::warn((module), (msg))
#define SAM_ERROR(module, msg) SAMLogger::error((module), (msg))
#define SAM_DEBUG(module, msg) SAMLogger::debug((module), (msg))
