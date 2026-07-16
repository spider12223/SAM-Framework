/*
 * Patch Editor — build an additive layered patch (patch.schema.json). Modifies a
 * vanilla data file (e.g. items/items.json) in place without replacing it; every
 * mod's ops stack in load order. One patch per target file.
 */
import { useEffect, useMemo, useState } from 'react';
import { PATCH_OPS } from '@/data/schemas.js';
import { VANILLA_TARGETS, vanillaTarget, fillPath } from '@/data/vanillaCatalog.js';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';

/**
 * Canonical target spelling. This is a correctness rule, not tidiness: the patcher
 * groups ops by the RAW target string and truncates the overlay on write, so two
 * spellings of one file become two groups and the second clobbers the first. A leading
 * "./" or a backslash is rejected outright by PhysFS and the patch silently does
 * nothing, which looks identical to "my patch didn't work".
 */
function canonicalTarget(t) {
  return t.trim().replace(/\\/g, '/').replace(/^\.\//, '').replace(/^\/+/, '');
}

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
    return { target: canonicalTarget(target), operations };
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

  /** Catalog entry for the current target, or undefined for a hand-typed file. */
  const known = useMemo(() => vanillaTarget(canonicalTarget(target)), [target]);

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <Panel title="Patch Target">
        <Field label="Vanilla file to patch" hint="One patch per target — every mod's ops stack in load order.">
          <select
            className="sam-input"
            value={known ? target : '__custom__'}
            onChange={(e) => { if (e.target.value !== '__custom__') setTarget(e.target.value); }}
          >
            {VANILLA_TARGETS.map((t) => (
              <option key={t.path} value={t.path}>{t.label} — {t.path}</option>
            ))}
            <option value="__custom__">Other file (type it below)…</option>
          </select>
        </Field>
        <Field label="Target path" hint="Forward slashes, no leading slash, exact lowercase.">
          <TextInput value={target} onChange={setTarget} placeholder="items/items.json" />
        </Field>
        {canonicalTarget(target) !== target.trim() && (
          <div className="text-xs mt-1 sam-error">
            Will be saved as <span className="sam-mono">{canonicalTarget(target)}</span> — a leading
            <span className="sam-mono"> ./ </span> or a backslash makes the patch silently do nothing.
          </div>
        )}
        {known ? (
          <>
            {known.whatItControls && (
              <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>{known.whatItControls}</div>
            )}
            {known.entryKeyStyle && (
              <div className="text-xs mt-1" style={{ color: '#6b5a35' }}>
                <b>Entry keys:</b> {known.entryKeyStyle}
              </div>
            )}
            {known.caveat && (
              <div className="text-xs mt-2 sam-error" style={{ whiteSpace: 'pre-wrap' }}>⚠ {known.caveat}</div>
            )}
          </>
        ) : (
          <div className="text-xs mt-2" style={{ color: '#e0b46a' }}>
            Not in the catalog — you're on your own for paths. It'll still work if Barony really
            loads this file; if it doesn't, the patch is skipped with an error in{' '}
            <span className="sam-mono">sam_log.txt</span> and the game runs normally.
          </div>
        )}
      </Panel>

      <Panel title="Operations">
        <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
          Path is dot-separated into the file, e.g. <span className="sam-mono">items.iron_sword.gold_value</span>.
          Value accepts JSON (<span className="sam-mono">150</span>, <span className="sam-mono">true</span>, <span className="sam-mono">"text"</span>); a bare word is treated as a string. <span className="sam-mono">multiply_field</span> needs a number; <span className="sam-mono">remove_field</span> takes no value.
        </div>
        <div className="space-y-3">
          {ops.map((o, i) => {
            const fld = known?.fields.find((f) => fillPath(f.path, o.entry || '<x>') === o.path)
              ?? known?.fields.find((f) => f.path === o.pickedField);
            const allowed = fld ? fld.ops : PATCH_OPS;
            const needsEntry = !!fld && /<[^>]+>/.test(fld.path);
            return (
              <div key={i} className="sam-well p-2 space-y-2">
                {known && (
                  <div className="grid grid-cols-[1fr_11rem] gap-2">
                    <select
                      className="sam-input"
                      value={o.pickedField ?? ''}
                      onChange={(e) => {
                        const f = known.fields.find((x) => x.path === e.target.value);
                        if (!f) { setOp(i, { pickedField: '' }); return; }
                        const nextOp = f.ops.includes(o.op) ? o.op : (f.ops[0] ?? 'edit_field');
                        setOp(i, { pickedField: f.path, path: fillPath(f.path, o.entry), op: nextOp });
                      }}
                    >
                      <option value="">— pick a field, or type a path below —</option>
                      {known.fields.map((f) => (
                        <option key={f.path} value={f.path} disabled={f.readOnly}>
                          {f.label} ({f.type}){f.readOnly ? ' — read-only' : ''}
                        </option>
                      ))}
                    </select>
                    <TextInput
                      value={o.entry ?? ''}
                      onChange={(v) => {
                        const f = known.fields.find((x) => x.path === o.pickedField);
                        setOp(i, { entry: v, path: f ? fillPath(f.path, v) : o.path });
                      }}
                      placeholder={needsEntry ? (known.exampleEntries?.[0] ?? 'entry') : '(no entry)'}
                      style={needsEntry ? undefined : { opacity: 0.4, pointerEvents: 'none' }}
                    />
                  </div>
                )}
                <div className="grid grid-cols-[9rem_1fr_9rem_auto] gap-2 items-center">
                  <select className="sam-input" value={o.op} onChange={(e) => setOp(i, { op: e.target.value })}>
                    {PATCH_OPS.map((op) => (
                      <option key={op} value={op} disabled={!allowed.includes(op)}>{op}</option>
                    ))}
                  </select>
                  <TextInput value={o.path} onChange={(v) => setOp(i, { path: v })} placeholder="items.iron_sword.gold_value" />
                  <TextInput
                    value={o.valueText}
                    onChange={(v) => setOp(i, { valueText: v })}
                    placeholder={o.op === 'multiply_field' ? '0.5' : o.op === 'remove_field' ? '(none)' : (fld?.example ?? '150')}
                    style={o.op === 'remove_field' ? { opacity: 0.4, pointerEvents: 'none' } : undefined}
                  />
                  <button type="button" className="sam-step sam-remove" style={{ width: 26, height: 26 }} onClick={() => removeOp(i)} aria-label="remove op">✕</button>
                </div>
                {fld?.note && <div className="text-xs" style={{ color: '#6b5a35' }}>{fld.note}</div>}
                {fld?.tooltipOnly && (
                  <div className="text-xs sam-error">
                    ⚠ On a vanilla item this only changes the tooltip — real combat is hardcoded per
                    item id and won't read it. (On your own custom items it does apply.)
                  </div>
                )}
                {fld?.arrayBacked && (
                  <div className="text-xs" style={{ color: '#e0b46a' }}>
                    This path indexes an array — only <span className="sam-mono">edit_field</span> and{' '}
                    <span className="sam-mono">multiply_field</span> work, and indices shift if another
                    mod reorders it.
                  </div>
                )}
                {known?.noAddEntry && o.op === 'add_entry' && (
                  <div className="text-xs sam-error">
                    ⚠ <span className="sam-mono">add_entry</span> on this file desyncs every item id
                    after it — Barony maps entry order to item ids. Add items as a custom item instead.
                  </div>
                )}
              </div>
            );
          })}
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
