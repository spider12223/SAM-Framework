/*
 * Barony's ten equipment slots, and which vanilla items belong in each.
 *
 * Starting items serialize with `equip: true` to send an item to its NATURAL slot — the
 * engine decides the slot from the item type (a helmet always goes on the head), so the
 * paperdoll only ever offers a slot items that actually fit it. Classification is by name
 * pattern, verified against all 524 vanilla ItemTypes: 224 map to a slot, the rest are
 * backpack-only (potions, scrolls, food, gems, thrown, tools, spellbooks, quivers, keys).
 *
 * Custom items ("namespace:item") carry no name pattern, so they're allowed in any slot and
 * in the backpack — the modder knows what their own item is.
 */

/** Paperdoll slots in Barony's inventory-screen arrangement: two side columns flanking the
 *  character, with the weapon and shield at the bottom. `col` drives layout. */
export const EQUIP_SLOTS = [
  { id: 'helmet',      label: 'Head',    icon: '🪖', col: 'left'   },
  { id: 'breastplate', label: 'Body',    icon: '🛡️', col: 'left'   },
  { id: 'gloves',      label: 'Hands',   icon: '🧤', col: 'left'   },
  { id: 'boots',       label: 'Feet',    icon: '🥾', col: 'left'   },
  { id: 'cloak',       label: 'Cloak',   icon: '🧥', col: 'right'  },
  { id: 'amulet',      label: 'Amulet',  icon: '📿', col: 'right'  },
  { id: 'ring',        label: 'Ring',    icon: '💍', col: 'right'  },
  { id: 'mask',        label: 'Face',    icon: '🕶️', col: 'right'  },
  { id: 'weapon',      label: 'Weapon',  icon: '⚔️', col: 'bottom' },
  { id: 'shield',      label: 'Offhand', icon: '🛡', col: 'bottom' },
];

export const EQUIP_SLOT_IDS = EQUIP_SLOTS.map((s) => s.id);
export const findSlot = (id) => EQUIP_SLOTS.find((s) => s.id === id);

/**
 * The equipment slot a vanilla item belongs to, or null for backpack-only.
 * Order matters: more specific patterns first (a "SPIKED_GAUNTLET" is a weapon, not gloves).
 */
export function equipSlotOf(type) {
  const t = String(type || '');
  if (t.includes(':')) return null;                                  // custom item: no fixed slot
  if (/^MAGICSTAFF_/.test(t)) return 'weapon';
  if (/SHIELD|SCUTUM|BUCKLER/.test(t)) return 'shield';
  if (/TORCH|LANTERN|CRYSTAL_SHARD|LIGHT_SOURCE/.test(t)) return 'shield'; // offhand light
  if (/BOOTS|SHOES|LOAFERS|CLEAT|GREAVE/.test(t)) return 'boots';
  if (/GLOVES|GAUNTLET|BRACERS/.test(t) && !/SPIKED_GAUNTLET|KNUCKLE/.test(t)) return 'gloves';
  if (/CLOAK|CAPE|BACKPACK|APRON/.test(t)) return 'cloak';
  if (/HELM|^HAT_|HOOD|COIF|CROWN|CIRCLET|LAURELS|TURBAN|HEADDRESS|MITER|PHRYGIAN/.test(t)) return 'helmet';
  // Body armour. PAULDRONS and SHAWL are torso pieces (Barony's breastplate slot,
  // entity.cpp checkEquipType), not a cloak — a tester caught iron pauldrons landing there.
  if (/BREASTPLATE|BREASTPIECE|TUNIC|DOUBLET|GAMBESON|HAUBERK|^ROBE|SUEDE|PAULDRONS|SHAWL/.test(t)) return 'breastplate';
  if (/GLASSES|MONOCLE|EYEPATCH|BLINDFOLD|^MASK|_MASK/.test(t)) return 'mask';
  if (/^AMULET_/.test(t)) return 'amulet';
  if (/^RING_/.test(t)) return 'ring';
  if (/SWORD|DAGGER|RAPIER|CLAYMORE|ANELACE|FALSHION|FALCHION|AXE|MACE|FLAIL|SHILLELAGH|MORNINGSTAR|WARHAMMER|SPEAR|HALBERD|TRIDENT|LANCE|GLAIVE|POLEARM|PARTISAN|BOW|CROSSBOW|SLING|LONGBOW|KNUCKLE|SPIKED_GAUNTLET|WHIP|SCYTHE|QUARTERSTAFF|SICKLE|MAUL|CAT_O/.test(t)) return 'weapon';
  return null;
}

/** Vanilla item types that fit a given equipment slot (for that slot's picker). */
export function itemsForSlot(slotId, allTypes) {
  return allTypes.filter((t) => equipSlotOf(t) === slotId);
}

