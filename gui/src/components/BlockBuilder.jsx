/*
 * Visual script builder — "lego bricks" that generate Lua.
 *
 * Shape: WHEN (one trigger) -> IF (0..n conditions) -> DO (1..n actions), each row a
 * dropdown plus its parameters, with a + to add another. That maps 1:1 onto how every
 * S.A.M script is actually written, so the generated code is the same canonical thing
 * you'd write by hand.
 *
 * Generation is ONE-WAY on purpose: blocks -> Lua. Reading edited Lua back into blocks
 * would need a Lua parser and would quietly lose anything it couldn't represent, so
 * instead you build a skeleton here and fine-tune it in Advanced. The UI says so rather
 * than pretending to round-trip.
 *
 * Everything offered here is derived from the real API manifest (see data/blocks.js), so
 * the builder cannot produce a hook, function, or event field that doesn't exist.
 */
import { useMemo, useState, useEffect } from 'react';
import { Panel, Field, Select, TextInput, NumberInput, GoldButton } from '@/components/ui.jsx';
import {
  TRIGGERS, allConditions, findTrigger, findCondition, findAction, actionsFor, EVERY_SECONDS,
  registerCustom,
} from '@/data/blocks.js';
import { generateLua, describeSpec } from '@/lib/codegen.js';
import CustomBlockEditor from '@/components/CustomBlockEditor.jsx';
import { loadBlocks, saveBlocks, toCatalogEntry } from '@/lib/customBlocks.js';

const defaults = (def) => {
  const out = {};
  for (const p of def?.params || []) out[p.name] = p.default;
  return out;
};

/** Renders one param control from its declared type. */
function Param({ p, value, onChange }) {
  const common = { value: value ?? p.default, onChange };
  return (
    <Field label={p.label || p.name} className="min-w-[8rem]">
      {p.type === 'select' ? <Select {...common} options={p.values} />
        : p.type === 'number' ? <NumberInput {...common} min={p.min} max={p.max} />
        : <TextInput {...common} placeholder={p.default} />}
    </Field>
  );
}

/**
 * One condition/action row: a picker, its params, a code peek, and a remove button.
 *
 * The </> peek is the point of the Basic tab, not a nicety: it shows the exact line THIS
 * brick becomes, with your values already in it. That's how someone crosses from blocks
 * to writing Lua — you see "give an item" is literally sam_grant_item(player, "..."),
 * and eventually you stop needing the block.
 */
function Row({ kind, row, options, onChange, onRemove }) {
  const [peek, setPeek] = useState(false);
  const def = kind === 'if' ? findCondition(row.id) : findAction(row.id);
  const pick = (id) => {
    const d = kind === 'if' ? findCondition(id) : findAction(id);
    onChange({ ...row, id, params: defaults(d), negate: false });
  };
  // The same call the generator emits — shown as a guard for conditions so it reads the
  // way it will actually appear in the script, not as a bare expression.
  let code = '';
  try {
    const expr = def?.lua(row.params || {});
    code = kind === 'if'
      ? (row.negate ? `if ${expr} then return end` : `if not (${expr}) then return end`)
      : expr;
  } catch { code = '(pick options above)'; }

  return (
    <div className="sam-well p-3 mb-2">
      <div className="flex items-end gap-2 flex-wrap">
        <Field label={kind === 'if' ? 'Condition' : 'Action'} className="flex-1 min-w-[12rem]">
          <Select
            value={row.id}
            onChange={pick}
            options={options.map((o) => ({ value: o.id, label: o.label }))}
          />
        </Field>
        {def?.negatable && (
          <Field label="Invert" className="w-24">
            <Select
              value={row.negate ? 'not' : 'is'}
              onChange={(v) => onChange({ ...row, negate: v === 'not' })}
              options={[{ value: 'is', label: 'is' }, { value: 'not', label: 'is NOT' }]}
            />
          </Field>
        )}
        {(def?.params || []).map((p) => (
          <Param
            key={p.name}
            p={p}
            value={row.params?.[p.name]}
            onChange={(v) => onChange({ ...row, params: { ...row.params, [p.name]: v } })}
          />
        ))}
        <GoldButton
          onClick={() => setPeek((v) => !v)}
          className="mb-[2px]"
          title="Show the Lua this block becomes"
          style={{ borderColor: peek ? 'var(--color-gold)' : undefined, color: peek ? 'var(--color-gold-bright)' : undefined }}
        >{'</>'}</GoldButton>
        <GoldButton tone="red" onClick={onRemove} className="mb-[2px]">✕</GoldButton>
      </div>
      {peek && (
        <pre
          className="mt-2 p-2 text-xs overflow-x-auto"
          style={{ background: '#150f06', border: '1px solid #4a3617', color: '#e8d5a3', whiteSpace: 'pre' }}
        >{code}</pre>
      )}
      {def?.note && <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>{def.note}</div>}
    </div>
  );
}

