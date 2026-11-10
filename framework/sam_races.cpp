/*-------------------------------------------------------------------------------
	S.A.M Framework — Custom playable-race registry + application.
	See sam_races.hpp for the design + the graceful-degrade rule.

	IRON no-op rule: with no race mod loaded the registry is empty, so any() is
	false and every hook returns before touching game state — vanilla behaviour is
	byte-identical. A vanilla/mismatched client reading a 200-255 race id maps to
	HUMAN via getMonsterFromPlayerRace's own default, so it degrades to Human.
-------------------------------------------------------------------------------*/

#include "sam_races.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "sam_errors.hpp"
#include "nlohmann/json.hpp"

#include "main.hpp"     // umbrella — provides decls stat.hpp needs
#include "stat.hpp"     // Stat members (STR..CHR, HP/MP, playerRace)
#include "monster.hpp"  // Monster enum, monstertypename[], NUMMONSTERS
#include "player.hpp"   // players[], MAXPLAYERS (applySpells)
#include "entity.hpp"  // Entity::behavior (isRaceHeadOnMonster)
#include "net.hpp"      // multiplayer, CLIENT, intro
#include "mod_tools.hpp" // ItemTooltips.spellItems (vanilla spell-name resolve)
#include "magic/magic.hpp" // addSpell
#include "sam_spells.hpp"  // grant custom "ns:spell" innate race spells
#include "sam_models.hpp"  // SAMModels::modelIndexForId (resolve a race's limb models)
#include "sam_items.hpp"   // SAMItems::resolveModImage (a race's icon PNG)

#include <algorithm>
#include <cmath>
#include <set>
#include <fstream>
#include <sstream>
#include <map>
#include <cctype>

using nlohmann::json;

// Monster TYPES a "recruits" list can never usefully name: bosses, traders, mimics, the tinkering
// bots, holograms and bats. neverAFollower() refuses these by looking at the entity; this is the
// same list by type, so a race file that names one is told at load instead of failing quietly.
static bool samTypeNeverFollows(int m)
{
	return m == SHOPKEEPER || m == MINOTAUR || m == LICH || m == LICH_ICE || m == LICH_FIRE
		|| m == DEVIL || m == SHADOW || m == MIMIC || m == MINIMIMIC || m == SENTRYBOT
		|| m == SPELLBOT || m == GYROBOT || m == DUMMYBOT || m == HOLOGRAM || m == BAT_SMALL;
}
static const char* MOD = "RACES";

namespace
{
	// FNV-1a 32 over the race's "namespace:race" id. The same function the rest of the
	// framework uses for its compact hashes (sam_sync.cpp, sam_backup.cpp); repeated here
	// rather than shared because those two are file-local and this one has to be stable
	// FOREVER -- it is on the wire, so changing it would desynchronise two builds.
	uint32_t samRaceKeyFromIdString(const std::string& s)
	{
		uint32_t h = 2166136261u;
		for ( unsigned char c : s )
		{
			h ^= (uint32_t)c;
			h *= 16777619u;
		}
		// 0 is reserved to mean "no custom race", so nudge the one input that could produce it.
		return ( h == 0u ) ? 1u : h;
	}

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

	// The 18 monster bodies that back the existing playable races — each has a
	// dedicated first-person arm, so a SAM race riding one is correct in BOTH the
	// 3rd-person body and the 1st-person view.
	bool isSupportedHostBody(int monster)
	{
		switch ( monster )
		{
			case HUMAN: case SKELETON: case VAMPIRE: case SUCCUBUS: case INCUBUS:
			case GOBLIN: case AUTOMATON: case INSECTOID: case GOATMAN: case GNOME:
			case GREMLIN: case DRYAD: case MYCONID: case SALAMANDER: case TROLL:
			case SPIDER: case CREATURE_IMP: case RAT:
				return true;
			default:
				return false;
		}
	}
	// Map a monster-type name (case-insensitive) to its Monster enum value, or -1.
	int monsterFromName(const std::string& nameIn)
	{
		std::string want = nameIn;
		for ( char& c : want ) { c = (char)std::tolower((unsigned char)c); }
		for ( int i = 0; i < NUMMONSTERS; ++i ) { if ( want == monstertypename[i] ) { return i; } }
		return -1;
	}

	// The five limbs the player draw path can be told to replace, and the JSON name for
	// each. "head" is deliberately absent: the engine sets the head on the player entity
	// itself, not through setDefaultPlayerModel, so it is carried separately.
	struct LimbSlot { const char* key; int limbType; };
	const LimbSlot kLimbSlots[] = {
		{ "torso",     LIMB_HUMANOID_TORSO    },
		{ "leg_right", LIMB_HUMANOID_RIGHTLEG },
		{ "leg_left",  LIMB_HUMANOID_LEFTLEG  },
		{ "arm_right", LIMB_HUMANOID_RIGHTARM },
		{ "arm_left",  LIMB_HUMANOID_LEFTARM  },
	};

	// "head" is not in kLimbSlots (the engine sets it outside the limb path), so the
	// accepted-name check has to include it explicitly or a valid file would be rejected.
	bool isKnownLimbSlot(const std::string& key)
	{
		if ( key == "head" ) { return true; }
		for ( const LimbSlot& s : kLimbSlots ) { if ( key == s.key ) { return true; } }
		return false;
	}

	// Every model index a registered race uses as a head, and the host body that head was
	// authored for. Rebuilt by resolveLimbModels; consulted by isRaceHeadSprite and by
	// Entity::getMonsterTypeFromSprite; empty in vanilla. Class head overrides keep a
	// separate map with its own lifetime (resolveAppearance owns it).
	std::map<int, int> s_headHost;        // head sprite -> host Monster (races)
	std::map<int, int> s_classHeadHost;   // head sprite -> host Monster (classes)

	// Registry — EMPTY in vanilla (the whole no-op guarantee).
	std::map<int, SAMRaceDef> s_byId;            // runtime id 200..255 -> def
	std::map<uint32_t, int> s_byKey;             // stable cross-machine key -> local runtime id
	std::map<std::string, int> s_byIdString;      // "ns:race" -> id
	int s_nextId = SAM_RACE_ID_BASE;
}

