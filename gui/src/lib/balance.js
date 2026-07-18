/*
 * Live balance checker — pure heuristics, ADVISORY ONLY.
 * checkBalance(kind, def) -> [{ level: 'warn'|'info', message }]
 *
 * Unlike enum lists (which are always schema-derived — see data/schemas.js),
 * the numeric thresholds here are deliberately hardcoded advisory constants:
 * "what feels overtuned vs vanilla Barony". They never gate save/export.
 * Key lists still come from the schemas where possible so new stats/categories
 * are at least iterated.
 */
import { CORE_ATTRIBUTES, MONSTER_STAT_KEYS } from '@/data/schemas.js';

/** Rough vanilla gold_value ceilings per item category (advisory). */
const CATEGORY_GOLD_NORMS = {
  WEAPON: 600, ARMOR: 800, AMULET: 1000, POTION: 250, SCROLL: 300,
  MAGICSTAFF: 800, RING: 1200, SPELLBOOK: 750, GEM: 3000, THROWN: 150,
  TOOL: 400, FOOD: 100, BOOK: 200,
};

const hint = (level, message) => ({ level, message });

function checkClass(def) {
  const out = [];
  const stats = def.stats ?? {};
  const coreVals = CORE_ATTRIBUTES.map((a) => stats[a] ?? 0);
  const total = coreVals.reduce((s, v) => s + v, 0);
  // These are MODIFIERS on the race base (schema: "stat->STR += n" in initClassStats), NOT
  // absolute attribute values. Vanilla classes net roughly 0 to +5 across the six and trade
  // strengths for weaknesses (e.g. +INT / -STR); the biggest single modifier in vanilla is +3.
  if (total > 12) out.push(hint('warn', `Attribute modifiers net +${total} — vanilla classes net about 0 to +5 (they trade strengths for weaknesses), so this is a large flat buff.`));
  const bigBuffs = CORE_ATTRIBUTES.filter((a) => (stats[a] ?? 0) >= 6);
  if (bigBuffs.length) out.push(hint('warn', `${bigBuffs.join(', ')} at +6 or more — vanilla tops out near +3 in a class's focus attribute.`));
  const hpmp = (stats.HP ?? 0) + (stats.MP ?? 0);
  if (hpmp > 30) out.push(hint('warn', `HP+MP offsets total +${hpmp} — vanilla offsets stay within ±20 combined.`));
  const skillTotal = Object.values(def.skills ?? {}).reduce((s, v) => s + v, 0);
  if (skillTotal > 300) out.push(hint('warn', `Skill points total ${skillTotal} — vanilla classes grant ~100-250.`));
  const skills100 = Object.entries(def.skills ?? {}).filter(([, v]) => v >= 100);
  if (skills100.length) out.push(hint('info', `${skills100.map(([k]) => k).join(', ')} start at 100 (legendary) — nothing left to train.`));
  if ((def.starting_items ?? []).length > 12) out.push(hint('info', `${def.starting_items.length} starting items — inventory starts cluttered; vanilla gives 3-8.`));
  if ((def.gold ?? 0) > 1000) out.push(hint('warn', `${def.gold} starting gold — vanilla's richest (Merchant) starts with 500-800.`));
  return out;
}

function checkItem(def) {
  const out = [];
  const norm = CATEGORY_GOLD_NORMS[def.category];
  if (norm && (def.gold_value ?? 0) > norm * 10) {
    out.push(hint('warn', `gold_value ${def.gold_value} is >10x a typical ${def.category} (~${norm}) — shops/economy may break.`));
  }
  if ((def.weight ?? 0) > 40) out.push(hint('info', `Weight ${def.weight} — heavier than a vanilla crystal breastpiece (30); most players can't lift it early.`));
  if ((def.level ?? 0) > 35) out.push(hint('info', `Level ${def.level} — random loot rolls rarely reach past ~35 dungeon level.`));
  const attrSum = Object.values(def.attributes ?? {}).reduce((s, v) => s + Math.abs(v), 0);
  if (attrSum > 20) out.push(hint('warn', `Attribute magnitudes total ${attrSum} — strongest vanilla gear stays under ~10.`));
  return out;
}

function checkMonster(def) {
  const out = [];
  const stats = def.stats ?? {};
  if ((stats.MAXHP ?? 0) > 500) out.push(hint('warn', `MAXHP ${stats.MAXHP} — tougher than vanilla bosses (Baron Herx ~1000, minotaur ~400). Early parties can't kill it.`));
  if ((stats.LVL ?? 0) > 35) out.push(hint('info', `LVL ${stats.LVL} — grants huge XP; killing one may jump players several levels.`));
  const cores = ['STR', 'DEX', 'CON', 'INT', 'PER', 'CHR'].filter((k) => MONSTER_STAT_KEYS.includes(k));
  const high = cores.filter((k) => (stats[k] ?? 0) >= 20);
  if (high.length >= 4) out.push(hint('warn', `${high.length} core stats at 20+ — an all-rounder juggernaut; vanilla elites spike one or two stats.`));
  if ((stats.GOLD ?? 0) > 2000) out.push(hint('warn', `${stats.GOLD} gold carried — a walking treasury; farming it breaks the economy.`));
  const prof100 = Object.entries(def.proficiencies ?? {}).filter(([, v]) => v >= 100);
  if (prof100.length >= 3) out.push(hint('info', `${prof100.length} proficiencies at 100 — every action it takes is legendary-tier.`));
  const followers = def.followers?.num_followers ?? 0;
  if (followers > 5) out.push(hint('warn', `${followers} followers per spawn — packs multiply fast and can lag or overwhelm floors.`));
  for (const s of def.spawn ?? []) {
    if ((s.default_weight ?? 1) === 0) {
      out.push(hint('warn', `Spawn on "${s.level_name}" has default_weight 0 — the vanilla ${s.base_species ?? def.base_type} can NEVER spawn there.`));
    }
    if ((s.variant_weight ?? 1) >= 10 * Math.max(1, s.default_weight ?? 1)) {
      out.push(hint('info', `Spawn on "${s.level_name}": variant outweighs vanilla ${Math.round((s.variant_weight ?? 1) / Math.max(1, s.default_weight ?? 1))}:1 — nearly every ${s.base_species ?? def.base_type} will be yours.`));
    }
  }
  return out;
}

export function checkBalance(kind, def) {
  try {
    if (kind === 'class') return checkClass(def);
    if (kind === 'item') return checkItem(def);
    if (kind === 'monster') return checkMonster(def);
  } catch {
    // A half-built def mid-keystroke must never crash the editor.
  }
  return [];
}
