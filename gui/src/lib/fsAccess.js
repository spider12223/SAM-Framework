/*
 * "Test in Barony" — write the current mod straight into the player's
 * Barony/mods/ folder via the File System Access API (Chromium only).
 *
 * The user picks their mods folder ONCE; the directory handle is stashed in a
 * one-key IndexedDB store so it survives reloads (a FileSystemDirectoryHandle
 * structured-clones into IDB but NOT localStorage). This is the SOLE, deliberate
 * exception to the app's no-persistence rule (see SAM_WORKFLOW): it stores a
 * folder permission grant, never any mod content.
 *
 * Non-Chromium browsers get isFsSupported === false; the UI falls back to a
 * plain zip download.
 */

export const isFsSupported =
  typeof window !== 'undefined' && typeof window.showDirectoryPicker === 'function';

/* -------- tiny promise wrapper over one IndexedDB key -------------------- */
const DB_NAME = 'sam-fs';
const STORE = 'handles';
const KEY = 'modsDir';
const LOG_KEY = 'samLog';

function idb() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(STORE);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function idbGet(key) {
  const db = await idb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readonly');
    const req = tx.objectStore(STORE).get(key);
    req.onsuccess = () => resolve(req.result ?? null);
    req.onerror = () => reject(req.error);
  });
}

async function idbSet(key, val) {
  const db = await idb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).put(val, key);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

/* -------- directory handle acquisition + permission ---------------------- */

async function ensurePermission(handle) {
  const opts = { mode: 'readwrite' };
  if ((await handle.queryPermission(opts)) === 'granted') return true;
  if ((await handle.requestPermission(opts)) === 'granted') return true;
  return false;
}

/** True if we already hold a usable (permission-granted) mods-dir handle. */
export async function hasSavedModsDir() {
  if (!isFsSupported) return false;
  try {
    const handle = await idbGet(KEY);
    if (!handle) return false;
    return (await handle.queryPermission({ mode: 'readwrite' })) === 'granted';
  } catch {
    return false;
  }
}

/**
 * Get the mods-dir handle, prompting the user to pick it if needed.
 * MUST be called from within a click handler (showDirectoryPicker needs a
 * user gesture). Pass { forcePick: true } for the "change folder" link.
 */
export async function getModsDirHandle({ forcePick = false } = {}) {
  if (!isFsSupported) throw new Error('File System Access API not supported in this browser.');

  if (!forcePick) {
    try {
      const saved = await idbGet(KEY);
      if (saved && (await ensurePermission(saved))) return saved;
    } catch {
      /* fall through to picker */
    }
  }

  const handle = await window.showDirectoryPicker({ id: 'barony-mods', mode: 'readwrite' });
  if (!(await ensurePermission(handle))) {
    throw new Error('Write permission was not granted for the selected folder.');
  }
  try { await idbSet(KEY, handle); } catch { /* non-fatal: still usable this session */ }
  return handle;
}

/* -------- write the mod files under <modsDir>/<folder>/ ------------------ */

/** Walk/create a slash-separated path under `root`, returning the file handle. */
async function fileHandleFor(root, relPath) {
  const parts = relPath.split('/').filter(Boolean);
  const fileName = parts.pop();
  let dir = root;
  for (const part of parts) {
    dir = await dir.getDirectoryHandle(part, { create: true });
  }
  return dir.getFileHandle(fileName, { create: true });
}

/* -------- read sam_log.txt (close the test loop) ------------------------ */

export const isFilePickSupported =
  typeof window !== 'undefined' && typeof window.showOpenFilePicker === 'function';

/**
 * Read the contents of the player's sam_log.txt. Prompts once for the file
 * (stored in IDB so later reads don't re-prompt). Pass { forcePick: true } to
 * choose a different file. Chromium only.
 */
export async function readSamLog({ forcePick = false } = {}) {
  if (!isFilePickSupported) throw new Error('Reading the log needs a Chromium browser (Chrome/Edge).');
  let handle = null;
  if (!forcePick) {
    try { handle = await idbGet(LOG_KEY); } catch { handle = null; }
  }
  if (handle) {
    const q = await handle.queryPermission?.({ mode: 'read' });
    if (q !== 'granted' && (await handle.requestPermission?.({ mode: 'read' })) !== 'granted') handle = null;
  }
  if (!handle) {
    const picked = await window.showOpenFilePicker({
      id: 'sam-log',
      types: [{ description: 'S.A.M log', accept: { 'text/plain': ['.txt', '.log'] } }],
    });
    handle = picked[0];
    try { await idbSet(LOG_KEY, handle); } catch { /* non-fatal */ }
  }
  const file = await handle.getFile();
  return file.text();
}

/**
 * Write buildModFiles() output into <modsDirHandle>/<folder>/…
 * files: [{ path, text? , base64? }]
 * Returns the number of files written.
 */
export async function writeModToDir(modsDirHandle, folder, files) {
  const modDir = await modsDirHandle.getDirectoryHandle(folder, { create: true });
  let written = 0;
  for (const f of files) {
    const fh = await fileHandleFor(modDir, f.path);
    const writable = await fh.createWritable();
    if (f.base64 !== undefined) {
      const bin = atob(f.base64);
      const bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      await writable.write(bytes);
    } else {
      await writable.write(f.text);
    }
    await writable.close();
    written++;
  }
  return written;
}