void SAMRaces::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const std::string& relPath : manifest.races )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "Race file not found: " + path + " (declared by [" + manifest.ns + "])");
			continue;
		}
		const std::string fileLabel = SAMErrors::displayFile(manifest.ns, relPath);
		json j;
		try { j = json::parse(text); }
		catch ( const json::parse_error& e )
		{
			SAMErrors::reportSyntax(MOD, fileLabel, text, e.what(), e.byte, "race not loaded.");
			continue;
		}
		if ( !j.is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "(root)", "", "not a JSON object",
				"a JSON object: { ... }", "wrap the file contents in { }", "race not loaded.");
			continue;
		}

		auto getStr = [&](const char* k) -> std::string {
			auto it = j.find(k);
			return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
		};
		auto getBool = [&](const char* k, bool dv) -> bool {
			auto it = j.find(k);
			return (it != j.end() && it->is_boolean()) ? it->get<bool>() : dv;
		};

		SAMRaceDef def;
		def.id = getStr("id");
		if ( def.id.empty() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/id", "", "missing (required)",
				"an id in \"namespace:race\" form, e.g. \"" + manifest.ns + ":frostborn\"",
				"add an \"id\" field", "race not loaded.");
			continue;
		}
		if ( s_byIdString.count(def.id) )
		{
			SAM_WARN(MOD, "Duplicate race id '" + def.id + "' — keeping the first, skipping this one.");
			continue;
		}
		if ( s_nextId > 255 )
		{
			SAM_ERROR(MOD, "Custom-race slots full (" + std::to_string(256 - SAM_RACE_ID_BASE)
				+ " max) — skipping '" + def.id + "'.");
			continue;
		}

		def.name = getStr("name").empty() ? def.id : getStr("name");
		def.description = getStr("description");
		def.modNamespace = manifest.ns;

		// "icon": find the PNG once, here, where a mistake can be reported, the way a spell's icon is.
		// A wrong path used to be impossible to see: the saved-game card and the HUD bars simply kept
		// the host body's icon.
		def.icon = getStr("icon");
		if ( !def.icon.empty() )
		{
			if ( SAMErrors::relPathEscapes(def.icon) )
			{
				SAM_WARN(MOD, "Race [" + def.id + "] icon path '" + def.icon + "' escapes the mod folder; "
					"ignoring it. The host body's icon is used.");
			}
			else
			{
				def.iconResolved = SAMItems::resolveModImage(manifest.modPath, def.icon, "icon", def.id,
					"RACES", "races/frostborn.png", "the host body's icon");
				if ( !def.iconResolved.empty() )
				{
					SAM_INFO(MOD, "Race [" + def.id + "] custom icon -> " + def.iconResolved);
				}
			}
		}

		// host_body: the existing monster whose model this race wears. Restricted to
		// the 18 bodies with a proper first-person arm; anything else falls to HUMAN.
		const std::string hostName = getStr("host_body");
		int host = hostName.empty() ? (int)HUMAN : monsterFromName(hostName);
		if ( host < 0 || !isSupportedHostBody(host) )
		{
			SAM_WARN(MOD, "Race '" + def.id + "' host_body '" + hostName
				+ "' is not a supported player body — falling back to HUMAN.");
			host = (int)HUMAN;
		}
		def.hostMonster = host;
		def.hostBodyName = (host >= 0 && host < NUMMONSTERS) ? monstertypename[host] : "human";

		// stat_modifiers: { "STR": 2, "DEX": -1, "HP": 10, ... } — deltas.
		auto sm = j.find("stat_modifiers");
		if ( sm != j.end() && sm->is_object() )
		{
			auto rd = [&](const char* k, int& out) {
				auto v = sm->find(k);
				if ( v != sm->end() && v->is_number() ) { out = v->get<int>(); }
			};
			rd("STR", def.str); rd("DEX", def.dex); rd("CON", def.con);
			rd("INT", def.intel); rd("PER", def.per); rd("CHR", def.chr);
			rd("HP", def.hp); rd("MP", def.mp);
		}
		def.bloodDiet = getBool("blood_diet", false);

		// vision / sneak_vision: whole tiles of light radius. Clamped rather than refused, with a
		// warning, because a light's cost grows with the square of its radius and the engine's own
		// largest sneaking bonus (a blessed eyepatch on a Gremlin) is +6.
		auto readVision = [&](const char* k, int lo, int hi, int& out) {
			auto v = j.find(k);
			if ( v == j.end() ) { return; }
			if ( !v->is_number() )
			{
				SAM_WARN(MOD, "Race '" + def.id + "' " + k + " is not a number; ignored.");
				return;
			}
			const double d = v->get<double>();
			if ( d < lo || d > hi )
			{
				SAM_WARN(MOD, "Race '" + def.id + "' " + k + " is outside "
					+ std::to_string(lo) + ".." + std::to_string(hi) + "; clamped.");
				out = d < lo ? lo : hi;
				return;
			}
			out = (int)d;   // whole tiles; a fraction is dropped
		};
		readVision("vision", -4, 6, def.vision);
		readVision("sneak_vision", -6, 6, def.sneakVision);

		// allies / enemies: monster-type names, resolved to Monster enum values now so the
		// hot path (checkEnemy, once per entity pair) never touches a string. An unknown
		// name is reported and dropped rather than silently ignored -- a typo'd "goatmen"
		// would otherwise look exactly like a race whose allegiance quietly does nothing.
		auto readMonsterList = [&](const char* key, std::vector<int>& out) {
			auto arr = j.find(key);
			if ( arr == j.end() || !arr->is_array() ) { return; }
			for ( const json& e : *arr )
			{
				if ( !e.is_string() )
				{
					SAMErrors::reportSemantic(MOD, fileLabel, std::string("/") + key, e.dump(),
						"not a string", "a quoted monster name, e.g. \"goatman\"",
						"put quotes around it",
						"that entry ignored; the rest of the race loaded.", true);
					continue;
				}
				const std::string nm = e.get<std::string>();
				const int m = monsterFromName(nm);
				if ( m <= 0 )   // 0 is "nothing", which is not a creature
				{
					SAMErrors::reportSemantic(MOD, fileLabel, std::string("/") + key, nm,
						"not a monster type",
						"a monster name such as \"goatman\", \"gnome\" or \"shopkeeper\"",
						"check the spelling against the monster list in the docs",
						"that entry ignored; the rest of the race loaded.", true);
					continue;
				}
				out.push_back(m);
			}
		};
		// limb_models: kept as written and resolved later — the model table does not exist
		// during mod load, so an index cannot be validated or looked up here.
		auto lm = j.find("limb_models");
		if ( lm != j.end() && lm->is_object() )
		{
			for ( auto it = lm->begin(); it != lm->end(); ++it )
			{
				// A slot name the draw path never asks for would sit in the map doing
				// nothing, which is indistinguishable from a race that declared no body at
				// all. Say so instead: "arm-right" and "rightArm" are the obvious typos and
				// neither is a JSON error, so nothing else would ever catch them.
				if ( !isKnownLimbSlot(it.key()) )
				{
					SAMErrors::reportSemantic(MOD, fileLabel, "/limb_models/" + it.key(), "",
						"not a limb this race can replace",
						"one of: head, torso, arm_right, arm_left, leg_right, leg_left",
						"check the spelling", "that entry ignored; the rest of the race loaded.", true);
					continue;
				}
				// A slot value may be a bare path (the original form) or an object carrying a
				// transform next to the model: { "model": "...", "scale": 1.2, "offset": {...} }.
				// A race hosted on a body of different proportions would otherwise have to
				// re-author its .vox to fit somebody else's skeleton.
				bool samHaveObjForm = false;
				if ( it.value().is_object() )
				{
					const auto& o = it.value();
					if ( !o.contains("model") || !o["model"].is_string() )
					{
						SAMErrors::reportSemantic(MOD, fileLabel, "/limb_models/" + it.key(),
							it.value().dump(), "an object with no \"model\"",
							"an object like { \"model\": \"models/torso.vox\", \"scale\": 1.2 }",
							"add the model", "that limb keeps the host body's model.", true);
						continue;
					}
					samHaveObjForm = true;
					const std::string samLimbPath = o["model"].get<std::string>();
					if ( samLimbPath.empty() ) { continue; }
					def.limbModels[it.key()] = samLimbPath;

					SAMRaceDef::LimbXform xf;
					auto num = [&](const char* k, double d) -> double {
						return ( o.contains(k) && o[k].is_number() ) ? o[k].get<double>() : d;
					};
					// The bent arm, if this race authored one. Reported rather than ignored on a
					// slot that has no bent pose, because a modder who writes it on a leg has
					// misunderstood something and silence would not tell them.
					if ( o.contains("bent") )
					{
						const bool samArmSlot = ( it.key() == "arm_right" || it.key() == "arm_left" );
						if ( !o["bent"].is_string() || o["bent"].get<std::string>().empty() )
						{
							SAMErrors::reportSemantic(MOD, fileLabel, "/limb_models/" + it.key() + "/bent",
								o["bent"].dump(), "not a model reference",
								"a path, a \"<ns>:model\" id, or a model index",
								"give the model this arm uses while it holds something",
								"that arm keeps one pose.", true);
						}
						else if ( !samArmSlot )
						{
							SAMErrors::reportSemantic(MOD, fileLabel, "/limb_models/" + it.key() + "/bent",
								o["bent"].dump(), "a bent pose on a limb that never bends",
								"\"bent\" on arm_right or arm_left",
								"move it to an arm, or remove it", "ignored; the limb loaded.", true);
						}
						else
						{
							def.limbModelsBent[it.key()] = o["bent"].get<std::string>();
						}
					}

					xf.scale = num("scale", 1.0);
					if ( xf.scale <= 0.0 ) { xf.scale = 1.0; }
					xf.pitch = num("pitch", 0.0);
					xf.roll  = num("roll", 0.0);
					if ( o.contains("offset") && o["offset"].is_object() )
					{
						const auto& off = o["offset"];
						if ( off.contains("x") && off["x"].is_number() ) { xf.offX = off["x"].get<double>(); }
						if ( off.contains("y") && off["y"].is_number() ) { xf.offY = off["y"].get<double>(); }
						if ( off.contains("z") && off["z"].is_number() ) { xf.offZ = off["z"].get<double>(); }
					}
					xf.any = ( xf.scale != 1.0 || xf.pitch != 0.0 || xf.roll != 0.0
						|| xf.offX != 0.0 || xf.offY != 0.0 || xf.offZ != 0.0 );
					if ( xf.any )
					{
						// "head" is not in kLimbSlots: the engine sets the head outside the limb
						// path entirely, so there is nowhere to apply a transform to it. Say so
						// rather than accept the keys and silently drop them.
						if ( it.key() == "head" )
						{
							SAMErrors::reportSemantic(MOD, fileLabel, "/limb_models/head", "",
								"a transform on the head",
								"a transform on torso, arm_right, arm_left, leg_right or leg_left",
								"remove the scale/offset/pitch/roll from head",
								"the head model still applies; its transform is ignored.", true);
						}
						for ( const LimbSlot& sl : kLimbSlots )
						{
							if ( it.key() == sl.key ) { def.limbXform[sl.limbType] = xf; break; }
						}
					}
				}

				if ( !samHaveObjForm )
				{
					if ( !it.value().is_string() )
					{
						// A bare 1025 rather than "1025" is the easy mistake, and the schema
						// only catches it for people using the builder.
						SAMErrors::reportSemantic(MOD, fileLabel, "/limb_models/" + it.key(),
							it.value().dump(), "not a string or an object",
							"a quoted model reference, e.g. \"1025\" or \"" + manifest.ns + ":body\"",
							"put quotes around it",
							"that limb keeps the host body's model.", true);
						continue;
					}
					if ( it.value().get<std::string>().empty() ) { continue; }
					def.limbModels[it.key()] = it.value().get<std::string>();
				}
			}
		}

		// "extra_limbs": [ { "slot": "tail", "model": "...", "attach": "body", ... } ]
	if ( j.contains("extra_limbs") )
	{
		if ( !j["extra_limbs"].is_array() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/extra_limbs", "", "not an array",
				"an array of limb objects", "fix or remove it",
				"extra limbs ignored for this race.", true);
		}
		else
		{
			for ( const auto& el : j["extra_limbs"] )
			{
				if ( !el.is_object() || !el.contains("model") || !el["model"].is_string() ) { continue; }
				if ( def.extraLimbs.size() >= 13 )
				{
					SAM_WARN(MOD, "Race [" + def.id + "] declares more than 13 extra limbs. Barony leaves"
						" exactly 13 limb slots unused, so the rest are ignored.");
					break;
				}
				SAMRaceDef::SAMExtraLimb lim;
				lim.model = el["model"].get<std::string>();
				if ( el.contains("attach") && el["attach"].is_string() )
				{
					const std::string a = el["attach"].get<std::string>();
					if ( a == "body" || a == "head" || a == "torso" ) { lim.attach = a; }
					else
					{
						SAM_WARN(MOD, "Race [" + def.id + "] extra limb attach '" + a
							+ "' is not one of body, head, torso -- using body.");
					}
				}
				auto rd = [&](const char* k, double dflt) -> double {
					return ( el.contains(k) && el[k].is_number() ) ? el[k].get<double>() : dflt;
				};
				if ( el.contains("offset") && el["offset"].is_object() )
				{
					const auto& o = el["offset"];
					if ( o.contains("forward") && o["forward"].is_number() ) { lim.offFwd = o["forward"].get<double>(); }
					if ( o.contains("side") && o["side"].is_number() )       { lim.offSide = o["side"].get<double>(); }
					if ( o.contains("up") && o["up"].is_number() )           { lim.offUp = o["up"].get<double>(); }
				}
				if ( el.contains("focal") && el["focal"].is_object() )
				{
					const auto& o = el["focal"];
					if ( o.contains("x") && o["x"].is_number() ) { lim.focalX = o["x"].get<double>(); }
					if ( o.contains("y") && o["y"].is_number() ) { lim.focalY = o["y"].get<double>(); }
					if ( o.contains("z") && o["z"].is_number() ) { lim.focalZ = o["z"].get<double>(); }
				}
				lim.pitch = rd("pitch", 0.0);
				lim.roll = rd("roll", 0.0);
				lim.yawOffsetDeg = rd("yaw_offset", 0.0);
				lim.scale = rd("scale", 1.0);
				if ( lim.scale <= 0.0 ) { lim.scale = 1.0; }
				if ( el.contains("sway") )
				{
					const auto& sw = el["sway"];
					const std::string swayName = sw.is_string() ? sw.get<std::string>() : std::string();
					if ( sw.is_boolean() )
					{
						lim.sway = sw.get<bool>() ? SAMRaceDef::SAMExtraLimb::SwayTilt : SAMRaceDef::SAMExtraLimb::SwayNone;
					}
					else if ( swayName == "tilt" ) { lim.sway = SAMRaceDef::SAMExtraLimb::SwayTilt; }
					else if ( swayName == "side" ) { lim.sway = SAMRaceDef::SAMExtraLimb::SwaySide; }
					else
					{
						SAM_WARN(MOD, "Race [" + def.id + "] extra limb sway must be true, false, \"tilt\" or"
							" \"side\"; this limb does not sway.");
					}
				}
				if ( el.contains("root") )
				{
					using L = SAMRaceDef::SAMExtraLimb;
					static const std::map<std::string, int> kRoots = {
						{ "top", L::RootTop }, { "bottom", L::RootBottom }, { "front", L::RootFront },
						{ "back", L::RootBack }, { "left", L::RootLeft }, { "right", L::RootRight } };
					const auto& rt = el["root"];
					const auto it = rt.is_string() ? kRoots.find(rt.get<std::string>()) : kRoots.end();
					if ( it != kRoots.end() ) { lim.root = it->second; }
					else
					{
						SAM_WARN(MOD, "Race [" + def.id + "] extra limb root must be \"top\", \"bottom\", \"front\","
							" \"back\", \"left\" or \"right\"; this limb turns about its middle.");
					}
				}
				def.extraLimbs.push_back(lim);
			}
		}
	}

	// "first_person": { "arm": "models/x.vox", "hand_left": "models/y.vox" }
	if ( j.contains("first_person") )
	{
		if ( !j["first_person"].is_object() )
		{
			SAMErrors::reportSemantic(MOD, fileLabel, "/first_person", "", "not an object",
				"an object like { \"arm\": \"models/arm.vox\" }", "fix or remove it",
				"first-person models ignored for this race.", true);
		}
		else
		{
			const auto& fp = j["first_person"];
			if ( fp.contains("arm") && fp["arm"].is_string() ) { def.fpArm = fp["arm"].get<std::string>(); }
			if ( fp.contains("hand_left") && fp["hand_left"].is_string() ) { def.fpHandLeft = fp["hand_left"].get<std::string>(); }
		}
	}

	readMonsterList("allies", def.allies);
		readMonsterList("enemies", def.enemies);
		readMonsterList("neutral", def.neutral);
		readMonsterList("recruits", def.recruits);

		// Declaring a type in two lists is a contradiction the author needs to resolve, not
		// something to resolve silently. The least friendly wins (enemies, then neutral, then
		// allies: hostility is the more consequential reading of an ambiguous file), and we say so.
		auto listed = [](const std::vector<int>& v, int m) { return std::find(v.begin(), v.end(), m) != v.end(); };
		for ( int m : def.enemies )
		{
			if ( listed(def.allies, m) || listed(def.neutral, m) )
			{
				SAM_WARN(MOD, "Race '" + def.id + "' lists '" + std::string(monstertypename[m])
					+ "' as an enemy AND as " + (listed(def.allies, m) ? "an ally" : "neutral")
					+ " -- treating it as an enemy.");
			}
		}
		for ( int m : def.neutral )
		{
			if ( listed(def.allies, m) && !listed(def.enemies, m) )
			{
				SAM_WARN(MOD, "Race '" + def.id + "' lists '" + std::string(monstertypename[m])
					+ "' as BOTH an ally and neutral -- treating it as neutral.");
			}
		}

		// "recruits" implies an ally, so a type that is also an enemy or neutral contradicts it,
		// and one no follower can be made of would promise something the game refuses. Say so, and
		// drop it: the race still loads, and the rest of the list stands.
		for ( auto it = def.recruits.begin(); it != def.recruits.end(); )
		{
			const int m = *it;
			const char* why = nullptr;
			if ( samTypeNeverFollows(m) )              { why = "can never be made a follower (a boss, a trader, a mimic, a hologram, a bat or a tinkering bot)"; }
			else if ( listed(def.enemies, m) )         { why = "is also listed under enemies"; }
			else if ( listed(def.neutral, m) )         { why = "is also listed under neutral"; }
			if ( why )
			{
				SAM_WARN(MOD, "Race '" + def.id + "' lists '" + std::string(monstertypename[m])
					+ "' under recruits, but it " + why + " -- removed from recruits.");
				it = def.recruits.erase(it);
			}
			else { ++it; }
		}
		def.canOpenTins = getBool("can_open_tins", false);
		if ( j.contains("starting_spells") && j["starting_spells"].is_array() )
		{
			for ( const json& e : j["starting_spells"] )
			{
				if ( e.is_string() ) { def.startingSpells.push_back(e.get<std::string>()); }
			}
		}

		def.numericId = s_nextId++;
		s_byId[def.numericId] = def;
		s_byIdString[def.id] = def.numericId;

		// The cross-machine key. FNV-1a over the id string the mod author wrote, which is the
		// one name for this race that every machine agrees on. A collision would silently swap
		// two races between players, so it is checked rather than assumed: with a handful of
		// races the odds are negligible, but "negligible" is not a thing to find out about
		// from a bug report.
		{
			const uint32_t key = samRaceKeyFromIdString(def.id);
			auto clash = s_byKey.find(key);
			if ( clash != s_byKey.end() )
			{
				SAM_ERROR(MOD, "Race id [" + def.id + "] hashes to the same cross-machine key as ["
					+ s_byId[clash->second].id + "]. One of the two must be renamed or they will"
					" swap places between players in multiplayer. Keeping the first.");
			}
			else
			{
				s_byKey[key] = def.numericId;
			}
		}
		SAM_INFO(MOD, "Registered race: " + def.name + " [" + def.id + "] -> id "
			+ std::to_string(def.numericId) + " on body " + def.hostBodyName
			+ " (STR " + std::to_string(def.str) + " DEX " + std::to_string(def.dex)
			+ " CON " + std::to_string(def.con) + " INT " + std::to_string(def.intel)
			+ " PER " + std::to_string(def.per) + " CHR " + std::to_string(def.chr)
			+ " HP " + std::to_string(def.hp) + " MP " + std::to_string(def.mp) + ")");
		if ( !def.allies.empty() || !def.enemies.empty() || !def.neutral.empty() || !def.recruits.empty() )
		{
			std::string line = "  " + def.name + " allegiance:";
			if ( !def.allies.empty() )
			{
				line += " allied with";
				for ( int m : def.allies ) { line += " " + std::string(monstertypename[m]); }
			}
			if ( !def.enemies.empty() )
			{
				line += " hostile to";
				for ( int m : def.enemies ) { line += " " + std::string(monstertypename[m]); }
			}
			if ( !def.neutral.empty() )
			{
				line += " neutral to";
				for ( int m : def.neutral ) { line += " " + std::string(monstertypename[m]); }
			}
			if ( !def.recruits.empty() )
			{
				line += " can recruit";
				for ( int m : def.recruits ) { line += " " + std::string(monstertypename[m]); }
			}
			SAM_INFO(MOD, line);
		}
		if ( def.canOpenTins ) { SAM_INFO(MOD, "  " + def.name + " opens tins without a tin opener."); }
	}
}

