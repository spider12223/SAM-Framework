/*
 * S.A.M GUI — shared medieval design-system components.
 * Every page composes ONLY these + tailwind layout utilities, so the whole
 * app inherits the reference look from one place.
 */
import { useEffect, useMemo, useRef, useState } from 'react';

/** Ornate panel with the «—◆ TITLE ◆—» header. */
export function Panel({ title, children, className = '', bodyClassName = '' }) {
  return (
    <section className={`sam-panel ${className}`}>
      {title && (
        <header className="sam-panel-header">
          <span className="sam-ornament">◆</span>
          <span>{title}</span>
          <span className="sam-ornament">◆</span>
        </header>
      )}
      <div className={`p-4 ${bodyClassName}`}>{children}</div>
    </section>
  );
}

/** Small-caps label + arbitrary control. */
export function Field({ label, children, hint, className = '' }) {
  return (
    <label className={`block ${className}`}>
      <div className="sam-label mb-1">{label}</div>
      {children}
      {hint && <div className="mt-1 text-xs" style={{ color: '#6b5a35' }}>{hint}</div>}
    </label>
  );
}

/** Text input in the parchment-on-dark style. */
export function TextInput({ value, onChange, placeholder, ...rest }) {
  return (
    <input
      className="sam-input"
      type="text"
      value={value}
      onChange={(e) => onChange(e.target.value)}
      placeholder={placeholder}
      {...rest}
    />
  );
}

/** Number input. */
export function NumberInput({ value, onChange, min, max, className = '', ...rest }) {
  return (
    <input
      className={`sam-input ${className}`}
      type="number"
      value={value}
      min={min}
      max={max}
      onChange={(e) => {
        const v = e.target.value;
        onChange(v === '' ? '' : Number(v));
      }}
      {...rest}
    />
  );
}

/** Native select styled to match. */
export function Select({ value, onChange, options, className = '', ...rest }) {
  return (
    <select
      className={`sam-input ${className}`}
      value={value}
      onChange={(e) => onChange(e.target.value)}
      {...rest}
    >
      {options.map((o) =>
        typeof o === 'string' ? (
          <option key={o} value={o}>{o}</option>
        ) : (
          <option key={o.value} value={o.value}>{o.label}</option>
        )
      )}
    </select>
  );
}

/** Gold slider with live fill + value box (matches Core Attributes rows). */
/**
 * Slider with an EDITABLE value box.
 *
 * The box, not the slider, is the precise control: you can type an exact number, and it
 * accepts values outside the slider's track (down to `inputMin`, up to `inputMax`) so a
 * negative attribute — a class that starts BELOW the racial base, which several vanilla
 * ones do — is reachable even though the slider itself stops at `min`. The slider is just
 * the quick-drag path; its fill is clamped so an out-of-track value pins it to an end
 * rather than overflowing.
 */
export function GoldSlider({
  label, icon, value, onChange, min = 0, max = 20, inputMin = min, inputMax = max,
}) {
  const n = value === '' ? 0 : Number(value);
  const fill = Math.max(0, Math.min(100, ((n - min) / (max - min)) * 100));
  const clampBox = (v) => (v === '' ? 0 : Math.max(inputMin, Math.min(inputMax, v)));
  return (
    <div className="flex items-center gap-3 py-1.5">
      {icon && <span className="w-5 text-center" aria-hidden>{icon}</span>}
      <span className="sam-label w-11 shrink-0" style={{ fontSize: '0.95rem' }}>{label}</span>
      <input
        className="sam-slider flex-1"
        style={{ '--fill': `${fill}%` }}
        type="range"
        min={min}
        max={max}
        value={Math.max(min, Math.min(max, n))}
        onChange={(e) => onChange(Number(e.target.value))}
      />
      <input
        className="sam-input sam-valuebox"
        style={{ width: '3.4rem', textAlign: 'center' }}
        type="number"
        min={inputMin}
        max={inputMax}
        value={value}
        onChange={(e) => onChange(e.target.value === '' ? '' : clampBox(Number(e.target.value)))}
        aria-label={`${label} value`}
      />
    </div>
  );
}

