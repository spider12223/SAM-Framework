/*
 * Custom block packs. The bugs these pin: a pack containing a null block crashed the import instead of
 * reporting it, and a parameter named " x" (with a space) passed validation, which trims names, but
 * was saved untrimmed, so the template's {x} was never filled and the block never generated anything.
 */
import { validateBlock, importPack, toCatalogEntry } from '../src/lib/customBlocks.js';

let pass = 0, fail = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { pass++; } else { fail++; console.log(`FAIL: ${name}\n  got  ${g}\n  want ${w}`); }
}

let threw = false;
try { validateBlock(null); } catch { threw = true; }
eq('validating a null block reports instead of throwing', threw, false);

let pack;
threw = false;
try { pack = importPack(JSON.stringify({ sam_block_pack: 1, blocks: [null] })); } catch { threw = true; }
eq('a pack with a null block imports without throwing', threw, false);
eq('and the null block is reported, not imported', pack && pack.blocks.length, 0);

const spaced = { id: 'say', kind: 'action', label: 'Say', lua: 'sam_log({msg})', params: [{ name: ' msg', type: 'text', default: '' }] };
eq('a parameter name with a space passes validation (names are trimmed)', validateBlock(spaced).ok, true);
const entry = toCatalogEntry(spaced);
eq('the catalog entry uses the trimmed name', entry.params[0].name, 'msg');
eq('and the template is filled from it', entry.lua({ msg: '"hello"' }), 'sam_log("hello")');

console.log(`${pass} passed, ${fail} failed  (customBlocks)`);
process.exit(fail ? 1 : 0);