namespace
{
	// Resolve one model reference three ways, in the order a modder expects: a model this
	// mod declared, then a mod-relative .vox path (which registration turned into an id),
	// then a raw vanilla index.
	//
	// A bad reference is REPORTED, never silently dropped. Both failure modes here are
	// invisible in game -- opengl.cpp draws index 0 and out-of-range indices as nothing at
	// all, with no log line and nothing in /sam_models, because no model was ever
	// registered -- so the message is the only thing standing between a modder and an
	// afternoon of wondering why their arm vanished.
	int resolveModelRef(const std::string& ref, const std::string& raceId, const std::string& slot)
	{
		// 1. a model this mod declared, or a mod-relative .vox path (registration turned
		//    that path into an id already).
		int idx = SAMModels::modelIndexForId(ref);
		if ( idx >= 0 ) { return idx; }

		// 2. a VANILLA model named by its path. Tried before the numeric form because it
		//    is the readable one: "gharbad_head.vox" says what it is, where 1025 says
		//    nothing and is one subtraction away from being a different model entirely.
		bool ambiguous = false;
		idx = SAMModels::vanillaModelIndexForPath(ref, &ambiguous);
		if ( idx >= 0 ) { return idx; }
		if ( ambiguous )
		{
			SAM_ERROR(MOD, "Race [" + raceId + "] limb_models." + slot + " '" + ref
				+ "' matches more than one model — several creatures ship a file with that"
				" name. Give the folder too, e.g."
				" \"models/creatures/goatman/goatman_named/" + ref + "\".");
			return -1;
		}

		// 3. a raw index, still accepted so every mod written before this keeps working.
		char* end = nullptr;
		const long n = std::strtol(ref.c_str(), &end, 10);
		if ( end && *end == '\0' )
		{
			if ( n > 0 && n < (long)nummodels ) { return (int)n; }
			SAM_ERROR(MOD, "Race [" + raceId + "] limb_models." + slot + " is model index "
				+ std::to_string(n) + ", which is "
				+ ( n == 0 ? std::string("models/system/null.vox, the engine's EMPTY model")
				           : std::string("past the end of the table (this game has ")
				             + std::to_string((long long)nummodels) + " models, 1.."
				             + std::to_string((long long)nummodels - 1) + ")" )
				+ ". That limb would draw as nothing, so it is ignored."
				" Remember models.txt line N is index N-1.");
			return -1;
		}

		SAM_WARN(MOD, "Race [" + raceId + "] limb_models." + slot + " '" + ref
			+ "' is not a model this game knows — ignoring it (that limb keeps the host"
			" body's own model). Use a path from models.txt (\"gharbad_head.vox\" or the"
			" full \"models/creatures/...\" form), an id you declared in mod.json"
			" \"models\", or a raw index.");
		return -1;
	}
}

