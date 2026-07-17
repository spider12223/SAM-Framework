/*
 * Make your own lego bricks, and share them as a pack.
 *
 * A brick is: a name, condition-or-action, some parameters, and a Lua template where
 * {param} gets substituted. Saved to localStorage; exported/imported as JSON so you can
 * hand a friend a set of bricks to build with.
 *
 * Every template is linted against the real API before it can be saved (see
 * lib/customBlocks.js). That's deliberate: the builder's whole value is that it can't
 * emit a function that doesn't exist, and a free-text template is the one place that
 * guarantee could leak. A brick that calls sam_bogus() is rejected here rather than
 * silently doing nothing in someone's game.
 */
import { useState } from 'react';
import { Panel, Field, TextInput, Select, GoldButton, ErrorList } from '@/components/ui.jsx';
import { validateBlock, renderTemplate, exportPack, importPack, PARAM_TYPES } from '@/lib/customBlocks.js';

const blank = () => ({
  id: Math.random().toString(36).slice(2, 9),
  label: '', kind: 'action', note: '',
  params: [{ name: 'x', type: 'text', default: '', label: '', values: '' }],
  lua: 'sam_grant_item(player, "{x}")',
});

export default function CustomBlockEditor({ blocks, onChange, onClose }) {
  const [draft, setDraft] = useState(blank());
  const [errors, setErrors] = useState([]);
  const [io, setIo] = useState('');

  const preview = renderTemplate(
    draft.lua,
    Object.fromEntries((draft.params || []).map((p) => [p.name, p.default || `<${p.name}>`])),
  );

  const save = () => {
    const v = validateBlock(draft);
    setErrors(v.errors);
    if (!v.ok) return;
    const next = blocks.some((b) => b.id === draft.id)
      ? blocks.map((b) => (b.id === draft.id ? draft : b))
      : [...blocks, draft];
    onChange(next);
    setDraft(blank());
  };

  const patchParam = (i, key, val) => setDraft((d) => {
    const params = [...d.params]; params[i] = { ...params[i], [key]: val };
    return { ...d, params };
  });

  const doImport = () => {
    const r = importPack(io);
    setErrors(r.errors);
    if (r.blocks.length) {
      const byId = new Map(blocks.map((b) => [b.id, b]));
      for (const b of r.blocks) byId.set(b.id, b);
      onChange([...byId.values()]);
      setIo('');
    }
  };

  return (
    <Panel title="🧱 Your own blocks — “lego pack”">
      <div className="text-xs mb-3" style={{ color: '#8a7a4a' }}>
        Build a brick once, reuse it forever, hand the pack to a friend. Use <span className="sam-mono">{'{name}'}</span> in
        the Lua for each parameter, and <span className="sam-mono">player</span> for whoever triggered it.
        Every template is checked against the real API before it saves.
      </div>

      <div className="grid gap-2" style={{ gridTemplateColumns: '1fr 10rem' }}>
        <Field label="Block name"><TextInput value={draft.label} onChange={(v) => setDraft({ ...draft, label: v })} placeholder="Take X, give Y" /></Field>
        <Field label="Kind"><Select value={draft.kind} onChange={(v) => setDraft({ ...draft, kind: v })}
          options={[{ value: 'action', label: 'Action (does)' }, { value: 'condition', label: 'Condition (tests)' }]} /></Field>
      </div>

      <div className="sam-label mt-3 mb-1">Parameters</div>
      {draft.params.map((p, i) => (
        <div key={i} className="flex items-end gap-2 mb-2 flex-wrap">
          <Field label="name" className="w-28"><TextInput value={p.name} onChange={(v) => patchParam(i, 'name', v)} /></Field>
          <Field label="type" className="w-28"><Select value={p.type} onChange={(v) => patchParam(i, 'type', v)} options={PARAM_TYPES} /></Field>
          <Field label="default" className="w-32"><TextInput value={p.default} onChange={(v) => patchParam(i, 'default', v)} /></Field>
          {p.type === 'select' && (
            <Field label="choices (comma-separated)" className="flex-1 min-w-[10rem]">
              <TextInput value={p.values} onChange={(v) => patchParam(i, 'values', v)} placeholder="HP, MP, GOLD" />
            </Field>
          )}
          <GoldButton tone="red" className="mb-[2px]"
            onClick={() => setDraft({ ...draft, params: draft.params.filter((_, j) => j !== i) })}>✕</GoldButton>
        </div>
      ))}
      <GoldButton onClick={() => setDraft({ ...draft, params: [...draft.params, { name: '', type: 'text', default: '', values: '' }] })}>
        + Add parameter
      </GoldButton>

      <Field label="Lua template" className="mt-3" hint="One expression for a condition, one statement for an action.">
        <textarea className="sam-input sam-mono" rows={3} spellCheck={false}
          value={draft.lua} onChange={(e) => setDraft({ ...draft, lua: e.target.value })} />
      </Field>

      <div className="sam-label mt-3 mb-1">Preview</div>
      <pre className="sam-well p-2 text-xs overflow-x-auto" style={{ color: '#e8d5a3', whiteSpace: 'pre' }}>{preview}</pre>

      {errors.length > 0 && <div className="mt-2"><ErrorList errors={errors} /></div>}

      <div className="flex gap-2 mt-3">
        <GoldButton onClick={save}>Save block</GoldButton>
        <GoldButton onClick={() => { setDraft(blank()); setErrors([]); }}>Clear</GoldButton>
        <GoldButton onClick={onClose} className="ml-auto">Done</GoldButton>
      </div>

      {blocks.length > 0 && (
        <>
          <div className="sam-label mt-4 mb-1">Your blocks ({blocks.length})</div>
          {blocks.map((b) => (
            <div key={b.id} className="sam-well p-2 mb-1 flex items-center gap-2">
              <span style={{ color: 'var(--color-gold-bright)', fontSize: '0.8rem' }}>{b.label}</span>
              <span className="text-xs" style={{ color: '#6b5a35' }}>{b.kind}</span>
              <GoldButton className="ml-auto" onClick={() => setDraft(b)}>Edit</GoldButton>
              <GoldButton tone="red" onClick={() => onChange(blocks.filter((x) => x.id !== b.id))}>✕</GoldButton>
            </div>
          ))}
        </>
      )}

      <div className="sam-label mt-4 mb-1">Share a pack</div>
      <textarea className="sam-input sam-mono" rows={3} spellCheck={false} value={io}
        onChange={(e) => setIo(e.target.value)} placeholder="Paste a pack here to import, or hit Export to fill this box." />
      <div className="flex gap-2 mt-2">
        <GoldButton onClick={() => setIo(exportPack(blocks))} disabled={!blocks.length}>Export pack</GoldButton>
        <GoldButton onClick={doImport} disabled={!io.trim()}>Import pack</GoldButton>
      </div>
    </Panel>
  );
}
