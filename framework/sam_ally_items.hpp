/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_ally_items.hpp
	Desc: gear on allies (unreleased). Read what a monster carries and wears, ask whether an
	      item may be worn, and move the SAME item between a player's backpack, an ally's
	      pockets and its worn slots. See docs/ally-items.md.

	      Game build only (GAME_SOURCES): calls entity and monster code. Host only: monsters
	      are the host's.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>

#include "sam_value.hpp"

namespace SAMAllyItems
{
	// What a call answers: ok, or the reason it was refused (a short code a script can compare)
	// and a sentence for a player. ticket is set by a give/take that was accepted.
	struct Result
	{
		bool ok = false;
		std::string reason;
		std::string text;
		std::uint32_t ticket = 0;
	};

	// { ok, reason, text }, plus ticket when it is not 0.
	SamValue toValue(const Result& r);

	// The player index (0..3) a creature follows, or -1 for a creature that follows nobody
	// (a wild monster, a player, a uid that is nothing).
	int leaderOf(std::uint32_t uid);
}
