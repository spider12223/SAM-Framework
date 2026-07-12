/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_logger.cpp
	Desc: implementation of the S.A.M console + file logger.

	Line format:
	    [HH:MM:SS][SAM LEVEL][MODULE  ] message
	    e.g. [14:32:05][SAM INFO ][CORE    ] S.A.M initializing...

	- LEVEL is padded to 5 chars (INFO /WARN /ERROR/DEBUG)
	- MODULE is padded to 8 chars for column alignment
	- stdout is ANSI colour-coded (green/yellow/red/cyan) when it is a terminal
	- the log file (<outputdir>/sam_log.txt) is always plain text, append mode

-------------------------------------------------------------------------------*/

#include "sam_logger.hpp"

#include <cstdio>
#include <ctime>

#ifdef _WIN32
	#include <windows.h>
	#include <io.h>      // _isatty
	// Older Windows SDKs may not define this flag.
	#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
		#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
	#endif
#else
	#include <unistd.h>  // isatty
#endif

/*-------------------------------------------------------------------------------
	ANSI colour codes (applied to stdout only).
-------------------------------------------------------------------------------*/
#define SAM_COLOR_RESET  "\033[0m"
#define SAM_COLOR_GREEN  "\033[32m"  // INFO
#define SAM_COLOR_YELLOW "\033[33m"  // WARN
#define SAM_COLOR_RED    "\033[31m"  // ERROR
#define SAM_COLOR_CYAN   "\033[36m"  // DEBUG

static const char* const SAM_LOG_RULE = "========================================";

/*-------------------------------------------------------------------------------
	Static member storage.
-------------------------------------------------------------------------------*/
std::ofstream SAMLogger::logFile;
std::mutex SAMLogger::logMutex;
bool SAMLogger::debugMode = false;
bool SAMLogger::initialized = false;

/*-------------------------------------------------------------------------------
	Local helpers.
-------------------------------------------------------------------------------*/

// Fill a tm struct from a time_t in a thread-safe, portable way.
static void samLocalTime(std::tm& out, std::time_t t)
{
#ifdef _WIN32
	localtime_s(&out, &t);
#else
	localtime_r(&t, &out);
#endif
}

// Cache whether stdout is an interactive terminal so we only emit colour codes
// when they will actually be interpreted (avoids garbage in redirected output).
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

