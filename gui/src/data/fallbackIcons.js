// A picture for an item that has none of its own: a Barony item icon (already shipped in
// public/item-icons/ for the pickers) that stands for its kind. Used where an ItemType has no PNG,
// for custom items (which never do), and as the faint ghost in an empty paperdoll slot.
export const itemIconUrl = (file) => (file ? `${import.meta.env.BASE_URL}item-icons/${file}` : null);

const KIND = {
  sword: 'SteelSword.png', shield: 'IronShield.png', bow: 'Shortbow.png', axe: 'IronBattleAxe.png',
  mace: 'IronMace.png', spear: 'IronSpear.png', potion: 'PotionRed.png', scroll: 'Scroll.png',
  book: 'SpellbookYellow.png', food: 'Bread.png', torch: 'Torch.png', ring: 'Ring1.png',
  amulet: 'Amulet1.png', gem: 'GemRed.png', helm: 'IronHelm.png', boots: 'LeatherBoots.png',
  gloves: 'Gloves.png', armor: 'IronBreastpiece.png', cloak: 'Cloak1.png', staff: 'RedMysticStaff.png',
  key: 'Key_Iron.png', tool: 'tinker_toolkit.png', thrown: 'CrystalShuriken.png', spell: 'OrbPurple.png',
  mask: 'mask_eyepatch.png',
};

/** The stand-in icon file for a vanilla ItemType name, by keyword. */
export function kindIconFile(type) {
  const t = String(type || '');
  if (/SWORD|DAGGER|RAPIER|CLAYMORE|ANELACE|FALSHION/.test(t)) return KIND.sword;
  if (/SHIELD|SCUTUM/.test(t)) return KIND.shield;
  if (/BOW|CROSSBOW|SLING|QUIVER/.test(t)) return KIND.bow;
  if (/TOMAHAWK|CHAKRAM|SHURIKEN|BOOMERANG|DART/.test(t)) return KIND.thrown;
  if (/AXE/.test(t)) return KIND.axe;
  if (/MACE|FLAIL|SHILLELAGH|KNUCKLES/.test(t)) return KIND.mace;
  if (/SPEAR|HALBERD|TRIDENT|LANCE|POLEARM|GLAIVE/.test(t)) return KIND.spear;
  if (/POTION/.test(t)) return KIND.potion;
  if (/SCROLL/.test(t)) return KIND.scroll;
  if (/SPELLBOOK|TOME|BOOK/.test(t)) return KIND.book;
  if (/FOOD|BREAD|CHEESE|APPLE|MEAT|FISH|RATION/.test(t)) return KIND.food;
  if (/TORCH|LANTERN/.test(t)) return KIND.torch;
  if (/RING/.test(t)) return KIND.ring;
  if (/AMULET/.test(t)) return KIND.amulet;
  if (/GEM/.test(t)) return KIND.gem;
  if (/MASK|GLASSES|VISOR/.test(t)) return KIND.mask;
  if (/HAT|HELM|HOOD|CROWN|CIRCLET/.test(t)) return KIND.helm;
  if (/BOOTS|LOAFERS|CLEAT/.test(t)) return KIND.boots;
  if (/GLOVES|GAUNTLETS|BRACERS/.test(t)) return KIND.gloves;
  if (/CLOAK/.test(t)) return KIND.cloak;
  if (/ROBE|DOUBLET|BREASTPIECE|TUNIC|GAMBESON|HAUBERK|SHAWL|APRON/.test(t)) return KIND.armor;
  if (/STAFF|SCEPTER/.test(t)) return KIND.staff;
  if (/KEY/.test(t)) return KIND.key;
  return KIND.sword;
}

/** The stand-in icon file for an item category (a custom item's `category`). */
export const CATEGORY_ICON_FILE = {
  WEAPON: KIND.sword, ARMOR: KIND.armor, POTION: KIND.potion, SCROLL: KIND.scroll,
  SPELLBOOK: KIND.book, TOME_SPELL: KIND.book, MAGICSTAFF: KIND.staff, RING: KIND.ring,
  AMULET: KIND.amulet, GEM: KIND.gem, FOOD: KIND.food, TOOL: KIND.tool, THROWN: KIND.thrown,
  BOOK: KIND.book, SPELL_CAT: KIND.spell,
};

/** The stand-in icon file for each paperdoll slot (data/equipment.js ids). */
export const SLOT_ICON_FILE = {
  helmet: KIND.helm, breastplate: KIND.armor, gloves: KIND.gloves, boots: KIND.boots,
  cloak: KIND.cloak, amulet: KIND.amulet, ring: KIND.ring, mask: KIND.mask,
  weapon: KIND.sword, shield: KIND.shield,
};