/** −/value/+ stepper row (matches Skill Levels rows). The ± buttons nudge by `step`, but
 *  the value box is editable so you can type an EXACT number — a class can start with
 *  Swords 3, not just multiples of 5. */
export function Stepper({ label, sub, value, onChange, min = 0, max = 100, step = 1 }) {
  const clamp = (v) => Math.min(max, Math.max(min, v));
  return (
    <div className="flex items-center gap-2 py-1">
      <span className="sam-label flex-1 truncate" style={{ fontSize: '0.95rem' }} title={sub}>
        {label}
      </span>
      <button type="button" className="sam-step" onClick={() => onChange(clamp((Number(value) || 0) - step))} aria-label={`decrease ${label}`}>−</button>
      <input
        className="sam-input sam-valuebox"
        style={{ minWidth: '2.6rem', width: '3.2rem', textAlign: 'center' }}
        type="number"
        min={min}
        max={max}
        value={value}
        onChange={(e) => onChange(e.target.value === '' ? '' : clamp(Number(e.target.value)))}
        aria-label={`${label} value`}
      />
      <button type="button" className="sam-step" onClick={() => onChange(clamp((Number(value) || 0) + step))} aria-label={`increase ${label}`}>+</button>
    </div>
  );
}

/** Primary ornate button. tone: 'gold' | 'red' | 'green' */
export function GoldButton({ children, onClick, tone = 'gold', disabled, className = '', ...rest }) {
  const toneClass = tone === 'red' ? 'sam-btn-red' : tone === 'green' ? 'sam-btn-green' : '';
  return (
    <button
      type="button"
      className={`sam-btn ${toneClass} ${className}`}
      onClick={onClick}
      disabled={disabled}
      {...rest}
    >
      {children}
    </button>
  );
}

/**
 * Searchable dropdown over a large option list (e.g. all vanilla ItemTypes).
 * Filters as you type; Enter or click picks; Esc closes.
 */
export function SearchSelect({ options, value, onPick, placeholder = 'Search…', maxShown = 50, allowCustom = false }) {
  const [query, setQuery] = useState('');
  const [open, setOpen] = useState(false);
  const boxRef = useRef(null);

  const matches = useMemo(() => {
    const q = query.trim().toUpperCase();
    const list = q ? options.filter((o) => o.toUpperCase().includes(q)) : options;
    return list.slice(0, maxShown);
  }, [options, query, maxShown]);

  useEffect(() => {
    const onDoc = (e) => {
      if (boxRef.current && !boxRef.current.contains(e.target)) setOpen(false);
    };
    document.addEventListener('mousedown', onDoc);
    return () => document.removeEventListener('mousedown', onDoc);
  }, []);

  return (
    <div className="relative" ref={boxRef}>
      <input
        className="sam-input"
        value={open ? query : (value ?? '')}
        placeholder={placeholder}
        onFocus={() => { setOpen(true); setQuery(''); }}
        onChange={(e) => setQuery(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === 'Enter' && matches.length > 0) {
            onPick(matches[0]);
            setOpen(false);
            e.preventDefault();
          } else if (e.key === 'Enter' && allowCustom && query.trim()) {
            // No match, but free-form values are allowed (e.g. custom item names).
            onPick(query.trim());
            setOpen(false);
            e.preventDefault();
          } else if (e.key === 'Escape') {
            setOpen(false);
          }
        }}
      />
      {open && (
        <div
          className="absolute z-20 mt-1 w-full max-h-64 overflow-y-auto sam-well"
          role="listbox"
        >
          {matches.length === 0 && (
            allowCustom && query.trim() ? (
              <div
                role="option"
                className="px-3 py-1.5 cursor-pointer text-sm hover:bg-[rgba(212,168,75,0.12)]"
                style={{ color: 'var(--color-parchment)' }}
                onMouseDown={() => { onPick(query.trim()); setOpen(false); }}
              >
                Use “<span className="sam-mono">{query.trim()}</span>” <span style={{ color: '#6b5a35' }}>(custom)</span>
              </div>
            ) : (
              <div className="px-3 py-2 text-sm" style={{ color: '#6b5a35' }}>No matches</div>
            )
          )}
          {matches.map((o) => (
            <div
              key={o}
              role="option"
              aria-selected={o === value}
              className="px-3 py-1.5 cursor-pointer text-sm hover:bg-[rgba(212,168,75,0.12)]"
              style={{ color: 'var(--color-parchment)' }}
              onMouseDown={() => { onPick(o); setOpen(false); }}
            >
              {o}
            </div>
          ))}
          {matches.length === maxShown && (
            <div className="px-3 py-1 text-xs" style={{ color: '#6b5a35' }}>
              …type to narrow further
            </div>
          )}
        </div>
      )}
    </div>
  );
}