export default function BlockBuilder({ onUseScript, hasExistingCode }) {
  const [spec, setSpec] = useState({
    trigger: { id: 'player.on_hit', params: {} },
    conditions: [],
    actions: [{ id: 'message', params: defaults(findAction('message')) }],
  });
  const [custom, setCustom] = useState(() => loadBlocks());
  const [editing, setEditing] = useState(false);

  // Push the user's bricks into the catalog so they behave exactly like built-ins.
  useEffect(() => {
    registerCustom(custom.map((b) => ({ kind: b.kind, entry: toCatalogEntry(b) })));
    saveBlocks(custom);
  }, [custom]);

  const trigger = findTrigger(spec.trigger.id);
  const availableActions = useMemo(() => actionsFor(trigger), [trigger, custom]);
  const availableConditions = useMemo(() => allConditions(), [custom]);
  const lua = useMemo(() => generateLua(spec), [spec]);
  const english = useMemo(() => describeSpec(spec), [spec]);

  // Changing the trigger can strip actions that need an event field the new one lacks
  // (e.g. "damage the target" needs target_uid). Drop them rather than emit a nil read.
  const setTrigger = (id) => {
    const t = findTrigger(id);
    const stillValid = actionsFor(t).map((a) => a.id);
    setSpec((s) => ({
      ...s,
      trigger: { id, params: defaults(t) },
      actions: s.actions.filter((a) => stillValid.includes(a.id)),
    }));
  };

  const addRow = (kind) => setSpec((s) => {
    const list = kind === 'if' ? availableConditions : availableActions;
    const first = list[0];
    if (!first) return s;
    const row = { id: first.id, params: defaults(first), negate: false };
    return kind === 'if'
      ? { ...s, conditions: [...s.conditions, row] }
      : { ...s, actions: [...s.actions, row] };
  });

  const patch = (kind, i, row) => setSpec((s) => {
    const key = kind === 'if' ? 'conditions' : 'actions';
    const next = [...s[key]]; next[i] = row;
    return { ...s, [key]: next };
  });
  const drop = (kind, i) => setSpec((s) => {
    const key = kind === 'if' ? 'conditions' : 'actions';
    return { ...s, [key]: s[key].filter((_, j) => j !== i) };
  });

  const triggerOptions = TRIGGERS.map((t) => ({ value: t.id, label: t.id === EVERY_SECONDS ? '⏱ Every N seconds' : t.id }));

  return (
    <div className="grid gap-4" style={{ gridTemplateColumns: 'minmax(0,1fr) minmax(0,22rem)' }}>
      <div>
        <Panel title="① WHEN — what sets it off">
          <div className="flex items-end gap-2 flex-wrap">
            <Field label="Trigger" className="flex-1 min-w-[14rem]">
              <Select value={spec.trigger.id} onChange={setTrigger} options={triggerOptions} />
            </Field>
            {(trigger?.params || []).map((p) => (
              <Param
                key={p.name}
                p={p}
                value={spec.trigger.params?.[p.name]}
                onChange={(v) => setSpec((s) => ({ ...s, trigger: { ...s.trigger, params: { ...s.trigger.params, [p.name]: v } } }))}
              />
            ))}
          </div>
          {trigger?.whenFired && (
            <div className="text-xs mt-2" style={{ color: '#8a7a4a' }}>Fires when {trigger.whenFired}.</div>
          )}
          {trigger?.gotcha && (
            <div className="text-xs mt-1 sam-error">⚠ {trigger.gotcha}</div>
          )}
        </Panel>

        <Panel title="② IF — conditions (optional)" className="mt-4">
          {spec.conditions.length === 0 && (
            <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
              No conditions — it fires every time. Add one to narrow it down.
            </div>
          )}
          {spec.conditions.map((row, i) => (
            <Row key={i} kind="if" row={row} options={availableConditions}
              onChange={(r) => patch('if', i, r)} onRemove={() => drop('if', i)} />
          ))}
          <GoldButton onClick={() => addRow('if')}>+ Add condition</GoldButton>
        </Panel>

        <Panel title="③ DO — what happens" className="mt-4">
          {spec.actions.map((row, i) => (
            <Row key={i} kind="do" row={row} options={availableActions}
              onChange={(r) => patch('do', i, r)} onRemove={() => drop('do', i)} />
          ))}
          <GoldButton onClick={() => addRow('do')}>+ Add action</GoldButton>
          <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>
            Only actions this trigger can actually support are listed — e.g. “damage the target”
            needs an event that carries one.
          </div>
        </Panel>

        {editing
          ? <div className="mt-4"><CustomBlockEditor blocks={custom} onChange={setCustom} onClose={() => setEditing(false)} /></div>
          : (
            <div className="mt-4 flex items-center gap-2">
              <GoldButton onClick={() => setEditing(true)}>🧱 Make your own blocks…</GoldButton>
              <span className="text-xs" style={{ color: '#6b5a35' }}>
                {custom.length
                  ? `${custom.length} custom block${custom.length === 1 ? '' : 's'} loaded — they show up in the lists above.`
                  : 'Build a brick once and reuse it, or import a pack from a friend.'}
              </span>
            </div>
          )}
      </div>

      <div>
        <Panel title="In plain English">
          <div className="text-sm" style={{ color: '#e8d5a3' }}>{english}</div>
        </Panel>

        <Panel title="Generated Lua" className="mt-4">
          <pre className="sam-well p-3 text-xs overflow-x-auto" style={{ color: '#e8d5a3', whiteSpace: 'pre' }}>{lua}</pre>
          <div className="mt-3 flex flex-col gap-2">
            <GoldButton onClick={() => onUseScript(lua)}>▶ Use this script</GoldButton>
            <div className="text-xs" style={{ color: '#6b5a35' }}>
              {hasExistingCode
                ? '⚠ This REPLACES the script you already have. Copy it first if you want to keep it.'
                : 'Writes this into the editor.'}
            </div>
            <div className="text-xs" style={{ color: '#6b5a35' }}>
              Blocks generate Lua one way. Tweak it in <b>Advanced</b> afterwards — those edits
              won’t come back into the blocks.
            </div>
          </div>
        </Panel>
      </div>
    </div>
  );
}
