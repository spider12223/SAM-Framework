/*
 * Bundle the current mod (manifest + saved classes + saved items) into a .zip
 * laid out exactly how S.A.M expects a mod folder:
 *   mod.json
 *   classes/<name>.json
 *   items/<name>.json
 * Ready to drop into Barony/mods/<folder>/.
 */
import JSZip from 'jszip';

/** "sam_test:shadow_knight" -> "shadow_knight" (file stem from the id). */
function fileStem(id, fallback) {
  const tail = (id ?? '').split(':')[1];
  return (tail && tail.trim()) || fallback;
}

/**
 * classes/items: arrays of schema-shaped objects (already validated).
 * meta: { namespace, name, author, version, framework_min_version, description }
 * Returns the Blob of the zip.
 */
export async function buildModZip(meta, classes, items) {
  const zip = new JSZip();

  const classPaths = classes.map((c, i) => `classes/${fileStem(c.id, `class_${i + 1}`)}.json`);
  const itemPaths = items.map((it, i) => `items/${fileStem(it.id, `item_${i + 1}`)}.json`);

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

  zip.file('mod.json', JSON.stringify(manifest, null, 2) + '\n');
  classes.forEach((c, i) => zip.file(classPaths[i], JSON.stringify(c, null, 2) + '\n'));
  items.forEach((it, i) => zip.file(itemPaths[i], JSON.stringify(it, null, 2) + '\n'));

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