void SAMRaces::resolveLimbModels()
{
	s_headHost.clear();
	for ( auto& kv : s_byId )
	{
		SAMRaceDef& def = kv.second;
		def.limbModelIdx.clear();
		def.limbBentIdx.clear();
		def.headModelIdx = -1;
		def.fpArmIdx = -1;
		def.fpHandLeftIdx = -1;
		// Only the limb/head resolution below needs limb_models. extra_limbs and the
		// first-person models are independent, and short-circuiting on an empty limb map meant
		// a race that declared ONLY a tail or ONLY a first-person arm resolved neither: the
		// .vox loaded, the index was never looked up, and nothing drew.
		if ( def.limbModels.empty() && def.extraLimbs.empty()
			&& def.fpArm.empty() && def.fpHandLeft.empty() ) { continue; }

		// A player on a RAT or SPIDER host body is not animated as a humanoid: actplayer sets
		// isHumanoid = false for those two, and the whole bodypart loop that calls
		// setDefaultPlayerModel sits inside `if ( isHumanoid )`. The four body limbs are
		// therefore never assigned and the mod's models are silently ignored -- only the head
		// works, because it is set earlier and outside that branch. Both bodies are otherwise
		// legal in the schema, so say this out loud rather than let it look like a bad path.
		{
			bool declaredBodyLimb = false;
			for ( const LimbSlot& slot : kLimbSlots )
			{
				if ( def.limbModels.find(slot.key) != def.limbModels.end() ) { declaredBodyLimb = true; break; }
			}
			if ( declaredBodyLimb && (def.hostMonster == RAT || def.hostMonster == SPIDER) )
			{
				SAM_WARN(MOD, "Race [" + def.id + "] declares body limb_models, but its host_body "
					"never reaches the limb path: a rat/spider player is animated as a creature, not "
					"a humanoid, so torso/arm/leg models are ignored. Only 'head' applies on this "
					"host body. Use a humanoid host_body if you need the limbs.");
			}
		}

		for ( const LimbSlot& slot : kLimbSlots )
		{
			auto it = def.limbModels.find(slot.key);
			if ( it == def.limbModels.end() ) { continue; }
			const int idx = resolveModelRef(it->second, def.id, slot.key);
			if ( idx >= 0 ) { def.limbModelIdx[slot.limbType] = idx; }

			auto bent = def.limbModelsBent.find(slot.key);
			if ( bent != def.limbModelsBent.end() )
			{
				const int bentIdx = resolveModelRef(bent->second, def.id, std::string(slot.key) + " (bent)");
				if ( bentIdx >= 0 ) { def.limbBentIdx[slot.limbType] = bentIdx; }
			}
		}

		for ( size_t ei = 0; ei < def.extraLimbs.size(); ++ei )
		{
			SAMRaceDef::SAMExtraLimb& lim = def.extraLimbs[ei];
			lim.modelIdx = -1;
			const int idx = resolveModelRef(lim.model, def.id, "extra_limbs[" + std::to_string(ei) + "]");
			if ( idx >= 0 ) { lim.modelIdx = idx; }
		}

		// First-person models resolve exactly like limbs, and for the same reason they cannot
		// resolve at parse time: the model table does not exist yet during mod load.
		if ( !def.fpArm.empty() )
		{
			const int idx = resolveModelRef(def.fpArm, def.id, "first_person.arm");
			if ( idx >= 0 ) { def.fpArmIdx = idx; }
		}
		if ( !def.fpHandLeft.empty() )
		{
			const int idx = resolveModelRef(def.fpHandLeft, def.id, "first_person.hand_left");
			if ( idx >= 0 ) { def.fpHandLeftIdx = idx; }
		}

		auto head = def.limbModels.find("head");
		if ( head != def.limbModels.end() )
		{
			const int idx = resolveModelRef(head->second, def.id, "head");
			if ( idx >= 0 )
			{
				def.headModelIdx = idx;
				// Whatever the head resolved to -- our .vox or a vanilla monster limb --
				// the engine has to agree it is a player head, or multiplayer breaks.
				s_headHost[idx] = def.hostMonster;
			}
		}

		if ( !def.limbModelIdx.empty() || def.headModelIdx >= 0 )
		{
			SAM_INFO(MOD, "  " + def.name + " body: "
				+ std::to_string((int)def.limbModelIdx.size() + (def.headModelIdx >= 0 ? 1 : 0))
				+ " custom limb model(s) on the " + def.hostBodyName + " frame");
		}
	}
}

