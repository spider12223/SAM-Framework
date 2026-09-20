/*
 * Sound Editor — add a sound effect to the game, or replace one of the game's own.
 *
 * Each sound becomes one object in mod.json "sounds" (sound.schema.json):
 *   add:     { "id": "mymod:boom", "file": "sounds/boom.ogg" }
 *   replace: { "replace": "SwingWeapon", "file": ["sounds/swingweapon.ogg", ...], "volume": 0.8 }
 * Several files = one is picked at random each time it plays. The audio is bundled as mod
 * assets under sounds/.
 *
 * Editing updates the entry IN PLACE. It used to read a `name` field that was never saved (so
 * the name came up blank), and saving under a different name added a second sound while the
 * old one stayed: the editor now remembers which entry it opened (origKey) and replaces it.
 */
import { useEffect, useMemo, useState } from 'react';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, NumberInput, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';
import { ModeSwitch, AudioFileList, VanillaSoundPicker, newRowKey } from '@/components/AudioParts.jsx';
import {
  slugify, audioKey, audioFiles, fileField, assignAudioPaths, planAudioRows, describeSoundTarget, MAX_SOUND_BYTES,
} from '@/lib/audio.js';

const blank = () => ({ mode: 'add', name: '', replace: '', rows: [], volume: 1, loop: false });

function stateFrom(def) {
  if (!def) return blank();
  return {
    mode: def.replace !== undefined && !def.id ? 'replace' : 'add',
    name: def.id ? String(def.id).split(':').pop() : '',
    replace: def.replace !== undefined ? String(def.replace) : '',
    rows: audioFiles(def).map((path) => ({ key: newRowKey(), path })),
    volume: def.volume ?? 1,
    loop: !!def.loop,
  };
}

