/*
 * Bundle the current mod (manifest + saved classes/items/monsters/spells/patches
 * + behavior scripts + binary assets) laid out exactly how S.A.M expects a mod
 * folder:
 *   mod.json
 *   classes/<name>.json      classes/<name>.lua|js|ts   (companion behavior script)
 *   items/<name>.json
 *   monsters/<name>.json
 *   spells/<name>.json
 *   patches/<name>.json
 *   portraits/<name>.png     (uploaded assets, at their declared paths)
 * Ready to drop into Barony/mods/<folder>/.
 *
 * buildModFiles() is the single source of truth for layout + $schema stamping;
 * both the zip export (buildModZip) and the direct Test-in-Barony disk writer
 * (lib/fsAccess.js) consume its [{ path, text | base64 }] list, so the two
 * outputs can never drift apart.
 */
import JSZip from 'jszip';

// Public URLs of the schemas (served from GitHub Pages). Stamped into each
// exported file as "$schema" so the modder gets autocomplete + validation the
// moment they open the file in VS Code — no setup required.
const SCHEMA_BASE = 'https://spider12223.github.io/SAM-Framework/schemas';
const MOD_SCHEMA = `${SCHEMA_BASE}/mod.schema.json`;
const CLASS_SCHEMA = `${SCHEMA_BASE}/class.schema.json`;
const ITEM_SCHEMA = `${SCHEMA_BASE}/item.schema.json`;
const MONSTER_SCHEMA = `${SCHEMA_BASE}/monster.schema.json`;
const SPELL_SCHEMA = `${SCHEMA_BASE}/spell.schema.json`;
const EFFECT_SCHEMA = `${SCHEMA_BASE}/effect.schema.json`;
const RACE_SCHEMA = `${SCHEMA_BASE}/race.schema.json`;
const SOUND_SCHEMA = `${SCHEMA_BASE}/sound.schema.json`;
const PATCH_SCHEMA = `${SCHEMA_BASE}/patch.schema.json`;

/** "sam_test:shadow_knight" -> "shadow_knight" (file stem from the id). */
export function fileStem(id, fallback) {
  const tail = (id ?? '').split(':')[1];
  return (tail && tail.trim()) || fallback;
}

/** A patch has no id — derive a filename stem from its target path.
 *  "items/items.json" -> "items_items". */
export function patchStem(target, fallback) {
  const s = String(target ?? '').replace(/\.json$/i, '').replace(/[^a-z0-9]+/gi, '_').replace(/^_+|_+$/g, '');
  return s || fallback;
}

/** Manifest-relative paths for each collection, exactly as the zip lays them out. */
export function contentPaths(classes, items, monsters, spells = [], patches = [], effects = [], races = [], sounds = []) {
  return {
    classPaths: classes.map((c, i) => `classes/${fileStem(c.id, `class_${i + 1}`)}.json`),
    itemPaths: items.map((it, i) => `items/${fileStem(it.id, `item_${i + 1}`)}.json`),
    monsterPaths: (monsters ?? []).map((m, i) => `monsters/${fileStem(m.id, `monster_${i + 1}`)}.json`),
    spellPaths: (spells ?? []).map((s, i) => `spells/${fileStem(s.id, `spell_${i + 1}`)}.json`),
    effectPaths: (effects ?? []).map((e, i) => `effects/${fileStem(e.id, `effect_${i + 1}`)}.json`),
    racePaths: (races ?? []).map((r, i) => `races/${fileStem(r.id, `race_${i + 1}`)}.json`),
    soundPaths: (sounds ?? []).map((s, i) => `sounds/${fileStem(s.id, `sound_${i + 1}`)}.json`),
    patchPaths: (patches ?? []).map((p, i) => `patches/${patchStem(p.target, `patch_${i + 1}`)}.json`),
  };
}

/** The mod.json object (sans $schema — callers stamp it when writing). */
export function buildManifest(meta, paths) {
  const { classPaths, itemPaths, monsterPaths, spellPaths, effectPaths, racePaths, soundPaths, patchPaths } = paths;
  const manifest = {
    namespace: meta.namespace,
    name: meta.name,
    author: meta.author ?? '',
    version: meta.version,
    framework_min_version: meta.framework_min_version,
  };
  // Optional version-gating fields — only emitted when set.
  for (const k of ['framework_max_version', 'barony_min_version', 'barony_max_version', 'incompatible_with_barony_version']) {
    if (meta[k]) manifest[k] = meta[k];
  }
  manifest.dependencies = Array.isArray(meta.dependencies) ? meta.dependencies : [];
  manifest.classes = classPaths;
  manifest.items = itemPaths;
  if (patchPaths && patchPaths.length) manifest.patches = patchPaths;
  if (monsterPaths && monsterPaths.length) manifest.monsters = monsterPaths;
  if (spellPaths && spellPaths.length) manifest.spells = spellPaths;
  if (effectPaths && effectPaths.length) manifest.effects = effectPaths;
  if (racePaths && racePaths.length) manifest.races = racePaths;
  if (soundPaths && soundPaths.length) manifest.sounds = soundPaths;
  manifest.plugins = [];
  manifest.description = meta.description ?? '';
  return manifest;
}