int SAMRaces::limbModelFor(int raceId, int limbType)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return -1; }
	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() ) { return -1; }
	auto lit = it->second.limbModelIdx.find(limbType);
	return ( lit == it->second.limbModelIdx.end() ) ? -1 : lit->second;
}

const std::vector<SAMRaceDef::SAMExtraLimb>* SAMRaces::extraLimbsFor(int raceId)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return nullptr; }
	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() || it->second.extraLimbs.empty() ) { return nullptr; }
	return &it->second.extraLimbs;
}

int SAMRaces::fpArmModelFor(int raceId)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return -1; }
	auto it = s_byId.find(raceId);
	return ( it == s_byId.end() ) ? -1 : it->second.fpArmIdx;
}

int SAMRaces::fpHandLeftModelFor(int raceId)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return -1; }
	auto it = s_byId.find(raceId);
	return ( it == s_byId.end() ) ? -1 : it->second.fpHandLeftIdx;
}

const SAMRaceDef::LimbXform* SAMRaces::limbXformFor(int raceId, int limbType)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return nullptr; }
	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() || it->second.limbXform.empty() ) { return nullptr; }
	auto xit = it->second.limbXform.find(limbType);
	if ( xit == it->second.limbXform.end() || !xit->second.any ) { return nullptr; }
	return &xit->second;
}

