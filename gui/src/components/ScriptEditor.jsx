/*
 * Behavior-script editor: a monospace code area plus a click-to-insert side
 * panel (API functions / events / snippets) driven by data/samApi.js +
 * data/snippets.js. Dependency-light (no CodeMirror) — inserts at the cursor.
 * Controlled: props { code, onCode, lang, onLang }.
 */
import { useMemo, useRef, useState } from 'react';
import { SAM_FUNCTIONS, SAM_EVENTS } from '@/data/samApi.js';
import { SNIPPETS } from '@/data/snippets.js';
import { lintScript } from '@/lib/lintScript.js';
import { generateLua } from '@/lib/codegen.js';
import { GoldButton } from '@/components/ui.jsx';
import BlockBuilder from '@/components/BlockBuilder.jsx';

const LANGS = [
  { id: 'lua', label: 'Lua' },
  { id: 'js', label: 'JS' },
  { id: 'ts', label: 'TS' },
];

/** A placeholder argument for a param when inserting a call template. */
function placeholder(p) {
  if (p.values && p.values.length) return JSON.stringify(p.values[0]);
  switch (p.type) {
    case 'string': return '""';
    case 'int':
    case 'number': return '0';
    case 'bool': return 'false';
    case 'uid': return 'uid';
    case 'table': return '{}';
    default: return p.name;
  }
}

export function callTemplate(fn) {
  return `${fn.name}(${(fn.params || []).map(placeholder).join(', ')})`;
}

export function callSignature(fn) {
  return `${fn.name}(${(fn.params || []).map((p) => p.name).join(', ')})`;
}

function eventSkeleton(ev, lang) {
  return lang === 'lua'
    ? `if event.name == "${ev.name}" then\n    \nend`
    : `if (event.name === "${ev.name}") {\n    \n}`;
}

