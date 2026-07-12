/*
 * Single source of truth: the three JSON schemas in SAM-Framework/schemas/.
 * Every enum list the GUI shows (item types, skills, categories, slots,
 * statuses, roll stats...) is DERIVED from them here — never hardcoded.
 * If the C++ side gains an item type or skill, updating the schema updates
 * the GUI automatically.
 */
import modSchema from '@schemas/mod.schema.json';
import classSchema from '@schemas/class.schema.json';
import itemSchema from '@schemas/item.schema.json';

export { modSchema, classSchema, itemSchema };

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
