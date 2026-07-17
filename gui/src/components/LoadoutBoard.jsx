/*
 * Starting Loadout editor — Barony's own inventory screen, for authoring a class's kit.
 *
 * A captioned paperdoll of the ten equipment slots flanks the character; a backpack grid
 * holds the rest. Clicking a slot opens the item picker FILTERED to that slot (you cannot
 * equip a helmet to the boots slot — the engine routes an equipped item to its natural slot
 * anyway, so the board only ever offers a slot the items that fit it). Selecting any item
 * opens an inline detail strip whose centerpiece is the blessing dial: the whole point of
 * this redesign is that a modder sets a "+2 blessed" or "cursed" starting item right here,
 * in the game's own language, instead of hand-typing a beatitude integer into JSON.
 *
 * State is ONE flat array of entries that maps 1:1 to starting_items. The two regions are
 * derived: equipped = entry.equip (placed in its slot), backpack = everything else. `_key`
 * and `_slot` are UI-only and stripped on save (see ClassEditor buildDef).
 */
import { useState } from 'react';
import { Panel, Select, NumberInput, ItemPickerModal } from '@/components/ui.jsx';
import {
  EQUIP_SLOTS, equipSlotOf, itemsForSlot, findSlot,
  BLESSING_MIN, BLESSING_MAX, blessingLabel, STATUSES, newEntry,
  statusStyleFor, statusWord,
} from '@/data/equipment.js';

/** Which paperdoll slot an entry occupies: its item's natural slot, or its stored hint. */
const slotOfEntry = (it) => equipSlotOf(it.type) || it._slot || null;

/** One 64px cell: filled item (icon + blessing/count/condition badges + remove) or empty. */
function Cell({ entry, ghostIcon, label, selected, onClick, onRemove }) {
  if (!entry) {
    return (
      <button type="button" className="sam-slot sam-slot-empty" title={label ? `Add ${label}` : 'Add an item'} onClick={onClick}>
        <span className="sam-slot-ghost" aria-hidden>{ghostIcon || '+'}</span>
      </button>
    );
  }
  const b = Number(entry.beatitude) || 0;
  return (
    <div
      role="button" tabIndex={0}
      className={`sam-slot ${selected ? 'selected' : ''} ${b > 0 ? 'is-blessed' : b < 0 ? 'is-cursed' : ''}`}
      title={`${entry.type} — click to edit`}
      onClick={onClick}
      onKeyDown={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onClick(); } }}
    >
      <span className={`sam-slot-ico ${entry.identified ? '' : 'sam-slot-unid'}`} aria-hidden>{ghostIcon}</span>
      {b !== 0 && <span className={`sam-slot-bless ${b > 0 ? 'bless-pos' : 'bless-neg'}`}>{b > 0 ? `+${b}` : b}</span>}
      {entry.count > 1 && <span className="sam-slot-count">×{entry.count}</span>}
      {!entry.identified && <span className="sam-slot-pip" title="unidentified">?</span>}
      <span className="sam-slot-x" title="Remove" onClick={(e) => { e.stopPropagation(); onRemove(); }}>✕</span>
    </div>
  );
}

/** The paperdoll column of equip slots (each captioned so a naked board still reads clearly). */
function EquipColumn({ slots, items, iconFor, selectedKey, onPick, onSelect, onRemove }) {
  return (
    <div className="sam-paperdoll-col">
      {slots.map((slot) => {
        const entry = items.find((it) => it.equip && slotOfEntry(it) === slot.id);
        return (
          <div key={slot.id} className="sam-slot-wrap">
            <div className="sam-slot-label">{slot.label}</div>
            <Cell
              entry={entry}
              ghostIcon={entry ? iconFor(entry.type) : slot.icon}
              label={slot.label}
              selected={entry && entry._key === selectedKey}
              onClick={() => (entry ? onSelect(entry._key) : onPick(slot))}
              onRemove={() => entry && onRemove(entry._key)}
            />
          </div>
        );
      })}
    </div>
  );
}

