/*
 * Visual script builder — "lego bricks" that generate Lua.
 *
 * A class can have MANY abilities. Each is one rule: WHEN (a trigger) -> IF (0..n
 * conditions) -> DO (1..n actions). They all compile into a single on_event / on_tick
 * pair, because a script may only define each handler once — pasting two generated
 * scripts together is a syntax error, and a syntax error stops the whole file loading,
 * so even the ability that worked alone dies. Building them here removes that trap.
 *
 * Generation is ONE-WAY on purpose: blocks -> Lua. Reading edited Lua back into blocks
 * would need a Lua parser and would quietly lose anything it couldn't represent, so you
 * build a skeleton here and fine-tune it in Advanced. The UI says so rather than
 * pretending to round-trip.
 *
 * Everything offered is derived from the real API manifest (see data/blocks.js), so the
 * builder cannot produce a hook, function, or event field that doesn't exist.
 */
import { useMemo, useState, useEffect } from 'react';
import { Panel, Field, Select, TextInput, NumberInput, GoldButton } from '@/components/ui.jsx';
import {
  TRIGGERS, allConditions, findTrigger, findCondition, findAction, actionsFor, EVERY_SECONDS,
  registerCustom,
} from '@/data/blocks.js';
import { generateLua, describeRule } from '@/lib/codegen.js';
import CustomBlockEditor from '@/components/CustomBlockEditor.jsx';
import { loadBlocks, saveBlocks, toCatalogEntry } from '@/lib/customBlocks.js';

const defaults = (def) => {
  const out = {};
  for (const p of def?.params || []) out[p.name] = p.default;
  return out;
};

const newRule = () => ({
  key: Math.random().toString(36).slice(2, 9),
  trigger: { id: 'player.on_hit', params: {} },
  conditions: [],
  actions: [{ id: 'message', params: defaults(findAction('message')) }],
});

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
 * One condition/action row. The </> peek shows the exact line THIS brick becomes, with
 * your values in it — the bridge from blocks to writing Lua yourself.
 */
function Row({ kind, row, options, onChange, onRemove }) {
  const [peek, setPeek] = useState(false);
  const def = kind === 'if' ? findCondition(row.id) : findAction(row.id);
  const pick = (id) => {
    const d = kind === 'if' ? findCondition(id) : findAction(id);
    onChange({ ...row, id, params: defaults(d), negate: false });
  };
  let code = '';
  try {
    const out = def?.lua(row.params || {});
    const expr = Array.isArray(out) ? out.join('\n') : out;   // "for a while" is multi-line
    code = kind === 'if' && row.negate ? `not (${expr})` : expr;
  } catch { code = '(pick options above)'; }

  return (
    <div className="sam-well p-3 mb-2">
      <div className="flex items-end gap-2 flex-wrap">
        <Field label={kind === 'if' ? 'Condition' : 'Action'} className="flex-1 min-w-[12rem]">
          <Select value={row.id} onChange={pick} options={options.map((o) => ({ value: o.id, label: o.label }))} />
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
          <Param key={p.name} p={p} value={row.params?.[p.name]}
            onChange={(v) => onChange({ ...row, params: { ...row.params, [p.name]: v } })} />
        ))}
        <GoldButton onClick={() => setPeek((v) => !v)} className="mb-[2px]" title="Show the Lua this block becomes"
          style={{ borderColor: peek ? 'var(--color-gold)' : undefined, color: peek ? 'var(--color-gold-bright)' : undefined }}
        >{'</>'}</GoldButton>
        <GoldButton tone="red" onClick={onRemove} className="mb-[2px]">✕</GoldButton>
      </div>
      {peek && (
        <pre className="mt-2 p-2 text-xs overflow-x-auto"
          style={{ background: '#150f06', border: '1px solid #4a3617', color: '#e8d5a3', whiteSpace: 'pre' }}>{code}</pre>
      )}
      {def?.note && <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>{def.note}</div>}
    </div>
  );
}

