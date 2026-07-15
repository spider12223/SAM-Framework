/*
 * Single source of truth: the JSON schemas in SAM-Framework/schemas/.
 * Every enum list the GUI shows (item types, skills, categories, slots,
 * statuses, roll stats, monster base types...) is DERIVED from them here —
 * never hardcoded. If the C++ side gains an item type or skill, updating the
 * schema updates the GUI automatically.
 */
import modSchema from '@schemas/mod.schema.json';
import classSchema from '@schemas/class.schema.json';
import itemSchema from '@schemas/item.schema.json';
import monsterSchema from '@schemas/monster.schema.json';
import spellSchema from '@schemas/spell.schema.json';
import patchSchema from '@schemas/patch.schema.json';

export { modSchema, classSchema, itemSchema, monsterSchema, spellSchema, patchSchema };

/** All vanilla Barony ItemType names (from class.schema.json's itemType enum). */
export const ITEM_TYPES = classSchema.definitions.itemType.enum;

/** The PRO_X skill names (keys of the class schema's skills object). */
export const SKILLS = Object.keys(classSchema.properties.skills.properties);

/** Human label for a PRO_X skill: "PRO_LOCKPICKING" -> "Lockpicking". */
export function skillLabel(pro) {
  const raw = pro.replace(/^PRO_/, '').toLowerCase().replace(/_/g, ' ');
  return raw.replace(/\b\w/g, (c) => c.toUpperCase());
}

/** Base attribute of a skill, parsed from its schema description ("Base attribute: DEX."). */
export function skillBaseAttr(pro) {
  const desc = classSchema.properties.skills.properties[pro]?.description ?? '';
  const m = desc.match(/Base attribute:\s*([A-Z]+)/);
  return m ? m[1] : '';
}

/** Core attribute keys in schema order, split from HP/MP. */
const statKeys = Object.keys(classSchema.properties.stats.properties);
export const CORE_ATTRIBUTES = statKeys.filter((k) => k !== 'HP' && k !== 'MP');
export const OFFSET_STATS = statKeys.filter((k) => k === 'HP' || k === 'MP');

/** Item categories + equip slots (from item.schema.json enums). */
export const CATEGORIES = itemSchema.properties.category.enum;
export const SLOTS = itemSchema.properties.slot.enum;

/** Item condition statuses (from the class schema's starting_items entry). */
export const STATUSES =
  classSchema.properties.starting_items.items.properties.status.enum;

/** Attributes eligible for strong/weak level-up rolls. */
export const ROLL_STATS =
  classSchema.properties.stat_growth.properties.strong_rolls.items.enum;

/** namespace pattern from mod.schema.json (kept as the schema's regex). */
export const NAMESPACE_PATTERN = new RegExp(modSchema.properties.namespace.pattern);

/** id pattern for classes/items ("namespace:thing"). */
export const ID_PATTERN = new RegExp(classSchema.properties.id.pattern);

/** Version pattern (MAJOR.MINOR.PATCH). */
export const VERSION_PATTERN = new RegExp(modSchema.properties.version.pattern);

/* ------------------------------------------------------------------ */
/* Monster enums (from monster.schema.json — note it uses $defs, not   */
/* definitions like the class/item schemas).                           */
/* ------------------------------------------------------------------ */

/** The 49 Barony base creature types a monster variant can build on. */
export const MONSTER_BASE_TYPES = monsterSchema.properties.base_type.enum;

/** Monster stat keys (HP/MAXHP/.../GOLD) in schema order. */
export const MONSTER_STAT_KEYS = Object.keys(monsterSchema.properties.stats.properties);

/** RANDOM_* variance keys in schema order. */
export const MONSTER_RANDOM_STAT_KEYS =
  Object.keys(monsterSchema.properties.random_stats.properties);

/** Engine skill display names (Tinkering..Alchemy — NOT the PRO_ constants). */
export const MONSTER_PROFICIENCIES =
  monsterSchema.properties.proficiencies.propertyNames.enum;

/** The 10 equipment slot keys (Barony spellings: helmet, breastplate, shoes...). */
export const MONSTER_EQUIP_SLOTS =
  Object.keys(monsterSchema.properties.equipped_items.properties);

/** Item condition statuses for monster gear (lowercase, incl. 'serviceable'). */
export const MONSTER_ITEM_STATUSES =
  monsterSchema.$defs.itemEntry.properties.status.oneOf[0].enum;

/** Boolean behaviour flag keys from properties (numbers filtered out). */
export const MONSTER_FLAG_KEYS = Object.entries(
  monsterSchema.properties.properties.properties
).filter(([, v]) => v.type === 'boolean').map(([k]) => k);

/** Shopkeeper store types. */
export const STORE_TYPES =
  monsterSchema.properties.shopkeeper_properties.properties.store_type_chances
    .propertyNames.enum;

/** Spawn modes ('random' | 'fixed'). */
export const SPAWN_MODES = monsterSchema.properties.spawn.items.properties.mode.enum;

/** Lowercase vanilla item names for monster gear (Barony's itemNameStrings
 *  are lowercase — the class schema's enum is the uppercase twin). */
export const ITEM_TYPES_LOWER = ITEM_TYPES.map((t) => t.toLowerCase());

/* ------------------------------------------------------------------ */
/* Spell + patch enums (from spell.schema.json / patch.schema.json).    */
/* ------------------------------------------------------------------ */

/** Spell effect payloads (map 1:1 to Barony spell elements). */
export const SPELL_PAYLOADS = spellSchema.properties.payload.enum;

/** How a spell is delivered ('missile' | 'missile_trio' | 'none'). */
export const SPELL_PROJECTILE_TYPES = spellSchema.properties.projectile_type.enum;

/** Spell id pattern ("namespace:spell"). */
export const SPELL_ID_PATTERN = new RegExp(spellSchema.properties.id.pattern);

/** Patch operation kinds (edit_field | add_entry | remove_field | multiply_field). */
export const PATCH_OPS = patchSchema.properties.operations.items.properties.op.enum;

/** Dependency string pattern ("[?!]namespace[@x.y.z]"). */
export const DEP_PATTERN = new RegExp(modSchema.properties.dependencies.items.pattern);

/** Barony version pattern (accepts an optional leading 'v'). */
export const BARONY_VERSION_PATTERN = new RegExp(modSchema.properties.barony_min_version.pattern);

/** A class's starting_spells accepts a vanilla SPELL_X constant OR a custom
 *  "namespace:spell" id — this matches either form. */
export const CLASS_SPELL_REF_PATTERN = /^(SPELL_[A-Z0-9_]+|[a-z][a-z0-9_]*:[a-z][a-z0-9_]*)$/;
