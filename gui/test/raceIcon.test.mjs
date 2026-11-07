/*
 * The Race Editor's icon path. The bug these pin: upload a PNG, then clear the path box. The note under
 * the Upload button kept saying "It will export at races/<name>.png" while the JSON had no `icon` and the
 * PNG was never written to the zip, so the author believed a picture shipped that did not.
 */
import { raceIconPath } from '../src/lib/raceIcon.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`FAIL: ${name}\n  got  ${g}\n  want ${w}`); }
}

eq('a typed path wins', raceIconPath('races/ice.png', false, 'frostborn'), 'races/ice.png');
eq('a typed path wins over an upload', raceIconPath('races/ice.png', true, 'frostborn'), 'races/ice.png');
eq('the typed path is trimmed', raceIconPath('  races/ice.png  ', true, 'frostborn'), 'races/ice.png');
eq('no path and no upload ships nothing', raceIconPath('', false, 'frostborn'), '');
eq('a blank path and no upload ships nothing', raceIconPath('   ', false, 'frostborn'), '');
eq('an upload with a cleared path still ships, at the default path', raceIconPath('', true, 'frostborn'), 'races/frostborn.png');
eq('an upload with a blank path still ships, at the default path', raceIconPath('   ', true, 'frostborn'), 'races/frostborn.png');
eq('a missing path is treated as empty', raceIconPath(undefined, true, 'ice_folk'), 'races/ice_folk.png');

console.log(`${pass} passed, ${fail} failed  (raceIcon)`);
process.exit(fail ? 1 : 0);
