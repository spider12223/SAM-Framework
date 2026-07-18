/*-------------------------------------------------------------------------------
	S.A.M Framework — Custom status effects registry + engine-hook helpers.
	See sam_effects.hpp for the design + the IRON no-op rule.
-------------------------------------------------------------------------------*/

#include "sam_effects.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include "main.hpp"   // umbrella — provides the FMOD decls stat.hpp needs under USE_FMOD
#include "stat.hpp"   // Stat, getEffectActive, NUMEFFECTS
#ifndef EDITOR
#	include "ui/GameUI.hpp" // StatusEffectQueue_t::StatusEffectDefinitions_t::allEffects (HUD display)
#endif

#include <fstream>
#include <sstream>
#include <map>

using nlohmann::json;
static const char* MOD = "EFFECTS";

namespace
{
	// A mod-folder-relative path joined onto the mod's absolute dir (copy of the shared helper).
	std::string joinPath(const std::string& dir, const std::string& file)
	{
		if ( dir.empty() ) { return file; }
		if ( file.empty() ) { return dir; }
		const char last = dir[dir.size() - 1];
		if ( last == '/' || last == '\\' ) { return dir + file; }
		return dir + "/" + file;
	}
	bool readWholeFile(const std::string& path, std::string& out)
	{
		std::ifstream f(path.c_str(), std::ios::binary);
		if ( !f.good() ) { return false; }
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}

	// Registry — EMPTY in vanilla (the whole no-op guarantee).
	std::map<int, SAMEffectDef> s_bySlot;       // engine slot 135..159 -> def
	std::map<std::string, int>  s_byName;        // "ns:effect" -> slot
	int s_nextSlot = SAM_EFFECT_SLOT_BASE;
}

void SAMEffects::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const std::string& relPath : manifest.effects )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "Effect file not found: " + path + " (declared by [" + manifest.ns + "])");
			continue;
		}
		const std::string fileLabel = SAMErrors::displayFile(manifest.ns, relPath);
		json j;
		try { j = json::parse(text); }
		catch ( const json::parse_error& e )
		{
			SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "effect not loaded.");
			continue;
		}
		if ( !j.is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
				"a JSON object: { ... }", "wrap the file contents in { }", "effect not loaded.");
			continue;
		}

		auto getStr = [&](const char* k) -> std::string {
			auto it = j.find(k);
			return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
		};
		auto getInt = [&](const char* k, int dv) -> int {
			auto it = j.find(k);
			return (it != j.end() && it->is_number()) ? it->get<int>() : dv;
		};
		auto getBool = [&](const char* k, bool dv) -> bool {
			auto it = j.find(k);
			return (it != j.end() && it->is_boolean()) ? it->get<bool>() : dv;
		};
		auto getNum = [&](const char* k, double dv) -> double {
			auto it = j.find(k);
			return (it != j.end() && it->is_number()) ? it->get<double>() : dv;
		};

		SAMEffectDef def;
		def.id = getStr("id");
		if ( def.id.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/id", "", "missing (required)",
				"an id in \"namespace:effect\" form, e.g. \"" + manifest.ns + ":frostbite\"",
				"add an \"id\" field", "effect not loaded.");
			continue;
		}
		if ( s_byName.count(def.id) )
		{
			SAM_WARN(MOD, "Duplicate effect id '" + def.id + "' — keeping the first, skipping this one.");
			continue;
		}
		if ( s_nextSlot >= NUMEFFECTS )
		{
			SAM_ERROR(MOD, "Custom-effect slots full (" + std::to_string(NUMEFFECTS - SAM_EFFECT_SLOT_BASE)
				+ " max) — skipping '" + def.id + "'.");
			continue;
		}

		def.name = getStr("name").empty() ? def.id : getStr("name");
		def.tooltip = getStr("tooltip");
		def.hpPerSecond = getInt("hp_per_second", 0);
		def.mpPerSecond = getInt("mp_per_second", 0);
		def.moveSpeedMult = getNum("move_speed_mult", 1.0);
		def.defaultDurationTicks = getInt("duration_ticks", 0);
		def.hudHidden = getBool("hud_hidden", false);
		def.modNamespace = manifest.ns;
		def.modPath = manifest.modPath;
		if ( !getStr("icon").empty() ) { def.icon = joinPath(manifest.modPath, getStr("icon")); }

		// stat_modifiers: { "STR": 5, "DEX": -3, ... }
		auto sm = j.find("stat_modifiers");
		if ( sm != j.end() && sm->is_object() )
		{
			static const char* const kKeys[6] = { "STR", "DEX", "CON", "INT", "PER", "CHR" };
			for ( int i = 0; i < 6; ++i )
			{
				auto v = sm->find(kKeys[i]);
				if ( v != sm->end() && v->is_number() ) { def.attr[i] = v->get<int>(); }
			}
		}

		def.slot = s_nextSlot++;
		s_bySlot[def.slot] = def;
		s_byName[def.id] = def.slot;
		SAM_INFO(MOD, "Registered effect: " + def.name + " [" + def.id + "] -> slot " + std::to_string(def.slot)
			+ " (STR " + std::to_string(def.attr[0]) + " DEX " + std::to_string(def.attr[1])
			+ " CON " + std::to_string(def.attr[2]) + " INT " + std::to_string(def.attr[3])
			+ " PER " + std::to_string(def.attr[4]) + " CHR " + std::to_string(def.attr[5])
			+ ", speed x" + std::to_string(def.moveSpeedMult) + ")");
	}
}

