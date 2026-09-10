/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_combat.cpp
	Desc: batch 4 — species damage resistance + the on_damage_multiplier hook.
	      See sam_combat.hpp for why neither of these can be a plain binding.

-------------------------------------------------------------------------------*/

#include "sam_combat.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "sam_logger.hpp"
#include "sam_event.hpp"

#include "main.hpp"     // umbrella -- establishes the engine context monster.hpp needs
#include "monster.hpp"  // NUMMONSTERS, NOTHING, DamageTableType

namespace SAMCombat
{
	// ---- species damage resistance ------------------------------------------------------

	// Keyed by species*8 + damageType. A map rather than a NUMMONSTERS*7 array because the
	// common case is a handful of entries and the read has to be cheap when it is EMPTY,
	// which anySpeciesResist() answers without touching this at all.
	static std::map<int, double> s_speciesResist;

	// The number of DamageTableType members (monster.hpp:554) and the width of every
	// damagetables row. Named rather than repeated so the two stay one fact.
	static const int SAM_DAMAGE_TYPE_COUNT = 7;

	static bool samSpeciesInRange(int species)
	{
		// NOTHING is the 0 sentinel and is not a creature anyone fights.
		return species > 0 && species < NUMMONSTERS;
	}

	bool setSpeciesResist(int species, int damageType, double multiplier)
	{
		if ( !samSpeciesInRange(species) )
		{
			SAM_WARN("COMBAT", "sam_set_species_damage_resist: " + std::to_string(species)
				+ " is not a creature this game has.");
			return false;
		}
		if ( damageType < 0 || damageType >= SAM_DAMAGE_TYPE_COUNT )
		{
			SAM_WARN("COMBAT", "sam_set_species_damage_resist: damage type " + std::to_string(damageType)
				+ " is not one of the seven the engine has (sword, mace, axe, polearm, ranged,"
				" magic, unarmed).");
			return false;
		}
		// Not finite, and negative, are two different mistakes with the same result: the
		// damage the engine computes stops being a number. Refuse both loudly rather than
		// clamping, because a NaN here would not show up until something died wrong.
		if ( !std::isfinite(multiplier) || multiplier < 0.0 )
		{
			SAM_WARN("COMBAT", "sam_set_species_damage_resist: the multiplier must be a finite"
				" number and cannot be negative. 0 means immune, 1 is normal, 2 is double damage taken.");
			return false;
		}
		// 20 is arbitrary but not decoration: this number multiplies a damage figure that is
		// then narrowed to int at several call sites, and a mod that typed 1e9 would wrap it.
		if ( multiplier > 20.0 )
		{
			SAM_WARN("COMBAT", "sam_set_species_damage_resist: clamped to 20. Past that the"
				" damage figure this multiplies overflows the int the engine stores it in.");
			multiplier = 20.0;
		}
		s_speciesResist[species * 8 + damageType] = multiplier;
		return true;
	}

	int clearSpeciesResist(int species, int damageType)
	{
		if ( !samSpeciesInRange(species) ) { return 0; }
		if ( damageType < 0 || damageType >= SAM_DAMAGE_TYPE_COUNT )
		{
			// Every type for this species.
			int removed = 0;
			for ( int t = 0; t < SAM_DAMAGE_TYPE_COUNT; ++t )
			{
				removed += (int)s_speciesResist.erase(species * 8 + t);
			}
			return removed;
		}
		return (int)s_speciesResist.erase(species * 8 + damageType);
	}

	int clearAllSpeciesResist()
	{
		const int n = (int)s_speciesResist.size();
		s_speciesResist.clear();
		return n;
	}

	bool anySpeciesResist()
	{
		return !s_speciesResist.empty();
	}

	bool speciesResist(int species, int damageType, double* out)
	{
		if ( s_speciesResist.empty() || !out ) { return false; }
		// The engine indexes damagetables with myStats.type, which for a CUSTOM RACE player is
		// the host monster body it was built on -- always a real species. It is still bounded
		// here, because a species out of range would be a lookup we invented, not one the
		// engine made.
		if ( !samSpeciesInRange(species) ) { return false; }
		if ( damageType < 0 || damageType >= SAM_DAMAGE_TYPE_COUNT ) { return false; }
		auto it = s_speciesResist.find(species * 8 + damageType);
		if ( it == s_speciesResist.end() ) { return false; }
		*out = it->second;
		return true;
	}

