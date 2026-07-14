/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_logger.cpp
	Desc: implementation of the S.A.M structured console + file logger (v0.7.0).

	Log layout (one block per launch, newest last, oldest rotated out at 5):

	    <boxed session header>
	    -- INIT --------------------------------
	    [HH:MM:SS] INFO  [MODULE  ] message
	    -- MOD LOAD ----------------------------
	    ...
	    -- LOAD SUMMARY ------------------------
	      Mods loaded:  1  ...
	    -- GAMEPLAY ----------------------------
	    [HH:MM:SS +0:04:23] INFO  [HOOK    ] ...
	    -- SESSION SUMMARY ---------------------
	      Duration: ...   <box bottom>

	- LEVEL is padded to 5 chars (INFO /WARN /ERROR/DEBUG)
	- MODULE is padded to 8 chars for column alignment
	- ERROR lines are prefixed with "!!! " so they stand out when scanning
	- during the GAMEPLAY phase a session-relative "+H:MM:SS" is added
	- stdout is ANSI colour-coded; the file is always plain UTF-8, append mode
	- box/divider glyphs are written as explicit UTF-8 bytes so they are correct
	  regardless of the compiler's source/exec codepage

-------------------------------------------------------------------------------*/

#include "sam_logger.hpp"

#include <cstdio>
#include <cstdlib>   // std::atexit
#include <ctime>
#include <vector>
#include <iterator>

#ifdef _WIN32
	#include <windows.h>
	#include <io.h>      // _isatty
	#include <process.h> // _getpid
	#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
		#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
	#endif
#else
	#include <unistd.h>  // isatty, getpid
#endif

/*-------------------------------------------------------------------------------
	ANSI colour codes (applied to stdout only).
-------------------------------------------------------------------------------*/
#define SAM_COLOR_RESET  "\033[0m"
#define SAM_COLOR_GREEN  "\033[32m"  // INFO
#define SAM_COLOR_YELLOW "\033[33m"  // WARN
#define SAM_COLOR_RED    "\033[31m"  // ERROR
#define SAM_COLOR_CYAN   "\033[36m"  // DEBUG

// Box-drawing glyphs as explicit UTF-8 byte sequences (encoding-independent).
static const char* const GX_TL = "\xE2\x95\x94"; // U+2554 top-left    ╔
static const char* const GX_TR = "\xE2\x95\x97"; // U+2557 top-right   ╗
static const char* const GX_BL = "\xE2\x95\x9A"; // U+255A bot-left    ╚
static const char* const GX_BR = "\xE2\x95\x9D"; // U+255D bot-right   ╝
static const char* const GX_H  = "\xE2\x95\x90"; // U+2550 heavy horiz ═
static const char* const GX_V  = "\xE2\x95\x91"; // U+2551 heavy vert  ║
static const char* const GX_L  = "\xE2\x94\x80"; // U+2500 light horiz ─

static const int SAM_BOX_INNER = 54;  // columns between the ║ borders
static const int SAM_DIV_WIDTH = 56;  // total columns of a section divider

/*-------------------------------------------------------------------------------
	Static member storage.
-------------------------------------------------------------------------------*/
std::ofstream SAMLogger::logFile;
std::mutex SAMLogger::logMutex;
bool SAMLogger::debugMode = false;
bool SAMLogger::initialized = false;
bool SAMLogger::summaryWritten = false;

// Fallback so the SESSION SUMMARY is written even when the process exits without
// SAM's mod-unload path running (e.g. closing the game window). Registered with
// std::atexit during init; the double-write guard makes it a no-op if unload
// already wrote the summary.
static void samAtExitSessionSummary()
{
	SAMLogger::logSessionSummary();
}

int SAMLogger::sessionNumber = 0;
SAMLogger::Phase SAMLogger::phase = SAMLogger::Phase::Init;
std::chrono::steady_clock::time_point SAMLogger::sessionStart;
std::chrono::steady_clock::time_point SAMLogger::modLoadStart;
long long SAMLogger::loadMillis = 0;
long long SAMLogger::warnCount = 0;
long long SAMLogger::errorCount = 0;
long long SAMLogger::hookCount = 0;
long long SAMLogger::hookScriptsTotal = 0;
long long SAMLogger::apiCallCount = 0;
long long SAMLogger::scriptErrorCount = 0;
long long SAMLogger::warnAtLoadEnd = 0;
long long SAMLogger::errorAtLoadEnd = 0;

/*-------------------------------------------------------------------------------
	Local helpers.
-------------------------------------------------------------------------------*/
static void samLocalTime(std::tm& out, std::time_t t)
{
#ifdef _WIN32
	localtime_s(&out, &t);
#else
	localtime_r(&t, &out);
#endif
}

