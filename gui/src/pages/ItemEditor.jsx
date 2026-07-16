/*
 * Item Editor — mirrors the S.A.M design reference (see ClassEditor):
 * identified/unidentified name up top; CLASSIFICATION / PROPERTIES /
 * APPEARANCE / ATTRIBUTES panels; SAVE ITEM at the bottom with a live JSON
 * preview. Category + slot lists come from the schemas at runtime.
 */
import { useEffect, useMemo, useState } from 'react';
import { CATEGORIES, SLOTS, ITEM_TYPES } from '@/data/schemas.js';
import { validate } from '@/lib/validate.js';
import { checkBalance } from '@/lib/balance.js';
import { useMod } from '@/state/ModContext.jsx';
import {
  Panel, Field, TextInput, NumberInput, Select, SearchSelect, GoldButton,
  ErrorList, SavedNote, BalanceHints,
} from '@/components/ui.jsx';

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

/** Coerce a NumberInput value ('' when cleared) to a fallback number. */
const numOr = (v, d) => (v === '' || v === null || v === undefined ? d : v);

export default function ItemEditor() {
  const { meta, items, editing, dispatch } = useMod();

  // "Edit" handoff from the Mod Builder: seed the form from a saved item.
  const editDef = editing?.kind === 'item' ? items.find((it) => it.id === editing.id) : null;

  const [nameId, setNameId] = useState(editDef?.name_identified ?? '');
  const [nameUnid, setNameUnid] = useState(editDef?.name_unidentified ?? '');
  const [description, setDescription] = useState(editDef?.description ?? '');
  const [category, setCategory] = useState(editDef?.category ?? CATEGORIES[0]);
  const [slot, setSlot] = useState(editDef?.slot ?? 'NO_EQUIP');
  const [weight, setWeight] = useState(editDef?.weight ?? 0);
  const [goldValue, setGoldValue] = useState(editDef?.gold_value ?? 0);
  const [level, setLevel] = useState(editDef?.level ?? 0);
  const [stackable, setStackable] = useState(editDef?.stackable ?? false);
  const [magicLevel, setMagicLevel] = useState(editDef?.magic_level ?? 0);
  const [model, setModel] = useState(editDef?.model ?? '');
  const [modelFp, setModelFp] = useState(editDef?.model_fp ?? '');
  const [modelFromItem, setModelFromItem] = useState(editDef?.model_from_item ?? '');
  const [icon, setIcon] = useState(editDef?.icon ?? '');
  const [attribs, setAttribs] = useState(() =>
    Object.entries(editDef?.attributes ?? {}).map(([key, value]) => ({ key, value }))
  );
  const [attrKey, setAttrKey] = useState('');
  const [attrVal, setAttrVal] = useState(0);
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');

  useEffect(() => {
    if (editing?.kind === 'item') dispatch({ type: 'clearEditing' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const namespace = meta.namespace || 'mymod';
  const itemId = `${namespace}:${slugify(nameId)}`;

  const addAttr = () => {
    const k = attrKey.trim();
    if (!k) return;
    const v = Math.trunc(numOr(attrVal, 0));
    setAttribs((prev) => {
      const i = prev.findIndex((a) => a.key === k);
      if (i >= 0) return prev.map((a, j) => (j === i ? { key: k, value: v } : a));
      return [...prev, { key: k, value: v }];
    });
    setAttrKey('');
    setAttrVal(0);
  };

  const buildDef = () => {
    const def = {
      id: itemId,
      name_identified: nameId.trim(),
      category,
      slot,
      weight: numOr(weight, 0),
      gold_value: numOr(goldValue, 0),
      level: numOr(level, 0),
    };
    if (nameUnid.trim()) def.name_unidentified = nameUnid.trim();
    if (description.trim()) def.description = description.trim();
    if (model.trim()) def.model = model.trim();
    if (modelFp.trim()) def.model_fp = modelFp.trim();
    if (modelFromItem.trim()) def.model_from_item = modelFromItem.trim();
    if (icon.trim()) def.icon = icon.trim();
    if (attribs.length) {
      def.attributes = Object.fromEntries(attribs.map((a) => [a.key, a.value]));
    }
    if (stackable) def.stackable = true;
    const mag = numOr(magicLevel, 0);
    if (mag !== 0) def.magic_level = mag;
    return def;
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const result = validate('item', def);
    if (!result.valid) {
      setErrors(result.errors);
      return;
    }
    setErrors([]);
    dispatch({ type: 'saveItem', def });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [nameId, nameUnid, description, category, slot, weight, goldValue, level, stackable,
      magicLevel, model, modelFp, modelFromItem, icon, attribs, namespace]);
  const preview = useMemo(() => JSON.stringify(def, null, 2), [def]);
  const hints = useMemo(() => checkBalance('item', def), [def]);

  return (
    <div className="space-y-4 max-w-7xl mx-auto">
      {/* --------------------------------------------- names + emblem */}
      <div className="flex items-center gap-4">
        <div
          className="sam-panel flex items-center justify-center shrink-0"
          style={{ width: 84, height: 84, fontSize: '2.2rem' }}
          title="Item emblem (custom item art is planned — not yet loaded)"
          aria-hidden
        >
          ⚔
        </div>
        <div className="flex-1 space-y-2">
          <TextInput
            value={nameId}
            onChange={setNameId}
            placeholder="Item name — e.g. Shadowblade"
            style={{ fontSize: '1.5rem', padding: '0.7rem 1rem' }}
            aria-label="Identified item name"
          />
          <TextInput
            value={nameUnid}
            onChange={setNameUnid}
            placeholder="dark sword"
            aria-label="Unidentified item name"
          />
          <div className="text-xs" style={{ color: '#6b5a35' }}>
            id: <span className="sam-mono">{itemId}</span>
            {' '}(namespace comes from the Mod Builder)
          </div>
        </div>
      </div>

      {/* --------------------------------------------- main panels */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="Classification">
          <div className="space-y-3">
            <Field label="Category">
              <Select value={category} onChange={setCategory} options={CATEGORIES} />
            </Field>
            <Field label="Equip Slot" hint="NO_EQUIP for items that can't be worn or wielded.">
              <Select value={slot} onChange={setSlot} options={SLOTS} />
            </Field>
          </div>
        </Panel>

        <Panel title="Properties">
          <div className="grid grid-cols-2 gap-3">
            <Field label="Weight">
              <NumberInput value={weight} min={0} onChange={setWeight} />
            </Field>
            <Field label="Gold Value">
              <NumberInput value={goldValue} min={0} onChange={setGoldValue} />
            </Field>
            <Field label="Level" hint="-1 = excluded from random loot">
              <NumberInput value={level} min={-1} onChange={setLevel} />
            </Field>
            <Field label="Magic Level">
              <NumberInput value={magicLevel} onChange={setMagicLevel} />
            </Field>
          </div>
          <div className="sam-divider" />
          <Field label="Description" hint="Hover tooltip text — one line per row">
            <textarea
              className="sam-input"
              rows={2}
              value={description}
              onChange={(e) => setDescription(e.target.value)}
              placeholder="A shield that answers a blow with one of its own."
            />
          </Field>
          <div className="sam-divider" />
          <Field label="Stackable">
            <GoldButton
              tone={stackable ? 'green' : 'gold'}
              onClick={() => setStackable((s) => !s)}
            >
              {stackable ? '✓ Stacks in one slot' : '✗ Does not stack'}
            </GoldButton>
          </Field>
        </Panel>

        <Panel title="Appearance">
          <div className="mb-3 text-xs" style={{ color: '#9dc76a' }}>
            ✓ Both the <span className="sam-mono">3D Model</span> and the <span className="sam-mono">Inventory Icon</span> load in-game (v0.9.3+).
            Borrow a vanilla item's 3D model, and point the icon at your own PNG.
          </div>
          <div className="space-y-3">
            <Field
              label="3D Model — reuse a vanilla item"
              hint='Your item wears this vanilla item&apos;s model (world + first-person), e.g. SILVER_SHIELD. Leave blank to auto-pick by equip slot (a shield looks like a shield, boots like boots…).'
            >
              <SearchSelect
                options={ITEM_TYPES}
                value={modelFromItem}
                onPick={setModelFromItem}
                placeholder="SILVER_SHIELD"
                allowCustom
              />
              {modelFromItem && (
                <button type="button" className="mt-1 text-xs underline" style={{ color: '#a03327' }} onClick={() => setModelFromItem('')}>clear</button>
              )}
            </Field>
            <Field label="Inventory Icon" hint="Loaded at runtime — path to a PNG in your mod folder (e.g. items/shadowblade.png)">
              <TextInput value={icon} onChange={setIcon} placeholder="items/shadowblade.png" />
            </Field>
            <details>
              <summary className="text-xs cursor-pointer" style={{ color: '#6b5a35' }}>Custom .vox model paths (not supported yet)</summary>
              <div className="space-y-3 mt-2">
                <Field label="World Model" hint="PLANNED — a new .vox can't be loaded yet; use the 3D Model picker above">
                  <TextInput value={model} onChange={setModel} placeholder="models/shadowblade.vox" />
                </Field>
                <Field label="First-Person Model" hint="PLANNED — not yet loaded">
                  <TextInput value={modelFp} onChange={setModelFp} placeholder="models/shadowblade_fp.vox" />
                </Field>
              </div>
            </details>
          </div>
        </Panel>

        <Panel title="Attributes">
          <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
            Named integer effects (e.g. <span className="sam-mono">ATK</span>,{' '}
            <span className="sam-mono">shadow_damage</span>).
          </div>
          <div className="flex flex-wrap gap-2 mb-3 min-h-8">
            {attribs.length === 0 && (
              <div className="text-sm" style={{ color: '#6b5a35' }}>No attributes yet.</div>
            )}
            {attribs.map((a) => (
              <span
                key={a.key}
                className="sam-well px-2 py-1 text-sm inline-flex items-center gap-2"
                style={{ color: 'var(--color-parchment)' }}
              >
                <span className="sam-mono">{a.key}</span> {a.value}
                <button
                  type="button"
                  className="sam-step sam-remove"
                  style={{ width: 18, height: 18, fontSize: '0.7rem' }}
                  onClick={() => setAttribs((prev) => prev.filter((x) => x.key !== a.key))}
                  aria-label={`remove ${a.key}`}
                >✕</button>
              </span>
            ))}
          </div>
          <div className="flex gap-2 items-end">
            <Field label="Key" className="flex-1">
              <TextInput
                value={attrKey}
                onChange={setAttrKey}
                placeholder="ATK"
                onKeyDown={(e) => e.key === 'Enter' && addAttr()}
              />
            </Field>
            <Field label="Value" className="w-24">
              <NumberInput
                value={attrVal}
                onChange={setAttrVal}
                onKeyDown={(e) => e.key === 'Enter' && addAttr()}
              />
            </Field>
            <GoldButton onClick={addAttr}>Add</GoldButton>
          </div>
        </Panel>
      </div>

      {/* --------------------------------------------------- save row */}
      <BalanceHints hints={hints} />
      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && (
          <SavedNote>
            Saved <span className="sam-mono">{savedAs}</span> to this session's mod — see Mod Builder.
          </SavedNote>
        )}
        <GoldButton tone="red" onClick={save} disabled={!nameId.trim()}>
          ⚔ Save Item
        </GoldButton>
      </div>

      {/* --------------------------------------------------- preview card */}
      <Panel title="Preview">
        <div className="sam-well p-3 flex gap-3 items-start" style={{ maxWidth: 440 }}>
          <div className="sam-panel flex items-center justify-center shrink-0" style={{ width: 56, height: 56, fontSize: '1.8rem', padding: 0 }} aria-hidden>⚔</div>
          <div className="flex-1 min-w-0">
            <div style={{ color: 'var(--color-gold-bright)', fontSize: '1.05rem' }}>{def.name_identified || 'Unnamed item'}</div>
            {def.name_unidentified && <div className="text-xs" style={{ color: '#6b5a35' }}>unidentified: “{def.name_unidentified}”</div>}
            <div className="text-xs mt-1" style={{ color: 'var(--color-parchment)' }}>
              {def.category}{def.slot && def.slot !== 'NO_EQUIP' ? ` · ${def.slot.replace('EQUIPPABLE_IN_SLOT_', '').toLowerCase()}` : ''}
              {def.stackable ? ' · stackable' : ''}{def.magic_level ? ` · magic ${def.magic_level}` : ''}
            </div>
            <div className="text-xs mt-1 sam-mono" style={{ color: '#9b8a5a' }}>wgt {def.weight} · {def.gold_value}g · lvl {def.level}</div>
            <div className="text-xs mt-1" style={{ color: '#9dc76a' }}>
              model: {def.model_from_item || (def.slot && def.slot !== 'NO_EQUIP' ? `auto (${def.slot.replace('EQUIPPABLE_IN_SLOT_', '').toLowerCase()} placeholder)` : 'category placeholder')}
            </div>
            {def.attributes && (
              <div className="flex flex-wrap gap-1 mt-2">
                {Object.entries(def.attributes).map(([k, v]) => (
                  <span key={k} className="sam-well px-2 py-0.5 text-xs" style={{ color: 'var(--color-parchment)' }}><span className="sam-mono">{k}</span> {v > 0 ? `+${v}` : v}</span>
                ))}
              </div>
            )}
          </div>
        </div>
      </Panel>

      {/* -------------------------------------------------- live JSON */}
      <Panel title="Live JSON Preview" bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>
          {preview}
        </pre>
      </Panel>
    </div>
  );
}
