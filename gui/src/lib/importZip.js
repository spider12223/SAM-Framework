/*
 * Import a S.A.M mod .zip back into editable session state — the inverse of
 * exportZip.buildModFiles. Reads mod.json, then every declared class/item/
 * monster JSON, validates each, and captures any other file (portraits, etc.)
 * as a base64 data URL so a round-trip is lossless.
 *
 * Returns { meta, classes, items, monsters, assets, report } where report is a
 * list of { path, message } for files that were skipped (missing or invalid).
 */
import JSZip from 'jszip';
import { validate } from '@/lib/validate.js';

/** Strip the stamped "$schema" key — state stays URL-free; export re-stamps it.
 *  (Spreading { $schema: URL, ...def } lets def's own $schema WIN, so we must
 *  drop it on import or a stale/foreign URL survives.) */
function stripSchema(obj) {
  if (obj && typeof obj === 'object' && '$schema' in obj) {
    const { $schema, ...rest } = obj;
    return rest;
  }
  return obj;
}

/** Read + parse + validate one declared content file. */
async function readDef(zip, path, kind, report) {
  const entry = zip.file(path);
  if (!entry) {
    report.push({ path, message: `declared in mod.json but missing from the zip — skipped` });
    return null;
  }
  let def;
  try {
    def = stripSchema(JSON.parse(await entry.async('string')));
  } catch (err) {
    report.push({ path, message: `invalid JSON (${err.message}) — skipped` });
    return null;
  }
  const res = validate(kind, def);
  if (!res.valid) {
    const first = res.errors[0];
    report.push({ path, message: `${kind} validation failed${first ? ` (${first.path} ${first.message})` : ''} — skipped` });
    return null;
  }
  return def;
}

export async function parseModZip(file) {
  const report = [];
  const zip = await JSZip.loadAsync(file);

  const modEntry = zip.file('mod.json');
  if (!modEntry) {
    throw new Error('No mod.json at the root of the zip — is this a S.A.M mod?');
  }

  let manifest;
  try {
    manifest = stripSchema(JSON.parse(await modEntry.async('string')));
  } catch (err) {
    throw new Error(`mod.json is not valid JSON: ${err.message}`);
  }

  const modRes = validate('mod', manifest);
  if (!modRes.valid) {
    const first = modRes.errors[0];
    report.push({ path: 'mod.json', message: `manifest validation failed${first ? ` (${first.path} ${first.message})` : ''} — imported anyway` });
  }

  const meta = {
    namespace: manifest.namespace ?? '',
    name: manifest.name ?? '',
    author: manifest.author ?? '',
    version: manifest.version ?? '1.0.0',
    framework_min_version: manifest.framework_min_version ?? '0.1.0',
    description: manifest.description ?? '',
  };

  const declared = {
    classes: manifest.classes ?? [],
    items: manifest.items ?? [],
    monsters: manifest.monsters ?? [],
  };

  const classes = [];
  for (const p of declared.classes) {
    const def = await readDef(zip, p, 'class', report);
    if (def) classes.push(def);
  }
  const items = [];
  for (const p of declared.items) {
    const def = await readDef(zip, p, 'item', report);
    if (def) items.push(def);
  }
  const monsters = [];
  for (const p of declared.monsters) {
    const def = await readDef(zip, p, 'monster', report);
    if (def) monsters.push(def);
  }

  // Capture any non-declared, non-JSON file as an asset (portraits, icons...).
  const declaredSet = new Set([
    'mod.json', ...declared.classes, ...declared.items, ...declared.monsters,
  ]);
  const assets = {};
  const assetEntries = [];
  zip.forEach((relPath, entry) => {
    if (entry.dir) return;
    if (declaredSet.has(relPath)) return;
    if (relPath.toLowerCase().endsWith('.json')) return; // stray JSON isn't an asset
    assetEntries.push([relPath, entry]);
  });
  for (const [relPath, entry] of assetEntries) {
    const base64 = await entry.async('base64');
    const ext = relPath.split('.').pop().toLowerCase();
    const mime = ext === 'png' ? 'image/png'
      : ext === 'jpg' || ext === 'jpeg' ? 'image/jpeg'
      : 'application/octet-stream';
    assets[relPath] = `data:${mime};base64,${base64}`;
  }

  return { meta, classes, items, monsters, assets, report };
}