static bool samStdoutIsTty()
{
	static bool checked = false;
	static bool tty = false;
	if ( !checked )
	{
#ifdef _WIN32
		tty = (_isatty(_fileno(stdout)) != 0);
#else
		tty = (isatty(fileno(stdout)) != 0);
#endif
		checked = true;
	}
	return tty;
}

static long samProcessId()
{
#ifdef _WIN32
	return (long)GetCurrentProcessId();
#else
	return (long)getpid();
#endif
}

#ifdef _WIN32
static void samEnableVirtualTerminal()
{
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if ( hOut == INVALID_HANDLE_VALUE || hOut == nullptr ) { return; }
	DWORD mode = 0;
	if ( !GetConsoleMode(hOut, &mode) ) { return; }
	SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#endif

// Repeat a UTF-8 glyph `n` times.
static std::string samRepeat(const char* glyph, int n)
{
	std::string s;
	if ( n < 0 ) { n = 0; }
	s.reserve((size_t)n * 4);
	for ( int i = 0; i < n; ++i ) { s += glyph; }
	return s;
}

// One boxed content line: ║  <text><pad>║  (text is ASCII, so bytes == columns).
static std::string samBoxLine(const std::string& text)
{
	std::string inner = "  " + text;
	if ( (int)inner.size() < SAM_BOX_INNER ) { inner.append(SAM_BOX_INNER - (int)inner.size(), ' '); }
	else if ( (int)inner.size() > SAM_BOX_INNER ) { inner = inner.substr(0, SAM_BOX_INNER); }
	return std::string(GX_V) + inner + GX_V;
}

static std::string samDivider(const std::string& name)
{
	// "── NAME ────────"
	std::string head = std::string(GX_L) + GX_L + " " + name + " ";
	const int usedCols = 2 + 1 + (int)name.size() + 1; // 2 lights + space + name + space
	return head + samRepeat(GX_L, SAM_DIV_WIDTH - usedCols);
}

/*-------------------------------------------------------------------------------
	Low-level emit (caller MUST hold logMutex).
-------------------------------------------------------------------------------*/
void SAMLogger::emitRaw(const std::string& fileLine, const char* color, const std::string& stdoutLine)
{
	if ( samStdoutIsTty() && color )
	{
		fprintf(stdout, "%s%s%s\n", color, stdoutLine.c_str(), SAM_COLOR_RESET);
	}
	else
	{
		fprintf(stdout, "%s\n", stdoutLine.c_str());
	}
	fflush(stdout);

	if ( logFile.is_open() )
	{
		logFile << fileLine << "\n";
		logFile.flush();
	}
}

// Emit an already-formatted structural block (box/divider/summary) with no colour.
static void samEmitPlain(std::ofstream& file, const std::string& text)
{
	fprintf(stdout, "%s\n", text.c_str());
	fflush(stdout);
	if ( file.is_open() ) { file << text << "\n"; file.flush(); }
}

/*-------------------------------------------------------------------------------
	Session counter + rotation.
-------------------------------------------------------------------------------*/
int SAMLogger::bumpSessionCounter(const std::string& dir)
{
	const std::string counterPath = dir + "sam_session.txt";
	int n = 0;
	{
		std::ifstream f(counterPath.c_str());
		if ( f ) { f >> n; }
	}
	if ( n < 0 ) { n = 0; }
	++n;
	{
		std::ofstream f(counterPath.c_str(), std::ios::out | std::ios::trunc);
		if ( f ) { f << n; }
	}
	return n;
}

void SAMLogger::rotateAndOpen(const std::string& path)
{
	// Keep only the most recent (5 - 1) sessions in the file, so this launch makes
	// five. Sessions are delimited by the box-top glyph (╔) at column 0.
	const int KEEP = 5;
	const std::string marker = std::string(GX_TL); // start of each session banner
	{
		std::ifstream in(path.c_str(), std::ios::binary);
		if ( in )
		{
			std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			in.close();
			std::vector<size_t> starts;
			size_t pos = 0;
			while ( (pos = content.find(marker, pos)) != std::string::npos )
			{
				starts.push_back(pos);
				pos += marker.size();
			}
			if ( (int)starts.size() >= KEEP )
			{
				const size_t keepFrom = starts[starts.size() - (KEEP - 1)];
				const std::string trimmed = content.substr(keepFrom);
				std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
				if ( out ) { out << trimmed; out.close(); }
			}
		}
	}
	logFile.open(path.c_str(), std::ios::out | std::ios::app);
}

void SAMLogger::writeSessionHeader()
{
	samEmitPlain(logFile, "");
	samEmitPlain(logFile, std::string(GX_TL) + samRepeat(GX_H, SAM_BOX_INNER) + GX_TR);
	samEmitPlain(logFile, samBoxLine(std::string("S.A.M Framework v") + SAM_FRAMEWORK_VERSION));
	samEmitPlain(logFile, samBoxLine("Session #" + std::to_string(sessionNumber) + " - " + getDateTimeStamp()));
	samEmitPlain(logFile, samBoxLine(std::string("Barony v") + SAM_BARONY_TARGET + " - PID " + std::to_string(samProcessId())));
	samEmitPlain(logFile, std::string(GX_BL) + samRepeat(GX_H, SAM_BOX_INNER) + GX_BR);
}

/*-------------------------------------------------------------------------------
	SAMLogger::init
-------------------------------------------------------------------------------*/
void SAMLogger::init(const std::string& outputDir, bool debugModeEnabled)
{
	std::lock_guard<std::mutex> lock(logMutex);

	if ( initialized ) { return; }

	debugMode = debugModeEnabled;

#ifdef _WIN32
	samEnableVirtualTerminal();
#endif

	std::string dir = outputDir;
	if ( !dir.empty() )
	{
		const char back = dir.back();
		if ( back != '/' && back != '\\' ) { dir += '/'; }
	}
	const std::string path = dir + "sam_log.txt";

	sessionNumber = bumpSessionCounter(dir);
	rotateAndOpen(path); // trims to last 5 sessions, opens the file in append mode

	initialized = true;
	summaryWritten = false;
	std::atexit(samAtExitSessionSummary); // ensure the SESSION SUMMARY writes on any clean exit
	sessionStart = std::chrono::steady_clock::now();
	modLoadStart = sessionStart;
	phase = Phase::Init;
	warnCount = errorCount = hookCount = hookScriptsTotal = apiCallCount = scriptErrorCount = 0;
	warnAtLoadEnd = errorAtLoadEnd = 0;
	loadMillis = 0;

	writeSessionHeader();
	samEmitPlain(logFile, "");
	samEmitPlain(logFile, samDivider("INIT"));

	// One line so the INIT section is never empty (mod loading, incl. runtime
	// bring-up, opens its own MOD LOAD section from the loader).
	const std::string initLine = "[" + getTimestamp() + "] " + levelToString(SAMLogLevel::Info)
		+ " [" + padModule("CORE") + "] S.A.M logger ready - v" SAM_FRAMEWORK_VERSION
		+ ", session #" + std::to_string(sessionNumber) + (debugMode ? " (debug on)" : "");
	emitRaw(initLine, levelToColor(SAMLogLevel::Info), initLine);
}

/*-------------------------------------------------------------------------------
	SAMLogger::shutdown
-------------------------------------------------------------------------------*/
void SAMLogger::shutdown()
{
	std::lock_guard<std::mutex> lock(logMutex);
	if ( logFile.is_open() )
	{
		logFile.flush();
		logFile.close();
	}
	initialized = false;
}

/*-------------------------------------------------------------------------------
	SAMLogger::log
-------------------------------------------------------------------------------*/
void SAMLogger::log(SAMLogLevel level, const std::string& module, const std::string& message)
{
	if ( level == SAMLogLevel::Debug && !debugMode ) { return; }

	std::lock_guard<std::mutex> lock(logMutex);

	if ( level == SAMLogLevel::Warn )  { ++warnCount; }
	if ( level == SAMLogLevel::Error ) { ++errorCount; }

	// Timestamp field — add a session-relative stamp during gameplay.
	std::string tsField = "[" + getTimestamp();
	if ( phase == Phase::Gameplay ) { tsField += " " + relativeStamp(); }
	tsField += "]";

	const std::string body = tsField + " " + levelToString(level) + " [" + padModule(module) + "] " + message;
	const std::string prefix = (level == SAMLogLevel::Error) ? "!!! " : "";
	const std::string line = prefix + body;

	emitRaw(line, levelToColor(level), line);
}

/*-------------------------------------------------------------------------------
	Convenience wrappers.
-------------------------------------------------------------------------------*/
void SAMLogger::info(const std::string& module, const std::string& message)  { log(SAMLogLevel::Info,  module, message); }
void SAMLogger::warn(const std::string& module, const std::string& message)  { log(SAMLogLevel::Warn,  module, message); }
void SAMLogger::error(const std::string& module, const std::string& message) { log(SAMLogLevel::Error, module, message); }
void SAMLogger::debug(const std::string& module, const std::string& message) { log(SAMLogLevel::Debug, module, message); }

/*-------------------------------------------------------------------------------
	Structured sections.
-------------------------------------------------------------------------------*/
void SAMLogger::beginSection(const std::string& name)
{
	std::lock_guard<std::mutex> lock(logMutex);
	samEmitPlain(logFile, "");
	samEmitPlain(logFile, samDivider(name));
}

void SAMLogger::beginModLoad()
{
	std::lock_guard<std::mutex> lock(logMutex);
	phase = Phase::ModLoad;
	modLoadStart = std::chrono::steady_clock::now();
	samEmitPlain(logFile, "");
	samEmitPlain(logFile, samDivider("MOD LOAD"));
}

void SAMLogger::logLoadSummary(const SAMLoadStats& s)
{
	std::lock_guard<std::mutex> lock(logMutex);

	loadMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - modLoadStart).count();
	warnAtLoadEnd = warnCount;
	errorAtLoadEnd = errorCount;

	const int scriptTotal = s.scriptsLua + s.scriptsJs;
	std::string monsters = std::to_string(s.monstersRegistered);
	if ( s.monstersRegistered > 0 || s.monstersDeclared > 0 )
	{
		monsters += " registered (" + std::to_string(s.monstersDeclared) + " declared) across "
			+ std::to_string(s.spawnLevels) + " spawn level(s)";
	}

	samEmitPlain(logFile, "");
	samEmitPlain(logFile, samDivider("LOAD SUMMARY"));
	samEmitPlain(logFile, "  Mods loaded:    " + std::to_string(s.mods));
	samEmitPlain(logFile, "  Classes:        " + std::to_string(s.classesRegistered) + " registered (" + std::to_string(s.classesDeclared) + " declared)");
	samEmitPlain(logFile, "  Items:          " + std::to_string(s.itemsRegistered) + " registered (" + std::to_string(s.itemsDeclared) + " declared)");
	samEmitPlain(logFile, "  Monsters:       " + monsters);
	samEmitPlain(logFile, "  Scripts:        " + std::to_string(scriptTotal) + " (" + std::to_string(s.scriptsLua) + " Lua, " + std::to_string(s.scriptsJs) + " JS/TS)");
	samEmitPlain(logFile, "  Patch ops:      " + std::to_string(s.patchOps) + " applied across " + std::to_string(s.patchFiles) + " file(s)");
	samEmitPlain(logFile, "  Plugins:        " + std::to_string(s.plugins) + " declared");
	samEmitPlain(logFile, "  Warnings:       " + std::to_string(warnCount));
	samEmitPlain(logFile, "  Errors:         " + std::to_string(errorCount));
	samEmitPlain(logFile, "  Load time:      " + std::to_string(loadMillis) + "ms");
}

