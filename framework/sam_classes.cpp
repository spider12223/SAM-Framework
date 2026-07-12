/*-------------------------------------------------------------------------------

	S.A.M Framework (Support All Mods)
	File: sam_classes.cpp
	Desc: implementation of the custom class registry + application.

	Two halves:
	  * Registry/parsing (loadFromManifest/clear/getClass/count) — decoupled
	    from Barony; compiled into both the game and the editor.
	  * Application (applyStats/applyLoadout/applySpells) — touches Barony
	    internals; compiled only into the game (#ifndef EDITOR).

-------------------------------------------------------------------------------*/

// Barony headers pull in <windows.h>; stop it defining min()/max() macros that
// would break nlohmann/std. Must come before any include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "sam_classes.hpp"
#include "sam_workshop.hpp"
#include "sam_logger.hpp"
#include "nlohmann/json.hpp"

#include <fstream>
#include <sstream>

#ifndef EDITOR
#include "main.hpp"          // MAXPLAYERS, CLIENT, multiplayer
#include "game.hpp"          // client_classes, intro
#include "stat.hpp"          // Stat, PRO_* constants
#include "items.hpp"         // ItemType, Status, newItem, itemPickup, useItem
#include "player.hpp"        // players[], hotbar, NUM_HOTBAR_SLOTS
#include "net.hpp"
#include "mod_tools.hpp"     // ItemTooltips (itemNameStringToItemID, spellItems)
#include "magic/magic.hpp"   // addSpell
#include <cctype>
#include <cstdlib>           // free
#endif

using nlohmann::json;

static const char* MOD = "CLASSES";

/*-------------------------------------------------------------------------------
	Registry storage (game-decoupled)
-------------------------------------------------------------------------------*/
static std::map<int, SAMClassDef> s_registry;
static int s_nextClassId = SAM_CLASS_ID_BASE;

static bool readWholeFile(const std::string& path, std::string& out)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if ( !f.is_open() )
	{
		return false;
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

static std::string joinPath(const std::string& dir, const std::string& file)
{
	if ( dir.empty() )
	{
		return file;
	}
	const char back = dir.back();
	return (back == '/' || back == '\\') ? (dir + file) : (dir + "/" + file);
}

// Normalize backslashes to forward slashes. SDL_image's IMG_Load accepts '/' on
// every platform; keeping one separator style also makes log lines readable and
// avoids any confusion when the path is later handed to the UI Image loader.
static std::string toForwardSlashes(std::string s)
{
	for ( char& c : s )
	{
		if ( c == '\\' )
		{
			c = '/';
		}
	}
	return s;
}

static bool fileExists(const std::string& path)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	return f.is_open();
}

