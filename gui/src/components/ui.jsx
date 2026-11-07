/*
 * S.A.M GUI — shared medieval design-system components.
 * Every page composes ONLY these + tailwind layout utilities, so the whole
 * app inherits the reference look from one place.
 */
import { useEffect, useMemo, useRef, useState } from 'react';
import { searchMatches, enterPick, offerTyped } from '@/lib/searchSelect.js';

/** A framed window opened by a crimson option band, a fleur-de-lis either side of the title. */
export function Panel({ title, children, className = '', bodyClassName = '' }) {
  return (
    <section className={`sam-panel ${className}`}>
      {title && (
        <header className="sam-panel-header">
          <span className="sam-fleur" aria-hidden />
          <span>{title}</span>
          <span className="sam-fleur" aria-hidden />
        </header>
      )}
      <div className={`p-4 ${bodyClassName}`}>{children}</div>
    </section>
  );
}

/**
 * A drawn icon from scripts/pixel-art.mjs (`--icon-<name>`): the attributes (str dex con int per chr),
 * the skills (sword axe mace polearm bow shield stealth key lens coin crown orb sparkle amulet flask)
 * and a few marks (hand, mana). 20px is 2px a pixel; keep sizes to multiples of 10.
 */
export function PixelIcon({ name, size = 20, className = '', title }) {
  if (!name) return null;
  return (
    <span
      className={`sam-picon ${className}`}
      style={{ width: size, height: size, backgroundImage: `var(--icon-${name})`, backgroundSize: `${size}px ${size}px` }}
      aria-hidden={title ? undefined : true}
      title={title}
    />
  );
}

/** One of Barony's three gems leading a state line: ruby an error, sapphire advice, emerald done. */
export function Gem({ kind = 'sapphire' }) {
  return <span className={`sam-gem sam-gem-sm sam-gem-${kind}`} aria-hidden />;
}

/** Small-caps label + arbitrary control. */
export function Field({ label, children, hint, className = '' }) {
  return (
    <label className={`block ${className}`}>
      <div className="sam-label mb-1">{label}</div>
      {children}
      {hint && <div className="mt-1 text-xs" style={{ color: 'var(--color-ink-faint)' }}>{hint}</div>}
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
/** −/value/+ stepper row (matches Skill Levels rows). The ± buttons nudge by `step`, but
 *  the value box is editable so you can type an EXACT number — a class can start with
 *  Swords 3, not just multiples of 5. */
export function Stepper({ label, icon, sub, value, onChange, min = 0, max = 100, step = 1 }) {
  const clamp = (v) => Math.min(max, Math.max(min, v));
  return (
    <div className="flex items-center gap-2 py-1">
      {icon && <PixelIcon name={icon} />}
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

  const matches = useMemo(() => searchMatches(options, query, maxShown), [options, query, maxShown]);
  // "Use what I typed" is offered whenever free values are allowed and the text is not an option,
  // not only when nothing matches: a custom value can be part of an existing option's name.
  const showTyped = offerTyped(options, query, allowCustom);

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
          if (e.key === 'Enter') {
            // An exact option first, then the first match, then (free values allowed) the typed text.
            const pick = enterPick(options, matches, query, allowCustom);
            if (pick !== null) onPick(pick);
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
          {showTyped && (
            <div
              role="option"
              className="px-3 py-1.5 cursor-pointer text-sm hover:bg-[rgba(226,176,78,0.12)]"
              style={{ color: 'var(--color-parchment)' }}
              onMouseDown={() => { onPick(query.trim()); setOpen(false); }}
            >
              Use “<span className="sam-mono">{query.trim()}</span>” <span style={{ color: 'var(--color-ink-faint)' }}>(custom)</span>
            </div>
          )}
          {matches.length === 0 && !showTyped && (
            <div className="px-3 py-2 text-sm" style={{ color: 'var(--color-ink-faint)' }}>No matches</div>
          )}
          {matches.map((o) => (
            <div
              key={o}
              role="option"
              aria-selected={o === value}
              className="px-3 py-1.5 cursor-pointer text-sm hover:bg-[rgba(226,176,78,0.12)]"
              style={{ color: 'var(--color-parchment)' }}
              onMouseDown={() => { onPick(o); setOpen(false); }}
            >
              {o}
            </div>
          ))}
          {matches.length === maxShown && (
            <div className="px-3 py-1 text-xs" style={{ color: 'var(--color-ink-faint)' }}>
              …type to narrow further
            </div>
          )}
        </div>
      )}
    </div>
  );
}

/** Row in a Starting Items-style list: name, count box, optional clone, red X. */
export function ItemRow({ icon = null, name, count, onCount, onRemove, onClone, onEdit, sub }) {
  return (
    <div className="sam-well flex items-center gap-3 px-3 py-2">
      <span className="sam-ico sam-ico-grip select-none cursor-grab" aria-hidden />
      {icon && <span className="sam-row-icon" aria-hidden>{icon}</span>}
      <div className="flex-1 min-w-0">
        <div className="truncate" style={{ color: 'var(--color-parchment)' }}>{name}</div>
        {sub && <div className="text-xs truncate" style={{ color: 'var(--color-ink-faint)' }}>{sub}</div>}
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
        <button type="button" className="sam-step" onClick={onEdit} title="Edit" aria-label={`edit ${name}`}><span className="sam-ico sam-ico-edit" aria-hidden /></button>
      )}
      {onClone && (
        <button type="button" className="sam-step" onClick={onClone} title="Duplicate" aria-label={`duplicate ${name}`}><span className="sam-ico sam-ico-copy" aria-hidden /></button>
      )}
      <button type="button" className="sam-step sam-remove" onClick={onRemove} aria-label={`remove ${name}`}>✕</button>
    </div>
  );
}

