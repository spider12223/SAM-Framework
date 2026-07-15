/*
 * API Reference — a searchable, always-accurate catalog of the SAM scripting
 * surface (data/samApi.js, generated from the Lua/JS runtime). Functions grouped
 * by category with signature + host-only flag + return; events with payload,
 * cancellability, and gotchas. This is the map modders previously had to
 * reverse-engineer from C++.
 */
import { useMemo, useState } from 'react';
import { SAM_FUNCTIONS, SAM_EVENTS } from '@/data/samApi.js';
import { callSignature } from '@/components/ScriptEditor.jsx';
import { Panel } from '@/components/ui.jsx';

const CATEGORY_ORDER = [
  'Logging', 'Player state', 'Rewards', 'Status effects', 'Inventory', 'Entities',
  'Damage', 'Monsters', 'Live patching', 'Spells', 'Persistence', 'Timers',
  'Custom events', 'Input',
];

function paramList(f) {
  return (f.params || []).map((p) => `${p.name}: ${p.type}${p.values ? ` (${p.values.join('|')})` : ''}`).join(', ');
}

export default function ApiReference() {
  const [query, setQuery] = useState('');
  const [view, setView] = useState('functions'); // functions | events

  const q = query.trim().toLowerCase();
  const fns = useMemo(
    () => SAM_FUNCTIONS.filter((f) => !q || f.name.includes(q) || f.desc.toLowerCase().includes(q) || f.category.toLowerCase().includes(q)),
    [q]
  );
  const events = useMemo(
    () => SAM_EVENTS.filter((e) => !q || e.name.includes(q) || (e.whenFired || '').toLowerCase().includes(q) || (e.category || '').includes(q)),
    [q]
  );

  const byCategory = useMemo(() => {
    const groups = {};
    for (const f of fns) (groups[f.category] ??= []).push(f);
    return CATEGORY_ORDER.filter((c) => groups[c]).map((c) => [c, groups[c]]);
  }, [fns]);

  return (
    <div className="space-y-4 max-w-7xl mx-auto">
      <Panel title="Scripting API Reference">
        <p className="text-sm mb-3" style={{ color: '#6b5a35' }}>
          Every <span className="sam-mono">sam_*</span> function and event a behavior script can use — the same
          surface in Lua, JS and TS. Scripts define <span className="sam-mono">on_event(event)</span> and/or{' '}
          <span className="sam-mono">on_tick(event)</span>. A{' '}
          <span className="sam-hostonly" style={{ position: 'static', display: 'inline-block' }}>host</span>{' '}
          tag means the call is host-authoritative (host / singleplayer only).
        </p>
        <div className="flex flex-wrap items-center gap-3">
          <div className="flex gap-1">
            {[['functions', `Functions (${SAM_FUNCTIONS.length})`], ['events', `Events (${SAM_EVENTS.length})`]].map(([id, label]) => (
              <button
                key={id}
                type="button"
                className="sam-btn"
                style={{
                  padding: '0.35rem 0.8rem',
                  borderColor: view === id ? 'var(--color-gold)' : undefined,
                  color: view === id ? 'var(--color-gold-bright)' : undefined,
                }}
                onClick={() => setView(id)}
              >{label}</button>
            ))}
          </div>
          <input
            className="sam-input flex-1"
            style={{ minWidth: 200 }}
            placeholder="search functions, events, categories…"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
          />
        </div>
      </Panel>

      {view === 'functions' && byCategory.map(([cat, list]) => (
        <Panel key={cat} title={`${cat} (${list.length})`}>
          <div className="space-y-2">
            {list.map((f) => (
              <div key={f.name} className="sam-well p-2">
                <div className="flex items-center gap-2 flex-wrap">
                  <span className="sam-mono" style={{ color: 'var(--color-gold-bright)' }}>{callSignature(f)}</span>
                  {f.hostOnly && <span className="sam-hostonly" style={{ position: 'static' }}>host</span>}
                </div>
                <div className="text-sm mt-1" style={{ color: 'var(--color-parchment)' }}>{f.desc}</div>
                <div className="text-xs mt-1 sam-mono" style={{ color: '#6b5a35' }}>
                  {paramList(f) && <>args: {paramList(f)} · </>}returns: {f.returns}
                </div>
              </div>
            ))}
          </div>
        </Panel>
      ))}

      {view === 'events' && (
        <Panel title={`Events (${events.length})`}>
          <div className="space-y-2">
            {events.map((e) => (
              <div key={e.name} className="sam-well p-2">
                <div className="flex items-center gap-2 flex-wrap">
                  <span className="sam-mono" style={{ color: 'var(--color-gold-bright)' }}>{e.name}</span>
                  <span className="text-xs" style={{ color: '#6b5a35' }}>[{e.category}]</span>
                  {e.cancellable && <span className="sam-hostonly" style={{ position: 'static', background: '#3a2a10' }}>cancellable</span>}
                </div>
                <div className="text-sm mt-1" style={{ color: 'var(--color-parchment)' }}>{e.whenFired}</div>
                {e.payload?.length > 0 && (
                  <div className="text-xs mt-1 sam-mono" style={{ color: '#6b5a35' }}>
                    event: {e.payload.map((p) => `${p.field}: ${p.type}`).join(', ')}
                  </div>
                )}
                {e.gotcha && <div className="text-xs mt-1" style={{ color: '#a5763a' }}>⚠ {e.gotcha}</div>}
              </div>
            ))}
          </div>
        </Panel>
      )}
    </div>
  );
}
