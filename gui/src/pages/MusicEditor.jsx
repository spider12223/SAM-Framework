/*
 * Music Editor — add a music track, or replace one of the game's.
 *
 * Each track becomes one object in mod.json "music" (the "music" item of mod.schema.json):
 *   add:      { "id": "mymod:boss", "file": "music/boss.ogg" }            -> sam_play_music / a monster's theme
 *             { "id": "mymod:deep", "file": "music/deep.ogg", "floors": [7, 8], "maps": ["My Hub"],
 *               "combat": "music/deep_combat.ogg" }                          -> that floor's or map's own music
 *   replace:  { "replace": "mines", "file": ["music/mines.ogg", "music/mines_2.ogg"] }
 * Several files take turns. The audio is bundled as mod assets under music/.
 *
 * Same in-place editing as the Sound Editor: the form remembers which entry it opened.
 */
import { useEffect, useMemo, useState } from 'react';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, Select, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';
import { ModeSwitch, AudioFileList, VanillaMusicPicker, newRowKey } from '@/components/AudioParts.jsx';
import {
  slugify, audioKey, audioFiles, fileField, assignAudioPaths, planAudioRows, isKnownMusicTarget,
  parseIntList, parseNameList, MAX_MUSIC_BYTES,
} from '@/lib/audio.js';

const LOOP_OPTIONS = [
  { value: 'auto', label: 'Automatic — one file loops, several take turns' },
  { value: 'yes', label: 'Always loop' },
  { value: 'no', label: 'Play once, then hand the music back (a sting)' },
];

const blank = () => ({ mode: 'add', name: '', replace: '', rows: [], loop: 'auto', floors: '', maps: '', combat: [] });

function stateFrom(def) {
  if (!def) return blank();
  const floors = def.floors === undefined ? [] : [].concat(def.floors);
  const maps = def.maps === undefined ? [] : [].concat(def.maps);
  return {
    mode: def.replace !== undefined && !def.id ? 'replace' : 'add',
    name: def.id ? String(def.id).split(':').pop() : '',
    replace: def.replace !== undefined ? String(def.replace) : '',
    rows: (Array.isArray(def.file) ? def.file : def.file ? [def.file] : []).map((path) => ({ key: newRowKey(), path })),
    loop: def.loop === true ? 'yes' : def.loop === false ? 'no' : 'auto',
    floors: floors.join(', '),
    maps: maps.join(', '),
    combat: def.combat ? [{ key: newRowKey(), path: def.combat }] : [],
  };
}