/*-------------------------------------------------------------------------------
	SAMClasses::loadFromManifest
-------------------------------------------------------------------------------*/
void SAMClasses::loadFromManifest(const SAMModManifest& manifest)
{
	for ( const std::string& relPath : manifest.classes )
	{
		const std::string path = joinPath(manifest.modPath, relPath);
		std::string text;
		if ( !readWholeFile(path, text) )
		{
			SAM_ERROR(MOD, "Class file not found: " + path + " (declared by [" + manifest.ns + "])");
			continue;
		}

		json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
		if ( j.is_discarded() || !j.is_object() )
		{
			SAM_ERROR(MOD, "Invalid class JSON (not a JSON object): " + path);
			continue;
		}

		// Small typed accessors that never throw.
		auto getStr = [&](const json& o, const char* k) -> std::string {
			auto it = o.find(k);
			return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
		};
		auto getInt = [&](const json& o, const char* k, int dv) -> int {
			auto it = o.find(k);
			return (it != o.end() && it->is_number()) ? it->get<int>() : dv;
		};
		auto getBool = [&](const json& o, const char* k, bool dv) -> bool {
			auto it = o.find(k);
			return (it != o.end() && it->is_boolean()) ? it->get<bool>() : dv;
		};

		SAMClassDef def;
		def.id = getStr(j, "id");
		def.name = getStr(j, "name");
		def.description = getStr(j, "description");
		def.modNamespace = manifest.ns;

		if ( def.id.empty() )
		{
			SAM_ERROR(MOD, "Class missing required 'id' in: " + path);
			continue;
		}
		if ( def.name.empty() )
		{
			SAM_ERROR(MOD, "Class [" + def.id + "] missing required 'name' in: " + path);
			continue;
		}

		// Optional custom portrait: resolve to an absolute file path and confirm it
		// exists NOW (load time). If it's missing we log a warning and leave
		// portraitPath empty so the carousel uses the placeholder — a bad portrait
		// must never block the class or crash. The actual image decode happens
		// lazily in the UI (Image::get); a corrupt-but-present file simply draws
		// no icon there, which is likewise non-fatal.
		def.portrait = getStr(j, "portrait");
		if ( !def.portrait.empty() )
		{
			const std::string abs = toForwardSlashes(joinPath(manifest.modPath, def.portrait));
			if ( fileExists(abs) )
			{
				def.portraitPath = abs;
				SAM_INFO(MOD, "Class [" + def.id + "] portrait -> " + abs);
			}
			else
			{
				SAM_WARN(MOD, "Class [" + def.id + "] portrait not found (using placeholder): " + abs);
			}
		}

		if ( j.contains("stats") && j["stats"].is_object() )
		{
			const json& s = j["stats"];
			def.str = getInt(s, "STR", 0);
			def.dex = getInt(s, "DEX", 0);
			def.con = getInt(s, "CON", 0);
			def.intel = getInt(s, "INT", 0);
			def.per = getInt(s, "PER", 0);
			def.chr = getInt(s, "CHR", 0);
			def.hp = getInt(s, "HP", 0);
			def.mp = getInt(s, "MP", 0);
		}

		if ( j.contains("skills") && j["skills"].is_object() )
		{
			for ( auto it = j["skills"].begin(); it != j["skills"].end(); ++it )
			{
				if ( it.value().is_number() )
				{
					def.skills[it.key()] = it.value().get<int>();
				}
			}
		}

		if ( j.contains("starting_items") && j["starting_items"].is_array() )
		{
			for ( const json& e : j["starting_items"] )
			{
				if ( !e.is_object() )
				{
					continue;
				}
				SAMStartingItem si;
				si.type = getStr(e, "type");
				if ( si.type.empty() )
				{
					continue;
				}
				si.status = getStr(e, "status");
				if ( si.status.empty() ) { si.status = "SERVICABLE"; }
				si.beatitude = getInt(e, "beatitude", 0);
				si.count = getInt(e, "count", 1);
				si.appearance = getInt(e, "appearance", 0);
				si.identified = getBool(e, "identified", true);
				si.equip = getBool(e, "equip", false);
				si.hotbarSlot = getInt(e, "hotbar_slot", -1);
				def.startingItems.push_back(si);
			}
		}

		if ( j.contains("starting_spells") && j["starting_spells"].is_array() )
		{
			for ( const json& e : j["starting_spells"] )
			{
				if ( e.is_string() ) { def.startingSpells.push_back(e.get<std::string>()); }
			}
		}

		if ( j.contains("stat_growth") && j["stat_growth"].is_object() )
		{
			const json& g = j["stat_growth"];
			if ( g.contains("strong_rolls") && g["strong_rolls"].is_array() )
			{
				for ( const json& e : g["strong_rolls"] )
				{
					if ( e.is_string() ) { def.strongRolls.push_back(e.get<std::string>()); }
				}
			}
			if ( g.contains("weak_rolls") && g["weak_rolls"].is_array() )
			{
				for ( const json& e : g["weak_rolls"] )
				{
					if ( e.is_string() ) { def.weakRolls.push_back(e.get<std::string>()); }
				}
			}
		}

		def.gold = getInt(j, "gold", 0);

		def.numericId = s_nextClassId++;
		s_registry[def.numericId] = def;

		SAM_INFO(MOD, "Registering class: " + def.name + " [" + def.id + "] -> runtime id "
			+ std::to_string(def.numericId));
		SAM_DEBUG(MOD, "  stats: STR " + std::to_string(def.str) + " DEX " + std::to_string(def.dex)
			+ " CON " + std::to_string(def.con) + " INT " + std::to_string(def.intel)
			+ " PER " + std::to_string(def.per) + " CHR " + std::to_string(def.chr)
			+ " HP " + std::to_string(def.hp) + " MP " + std::to_string(def.mp)
			+ " GOLD " + std::to_string(def.gold));
		SAM_DEBUG(MOD, "  " + std::to_string(def.skills.size()) + " skill(s), "
			+ std::to_string(def.startingItems.size()) + " starting item(s), "
			+ std::to_string(def.startingSpells.size()) + " starting spell(s)");
	}
}