/** The 7-stop blessing dial: the marquee control. Speaks Barony's "+2 / cursed" language. */
function BlessingDial({ value, onChange }) {
  const [manual, setManual] = useState(false);
  const b = Number(value) || 0;
  const stops = [];
  for (let i = BLESSING_MIN; i <= BLESSING_MAX; i++) stops.push(i);
  const inRange = b >= BLESSING_MIN && b <= BLESSING_MAX;
  return (
    <div>
      <div className="sam-label mb-1" style={{ fontSize: '0.72rem' }}>Blessing</div>
      {manual || !inRange ? (
        <div className="flex items-center gap-2">
          <NumberInput value={b} onChange={(v) => onChange(Math.trunc(Number(v) || 0))} className="w-20" aria-label="beatitude" />
          <button type="button" className="sam-linkish" onClick={() => setManual(false)}>use dial</button>
        </div>
      ) : (
        <div className="flex items-center gap-2">
          <div className="sam-beatitude" role="group" aria-label="blessing">
            {stops.map((n) => (
              <button
                key={n}
                type="button"
                className={`sam-bstop ${n < 0 ? 'neg' : n > 0 ? 'pos' : 'zero'} ${n === b ? 'on' : ''}`}
                onClick={() => onChange(n)}
                title={blessingLabel(n)}
              >{n > 0 ? `+${n}` : n}</button>
            ))}
          </div>
          <button type="button" className="sam-linkish" title="a value beyond ±3" onClick={() => setManual(true)}>±</button>
        </div>
      )}
      <div className="text-xs mt-1" style={{ color: b > 0 ? 'var(--color-gold-bright)' : b < 0 ? 'var(--color-blood)' : '#8a7749' }}>
        {blessingLabel(b)}
      </div>
    </div>
  );
}

/** The inline detail strip: blessing + status + identified + count + hotbar, and a live read-out. */
function DetailStrip({ entry, patch, onHotbar, iconFor, onRemove, category }) {
  const equipped = entry.equip;
  const style = statusStyleFor(category);              // category-appropriate word for Status
  const word = statusWord(category, entry.status);     // e.g. potion -> "bubbly"
  const bWord = (Number(entry.beatitude) || 0) !== 0 ? `${blessingLabel(entry.beatitude).toLowerCase()} ` : '';
  const plain = `${bWord}${word} ${entry.type}${entry.identified ? '' : ', unidentified'}`;
  return (
    <div className="sam-well sam-detailstrip">
      <div className="flex items-center gap-2 mb-3">
        <span className="sam-pick-icon" aria-hidden>{iconFor(entry.type)}</span>
        <span className="flex-1 min-w-0 truncate" style={{ color: 'var(--color-parchment)', fontWeight: 600 }}>{entry.type}</span>
        <span className="text-xs" style={{ color: '#8a7749' }}>{equipped ? 'equipped' : 'in backpack'}</span>
        <button type="button" className="sam-step sam-remove" onClick={onRemove} aria-label="remove">✕</button>
      </div>
      <div className="sam-detailgrid">
        <BlessingDial value={entry.beatitude} onChange={(v) => patch({ beatitude: v })} />
        <div>
          <div className="sam-label mb-1" style={{ fontSize: '0.72rem' }} title={style.hint || ''}>{style.label}</div>
          <Select value={entry.status} onChange={(v) => patch({ status: v })}
            options={STATUSES.map((s, i) => ({ value: s, label: style.words[i][0].toUpperCase() + style.words[i].slice(1) }))} />
        </div>
        <div>
          <div className="sam-label mb-1" style={{ fontSize: '0.72rem' }}>Count</div>
          <NumberInput value={entry.count} min={1} disabled={equipped}
            onChange={(v) => patch({ count: Math.max(1, Number(v) || 1) })}
            title={equipped ? 'equipped gear starts as one piece' : 'stack size'} />
        </div>
        <label className="flex items-end gap-2 pb-1 cursor-pointer" style={{ minHeight: '2.4rem' }}>
          <input type="checkbox" className="sam-check" checked={!!entry.identified} onChange={(e) => patch({ identified: e.target.checked })} />
          <span className="sam-label" style={{ fontSize: '0.72rem' }}>Identified</span>
        </label>
        {!equipped && (
          <div>
            <div className="sam-label mb-1" style={{ fontSize: '0.72rem' }}>Hotbar</div>
            <Select value={String(entry.hotbar_slot)} onChange={(v) => onHotbar(Number(v))}
              options={[{ value: '-1', label: 'in pack' }, ...Array.from({ length: 10 }, (_, i) => ({ value: String(i), label: `slot ${i === 9 ? 0 : i + 1}` }))]} />
          </div>
        )}
      </div>
      <div className="text-xs mt-3" style={{ color: '#8a7749' }}>In game: <span style={{ color: 'var(--color-parchment)' }}>{plain}</span></div>
    </div>
  );
}

