/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_logger.hpp
	Desc: console + file logger for the S.A.M Barony modding framework.
	      This is the first system built — every other S.A.M system depends
	      on it, so it has no dependency on any other S.A.M file and only a
	      minimal dependency on the C++ standard library.

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

// S.A.M framework version — stamped into the session banner in the log file.
#define SAM_FRAMEWORK_VERSION "0.4.0"

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

class SAMLogger
{
public:
	// Opens <outputDir>/sam_log.txt in append mode, enables ANSI colour on
	// Windows consoles, and writes the session-start banner. Safe to call
	// more than once — subsequent calls are ignored (idempotent). If the log
	// file cannot be opened, logging still proceeds to stdout.
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

	// Writes a rule (optionally with a label between two rules) to both sinks.
	static void separator(const std::string& label = "");

	// Toggle verbose DEBUG output at runtime (SAM_DEBUG=1 style flag).
	static void setDebugMode(bool enabled) { debugMode = enabled; }
	static bool isDebugMode() { return debugMode; }
	static bool isInitialized() { return initialized; }

private:
	static std::ofstream logFile;
	static std::mutex logMutex;
	static bool debugMode;
	static bool initialized;

	static std::string getTimestamp();      // "HH:MM:SS"
	static std::string getDateTimeStamp();   // "YYYY-MM-DD HH:MM:SS"
	static std::string levelToString(SAMLogLevel level);  // padded to 5 chars
	static const char* levelToColor(SAMLogLevel level);   // ANSI colour code
	static std::string padModule(const std::string& module);  // padded to 8 chars
};

/*-------------------------------------------------------------------------------
	Convenience macros — the intended way to call the logger everywhere in SAM.
-------------------------------------------------------------------------------*/
#define SAM_INFO(module, msg)  SAMLogger::info((module), (msg))
#define SAM_WARN(module, msg)  SAMLogger::warn((module), (msg))
#define SAM_ERROR(module, msg) SAMLogger::error((module), (msg))
#define SAM_DEBUG(module, msg) SAMLogger::debug((module), (msg))