export default function SoundEditor() {
  const { meta, sounds, music, assets, editing, dispatch } = useMod();
  const editDef = editing?.kind === 'sound' ? sounds.find((s) => audioKey(s) === editing.id) : null;

  // Which saved entry this form is editing (null = a new one). Survives renames, so a second
  // save keeps updating the same sound instead of adding another.
  const [origKey, setOrigKey] = useState(editDef ? audioKey(editDef) : null);
  const [form, setForm] = useState(() => stateFrom(editDef));
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');
  const set = (patch) => { setForm((f) => ({ ...f, ...patch })); setSavedAs(''); };

  useEffect(() => {
    if (editing?.kind === 'sound') dispatch({ type: 'clearEditing' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const namespace = meta.namespace || 'mymod';
  const slug = slugify(form.name);
  const soundId = `${namespace}:${slug}`;
  // An all-digit target is an index; the schema takes it as a number.
  const replaceValue = /^\d+$/.test(form.replace.trim()) ? Number(form.replace.trim()) : form.replace.trim();
  const fileSlug = form.mode === 'add' ? slug : slugify(String(replaceValue || 'replacement'));

  const origDef = origKey ? sounds.find((s) => audioKey(s) === origKey) : null;
  const oldSlug = origDef ? slugify(origDef.id ? String(origDef.id).split(':').pop() : String(origDef.replace)) : '';
  const otherUse = useMemo(
    () => [...sounds.filter((s) => audioKey(s) !== origKey), ...(music ?? [])].flatMap(audioFiles),
    [sounds, music, origKey],
  );
  // replaceTarget is what this entry replaces NOW. A file the author dropped in
  // sounds/replace/ is named after the sound it replaces and the engine reads that name on
  // its own, so retargeting the entry has to move the file out -- otherwise the old sound
  // goes on being replaced as well as the new one.
  const plan = useMemo(
    () => planAudioRows(form.rows, {
      folder: 'sounds', oldSlug, newSlug: fileSlug, assets, otherUse,
      replaceTarget: form.mode === 'replace' ? replaceValue : null,
    }),
    [form.rows, form.mode, replaceValue, oldSlug, fileSlug, assets, otherUse],
  );
  const paths = useMemo(() => assignAudioPaths({
    files: plan, folder: 'sounds', slug: fileSlug, assets, ownOld: audioFiles(origDef), otherUse,
  }), [plan, fileSlug, assets, origDef, otherUse]);

  const volumeNum = form.volume === '' ? 1 : Number(form.volume);
  const def = useMemo(() => {
    const d = form.mode === 'add' ? { id: soundId } : { replace: replaceValue };
    if (paths.length) d.file = fileField(paths);
    if (volumeNum !== 1) d.volume = volumeNum;
    if (form.loop) d.loop = true;
    return d;
  }, [form.mode, form.loop, soundId, replaceValue, paths, volumeNum]);

  const target = form.mode === 'replace' ? describeSoundTarget(replaceValue) : null;

  const save = () => {
    setSavedAs('');
    const errs = [];
    if (form.mode === 'add' && !form.name.trim()) errs.push({ path: 'name', message: 'Give the sound a name — scripts play it by that name.' });
    if (form.mode === 'replace' && replaceValue === '') errs.push({ path: 'replace', message: 'Pick the game sound to replace.' });
    if (!form.rows.length) errs.push({ path: 'file', message: 'Upload at least one audio file.' });
    const clash = sounds.find((s) => audioKey(s) === audioKey(def) && audioKey(s) !== origKey);
    if (clash) {
      errs.push(form.mode === 'add'
        ? { path: 'id', message: `There is already a sound called ${def.id}. Pick another name, or open that one from the Mod Builder.` }
        : { path: 'replace', message: `This mod already replaces ${clash.replace}. Open that entry from the Mod Builder instead.` });
    }
    if (!errs.length) {
      const res = validate('sound', def);
      if (!res.valid) errs.push(...res.errors);
    }
    if (errs.length) { setErrors(errs); return; }
    setErrors([]);
    // New uploads, and files carried over to the new name (the reducer then drops the old copy).
    plan.forEach((p, i) => {
      if (p.dataUrl) dispatch({ type: 'setAsset', path: paths[i], dataUrl: p.dataUrl });
      else if (p.from && p.from !== paths[i]) dispatch({ type: 'setAsset', path: paths[i], dataUrl: assets[p.from] });
    });
    dispatch({ type: 'saveSound', def, prevKey: origKey });
    // The uploads are assets now; keep the File for the preview, drop the big string.
    setForm((f) => ({ ...f, rows: f.rows.map((r, i) => ({ ...r, path: paths[i], dataUrl: undefined })) }));
    setOrigKey(audioKey(def));
    setSavedAs(def.id ?? `the replacement for ${def.replace}`);
  };

  const startNew = () => { setForm(blank()); setOrigKey(null); setErrors([]); setSavedAs(''); };
  const editingLabel = origDef ? (origDef.id ?? `replacement for ${origDef.replace}`) : null;

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <div className="flex items-center justify-between gap-3 flex-wrap">
        <ModeSwitch value={form.mode} onChange={(mode) => set({ mode })}
          options={[{ value: 'add', label: 'Add a new sound' }, { value: 'replace', label: 'Replace a game sound' }]} />
        {origKey && (
          <div className="flex items-center gap-2 text-sm" style={{ color: 'var(--color-parchment)' }}>
            <span>Editing <span className="sam-mono">{editingLabel || '(saved sound)'}</span> — Save updates it.</span>
            <GoldButton onClick={startNew}>＋ New sound</GoldButton>
          </div>
        )}
      </div>

      {form.mode === 'add' ? (
        <div>
          <TextInput value={form.name} onChange={(name) => set({ name })} placeholder="Sound name — e.g. Boom"
            style={{ fontSize: '1.5rem', padding: '0.7rem 1rem' }} aria-label="Sound name" />
          <div className="mt-1 text-xs" style={{ color: '#6b5a35' }}>
            id: <span className="sam-mono">{soundId}</span> · play it from a script with{' '}
            <span className="sam-mono">sam_play_sound("{soundId}")</span>, test it in game with{' '}
            <span className="sam-mono">/sam_playsound {soundId}</span>, or give it to a monster or item in their editors.
          </div>
        </div>
      ) : (
        <Panel title="Which game sound?">
          <div className="text-sm mb-2" style={{ color: 'var(--color-parchment)' }}>
            {replaceValue === ''
              ? 'Pick a sound below. Everyone playing with your mod hears yours instead — no script needed.'
              : target
                ? <>Replacing <span className="sam-mono" style={{ color: 'var(--color-gold)' }}>{String(replaceValue)}</span> — {target.label}.</>
                : <span style={{ color: '#d4a84b' }}>⚠ The game has no sound or group called “{String(replaceValue)}”. It would log that and skip this entry.</span>}
          </div>
          <VanillaSoundPicker value={form.replace} onPick={(name) => set({ replace: name })} />
          <Field label="…or type the exact name or index" className="mt-3" hint="Exact file names (SwingWeapon3V1), group names (SwingWeapon) and index numbers all work. /sam_sounds vanilla <text> in the game console searches the same list.">
            <TextInput value={form.replace} onChange={(replace) => set({ replace })} placeholder="SwingWeapon" />
          </Field>
        </Panel>
      )}

      <Panel title="Audio files">
        <AudioFileList
          rows={form.rows}
          onChange={(rows) => set({ rows })}
          paths={paths}
          assets={assets}
          maxBytes={MAX_SOUND_BYTES}
          maxLabel="2 MB"
          onErrors={setErrors}
          uploadLabel={form.rows.length ? 'Add another file' : 'Upload audio'}
          emptyText="No audio yet. Upload a file — several files make it pick one at random each time, the way the game varies its footsteps and swings."
        />
        {form.rows.length > 1 && (
          <div className="text-xs mt-2" style={{ color: '#9dc76a' }}>✓ {form.rows.length} files — one is picked at random each time it plays.</div>
        )}
      </Panel>

      <Panel title="How it plays">
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-4 items-start">
          <Field label="Volume" hint="1 = as loud as the game plays it, 0.5 = half. Above 1 lifts a quiet use toward the game's maximum, not past it. 0 to 4.">
            <NumberInput value={form.volume} min={0} max={4} step={0.1} onChange={(volume) => set({ volume })} />
          </Field>
          <label className="flex items-center gap-2 mt-6 cursor-pointer text-sm" style={{ color: 'var(--color-parchment)' }}>
            <input type="checkbox" className="sam-check" checked={form.loop} onChange={(e) => set({ loop: e.target.checked })} />
            <span>Loop until stopped (ambience — stop it with <span className="sam-mono">sam_stop_sound</span>). Most effects leave this off.</span>
          </label>
        </div>
      </Panel>

      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> — see Mod Builder.</SavedNote>}
        <GoldButton tone="green" onClick={save}>🔊 {origKey ? 'Save changes' : 'Save sound'}</GoldButton>
      </div>

      <Panel title='Live preview — this entry goes into mod.json "sounds"' bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>{JSON.stringify(def, null, 2)}</pre>
      </Panel>
    </div>
  );
}
