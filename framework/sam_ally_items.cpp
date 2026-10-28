/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_ally_items.cpp
	Desc: gear on allies. See sam_ally_items.hpp.

-------------------------------------------------------------------------------*/

// Must precede every include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_ally_items.hpp"
#include "sam_party.hpp"

#include "main.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "entity.hpp"
#include "monster.hpp"

namespace SAMAllyItems
{
	SamValue toValue(const Result& r)
	{
		SamValue v = SamValue::object();
		v.set("ok", SamValue::boolean(r.ok));
		v.set("reason", SamValue::string(r.reason));
		v.set("text", SamValue::string(r.text));
		if ( r.ticket ) { v.set("ticket", SamValue::integer((long long)r.ticket)); }
		return v;
	}

	int leaderOf(std::uint32_t uid)
	{
		if ( uid == 0 ) { return -1; }
		return SAMParty::followerLeader(uidToEntity(uid));
	}
}
