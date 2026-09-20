/*
 * Shared, pure helpers for mod sounds and music. No React here, so the reducer, the exporter,
 * the importer and the tests all use the SAME rules.
 *
 * What the engine accepts (framework/sam_workshop.cpp samParseAudioDecl):
 *   mod.json "sounds": [ { id | replace, file: "x.ogg" | [..], volume?, loop? }, "sounds/x.json", "sounds/x.ogg" ]
 *   mod.json "music":  [ { id | replace, file: "x.ogg" | [..], loop?, floors?, maps?, combat? } ]
 *
 * Two engine facts shape this file:
 *   - An id and a replace target are compared case-insensitively ("SwingWeapon" and
 *     "swingweapon" are one target). The builder's keys are lower case for the same reason, so
 *     it can never hold two entries the engine would treat as one ("keeping the first").
 *   - Every audio file in sounds/ and music/ is ALSO picked up by the folder scan. Deleting an
 *     entry therefore has to delete its audio, or the file still ships and still registers.
 */
import { VANILLA_SOUNDS } from '@/data/vanillaSounds.js';
import { VANILLA_MUSIC } from '@/data/vanillaMusic.js';

export const AUDIO_EXTS = ['ogg', 'wav', 'mp3', 'flac'];
export const AUDIO_EXT_RE = /\.(ogg|wav|mp3|flac)$/i;
export const AUDIO_ACCEPT = '.ogg,.wav,.mp3,.flac,audio/ogg,audio/wav,audio/x-wav,audio/mpeg,audio/flac,audio/x-flac';

export const MAX_SOUND_BYTES = 2 * 1024 * 1024;   // sound effects: keep them small
export const MAX_MUSIC_BYTES = 30 * 1024 * 1024;  // a looping track is minutes long

const MIME = { ogg: 'audio/ogg', wav: 'audio/wav', mp3: 'audio/mpeg', flac: 'audio/flac' };