namespace
{
	// The box around a model's FILLED voxels, in the engine's model units (one voxel = 1, centred the
	// way files.cpp builds the mesh: x and y run from v - size/2 to v - size/2 + 1, z from
	// v - size/2 - 1 to v - size/2). A .vox canvas is often bigger than what is drawn in it, so the
	// canvas edge is not where the tail ends. Cached per model; a remount that swaps the model at an
	// index swaps the voxel_t too, so the pointer is part of the key.
	struct SAMFilledBox { const voxel_t* model = nullptr; double lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 }; };

	const SAMFilledBox& samFilledBox(int modelIdx)
	{
		static std::map<int, SAMFilledBox> cache;
		static const SAMFilledBox kEmpty;
		if ( modelIdx < 0 || (Uint32)modelIdx >= nummodels || !models || !models[modelIdx] ) { return kEmpty; }
		const voxel_t* m = models[modelIdx];
		SAMFilledBox& box = cache[modelIdx];
		if ( box.model == m ) { return box; }
		box = SAMFilledBox();
		box.model = m;
		if ( !m->data || m->sizex <= 0 || m->sizey <= 0 || m->sizez <= 0 ) { return box; }
		int lo[3] = { m->sizex, m->sizey, m->sizez }, hi[3] = { -1, -1, -1 };
		for ( int x = 0; x < m->sizex; ++x )
			for ( int y = 0; y < m->sizey; ++y )
				for ( int z = 0; z < m->sizez; ++z )
				{
					if ( m->data[z + y * m->sizez + x * m->sizey * m->sizez] == 255 ) { continue; }   // 255 = empty
					const int v[3] = { x, y, z };
					for ( int a = 0; a < 3; ++a ) { lo[a] = std::min(lo[a], v[a]); hi[a] = std::max(hi[a], v[a]); }
				}
		if ( hi[0] < 0 ) { return box; }   // nothing drawn at all
		box.lo[0] = lo[0] - m->sizex / 2.0;     box.hi[0] = hi[0] + 1 - m->sizex / 2.0;
		box.lo[1] = lo[1] - m->sizey / 2.0;     box.hi[1] = hi[1] + 1 - m->sizey / 2.0;
		box.lo[2] = lo[2] - m->sizez / 2.0 - 1; box.hi[2] = hi[2] - m->sizez / 2.0;
		return box;
	}

	// 3x3 rotations in the renderer's GL frame (x = east, y = up, z = south), right-handed, the
	// way rotate_mat in opengl.cpp builds them. glDrawVoxel turns a model by
	// Ry(-yaw) * Rz(-pitch) * Rx(roll), then moves it by focal, then scales it.
	struct M3 { double m[3][3]; };
	M3 mul(const M3& a, const M3& b)
	{
		M3 r{};
		for ( int i = 0; i < 3; ++i )
			for ( int j = 0; j < 3; ++j )
				r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
		return r;
	}
	M3 rotY(double t) { const double c = cos(t), s = sin(t); return M3{ { { c, 0, s }, { 0, 1, 0 }, { -s, 0, c } } }; }
	M3 rotZ(double t) { const double c = cos(t), s = sin(t); return M3{ { { c, -s, 0 }, { s, c, 0 }, { 0, 0, 1 } } }; }
	M3 rotX(double t) { const double c = cos(t), s = sin(t); return M3{ { { 1, 0, 0 }, { 0, c, -s }, { 0, s, c } } }; }
	M3 rotAxis(const double ax[3], double t)
	{
		const double c = cos(t), s = sin(t), k = 1.0 - c;
		const double x = ax[0], y = ax[1], z = ax[2];
		return M3{ { { c + k * x * x, k * x * y - s * z, k * x * z + s * y },
		             { k * x * y + s * z, c + k * y * y, k * y * z - s * x },
		             { k * x * z - s * y, k * y * z + s * x, c + k * z * z } } };
	}
	// The angle equal to `a` (mod 2 pi) nearest `near`, so a limb's yaw or roll does not jump a
	// full turn between frames.
	double nearAngle(double a, double near)
	{
		const double twoPi = 2.0 * PI;
		return a + twoPi * std::round((near - a) / twoPi);
	}
}

void SAMRaces::extraLimbFocal(const SAMRaceDef::SAMExtraLimb& lim, double& fx, double& fy, double& fz,
	double& mx, double& my, double& mz)
{
	fx = lim.focalX; fy = lim.focalY; fz = lim.focalZ;
	mx = my = mz = 0.0;
	const bool noFocal = ( fx == 0.0 && fy == 0.0 && fz == 0.0 );
	// No root and no focal: the limb turns about its own middle, as it always has, and the
	// middle stays reported as on the turning point so "side" turns it exactly as 3.9.1 did.
	if ( lim.root == SAMRaceDef::SAMExtraLimb::RootNone && noFocal ) { return; }
	const SAMFilledBox& box = samFilledBox(lim.modelIdx);
	// Where model point q is drawn, from the turning point, in the limb's frame: focal + q * scale / 2
	// (a model unit is half a world unit, and the scale is applied after the focal).
	const double half = lim.scale / 2.0;
	double c[3] = { 0.0, 0.0, 0.0 };   // the middle of the filled voxels, model units
	if ( box.model ) { for ( int a = 0; a < 3; ++a ) { c[a] = (box.lo[a] + box.hi[a]) / 2.0; } }
	if ( lim.root != SAMRaceDef::SAMExtraLimb::RootNone && box.model )
	{
		// The point of the model that must sit on the turning point: the middle of the named face.
		// Model axes: x forward, y right, z down (files.cpp builds the mesh as (x, -z, y) in GL).
		double p[3] = { c[0], c[1], c[2] };
		using L = SAMRaceDef::SAMExtraLimb;
		switch ( lim.root )
		{
			case L::RootFront:  p[0] = box.hi[0]; break;
			case L::RootBack:   p[0] = box.lo[0]; break;
			case L::RootRight:  p[1] = box.hi[1]; break;
			case L::RootLeft:   p[1] = box.lo[1]; break;
			case L::RootTop:    p[2] = box.lo[2]; break;
			case L::RootBottom: p[2] = box.hi[2]; break;
			default: break;
		}
		fx -= p[0] * half; fy -= p[1] * half; fz -= p[2] * half;
	}
	// The line a "side" swing works from: turning point to the middle of what is drawn. The
	// model's own origin can sit off that line (files.cpp shifts z by a voxel), and a swing worked
	// out from it would dip as it went.
	mx = fx + c[0] * half; my = fy + c[1] * half; mz = fz + c[2] * half;
}

void SAMRaces::swingSide(double bodyYaw, double mx, double my, double mz, double angle,
	double& yaw, double& pitch, double& roll)
{
	// The middle in GL, the way glDrawVoxel moves a model by focal: (x, -z, y).
	const double f[3] = { mx, -mz, my };
	const double fLen = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	if ( fLen < 1e-3 )
	{
		// The model's middle is on the turning point: turning about the upright line is all a
		// swing can do, and it is exactly what 3.9.1 did.
		yaw += angle;
		return;
	}
	const M3 turned = mul(mul(rotY(-yaw), rotZ(-pitch)), rotX(roll));
	double d[3];   // from the turning point to the model's middle, in the world
	for ( int i = 0; i < 3; ++i ) { d[i] = turned.m[i][0] * f[0] + turned.m[i][1] * f[1] + turned.m[i][2] * f[2]; }
	const double side[3] = { -sin(bodyYaw), 0.0, cos(bodyYaw) };   // the player's right, in GL
	double ax[3] = { d[1] * side[2] - d[2] * side[1], d[2] * side[0] - d[0] * side[2], d[0] * side[1] - d[1] * side[0] };
	double axLen = sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
	if ( axLen < 1e-3 * fLen )
	{
		// It points straight out to the side, where "left and right" is along its own length.
		// Turn it about the upright line, which at least moves its far end.
		ax[0] = 0.0; ax[1] = 1.0; ax[2] = 0.0; axLen = 1.0;
	}
	for ( int i = 0; i < 3; ++i ) { ax[i] /= axLen; }
	const M3 r = mul(rotAxis(ax, angle), turned);

	// Back into the renderer's angles: r = Ry(A) * Rz(B) * Rx(C), and A = -yaw, B = -pitch, C = roll.
	const double sB = std::max(-1.0, std::min(1.0, r.m[1][0]));
	const double B = asin(sB);
	double A, C;
	if ( cos(B) > 1e-6 )
	{
		A = atan2(-r.m[2][0], r.m[0][0]);
		C = atan2(-r.m[1][2], r.m[1][1]);
	}
	else
	{
		A = atan2(r.m[0][2], r.m[2][2]);
		C = 0.0;
	}
	yaw = nearAngle(-A, yaw);
	pitch = -B;
	roll = nearAngle(C, roll);
}

int SAMRaces::limbBentModelFor(int raceId, int limbType)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return -1; }
	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() ) { return -1; }
	auto bit = it->second.limbBentIdx.find(limbType);
	return ( bit == it->second.limbBentIdx.end() ) ? -1 : bit->second;
}

bool SAMRaces::usesLimbOverride(int raceId, int limbType)
{
	return limbModelFor(raceId, limbType) >= 0;
}

int SAMRaces::headModelFor(int raceId)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return -1; }
	auto it = s_byId.find(raceId);
	return ( it == s_byId.end() ) ? -1 : it->second.headModelIdx;
}

