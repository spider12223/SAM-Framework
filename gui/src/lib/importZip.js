/*
 * Import a S.A.M mod .zip back into editable session state — the inverse of
 * exportZip.buildModFiles. Reads mod.json, then every declared class/item/
 * monster/spell/patch JSON, validates each, loads each class's companion
 * behavior script (classes/<name>.lua|js|ts), and captures any other file
 * (portraits, icons) as a base64 data URL so a round-trip is lossless.
 *
 * Returns { meta, classes, items, monsters, spells, patches, scripts, assets,
 * report } where report is a list of { path, message } for skipped files.
 */
import JSZip from 'jszip';
import { validate } from '@/lib/validate.js';

const SCRIPT_LANGS = ['ts', 'js', 'lua']; // detection order mirrors the loader

/** Strip the stamped "$schema" key — state stays URL-free; export re-stamps it. */
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

/** Wrap a JSZip so every lookup is transparently prefixed with the mod's folder. Exports
 *  now nest everything under `<namespace>/`; this lets the rest of the importer keep using
 *  plain root-relative paths (mod.json, classes/x.json) whether or not that wrapper exists. */
function withPrefix(zip, prefix) {
  if (!prefix) return zip;
  return {
    file: (p) => zip.file(prefix + p),
    forEach: (cb) => zip.forEach((relPath, entry) => {
      if (relPath.startsWith(prefix)) cb(relPath.slice(prefix.length), entry);
    }),
  };
}

export async function parseModZip(file) {
  const report = [];
  const scriptPaths = new Set(); // companion scripts consumed, so they aren't re-captured as assets
  const raw = await JSZip.loadAsync(file);

  // mod.json is at the root of an old zip, or under a single top folder in a new one.
  let prefix = '';
  if (!raw.file('mod.json')) {
    const wrapped = Object.keys(raw.files).filter((p) => p.endsWith('/mod.json') && p.split('/').length === 2);
    if (wrapped.length === 1) prefix = wrapped[0].slice(0, -'mod.json'.length);
  }
  const zip = withPrefix(raw, prefix);

  const modEntry = zip.file('mod.json');
  if (!modEntry) {
    throw new Error('No mod.json in the zip — is this a S.A.M mod?');
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
    framework_max_version: manifest.framework_max_version ?? '',
    barony_min_version: manifest.barony_min_version ?? '',
    barony_max_version: manifest.barony_max_version ?? '',
    incompatible_with_barony_version: manifest.incompatible_with_barony_version ?? '',
    dependencies: Array.isArray(manifest.dependencies) ? manifest.dependencies : [],
    description: manifest.description ?? '',
  };

  const declared = {
    classes: manifest.classes ?? [],
    items: manifest.items ?? [],
    monsters: manifest.monsters ?? [],
    spells: manifest.spells ?? [],
    effects: manifest.effects ?? [],
    races: manifest.races ?? [],
    sounds: manifest.sounds ?? [],
    recipes: manifest.recipes ?? [],
    patches: manifest.patches ?? [],
  };

  const classes = [];
  const scripts = {};
  for (const p of declared.classes) {
    const def = await readDef(zip, p, 'class', report);
    if (!def) continue;
    classes.push(def);
    // Load the companion behavior script sitting next to the class JSON.
    for (const lang of SCRIPT_LANGS) {
      const sp = p.replace(/\.json$/i, `.${lang}`);
      const entry = zip.file(sp);
      if (entry) {
        scripts[def.id] = { lang, code: await entry.async('string') };
        scriptPaths.add(sp);
        break;
      }
    }
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
  const spells = [];
  for (const p of declared.spells) {
    const def = await readDef(zip, p, 'spell', report);
    if (def) spells.push(def);
  }
  const effects = [];
  for (const p of declared.effects) {
    const def = await readDef(zip, p, 'effect', report);
    if (def) effects.push(def);
  }
  const races = [];
  for (const p of declared.races) {
    const def = await readDef(zip, p, 'race', report);
    if (!def) continue;
    races.push(def);
    // Load the companion behavior script sitting next to the race JSON.
    for (const lang of SCRIPT_LANGS) {
      const sp = p.replace(/\.json$/i, `.${lang}`);
      const entry = zip.file(sp);
      if (entry) {
        scripts[def.id] = { lang, code: await entry.async('string') };
        scriptPaths.add(sp);
        break;
      }
    }
  }
  const sounds = [];
  for (const p of declared.sounds) {
    const def = await readDef(zip, p, 'sound', report);
    if (def) sounds.push(def);
  }
  const recipes = [];
  for (const p of declared.recipes) {
    const def = await readDef(zip, p, 'recipe', report);
    if (def) recipes.push(def);
  }
  const patches = [];
  for (const p of declared.patches) {
    const def = await readDef(zip, p, 'patch', report);
    if (def) patches.push(def);
  }

  // Capture any non-declared, non-JSON, non-script file as an asset.
  const declaredSet = new Set([
    'mod.json',
    ...declared.classes, ...declared.items, ...declared.monsters,
    ...declared.spells, ...declared.effects, ...declared.races, ...declared.sounds, ...declared.recipes, ...declared.patches,
    ...scriptPaths,
  ]);
  const assets = {};
  const assetEntries = [];
  zip.forEach((relPath, entry) => {
    if (entry.dir) return;
    if (declaredSet.has(relPath)) return;
    if (/\.(json|lua|js|ts)$/i.test(relPath)) return; // JSON + scripts aren't assets
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

  return { meta, classes, items, monsters, spells, effects, races, sounds, recipes, patches, scripts, assets, report };
}