void SAMEffects::clear()
{
	s_bySlot.clear();
	s_byName.clear();
	s_nextSlot = SAM_EFFECT_SLOT_BASE;
}

int  SAMEffects::count() { return static_cast<int>(s_bySlot.size()); }
bool SAMEffects::any()   { return !s_bySlot.empty(); }

int SAMEffects::idForName(const std::string& id)
{
	auto it = s_byName.find(id);
	return (it != s_byName.end()) ? it->second : -1;
}

const SAMEffectDef* SAMEffects::get(int slot)
{
	auto it = s_bySlot.find(slot);
	return (it != s_bySlot.end()) ? &it->second : nullptr;
}

std::string SAMEffects::nameForSlot(int slot)
{
	auto it = s_bySlot.find(slot);
	return (it != s_bySlot.end()) ? it->second.id : std::string();
}

int SAMEffects::defaultDurationForSlot(int slot)
{
	auto it = s_bySlot.find(slot);
	return (it != s_bySlot.end()) ? it->second.defaultDurationTicks : 0;
}

int SAMEffects::sumAttrMod(const Stat* s, int attrIndex)
{
	if ( !s || attrIndex < 0 || attrIndex > 5 || s_bySlot.empty() ) { return 0; }
	int total = 0;
	for ( const auto& kv : s_bySlot )
	{
		if ( s->getEffectActive(kv.first) ) { total += kv.second.attr[attrIndex]; }
	}
	return total;
}

double SAMEffects::speedMult(const Stat* s)
{
	if ( !s || s_bySlot.empty() ) { return 1.0; }
	double mult = 1.0;
	for ( const auto& kv : s_bySlot )
	{
		if ( kv.second.moveSpeedMult != 1.0 && s->getEffectActive(kv.first) )
		{
			mult *= kv.second.moveSpeedMult;
		}
	}
	return mult;
}

void SAMEffects::reapplyDisplayEntries()
{
#ifndef EDITOR
	// A neutral vanilla status-fx icon so a custom effect always renders SOMETHING on the HUD
	// (per-effect custom icons are a later slice — they need the absolute-path Image::get flow).
	static const char* const kFallbackIcon = "*images/ui/HUD/statusfx/bellbuff.png";
	for ( const auto& kv : s_bySlot )
	{
		const SAMEffectDef& d = kv.second;
		auto& e = StatusEffectQueue_t::StatusEffectDefinitions_t::allEffects[d.slot];
		e.effect_id = d.slot;
		e.internal_name = d.id;
		e.name = d.name;
		e.desc = d.tooltip;
		e.imgPath = kFallbackIcon;
		e.neverDisplay = d.hudHidden;
		// getName(variation>=0)/getDesc index the *Variations vectors; give them one entry each
		// so the HUD can never index an empty vector out of bounds.
		e.nameVariations.clear();      e.nameVariations.push_back(d.name);
		e.descVariations.clear();      e.descVariations.push_back(d.tooltip);
		e.imgPathVariations.clear();   e.imgPathVariations.push_back(kFallbackIcon);
	}
	if ( !s_bySlot.empty() )
	{
		SAM_INFO(MOD, "Re-applied " + std::to_string(s_bySlot.size()) + " custom-effect HUD entries.");
	}
#endif
}
