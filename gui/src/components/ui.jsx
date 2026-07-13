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
export function GoldSlider({ label, icon, value, onChange, min = 0, max = 20 }) {
  const fill = ((value - min) / (max - min)) * 100;
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
        value={value}
        onChange={(e) => onChange(Number(e.target.value))}
      />
      <span className="sam-valuebox">{value}</span>
    </div>
  );
}

/** −/value/+ stepper row (matches Skill Levels rows). */
export function Stepper({ label, sub, value, onChange, min = 0, max = 100, step = 1 }) {
  const clamp = (v) => Math.min(max, Math.max(min, v));
  return (
    <div className="flex items-center gap-2 py-1">
      <span className="sam-label flex-1 truncate" style={{ fontSize: '0.95rem' }} title={sub}>
        {label}
      </span>
      <button type="button" className="sam-step" onClick={() => onChange(clamp(value - step))} aria-label={`decrease ${label}`}>−</button>
      <span className="sam-valuebox" style={{ minWidth: '2.6rem' }}>{value}</span>
      <button type="button" className="sam-step" onClick={() => onChange(clamp(value + step))} aria-label={`increase ${label}`}>+</button>
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
export function ItemRow({ icon = '⚔', name, count, onCount, onRemove, onClone, sub }) {
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