/** Same rule every editor uses for ids: lower case, runs of anything else become one "_". */
export function slugify(name) {
  return String(name ?? '').toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

/** "Sounds/Boom.OGG" -> "ogg" (lower case), or '' when it is not an audio file. */
export function audioExt(path) {
  const m = String(path ?? '').match(AUDIO_EXT_RE);
  return m ? m[1].toLowerCase() : '';
}

export function mimeForPath(path) {
  return MIME[audioExt(path)] || 'application/octet-stream';
}

/** A data URL's bytes as a Blob of the right audio type -- for <audio> previews. The type
 *  comes from the file extension, because an imported asset's data URL says octet-stream. */
export function dataUrlToBlob(dataUrl, type) {
  const s = String(dataUrl ?? '');
  const comma = s.indexOf(',');
  if (comma < 0) return null;
  const bin = atob(s.slice(comma + 1));
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Blob([bytes], { type: type || 'application/octet-stream' });
}

/** The unique key of a sound/music entry: its id, or the vanilla thing it replaces. */
export function audioKey(def) {
  if (def?.id) return `id:${String(def.id).toLowerCase()}`;
  return `replace:${String(def?.replace ?? '').toLowerCase()}`;
}

/** Every audio file an entry points at ("file" as one or a list, plus music's "combat"). */
export function audioFiles(def) {
  const f = def?.file;
  const list = Array.isArray(f) ? f : (f ? [f] : []);
  return def?.combat ? [...list, def.combat] : list;
}

/** The "file" field as the schema wants it: a string for one file, a list for several. */
export function fileField(paths) {
  return paths.length === 1 ? paths[0] : [...paths];
}

/**
 * Remove the assets of `oldFiles` that no remaining sound or music entry still uses.
 * Files that nothing ever declared (a hand-made mod's sounds/ folder) are never touched: only
 * files the removed or edited entry itself pointed at are candidates.
 */
export function pruneAudioAssets(assets, oldFiles, sounds, music) {
  if (!oldFiles || !oldFiles.length) return assets;
  const inUse = new Set([...(sounds || []), ...(music || [])].flatMap(audioFiles));
  let out = assets;
  for (const f of oldFiles) {
    if (!inUse.has(f) && out && Object.prototype.hasOwnProperty.call(out, f)) {
      if (out === assets) out = { ...assets };
      delete out[f];
    }
  }
  return out;
}

/**
 * Insert or update an entry in `list`, in place.
 *
 * `prevKey` is the key the entry had when the editor opened it. Without it, saving a sound
 * under a new name appended a second entry and left the old one behind -- two sounds, two
 * copies of the audio in the zip. With it, the entry at that position is replaced. Any OTHER
 * entry that already owns the new key is dropped so keys stay unique (the editors refuse that
 * case before it gets here; this is the backstop).
 */
export function upsertAudio(list, def, prevKey) {
  const newKey = audioKey(def);
  const at = list.findIndex((x) => audioKey(x) === (prevKey || newKey));
  const i = at >= 0 ? at : list.findIndex((x) => audioKey(x) === newKey);
  if (i < 0) return { list: [...list.filter((x) => audioKey(x) !== newKey), def], old: null };
  const old = list[i];
  const next = list.map((x, j) => (j === i ? def : x)).filter((x, j) => j === i || audioKey(x) !== newKey);
  return { list: next, old };
}

/**
 * Choose the bundle path for every file of one entry.
 *
 * files:     [{ path } | { pending: true, ext }] in the order the editor shows them. Files that
 *            already have a path keep it; pending uploads get <folder>/<slug>.<ext>, then
 *            <slug>_2, <slug>_3, ... skipping anything another entry (or any asset) uses.
 * ownOld:    paths the entry had before this edit; free to be reused unless someone else uses them.
 * otherUse:  paths every OTHER sound/music entry points at.
 * Returns the paths in the same order as `files`.
 */
export function assignAudioPaths({ files, folder, slug, suffix = '', assets = {}, ownOld = [], otherUse = [] }) {
  const reserved = new Set([...Object.keys(assets || {}), ...otherUse]);
  const others = new Set(otherUse);
  for (const p of ownOld) if (!others.has(p)) reserved.delete(p);
  for (const f of files) if (f.path) reserved.add(f.path);
  let n = 1;
  return files.map((f) => {
    if (f.path) return f.path;
    let p;
    do {
      p = `${folder}/${slug}${suffix}${n === 1 ? '' : `_${n}`}.${f.ext || 'ogg'}`;
      n++;
    } while (reserved.has(p));
    reserved.add(p);
    return p;
  });
}

/** Is `path` one the editor generated for `slug` (<folder>/<slug><suffix>[_N].<ext>)? */
export function isAutoNamed(path, folder, slug, suffix = '') {
  const esc = `${slug}${suffix}`.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return new RegExp(`^${folder}/${esc}(_\\d+)?\\.(ogg|wav|mp3|flac)$`, 'i').test(String(path ?? ''));
}

/**
 * The vanilla sound a file replaces JUST BY SITTING THERE, or null.
 *
 * <folder>/replace/<Name>.<ext> is the engine's drop-in convention: the folder scan reads
 * the file name and replaces the vanilla sound (or track) called <Name>, whatever mod.json
 * says. So this path carries a second, invisible claim alongside the manifest entry that
 * points at it -- and retargeting the entry does not move the file, so the OLD name goes on
 * being replaced and the author ends up replacing two sounds when they asked for one.
 * Switching the same entry from Replace to Add is the same thing with a worse ending: they
 * get their new id AND a replacement nobody asked for.
 */
export function dropInReplaceTarget(path, folder) {
  const m = String(path ?? '').match(new RegExp(`^${folder}/replace/([^/]+)\\.(ogg|wav|mp3|flac)$`, 'i'));
  return m ? m[1] : null;
}

/**
 * How each file row of an entry is to be stored, before paths are chosen:
 *   { path }             keep it where it is
 *   { ext, dataUrl }     a new upload
 *   { ext, from }        a bundled file to MOVE, because its current path still claims
 *                        something this entry no longer means. Two ways that happens:
 *                        the editor named the file after the entry and the entry was
 *                        renamed (otherwise "bang" ships as sounds/boom.ogg and the folder
 *                        scan registers a second, stray sound "<ns>:boom"); or the file
 *                        sits in <folder>/replace/ under a name this entry no longer
 *                        replaces, which is a replacement the author did not ask for.
 *
 * `replaceTarget` is what the entry replaces NOW, or null when it is an "add" entry -- a
 * drop-in file is only left alone when it is named after exactly that. Both editors pass it;
 * leaving it out means "this entry replaces nothing", which is the safe answer rather than
 * the quiet one.
 *
 * A file another entry also plays, or one that is not a bundled asset (a path typed by hand
 * for a file the author will add later), is never moved: there is nothing here to move it.
 */
export function planAudioRows(rows, { folder, oldSlug, newSlug, suffix = '', assets = {}, otherUse = [], replaceTarget = null }) {
  const shared = new Set(otherUse);
  const eq = (a, b) => String(a ?? '').toLowerCase() === String(b ?? '').toLowerCase();
  return rows.map((r) => {
    if (!r.path) return { ext: r.ext, dataUrl: r.dataUrl };
    const mine = !!assets?.[r.path] && !shared.has(r.path);
    const dropIn = dropInReplaceTarget(r.path, folder);
    const renamed = !!oldSlug && !!newSlug && oldSlug !== newSlug && isAutoNamed(r.path, folder, oldSlug, suffix);
    const staleDropIn = dropIn !== null && !(replaceTarget != null && replaceTarget !== '' && eq(dropIn, replaceTarget));
    return (mine && (renamed || staleDropIn)) ? { ext: audioExt(r.path), from: r.path } : { path: r.path };
  });
}

/* ------------------------------------------------------------------ manifest entries */

const SOUND_KEYS = ['id', 'replace', 'file', 'volume', 'loop'];
const MUSIC_KEYS = ['id', 'replace', 'file', 'loop', 'floors', 'maps', 'combat'];

function pick(def, keys) {
  const out = {};
  for (const k of keys) if (def?.[k] !== undefined) out[k] = def[k];
  return out;
}

/** A saved sound as it goes into mod.json "sounds" (known keys only, in a readable order). */
export function soundManifestEntry(def) { return pick(def, SOUND_KEYS); }

/** A saved track as it goes into mod.json "music". */
export function musicManifestEntry(def) { return pick(def, MUSIC_KEYS); }

/** "boom" -> "mymod:boom". The engine does the same to a bare id, so this changes nothing
 *  about what plays; it just gives the builder one spelling per sound. */
export function withNamespace(id, ns) {
  const s = String(id ?? '');
  if (!s || s.includes(':') || !ns) return s;
  return `${ns}:${s}`;
}

/**
 * One mod.json "sounds"/"music" object (or a legacy sound JSON file's contents) as a state entry.
 * Returns null when it is not an object.
 */
export function normalizeAudioEntry(raw, ns, keys = SOUND_KEYS) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const def = {};
  for (const k of keys) if (raw[k] !== undefined) def[k] = raw[k];
  if (def.id !== undefined) def.id = withNamespace(def.id, ns);
  return def;
}
export const MUSIC_ENTRY_KEYS = MUSIC_KEYS;
export const SOUND_ENTRY_KEYS = SOUND_KEYS;

