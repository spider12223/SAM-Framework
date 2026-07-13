/*
 * Bundle the current mod (manifest + saved classes/items/monsters + binary
 * assets like portraits) laid out exactly how S.A.M expects a mod folder:
 *   mod.json
 *   classes/<name>.json
 *   items/<name>.json
 *   monsters/<name>.json
 *   portraits/<name>.png   (any uploaded assets, at their declared paths)
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

/** "sam_test:shadow_knight" -> "shadow_knight" (file stem from the id). */
export function fileStem(id, fallback) {
  const tail = (id ?? '').split(':')[1];
  return (tail && tail.trim()) || fallback;
}

/** Manifest-relative paths for each collection, exactly as the zip lays them out. */
export function contentPaths(classes, items, monsters) {
  return {
    classPaths: classes.map((c, i) => `classes/${fileStem(c.id, `class_${i + 1}`)}.json`),
    itemPaths: items.map((it, i) => `items/${fileStem(it.id, `item_${i + 1}`)}.json`),
    monsterPaths: (monsters ?? []).map((m, i) => `monsters/${fileStem(m.id, `monster_${i + 1}`)}.json`),
  };
}

/** The mod.json object (sans $schema — callers stamp it when writing). */
export function buildManifest(meta, classPaths, itemPaths, monsterPaths) {
  const manifest = {
    namespace: meta.namespace,
    name: meta.name,
    author: meta.author ?? '',
    version: meta.version,
    framework_min_version: meta.framework_min_version,
    dependencies: [],
    classes: classPaths,
    items: itemPaths,
    plugins: [],
    description: meta.description ?? '',
  };
  if (monsterPaths && monsterPaths.length) manifest.monsters = monsterPaths;
  return manifest;
}

/**
 * The complete file list for the mod folder:
 *   [{ path, text }]  for JSON files ("$schema" stamped first)
 *   [{ path, base64 }] for binary assets (portraits etc.)
 * classes/items/monsters: arrays of schema-shaped objects (already validated).
 * assets: { 'portraits/x.png': 'data:image/png;base64,...' }
 */
export function buildModFiles(meta, classes, items, monsters = [], assets = {}) {
  const { classPaths, itemPaths, monsterPaths } = contentPaths(classes, items, monsters);
  const manifest = buildManifest(meta, classPaths, itemPaths, monsterPaths);

  const files = [
    // "$schema" first so it sits at the top of each file (editors expect it there).
    { path: 'mod.json', text: JSON.stringify({ $schema: MOD_SCHEMA, ...manifest }, null, 2) + '\n' },
    ...classes.map((c, i) => ({
      path: classPaths[i],
      text: JSON.stringify({ $schema: CLASS_SCHEMA, ...c }, null, 2) + '\n',
    })),
    ...items.map((it, i) => ({
      path: itemPaths[i],
      text: JSON.stringify({ $schema: ITEM_SCHEMA, ...it }, null, 2) + '\n',
    })),
    ...monsters.map((m, i) => ({
      path: monsterPaths[i],
      text: JSON.stringify({ $schema: MONSTER_SCHEMA, ...m }, null, 2) + '\n',
    })),
  ];

  for (const [path, dataUrl] of Object.entries(assets)) {
    const base64 = String(dataUrl).split(',')[1];
    if (base64) files.push({ path, base64 });
  }
  return files;
}

/** Returns the Blob of the zip. */
export async function buildModZip(meta, classes, items, monsters = [], assets = {}) {
  const zip = new JSZip();
  for (const f of buildModFiles(meta, classes, items, monsters, assets)) {
    if (f.base64 !== undefined) zip.file(f.path, f.base64, { base64: true });
    else zip.file(f.path, f.text);
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
