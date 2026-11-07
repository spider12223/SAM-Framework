/*
 * Where an uploaded picture ships (the Race Editor's icon, the Spell Editor's icon, the Class Editor's
 * portrait and selected portrait). The bug these pin: upload a PNG, then clear the path box. The note
 * under the Upload button kept saying "It will export at races/<name>.png" (or spells/, or portraits/)
 * while the JSON had no path and the PNG was never written to the zip, so the author believed a picture
 * shipped that did not.
 */
import { assetPath } from '../src/lib/assetPath.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`FAIL: ${name}\n  got  ${g}\n  want ${w}`); }
}

const RACE = 'races/frostborn.png', SPELL = 'spells/shadow_bolt.png', PORTRAIT = 'portraits/warden.png';

eq('a typed path wins', assetPath('races/ice.png', false, RACE), 'races/ice.png');
eq('a typed path wins over an upload', assetPath('races/ice.png', true, RACE), 'races/ice.png');
eq('the typed path is trimmed', assetPath('  races/ice.png  ', true, RACE), 'races/ice.png');
eq('no path and no upload ships nothing', assetPath('', false, RACE), '');
eq('a blank path and no upload ships nothing', assetPath('   ', false, RACE), '');
eq('an upload with a cleared path still ships, at the default path', assetPath('', true, RACE), RACE);
eq('an upload with a blank path still ships, at the default path', assetPath('   ', true, RACE), RACE);
eq('a missing path is treated as empty', assetPath(undefined, true, RACE), RACE);
eq('a spell icon upload with a cleared path ships at spells/', assetPath('', true, SPELL), SPELL);
eq('a portrait upload with a cleared path ships at portraits/', assetPath('', true, PORTRAIT), PORTRAIT);
eq('a cleared image with a typed path still names the path', assetPath('portraits/x.png', false, PORTRAIT), 'portraits/x.png');

console.log(`${pass} passed, ${fail} failed  (assetPath)`);
process.exit(fail ? 1 : 0);