/** One ability: trigger + conditions + actions. */
function RuleEditor({ rule, index, total, conditions, onChange, onRemove }) {
  const trigger = findTrigger(rule.trigger.id);
  const availableActions = useMemo(() => actionsFor(trigger), [trigger, conditions]);

  // Changing the trigger can strip actions needing an event field the new one lacks
  // (e.g. "damage the target" needs target_uid). Drop them rather than emit a nil read.
  const setTrigger = (id) => {
    const t = findTrigger(id);
    const ok = actionsFor(t).map((a) => a.id);
    onChange({ ...rule, trigger: { id, params: defaults(t) }, actions: rule.actions.filter((a) => ok.includes(a.id)) });
  };
  const addRow = (kind) => {
    const list = kind === 'if' ? conditions : availableActions;
    const first = list[0];
    if (!first) return;
    const row = { id: first.id, params: defaults(first), negate: false };
    onChange(kind === 'if'
      ? { ...rule, conditions: [...rule.conditions, row] }
      : { ...rule, actions: [...rule.actions, row] });
  };
  const patch = (kind, i, row) => {
    const key = kind === 'if' ? 'conditions' : 'actions';
    const next = [...rule[key]]; next[i] = row;
    onChange({ ...rule, [key]: next });
  };
  const drop = (kind, i) => {
    const key = kind === 'if' ? 'conditions' : 'actions';
    onChange({ ...rule, [key]: rule[key].filter((_, j) => j !== i) });
  };

  const triggerOptions = TRIGGERS.map((t) => ({ value: t.id, label: t.id === EVERY_SECONDS ? '⏱ Every N seconds' : t.id }));

  return (
    <Panel title={`Ability ${index + 1}${total > 1 ? ` of ${total}` : ''}`} className="mb-4">
      <div className="text-sm mb-3" style={{ color: '#e8d5a3' }}>{describeRule(rule)}</div>

      <div className="sam-label mb-1">① WHEN</div>
      <div className="flex items-end gap-2 flex-wrap mb-1">
        <Field label="Trigger" className="flex-1 min-w-[14rem]">
          <Select value={rule.trigger.id} onChange={setTrigger} options={triggerOptions} />
        </Field>
        {(trigger?.params || []).map((p) => (
          <Param key={p.name} p={p} value={rule.trigger.params?.[p.name]}
            onChange={(v) => onChange({ ...rule, trigger: { ...rule.trigger, params: { ...rule.trigger.params, [p.name]: v } } })} />
        ))}
      </div>
      {trigger?.gotcha && <div className="text-xs mb-2 sam-error">⚠ {trigger.gotcha}</div>}

      <div className="sam-label mt-3 mb-1">② IF (optional)</div>
      {rule.conditions.length === 0 && (
        <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>No conditions — it fires every time.</div>
      )}
      {rule.conditions.map((row, i) => (
        <Row key={i} kind="if" row={row} options={conditions}
          onChange={(r) => patch('if', i, r)} onRemove={() => drop('if', i)} />
      ))}
      <GoldButton onClick={() => addRow('if')}>+ Add condition</GoldButton>

      <div className="sam-label mt-3 mb-1">③ DO</div>
      {rule.actions.map((row, i) => (
        <Row key={i} kind="do" row={row} options={availableActions}
          onChange={(r) => patch('do', i, r)} onRemove={() => drop('do', i)} />
      ))}
      <GoldButton onClick={() => addRow('do')}>+ Add action</GoldButton>

      {total > 1 && (
        <div className="mt-3 pt-3" style={{ borderTop: '1px solid #4a3617' }}>
          <GoldButton tone="red" onClick={onRemove}>✕ Remove this ability</GoldButton>
        </div>
      )}
    </Panel>
  );
}

export default function BlockBuilder({ onUseScript, hasExistingCode }) {
  const [rules, setRules] = useState(() => [newRule()]);
  const [custom, setCustom] = useState(() => loadBlocks());
  const [editing, setEditing] = useState(false);
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    registerCustom(custom.map((b) => ({ kind: b.kind, entry: toCatalogEntry(b) })));
    saveBlocks(custom);
  }, [custom]);

  const conditions = useMemo(() => allConditions(), [custom]);
  const lua = useMemo(() => generateLua({ rules }), [rules, custom]);

  return (
    <div className="grid gap-4" style={{ gridTemplateColumns: 'minmax(0,1fr) minmax(0,22rem)' }}>
      <div>
        {rules.map((rule, i) => (
          <RuleEditor
            key={rule.key}
            rule={rule}
            index={i}
            total={rules.length}
            conditions={conditions}
            onChange={(r) => setRules((rs) => rs.map((x, j) => (j === i ? r : x)))}
            onRemove={() => setRules((rs) => rs.filter((_, j) => j !== i))}
          />
        ))}

        <div className="flex items-center gap-2">
          <GoldButton onClick={() => setRules((rs) => [...rs, newRule()])}>+ Add another ability</GoldButton>
          <span className="text-xs" style={{ color: '#6b5a35' }}>
            A class can have as many as you like — they all go into one script.
          </span>
        </div>

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
        <Panel title="Generated Lua">
          <pre className="sam-well p-3 text-xs overflow-x-auto" style={{ color: '#e8d5a3', whiteSpace: 'pre' }}>{lua}</pre>
          <div className="mt-3 flex flex-col gap-2">
            <div className="flex gap-2">
              <GoldButton onClick={() => onUseScript(lua)} className="flex-1">▶ Use this script</GoldButton>
              {/*
                Copy the whole thing, because hand-selecting it off the screen loses lines.
                Someone deleted the comment block and took `function on_event(event)` with
                it — leaving a bare `if` and an orphan `end`, which is a SYNTAX error, so
                the file doesn't load at all and even the ability that worked alone dies.
              */}
              <GoldButton onClick={() => { navigator.clipboard?.writeText(lua); setCopied(true); setTimeout(() => setCopied(false), 1200); }}>
                {copied ? '✓ Copied' : '⎘ Copy'}
              </GoldButton>
            </div>
            <div className="text-xs" style={{ color: '#6b5a35' }}>
              {hasExistingCode
                ? '⚠ This REPLACES the script you already have. Copy it first if you want to keep it.'
                : 'Writes this into the editor.'}
            </div>
            <div className="text-xs" style={{ color: '#6b5a35' }}>
              Copy the whole thing. The comments are safe to delete, but keep
              <span className="sam-mono"> function on_event(event) </span> and its final
              <span className="sam-mono"> end </span> — without them the file won’t parse, and a file
              that won’t parse loads nothing at all.
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
