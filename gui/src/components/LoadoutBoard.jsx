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
 * State is ONE flat array of entries that maps 1:1 to starting_items. The regions are
 * derived: equipped = entry.equip (placed in its slot), hotbar = entry.hotbar_slot >= 0,
 * backpack = everything that is neither. An item can be equipped AND pinned to a hotbar key
 * (the Wizard's staff starts in hand and on key 1); the engine does both for one entry. `_key`
 * and `_slot` are UI-only and stripped on save (see ClassEditor buildDef).
 */
import { useMemo, useState } from 'react';
import { Panel, Select, NumberInput, ItemPickerModal, PixelIcon, Gem, ItemIcon } from '@/components/ui.jsx';
import { itemIconUrl, SLOT_ICON_FILE, kindIconFile, CATEGORY_ICON_FILE } from '@/data/fallbackIcons.js';
import { ITEM_ICONS } from '@/data/itemIcons.js';
import { ITEM_TYPES, FRAMEWORK_ITEMS } from '@/data/schemas.js';
import { looksOf } from '@/data/itemLooks.js';
import {
  EQUIP_SLOTS, equipSlotOf, itemsForSlot, findSlot,
  BLESSING_MIN, BLESSING_MAX, blessingLabel, STATUSES, newEntry,
  statusStyleFor, statusWord, entryFromJson,
} from '@/data/equipment.js';
import { BOOKS, bookTitle } from '@/data/books.js';

/* Real in-game icon URL for an ItemType, or null if we don't have one.
 * import.meta.env.BASE_URL is '/' in dev and '/SAM-Framework/' in the Pages build. */
function iconSrc(type) {
  const file = ITEM_ICONS[type];
  return file ? `${import.meta.env.BASE_URL}item-icons/${file}` : null;
}

/* What the grid + picker render for an item: its own Barony PNG, or else the icon of an item of
 * the same kind (data/fallbackIcons.js). */
export function itemIcon(type) {
  return <ItemIcon src={iconSrc(type) ?? itemIconUrl(kindIconFile(type))} />;
}

/* Group an ItemType into one of the schema's categories for the picker.
 * Barony names items by category prefix (POTION_, SCROLL_, TOOL_, …); the rest
 * (weapons/armor named by material+type) fall to keyword matching. Heuristic —
 * only drives grouping in the picker, so the odd edge case is harmless. */
export function itemCategory(t) {
  if (/^POTION_/.test(t)) return 'POTION';
  if (/^SCROLL_/.test(t)) return 'SCROLL';
  if (/^SPELLBOOK_/.test(t)) return 'SPELLBOOK';
  if (/^TOME_/.test(t)) return 'TOME_SPELL';
  if (/^MAGICSTAFF_/.test(t)) return 'MAGICSTAFF';
  if (/^RING_/.test(t)) return 'RING';
  if (/^AMULET_/.test(t)) return 'AMULET';
  if (/^GEM_/.test(t)) return 'GEM';
  if (/^FOOD_/.test(t)) return 'FOOD';
  if (/^TOOL_|^KEY_|^INSTRUMENT_/.test(t)) return 'TOOL';
  if (t === 'SPELL_ITEM') return 'SPELL_CAT';
  if (t === 'READABLE_BOOK') return 'BOOK';
  if (/TOMAHAWK|CHAKRAM|SHURIKEN|BOOMERANG|BOLAS|PLUMBATA|DART|GREASE_BALL|DUST_BALL|SLOP_BALL|THROWING/.test(t)) return 'THROWN';
  if (/SWORD|DAGGER|RAPIER|CLAYMORE|ANELACE|FALSHION|AXE|MACE|FLAIL|SHILLELAGH|KNUCKLES|SPEAR|HALBERD|TRIDENT|LANCE|GLAIVE|POLEARM|BOW|CROSSBOW|SLING|QUIVER|KNIFE|SCEPTER/.test(t)) return 'WEAPON';
  if (/SHIELD|SCUTUM|BREASTPIECE|DOUBLET|TUNIC|GAMBESON|HAUBERK|ROBE|SHAWL|HELM|HAT|HOOD|MASK|CAP|COIF|CROWN|CIRCLET|LAURELS|TURBAN|HEADDRESS|MITER|BOOTS|LOAFERS|CLEAT|GLOVES|GAUNTLETS|BRACERS|CLOAK|PAULDRONS|APRON|VISOR/.test(t)) return 'ARMOR';
  return 'TOOL';
}

// A custom item declares a `category` (WEAPON/ARMOR/POTION/…); show the icon of a vanilla item of
// that category so it reads in the loadout picker even though it has no PNG of its own.
export const customIconSrc = (it) => itemIconUrl((it && CATEGORY_ICON_FILE[it.category]) || 'tinker_toolkit.png');

// A custom item's `slot` (EQUIPPABLE_IN_SLOT_*) → the loadout paperdoll slot id, so a
// one-click add can EQUIP a weapon/armour custom item, not just stash it in the backpack.
export const CUSTOM_SLOT_TO_PAPERDOLL = {
  EQUIPPABLE_IN_SLOT_WEAPON: 'weapon', EQUIPPABLE_IN_SLOT_SHIELD: 'shield',
  EQUIPPABLE_IN_SLOT_MASK: 'mask', EQUIPPABLE_IN_SLOT_HELM: 'helmet',
  EQUIPPABLE_IN_SLOT_GLOVES: 'gloves', EQUIPPABLE_IN_SLOT_BOOTS: 'boots',
  EQUIPPABLE_IN_SLOT_BREASTPLATE: 'breastplate', EQUIPPABLE_IN_SLOT_CLOAK: 'cloak',
  EQUIPPABLE_IN_SLOT_AMULET: 'amulet', EQUIPPABLE_IN_SLOT_RING: 'ring',
};

// The item list a loadout board picks from, and how it draws and groups each item: the vanilla
// items, the framework's built-ins (they ship inside the framework, so any class or race can use
// them) and this mod's own items. Shared by the Class Editor and the Race Editor.
export function useItemCatalog(modItems) {
  const customItemMap = useMemo(
    () => Object.fromEntries([...FRAMEWORK_ITEMS, ...(modItems || [])].map((it) => [it.id, it])),
    [modItems],
  );
  const allTypes = useMemo(
    () => [...ITEM_TYPES, ...FRAMEWORK_ITEMS.map((it) => it.id), ...(modItems || []).map((it) => it.id)],
    [modItems],
  );
  const iconFor = (type) =>
    customItemMap[type] ? <ItemIcon src={customIconSrc(customItemMap[type])} /> : itemIcon(type);
  const categoryFor = (type) =>
    customItemMap[type] ? (customItemMap[type].category || 'TOOL') : itemCategory(type);
  return { customItemMap, allTypes, iconFor, categoryFor };
}

// Saved starting_items back into board entries. The paperdoll cell of a vanilla item comes from its
// type; a custom item's comes from the slot its own JSON declares. entryFromJson cannot know that,
// so without this a custom weapon reopened from a saved file lands in the "slot already taken" tray.
export function entriesFromJson(list, customItemMap) {
  return (list ?? []).map((si) => {
    const e = entryFromJson(si);
    const cell = e.equip ? CUSTOM_SLOT_TO_PAPERDOLL[customItemMap[e.type]?.slot] : undefined;
    return cell ? { ...e, _slot: cell } : e;
  });
}

/** Which paperdoll slot an entry occupies: its item's natural slot, or its stored hint. */
const slotOfEntry = (it) => equipSlotOf(it.type) || it._slot || null;

/** One 64px cell: filled item (icon + blessing/count/condition badges + remove) or empty. */
function Cell({ entry, ghostIcon, label, selected, onClick, onRemove, removeTitle = 'Remove', handBadge = false }) {
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
      {handBadge && entry.count <= 1 && <span className="sam-slot-count" title="Also starts in hand (equipped)"><PixelIcon name="hand" /></span>}
      {!entry.identified && <span className="sam-slot-pip" title="unidentified">?</span>}
      <span className="sam-slot-x" title={removeTitle} onClick={(e) => { e.stopPropagation(); onRemove(); }}>✕</span>
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
              ghostIcon={entry ? iconFor(entry.type) : <img className="sam-item-icon" src={itemIconUrl(SLOT_ICON_FILE[slot.id])} alt="" draggable={false} />}
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
      <div className="text-xs mt-1" style={{ color: b > 0 ? 'var(--color-gold-bright)' : b < 0 ? 'var(--color-blood)' : 'var(--color-ink-dim)' }}>
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
  const isBook = category === 'BOOK';                  // READABLE_BOOK: appearance picks the book
  const looks = isBook ? 1 : looksOf(entry.type);      // items with several pictures: appearance picks one
  const look = looks > 1 ? (Number(entry.appearance) || 0) % looks : 0;
  const lookWord = looks > 1 ? `, look ${look + 1} of ${looks}` : '';
  const plain = isBook
    ? `${bWord}${word} ${entry.type} — "${bookTitle(entry.appearance)}"${entry.identified ? '' : ', unidentified'}`
    : `${bWord}${word} ${entry.type}${lookWord}${entry.identified ? '' : ', unidentified'}`;
  return (
    <div className="sam-well sam-detailstrip">
      <div className="flex items-center gap-2 mb-3">
        <span className="sam-pick-icon" aria-hidden>{iconFor(entry.type)}</span>
        <span className="flex-1 min-w-0 truncate" style={{ color: 'var(--color-parchment)', fontWeight: 600 }}>{entry.type}</span>
        <span className="text-xs" style={{ color: 'var(--color-ink-dim)' }}>{equipped ? 'equipped' : 'in backpack'}</span>
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
        <div>
          <div className="sam-label mb-1" style={{ fontSize: '0.72rem' }}
            title={equipped ? 'An equipped item can also sit on a hotbar key: it starts in hand and on that key.' : undefined}>Hotbar</div>
          <Select value={String(entry.hotbar_slot)} onChange={(v) => onHotbar(Number(v))}
            options={[{ value: '-1', label: equipped ? 'not on the hotbar' : 'in pack' }, ...Array.from({ length: 10 }, (_, i) => ({ value: String(i), label: `slot ${i === 9 ? 0 : i + 1}` }))]} />
        </div>
        {looks > 1 && (
          <div>
            <div className="sam-label mb-1" style={{ fontSize: '0.72rem' }}
              title={`Which of this item's ${looks} looks it starts with, shown as its picture (written as "appearance": ${look}).`}>Look</div>
            <Select value={String(look)} onChange={(v) => patch({ appearance: Number(v) })}
              options={Array.from({ length: looks }, (_, i) => ({ value: String(i), label: `Look ${i + 1} of ${looks}` }))} />
          </div>
        )}
        {isBook && (
          <div style={{ gridColumn: '1 / -1' }}>
            <div className="sam-label mb-1" style={{ fontSize: '0.72rem' }} title="Which readable book this is — the engine picks it by the item's appearance index.">Book / Contents</div>
            <Select value={String(entry.appearance || 0)} onChange={(v) => patch({ appearance: Number(v) })}
              options={BOOKS.map((title, i) => ({ value: String(i), label: title }))} />
          </div>
        )}
      </div>
      <div className="text-xs mt-3" style={{ color: 'var(--color-ink-dim)' }}>In game: <span style={{ color: 'var(--color-parchment)' }}>{plain}</span></div>
    </div>
  );
}

export default function LoadoutBoard({ items, onChange, allTypes, iconFor, categoryFor, categories, portraitUrl, gold, onGold }) {
  const [picker, setPicker] = useState(null); // { slot } for equip, { backpack:true } for pack
  const [selectedKey, setSelectedKey] = useState(null);

  const isPinned = (it) => Number(it.hotbar_slot) >= 0;           // equipped or not
  const backpack = items.filter((it) => !it.equip && !isPinned(it)); // loose (unpinned) items
  const hotbarItem = (n) => items.find((it) => isPinned(it) && Number(it.hotbar_slot) === n);
  const selected = items.find((it) => it._key === selectedKey);

  const patch = (key, fields) => onChange(items.map((it) => (it._key === key ? { ...it, ...fields } : it)));
  const remove = (key) => { onChange(items.filter((it) => it._key !== key)); if (key === selectedKey) setSelectedKey(null); };

  /** Pin an item to a hotbar slot (or -1 to unpin it), demoting whatever held that slot so two
   *  starting items never fight over one hotbar key. An equipped item keeps its equip and its
   *  paperdoll cell: it starts in hand and on that key. */
  const setHotbar = (key, n) => onChange(items.map((it) => {
    if (it._key === key) return { ...it, hotbar_slot: n };
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
      // If it was also pinned to a hotbar key, the key moves to the newcomer (replacing the
      // Wizard's staff with a sword should leave the sword on key 1, not strand the staff there).
      const pin = (items.find((it) => it.equip && slotOfEntry(it) === slot.id && isPinned(it)) || {}).hotbar_slot;
      const demoted = items.map((it) =>
        it.equip && slotOfEntry(it) === slot.id
          ? { ...it, equip: false, _slot: undefined, hotbar_slot: isPinned(it) ? -1 : it.hotbar_slot }
          : it);
      const e = { ...newEntry(type, true, slot.id), ...(pin != null && Number(pin) >= 0 ? { hotbar_slot: Number(pin) } : {}) };
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
      {onGold && (
        <div className="flex items-center gap-2 mb-3">
          <span className="sam-label"><PixelIcon name="coin" /> Starting gold</span>
          <div style={{ width: 140 }}>
            <NumberInput value={gold} min={0} onChange={(v) => onGold(v === '' ? 0 : Math.max(0, Number(v)))} aria-label="starting gold" />
          </div>
          <span className="text-xs" style={{ color: 'var(--color-ink-faint)' }}>Coins the class begins the run with.</span>
        </div>
      )}
      <div className="sam-loadout">
        {/* PAPERDOLL */}
        <div className="sam-paperdoll">
          <div className="sam-paperdoll-body">
            <EquipColumn slots={left} items={items} iconFor={iconFor} selectedKey={selectedKey}
              onPick={(s) => setPicker({ slot: s })} onSelect={setSelectedKey} onRemove={remove} />
            <div className="sam-paperdoll-center">
              {portraitUrl
                ? <img src={portraitUrl} alt="" className="sam-paperdoll-portrait" />
                : <span className="sam-paperdoll-empty">No portrait yet</span>}
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
              <div className="text-xs mt-2" style={{ color: 'var(--color-ink-faint)' }}>Backpack empty — click + to stash starting supplies.</div>
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
                      handBadge={!!(it && it.equip)}
                      removeTitle={it && it.equip ? 'Take off the hotbar (it stays equipped)' : 'Remove'}
                      onRemove={() => it && (it.equip ? patch(it._key, { hotbar_slot: -1 }) : remove(it._key))}
                    />
                  </div>
                );
              })}
            </div>
            <div className="text-xs mt-2" style={{ color: 'var(--color-ink-faint)' }}>Numbered quick-use slots. Click one to pin a starting item, or set an item's Hotbar in its details. A pinned item that is not worn still counts as in the pack; a worn one (marked with a hand) starts in hand and on that key, like the Wizard's staff.</div>
          </div>
        </div>
      </div>

      {conflicts.length > 0 && (
        <div className="sam-well mt-3" style={{ borderImageSource: 'var(--px-field-error)' }}>
          <div className="sam-error text-sm mb-2"><Gem kind="ruby" /> {conflicts.length} item(s) want a slot that's already taken (Barony has one of each). Move them to the backpack:</div>
          <div className="flex flex-wrap gap-2">
            {conflicts.map((it) => (
              <button key={it._key} type="button" className="sam-btn" style={{ padding: '0.2rem 0.5rem', fontSize: '0.78rem' }}
                onClick={() => patch(it._key, { equip: false, _slot: undefined })}>
                <span className="sam-btn-icon">{iconFor(it.type)}</span> {it.type} → backpack
              </button>
            ))}
          </div>
        </div>
      )}

      {/* DETAIL STRIP */}
      {selected
        ? <div className="mt-4"><DetailStrip entry={selected} patch={(f) => patch(selected._key, f)} onHotbar={(n) => setHotbar(selected._key, n)} iconFor={iconFor} category={categoryFor(selected.type)} onRemove={() => remove(selected._key)} /></div>
        : <div className="text-sm mt-4 text-center" style={{ color: 'var(--color-ink-faint)' }}>Click a slot to gear up, or click an item to set its blessing.</div>}

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