bool SAMRaces::clientEnemyView(int player, int monsterType, bool vanilla)
{
	if ( s_byId.empty() ) { return vanilla; }
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return vanilla; }
	if ( stats[player]->stat_appearance != 0 ) { return vanilla; }
	// A shopkeeper's opinion of you is the wanted level's to give, and it already accounts
	// for a race that cannot shop. Answering here as well would let the two disagree.
	if ( monsterType == SHOPKEEPER ) { return vanilla; }

	const int rel = declaredAllegiance(stats[player]->playerRace, monsterType);
	return ( rel < 0 ) ? vanilla : (rel == 0);
}

bool SAMRaces::isRaceHeadSprite(int sprite)
{
	return hostMonsterForHeadSprite(sprite) != 0;
}

int SAMRaces::hostMonsterForHeadSprite(int sprite)
{
	if ( !s_headHost.empty() )
	{
		auto it = s_headHost.find(sprite);
		if ( it != s_headHost.end() ) { return it->second; }
	}
	if ( !s_classHeadHost.empty() )
	{
		auto it = s_classHeadHost.find(sprite);
		if ( it != s_classHeadHost.end() ) { return it->second; }
	}
	return 0;
}

void SAMRaces::noteClassHeadSprite(int sprite, int hostMonster)
{
	if ( sprite > 0 && hostMonster > 0 ) { s_classHeadHost[sprite] = hostMonster; }
}

void SAMRaces::clearClassHeadNotes() { s_classHeadHost.clear(); }

bool SAMRaces::isRaceHeadOnMonster(const Entity* e)
{
	if ( !e || (s_headHost.empty() && s_classHeadHost.empty()) ) { return false; }
	return e->behavior == &actMonster && hostMonsterForHeadSprite(e->sprite) != 0;
}

std::vector<std::string> SAMRaces::limbModelPaths()
{
	// A limb reference is a path when it has a directory part and a .vox suffix; a bare
	// "ns:name" id and a raw index have neither. A path that is already a vanilla model
	// (models/creatures/...) must NOT be appended -- that would register a second copy of
	// a stock file -- so those resolve through vanillaModelIndexForPath instead.
	std::vector<std::string> out;
	for ( const auto& kv : s_byId )
	{
		// The first-person models are ordinary mod-relative paths and need registering the
		// same as any limb, or they resolve to nothing and silently fall back to the host body.
		for ( const auto& lim : kv.second.extraLimbs )
		{
			const std::string& ref = lim.model;
			if ( ref.size() < 5 || ref.find('/') == std::string::npos ) { continue; }
			std::string t = ref.substr(ref.size() - 4);
			for ( char& c : t ) { c = (char)std::tolower((unsigned char)c); }
			if ( t != ".vox" ) { continue; }
			if ( SAMModels::vanillaModelIndexForPath(ref) >= 0 ) { continue; }
			if ( std::find(out.begin(), out.end(), ref) == out.end() ) { out.push_back(ref); }
		}
		for ( const std::string& fpRef : { kv.second.fpArm, kv.second.fpHandLeft } )
		{
			if ( fpRef.size() < 5 || fpRef.find('/') == std::string::npos ) { continue; }
			std::string t = fpRef.substr(fpRef.size() - 4);
			for ( char& c : t ) { c = (char)std::tolower((unsigned char)c); }
			if ( t != ".vox" ) { continue; }
			if ( SAMModels::vanillaModelIndexForPath(fpRef) >= 0 ) { continue; }
			if ( std::find(out.begin(), out.end(), fpRef) == out.end() ) { out.push_back(fpRef); }
		}
		// Both maps: a bent arm is a model this mod ships like any other, and if it is not
		// collected here its .vox is never appended to the model table and the arm resolves
		// to nothing.
		std::vector<const std::map<std::string, std::string>*> samLimbMaps = { &kv.second.limbModels, &kv.second.limbModelsBent };
		for ( const auto* samMap : samLimbMaps )
		for ( const auto& lm : *samMap )
		{
			const std::string& ref = lm.second;
			if ( ref.size() < 5 || ref.find('/') == std::string::npos ) { continue; }
			std::string tail = ref.substr(ref.size() - 4);
			for ( char& c : tail ) { c = (char)std::tolower((unsigned char)c); }
			if ( tail != ".vox" ) { continue; }
			if ( SAMModels::vanillaModelIndexForPath(ref) >= 0 ) { continue; }
			if ( std::find(out.begin(), out.end(), ref) == out.end() ) { out.push_back(ref); }
		}
	}
	return out;
}

int SAMRaces::declaredAllegiance(int raceId, int monsterType)
{
	// Ordered cheapest-first. For a vanilla race raceId is 0, so this returns on the
	// first comparison without touching the map -- which is every call in a game with
	// no race mod loaded, and every call about a vanilla-race player in one that has.
	if ( raceId < SAM_RACE_ID_BASE ) { return -1; }
	if ( s_byId.empty() ) { return -1; }
	if ( monsterType <= 0 || monsterType >= NUMMONSTERS ) { return -1; }

	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() ) { return -1; }
	const SAMRaceDef& def = it->second;

	// The least friendly list first, matching the load-time warnings: enemies, neutral, allies.
	for ( int m : def.enemies ) { if ( m == monsterType ) { return 0; } }
	for ( int m : def.neutral ) { if ( m == monsterType ) { return 2; } }
	for ( int m : def.allies )  { if ( m == monsterType ) { return 1; } }
	// A recruitable type is an ally: pressing Use on a creature that is trying to kill you would
	// only start a fight. The load-time check already took a type that is also an enemy or neutral
	// off this list, so these two never contradict the lines above.
	for ( int m : def.recruits ) { if ( m == monsterType ) { return 1; } }
	return -1;
}

bool SAMRaces::neverAFollower(Entity* monster, Stat* monsterStats)
{
	if ( !monster || !monsterStats ) { return true; }
	// The four bots are tested by Stat::type as well as by monsterIsTinkeringCreation(), which
	// reads the entity's SPRITE: a monster spawned this very tick has not had one assigned yet
	// (its first act does that), so the sprite test answers false for a sentrybot made a moment ago.
	const bool bot = ( monsterStats->type == SENTRYBOT || monsterStats->type == GYROBOT
		|| monsterStats->type == SPELLBOT || monsterStats->type == DUMMYBOT );
	const bool innerDemon = ( monsterStats->type == INCUBUS
		&& !strncmp(monsterStats->name, "inner demon", strlen("inner demon")) );
	// isBossMonster already covers shopkeepers, other traders, the minotaur, shadows, liches, the
	// devil and a mod's own monster declared a boss.
	return monster->isBossMonster() || monster->isInertMimic() || innerDemon || bot
		|| monsterStats->type == MIMIC || monsterStats->type == MINIMIMIC
		|| monsterStats->type == BAT_SMALL || monsterStats->type == HOLOGRAM
		|| monster->monsterIsTinkeringCreation() || monster->monsterAllySummonRank != 0;
}

bool SAMRaces::recruitsByUse(const Stat* playerStats, Entity* monster, Stat* monsterStats)
{
	// Cheapest first: playerRace is 0 for every vanilla race, so a game with no race mod returns on
	// the first comparison, every time makeFollower is asked.
	if ( !playerStats || playerStats->playerRace < SAM_RACE_ID_BASE || playerStats->stat_appearance != 0
		|| s_byId.empty() ) { return false; }
	if ( !monster || !monsterStats ) { return false; }
	const SAMRaceDef* def = get(playerStats->playerRace);
	if ( !def || def->recruits.empty() ) { return false; }
	const int type = (int)monster->getRace();
	if ( std::find(def->recruits.begin(), def->recruits.end(), type) == def->recruits.end() ) { return false; }
	return !neverAFollower(monster, monsterStats);
}

bool SAMRaces::canOpenTins(int raceId, int statAppearance)
{
	if ( raceId < SAM_RACE_ID_BASE || statAppearance != 0 || s_byId.empty() ) { return false; }
	const SAMRaceDef* def = get(raceId);
	return def && def->canOpenTins;
}