/** "sounds/Big Boom!.ogg" -> the id the engine gives a bare audio path ("big_boom_" -- the
 *  engine maps each non-alphanumeric character to "_" and does NOT collapse runs). */
export function engineAudioSlug(path) {
  const base = String(path ?? '').split(/[\\/]/).pop() || '';
  const stem = base.includes('.') ? base.slice(0, base.lastIndexOf('.')) : base;
  return stem.replace(/[^A-Za-z0-9]/g, '_').toLowerCase();
}

/* ------------------------------------------------------------------ vanilla lookups */

/*
 * Mirrors SAMSounds::vanillaIndicesFor (framework/sam_sounds.cpp):
 *   1. all digits  -> that index
 *   2. an EXACT file name, case-insensitive -> every sound with that name (a few exist twice)
 *   3. a GROUP name, case-insensitive       -> every sound in the group
 * A name that is both (seven are: "Casting" is a group of four AND the name of one of them)
 * means everything that answers to it, so replace: "Casting" swaps the whole group; the one
 * base sound alone is picked by its NUMBER. Groups that differ only in case ("Zap" and "zap")
 * are one group to the engine. The picker asks this function what a name means rather than
 * assuming, so what it shows is what the game will do.
 */
const STEM_LC = new Map();   // lower(name)  -> [{ i, name, group }]
const GROUP_LC = new Map();  // lower(group) -> [{ i, name, group }]
const BY_INDEX = new Map();
for (const g of VANILLA_SOUNDS.groups) {
  for (const s of g.sounds) {
    const e = { i: s.i, name: s.name, group: g.group };
    const sk = s.name.toLowerCase();
    const gk = g.group.toLowerCase();
    STEM_LC.set(sk, [...(STEM_LC.get(sk) || []), e]);
    GROUP_LC.set(gk, [...(GROUP_LC.get(gk) || []), e]);
    BY_INDEX.set(s.i, e);
  }
}
export const VANILLA_SOUND_COUNT = BY_INDEX.size;

/** What the engine plays for a replace target: { via: 'index'|'sound'|'group', sounds } or null. */
export function resolveVanillaSound(target) {
  const t = String(target ?? '').trim();
  if (!t) return null;
  if (/^\d+$/.test(t)) {
    const s = BY_INDEX.get(Number(t));
    return s ? { via: 'index', sounds: [s] } : null;
  }
  const lc = t.toLowerCase();
  const st = STEM_LC.get(lc);
  const gr = GROUP_LC.get(lc);
  if (!st && !gr) return null;
  if (!gr) return { via: 'sound', sounds: st };
  if (!st) return { via: 'group', sounds: gr };
  const all = new Map();
  for (const s of [...gr, ...st]) all.set(s.i, s);
  const sounds = [...all.values()].sort((a, b) => a.i - b.i);
  // A group with one member whose name is its own ("Levelup" of 1) is still just that sound.
  return { via: sounds.length > st.length ? 'group' : 'sound', sounds };
}

