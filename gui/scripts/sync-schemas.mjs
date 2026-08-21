/*
 * Copy the canonical JSON schemas into the site's public folder.
 *
 * There are two copies of every schema: schemas/ (canonical, what mod.json's $schema URL
 * ultimately points at) and gui/public/schemas/ (what the Mod Builder validates against
 * in the browser). They were kept in step by hand, and by v1.10.3 they had drifted:
 * item.schema.json had lost `traits` in the public copy, so the builder silently rejected
 * a field the framework accepts. Same failure mode as framework_version.json going stale
 * twice. Copy it instead of remembering to.
 *
 * Runs automatically before `npm run build` (the `prebuild` hook). If you build with
 * `npx vite build` you skip npm scripts entirely and this does NOT run -- use `npm run
 * build`, or run `npm run sync:schemas` first.
 */
import { readdirSync, readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';

const SRC = fileURLToPath(new URL('../../schemas/', import.meta.url));
const DST = fileURLToPath(new URL('../public/schemas/', import.meta.url));

mkdirSync(DST, { recursive: true });

let copied = 0;
let same = 0;
for (const name of readdirSync(SRC).filter((f) => f.endsWith('.json'))) {
  const from = readFileSync(join(SRC, name), 'utf8');
  // Parse before writing: a schema that does not parse would break the builder's validator
  // at runtime, and this is the last place it is cheap to catch.
  try {
    JSON.parse(from);
  } catch (e) {
    console.error(`sync-schemas: ${name} is not valid JSON -- ${e.message}`);
    process.exit(1);
  }
  let to = null;
  try {
    to = readFileSync(join(DST, name), 'utf8');
  } catch {
    /* not there yet */
  }
  if (to === from) {
    same += 1;
    continue;
  }
  writeFileSync(join(DST, name), from);
  console.log(`sync-schemas: updated ${name}`);
  copied += 1;
}
console.log(`sync-schemas: ${copied} updated, ${same} already in step.`);
