/*
 * Audio building blocks shared by the Sound Editor, the Music Editor and the monster/item
 * "sounds" maps: a file list with upload + preview, the vanilla sound and music pickers, and
 * the key -> sound map editor.
 */
import { useEffect, useMemo, useRef, useState } from 'react';
import { VANILLA_SOUNDS } from '@/data/vanillaSounds.js';
import { VANILLA_MUSIC } from '@/data/vanillaMusic.js';
import {
  AUDIO_ACCEPT, AUDIO_EXT_RE, audioExt, mimeForPath, dataUrlToBlob,
  searchVanillaSounds, groupNameMeansGroup, describeSoundTarget, VANILLA_SOUND_COUNT,
  resolveVanillaSound, singleSoundTarget,
} from '@/lib/audio.js';
import { Field, TextInput, GoldButton, SearchSelect } from '@/components/ui.jsx';

let rowSeq = 0;
/** A fresh React key for a file row. */
export const newRowKey = () => `r${++rowSeq}`;

/** Two-way switch between adding something new and replacing one of the game's. */
export function ModeSwitch({ value, onChange, options }) {
  return (
    <div className="flex flex-wrap gap-2" role="radiogroup">
      {options.map((o) => (
        <GoldButton
          key={o.value}
          role="radio"
          aria-checked={value === o.value}
          tone={value === o.value ? 'green' : 'gold'}
          onClick={() => onChange(o.value)}
        >
          {value === o.value ? '●' : '○'} {o.label}
        </GoldButton>
      ))}
    </div>
  );
}

/**
 * An <audio> player for one file. Uses an object URL rather than the data URL itself: a music
 * track's data URL is tens of megabytes, and an imported asset's says octet-stream, which some
 * browsers refuse to play. The type comes from the file extension instead.
 */
export function AudioPreview({ file, dataUrl, path }) {
  const [url, setUrl] = useState(null);
  useEffect(() => {
    let u = null;
    try {
      if (file) u = URL.createObjectURL(file);
      else if (dataUrl) {
        const blob = dataUrlToBlob(dataUrl, mimeForPath(path));
        if (blob) u = URL.createObjectURL(blob);
      }
    } catch { u = null; }
    setUrl(u);
    return () => { if (u) URL.revokeObjectURL(u); };
  }, [file, dataUrl, path]);
  if (!url) return null;
  return <audio controls preload="metadata" src={url} style={{ height: 32, maxWidth: '100%' }} />;
}

const kb = (n) => (n >= 1024 * 1024 ? `${(n / (1024 * 1024)).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`);

/** Read picked files as data URLs, keeping only audio under `maxBytes`. */
function readAudioFiles(fileList, maxBytes, maxLabel) {
  const errors = [];
  const picked = [];
  for (const f of Array.from(fileList || [])) {
    if (!AUDIO_EXT_RE.test(f.name)) { errors.push({ path: f.name, message: 'not a .ogg, .wav, .mp3 or .flac file' }); continue; }
    if (f.size > maxBytes) { errors.push({ path: f.name, message: `is ${kb(f.size)} — the limit here is ${maxLabel}` }); continue; }
    picked.push(f);
  }
  return Promise.all(picked.map((f) => new Promise((resolve) => {
    const reader = new FileReader();
    reader.onload = () => resolve({ key: newRowKey(), file: f, dataUrl: String(reader.result ?? ''), name: f.name, ext: audioExt(f.name), size: f.size });
    reader.onerror = () => { errors.push({ path: f.name, message: 'could not be read' }); resolve(null); };
    reader.readAsDataURL(f);
  }))).then((rows) => ({ rows: rows.filter(Boolean), errors }));
}

/**
 * The audio files of one entry.
 *
 * rows:  [{ key, path } | { key, file, dataUrl, name, ext, size }] -- a bundled file, or one
 *        uploaded in this editor and not saved yet. `paths` (same order) is where each will be
 *        stored, worked out by the page, so this list shows the final path of a new upload too.
 * single: one file only (a music track's combat file).
 */
