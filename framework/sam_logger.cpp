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
	- a SECOND copy of the game started from the same folder (how you test multiplayer
	  on one computer) writes sam_log_2.txt instead, and leaves the first copy's live
	  sam_log.txt, its archive and the session counter alone. See claimInstanceSlot.

-------------------------------------------------------------------------------*/

#include "sam_logger.hpp"

#include <cstdio>
#include <cstdlib>   // std::atexit
#include <ctime>
#include <vector>
#include <string>
#include <algorithm> // std::sort, std::min
#include <utility>
#include <map>
#include <iterator>

#ifdef _WIN32
	#include <windows.h>
	#include <io.h>      // _isatty
	#include <direct.h>  // _mkdir
	#include <process.h> // _getpid
	#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
		#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
	#endif
#else
	#include <unistd.h>     // isatty, getpid, close
	#include <sys/stat.h>   // mkdir
	#include <dirent.h>     // opendir/readdir
	#include <fcntl.h>      // open, fcntl (the instance lock)
	#include <cerrno>       // EACCES/EAGAIN: "another copy holds it" vs "cannot lock here"
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
std::string SAMLogger::repeatModule;
std::string SAMLogger::repeatMessage;
SAMLogLevel SAMLogger::repeatLevel = SAMLogLevel::Info;
int SAMLogger::repeatCount = 0;
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
int SAMLogger::instanceIndex = 1;
SAMLogger::Phase SAMLogger::phase = SAMLogger::Phase::Init;
std::chrono::steady_clock::time_point SAMLogger::sessionStart;
std::chrono::steady_clock::time_point SAMLogger::modLoadStart;
long long SAMLogger::loadMillis = 0;
long long SAMLogger::warnCount = 0;
long long SAMLogger::errorCount = 0;
long long SAMLogger::hookCount = 0;
std::map<std::string, long long> SAMLogger::hookTally;
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
// Emit the "... and N more" line for a run of identical messages. Caller holds the lock.
void SAMLogger::flushRepeatLocked()
{
	if ( repeatCount <= 0 ) { return; }
	const int n = repeatCount;
	repeatCount = 0;
	const std::string text = "[" + getTimestamp() + "] " + levelToString(repeatLevel)
		+ " [" + padModule(repeatModule) + "] ... last message repeated "
		+ std::to_string(n) + " more time" + ( n == 1 ? "" : "s" );
	emitRaw(text, levelToColor(repeatLevel), text);
}

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
int SAMLogger::readSessionCounter(const std::string& dir)
{
	const std::string counterPath = dir + "sam_session.txt";
	int n = 0;
	std::ifstream f(counterPath.c_str());
	if ( f ) { f >> n; }
	return ( n < 0 ) ? 0 : n;
}

int SAMLogger::bumpSessionCounter(const std::string& dir)
{
	const std::string counterPath = dir + "sam_session.txt";
	int n = readSessionCounter(dir);
	++n;
	{
		std::ofstream f(counterPath.c_str(), std::ios::out | std::ios::trunc);
		if ( f ) { f << n; }
	}
	return n;
}

/*-------------------------------------------------------------------------------
	Instance slots: one log file per copy of the game running from this folder.

	Testing multiplayer alone means starting the same install twice and joining
	127.0.0.1 from the second copy. Both copies run this logger against the same
	directory, and sam_log.txt is opened with std::ios::trunc after archiving the
	previous run -- so without this, the second copy would archive the host's log
	seconds after it started, empty the file the host is still writing to, and then
	the two runs would interleave line by line into one unreadable file.

	Each copy claims the lowest free slot by opening "<dir>sam_log[_N].lock" (".sam_log[_N].lock"
	on Linux and macOS, where a leading dot is the only way to keep it out of sight) so that
	nobody else can hold it, and keeps that handle for the life of the process. The OS releases
	it when the process ends, crash included, so the next launch gets slot 1 again with no
	stale-lock cleanup to go wrong. Windows also deletes the file on close; on POSIX the file
	stays, on purpose -- removing it would let a later copy lock a new file at the same name
	while an older one still holds the lock on the old inode, and two copies would then both
	claim slot 1. Slot 1 writes sam_log.txt and behaves exactly as before; slot 2 writes
	sam_log_2.txt, and so on.

	If the platform cannot lock at all (a filesystem with no locking, a permission
	problem, Windows Controlled Folder Access, an antivirus policy), claiming falls
	back to slot 1: identical to the behaviour before this existed, rather than
	refusing to log. "Held by another copy" and "could not be created at all" are
	therefore two DIFFERENT answers, and samTryClaimSlot has to tell them apart --
	if it could not, a machine that simply cannot make the file would fail all eight
	attempts, land on slot 8, and quietly write its whole session to sam_log_8.txt
	while the user, support and the ship gate all read a stale sam_log.txt.
-------------------------------------------------------------------------------*/
static const int SAM_MAX_INSTANCES = 8;

