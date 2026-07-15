/*
 * Behavior-script editor: a monospace code area plus a click-to-insert side
 * panel (API functions / events / snippets) driven by data/samApi.js +
 * data/snippets.js. Dependency-light (no CodeMirror) — inserts at the cursor.
 * Controlled: props { code, onCode, lang, onLang }.
 */
import { useMemo, useRef, useState } from 'react';
import { SAM_FUNCTIONS, SAM_EVENTS } from '@/data/samApi.js';
import { SNIPPETS } from '@/data/snippets.js';
import { GoldButton } from '@/components/ui.jsx';

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

export default function ScriptEditor({ code, onCode, lang, onLang }) {
  const taRef = useRef(null);
  const [tab, setTab] = useState('api'); // api | events | snippets
  const [query, setQuery] = useState('');

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

  return (
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
            ships as <span className="sam-mono">classes/&lt;class&gt;.{lang}</span>
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
  );
}