/** The companion behavior-script path for a class, or null if it has no script.
 *  scripts: { classId -> { lang, code } }. */
export function scriptPathFor(classDef, i, scripts) {
  const s = scripts?.[classDef.id];
  if (!s || !s.code || !s.code.trim()) return null;
  return { path: `classes/${fileStem(classDef.id, `class_${i + 1}`)}.${s.lang}`, code: s.code };
}

/**
 * The complete file list for the mod folder:
 *   [{ path, text }]  for JSON + script files ("$schema" stamped on JSON)
 *   [{ path, base64 }] for binary assets (portraits etc.)
 * mod = { meta, classes, items, monsters, spells, patches, scripts, assets }.
 */
export function buildModFiles(mod) {
  const {
    meta, classes = [], items = [], monsters = [], spells = [], effects = [], races = [], sounds = [], patches = [],
    scripts = {}, assets = {},
  } = mod;
  const paths = contentPaths(classes, items, monsters, spells, patches, effects, races, sounds);
  const manifest = buildManifest(meta, paths);

  const files = [
    // "$schema" first so it sits at the top of each file (editors expect it there).
    { path: 'mod.json', text: JSON.stringify({ $schema: MOD_SCHEMA, ...manifest }, null, 2) + '\n' },
    ...classes.map((c, i) => ({
      path: paths.classPaths[i],
      text: JSON.stringify({ $schema: CLASS_SCHEMA, ...c }, null, 2) + '\n',
    })),
    ...items.map((it, i) => ({
      path: paths.itemPaths[i],
      text: JSON.stringify({ $schema: ITEM_SCHEMA, ...it }, null, 2) + '\n',
    })),
    ...monsters.map((m, i) => ({
      path: paths.monsterPaths[i],
      text: JSON.stringify({ $schema: MONSTER_SCHEMA, ...m }, null, 2) + '\n',
    })),
    ...spells.map((s, i) => ({
      path: paths.spellPaths[i],
      text: JSON.stringify({ $schema: SPELL_SCHEMA, ...s }, null, 2) + '\n',
    })),
    ...effects.map((e, i) => ({
      path: paths.effectPaths[i],
      text: JSON.stringify({ $schema: EFFECT_SCHEMA, ...e }, null, 2) + '\n',
    })),
    ...races.map((r, i) => ({
      path: paths.racePaths[i],
      text: JSON.stringify({ $schema: RACE_SCHEMA, ...r }, null, 2) + '\n',
    })),
    ...sounds.map((s, i) => ({
      path: paths.soundPaths[i],
      text: JSON.stringify({ $schema: SOUND_SCHEMA, ...s }, null, 2) + '\n',
    })),
    ...patches.map((p, i) => ({
      path: paths.patchPaths[i],
      text: JSON.stringify({ $schema: PATCH_SCHEMA, ...p }, null, 2) + '\n',
    })),
  ];

  // Companion behavior scripts, laid out next to their class JSON.
  classes.forEach((c, i) => {
    const sp = scriptPathFor(c, i, scripts);
    if (sp) files.push({ path: sp.path, text: sp.code.endsWith('\n') ? sp.code : sp.code + '\n' });
  });
  // Race behavior scripts, next to their race JSON (same mechanism as class scripts).
  races.forEach((r, i) => {
    const s = scripts?.[r.id];
    if (s && s.code && s.code.trim()) {
      files.push({ path: `races/${fileStem(r.id, `race_${i + 1}`)}.${s.lang}`, text: s.code.endsWith('\n') ? s.code : s.code + '\n' });
    }
  });

  for (const [path, dataUrl] of Object.entries(assets)) {
    const base64 = String(dataUrl).split(',')[1];
    if (base64) files.push({ path, base64 });
  }
  return files;
}

/** Returns the Blob of the zip.
 *  Everything is nested under a single `<namespace>/` folder so unzipping drops a clean
 *  mod folder straight into Barony/mods/, instead of spraying loose files into whatever
 *  directory you unzipped in. Import (parseModZip) strips this wrapper back off. */
export async function buildModZip(mod) {
  const zip = new JSZip();
  const folder = (mod?.meta?.namespace || 'sam_mod').trim() || 'sam_mod';
  for (const f of buildModFiles(mod)) {
    const p = `${folder}/${f.path}`;
    if (f.base64 !== undefined) zip.file(p, f.base64, { base64: true });
    else zip.file(p, f.text);
  }
  return zip.generateAsync({ type: 'blob' });
}

/** Trigger a browser download of a Blob. */
export function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}
