/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_backup.cpp
	Desc: implementation of the automatic save-file backup.

	Game build only (GAME_SOURCES); caller is #ifndef EDITOR-guarded.

-------------------------------------------------------------------------------*/

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_backup.hpp"
#include "sam_logger.hpp"

#include "files.hpp"   // outputdir

#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <system_error>

namespace fs = std::filesystem;

static const char* MOD = "BACKUP";
static const int MAX_BACKUPS = 5;

static std::string joinPath(const std::string& dir, const std::string& file)
{
	if ( dir.empty() ) { return file; }
	const char back = dir.back();
	return (back == '/' || back == '\\') ? (dir + file) : (dir + "/" + file);
}

// FNV-1a 8-hex-char hash of the fingerprint string, for a compact folder suffix.
static std::string shortHash(const std::string& s)
{
	unsigned int h = 2166136261u;
	for ( char c : s )
	{
		h ^= static_cast<unsigned char>(c);
		h *= 16777619u;
	}
	char buf[16];
	snprintf(buf, sizeof(buf), "%08x", h);
	return std::string(buf);
}

// Local date "YYYY-MM-DD" and full stamp "YYYY-MM-DD_HH-MM-SS".
static void nowStamps(std::string& dateOut, std::string& stampOut)
{
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char d[16], s[32];
	std::strftime(d, sizeof(d), "%Y-%m-%d", &tmv);
	std::strftime(s, sizeof(s), "%Y-%m-%d_%H-%M-%S", &tmv);
	dateOut = d;
	stampOut = s;
}

void SAMBackup::backupIfNeeded(const std::string& modFingerprint)
{
	std::error_code ec;

	const std::string saveDir = joinPath(std::string(outputdir), "savegames");
	const std::string backupRoot = joinPath(std::string(outputdir), "sam_backups");

	// Gather the actual save files (*.baronysave). Nothing to back up? bail quietly.
	std::vector<fs::path> saves;
	if ( fs::exists(saveDir, ec) )
	{
		for ( const auto& entry : fs::directory_iterator(saveDir, ec) )
		{
			if ( ec ) { break; }
			if ( entry.is_regular_file(ec) && entry.path().extension() == ".baronysave" )
			{
				saves.push_back(entry.path());
			}
		}
	}
	if ( saves.empty() )
	{
		SAM_DEBUG(MOD, "No .baronysave files to back up — skipping.");
		return;
	}

	std::string today, stamp;
	nowStamps(today, stamp);

	// Once per day: if any existing backup folder starts with today's date, skip.
	if ( fs::exists(backupRoot, ec) )
	{
		for ( const auto& entry : fs::directory_iterator(backupRoot, ec) )
		{
			if ( ec ) { break; }
			if ( entry.is_directory(ec) )
			{
				const std::string name = entry.path().filename().string();
				if ( name.rfind(today, 0) == 0 ) // starts with "YYYY-MM-DD"
				{
					SAM_DEBUG(MOD, "Saves already backed up today (" + name + ") — skipping.");
					return;
				}
			}
		}
	}

	// Create this backup's folder: sam_backups/<stamp>_<fingerprint-hash>/
	const std::string folderName = stamp + "_" + shortHash(modFingerprint);
	const std::string dest = joinPath(backupRoot, folderName);
	fs::create_directories(dest, ec);
	if ( ec )
	{
		SAM_WARN(MOD, "Could not create backup folder " + dest + " (" + ec.message()
			+ ") — skipping backup (mod loading continues normally).");
		return;
	}

	int copied = 0;
	for ( const fs::path& src : saves )
	{
		std::error_code cec;
		fs::copy_file(src, fs::path(dest) / src.filename(),
			fs::copy_options::overwrite_existing, cec);
		if ( cec )
		{
			SAM_WARN(MOD, "Could not copy " + src.filename().string() + " (" + cec.message() + ").");
		}
		else
		{
			++copied;
		}
	}

	if ( copied == 0 )
	{
		SAM_WARN(MOD, "Backup folder created but no saves copied — leaving it empty.");
		return;
	}
	SAM_INFO(MOD, "Save backed up to sam_backups/" + folderName + "/ (" + std::to_string(copied)
		+ " file(s)) — mods are active.");

	// Prune: keep the MAX_BACKUPS most-recent folders (names sort chronologically).
	std::vector<std::string> folders;
	for ( const auto& entry : fs::directory_iterator(backupRoot, ec) )
	{
		if ( ec ) { break; }
		if ( entry.is_directory(ec) )
		{
			folders.push_back(entry.path().filename().string());
		}
	}
	if ( static_cast<int>(folders.size()) > MAX_BACKUPS )
	{
		std::sort(folders.begin(), folders.end()); // ascending = oldest first
		const int toRemove = static_cast<int>(folders.size()) - MAX_BACKUPS;
		for ( int i = 0; i < toRemove; ++i )
		{
			std::error_code rec;
			fs::remove_all(fs::path(backupRoot) / folders[i], rec);
			if ( !rec )
			{
				SAM_DEBUG(MOD, "Pruned old backup: " + folders[i]);
			}
		}
	}
}