/** Row in a Starting Items-style list: name, count box, optional clone, red X. */
export function ItemRow({ icon = '⚔', name, count, onCount, onRemove, onClone, onEdit, sub }) {
  return (
    <div className="sam-well flex items-center gap-3 px-3 py-2">
      <span className="opacity-40 select-none cursor-grab" aria-hidden>⣿</span>
      <span className="w-6 text-center" aria-hidden>{icon}</span>
      <div className="flex-1 min-w-0">
        <div className="truncate" style={{ color: 'var(--color-parchment)' }}>{name}</div>
        {sub && <div className="text-xs truncate" style={{ color: '#6b5a35' }}>{sub}</div>}
      </div>
      {onCount && (
        <input
          className="sam-valuebox w-14"
          type="number"
          min={1}
          value={count}
          onChange={(e) => onCount(Math.max(1, Number(e.target.value) || 1))}
          aria-label={`${name} count`}
        />
      )}
      {onEdit && (
        <button type="button" className="sam-step" onClick={onEdit} title="Edit" aria-label={`edit ${name}`}>✎</button>
      )}
      {onClone && (
        <button type="button" className="sam-step" onClick={onClone} title="Duplicate" aria-label={`duplicate ${name}`}>⧉</button>
      )}
      <button type="button" className="sam-step sam-remove" onClick={onRemove} aria-label={`remove ${name}`}>✕</button>
    </div>
  );
}

/** Advisory balance-hint panel — amber, never blocks save. Renders null when empty. */
export function BalanceHints({ hints }) {
  if (!hints || hints.length === 0) return null;
  return (
    <div className="sam-well p-3" style={{ borderColor: '#7a5a2a' }}>
      <div className="sam-label mb-2" style={{ color: '#d4a84b' }}>⚖ Balance Hints</div>
      <ul className="space-y-1">
        {hints.map((h, i) => (
          <li key={i} className="text-sm" style={{ color: 'var(--color-parchment)' }}>
            <span aria-hidden>{h.level === 'warn' ? '⚠' : 'ℹ'}</span>{' '}
            {h.message}
          </li>
        ))}
      </ul>
      <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>advisory only — these never block saving or exporting</div>
    </div>
  );
}

/** Validation error list (shared by editors + validator page). */
export function ErrorList({ errors, className = '' }) {
  if (!errors || errors.length === 0) return null;
  return (
    <div className={`sam-well p-3 ${className}`}>
      <div className="sam-label mb-2" style={{ color: '#e07a6a' }}>Validation Errors</div>
      <ul className="space-y-1">
        {errors.map((e, i) => (
          <li key={i} className="text-sm">
            <span className="sam-mono sam-error">{e.path}</span>
            <span style={{ color: 'var(--color-parchment)' }}> — {e.message}</span>
          </li>
        ))}
      </ul>
    </div>
  );
}

/** Toast-ish inline confirmation. */
export function SavedNote({ children }) {
  return <div className="sam-ok text-sm mt-2">{children}</div>;
}

/**
 * Item icon: renders the real in-game PNG when `src` is given, else the `emoji`.
 * Degrades to the emoji on image load error (e.g. when the Barony art isn't
 * bundled), so the icon area always shows something.
 */
