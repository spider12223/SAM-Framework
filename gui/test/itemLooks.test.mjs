/*
 * The Class Editor's "Look" picker comes from data/itemLooks.js, a table generated from the game's
 * items.json. These checks pin the facts an author relies on (a cloak has 4 looks) and the two ways the
 * table could do harm: offering a Look on an item whose appearance number is game data (a spell tome's
 * spell, a scepter's charges), or dropping a hand-written `appearance` when a class is saved again.
 */
import { ITEM_LOOKS, looksOf } from '../src/data/itemLooks.js';
import { ITEM_TYPES } from '../src/data/schemas.js';
import { entryFromJson, entryToJson } from '../src/data/equipment.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`FAIL: ${name}\n  got  ${g}\n  want ${w}`); }
}

// The facts the Discord question and the docs state.
eq('a cloak has 4 looks', looksOf('CLOAK'), 4);
eq('a hood has 4 looks', looksOf('HAT_HOOD'), 4);
eq('a ring has 5 looks', looksOf('RING_PROTECTION'), 5);
eq('an amulet has 6 looks', looksOf('AMULET_LIFESAVING'), 6);
eq('booze has 8 looks', looksOf('POTION_BOOZE'), 8);
eq('an item with one picture has none to choose', looksOf('IRON_SWORD'), 1);
eq('a custom item (not a vanilla type) has none to choose', looksOf('mymod:blade'), 1);

// Every name in the table is a real item type, so a typo in a regenerated table is caught.
const real = new Set(ITEM_TYPES);
eq('every listed type is in the class schema', Object.keys(ITEM_LOOKS).filter((t) => !real.has(t)), []);
eq('every count is at least 2', Object.values(ITEM_LOOKS).filter((n) => !(n >= 2)), []);

// Types whose appearance is game data, not a look (samItemAppearanceIsGameplay in sam_js_runtime.cpp,
// plus spellbooks, whose cover follows the spell, and the rubber duck). Offering a picker there would
// write a number that changes what the item is.
const NOT_A_LOOK = [
  'READABLE_BOOK', 'SPELL_ITEM', 'SCROLL_MAIL', 'ENCHANTED_FEATHER', 'MAGICSTAFF_SCEPTER',
  'TOOL_PLAYER_LOOT_BAG', 'TOOL_SENTRYBOT', 'TOOL_SPELLBOT', 'TOOL_GYROBOT', 'TOOL_DUMMYBOT', 'TOOL_DUCK',
  'TOME_SORCERY', 'TOME_MYSTICISM', 'TOME_THAUMATURGY', 'SPELLBOOK_FORCEBOLT', 'SPELLBOOK_FIREBALL',
];
for (const t of NOT_A_LOOK) eq(`${t} gets no Look picker`, looksOf(t), 1);
eq('no tome or spellbook is in the table', Object.keys(ITEM_LOOKS).filter((t) => /^(TOME_|SPELLBOOK_)/.test(t)), []);

// The editor keeps a number written by hand, on any item, and writes it back unchanged.
const hand = entryFromJson({ type: 'CLOAK', appearance: 2 });
eq('a hand-written appearance is read', hand.appearance, 2);
eq('and written back', entryToJson(hand).appearance, 2);
eq('look 1 is the default and is not written', 'appearance' in entryToJson(entryFromJson({ type: 'CLOAK' })), false);
const picked = { ...hand, appearance: 3 };
eq('picking the last look writes 3', entryToJson(picked), { type: 'CLOAK', appearance: 3 });
const wrapped = entryFromJson({ type: 'CLOAK', appearance: 6 });
eq('a number past the last look is kept as written, not rewritten', entryToJson(wrapped).appearance, 6);
eq('and it shows as look 3 of 4, the way the game wraps it', wrapped.appearance % looksOf('CLOAK'), 2);

console.log(`\n${pass} passed, ${fail} failed (itemLooks)`);
process.exit(fail ? 1 : 0);