export default function ScriptEditor({ code, onCode, lang, onLang, pathHint = 'classes/<class>', blocks, onBlocks }) {
  const taRef = useRef(null);
  const [tab, setTab] = useState('api'); // api | events | snippets
  const [query, setQuery] = useState('');
  // Basic = build it from blocks; Advanced = write the code. Prefer Basic when there are
  // saved blocks to restore (so reopening shows the bricks, not raw Lua) — but ONLY if the
  // saved code still matches what those blocks generate. If someone built blocks, switched to
  // Advanced and hand-edited the Lua, the code has diverged and the blocks are no longer the
  // source of truth: stay in Advanced so those edits stay visible and aren't regenerated over.
  const [mode, setMode] = useState(() => {
    const trimmed = (code ?? '').trim();
    if (blocks && blocks.length) {
      if (!trimmed) return 'basic';
      try { if (trimmed === generateLua({ rules: blocks }).trim()) return 'basic'; } catch { /* fall through */ }
      return 'advanced'; // code diverged from the blocks — keep the hand edits
    }
    return trimmed ? 'advanced' : 'basic';
  });

  const insert = (text) => {
    const ta = taRef.current;
    const cur = code ?? '';
    if (!ta) { onCode(cur + text); return; }
    const start = ta.selectionStart ?? cur.length;
    const end = ta.selectionEnd ?? start;
    const next = cur.slice(0, start) + text + cur.slice(end);
    onCode(next);
    requestAnimationFrame(() => {
      ta.focus();
      const pos = start + text.length;
      ta.setSelectionRange(pos, pos);
    });
  };

  const onKeyDown = (e) => {
    if (e.key === 'Tab') { e.preventDefault(); insert('    '); }
  };

  const diagnostics = useMemo(() => lintScript(code), [code]);

  const q = query.trim().toLowerCase();
  const fns = useMemo(
    () => SAM_FUNCTIONS.filter((f) => !q || f.name.includes(q) || f.desc.toLowerCase().includes(q) || f.category.toLowerCase().includes(q)),
    [q]
  );
  const events = useMemo(
    () => SAM_EVENTS.filter((e) => !q || e.name.includes(q) || (e.whenFired || '').toLowerCase().includes(q)),
    [q]
  );
  const snippets = useMemo(
    () => SNIPPETS.filter((s) => !q || s.title.toLowerCase().includes(q) || s.desc.toLowerCase().includes(q)),
    [q]
  );

  const langForSnippet = lang === 'lua' ? 'lua' : 'js';

  const ModeTabs = (
    <div className="flex items-center gap-1 mb-3">
      {[
        { id: 'basic', label: '🧱 Basic', hint: 'build it from blocks' },
        { id: 'advanced', label: '</> Advanced', hint: 'write the code' },
      ].map((m) => (
        <button
          key={m.id}
          type="button"
          className="sam-btn"
          title={m.hint}
          style={{
            padding: '0.3rem 0.8rem',
            borderColor: mode === m.id ? 'var(--color-gold)' : undefined,
            color: mode === m.id ? 'var(--color-gold-bright)' : undefined,
          }}
          onClick={() => setMode(m.id)}
        >{m.label}</button>
      ))}
      <span className="text-xs ml-2" style={{ color: '#6b5a35' }}>
        {mode === 'basic'
          ? 'Pick a trigger, add conditions and actions — it writes the Lua for you.'
          : 'Full editor with the API, events and snippets.'}
      </span>
    </div>
  );

  if (mode === 'basic') {
    return (
      <div>
        {ModeTabs}
        <BlockBuilder
          hasExistingCode={!!(code ?? '').trim()}
          initialRules={blocks}
          onRules={onBlocks}
          onLiveCode={(generated) => { onCode(generated); onLang('lua'); }}
          onUseScript={(generated) => {
            onCode(generated);
            onLang('lua');      // the builder emits Lua
            setMode('advanced'); // land in the editor so it's obvious what you got
          }}
        />
      </div>
    );
  }

  return (
    <div>
    {ModeTabs}
    <div className="grid grid-cols-1 lg:grid-cols-[1fr_320px] gap-3 items-start">
      {/* -------- editor -------- */}
      <div>
        <div className="flex items-center gap-2 mb-2">
          <span className="sam-label" style={{ color: '#8a6d2e' }}>Language</span>
          <div className="flex gap-1">
            {LANGS.map((l) => (
              <button
                key={l.id}
                type="button"
                className="sam-btn"
                style={{
                  padding: '0.25rem 0.6rem',
                  borderColor: lang === l.id ? 'var(--color-gold)' : undefined,
                  color: lang === l.id ? 'var(--color-gold-bright)' : undefined,
                }}
                onClick={() => onLang(l.id)}
              >{l.label}</button>
            ))}
          </div>
          <span className="text-xs ml-auto" style={{ color: '#6b5a35' }}>
            ships as <span className="sam-mono">{pathHint}.{lang}</span>
          </span>
        </div>
        <textarea
          ref={taRef}
          className="sam-input sam-mono"
          spellCheck={false}
          value={code}
          onChange={(e) => onCode(e.target.value)}
          onKeyDown={onKeyDown}
          rows={18}
          placeholder={'-- define on_event(event) and/or on_tick(event)\n-- click an API call, event, or snippet on the right to insert it'}
          style={{ fontSize: '0.8rem', lineHeight: 1.5, whiteSpace: 'pre', overflowWrap: 'normal', tabSize: 4 }}
        />
        {diagnostics.length > 0 && (
          <div className="sam-well p-2 mt-2" style={{ borderColor: '#7a5a2a' }}>
            <div className="sam-label mb-1" style={{ color: '#d4a84b' }}>⚠ {diagnostics.length} hint{diagnostics.length === 1 ? '' : 's'}</div>
            <ul className="space-y-1">
              {diagnostics.slice(0, 20).map((d, i) => (
                <li key={i} className="text-xs" style={{ color: 'var(--color-parchment)' }}>
                  <span className="sam-mono" style={{ color: '#8a6d2e' }}>line {d.line}</span> — {d.message}
                </li>
              ))}
            </ul>
            <div className="text-xs mt-1" style={{ color: '#6b5a35' }}>advisory — checked against the SAM API; never blocks saving/export</div>
          </div>
        )}
      </div>

      {/* -------- reference / insert panel -------- */}
      <div className="sam-well p-2" style={{ maxHeight: 460, display: 'flex', flexDirection: 'column' }}>
        <div className="flex gap-1 mb-2">
          {[['api', 'API'], ['events', 'Events'], ['snippets', 'Snippets']].map(([id, label]) => (
            <button
              key={id}
              type="button"
              className="sam-btn flex-1"
              style={{
                padding: '0.25rem',
                fontSize: '0.75rem',
                borderColor: tab === id ? 'var(--color-gold)' : undefined,
                color: tab === id ? 'var(--color-gold-bright)' : undefined,
              }}
              onClick={() => setTab(id)}
            >{label}</button>
          ))}
        </div>
        <input
          className="sam-input mb-2"
          style={{ fontSize: '0.75rem', padding: '0.3rem 0.5rem' }}
          placeholder="search…"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
        />
        <div style={{ overflowY: 'auto', flex: 1 }}>
          {tab === 'api' && fns.map((f) => (
            <button
              key={f.name}
              type="button"
              className="sam-ref-item"
              title={f.desc}
              onClick={() => insert(callTemplate(f))}
            >
              <span className="sam-mono" style={{ color: 'var(--color-gold)', fontSize: '0.72rem' }}>{callSignature(f)}</span>
              {f.hostOnly && <span className="sam-hostonly" title="Host-authoritative — runs on host/singleplayer only">host</span>}
              <span className="block text-xs" style={{ color: '#6b5a35' }}>{f.desc}</span>
            </button>
          ))}
          {tab === 'events' && events.map((e) => (
            <button
              key={e.name}
              type="button"
              className="sam-ref-item"
              title={e.whenFired}
              onClick={() => insert(eventSkeleton(e, lang))}
            >
              <span className="sam-mono" style={{ color: 'var(--color-gold)', fontSize: '0.72rem' }}>{e.name}</span>
              {e.cancellable && <span className="sam-hostonly" style={{ background: '#3a2a10' }}>cancel</span>}
              <span className="block text-xs" style={{ color: '#6b5a35' }}>{e.whenFired}{e.gotcha ? ` — ${e.gotcha}` : ''}</span>
            </button>
          ))}
          {tab === 'snippets' && snippets.map((s) => (
            <button
              key={s.title}
              type="button"
              className="sam-ref-item"
              title={s.desc}
              onClick={() => insert((code && !code.endsWith('\n') ? '\n\n' : '') + s[langForSnippet])}
            >
              <span style={{ color: 'var(--color-gold-bright)', fontSize: '0.78rem' }}>{s.title}</span>
              <span className="block text-xs" style={{ color: '#6b5a35' }}>{s.desc}</span>
            </button>
          ))}
        </div>
      </div>
    </div>
    </div>
  );
}