export function AudioFileList({
  rows, onChange, paths, assets, maxBytes, maxLabel, onErrors, single = false,
  uploadLabel = 'Upload audio', emptyText = 'No audio yet.',
}) {
  const inputRef = useRef(null);
  const [manual, setManual] = useState('');

  const onPick = async (e) => {
    // Copy the files out BEFORE clearing the input (clearing it is what lets the same file be
    // picked twice in a row); reading them afterwards finds an empty list.
    const list = Array.from(e.target.files || []);
    e.target.value = '';
    if (!list.length) return;
    const { rows: added, errors } = await readAudioFiles(single ? [list[0]] : list, maxBytes, maxLabel);
    onErrors?.(errors);
    if (added.length) onChange(single ? added.slice(0, 1) : [...rows, ...added]);
  };

  const addManual = () => {
    const p = manual.trim().replace(/\\/g, '/').replace(/^\.?\//, '');
    if (!p) return;
    if (!AUDIO_EXT_RE.test(p)) { onErrors?.([{ path: p, message: 'must end in .ogg, .wav, .mp3 or .flac' }]); return; }
    if (p.split('/').includes('..')) { onErrors?.([{ path: p, message: 'must stay inside the mod folder (no "..")' }]); return; }
    onErrors?.([]);
    onChange(single ? [{ key: newRowKey(), path: p }] : [...rows, { key: newRowKey(), path: p }]);
    setManual('');
  };

  return (
    <div className="space-y-2">
      {rows.length === 0 && <div className="text-sm" style={{ color: '#6b5a35' }}>{emptyText}</div>}
      {rows.map((r, i) => {
        const path = paths?.[i] || r.path || '';
        // A renamed entry's file is still stored under its old path until the save moves it.
        const stored = assets?.[path] || (r.path ? assets?.[r.path] : undefined);
        const moving = r.path && path && r.path !== path;
        const bundled = !!r.dataUrl || !!r.file || !!stored;
        return (
          <div key={r.key} className="sam-well px-3 py-2">
            <div className="flex items-center gap-3 flex-wrap">
              <div className="flex-1 min-w-[12rem]">
                <div className="sam-mono text-sm truncate" style={{ color: 'var(--color-parchment)' }}>{path || '(path chosen on save)'}</div>
                <div className="text-xs" style={{ color: '#6b5a35' }}>
                  {r.name ? <>from <span className="sam-mono">{r.name}</span>{r.size ? ` · ${kb(r.size)}` : ''}{r.dataUrl ? ' · not saved yet' : ''}</> : null}
                  {!r.name && bundled && !moving && 'bundled with the mod'}
                  {moving && <> · renamed from <span className="sam-mono">{r.path}</span> on save</>}
                </div>
              </div>
              <AudioPreview file={r.file} dataUrl={r.file ? null : (r.dataUrl || stored)} path={path} />
              <button type="button" className="sam-step sam-remove" onClick={() => onChange(rows.filter((_, j) => j !== i))} aria-label={`remove ${path}`}>✕</button>
            </div>
            {!bundled && (
              <div className="text-xs mt-1" style={{ color: '#d4a84b' }}>
                ⚠ Not bundled in this session (big files are not kept when the page reloads). Upload it
                again, or copy it into your mod folder at this path yourself.
              </div>
            )}
          </div>
        );
      })}
      <div className="flex items-center gap-3 flex-wrap">
        <input ref={inputRef} type="file" accept={AUDIO_ACCEPT} multiple={!single} className="hidden" onChange={onPick} />
        <GoldButton onClick={() => inputRef.current?.click()}>🎵 {uploadLabel}</GoldButton>
        <span className="text-xs" style={{ color: '#6b5a35' }}>.ogg · .wav · .mp3 · .flac — up to {maxLabel} each</span>
      </div>
      <details className="text-xs" style={{ color: '#6b5a35' }}>
        <summary className="cursor-pointer">The file is already in my mod folder…</summary>
        <div className="flex gap-2 mt-2 items-center">
          <div className="flex-1">
            <TextInput value={manual} onChange={setManual} placeholder={single ? 'music/boss_fight.ogg' : 'sounds/boom.ogg'}
              onKeyDown={(e) => e.key === 'Enter' && addManual()} aria-label="mod-relative audio path" />
          </div>
          <GoldButton onClick={addManual}>Add path</GoldButton>
        </div>
        <div className="mt-1">Mod-relative, e.g. <span className="sam-mono">sounds/boom.ogg</span>. Nothing is uploaded — put the file there yourself before you test.</div>
      </details>
    </div>
  );
}

/* ------------------------------------------------------------------ vanilla sound picker */

function PickChip({ active, onClick, children, title }) {
  return (
    <button
      type="button"
      title={title}
      onClick={onClick}
      className="px-2 py-0.5 text-xs sam-mono cursor-pointer"
      style={{
        border: `1px solid ${active ? 'var(--color-gold)' : '#4a3617'}`,
        background: active ? 'rgba(212,168,75,0.18)' : 'transparent',
        color: active ? 'var(--color-gold-bright)' : 'var(--color-parchment)',
        borderRadius: 3,
      }}
    >{children}</button>
  );
}

/**
 * Search the game's sounds and pick one. The common ones are listed first with what they are;
 * typing searches every group and file name (or an index). Picking a group's name replaces
 * the whole group; picking one file replaces just that file (stored by number when its name
 * is also its group's, as "Casting" is).
 */
export function VanillaSoundPicker({ value, onPick, initialQuery = '' }) {
  // Start on a hint (the monster's base creature) only if it finds something; "human" has no
  // sounds of its own, and an empty result is a worse start than the common list.
  const [q, setQ] = useState(() => (initialQuery && searchVanillaSounds(initialQuery).total ? initialQuery : ''));
  const { groups, total } = useMemo(() => searchVanillaSounds(q), [q]);
  const current = String(value ?? '').toLowerCase();
  const isCurrent = (name) => name.toLowerCase() === current;

  return (
    <div className="sam-well p-3">
      <TextInput value={q} onChange={setQ}
        placeholder={`Search the game's ${VANILLA_SOUND_COUNT} sounds — e.g. swing, door, goblin, 25`}
        aria-label="search vanilla sounds" />
      <div className="mt-2 overflow-y-auto" style={{ maxHeight: 300 }}>
        {!q.trim() ? (
          <>
            <div className="sam-label mb-1">Most asked for</div>
            <div className="space-y-1">
              {VANILLA_SOUNDS.common.map((c) => (
                <button key={c.name} type="button" onClick={() => onPick(c.name)}
                  className="w-full text-left px-2 py-1 text-sm cursor-pointer hover:bg-[rgba(212,168,75,0.12)]"
                  style={{ color: 'var(--color-parchment)', border: `1px solid ${isCurrent(c.name) ? 'var(--color-gold)' : 'transparent'}`, borderRadius: 3 }}>
                  <span className="sam-mono" style={{ color: 'var(--color-gold)' }}>{c.name}</span>
                  <span style={{ color: '#8a7749' }}> — {c.what}</span>
                  <span className="text-xs" style={{ color: '#6b5a35' }}> · {describeSoundTarget(c.name)?.label}</span>
                </button>
              ))}
            </div>
            <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>Not here? Type to search all of them.</div>
          </>
        ) : groups.length === 0 ? (
          <div className="text-sm" style={{ color: '#6b5a35' }}>Nothing matches “{q}”.</div>
        ) : (
          <div className="space-y-2">
            {groups.map((g) => {
              // The engine's count: groups that differ only in case ("Zap", "zap") are one group.
              const whole = groupNameMeansGroup(g.group);
              const soundsInGroup = whole ? resolveVanillaSound(g.group).sounds.length : 1;
              return (
                <div key={g.group} className="px-2 py-1" style={{ borderLeft: '2px solid #4a3617' }}>
                  <div className="flex items-center gap-2 flex-wrap">
                    {whole ? (
                      <PickChip active={isCurrent(g.group)} onClick={() => onPick(g.group)} title="Replace every sound in this group">
                        {g.group} — whole group ({soundsInGroup})
                      </PickChip>
                    ) : (
                      <span className="sam-mono text-xs" style={{ color: '#8a7749' }}>{g.group}</span>
                    )}
                  </div>
                  <div className="flex flex-wrap gap-1 mt-1">
                    {g.sounds.map((s) => {
                      // "Casting" alone is stored as its number: the name means the whole group.
                      const target = singleSoundTarget(s);
                      return (
                        <PickChip key={s.i} active={isCurrent(target)} onClick={() => onPick(target)} title={`#${s.i} — replace just this one`}>
                          {s.name}
                        </PickChip>
                      );
                    })}
                  </div>
                </div>
              );
            })}
            {total > groups.length && (
              <div className="text-xs" style={{ color: '#6b5a35' }}>…{total - groups.length} more — type more to narrow it down.</div>
            )}
          </div>
        )}
      </div>
    </div>
  );
}

/* ------------------------------------------------------------------ vanilla music picker */

/** Pick one of the game's replaceable tracks (with what each one is), or type an exact name. */
export function VanillaMusicPicker({ value, onPick }) {
  const [q, setQ] = useState('');
  const list = useMemo(() => {
    const t = q.trim().toLowerCase();
    return t ? VANILLA_MUSIC.filter((m) => m.name.toLowerCase().includes(t) || m.what.toLowerCase().includes(t)) : VANILLA_MUSIC;
  }, [q]);
  const current = String(value ?? '').toLowerCase();
  return (
    <div className="sam-well p-3">
      <TextInput value={q} onChange={setQ} placeholder="Search — e.g. mines, shop, boss, menu" aria-label="search vanilla music" />
      <div className="mt-2 overflow-y-auto space-y-1" style={{ maxHeight: 300 }}>
        {list.map((m) => (
          <button key={m.name} type="button" onClick={() => onPick(m.name)}
            className="w-full text-left px-2 py-1 text-sm cursor-pointer hover:bg-[rgba(212,168,75,0.12)]"
            style={{ color: 'var(--color-parchment)', border: `1px solid ${m.name.toLowerCase() === current ? 'var(--color-gold)' : 'transparent'}`, borderRadius: 3 }}>
            <span className="sam-mono" style={{ color: 'var(--color-gold)' }}>{m.name}</span>
            <span style={{ color: '#8a7749' }}> — {m.what}</span>
          </button>
        ))}
        {list.length === 0 && <div className="text-sm" style={{ color: '#6b5a35' }}>Nothing matches “{q}”.</div>}
      </div>
    </div>
  );
}

/* ------------------------------------------------------------------ key -> sound map */

/**
 * A monster's or item's "sounds": each row says "when the game would play THIS vanilla sound,
 * play THAT one of mine instead". rows: [{ key, from, to }].
 */
export function SoundMapEditor({ rows, onChange, ownSounds, initialQuery = '', hint }) {
  const [picking, setPicking] = useState(null); // row key whose vanilla picker is open
  const set = (k, patch) => onChange(rows.map((r) => (r.key === k ? { ...r, ...patch } : r)));
  return (
    <div className="space-y-2">
      {hint && <div className="text-xs" style={{ color: '#6b5a35' }}>{hint}</div>}
      {rows.length === 0 && <div className="text-sm" style={{ color: '#6b5a35' }}>No sounds changed — it plays the base game's.</div>}
      {rows.map((r) => {
        const target = describeSoundTarget(r.from);
        return (
          <div key={r.key} className="sam-well p-2">
            <div className="flex items-end gap-2 flex-wrap">
              <Field label="Instead of (game sound)" className="flex-1 min-w-[10rem]">
                <div className="flex gap-1">
                  <div className="flex-1"><TextInput value={r.from} onChange={(v) => set(r.key, { from: v })} placeholder="SwingWeapon" /></div>
                  <GoldButton onClick={() => setPicking(picking === r.key ? null : r.key)} title="Pick from the game's sounds">
                    {picking === r.key ? '▾' : '🔍'}
                  </GoldButton>
                </div>
              </Field>
              <Field label="Play (your sound)" className="flex-1 min-w-[10rem]">
                <SearchSelect options={ownSounds} value={r.to} onPick={(v) => set(r.key, { to: v })}
                  placeholder={ownSounds.length ? 'pick one of your sounds' : 'make one in the Sound Editor'} allowCustom />
              </Field>
              <button type="button" className="sam-step sam-remove mb-[6px]" onClick={() => onChange(rows.filter((x) => x.key !== r.key))} aria-label="remove sound mapping">✕</button>
            </div>
            {r.from.trim() && (
              <div className="text-xs mt-1" style={{ color: target ? '#6b5a35' : '#d4a84b' }}>
                {target ? `replaces ${target.label}` : `⚠ the game has no sound or group called “${r.from.trim()}”`}
              </div>
            )}
            {picking === r.key && (
              <div className="mt-2">
                <VanillaSoundPicker value={r.from} initialQuery={r.from || initialQuery}
                  onPick={(name) => { set(r.key, { from: name }); setPicking(null); }} />
              </div>
            )}
          </div>
        );
      })}
      <GoldButton onClick={() => onChange([...rows, { key: newRowKey(), from: '', to: '' }])}>+ Change a sound</GoldButton>
    </div>
  );
}

/** { SwingWeapon: 'mymod:whoosh' } -> editor rows. */
export function soundMapToRows(map) {
  return Object.entries(map || {}).map(([from, to]) => ({ key: newRowKey(), from, to: String(to ?? '') }));
}
/** Editor rows -> { SwingWeapon: 'mymod:whoosh' } (blank rows dropped), or undefined when empty. */
export function rowsToSoundMap(rows) {
  const out = {};
  for (const r of rows) if (r.from.trim() && r.to.trim()) out[r.from.trim()] = r.to.trim();
  return Object.keys(out).length ? out : undefined;
}