void SAMClasses::clear()
{
	s_registry.clear();
	s_nextClassId = SAM_CLASS_ID_BASE;
}

const SAMClassDef* SAMClasses::getClass(int classId)
{
	auto it = s_registry.find(classId);
	return (it != s_registry.end()) ? &it->second : nullptr;
}

int SAMClasses::count()
{
	return static_cast<int>(s_registry.size());
}

int SAMClasses::classIdAtIndex(int index)
{
	if ( index < 0 )
	{
		return -1;
	}
	int i = 0;
	for ( const auto& kv : s_registry )
	{
		if ( i == index )
		{
			return kv.first;
		}
		++i;
	}
	return -1;
}

int SAMClasses::classIdForIdString(const std::string& idString)
{
	for ( const auto& kv : s_registry )
	{
		if ( kv.second.id == idString )
		{
			return kv.first;
		}
	}
	return -1;
}

/*-------------------------------------------------------------------------------
	Application into the running game (game build only)
-------------------------------------------------------------------------------*/
#ifndef EDITOR

static std::string toLower(std::string s)
{
	for ( char& c : s ) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
	return s;
}

// "PRO_SWORD" -> the proficiency index from stat.hpp, or -1 if unknown.
static int proIdFromName(const std::string& n)
{
	static const std::map<std::string, int> m = {
		{ "PRO_LOCKPICKING", PRO_LOCKPICKING }, { "PRO_STEALTH", PRO_STEALTH },
		{ "PRO_TRADING", PRO_TRADING }, { "PRO_APPRAISAL", PRO_APPRAISAL },
		{ "PRO_THAUMATURGY", PRO_THAUMATURGY }, { "PRO_LEADERSHIP", PRO_LEADERSHIP },
		{ "PRO_MYSTICISM", PRO_MYSTICISM }, { "PRO_SORCERY", PRO_SORCERY },
		{ "PRO_RANGED", PRO_RANGED }, { "PRO_SWORD", PRO_SWORD },
		{ "PRO_MACE", PRO_MACE }, { "PRO_AXE", PRO_AXE },
		{ "PRO_POLEARM", PRO_POLEARM }, { "PRO_SHIELD", PRO_SHIELD },
		{ "PRO_UNARMED", PRO_UNARMED }, { "PRO_ALCHEMY", PRO_ALCHEMY },
	};
	auto it = m.find(n);
	return (it != m.end()) ? it->second : -1;
}

static Status statusFromName(const std::string& n)
{
	if ( n == "BROKEN" ) { return BROKEN; }
	if ( n == "DECREPIT" ) { return DECREPIT; }
	if ( n == "WORN" ) { return WORN; }
	if ( n == "EXCELLENT" ) { return EXCELLENT; }
	return SERVICABLE; // Barony's spelling; also the default
}

// Map a schema item type name to a vanilla ItemType. Barony's itemNameStrings
// are lowercase ("iron_axe"); the schema uses the uppercase enum name
// ("IRON_AXE"), so we lowercase before looking up. Custom "namespace:item"
// references are not resolvable yet (the item loader is a later session).
static bool itemTypeFromName(const std::string& n, ItemType& out)
{
	if ( n.find(':') != std::string::npos )
	{
		return false; // custom item — not loaded yet
	}
	auto it = ItemTooltips.itemNameStringToItemID.find(toLower(n));
	if ( it != ItemTooltips.itemNameStringToItemID.end() )
	{
		out = static_cast<ItemType>(it->second);
		return true;
	}
	return false;
}

// "SPELL_FORCEBOLT" -> spell id, via the lowercase spellItems internalName.
static int spellIdFromName(const std::string& n)
{
	const std::string lower = toLower(n);
	for ( const auto& kv : ItemTooltips.spellItems )
	{
		if ( kv.second.internalName == lower )
		{
			return kv.first;
		}
	}
	return -1;
}

