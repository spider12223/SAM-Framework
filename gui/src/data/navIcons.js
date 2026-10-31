// The nav's pictures: Barony item icons the site already ships for its item pickers
// (public/item-icons/), one per destination, chosen for what the page makes.
const icon = (file) => `${import.meta.env.BASE_URL}item-icons/${file}`;

export const NAV_ICONS = {
  '/start': icon('Lantern.png'),
  '/dashboard': icon('crown.png'),
  '/class-editor': icon('SteelHelm.png'),
  '/item-editor': icon('SteelSword.png'),
  '/monster-editor': icon('skull_mask.png'),
  '/race-editor': icon('mask_masquerade.png'),
  '/spell-editor': icon('SpellbookYellow.png'),
  '/effect-editor': icon('PotionPurple.png'),
  '/sound-editor': icon('horn.png'),
  '/music-editor': icon('lute.png'),
  '/model-editor': icon('Rock.png'),
  '/recipe-editor': icon('tinker_toolkit.png'),
  '/patch-editor': icon('Scroll.png'),
  '/mod-builder': icon('Key_Gold.png'),
  '/validator': icon('Mirror.png'),
  '/api-reference': icon('magic_quill.png'),
};