#ifdef _WIN32
// Enable ANSI escape-sequence processing on the Windows console. Fails
// silently if there is no console attached (e.g. launched from Steam), which
// is fine — plain text still goes to stdout.
static void samEnableVirtualTerminal()
{
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if ( hOut == INVALID_HANDLE_VALUE || hOut == nullptr )
	{
		return;
	}
	DWORD mode = 0;
	if ( !GetConsoleMode(hOut, &mode) )
	{
		return;
	}
	SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#endif

/*-------------------------------------------------------------------------------
	SAMLogger::init
-------------------------------------------------------------------------------*/
void SAMLogger::init(const std::string& outputDir, bool debugModeEnabled)
{
	std::lock_guard<std::mutex> lock(logMutex);

	if ( initialized )
	{
		return; // idempotent — safe to call from multiple init paths
	}

	debugMode = debugModeEnabled;

#ifdef _WIN32
	samEnableVirtualTerminal();
#endif

	// Build "<outputDir>/sam_log.txt", tolerating a trailing separator.
	std::string path = outputDir;
	if ( !path.empty() )
	{
		const char back = path.back();
		if ( back != '/' && back != '\\' )
		{
			path += '/';
		}
	}
	path += "sam_log.txt";

	// Append so multiple launches accumulate, each with its own banner.
	logFile.open(path.c_str(), std::ios::out | std::ios::app);

	initialized = true;

	// Session-start banner (written to file and echoed to stdout).
	const std::string line2 = std::string(" S.A.M Framework v") + SAM_FRAMEWORK_VERSION + " | Session Start";
	const std::string line3 = std::string(" ") + getDateTimeStamp();

	if ( logFile.is_open() )
	{
		logFile << "\n"
		        << SAM_LOG_RULE << "\n"
		        << line2 << "\n"
		        << line3 << "\n"
		        << SAM_LOG_RULE << "\n";
		logFile.flush();
	}

	fprintf(stdout, "\n%s\n%s\n%s\n%s\n",
		SAM_LOG_RULE, line2.c_str(), line3.c_str(), SAM_LOG_RULE);
	fflush(stdout);
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
	// Suppress verbose DEBUG output unless debug mode is on. Reading the flag
	// without the lock is safe: it is a bool set once during init().
	if ( level == SAMLogLevel::Debug && !debugMode )
	{
		return;
	}

	std::lock_guard<std::mutex> lock(logMutex);

	const std::string line = "[" + getTimestamp() + "][SAM " + levelToString(level)
		+ "][" + padModule(module) + "] " + message;

	// stdout — colourised when interactive.
	if ( samStdoutIsTty() )
	{
		fprintf(stdout, "%s%s%s\n", levelToColor(level), line.c_str(), SAM_COLOR_RESET);
	}
	else
	{
		fprintf(stdout, "%s\n", line.c_str());
	}
	fflush(stdout);

	// log file — always plain text, flushed each line so it survives a crash.
	if ( logFile.is_open() )
	{
		logFile << line << "\n";
		logFile.flush();
	}
}

/*-------------------------------------------------------------------------------
	SAMLogger convenience wrappers.
-------------------------------------------------------------------------------*/
void SAMLogger::info(const std::string& module, const std::string& message)
{
	log(SAMLogLevel::Info, module, message);
}

void SAMLogger::warn(const std::string& module, const std::string& message)
{
	log(SAMLogLevel::Warn, module, message);
}

void SAMLogger::error(const std::string& module, const std::string& message)
{
	log(SAMLogLevel::Error, module, message);
}

void SAMLogger::debug(const std::string& module, const std::string& message)
{
	log(SAMLogLevel::Debug, module, message);
}

/*-------------------------------------------------------------------------------
	SAMLogger::separator
-------------------------------------------------------------------------------*/
void SAMLogger::separator(const std::string& label)
{
	std::lock_guard<std::mutex> lock(logMutex);

	if ( logFile.is_open() )
	{
		logFile << SAM_LOG_RULE << "\n";
		if ( !label.empty() )
		{
			logFile << " " << label << "\n" << SAM_LOG_RULE << "\n";
		}
		logFile.flush();
	}

	fprintf(stdout, "%s\n", SAM_LOG_RULE);
	if ( !label.empty() )
	{
		fprintf(stdout, " %s\n%s\n", label.c_str(), SAM_LOG_RULE);
	}
	fflush(stdout);
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

std::string SAMLogger::levelToString(SAMLogLevel level)
{
	// Padded to 5 chars so the level column stays aligned: "SAM INFO ", "SAM ERROR".
	switch ( level )
	{
		case SAMLogLevel::Info:  return "INFO ";
		case SAMLogLevel::Warn:  return "WARN ";
		case SAMLogLevel::Error:   return "ERROR";
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
		case SAMLogLevel::Error:   return SAM_COLOR_RED;
		case SAMLogLevel::Debug: return SAM_COLOR_CYAN;
		default:                 return SAM_COLOR_RESET;
	}
}

std::string SAMLogger::padModule(const std::string& module)
{
	// Pad short module names to 8 chars; never truncate longer ones so no
	// module identity is ever lost (alignment may drift for names > 8 chars).
	std::string m = module;
	if ( m.size() < 8 )
	{
		m.append(8 - m.size(), ' ');
	}
	return m;
}

/*-------------------------------------------------------------------------------
	Standalone self-test.

	This is NOT compiled into Barony. It only exists when the translation unit
	is built on its own with -DSAM_LOGGER_SELFTEST, so it can never collide with
	Barony's own main(). Build & run standalone with:

	    g++ -std=c++17 -DSAM_LOGGER_SELFTEST sam_logger.cpp -o sam_logger_test
	    ./sam_logger_test
-------------------------------------------------------------------------------*/
#ifdef SAM_LOGGER_SELFTEST
int main()
{
	SAMLogger::init(".", /*debugModeEnabled=*/true);

	SAM_INFO("CORE", "S.A.M initializing...");
	SAM_INFO("WORKSHOP", "Scanning Workshop folder: steamapps/workshop/content/371970/");
	SAM_INFO("LOADER", "Reading mod.json for: darkblade_pack");
	SAM_INFO("CLASSES", "Registering class: Assassin [darkblade:assassin]");
	SAM_WARN("ITEMS", "No model found for: smoke_bomb - using fallback model");
	SAM_ERROR("SYNC", "Mod list mismatch: client missing darkblade_pack");
	SAM_DEBUG("LOADER", "verbose detail visible because debug mode is ON");

	SAMLogger::separator("Debug mode OFF - next debug line should be hidden");
	SAMLogger::setDebugMode(false);
	SAM_DEBUG("LOADER", "this DEBUG line must NOT appear");
	SAM_INFO("CORE", "S.A.M load complete. 1 mod loaded, 1 class, 2 items.");

	SAMLogger::shutdown();
	return 0;
}
#endif