void SAMClasses::applyStats(int classnum, Stat* stat)
{
	if ( !stat ) { return; }
	const SAMClassDef* def = getClass(classnum);
	if ( !def )
	{
		SAM_WARN(MOD, "applyStats: no custom class registered for id " + std::to_string(classnum));
		return;
	}

	stat->STR += def->str;
	stat->DEX += def->dex;
	stat->CON += def->con;
	stat->INT += def->intel;
	stat->PER += def->per;
	stat->CHR += def->chr;
	stat->MAXHP += def->hp;  stat->HP += def->hp;
	stat->MAXMP += def->mp;  stat->MP += def->mp;
	stat->GOLD += def->gold;

	for ( const auto& kv : def->skills )
	{
		const int pro = proIdFromName(kv.first);
		if ( pro >= 0 )
		{
			stat->setProficiency(pro, kv.second);
		}
		else
		{
			SAM_WARN(MOD, "class [" + def->id + "] unknown skill '" + kv.first + "' — ignored.");
		}
	}

	SAM_INFO(MOD, "Applied stats for [" + def->id + "] (STR " + std::to_string(def->str)
		+ " DEX " + std::to_string(def->dex) + " CON " + std::to_string(def->con)
		+ " INT " + std::to_string(def->intel) + " PER " + std::to_string(def->per)
		+ " CHR " + std::to_string(def->chr) + " HP " + std::to_string(def->hp)
		+ " MP " + std::to_string(def->mp) + " GOLD " + std::to_string(def->gold) + "), "
		+ std::to_string(def->skills.size()) + " skill(s).");
	// NOTE: HP/MP clamping is intentionally left to initClassStats' unconditional
	// clamp block (charclass.cpp ~635-690), which runs after this returns.
}

void SAMClasses::applyLoadout(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return; }
	const SAMClassDef* def = getClass(client_classes[player]);
	if ( !def ) { return; }

	const bool isLocalPlayer = players[player]->isLocalPlayer();
	// Defensive multiplayer guard (the charclass.cpp hook also returns before
	// calling us for players we don't own).
	if ( !isLocalPlayer && multiplayer == CLIENT && intro == false ) { return; }

	auto& hotbar = players[player]->hotbar.slots();
	int given = 0;
	for ( const SAMStartingItem& si : def->startingItems )
	{
		ItemType type;
		if ( !itemTypeFromName(si.type, type) )
		{
			SAM_ERROR(MOD, "class [" + def->id + "] references item '" + si.type
				+ "' which does not exist (custom items are not loaded yet) — skipping.");
			continue;
		}
		const Status status = statusFromName(si.status);
		Item* item = newItem(type, status, static_cast<Sint16>(si.beatitude),
			static_cast<Sint16>(si.count), static_cast<Uint32>(si.appearance), si.identified, nullptr);
		if ( !item ) { continue; }

		if ( isLocalPlayer )
		{
			Item* item2 = itemPickup(player, item);
			if ( item2 )
			{
				if ( si.equip ) { useItem(item2, player); }
				if ( si.hotbarSlot >= 0 && si.hotbarSlot < static_cast<int>(NUM_HOTBAR_SLOTS) )
				{
					hotbar[si.hotbarSlot].item = item2->uid;
				}
			}
			free(item);
		}
		else
		{
			// Host applying to another player: equip if requested, else discard
			// the temp (consumables are only granted to local players, as vanilla).
			if ( si.equip ) { useItem(item, player); }
			else { free(item); }
		}
		++given;
		SAM_DEBUG(MOD, "  gave " + si.type + " x" + std::to_string(si.count)
			+ (si.equip ? " (equipped)" : "")
			+ (si.hotbarSlot >= 0 ? (" -> hotbar " + std::to_string(si.hotbarSlot)) : std::string()));
	}
	SAM_INFO(MOD, "Applied loadout for [" + def->id + "]: " + std::to_string(given) + " item stack(s).");
}

void SAMClasses::applySpells(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !players[player] ) { return; }
	const SAMClassDef* def = getClass(client_classes[player]);
	if ( !def ) { return; }

	const bool isLocalPlayer = players[player]->isLocalPlayer();
	if ( !isLocalPlayer && multiplayer == CLIENT && intro == false ) { return; }

	int learned = 0;
	for ( const std::string& sp : def->startingSpells )
	{
		const int id = spellIdFromName(sp);
		if ( id < 0 )
		{
			SAM_ERROR(MOD, "class [" + def->id + "] references unknown spell '" + sp + "' — skipping.");
			continue;
		}
		addSpell(id, player, true);
		++learned;
		SAM_DEBUG(MOD, "  learned " + sp);
	}
	if ( learned > 0 )
	{
		SAM_INFO(MOD, "Applied " + std::to_string(learned) + " starting spell(s) for [" + def->id + "].");
	}
}

#endif // !EDITOR
