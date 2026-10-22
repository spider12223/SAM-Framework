/*
 * checkOnHitEffect decides what the Spell Editor tells an author about the "on-hit effect" box.
 * It has to agree with the engine's lookup (framework/sam_spells.cpp): exact custom id, then a
 * vanilla name, then a raw slot. A wrong answer here is worse than none -- a green tick on an id
 * the engine ignores is exactly the silent failure this check exists to prevent.
 */
import { checkOnHitEffect, describeOnHitEffect } from '../src/lib/spellEffectRef.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`FAIL: ${name}\n  got  ${g}\n  want ${w}`); }
}

const mine = ['mymod:plague', 'mymod:frost_bite'];

eq('empty box says nothing', checkOnHitEffect('', mine).status, 'empty');
eq('whitespace only is empty', checkOnHitEffect('   ', mine).status, 'empty');
eq('exact custom id resolves', checkOnHitEffect('mymod:plague', mine), { status: 'custom', id: 'mymod:plague' });
eq('custom id with stray spaces is trimmed, as the editor saves it', checkOnHitEffect('  mymod:plague ', mine).status, 'custom');
eq('vanilla name with EFF_', checkOnHitEffect('EFF_POISONED', mine).status, 'vanilla');
eq('vanilla name without EFF_, any case', checkOnHitEffect('poisoned', mine).status, 'vanilla');
eq('raw custom slot number', checkOnHitEffect('135', mine).status, 'slot');
eq('CUSTOM:n form', checkOnHitEffect('CUSTOM:140', mine).status, 'slot');
eq('a number outside the custom range is not accepted', checkOnHitEffect('12', mine).status, 'unknown');

// The reported case: the effect exists as mymod:plague and the author typed something else.
eq('wrong case is flagged and names the exact id', checkOnHitEffect('MyMod:Plague', mine), { status: 'wrong-case', id: 'mymod:plague' });
eq('bare name is flagged and names the exact id', checkOnHitEffect('plague', mine), { status: 'missing-namespace', id: 'mymod:plague' });
eq('EFF_ prefix on a custom effect is flagged', checkOnHitEffect('EFF_PLAGUE', mine), { status: 'missing-namespace', id: 'mymod:plague' });
eq('display-style capitalised name is flagged', checkOnHitEffect('Plague', mine), { status: 'missing-namespace', id: 'mymod:plague' });
eq('a different effect is simply unknown', checkOnHitEffect('mymod:nonsense', mine).status, 'unknown');
eq('with no custom effects at all, a bare name is unknown', checkOnHitEffect('plague', []).status, 'unknown');

// A near miss must never read as acceptable: only these statuses get the green tone.
for (const text of ['plague', 'Plague', 'EFF_PLAGUE', 'MyMod:Plague', 'mymod:nonsense', 'zzz']) {
  const d = describeOnHitEffect(checkOnHitEffect(text, mine));
  eq(`"${text}" is shown as a warning`, d && d.tone, 'warn');
}
for (const text of ['mymod:plague', 'EFF_POISONED', '136']) {
  const d = describeOnHitEffect(checkOnHitEffect(text, mine));
  eq(`"${text}" is shown as fine`, d && d.tone, 'ok');
}
eq('empty text has no message', describeOnHitEffect(checkOnHitEffect('', mine)), null);

console.log(`\n${pass} passed, ${fail} failed (spellEffectRef)`);
process.exit(fail ? 1 : 0);