export function ItemIcon({ src, emoji }) {
  const [failed, setFailed] = useState(false);
  if (src && !failed) {
    return <img src={src} alt="" className="sam-item-icon" draggable={false} onError={() => setFailed(true)} />;
  }
  return emoji;
}

/* ------------------------------------------------------------------ */
/* Barony-style starting-items inventory grid + item-picker modal.     */
/* ------------------------------------------------------------------ */

/**
 * Full-screen modal listing every vanilla ItemType, grouped by category, with a
 * search box. Click an item to pick it (calls onPick then onClose).
 *   allTypes    : string[]  — every ItemType name
 *   iconFor     : (t) => string emoji
 *   categoryFor : (t) => category key
 *   categories  : string[]  — category display order
 */
export function ItemPickerModal({ allTypes, iconFor, categoryFor, categories, onPick, onClose }) {
  const [query, setQuery] = useState('');
  const inputRef = useRef(null);

  useEffect(() => { inputRef.current?.focus(); }, []);
  useEffect(() => {
    const onKey = (e) => { if (e.key === 'Escape') onClose(); };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  }, [onClose]);

  const groups = useMemo(() => {
    const q = query.trim().toUpperCase();
    const byCat = new Map(categories.map((c) => [c, []]));
    const extra = [];
    for (const t of allTypes) {
      if (q && !t.toUpperCase().includes(q)) continue;
      const cat = categoryFor(t);
      if (byCat.has(cat)) byCat.get(cat).push(t);
      else extra.push(t);
    }
    const out = categories.map((c) => [c, byCat.get(c)]).filter(([, xs]) => xs.length > 0);
    if (extra.length) out.push(['OTHER', extra]);
    return out;
  }, [allTypes, categoryFor, categories, query]);

  const total = useMemo(() => groups.reduce((n, [, xs]) => n + xs.length, 0), [groups]);
  const nice = (c) => c.replace(/_/g, ' ');

  return (
    <div className="sam-modal-backdrop" onMouseDown={onClose}>
      <div className="sam-panel sam-modal" onMouseDown={(e) => e.stopPropagation()}>
        <header className="sam-panel-header">
          <span className="sam-ornament">◆</span>
          <span>Choose an Item</span>
          <span className="sam-ornament">◆</span>
        </header>
        <div className="p-3">
          <input
            ref={inputRef}
            className="sam-input"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder={`Search ${allTypes.length} items — e.g. IRON_SWORD`}
          />
        </div>
        <div className="flex-1 overflow-y-auto" style={{ borderTop: '1px solid #4a3617' }}>
          {total === 0 ? (
            <div className="px-3 py-6 text-sm text-center" style={{ color: '#6b5a35' }}>
              No items match “{query}”.
            </div>
          ) : (
            groups.map(([cat, xs]) => (
              <div key={cat}>
                <div className="sam-cat-header">{nice(cat)} · {xs.length}</div>
                {xs.map((t) => (
                  <div key={t} className="sam-pick-row" onMouseDown={() => { onPick(t); onClose(); }}>
                    <span className="sam-pick-icon" aria-hidden>{iconFor(t)}</span>
                    <span className="flex-1 truncate" style={{ color: 'var(--color-parchment)' }}>{t}</span>
                    <span className="text-xs shrink-0" style={{ color: '#6b5a35' }}>{nice(cat)}</span>
                  </div>
                ))}
              </div>
            ))
          )}
        </div>
        <div className="px-3 py-2 flex items-center justify-between" style={{ borderTop: '1px solid #4a3617' }}>
          <span className="text-xs" style={{ color: '#6b5a35' }}>{total} shown · Esc to close</span>
          <GoldButton onClick={onClose}>Close</GoldButton>
        </div>
      </div>
    </div>
  );
}

/**
 * Barony-style starting-items editor: a 4-column 64×64 slot grid (click an empty
 * slot to add via the picker, click a filled slot to change it, ✕ to remove),
 * plus a loadout list below with a count input + equip checkbox per item.
 *   items    : [{ type, count, equip }]
 *   onChange : (nextItems) => void
 * plus the same allTypes / iconFor / categoryFor / categories the modal needs.
 */
