/*
 * Sound Editor — register a custom sound (sound.schema.json). The .ogg/.wav is bundled
 * as a mod asset; S.A.M FMOD-loads it and appends it onto the engine sound table so it
 * plays by its "namespace:sound" id via sam_play_sound (or the /sam_playsound test cmd).
 */
import { useEffect, useMemo, useRef, useState } from 'react';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';

const MAX_SOUND_BYTES = 2 * 1024 * 1024; // 2 MB — keep sound effects small

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

export default function SoundEditor() {
  const { meta, sounds, assets, editing, dispatch } = useMod();
  const editDef = editing?.kind === 'sound' ? sounds.find((s) => s.id === editing.id) : null;

  const [name, setName] = useState(editDef?.name ?? '');
  const [file, setFile] = useState(editDef?.file ?? '');
  const [loop, setLoop] = useState(editDef?.loop ?? false);
  const [pendingDataUrl, setPendingDataUrl] = useState('');
  const [uploadName, setUploadName] = useState('');
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');
  const fileRef = useRef(null);

  useEffect(() => {
    if (editing?.kind === 'sound') dispatch({ type: 'clearEditing' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const namespace = meta.namespace || 'mymod';
  const slug = slugify(name);
  const soundId = `${namespace}:${slug}`;
  const defaultPath = `sounds/${slug}.ogg`;
  const effectivePath = (file.trim() || (name.trim() ? defaultPath : ''));

  const onFile = (e) => {
    const f = e.target.files?.[0];
    e.target.value = '';
    if (!f) return;
    if (!/\.(ogg|wav)$/i.test(f.name)) { setErrors([{ path: 'file', message: 'Pick a .ogg or .wav file.' }]); return; }
    if (f.size > MAX_SOUND_BYTES) { setErrors([{ path: 'file', message: `That file is ${Math.round(f.size / 1024)} KB — keep sound effects under 2 MB.` }]); return; }
    const ext = f.name.split('.').pop().toLowerCase();
    const reader = new FileReader();
    reader.onload = () => {
      setErrors([]);
      setPendingDataUrl(String(reader.result ?? ''));
      setUploadName(f.name);
      // default the path to sounds/<slug>.<ext> if the user hasn't typed one
      setFile((prev) => prev.trim() || `sounds/${slug}.${ext}`);
    };
    reader.readAsDataURL(f);
  };

  const buildDef = () => {
    const def = { id: soundId, file: effectivePath };
    if (loop) def.loop = true;
    return def;
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const res = validate('sound', def);
    if (!res.valid) { setErrors(res.errors); return; }
    // require the .ogg to be present (uploaded now, or already an asset from a prior edit/import)
    if (!pendingDataUrl && !assets[def.file]) {
      setErrors([{ path: 'file', message: `No audio for "${def.file}". Upload the .ogg/.wav, or make sure it's already bundled.` }]);
      return;
    }
    setErrors([]);
    if (pendingDataUrl) dispatch({ type: 'setAsset', path: def.file, dataUrl: pendingDataUrl });
    dispatch({ type: 'saveSound', def });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [name, file, loop, namespace]);
  const preview = useMemo(() => JSON.stringify(def, null, 2), [def]);
  const haveAudio = !!pendingDataUrl || !!assets[effectivePath];

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <div>
        <TextInput value={name} onChange={setName} placeholder="Sound name — e.g. Boom"
          style={{ fontSize: '1.5rem', padding: '0.7rem 1rem' }} aria-label="Sound name" />
        <div className="mt-1 text-xs" style={{ color: '#6b5a35' }}>
          id: <span className="sam-mono">{soundId}</span> · play with{' '}
          <span className="sam-mono">sam_play_sound("{soundId}")</span> or{' '}
          <span className="sam-mono">/sam_playsound {soundId}</span>
        </div>
      </div>

      <Panel title="Audio file">
        <Field label="File (.ogg or .wav)" hint="Mod-relative path. Upload a file to bundle it, or type a path to one already in your mod folder.">
          <TextInput value={file} onChange={setFile} placeholder={defaultPath} />
        </Field>
        <div className="flex items-center gap-3 mt-2">
          <input ref={fileRef} type="file" accept=".ogg,.wav,audio/ogg,audio/wav" className="hidden" onChange={onFile} />
          <GoldButton onClick={() => fileRef.current?.click()}>🎵 Upload .ogg / .wav</GoldButton>
          {uploadName && <span className="text-sm sam-mono" style={{ color: 'var(--color-parchment)' }}>{uploadName} loaded → {effectivePath}</span>}
          {!uploadName && assets[effectivePath] && <span className="text-sm" style={{ color: '#6b8a4a' }}>✓ bundled: {effectivePath}</span>}
        </div>
        <label className="flex items-center gap-2 mt-4 cursor-pointer text-sm" style={{ color: 'var(--color-parchment)' }}>
          <input type="checkbox" className="sam-check" checked={loop} onChange={(e) => setLoop(e.target.checked)} />
          Loop (for ambience — most sound effects leave this off)
        </label>
      </Panel>

      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> — see Mod Builder.</SavedNote>}
        {!haveAudio && name.trim() && <span className="text-xs" style={{ color: '#8a6d2e' }}>upload the audio to save</span>}
        <GoldButton tone="green" onClick={save} disabled={!name.trim()}>🔊 Save Sound</GoldButton>
      </div>

      <Panel title="Live JSON Preview" bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>{preview}</pre>
      </Panel>
    </div>
  );
}
