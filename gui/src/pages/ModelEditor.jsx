/*
 * Model Editor — declare a custom .vox and BUNDLE IT.
 *
 * The builder could already declare a model on a class or an item, but there was no way to
 * put the actual file in the exported mod: you saved a path, the zip did not contain it, and
 * the model silently never loaded. This is the missing half.
 *
 * It also validates the file the moment you pick it. Shipping a MagicaVoxel .vox instead of a
 * Barony slab is the single most common way a custom model fails, and catching it in the
 * browser turns "my model does not show up" into a sentence you read before you ever launch.
 *
 * The models list lives on meta.models as [{ id, file }] — exportZip already writes that into
 * mod.json, and the file itself goes into `assets`, which the zip writer emits as binary.
 */
import { useRef, useState } from 'react';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';
import { inspectSlabVox, describeSlab } from '@/lib/voxSlab.js';

// Kept well under the browser's ~5 MB per-origin localStorage quota, which the whole mod
// (portraits, sounds, every other model) shares. Assets are stored as base64, which inflates
// by 4/3 before JSON escaping, so a "4 MB" model really costs ~5.6 MB and on its own exceeds
// the store -- at which point the persist fallback drops EVERY bundled asset in the mod.
const MAX_MODEL_BYTES = 1024 * 1024;

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

export default function ModelEditor() {
  const { meta, assets, dispatch } = useMod();
  const models = Array.isArray(meta.models) ? meta.models : [];

  const [name, setName] = useState('');
  const [pending, setPending] = useState(null); // { dataUrl, info, fileName }
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');
  const fileRef = useRef(null);

  const namespace = meta.namespace || 'mymod';
  const slug = slugify(name);
  const modelId = `${namespace}:${slug}`;
  const modelPath = `models/${namespace}/${slug}.vox`;

  const onFile = (e) => {
    const f = e.target.files?.[0];
    e.target.value = '';
    setSavedAs('');
    if (!f) return;
    if (!/\.vox$/i.test(f.name)) {
      setErrors([{ path: 'file', message: 'Pick a .vox file.' }]);
      return;
    }
    if (f.size > MAX_MODEL_BYTES) {
      setErrors([{ path: 'file', message: `That file is ${Math.round(f.size / 1024)} KB. Models are bundled in browser storage, which the whole mod shares, so keep each one under ${MAX_MODEL_BYTES / 1024} KB. A Barony slab this large is unusual — check the dimensions.` }]);
      return;
    }
    const buf = new FileReader();
    buf.onload = () => {
      // Validate the BYTES, not the extension. Both formats are called .vox.
      const info = inspectSlabVox(buf.result);
      if (!info.ok) {
        setPending(null);
        setErrors([{ path: 'file', message: info.reason }]);
        return;
      }
      const url = new FileReader();
      url.onload = () => {
        setErrors([]);
        setPending({ dataUrl: String(url.result ?? ''), info, fileName: f.name });
        setName((prev) => prev.trim() || f.name.replace(/\.vox$/i, ''));
      };
      url.readAsDataURL(f);
    };
    buf.readAsArrayBuffer(f);
  };

  const save = () => {
    setSavedAs('');
    if (!name.trim()) { setErrors([{ path: 'name', message: 'Give the model a name.' }]); return; }
    if (!pending && !assets[modelPath]) {
      setErrors([{ path: 'file', message: 'Upload the .vox itself, or the exported mod will declare a model it does not contain.' }]);
      return;
    }
    if (models.some((m) => m.id === modelId)) {
      setErrors([{ path: 'name', message: `A model called "${modelId}" is already declared. Pick another name, or remove that one first.` }]);
      return;
    }
    setErrors([]);
    if (pending) dispatch({ type: 'setAsset', path: modelPath, dataUrl: pending.dataUrl });
    // The reducer spreads action.patch; sending `meta` here was a silent no-op.
    dispatch({ type: 'setMeta', patch: { models: [...models, { id: modelId, file: modelPath }] } });
    setSavedAs(modelId);
    setPending(null);
    setName('');
  };

  const remove = (m) => {
    const left = models.filter((x) => x.id !== m.id);
    dispatch({ type: 'setMeta', patch: { models: left } });
    // Two declarations may legitimately share one .vox, so only drop the file when nothing
    // else still points at it.
    if (assets[m.file] && !left.some((x) => x.file === m.file)) {
      dispatch({ type: 'removeAsset', path: m.file });
    }
  };

  return (
    <div className="space-y-4">
      <Panel title="Add a model">
        <p className="text-sm opacity-80 mb-3">
          Barony reads <strong>slab</strong> <code>.vox</code>. MagicaVoxel's own export is a
          different format that happens to use the same extension — build in whatever you like,
          but convert before shipping. This page checks the file for you.
        </p>

        <Field label="Name" hint={name.trim() ? `Declared as ${modelId}, bundled at ${modelPath}` : 'Used for the model id and the file name.'}>
          <TextInput value={name} onChange={setName} placeholder="wyvern" />
        </Field>

        <Field label="The .vox file" hint="Bundled into the exported mod. Without it, the mod declares a model it does not contain.">
          <div className="flex items-center gap-3">
            <input ref={fileRef} type="file" accept=".vox" className="hidden" onChange={onFile} />
            <GoldButton onClick={() => fileRef.current?.click()}>Choose .vox…</GoldButton>
            {pending && (
              <span className="text-sm">
                <strong>{pending.fileName}</strong>
                <span className="opacity-70"> — {describeSlab(pending.info)}</span>
              </span>
            )}
            {!pending && assets[modelPath] && <span className="text-sm opacity-70">already bundled</span>}
          </div>
        </Field>

        <ErrorList errors={errors} />
        <div className="mt-3 flex items-center gap-3">
          <GoldButton onClick={save}>Add model</GoldButton>
          {savedAs && <SavedNote>Added {savedAs}</SavedNote>}
        </div>
      </Panel>

      <Panel title={`Declared models (${models.length})`}>
        {models.length === 0 && (
          <p className="text-sm opacity-70">
            None yet. A declared model can be used as a class <code>body_model</code>, as a
            monster <code>body.model</code>, or spawned from a script with
            <code> sam_spawn_companion</code>.
          </p>
        )}
        {models.map((m) => {
          const bundled = Boolean(assets[m.file]);
          return (
            <div key={m.id} className="flex items-center justify-between py-2 border-b border-white/10 last:border-0">
              <div className="min-w-0">
                <div className="font-mono text-sm truncate">{m.id}</div>
                <div className="text-xs opacity-70 truncate">{m.file}</div>
              </div>
              <div className="flex items-center gap-3 shrink-0">
                {/* A declaration with no file is the exact failure this page exists to stop,
                    so say so plainly rather than letting it export quietly. */}
                <span className={`text-xs ${bundled ? 'opacity-70' : 'text-red-400'}`}>
                  {bundled ? 'bundled' : 'FILE MISSING — will not load'}
                </span>
                <GoldButton tone="red" onClick={() => remove(m)}>Remove</GoldButton>
              </div>
            </div>
          );
        })}
      </Panel>
    </div>
  );
}
