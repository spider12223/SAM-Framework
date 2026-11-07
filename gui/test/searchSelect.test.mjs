/*
 * The search box behind every picker (<SearchSelect>). The bugs these pin: Enter took the first option
 * that merely CONTAINS the typed text, so typing an option exactly could pick a longer one listed
 * before it; and with free values allowed, a value that is part of an option's name could not be
 * entered at all, because "use what I typed" only showed when nothing matched.
 */
import { searchMatches, enterPick, offerTyped } from '../src/lib/searchSelect.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`FAIL: ${name}\n  got  ${g}\n  want ${w}`); }
}

const spells = ['mymod:fireball', 'mymod:fire', 'SPELL_FIREBALL', 'SPELL_COLD'];
const pick = (q, allowCustom = false) => enterPick(spells, searchMatches(spells, q, 50), q, allowCustom);

eq('typing an option exactly picks it, not a longer one listed first', pick('mymod:fire'), 'mymod:fire');
eq('exact match ignores case', pick('spell_cold'), 'SPELL_COLD');
eq('a partial query still picks the first match', pick('fireb'), 'mymod:fireball');
eq('an empty query picks nothing', pick('   '), null);
eq('no match and no free values picks nothing', pick('lightning'), null);
eq('no match with free values picks the typed text', pick('mymod:spark', true), 'mymod:spark');
eq('free values: a value inside an option name is offered', offerTyped(spells, 'mymod:fir', true), true);
eq('free values: an exact option is not offered twice', offerTyped(spells, 'MYMOD:FIRE', true), false);
eq('no free values: nothing typed is offered', offerTyped(spells, 'mymod:fir', false), false);
eq('matches are case-insensitive substrings', searchMatches(spells, 'FIRE', 50), ['mymod:fireball', 'mymod:fire', 'SPELL_FIREBALL']);
eq('matches are capped', searchMatches(spells, '', 2), ['mymod:fireball', 'mymod:fire']);

console.log(`${pass} passed, ${fail} failed  (searchSelect)`);
process.exit(fail ? 1 : 0);