/** The value to store so the game replaces exactly this one sound: its name, unless that name
 *  also means a bigger group ("Casting"), in which case its number. (A file the game lists
 *  twice keeps its name: both copies are the same sound.) */
export function singleSoundTarget(s) {
  const r = resolveVanillaSound(s.name);
  const lc = String(s.name).toLowerCase();
  return r && r.sounds.every((x) => x.name.toLowerCase() === lc) ? s.name : String(s.i);
}

/**
 * What a replace target means, in words: { kind, label } or null if the game has nothing by
 * that name (the engine then logs it and ignores the entry).
 */
export function describeSoundTarget(target) {
  const r = resolveVanillaSound(target);
  if (!r) return null;
  const n = r.sounds.length;
  if (r.via === 'index') return { kind: 'index', label: `sound #${r.sounds[0].i} (${r.sounds[0].name})` };
  if (r.via === 'sound') {
    const s = r.sounds[0];
    const groupSize = GROUP_LC.get(s.group.toLowerCase())?.length ?? 1;
    if (n > 1) return { kind: 'sound', label: `${s.name} (the game has ${n} copies of it; all are replaced)` };
    return { kind: 'sound', label: groupSize > 1 ? `just ${s.name} (#${s.i}, one of the ${groupSize} ${s.group} sounds)` : `${s.name} (1 sound)` };
  }
  return { kind: 'group', label: n > 1 ? `the whole ${r.sounds[0].group} group (${n} sounds)` : `${r.sounds[0].name} (1 sound)` };
}

/** Does picking this group's NAME replace more than one sound? False only for a group of one. */
export function groupNameMeansGroup(group) {
  const r = resolveVanillaSound(group);
  return !!r && r.via === 'group';
}

/**
 * Search the game's sounds. Matches group names and exact names (case-insensitive), or an
 * index when the query is a number. Groups whose name starts with the query sort first.
 * Returns { groups: [{ group, sounds, hit: 'group'|'sound' }], total } with at most `limit` groups.
 */
export function searchVanillaSounds(query, limit = 40) {
  const q = String(query ?? '').trim().toLowerCase();
  if (!q) return { groups: [], total: 0 };
  let hits;
  if (/^\d+$/.test(q)) {
    const s = BY_INDEX.get(Number(q));
    hits = s ? [{ group: s.group, sounds: [{ i: s.i, name: s.name }], hit: 'sound' }] : [];
    // A number can also be part of a name ("thunder2"), so fall through to the text search.
    for (const g of VANILLA_SOUNDS.groups) {
      const sounds = g.sounds.filter((x) => x.name.toLowerCase().includes(q));
      if (sounds.length) hits.push({ group: g.group, sounds, hit: 'sound' });
    }
  } else {
    hits = [];
    for (const g of VANILLA_SOUNDS.groups) {
      const gl = g.group.toLowerCase();
      if (gl.includes(q)) { hits.push({ ...g, hit: 'group', rank: gl.startsWith(q) ? 0 : 1 }); continue; }
      const sounds = g.sounds.filter((s) => s.name.toLowerCase().includes(q));
      if (sounds.length) hits.push({ group: g.group, sounds, hit: 'sound', rank: 2 });
    }
    hits.sort((a, b) => a.rank - b.rank);
  }
  return { groups: hits.slice(0, limit), total: hits.length };
}

/** Name of a vanilla sound index (for the block builder), or ''. */
export function vanillaSoundName(i) {
  return BY_INDEX.get(Number(i))?.name || '';
}

const MUSIC_LC = new Set(VANILLA_MUSIC.map((m) => m.name.toLowerCase()));
/** Whether a music replace target is one of the listed names. Exact file names ("mines03") are
 *  also accepted by the engine, so an unknown name is a warning, not an error. */
export function isKnownMusicTarget(name) {
  return MUSIC_LC.has(String(name ?? '').toLowerCase());
}

/* ------------------------------------------------------------------ list parsing */

/** "7, 8 9" -> { values: [7, 8, 9], bad: [] }. Anything that is not a whole number is reported. */
export function parseIntList(text) {
  const values = [];
  const bad = [];
  for (const tok of String(text ?? '').split(/[\s,]+/).filter(Boolean)) {
    if (/^-?\d+$/.test(tok)) values.push(Number(tok)); else bad.push(tok);
  }
  return { values, bad };
}

/** "My Hub, Minetown" -> ["My Hub", "Minetown"]. Map names may contain spaces, so only commas split. */
export function parseNameList(text) {
  return String(text ?? '').split(',').map((s) => s.trim()).filter(Boolean);
}