void SAMRaces::clear()
{
	// The head set outlives the defs otherwise, and a stale entry makes
	// isPlayerHeadSprite answer true for a model no race uses any more.
	s_headHost.clear();
	s_classHeadHost.clear();
	s_byId.clear();
	s_byIdString.clear();
	s_nextId = SAM_RACE_ID_BASE;
	s_byKey.clear();
}

bool SAMRaces::any() { return !s_byId.empty(); }
int  SAMRaces::count() { return static_cast<int>(s_byId.size()); }

const SAMRaceDef* SAMRaces::get(int raceId)
{
	auto it = s_byId.find(raceId);
	return (it != s_byId.end()) ? &it->second : nullptr;
}

int SAMRaces::visionBonus(int raceId, int statAppearance)
{
	if ( statAppearance != 0 ) { return 0; }
	const SAMRaceDef* def = get(raceId);
	return def ? def->vision : 0;
}

int SAMRaces::sneakVisionBonus(int raceId, int statAppearance)
{
	if ( statAppearance != 0 ) { return 0; }
	const SAMRaceDef* def = get(raceId);
	return def ? def->sneakVision : 0;
}

bool SAMRaces::requiresBloodDiet(int raceId)
{
	const SAMRaceDef* def = get(raceId);
	return def && def->bloodDiet;
}

void SAMRaces::applySpells(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !players[player] || !stats[player] ) { return; }
	const SAMRaceDef* def = get(stats[player]->playerRace);
	if ( !def ) { return; }
	if ( stats[player]->stat_appearance != 0 ) { return; }   // abilities disabled: no innate spells
	const bool isLocalPlayer = players[player]->isLocalPlayer();
	if ( !isLocalPlayer && multiplayer == CLIENT && intro == false ) { return; }

	int learned = 0;
	for ( const std::string& sp : def->startingSpells )
	{
		// Vanilla spell name first, resolved through the same map SAMClasses uses.
		std::string lower = sp;
		for ( char& c : lower ) { c = (char)std::tolower((unsigned char)c); }
		int id = -1;
		for ( const auto& kv : ItemTooltips.spellItems )
		{
			if ( kv.second.internalName == lower ) { id = kv.first; break; }
		}
		if ( id < 0 )
		{
			// A custom "namespace:spell": grant via the engine spell_t SAMSpells built.
			if ( SAMSpells::getSpellByName(sp) )
			{
				if ( SAMSpells::grantCustomSpell(player, sp) ) { ++learned; }
			}
			else
			{
				SAM_ERROR(MOD, "race [" + def->id + "] references unknown spell '" + sp + "' — skipping.");
			}
			continue;
		}
		addSpell(id, player, true); // ignoreSkill: innate spells are never gated
		++learned;
	}
	if ( learned > 0 )
	{
		SAM_INFO(MOD, "Applied " + std::to_string(learned) + " innate spell(s) for race [" + def->id + "].");
	}
}

int SAMRaces::raceIdAtIndex(int index)
{
	if ( index < 0 || index >= (int)s_byId.size() ) { return -1; }
	auto it = s_byId.begin();
	std::advance(it, index);
	return it->first;
}

uint32_t SAMRaces::raceKeyFor(int raceId)
{
	if ( raceId < SAM_RACE_ID_BASE || s_byId.empty() ) { return 0; }
	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() ) { return 0; }
	return samRaceKeyFromIdString(it->second.id);
}

int SAMRaces::raceIdForKey(uint32_t key)
{
	if ( key == 0 || s_byKey.empty() ) { return -1; }
	auto it = s_byKey.find(key);
	return ( it != s_byKey.end() ) ? it->second : -1;
}

std::string SAMRaces::saveIdTable()
{
	std::string out;
	for ( const auto& kv : s_byId )
	{
		if ( kv.second.id.empty() ) { continue; }
		if ( !out.empty() ) { out += ';'; }
		out += std::to_string(kv.first) + "=" + kv.second.id;
	}
	return out;
}

bool SAMRaces::remapSavedRaceIds(const std::string& savedTable,
	std::map<int, int>& oldToNew, std::map<int, std::string>& unresolved)
{
	oldToNew.clear();
	unresolved.clear();
	if ( savedTable.empty() ) { return false; }
	// Same guard, for the same reason, as SAMItems::remapSavedItemIds: the registry only exists while a
	// mod set is MOUNTED, and getSaveGameInfo also runs for the main menu's save-slot list after S.A.M
	// has unloaded. Judging then would call every race "removed".
	if ( SAMWorkshop::manifests().empty() ) { return false; }
	size_t start = 0;
	while ( start < savedTable.size() )
	{
		size_t end = savedTable.find(';', start);
		if ( end == std::string::npos ) { end = savedTable.size(); }
		const std::string entry = savedTable.substr(start, end - start);
		const size_t eq = entry.find('=');
		if ( eq != std::string::npos && eq > 0 )
		{
			const int savedId = std::atoi(entry.substr(0, eq).c_str());
			const std::string name = entry.substr(eq + 1);
			if ( savedId >= SAM_RACE_ID_BASE && !name.empty() )
			{
				const int now = raceIdForIdString(name);
				if ( now >= 0 ) { oldToNew[savedId] = now; }
				else { unresolved[savedId] = name; }
			}
		}
		start = end + 1;
	}
	return true;
}

std::string SAMRaces::canonicalName(int raceId)
{
	if ( raceId >= SAM_RACE_ID_BASE )
	{
		const SAMRaceDef* def = get(raceId);
		return def ? def->id : std::string();
	}
	const int mon = (int)getMonsterFromPlayerRace(raceId);
	if ( mon >= 0 && mon < NUMMONSTERS ) { return std::string(monstertypename[mon]); }
	return std::string();
}

int SAMRaces::raceIdForIdString(const std::string& idString)
{
	auto it = s_byIdString.find(idString);
	return (it != s_byIdString.end()) ? it->second : -1;
}

int SAMRaces::hostMonsterForRace(int raceId)
{
	auto it = s_byId.find(raceId);
	return (it != s_byId.end()) ? it->second.hostMonster : (int)HUMAN;
}

std::string SAMRaces::displayName(int raceId)
{
	auto it = s_byId.find(raceId);
	return (it != s_byId.end()) ? it->second.name : std::string();
}

std::string SAMRaces::description(int raceId)
{
	auto it = s_byId.find(raceId);
	return (it != s_byId.end()) ? it->second.description : std::string();
}

std::string SAMRaces::iconPath(int raceId)
{
	// Resolved (and any problem reported) once when the race loaded, so this is a plain read: the HUD
	// asks for it when a bar's name is refreshed.
	auto it = s_byId.find(raceId);
	return (it != s_byId.end()) ? it->second.iconResolved : std::string();
}

void SAMRaces::applyStats(int raceId, Stat* myStats)
{
	if ( !myStats || raceId < SAM_RACE_ID_BASE ) { return; }
	auto it = s_byId.find(raceId);
	if ( it == s_byId.end() ) { return; }
	const SAMRaceDef& d = it->second;
	// "Disable monster abilities" (stat_appearance != 0) keeps the look and drops the
	// abilities, exactly as it does for a vanilla goatman or vampire. The deltas are an
	// ability; the body is not.
	if ( myStats->stat_appearance != 0 ) { return; }
	myStats->STR += d.str;
	myStats->DEX += d.dex;
	myStats->CON += d.con;
	myStats->INT += d.intel;
	myStats->PER += d.per;
	myStats->CHR += d.chr;
	myStats->HP += d.hp;   myStats->MAXHP += d.hp;
	myStats->MP += d.mp;   myStats->MAXMP += d.mp;
	// The caller's unconditional std::max(1, ...) clamp runs right after this.
}