export default function MusicEditor() {
  const { meta, sounds, music = [], assets, editing, dispatch } = useMod();
  const editDef = editing?.kind === 'music' ? music.find((m) => audioKey(m) === editing.id) : null;

  const [origKey, setOrigKey] = useState(editDef ? audioKey(editDef) : null);
  const [form, setForm] = useState(() => stateFrom(editDef));
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');
  const set = (patch) => { setForm((f) => ({ ...f, ...patch })); setSavedAs(''); };

  useEffect(() => {
    if (editing?.kind === 'music') dispatch({ type: 'clearEditing' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const namespace = meta.namespace || 'mymod';
  const slug = slugify(form.name);
  const trackId = `${namespace}:${slug}`;
  const replaceValue = form.replace.trim();
  const isAdd = form.mode === 'add';
  const fileSlug = isAdd ? slug : slugify(replaceValue || 'replacement');

  const origDef = origKey ? music.find((m) => audioKey(m) === origKey) : null;
  const oldSlug = origDef ? slugify(origDef.id ? String(origDef.id).split(':').pop() : String(origDef.replace)) : '';
  const otherUse = useMemo(
    () => [...(sounds ?? []), ...music.filter((m) => audioKey(m) !== origKey)].flatMap(audioFiles),
    [sounds, music, origKey],
  );
  // How each file is stored (kept, uploaded, or carried over to a new name), then where. The
  // main files first, then the combat file with a "_combat" suffix, so the two lists can never
  // be handed the same path.
  // replaceTarget is what this entry replaces NOW. A file the author dropped in
  // music/replace/ is named after the track it replaces and the engine reads that name on
  // its own, so retargeting the entry has to move the file out -- otherwise the old track
  // goes on being replaced as well as the new one. A combat track is only ever part of an
  // "add" entry, so it replaces nothing.
  const plan = useMemo(
    () => planAudioRows(form.rows, {
      folder: 'music', oldSlug, newSlug: fileSlug, assets, otherUse,
      replaceTarget: isAdd ? null : replaceValue,
    }),
    [form.rows, isAdd, replaceValue, oldSlug, fileSlug, assets, otherUse],
  );
  const combatPlan = useMemo(
    () => planAudioRows(form.combat, {
      folder: 'music', oldSlug, newSlug: fileSlug, suffix: '_combat', assets, otherUse, replaceTarget: null,
    }),
    [form.combat, oldSlug, fileSlug, assets, otherUse],
  );
  const paths = useMemo(() => assignAudioPaths({
    files: plan, folder: 'music', slug: fileSlug, assets, ownOld: audioFiles(origDef), otherUse,
  }), [plan, fileSlug, assets, origDef, otherUse]);
  const combatPaths = useMemo(() => assignAudioPaths({
    files: combatPlan, folder: 'music', slug: fileSlug, suffix: '_combat', assets, ownOld: audioFiles(origDef), otherUse: [...otherUse, ...paths],
  }), [combatPlan, fileSlug, assets, origDef, otherUse, paths]);

  const floors = parseIntList(form.floors);
  const maps = parseNameList(form.maps);

  const def = useMemo(() => {
    const d = isAdd ? { id: trackId } : { replace: replaceValue };
    if (paths.length) d.file = fileField(paths);
    if (form.loop === 'yes') d.loop = true;
    if (form.loop === 'no') d.loop = false;
    if (isAdd) {
      if (floors.values.length) d.floors = floors.values;
      if (maps.length) d.maps = maps;
      if (combatPaths.length) d.combat = combatPaths[0];
    }
    return d;
    // floors/maps are re-parsed each render; depend on their CONTENT, not the fresh arrays.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isAdd, trackId, replaceValue, paths, combatPaths, form.loop, JSON.stringify(floors.values), JSON.stringify(maps)]);

  const save = () => {
    setSavedAs('');
    const errs = [];
    if (isAdd && !form.name.trim()) errs.push({ path: 'name', message: 'Give the track a name — scripts and monsters play it by that name.' });
    if (!isAdd && !replaceValue) errs.push({ path: 'replace', message: 'Pick the game track to replace.' });
    if (!form.rows.length) errs.push({ path: 'file', message: 'Upload at least one audio file.' });
    if (isAdd && floors.bad.length) errs.push({ path: 'floors', message: `Floors are whole numbers separated by commas — "${floors.bad.join('", "')}" is not one.` });
    const clash = music.find((m) => audioKey(m) === audioKey(def) && audioKey(m) !== origKey);
    if (clash) {
      errs.push(isAdd
        ? { path: 'id', message: `There is already a track called ${def.id}. Pick another name, or open that one from the Mod Builder.` }
        : { path: 'replace', message: `This mod already replaces "${clash.replace}". Open that entry from the Mod Builder instead.` });
    }
    if (!errs.length) {
      const res = validate('music', def);
      if (!res.valid) errs.push(...res.errors);
    }
    if (errs.length) { setErrors(errs); return; }
    setErrors([]);
    // New uploads, and files carried over to the new name (the reducer then drops the old copy).
    const store = (p, to) => {
      if (p.dataUrl) dispatch({ type: 'setAsset', path: to, dataUrl: p.dataUrl });
      else if (p.from && p.from !== to) dispatch({ type: 'setAsset', path: to, dataUrl: assets[p.from] });
    };
    plan.forEach((p, i) => store(p, paths[i]));
    if (isAdd) combatPlan.forEach((p, i) => store(p, combatPaths[i]));
    dispatch({ type: 'saveMusic', def, prevKey: origKey });
    setForm((f) => ({
      ...f,
      rows: f.rows.map((r, i) => ({ ...r, path: paths[i], dataUrl: undefined })),
      // A replacement has no combat file; drop any the form was holding so it is not shown as kept.
      combat: isAdd ? f.combat.map((r, i) => ({ ...r, path: combatPaths[i], dataUrl: undefined })) : [],
    }));
    setOrigKey(audioKey(def));
    setSavedAs(def.id ?? `the replacement for ${def.replace}`);
  };

  const startNew = () => { setForm(blank()); setOrigKey(null); setErrors([]); setSavedAs(''); };
  const editingLabel = origDef ? (origDef.id ?? `replacement for ${origDef.replace}`) : null;
  const playsWhere = floors.values.length || maps.length;

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <div className="flex items-center justify-between gap-3 flex-wrap">
        <ModeSwitch value={form.mode} onChange={(mode) => set({ mode })}
          options={[{ value: 'add', label: 'Add a track' }, { value: 'replace', label: 'Replace a game track' }]} />
        {origKey && (
          <div className="flex items-center gap-2 text-sm" style={{ color: 'var(--color-parchment)' }}>
            <span>Editing <span className="sam-mono">{editingLabel || '(saved track)'}</span> — Save updates it.</span>
            <GoldButton onClick={startNew}>＋ New track</GoldButton>
          </div>
        )}
      </div>

      {isAdd ? (
        <div>
          <TextInput value={form.name} onChange={(name) => set({ name })} placeholder="Track name — e.g. Boss Fight"
            style={{ fontSize: '1.5rem', padding: '0.7rem 1rem' }} aria-label="Track name" />
          <div className="mt-1 text-xs" style={{ color: '#6b5a35' }}>
            id: <span className="sam-mono">{trackId}</span> · play it from a script with{' '}
            <span className="sam-mono">sam_play_music("{trackId}")</span>, make it a monster's theme in the
            Monster Editor, or give it floors or maps below.
          </div>
        </div>
      ) : (
        <Panel title="Which game track?">
          <div className="text-sm mb-2" style={{ color: 'var(--color-parchment)' }}>
            {!replaceValue
              ? 'Pick a track below. Everyone playing with your mod hears yours wherever the game would play it.'
              : isKnownMusicTarget(replaceValue)
                ? <>Replacing <span className="sam-mono" style={{ color: 'var(--color-gold)' }}>{replaceValue}</span>.</>
                : <span style={{ color: '#d4a84b' }}>“{replaceValue}” is not one of the listed names. An exact file name (like mines03) works; anything else is logged and skipped. /sam_music vanilla in the game console lists them all.</span>}
          </div>
          <VanillaMusicPicker value={form.replace} onPick={(name) => set({ replace: name })} />
          <Field label="…or type an exact name" className="mt-3" hint="A theme (shop), an area (mines), its fight track (mines_combat), or one exact file (mines03).">
            <TextInput value={form.replace} onChange={(replace) => set({ replace })} placeholder="mines" />
          </Field>
        </Panel>
      )}

      <Panel title="Audio files">
        <AudioFileList
          rows={form.rows}
          onChange={(rows) => set({ rows })}
          paths={paths}
          assets={assets}
          maxBytes={MAX_MUSIC_BYTES}
          maxLabel="30 MB"
          onErrors={setErrors}
          uploadLabel={form.rows.length ? 'Add another file' : 'Upload audio'}
          emptyText="No audio yet. Upload a track — several files take turns, the way the game rotates an area's tracks."
        />
        <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>
          .ogg keeps a long track small. Tracks this big are not kept if you reload the page, so export or
          use Test in Barony before you close the tab.
        </div>
      </Panel>

      <Panel title="How it plays">
        <Field label="Loop">
          <Select value={form.loop} onChange={(loop) => set({ loop })} options={LOOP_OPTIONS} />
        </Field>
        {isAdd ? (
          <>
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-4 mt-3">
              <Field label="Plays on floors" hint="Floor numbers, e.g. 7, 8 — the same numbers sam_get_floor() gives. Replaces the game's music there.">
                <TextInput value={form.floors} onChange={(floors) => set({ floors })} placeholder="7, 8" />
                {floors.bad.length > 0 && <div className="sam-error text-xs mt-1">not a whole number: {floors.bad.join(', ')}</div>}
              </Field>
              <Field label="Plays on maps" hint="Map names separated by commas — a vanilla one (Minetown) or your own level's.">
                <TextInput value={form.maps} onChange={(maps) => set({ maps })} placeholder="Minetown, My Hub" />
              </Field>
            </div>
            <div className="mt-4">
              <div className="sam-label mb-1">Fight track (optional)</div>
              <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
                Played on those floors or maps while monsters are after you.
                {!playsWhere && form.combat.length > 0 && ' It only does anything once the track has floors or maps.'}
              </div>
              <AudioFileList
                single
                rows={form.combat}
                onChange={(combat) => set({ combat })}
                paths={combatPaths}
                assets={assets}
                maxBytes={MAX_MUSIC_BYTES}
                maxLabel="30 MB"
                onErrors={setErrors}
                uploadLabel={form.combat.length ? 'Replace fight track' : 'Upload fight track'}
                emptyText="None — fights keep the game's own fight music."
              />
            </div>
          </>
        ) : (
          <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>
            A replacement plays wherever the game would play the original, so it has no floors or maps of its
            own. To change an area's fight music, replace its <span className="sam-mono">_combat</span> track
            (e.g. <span className="sam-mono">mines_combat</span>).
          </div>
        )}
      </Panel>

      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> — see Mod Builder.</SavedNote>}
        <GoldButton tone="green" onClick={save}>🎵 {origKey ? 'Save changes' : 'Save track'}</GoldButton>
      </div>

      <Panel title='Live preview — this entry goes into mod.json "music"' bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>{JSON.stringify(def, null, 2)}</pre>
      </Panel>
    </div>
  );
}