// "" for slot 1 (sam_log.txt), "_2" for slot 2 (sam_log_2.txt), and so on.
static std::string samInstanceSuffix(int slot)
{
	return ( slot <= 1 ) ? std::string() : ( "_" + std::to_string(slot) );
}

#ifdef _WIN32
static HANDLE samInstanceLock = INVALID_HANDLE_VALUE;
#else
static int samInstanceLock = -1;
#endif

// The three answers a claim can give. "Held" means another live copy of the game owns
// that slot, so walking on to the next one is right. "CannotCreate" means this machine
// cannot make the lock file at all, so walking on is pointless (every slot will answer
// the same) and would end in the silent sam_log_8.txt described above.
enum class SamSlotClaim { Claimed, Held, CannotCreate };

// Try to take exclusive hold of one slot's lock file.
static SamSlotClaim samTryClaimSlot(const std::string& dir, int slot)
{
	// A leading dot on POSIX, where there is no hidden attribute: the file has no contents worth
	// seeing and it lives in the player's savegame folder. It is deliberately NOT unlinked at
	// exit -- unlinking the path while another copy is waiting on that inode's lock would let a
	// third copy create a new inode at the same name and lock it successfully, and two copies
	// would then both believe they own slot 1 and both truncate sam_log.txt. The OS releases the
	// lock when the process ends, crash included, so a file left behind costs nothing.
#ifdef _WIN32
	const std::string lockPath = dir + "sam_log" + samInstanceSuffix(slot) + ".lock";
#else
	const std::string lockPath = dir + ".sam_log" + samInstanceSuffix(slot) + ".lock";
#endif
#ifdef _WIN32
	// dwShareMode 0: a second process opening the same name fails with a sharing
	// violation, which is exactly the question being asked. DELETE_ON_CLOSE keeps the
	// folder clean, since the file has no contents worth keeping.
	HANDLE h = CreateFileA(lockPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if ( h == INVALID_HANDLE_VALUE )
	{
		// ERROR_SHARING_VIOLATION is the only failure that means "somebody else has it":
		// while a live copy holds the handle with dwShareMode 0, that is the error every
		// other opener gets. Everything else (ERROR_ACCESS_DENIED from Controlled Folder
		// Access or an AV policy, a read-only or full disk, a share that will not take
		// the hidden attribute) means we could not create the file, which is not an
		// answer about other copies.
		return ( GetLastError() == ERROR_SHARING_VIOLATION ) ? SamSlotClaim::Held : SamSlotClaim::CannotCreate;
	}
	samInstanceLock = h;
	return SamSlotClaim::Claimed;
#else
	const int fd = open(lockPath.c_str(), O_CREAT | O_RDWR, 0644);
	if ( fd < 0 ) { return SamSlotClaim::CannotCreate; } // could not even make the file
	struct flock fl;
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	if ( fcntl(fd, F_SETLK, &fl) != 0 )
	{
		// The file opened, so the folder is writable; only the LOCK failed. EACCES and
		// EAGAIN mean another copy holds it. Anything else (a filesystem with no record
		// locking, an NFS mount with no lock daemon) is "this platform cannot lock",
		// which must fall back to slot 1 rather than walk to slot 8.
		const int err = errno;
		close(fd);
		return ( err == EACCES || err == EAGAIN ) ? SamSlotClaim::Held : SamSlotClaim::CannotCreate;
	}
	samInstanceLock = fd;
	return SamSlotClaim::Claimed;
#endif
}

int SAMLogger::claimInstanceSlot(const std::string& dir)
{
	for ( int slot = 1; slot <= SAM_MAX_INSTANCES; ++slot )
	{
		const SamSlotClaim r = samTryClaimSlot(dir, slot);
		if ( r == SamSlotClaim::Claimed ) { return slot; }
		if ( r == SamSlotClaim::CannotCreate )
		{
			// Locking is not available here at all. Behave exactly as the logger did
			// before slots existed: slot 1, sam_log.txt, rotate and archive as usual.
			// Nothing else holds slot 1 (a holder would have said Held), so there is no
			// live log to trample. If sam_log.txt cannot be opened either, the logger
			// already falls back to stdout on its own.
			return 1;
		}
	}
	// Every slot is genuinely held by another live copy. Slot 1 could not be claimed,
	// so writing sam_log.txt is the one thing we must not do: take the last slot and
	// share it rather than trample a live log.
	return SAM_MAX_INSTANCES;
}

void SAMLogger::releaseInstanceSlot()
{
#ifdef _WIN32
	if ( samInstanceLock != INVALID_HANDLE_VALUE )
	{
		CloseHandle(samInstanceLock);
		samInstanceLock = INVALID_HANDLE_VALUE;
	}
#else
	if ( samInstanceLock >= 0 )
	{
		close(samInstanceLock); // closing drops the fcntl lock
		samInstanceLock = -1;
	}
#endif
	instanceIndex = 1;
}

/*-------------------------------------------------------------------------------
	Per-session log files. sam_log.txt is always the CURRENT run; previous runs are
	archived beside it in sam_logs/ so a specific session can be found by date.
-------------------------------------------------------------------------------*/

// "<dir>/sam_log.txt" -> "<dir>/sam_logs"
static std::string samLogArchiveDir(const std::string& logPath)
{
	const size_t slash = logPath.find_last_of("/\\");
	const std::string dir = ( slash == std::string::npos ) ? std::string(".") : logPath.substr(0, slash);
	return dir + "/sam_logs";
}

static void samEnsureDir(const std::string& dir)
{
#ifdef _WIN32
	_mkdir(dir.c_str());
#else
	mkdir(dir.c_str(), 0755);
#endif
}

// Recover "0041_2026-07-23_21-18" from a session banner, so an archived file is named for
// the run it contains rather than the run doing the archiving. Falls back to the number
// alone if the banner is missing or in an older format.
static std::string samExtractSessionStamp(const std::string& content, int fallbackNumber)
{
	std::string num, date;
	const size_t sPos = content.find("Session #");
	if ( sPos != std::string::npos )
	{
		size_t i = sPos + 9;
		while ( i < content.size() && content[i] >= '0' && content[i] <= '9' ) { num += content[i++]; }
		// The banner reads: Session #N - YYYY-MM-DD HH:MM:SS
		const size_t dash = content.find(" - ", i);
		if ( dash != std::string::npos && dash < i + 8 )
		{
			const size_t eol = content.find(char(10), i);
			std::string when = content.substr(dash + 3, ( eol == std::string::npos ? 19 : std::min<size_t>(19, eol - dash - 3) ));
			for ( char& c : when )
			{
				if ( c == ':' ) { c = '-'; }
				else if ( c == ' ' ) { c = '_'; }
			}
			// keep YYYY-MM-DD_HH-MM
			if ( when.size() > 16 ) { when = when.substr(0, 16); }
			date = when;
		}
	}
	if ( num.empty() ) { num = std::to_string( fallbackNumber > 0 ? fallbackNumber : 0 ); }
	while ( num.size() < 4 ) { num = "0" + num; }
	return date.empty() ? num : ( num + "_" + date );
}

// Keep the newest `keep` archived sessions, delete the rest. Names sort chronologically
// because they lead with a zero-padded session number.
static void samPruneArchive(const std::string& dir, int keep)
{
	std::vector<std::string> files;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	const std::string pattern = dir + "/session_*.txt";
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if ( h != INVALID_HANDLE_VALUE )
	{
		do { files.push_back(fd.cFileName); } while ( FindNextFileA(h, &fd) );
		FindClose(h);
	}
#else
	if ( DIR* d = opendir(dir.c_str()) )
	{
		while ( struct dirent* e = readdir(d) )
		{
			const std::string n = e->d_name;
			if ( n.rfind("session_", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".txt" )
			{
				files.push_back(n);
			}
		}
		closedir(d);
	}
#endif
	if ( (int)files.size() <= keep ) { return; }
	std::sort(files.begin(), files.end());
	const int remove = (int)files.size() - keep;
	for ( int i = 0; i < remove; ++i )
	{
		std::remove((dir + "/" + files[i]).c_str());
	}
}

void SAMLogger::rotateAndOpen(const std::string& path)
{
	// ONE SESSION PER FILE. sam_log.txt used to accumulate five sessions, which meant
	// scrolling past three dead runs to reach the one you just played. Now sam_log.txt is
	// always exactly the current run, and the previous run is moved into sam_logs/ first.
	//
	//   sam_log.txt                              <- the run you just did, nothing else
	//   sam_logs/session_0041_2026-07-23_21-18.txt   <- the one before, and so on
	//
	// The archive keeps the newest KEEP_ARCHIVE files and deletes the rest, so the folder
	// cannot grow without bound.
	const int KEEP_ARCHIVE = 12;

	// Move the previous session aside, naming it by ITS session number and start time so
	// the folder sorts chronologically and a run is findable by when it happened.
	{
		std::ifstream prev(path.c_str(), std::ios::binary);
		if ( prev.good() )
		{
			std::string content((std::istreambuf_iterator<char>(prev)), std::istreambuf_iterator<char>());
			prev.close();
			if ( !content.empty() )
			{
				const std::string dir = samLogArchiveDir(path);
				samEnsureDir(dir);
				// Recover the previous run's number + date from its own banner, so the
				// archived name describes that run rather than this one.
				const std::string stamp = samExtractSessionStamp(content, sessionNumber - 1);
				const std::string dest = dir + "/session_" + stamp + ".txt";
				std::ofstream out(dest.c_str(), std::ios::binary | std::ios::trunc);
				if ( out ) { out << content; out.close(); }
				samPruneArchive(dir, KEEP_ARCHIVE);
			}
		}
	}

	// Truncate, not append: this file is one session now.
	logFile.open(path.c_str(), std::ios::out | std::ios::trunc);
}

void SAMLogger::writeSessionHeader()
{
	// THE FIRST LINE of a second copy's log says so, before anything else, because the
	// person reading it opened sam_log_2.txt without knowing why it exists.
	if ( instanceIndex > 1 )
	{
		samEmitPlain(logFile, "S.A.M instance " + std::to_string(instanceIndex)
			+ ": this is copy " + std::to_string(instanceIndex)
			+ " of the game running from this folder, writing sam_log"
			+ samInstanceSuffix(instanceIndex) + ".txt."
			+ " The first copy owns sam_log.txt and nothing here touched it.");
	}
	samEmitPlain(logFile, "");
	samEmitPlain(logFile, std::string(GX_TL) + samRepeat(GX_H, SAM_BOX_INNER) + GX_TR);
	samEmitPlain(logFile, samBoxLine(std::string("S.A.M Framework v") + SAM_FRAMEWORK_VERSION));
	samEmitPlain(logFile, samBoxLine("Session #" + std::to_string(sessionNumber) + " - " + getDateTimeStamp()));
	samEmitPlain(logFile, samBoxLine(std::string("Barony v") + SAM_BARONY_TARGET + " - PID " + std::to_string(samProcessId())
		+ ( instanceIndex > 1 ? ( " - instance " + std::to_string(instanceIndex) ) : std::string() )));
	samEmitPlain(logFile, std::string(GX_BL) + samRepeat(GX_H, SAM_BOX_INNER) + GX_BR);
	// Outside the box on purpose: the box is SAM_BOX_INNER (54) columns and samBoxLine
	// TRUNCATES anything longer, which would silently cut a URL in half.
	//
	// This line exists because most of the API was invisible for a long time: the framework
	// shipped 184 functions while the public guide described 48, and a function nobody can
	// find is not a feature. This is the only channel that reaches a modder who never opens
	// GitHub or the Workshop page, since sam_log.txt is what they read when something breaks.
	//
	// No function COUNT here on purpose: a number compiled into the exe is precisely the
	// drift v2.5.1 was written to stop, and it would be wrong the next time a binding lands.
	samEmitPlain(logFile, "  Every function and event, with arguments and return values:");
	samEmitPlain(logFile, "  https://github.com/spider12223/SAM-Framework/blob/main/docs/function-reference.md");
	samEmitPlain(logFile, "");
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
	// Which copy of the game in this folder are we? (See claimInstanceSlot.)
	instanceIndex = claimInstanceSlot(dir);
	const std::string path = dir + "sam_log" + samInstanceSuffix(instanceIndex) + ".txt";

	// SAY IT SOMEWHERE THE READER IS ALREADY LOOKING. The banner below names the instance,
	// but it is written INSIDE the file nobody opened -- and a session that landed on another
	// slot is exactly the session whose log goes missing, because everyone (the user, support,
	// the ship gate) opens sam_log.txt. Barony's own log is stderr after openLogFile()'s
	// freopen, so one fprintf here lands in <outputdir>/log.txt with no dependency on any
	// Barony header, which this file deliberately has none of.
	if ( instanceIndex != 1 )
	{
		fprintf(stderr, "[S.A.M] this copy of the game is instance %d: its log is %s,"
			" NOT sam_log.txt (another copy of the game in this folder holds that one).\n",
			instanceIndex, path.c_str());
		fflush(stderr);
	}

	if ( instanceIndex == 1 )
	{
		sessionNumber = bumpSessionCounter(dir);
		rotateAndOpen(path); // archives the previous run, then opens this one's file
	}
	else
	{
		// A second copy of the game, started to test multiplayer alone. It must not
		// touch anything the first copy owns: no archiving, no pruning of sam_logs/,
		// and no bump of the shared session counter -- both copies report the same
		// session number, which is what a reader comparing the two logs wants.
		// Truncating THIS file is right: it belongs to this slot, and holds a dead run.
		sessionNumber = readSessionCounter(dir);
		logFile.open(path.c_str(), std::ios::out | std::ios::trunc);
	}

	initialized = true;
	summaryWritten = false;
	std::atexit(samAtExitSessionSummary); // ensure the SESSION SUMMARY writes on any clean exit
	sessionStart = std::chrono::steady_clock::now();
	modLoadStart = sessionStart;
	phase = Phase::Init;
	warnCount = errorCount = hookCount = hookScriptsTotal = apiCallCount = scriptErrorCount = 0;
	hookTally.clear();
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
	flushRepeatLocked();
	if ( logFile.is_open() )
	{
		logFile.flush();
		logFile.close();
	}
	// Give the slot back, so a re-init in the same process reopens the same file
	// instead of walking to the next one.
	releaseInstanceSlot();
	initialized = false;
}

/*-------------------------------------------------------------------------------
	SAMLogger::log
-------------------------------------------------------------------------------*/
void SAMLogger::log(SAMLogLevel level, const std::string& module, const std::string& message)
{
	if ( level == SAMLogLevel::Debug && !debugMode ) { return; }

	std::lock_guard<std::mutex> lock(logMutex);

	// REPEAT COLLAPSING. A per-frame or per-input hook emits the identical line hundreds of
	// times in a row -- one real session was 80% "Dispatched 'on_action_pressed' to 1
	// script(s)" -- which buries everything that actually happened. Identical consecutive
	// messages are counted instead of repeated, and the total is flushed as a single line
	// when something different comes along. Warnings and errors are NEVER collapsed: if a
	// problem is recurring, seeing it recur is the point.
	if ( level != SAMLogLevel::Warn && level != SAMLogLevel::Error )
	{
		if ( module == repeatModule && message == repeatMessage )
		{
			++repeatCount;
			return;
		}
	}
	flushRepeatLocked();
	repeatModule  = ( level == SAMLogLevel::Warn || level == SAMLogLevel::Error ) ? std::string() : module;
	repeatMessage = ( level == SAMLogLevel::Warn || level == SAMLogLevel::Error ) ? std::string() : message;
	repeatLevel   = level;
	repeatCount   = 0;

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
	// Which hooks, and how often. This replaces the per-dispatch log lines: a hook firing is
	// routine, and the count is far more useful than the individual events -- "fired 62
	// times" tells you something, 62 identical lines do not.
	if ( !hookTally.empty() )
	{
		std::vector<std::pair<long long, std::string>> byCount;
		for ( const auto& kv : hookTally ) { byCount.push_back(std::make_pair(kv.second, kv.first)); }
		std::sort(byCount.begin(), byCount.end(),
			[](const std::pair<long long, std::string>& a, const std::pair<long long, std::string>& b)
			{ return a.first > b.first; });
		samEmitPlain(logFile, "  Hooks by name:");
		int shown = 0;
		for ( const auto& e : byCount )
		{
			if ( shown++ >= 12 ) 
			{
				samEmitPlain(logFile, "                  ... and " + std::to_string((int)byCount.size() - 12) + " more");
				break;
			}
			std::string name = e.second;
			while ( name.size() < 28 ) { name += ' '; }
			samEmitPlain(logFile, "      " + name + std::to_string(e.first));
		}
	}
	samEmitPlain(logFile, "  API calls:      " + std::to_string(apiCallCount));
	samEmitPlain(logFile, "  Script errors:  " + std::to_string(scriptErrorCount));
	samEmitPlain(logFile, "  Warnings:       " + std::to_string(warnCount - warnAtLoadEnd) + " (gameplay)");
	samEmitPlain(logFile, "  Errors:         " + std::to_string(errorCount - errorAtLoadEnd) + " (gameplay)");
	samEmitPlain(logFile, std::string(GX_BL) + samRepeat(GX_H, SAM_BOX_INNER) + GX_BR);
}

/*-------------------------------------------------------------------------------
	Gameplay counters.
-------------------------------------------------------------------------------*/
void SAMLogger::noteHookFired(int scriptsReached, const char* eventName)
{
	std::lock_guard<std::mutex> lock(logMutex);
	++hookCount;
	if ( scriptsReached > 0 ) { hookScriptsTotal += scriptsReached; }
	if ( eventName && *eventName ) { ++hookTally[eventName]; }
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