void SAMLogger::logSessionSummary()
{
	std::lock_guard<std::mutex> lock(logMutex);
	if ( !initialized || summaryWritten ) { return; } // write at most once per session
	summaryWritten = true;

	const long long secs = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - sessionStart).count();
	char dur[48] = { 0 };
	if ( secs >= 3600 ) { snprintf(dur, sizeof(dur), "%lldh %lldm %llds", secs / 3600, (secs % 3600) / 60, secs % 60); }
	else                { snprintf(dur, sizeof(dur), "%lldm %llds", secs / 60, secs % 60); }

	samEmitPlain(logFile, "");
	samEmitPlain(logFile, samDivider("SESSION SUMMARY"));
	samEmitPlain(logFile, std::string("  Duration:       ") + dur);
	samEmitPlain(logFile, "  Hooks fired:    " + std::to_string(hookCount) + " (Lua+JS dispatches, " + std::to_string(hookScriptsTotal) + " script deliveries)");
	samEmitPlain(logFile, "  API calls:      " + std::to_string(apiCallCount));
	samEmitPlain(logFile, "  Script errors:  " + std::to_string(scriptErrorCount));
	samEmitPlain(logFile, "  Warnings:       " + std::to_string(warnCount - warnAtLoadEnd) + " (gameplay)");
	samEmitPlain(logFile, "  Errors:         " + std::to_string(errorCount - errorAtLoadEnd) + " (gameplay)");
	samEmitPlain(logFile, std::string(GX_BL) + samRepeat(GX_H, SAM_BOX_INNER) + GX_BR);
}

