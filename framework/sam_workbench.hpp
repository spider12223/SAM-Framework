/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_workbench.hpp
	Desc: the framework's own built-in crafting bench, the Hunter's Workbench.

	Every other custom kit belongs to a mod: the mod ships the item, the art, and the
	recipes. That means two mods wanting a shared bench each have to ship their own.
	This one belongs to the FRAMEWORK, so a mod only has to do two things:

	  1. name it as a recipe's kit --  "kit": "sam:hunters_workbench"
	  2. put it in a class's starting gear, like any other item id

	and its item, its models and its panel art all already exist. There is no limit on
	how many recipes bind to it beyond the 5x4 grid every kit shares.

	IDS. The bench is pinned to a FIXED id in a reserved band above the mod range
	(SAM_ITEM_HUNTERS_WORKBENCH), never allocated from the mod counter. Item ids are
	written into save files as raw numbers, so an id that moved with load order would
	silently rewrite what a player's saved items mean. Pinning it above the mod range
	also means no existing save can already contain the number: every shipped exe
	clamps an out-of-range type to GEM_ROCK on load.

	NO-OP. Nothing here runs unless at least one mod is loaded. With no mod the item is
	never registered, the kit set stays empty, and not one byte is written to disk.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

namespace SAMWorkbench
{
	// Absolute directory the embedded assets are written to. Deliberately NOT under
	// mods/, because the main menu lists every mods/ subdirectory as a loadable local
	// mod and the bench's art is not a mod.
	std::string assetDir();

	// Write out any embedded asset that is missing or the wrong size, and register the
	// bench item + its kit binding. Call once per mod-load cycle, and ONLY when at
	// least one mod is loaded. Safe to call again; unchanged files are left alone.
	void install();

	// Drop the registration (the item registry and recipe registry are cleared by their
	// own clear() calls; this only resets what this module owns).
	void clear();
}
