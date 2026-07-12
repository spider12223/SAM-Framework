/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_backup.hpp
	Desc: automatic save-file backups while mods are active.

	A broken mod can corrupt a save and cost the player hours. So, whenever a
	modded game is loaded, S.A.M takes a once-per-day snapshot of the player's
	Barony saves BEFORE the session starts — the same safety net SMAPI gives
	Stardew players. Backups go to <outputdir>/sam_backups/<timestamp>_<fp>/
	(outside ./savegames/ so Barony's own save browser never sees them). The last
	5 are kept; older ones are pruned. Backup failure (disk full, permissions)
	is logged and ignored — it must NEVER block mod loading.

	Game build only (needs Barony's outputdir + filesystem); the call in
	sam_loader.cpp is #ifndef EDITOR-guarded and this .cpp is in GAME_SOURCES only.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

class SAMBackup
{
public:
	// If at least one mod is active and today's backup hasn't been made yet,
	// snapshot every savegames/*.baronysave into a fresh timestamped folder,
	// then prune to the 5 most-recent backups. `modFingerprint` is a short label
	// for the loaded mod set, appended to the folder name. Never throws.
	static void backupIfNeeded(const std::string& modFingerprint);
};