/*-------------------------------------------------------------------------------
	Gameplay counters.
-------------------------------------------------------------------------------*/
void SAMLogger::noteHookFired(int scriptsReached)
{
	std::lock_guard<std::mutex> lock(logMutex);
	++hookCount;
	if ( scriptsReached > 0 ) { hookScriptsTotal += scriptsReached; }
	if ( phase != Phase::Gameplay )
	{
		phase = Phase::Gameplay;
		samEmitPlain(logFile, "");
		samEmitPlain(logFile, samDivider("GAMEPLAY"));
	}
}

void SAMLogger::noteApiCall()
{
	std::lock_guard<std::mutex> lock(logMutex);
	++apiCallCount;
}

void SAMLogger::noteScriptError()
{
	std::lock_guard<std::mutex> lock(logMutex);
	++scriptErrorCount;
}

void SAMLogger::separator(const std::string& label)
{
	// Back-compat: draw a section divider (or a plain rule when unlabelled).
	std::lock_guard<std::mutex> lock(logMutex);
	samEmitPlain(logFile, label.empty() ? samRepeat(GX_L, SAM_DIV_WIDTH) : samDivider(label));
}

/*-------------------------------------------------------------------------------
	Formatting helpers.
-------------------------------------------------------------------------------*/
std::string SAMLogger::getTimestamp()
{
	std::time_t now = std::time(nullptr);
	std::tm tm{};
	samLocalTime(tm, now);
	char buf[16] = { 0 };
	std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
	return std::string(buf);
}