	// ---- the on_damage_multiplier hook --------------------------------------------------

	static bool                s_multActive = false;
	static std::vector<double> s_multContributions;

	bool multiplierHookActive() { return s_multActive; }

	void addMultiplier(double fraction)
	{
		if ( !s_multActive ) { return; }
		if ( !std::isfinite(fraction) ) { return; }
		// One contribution is bounded so that the SUM cannot be driven anywhere silly by a
		// single typo; the combined result is clamped again below.
		if ( fraction >  20.0 ) { fraction =  20.0; }
		if ( fraction < -20.0 ) { fraction = -20.0; }
		s_multContributions.push_back(fraction);
	}

	bool fireDamageMultiplierHook(long long targetUid, long long attackerUid,
		double& mult, int damageType, long long projectileUid, int spellID)
	{
		// Nothing loaded: one bool test and out. This runs inside damage resolution, several
		// times per swing in the worst case, so it has to cost nothing in a vanilla game.
		if ( !SamEvent::anyScripts() ) { return false; }

		// NESTING IS REFUSED. A handler is free to deal damage, which re-enters the engine's
		// damage path and arrives back here. Without this the inner dispatch would take over
		// the contribution list and the outer hit would apply somebody else's numbers.
		if ( s_multActive ) { return false; }
		if ( !std::isfinite(mult) ) { return false; }

		const double before = mult;

		s_multActive = true;
		s_multContributions.clear();

		SamEvent e("on_damage_multiplier");
		e.i("target_uid",   targetUid);
		e.i("attacker_uid", attackerUid);
		e.i("damage_type",  (long long)damageType);
		// The event bus carries long long and std::string only, so a fractional multiplier
		// crosses as thousandths: 1000 is normal damage, 1500 is +50%, 0 is immune. A handler
		// may assign this field directly, which overrides every contribution.
		e.i("multiplier_x1000", (long long)std::lround(before * 1000.0));
		e.i("projectile_uid", projectileUid);
		e.i("spell_id",       (long long)spellID);

		const bool allowed = e.fire();

		s_multActive = false;

		// Positives SUM, negatives MULTIPLY -- the engine's own rule for its allBonuses pool
		// (entity.cpp:32010-32022). Two mods each adding +0.2 give +40%, not +44%; two each
		// halving give a quarter, not nothing. Copying the rule means a script author who has
		// read Barony's numbers is not surprised by ours.
		double summed = 0.0;
		double multiplied = 1.0;
		for ( double v : s_multContributions )
		{
			if ( v > 0.0 )       { summed += v; }
			else if ( v < 0.0 )  { multiplied *= (1.0 + v); }
		}
		s_multContributions.clear();
		if ( multiplied < 0.0 ) { multiplied = 0.0; }   // a contribution of exactly -1 zeroes it
		double after = before * multiplied + summed;

		// An assignment to the field itself is ABSOLUTE and wins over the contributions.
		// Compared against what we PUT IN, not against `after`: the write-back store always
		// holds this key because we seeded it, so a fallback comparison could never tell
		// "untouched" from "set to the same value".
		const long long seeded  = (long long)std::lround(before * 1000.0);
		const long long written = e.get("multiplier_x1000", seeded);
		if ( written != seeded ) { after = (double)written / 1000.0; }

		// A handler returning false means this hit does nothing, the same meaning `return
		// false` already has in on_before_damage.
		if ( !allowed ) { after = 0.0; }

		if ( !std::isfinite(after) || after < 0.0 ) { after = 0.0; }
		if ( after > 20.0 ) { after = 20.0; }

		if ( after == before ) { return false; }
		mult = after;
		return true;
	}

	void clear()
	{
		s_speciesResist.clear();
		s_multActive = false;
		s_multContributions.clear();
	}
}