/** Advisory balance-hint panel — amber, never blocks save. Renders null when empty. */
export function BalanceHints({ hints }) {
  if (!hints || hints.length === 0) return null;
  return (
    <div className="sam-well sam-well-advice p-3">
      <div className="sam-label mb-2 flex items-center gap-2">
        <span className="sam-gem sam-gem-sm sam-gem-sapphire" aria-hidden /> Balance hints
      </div>
      <ul className="space-y-1">
        {hints.map((h, i) => (
          <li key={i} className="text-sm" style={{ color: h.level === 'warn' ? 'var(--color-gold)' : 'var(--color-parchment)' }}>
            {h.message}
          </li>
        ))}
      </ul>
      <div className="text-xs mt-2" style={{ color: 'var(--color-ink-faint)' }}>advisory only — these never block saving or exporting</div>
    </div>
  );
}

/** Validation error list (shared by editors + validator page). */
export function ErrorList({ errors, className = '' }) {
  if (!errors || errors.length === 0) return null;
  return (
    <div className={`sam-well p-3 ${className}`}>
      <div className="sam-label mb-2 flex items-center gap-2" style={{ color: 'var(--color-ruby-text)' }}>
        <span className="sam-gem sam-gem-sm sam-gem-ruby" aria-hidden /> Validation errors
      </div>
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
  return (
    <div className="sam-ok text-sm mt-2 flex items-center gap-2">
      <span className="sam-gem sam-gem-sm sam-gem-emerald" aria-hidden />
      <span>{children}</span>
    </div>
  );
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
export function ItemPickerModal({ allTypes, iconFor, categoryFor, categories, onPick, onClose, title = 'Choose an Item' }) {
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
          <span className="sam-fleur" aria-hidden />
          <span>{title}</span>
          <span className="sam-fleur" aria-hidden />
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
        <div className="flex-1 overflow-y-auto" style={{ borderTop: '1px solid var(--color-edge)' }}>
          {total === 0 ? (
            <div className="px-3 py-6 text-sm text-center" style={{ color: 'var(--color-ink-faint)' }}>
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
                    <span className="text-xs shrink-0" style={{ color: 'var(--color-ink-faint)' }}>{nice(cat)}</span>
                  </div>
                ))}
              </div>
            ))
          )}
        </div>
        <div className="px-3 py-2 flex items-center justify-between" style={{ borderTop: '1px solid var(--color-edge)' }}>
          <span className="text-xs" style={{ color: 'var(--color-ink-faint)' }}>{total} shown · Esc to close</span>
          <GoldButton onClick={onClose}>Close</GoldButton>
        </div>
      </div>
    </div>
  );
}