/**
 * Blessing (beatitude) is a signed integer. In game it reads as -1 / +2 etc.; a starter
 * usually sits in a small range, so the picker offers -3..+3 (wider values are still legal
 * JSON if a modder types them). 0 is a plain, uncursed item.
 */
export const BLESSING_MIN = -3;
export const BLESSING_MAX = 3;
export function blessingLabel(b) {
  const n = Number(b) || 0;
  if (n === 0) return 'Uncursed';
  return (n > 0 ? `Blessed +${n}` : `Cursed ${n}`);
}

/** Item condition, worst to best (Barony's Status enum; note the spelling "SERVICABLE"). */
export const STATUSES = ['BROKEN', 'DECREPIT', 'WORN', 'SERVICABLE', 'EXCELLENT'];

/**
 * The five Status values (BROKEN..EXCELLENT, indices 0-4) are ONE enum, but the game renders
 * them with a completely different word per item category: a "serviceable" sword, a "flawed"
 * ring, a "plain" potion, a "marked" scroll, a "slightly aged" ration are all Status 3. So on
 * a potion, "condition" is the wrong word — it's the potion's look (evaporated / cloudy /
 * swirly / plain / bubbly). Straight from lang/en.txt 982-1006; the stored value stays the
 * Status enum, only the label changes, so no JSON or engine change is involved.
 */
const STATUS_WORDS = {
  gear:   ['broken', 'decrepit', 'worn', 'serviceable', 'excellent'],      // weapon/armor/staff/tool/thrown (982)
  gem:    ['destroyed', 'cracked', 'rough', 'flawed', 'flawless'],         // amulet/ring/gem (987)
  potion: ['evaporated', 'cloudy', 'swirly', 'plain', 'bubbly'],          // potion (992)
  scroll: ['shredded', 'torn', 'faded', 'marked', 'brand new'],           // scroll/book/tome (997)
  food:   ['rotten', 'mouldy', 'aged', 'slightly aged', 'fresh'],         // food (1002)
};

/** How the Status control should read for an item of `category`: its field label, the word
 *  for each Status value, and an optional hint. */
export function statusStyleFor(category) {
  switch (category) {
    case 'POTION':
      return { label: 'Look', words: STATUS_WORDS.potion, hint: 'How it looks before it’s identified.' };
    case 'GEM': case 'AMULET': case 'RING':
      return { label: 'Quality', words: STATUS_WORDS.gem };
    case 'SCROLL': case 'SPELLBOOK': case 'BOOK': case 'TOME_SPELL': case 'SPELL_CAT': case 'TOME_SPELL_CAT':
      return { label: 'Condition', words: STATUS_WORDS.scroll };
    case 'FOOD':
      return { label: 'Freshness', words: STATUS_WORDS.food };
    default:
      return { label: 'Condition', words: STATUS_WORDS.gear };
  }
}

/** The category-specific word for a Status enum value (e.g. potion + 'EXCELLENT' -> 'bubbly'). */
export function statusWord(category, status) {
  const i = STATUSES.indexOf(status);
  return statusStyleFor(category).words[i < 0 ? 3 : i];
}

/* ---- loadout entry <-> starting_items JSON ------------------------------------ */

const rndKey = () => Math.random().toString(36).slice(2, 9);

/** A fresh loadout entry with schema defaults. `slot` is the UI hint for custom items. */
export function newEntry(type, equip, slot) {
  return {
    _key: rndKey(), type, count: 1, equip: !!equip,
    beatitude: 0, status: 'SERVICABLE', identified: true, hotbar_slot: -1,
    ...(slot ? { _slot: slot } : {}),
  };
}

/** Load a starting_items JSON entry into UI state (fills defaults, adds a UI key). */
export function entryFromJson(si) {
  return {
    _key: rndKey(), type: si.type,
    count: si.count ?? 1, equip: !!si.equip,
    beatitude: si.beatitude ?? 0, status: si.status ?? 'SERVICABLE',
    identified: si.identified ?? true, hotbar_slot: si.hotbar_slot ?? -1,
  };
}

/** Serialize one entry, OMITTING defaults so untouched classes export byte-identical JSON.
 *  UI-only fields (_key, _slot) are never written. */
export function entryToJson(it) {
  const e = { type: it.type };
  if (it.count > 1) e.count = it.count;
  if (it.equip) e.equip = true;
  const b = Math.trunc(Number(it.beatitude) || 0);
  if (b !== 0) e.beatitude = b;
  if (it.status && it.status !== 'SERVICABLE') e.status = it.status;
  if (it.identified === false) e.identified = false;
  if (Number(it.hotbar_slot) >= 0) e.hotbar_slot = Number(it.hotbar_slot);
  return e;
}
