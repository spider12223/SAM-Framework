/*
 * Import a S.A.M mod .zip back into editable session state — the inverse of
 * exportZip.buildModFiles. Reads mod.json, then every declared class/item/
 * monster/spell/patch JSON, validates each, loads each class's companion
 * behavior script (classes/<name>.lua|js|ts), and captures any other file
 * (portraits, icons) as a base64 data URL so a round-trip is lossless.
 *
 * Returns { meta, classes, items, monsters, spells, patches, sounds, music, scripts, assets,
 * report } where report is a list of { path, message } for skipped files.
 *
 * mod.json "sounds" may hold any mix of: inline objects, paths to (legacy) sound JSON files,
 * and bare audio paths. All three become the same inline entries the builder exports. Audio
 * dropped in sounds/, sounds/replace/, music/ or music/replace/ with no entry at all (the
 * engine's folder convention) is listed as an explicit entry too, so it can be edited here;
 * the engine treats the two forms the same.
 */
import JSZip from 'jszip';
import { validate } from '@/lib/validate.js';
import {
  AUDIO_EXT_RE, audioKey, audioFiles, engineAudioSlug, mimeForPath, normalizeAudioEntry, MUSIC_ENTRY_KEYS,
} from '@/lib/audio.js';

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
    // Carried through untouched so an import/export cycle cannot delete custom .vox
    // declarations (monster bodies, item models, companions). The builder has no editor for
    // these; it just must not lose them.
    models: Array.isArray(manifest.models) ? manifest.models : [],
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
    sounds: Array.isArray(manifest.sounds) ? manifest.sounds : [],
    music: Array.isArray(manifest.music) ? manifest.music : [],
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
  // --- sounds: inline objects, legacy sound JSON paths, bare audio paths -------------------
  const ns = meta.namespace;
  const sounds = [];
  const soundJsonPaths = [];
  // First one wins on a duplicate id/target, exactly as the engine does ("keeping the first").
  const addAudio = (list, def, where) => {
    const key = audioKey(def);
    if (list.some((x) => audioKey(x) === key)) {
      report.push({ path: where, message: `a second entry for "${def.id ?? def.replace}" — the game keeps the first, so this one was skipped` });
      return;
    }
    list.push(def);
  };
  for (const [i, entry] of declared.sounds.entries()) {
    const where = `mod.json sounds[${i}]`;
    if (typeof entry === 'string') {
      if (/\.json$/i.test(entry)) {
        soundJsonPaths.push(entry);
        const raw = await readDef(zip, entry, 'sound', report);
        const def = normalizeAudioEntry(raw, ns);
        if (def) addAudio(sounds, def, entry);
      } else if (AUDIO_EXT_RE.test(entry)) {
        // The engine names a bare audio path after its file: "sounds/Boom.ogg" -> "<ns>:boom".
        const slug = engineAudioSlug(entry);
        addAudio(sounds, { id: ns ? `${ns}:${slug}` : slug, file: entry }, where);
      } else {
        report.push({ path: where, message: `"${entry}" is neither a sound JSON file nor an audio file — skipped` });
      }
      continue;
    }
    const def = normalizeAudioEntry(entry, ns);
    if (!def) { report.push({ path: where, message: 'not a sound entry — skipped' }); continue; }
    const res = validate('sound', def);
    if (!res.valid) {
      const first = res.errors[0];
      report.push({ path: where, message: `sound validation failed${first ? ` (${first.path} ${first.message})` : ''} — skipped` });
      continue;
    }
    addAudio(sounds, def, where);
  }

  // --- music: objects only ----------------------------------------------------------------
  const music = [];
  for (const [i, entry] of declared.music.entries()) {
    const where = `mod.json music[${i}]`;
    const def = normalizeAudioEntry(entry, ns, MUSIC_ENTRY_KEYS);
    if (!def) { report.push({ path: where, message: 'not a music entry (music entries are objects) — skipped' }); continue; }
    const res = validate('music', def);
    if (!res.valid) {
      const first = res.errors[0];
      report.push({ path: where, message: `music validation failed${first ? ` (${first.path} ${first.message})` : ''} — skipped` });
      continue;
    }
    addAudio(music, def, where);
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

  // Capture any non-declared, non-JSON, non-script file as an asset. Only the sound JSON
  // FILES count as declared here: a bare audio path in "sounds" IS the audio, and skipping it
  // would import the entry and silently drop its file.
  const declaredSet = new Set([
    'mod.json',
    ...declared.classes, ...declared.items, ...declared.monsters,
    ...declared.spells, ...declared.effects, ...declared.races, ...soundJsonPaths, ...declared.recipes, ...declared.patches,
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
      : AUDIO_EXT_RE.test(relPath) ? mimeForPath(relPath)
      : 'application/octet-stream';
    assets[relPath] = `data:${mime};base64,${base64}`;
  }

  // The engine's folder convention: audio in sounds/ is "<ns>:<file name>", audio in
  // sounds/replace/ replaces the vanilla sound it is named after (same for music/). The scan is
  // not recursive, and an entry in mod.json for the same id or target wins. A file some entry
  // already plays (a variant, a combat track) is left alone.
  const used = new Set([...sounds, ...music].flatMap(audioFiles));
  let byFolder = 0;
  for (const relPath of Object.keys(assets).sort()) {
    if (used.has(relPath) || !AUDIO_EXT_RE.test(relPath)) continue;
    const m = relPath.match(/^(sounds|music)\/(replace\/)?([^/]+)$/);
    if (!m) continue;
    const list = m[1] === 'sounds' ? sounds : music;
    const stem = m[3].slice(0, m[3].lastIndexOf('.'));
    const slug = engineAudioSlug(m[3]);
    const def = m[2] ? { replace: stem, file: relPath } : { id: ns ? `${ns}:${slug}` : slug, file: relPath };
    if (list.some((x) => audioKey(x) === audioKey(def))) continue;
    list.push(def);
    byFolder++;
  }
  if (byFolder) {
    report.push({ path: 'sounds/, music/', message: `${byFolder} audio file(s) had no entry in mod.json (the folder convention) — listed as entries so you can edit them` });
  }

  // An entry whose audio is not in the zip is kept (it may be meant for a file you add by
  // hand), but say so: otherwise it ships as an entry that plays nothing.
  for (const def of [...sounds, ...music]) {
    for (const f of audioFiles(def)) {
      if (!assets[f]) report.push({ path: f, message: `"${def.id ?? def.replace}" plays this file, but it is not in the zip` });
    }
  }

  return { meta, classes, items, monsters, spells, effects, races, sounds, music, recipes, patches, scripts, assets, report };
}