std::string SAMLogger::getDateTimeStamp()
{
	std::time_t now = std::time(nullptr);
	std::tm tm{};
	samLocalTime(tm, now);
	char buf[32] = { 0 };
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	return std::string(buf);
}

std::string SAMLogger::relativeStamp()
{
	const long long secs = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - sessionStart).count();
	char buf[24] = { 0 };
	snprintf(buf, sizeof(buf), "+%lld:%02lld:%02lld", secs / 3600, (secs % 3600) / 60, secs % 60);
	return std::string(buf);
}

std::string SAMLogger::levelToString(SAMLogLevel level)
{
	switch ( level )
	{
		case SAMLogLevel::Info:  return "INFO ";
		case SAMLogLevel::Warn:  return "WARN ";
		case SAMLogLevel::Error: return "ERROR";
		case SAMLogLevel::Debug: return "DEBUG";
		default:                 return "?????";
	}
}

const char* SAMLogger::levelToColor(SAMLogLevel level)
{
	switch ( level )
	{
		case SAMLogLevel::Info:  return SAM_COLOR_GREEN;
		case SAMLogLevel::Warn:  return SAM_COLOR_YELLOW;
		case SAMLogLevel::Error: return SAM_COLOR_RED;
		case SAMLogLevel::Debug: return SAM_COLOR_CYAN;
		default:                 return SAM_COLOR_RESET;
	}
}

std::string SAMLogger::padModule(const std::string& module)
{
	std::string m = module;
	if ( m.size() < 8 ) { m.append(8 - m.size(), ' '); }
	return m;
}

/*-------------------------------------------------------------------------------
	Standalone self-test (NOT compiled into Barony).

	    g++ -std=c++17 -DSAM_LOGGER_SELFTEST sam_logger.cpp -o sam_logger_test
	    ./sam_logger_test
-------------------------------------------------------------------------------*/
#ifdef SAM_LOGGER_SELFTEST
int main()
{
	SAMLogger::init(".", /*debugModeEnabled=*/true);

	SAM_INFO("LUA", "Lua 5.4 runtime initialized");
	SAM_INFO("JS", "QuickJS runtime initialized");

	SAMLogger::beginModLoad();
	SAM_INFO("WORKSHOP", "Found: S.A.M Test Mod [sam_test] v1.0.0");
	SAM_INFO("CLASSES", "Registered: Assassin [sam_test:assassin] -> id 1000");
	SAM_WARN("PATCHER", "edit_field: 'items.bronze_sword.NONEXISTENT' not found - skipped");

	SAMLoadStats st;
	st.mods = 1; st.classesRegistered = 2; st.classesDeclared = 2;
	st.itemsRegistered = 1; st.itemsDeclared = 1; st.scriptsLua = 1; st.scriptsJs = 2;
	st.patchOps = 4; st.patchFiles = 1;
	SAMLogger::logLoadSummary(st);

	SAMLogger::noteHookFired(3);
	SAM_INFO("SCRIPT", "[Lua] Assassin leveled up to 2");
	SAMLogger::noteApiCall();
	SAM_ERROR("SCRIPT", "example error line stands out");
	SAMLogger::noteHookFired(3);

	SAMLogger::logSessionSummary();
	SAMLogger::shutdown();
	return 0;
}
#endif
