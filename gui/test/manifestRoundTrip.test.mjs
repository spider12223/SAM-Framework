/*
 * A mod imported into the Mod Builder and exported again must keep its mod.json. The bug this pins:
 * the importer kept only `models` from the manifest's extra lists, and the exporter wrote
 * `plugins: []` always, so an import/export cycle silently deleted a mod's declared `images`
 * (sam_show_image / sam_hud_image ids stopped resolving), its `rooms` (prefab rooms never loaded)
 * and its `plugins` list.
 */
import JSZip from 'jszip';
import { parseModZip } from '../src/lib/importZip.js';
import { buildModFiles } from '../src/lib/exportZip.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`FAIL: ${name}\n  got  ${g}\n  want ${w}`); }
}

const manifest = {
  namespace: 'roundtrip',
  name: 'Round Trip',
  author: 'test',
  version: '1.0.0',
  framework_min_version: '3.9.0',
  dependencies: [],
  classes: [],
  items: [],
  models: [{ id: 'roundtrip:statue', file: 'models/statue.vox' }],
  images: [{ id: 'roundtrip:jumpscare', file: 'art/jumpscare.png' }],
  rooms: { mine: ['rooms/mine_shrine.lmp'], swamp: ['rooms/swamp_hut.lmp', 'rooms/swamp_pond.lmp'] },
  plugins: [],
  description: 'A mod that declares pictures and rooms.',
};

const zip = new JSZip();
zip.file('mod.json', JSON.stringify(manifest, null, 2));
zip.file('art/jumpscare.png', 'not really a png');
zip.file('rooms/mine_shrine.lmp', 'room');
zip.file('rooms/swamp_hut.lmp', 'room');
zip.file('rooms/swamp_pond.lmp', 'room');
zip.file('models/statue.vox', 'vox');
const bytes = await zip.generateAsync({ type: 'uint8array' });

const mod = await parseModZip(bytes);
const files = buildModFiles(mod);
const out = JSON.parse(files.find((f) => f.path === 'mod.json').text);

eq('models survive an import and export', out.models, manifest.models);
eq('images survive an import and export', out.images, manifest.images);
eq('rooms survive an import and export', out.rooms, manifest.rooms);
eq('the description survives', out.description, manifest.description);
eq('the room files still ship', ['rooms/mine_shrine.lmp', 'rooms/swamp_hut.lmp', 'rooms/swamp_pond.lmp'].every((p) => files.some((f) => f.path === p)), true);
eq('the picture still ships', files.some((f) => f.path === 'art/jumpscare.png'), true);

// A mod without the optional lists gets none written (no empty "images": [] noise).
const plain = await parseModZip(await (() => {
  const z = new JSZip();
  z.file('mod.json', JSON.stringify({ namespace: 'plain', name: 'Plain', author: '', version: '1.0.0', framework_min_version: '3.9.0', dependencies: [], classes: [], items: [] }));
  return z.generateAsync({ type: 'uint8array' });
})());
const plainOut = JSON.parse(buildModFiles(plain).find((f) => f.path === 'mod.json').text);
eq('no images key when the mod had none', 'images' in plainOut, false);
eq('no rooms key when the mod had none', 'rooms' in plainOut, false);

console.log(`${pass} passed, ${fail} failed  (manifestRoundTrip)`);
process.exit(fail ? 1 : 0);