export function InventoryGrid({ items, allTypes, iconFor, categoryFor, categories, onChange }) {
  const [picker, setPicker] = useState(null); // null=closed | { slot: number | 'new' }

  const nSlots = Math.max(8, Math.ceil((items.length + 1) / 4) * 4);

  const pick = (type) => {
    if (!picker) return;
    if (picker.slot === 'new') {
      const i = items.findIndex((it) => it.type === type);
      if (i >= 0) onChange(items.map((it, j) => (j === i ? { ...it, count: it.count + 1 } : it)));
      else onChange([...items, { type, count: 1, equip: false }]);
    } else {
      onChange(items.map((it, j) => (j === picker.slot ? { ...it, type } : it)));
    }
  };

  const removeAt = (i) => onChange(items.filter((_, j) => j !== i));
  const setField = (i, field, val) => onChange(items.map((it, j) => (j === i ? { ...it, [field]: val } : it)));

  return (
    <div>
      <div className="grid" style={{ gridTemplateColumns: 'repeat(4, 64px)', gap: 4, justifyContent: 'start' }}>
        {Array.from({ length: nSlots }).map((_, i) => {
          const it = items[i];
          if (!it) {
            const isNextEmpty = i === items.length;
            return (
              <button
                key={i}
                type="button"
                className={`sam-slot sam-slot-empty ${picker?.slot === 'new' && isNextEmpty ? 'selected' : ''}`}
                title="Add an item"
                onClick={() => setPicker({ slot: 'new' })}
              >
                +
              </button>
            );
          }
          return (
            <div
              key={i}
              role="button"
              tabIndex={0}
              className={`sam-slot ${picker?.slot === i ? 'selected' : ''}`}
              title={`${it.type} — click to change`}
              onClick={() => setPicker({ slot: i })}
              onKeyDown={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); setPicker({ slot: i }); } }}
            >
              <span className="sam-slot-ico" aria-hidden>{iconFor(it.type)}</span>
              {it.count > 1 && <span className="sam-slot-count">×{it.count}</span>}
              <span className="sam-slot-x" title="Remove" onClick={(e) => { e.stopPropagation(); removeAt(i); }}>✕</span>
            </div>
          );
        })}
      </div>

      {items.length === 0 ? (
        <div className="text-sm py-3 text-center" style={{ color: '#6b5a35' }}>
          Empty loadout — click a slot to add starting gear.
        </div>
      ) : (
        <div className="mt-4 space-y-1.5">
          <div className="sam-label mb-1" style={{ fontSize: '0.78rem' }}>Loadout — count &amp; equip</div>
          {items.map((it, i) => (
            <div key={i} className="sam-well flex items-center gap-2 px-2 py-1.5">
              <span className="sam-pick-icon" aria-hidden>{iconFor(it.type)}</span>
              <span className="flex-1 min-w-0 truncate text-sm" style={{ color: 'var(--color-parchment)' }}>{it.type}</span>
              <input
                className="sam-valuebox w-12"
                type="number"
                min={1}
                value={it.count}
                onChange={(e) => setField(i, 'count', Math.max(1, Number(e.target.value) || 1))}
                aria-label={`${it.type} count`}
                title="count"
              />
              <label className="flex items-center gap-1.5 cursor-pointer" title="Equip on start (weapons/armor/rings)">
                <input type="checkbox" className="sam-check" checked={!!it.equip} onChange={(e) => setField(i, 'equip', e.target.checked)} />
                <span className="sam-label" style={{ fontSize: '0.72rem' }}>equip</span>
              </label>
              <button type="button" className="sam-step sam-remove" onClick={() => removeAt(i)} aria-label={`remove ${it.type}`}>✕</button>
            </div>
          ))}
        </div>
      )}

      {picker && (
        <ItemPickerModal
          allTypes={allTypes}
          iconFor={iconFor}
          categoryFor={categoryFor}
          categories={categories}
          onPick={pick}
          onClose={() => setPicker(null)}
        />
      )}
    </div>
  );
}
