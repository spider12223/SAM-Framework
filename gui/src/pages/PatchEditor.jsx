/*
 * Patch Editor — build an additive layered patch (patch.schema.json). Modifies a
 * vanilla data file (e.g. items/items.json) in place without replacing it; every
 * mod's ops stack in load order. One patch per target file.
 */
import { useEffect, useMemo, useState } from 'react';
import { PATCH_OPS } from '@/data/schemas.js';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';

const COMMON_TARGETS = [
  'items/items.json', 'data/monstercurve.json', 'data/gameplaymodifiers.json',
  'data/shopkeeper.json', 'items/items_global_inventory.json',
];

/** Store each op's value as the exact JSON text so it round-trips unambiguously. */
function valueToText(v) {
  return v === undefined ? '' : JSON.stringify(v);
}
/** Parse the value cell: proper JSON wins; a bare word is taken as a string. */
function textToValue(text) {
  const t = text.trim();
  if (t === '') return undefined;
  try { return JSON.parse(t); } catch { return t; }
}

export default function PatchEditor() {
  const { patches, editing, dispatch } = useMod();
  const editDef = editing?.kind === 'patch' ? patches.find((p) => p.target === editing.id) : null;

  const [target, setTarget] = useState(editDef?.target ?? 'items/items.json');
  const [ops, setOps] = useState(() =>
    (editDef?.operations ?? [{ op: 'edit_field', path: '', valueText: '' }]).map((o) =>
      o.valueText !== undefined ? o : { op: o.op, path: o.path, valueText: valueToText(o.value) }
    )
  );
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');

  useEffect(() => {
    if (editing?.kind === 'patch') dispatch({ type: 'clearEditing' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const setOp = (i, patch) => setOps((prev) => prev.map((o, j) => (j === i ? { ...o, ...patch } : o)));
  const addOp = () => setOps((prev) => [...prev, { op: 'edit_field', path: '', valueText: '' }]);
  const removeOp = (i) => setOps((prev) => prev.filter((_, j) => j !== i));

  const buildDef = () => {
    const operations = ops
      .filter((o) => o.path.trim())
      .map((o) => {
        const entry = { op: o.op, path: o.path.trim() };
        if (o.op !== 'remove_field') {
          let v = textToValue(o.valueText);
          if (o.op === 'multiply_field') v = Number(v); // must be numeric
          entry.value = v;
        }
        return entry;
      });
    return { target: target.trim(), operations };
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const res = validate('patch', def);
    if (!res.valid) { setErrors(res.errors); return; }
    setErrors([]);
    dispatch({ type: 'savePatch', def });
    setSavedAs(def.target);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [target, ops]);
  const preview = useMemo(() => JSON.stringify(def, null, 2), [def]);

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <Panel title="Patch Target">
        <Field label="Target file" hint="A file Barony loads, e.g. items/items.json. One patch per target — ops stack.">
          <input className="sam-input" list="patch-targets" value={target} onChange={(e) => setTarget(e.target.value)} placeholder="items/items.json" />
          <datalist id="patch-targets">{COMMON_TARGETS.map((t) => <option key={t} value={t} />)}</datalist>
        </Field>
      </Panel>

      <Panel title="Operations">
        <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
          Path is dot-separated into the file, e.g. <span className="sam-mono">items.iron_sword.gold_value</span>.
          Value accepts JSON (<span className="sam-mono">150</span>, <span className="sam-mono">true</span>, <span className="sam-mono">"text"</span>); a bare word is treated as a string. <span className="sam-mono">multiply_field</span> needs a number; <span className="sam-mono">remove_field</span> takes no value.
        </div>
        <div className="space-y-2">
          {ops.map((o, i) => (
            <div key={i} className="grid grid-cols-[9rem_1fr_9rem_auto] gap-2 items-center">
              <select className="sam-input" value={o.op} onChange={(e) => setOp(i, { op: e.target.value })}>
                {PATCH_OPS.map((op) => <option key={op} value={op}>{op}</option>)}
              </select>
              <TextInput value={o.path} onChange={(v) => setOp(i, { path: v })} placeholder="items.iron_sword.gold_value" />
              <TextInput
                value={o.valueText}
                onChange={(v) => setOp(i, { valueText: v })}
                placeholder={o.op === 'multiply_field' ? '0.5' : o.op === 'remove_field' ? '(none)' : '150'}
                style={o.op === 'remove_field' ? { opacity: 0.4, pointerEvents: 'none' } : undefined}
              />
              <button type="button" className="sam-step sam-remove" style={{ width: 26, height: 26 }} onClick={() => removeOp(i)} aria-label="remove op">✕</button>
            </div>
          ))}
        </div>
        <div className="mt-3"><GoldButton onClick={addOp}>+ Add operation</GoldButton></div>
      </Panel>

      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && <SavedNote>Saved patch for <span className="sam-mono">{savedAs}</span> — see Mod Builder.</SavedNote>}
        <GoldButton tone="red" onClick={save} disabled={!target.trim()}>🧩 Save Patch</GoldButton>
      </div>

      <Panel title="Live JSON Preview" bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>{preview}</pre>
      </Panel>
    </div>
  );
}