export default function LoadoutBoard({ items, onChange, allTypes, iconFor, categoryFor, categories, portraitUrl }) {
  const [picker, setPicker] = useState(null); // { slot } for equip, { backpack:true } for pack
  const [selectedKey, setSelectedKey] = useState(null);

  const isPinned = (it) => !it.equip && Number(it.hotbar_slot) >= 0;
  const backpack = items.filter((it) => !it.equip && !isPinned(it)); // loose (unpinned) items
  const hotbarItem = (n) => items.find((it) => isPinned(it) && Number(it.hotbar_slot) === n);
  const selected = items.find((it) => it._key === selectedKey);

  const patch = (key, fields) => onChange(items.map((it) => (it._key === key ? { ...it, ...fields } : it)));
  const remove = (key) => { onChange(items.filter((it) => it._key !== key)); if (key === selectedKey) setSelectedKey(null); };

  /** Pin an item to a hotbar slot (or -1 to send it back to the loose backpack), demoting
   *  whatever held that slot so two starting items never fight over one hotbar key. */
  const setHotbar = (key, n) => onChange(items.map((it) => {
    if (it._key === key) return { ...it, equip: false, hotbar_slot: n, _slot: undefined };
    if (n >= 0 && isPinned(it) && Number(it.hotbar_slot) === n) return { ...it, hotbar_slot: -1 };
    return it;
  }));

  const pick = (type) => {
    if (!picker) return;
    if (picker.hotbar != null) {
      const n = picker.hotbar;
      const demoted = items.map((it) => (isPinned(it) && Number(it.hotbar_slot) === n ? { ...it, hotbar_slot: -1 } : it));
      const e = { ...newEntry(type, false), hotbar_slot: n };
      onChange([...demoted, e]);
      setSelectedKey(e._key);
    } else if (picker.backpack) {
      const existing = backpack.find((it) => it.type === type);
      if (existing) patch(existing._key, { count: existing.count + 1 });
      else { const e = newEntry(type, false); onChange([...items, e]); setSelectedKey(e._key); }
    } else {
      const slot = picker.slot;
      // Replace-and-demote: an item already in this slot drops to the backpack, never lost.
      const demoted = items.map((it) =>
        it.equip && slotOfEntry(it) === slot.id ? { ...it, equip: false, _slot: undefined } : it);
      const e = newEntry(type, true, slot.id);
      onChange([...demoted, e]);
      setSelectedKey(e._key);
    }
    setPicker(null);
  };

  const left = EQUIP_SLOTS.filter((s) => s.col === 'left');
  const right = EQUIP_SLOTS.filter((s) => s.col === 'right');
  const bottom = EQUIP_SLOTS.filter((s) => s.col === 'bottom');

  // Items whose equip:true slot collided on load (two rings, etc.): show a rescue tray.
  const placed = new Set();
  const conflicts = [];
  for (const it of items.filter((x) => x.equip)) {
    const s = slotOfEntry(it);
    // No resolvable slot (a custom item reloaded without its UI hint) or a slot already
    // taken (two rings): can't be shown on the paperdoll, so surface it in the rescue tray.
    if (!s || placed.has(s)) conflicts.push(it); else placed.add(s);
  }

  const pickerTypes = (picker?.backpack || picker?.hotbar != null) ? allTypes
    : picker ? [...itemsForSlot(picker.slot.id, allTypes), ...allTypes.filter((t) => t.includes(':'))]
    : allTypes;

  return (
    <Panel title="Starting Loadout">
      <div className="sam-loadout">
        {/* PAPERDOLL */}
        <div className="sam-paperdoll">
          <div className="sam-paperdoll-body">
            <EquipColumn slots={left} items={items} iconFor={iconFor} selectedKey={selectedKey}
              onPick={(s) => setPicker({ slot: s })} onSelect={setSelectedKey} onRemove={remove} />
            <div className="sam-paperdoll-center">
              {portraitUrl
                ? <img src={portraitUrl} alt="" className="sam-paperdoll-portrait" />
                : <span className="sam-paperdoll-glyph" aria-hidden>🧍</span>}
            </div>
            <EquipColumn slots={right} items={items} iconFor={iconFor} selectedKey={selectedKey}
              onPick={(s) => setPicker({ slot: s })} onSelect={setSelectedKey} onRemove={remove} />
          </div>
          <div className="sam-paperdoll-bottom">
            <EquipColumn slots={bottom} items={items} iconFor={iconFor} selectedKey={selectedKey}
              onPick={(s) => setPicker({ slot: s })} onSelect={setSelectedKey} onRemove={remove} />
          </div>
        </div>

        {/* BACKPACK + HOTBAR */}
        <div className="sam-loadout-right">
          <div className="sam-well sam-backpack">
            <div className="sam-label mb-2" style={{ fontSize: '0.78rem' }}>Backpack · {backpack.length}</div>
            <div className="sam-backpack-grid">
              {backpack.map((it) => (
                <Cell key={it._key} entry={it} ghostIcon={iconFor(it.type)}
                  selected={it._key === selectedKey}
                  onClick={() => setSelectedKey(it._key)} onRemove={() => remove(it._key)} />
              ))}
              <button type="button" className="sam-slot sam-slot-empty" title="Stash an item" onClick={() => setPicker({ backpack: true })}>
                <span className="sam-slot-ghost" aria-hidden>+</span>
              </button>
            </div>
            {backpack.length === 0 && (
              <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>Backpack empty — click + to stash starting supplies.</div>
            )}
          </div>

          <div className="sam-well sam-hotbar-panel">
            <div className="sam-label mb-2" style={{ fontSize: '0.78rem' }}>Hotbar</div>
            <div className="sam-hotbar">
              {Array.from({ length: 10 }, (_, n) => {
                const it = hotbarItem(n);
                return (
                  <div key={n} className="sam-slot-wrap">
                    <div className="sam-slot-label">{n === 9 ? 0 : n + 1}</div>
                    <Cell
                      entry={it}
                      ghostIcon={it ? iconFor(it.type) : ''}
                      selected={it && it._key === selectedKey}
                      onClick={() => (it ? setSelectedKey(it._key) : setPicker({ hotbar: n }))}
                      onRemove={() => it && remove(it._key)}
                    />
                  </div>
                );
              })}
            </div>
            <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>Numbered quick-use slots. Click one to pin a starting item, or set an item's Hotbar in its details. A pinned item still counts as in the pack.</div>
          </div>
        </div>
      </div>

      {conflicts.length > 0 && (
        <div className="sam-well mt-3" style={{ borderColor: 'var(--color-blood)' }}>
          <div className="sam-error text-sm mb-2">⚠ {conflicts.length} item(s) want a slot that's already taken (Barony has one of each). Move them to the backpack:</div>
          <div className="flex flex-wrap gap-2">
            {conflicts.map((it) => (
              <button key={it._key} type="button" className="sam-btn" style={{ padding: '0.2rem 0.5rem', fontSize: '0.78rem' }}
                onClick={() => patch(it._key, { equip: false, _slot: undefined })}>
                {iconFor(it.type)} {it.type} → backpack
              </button>
            ))}
          </div>
        </div>
      )}

      {/* DETAIL STRIP */}
      {selected
        ? <div className="mt-4"><DetailStrip entry={selected} patch={(f) => patch(selected._key, f)} onHotbar={(n) => setHotbar(selected._key, n)} iconFor={iconFor} category={categoryFor(selected.type)} onRemove={() => remove(selected._key)} /></div>
        : <div className="text-sm mt-4 text-center" style={{ color: '#6b5a35' }}>Click a slot to gear up, or click an item to set its blessing.</div>}

      {picker && (
        <ItemPickerModal
          allTypes={pickerTypes}
          iconFor={iconFor}
          categoryFor={categoryFor}
          categories={categories}
          title={picker.hotbar != null ? `Hotbar Slot ${picker.hotbar === 9 ? 0 : picker.hotbar + 1}`
            : picker.backpack ? 'Stash an Item'
            : `Choose ${findSlot(picker.slot.id)?.label} Gear`}
          onPick={pick}
          onClose={() => setPicker(null)}
        />
      )}
    </Panel>
  );
}
