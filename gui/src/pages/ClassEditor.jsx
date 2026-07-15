/*
 * Class Editor — mirrors the S.A.M design reference:
 * portrait + name up top; CORE ATTRIBUTES / SKILL LEVELS / STARTING ITEMS
 * panels side by side; spells, stat growth and gold below; SAVE CLASS bottom.
 * Every list here comes from the schemas at runtime (see data/schemas.js).
 */
import { useMemo, useRef, useState } from 'react';
import {
  CORE_ATTRIBUTES, OFFSET_STATS, SKILLS, ITEM_TYPES, ROLL_STATS, CATEGORIES,
  skillLabel, skillBaseAttr,
} from '@/data/schemas.js';
import { validate } from '@/lib/validate.js';
import { checkBalance } from '@/lib/balance.js';
import { useMod } from '@/state/ModContext.jsx';
import {
  Panel, Field, TextInput, NumberInput, GoldSlider, Stepper, GoldButton,
  InventoryGrid, ErrorList, SavedNote, BalanceHints,
} from '@/components/ui.jsx';

const MAX_PORTRAIT_BYTES = 256 * 1024; // portraits are 54x54 — anything big is a mistake

const ATTR_ICONS = { STR: '💪', DEX: '🪶', CON: '❤️', INT: '📖', PER: '👁️', CHR: '🎭' };

const SKILL_ICONS = {
  PRO_SWORD: '⚔️', PRO_AXE: '🪓', PRO_MACE: '🔨', PRO_POLEARM: '🔱',
  PRO_RANGED: '🏹', PRO_SHIELD: '🛡️', PRO_UNARMED: '👊', PRO_STEALTH: '🌑',
  PRO_LOCKPICKING: '🗝️', PRO_APPRAISAL: '🔍', PRO_TRADING: '💰',
  PRO_LEADERSHIP: '👑', PRO_SORCERY: '🔮', PRO_MYSTICISM: '✨',
  PRO_THAUMATURGY: '📿', PRO_ALCHEMY: '⚗️',
};

function itemIcon(type) {
  if (/SWORD|DAGGER|RAPIER|CLAYMORE|ANELACE|FALSHION/.test(type)) return '🗡️';
  if (/SHIELD|SCUTUM/.test(type)) return '🛡️';
  if (/BOW|CROSSBOW|SLING|QUIVER/.test(type)) return '🏹';
  if (/AXE|TOMAHAWK/.test(type)) return '🪓';
  if (/MACE|FLAIL|SHILLELAGH|KNUCKLES/.test(type)) return '🔨';
  if (/SPEAR|HALBERD|TRIDENT|LANCE|POLEARM|GLAIVE/.test(type)) return '🔱';
  if (/POTION/.test(type)) return '🧪';
  if (/SCROLL/.test(type)) return '📜';
  if (/SPELLBOOK|TOME|BOOK/.test(type)) return '📖';
  if (/FOOD|BREAD|CHEESE|APPLE|MEAT|FISH|RATION/.test(type)) return '🍞';
  if (/TORCH|LANTERN/.test(type)) return '🔥';
  if (/RING/.test(type)) return '💍';
  if (/AMULET/.test(type)) return '📿';
  if (/GEM/.test(type)) return '💎';
  if (/HAT|HELM|HOOD|MASK|CROWN|CIRCLET/.test(type)) return '🪖';
  if (/BOOTS|LOAFERS|CLEAT/.test(type)) return '🥾';
  if (/GLOVES|GAUNTLETS|BRACERS/.test(type)) return '🧤';
  if (/CLOAK|ROBE|DOUBLET|BREASTPIECE|TUNIC|GAMBESON|HAUBERK|SHAWL|APRON/.test(type)) return '🥋';
  if (/STAFF|SCEPTER/.test(type)) return '🪄';
  if (/KEY/.test(type)) return '🗝️';
  return '⚔️';
}

/* Group an ItemType into one of the schema's categories for the picker.
 * Barony names items by category prefix (POTION_, SCROLL_, TOOL_, …); the rest
 * (weapons/armor named by material+type) fall to keyword matching. Heuristic —
 * only drives grouping in the picker, so the odd edge case is harmless. */
function itemCategory(t) {
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

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

const SPELL_PATTERN = /^SPELL_[A-Z0-9_]+$/;

export default function ClassEditor() {
  const { meta, dispatch } = useMod();

  const [name, setName] = useState('');
  const [description, setDescription] = useState('');
  const [attrs, setAttrs] = useState(() =>
    Object.fromEntries(CORE_ATTRIBUTES.map((a) => [a, 10]))
  );
  const [offsets, setOffsets] = useState(() =>
    Object.fromEntries(OFFSET_STATS.map((s) => [s, 0]))
  );
  const [skills, setSkills] = useState(() =>
    Object.fromEntries(SKILLS.map((s) => [s, 0]))
  );
  const [items, setItems] = useState([]); // [{type, count}]
  const [spells, setSpells] = useState([]);
  const [spellDraft, setSpellDraft] = useState('');
  const [spellError, setSpellError] = useState('');
  const [growth, setGrowth] = useState({}); // attr -> 'strong' | 'weak' | undefined
  const [gold, setGold] = useState(0);
  const [portrait, setPortrait] = useState({ path: '', dataUrl: '' });
  const [portraitError, setPortraitError] = useState('');
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');
  const portraitRef = useRef(null);

  const namespace = meta.namespace || 'mymod';
  const classId = `${namespace}:${slugify(name)}`;
  const defaultPortraitPath = `portraits/${slugify(name)}.png`;

  const onPortraitFile = (e) => {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    if (file.size > MAX_PORTRAIT_BYTES) {
      setPortraitError(`That PNG is ${Math.round(file.size / 1024)} KB — portraits are 54×54; keep it under 256 KB.`);
      return;
    }
    const reader = new FileReader();
    reader.onload = () => {
      setPortraitError('');
      setPortrait((p) => ({ path: p.path || defaultPortraitPath, dataUrl: String(reader.result ?? '') }));
    };
    reader.readAsDataURL(file);
  };

  const addSpell = () => {
    const s = spellDraft.trim().toUpperCase();
    if (!s) return;
    if (!SPELL_PATTERN.test(s)) {
      setSpellError('Must be a SPELL_X constant, e.g. SPELL_FORCEBOLT');
      return;
    }
    setSpellError('');
    setSpells((prev) => (prev.includes(s) ? prev : [...prev, s]));
    setSpellDraft('');
  };

  const cycleGrowth = (attr) => {
    setGrowth((prev) => {
      const cur = prev[attr];
      const next = cur === undefined ? 'strong' : cur === 'strong' ? 'weak' : undefined;
      return { ...prev, [attr]: next };
    });
  };

  const buildDef = () => {
    const stats = {};
    for (const a of CORE_ATTRIBUTES) stats[a] = attrs[a];
    for (const s of OFFSET_STATS) stats[s] = offsets[s] === '' ? 0 : offsets[s];

    const nonzeroSkills = Object.fromEntries(
      Object.entries(skills).filter(([, v]) => v > 0)
    );

    const def = {
      id: classId,
      name: name.trim(),
      stats,
    };
    if (description.trim()) def.description = description.trim();
    if (Object.keys(nonzeroSkills).length) def.skills = nonzeroSkills;
    if (items.length) {
      def.starting_items = items.map((it) => {
        const entry = { type: it.type };
        if (it.count > 1) entry.count = it.count;
        if (it.equip) entry.equip = true;
        return entry;
      });
    }
    if (spells.length) def.starting_spells = spells;
    const strong = ROLL_STATS.filter((a) => growth[a] === 'strong');
    const weak = ROLL_STATS.filter((a) => growth[a] === 'weak');
    if (strong.length || weak.length) {
      def.stat_growth = {};
      if (strong.length) def.stat_growth.strong_rolls = strong;
      if (weak.length) def.stat_growth.weak_rolls = weak;
    }
    if (gold > 0) def.gold = gold;
    if (portrait.path.trim()) def.portrait = portrait.path.trim();
    return def;
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const result = validate('class', def);
    if (!result.valid) {
      setErrors(result.errors);
      return;
    }
    setErrors([]);
    // Ship the uploaded PNG as a mod asset at the portrait path.
    if (portrait.path.trim() && portrait.dataUrl) {
      dispatch({ type: 'setAsset', path: portrait.path.trim(), dataUrl: portrait.dataUrl });
    }
    dispatch({ type: 'saveClass', def });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [name, description, attrs, offsets, skills, items, spells, growth, gold, portrait, namespace]);
  const preview = useMemo(() => JSON.stringify(def, null, 2), [def]);
  const hints = useMemo(() => checkBalance('class', def), [def]);

  return (
    <div className="space-y-4 max-w-7xl mx-auto">
      {/* ------------------------------------------------ name + portrait */}
      <div className="flex items-center gap-4">
        <button
          type="button"
          className="sam-panel flex items-center justify-center shrink-0 overflow-hidden"
          style={{ width: 84, height: 84, fontSize: '2.2rem', padding: 0, cursor: 'pointer' }}
          title="Upload class portrait (PNG, 54×54)"
          onClick={() => portraitRef.current?.click()}
        >
          {portrait.dataUrl
            ? <img src={portrait.dataUrl} alt="Class portrait" style={{ width: 54, height: 54, imageRendering: 'pixelated' }} />
            : <span aria-hidden>🛡</span>}
        </button>
        <input ref={portraitRef} type="file" accept="image/png" className="hidden" onChange={onPortraitFile} />
        <div className="flex-1">
          <TextInput
            value={name}
            onChange={setName}
            placeholder="Class name — e.g. Warden"
            style={{ fontSize: '1.5rem', padding: '0.7rem 1rem' }}
            aria-label="Class name"
          />
          <div className="mt-1 text-xs" style={{ color: '#6b5a35' }}>
            id: <span className="sam-mono">{classId}</span>
            {' '}(namespace comes from the Mod Builder)
          </div>
        </div>
      </div>

      {/* ------------------------------------------------------- portrait */}
      <Panel title="Portrait">
        <div className="grid grid-cols-1 sm:grid-cols-[auto_1fr] gap-4 items-end">
          <GoldButton onClick={() => portraitRef.current?.click()}>🖼 Upload PNG</GoldButton>
          <Field label="Portrait path (mod-relative)" hint="Shipped in the zip; shown as the class-select icon in-game.">
            <TextInput
              value={portrait.path}
              onChange={(v) => setPortrait((p) => ({ ...p, path: v }))}
              placeholder={defaultPortraitPath}
            />
          </Field>
        </div>
        {portrait.dataUrl && (
          <div className="mt-2 text-xs" style={{ color: '#6b5a35' }}>
            PNG loaded — it will export at <span className="sam-mono">{portrait.path || defaultPortraitPath}</span>.
            <button type="button" className="ml-2 underline" style={{ color: '#a03327' }} onClick={() => setPortrait((p) => ({ ...p, dataUrl: '' }))}>clear image</button>
          </div>
        )}
        {portraitError && <div className="sam-error text-sm mt-1">{portraitError}</div>}
      </Panel>

      {/* ------------------------------------------- three main panels */}
      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4 items-start">
        <Panel title="Core Attributes">
          {CORE_ATTRIBUTES.map((a) => (
            <GoldSlider
              key={a}
              label={a}
              icon={ATTR_ICONS[a]}
              value={attrs[a]}
              min={0}
              max={20}
              onChange={(v) => setAttrs((prev) => ({ ...prev, [a]: v }))}
            />
          ))}
          <div className="sam-divider" />
          <div className="grid grid-cols-2 gap-3">
            {OFFSET_STATS.map((s) => (
              <Field key={s} label={`${s} offset`}>
                <NumberInput
                  value={offsets[s]}
                  onChange={(v) => setOffsets((prev) => ({ ...prev, [s]: v }))}
                />
              </Field>
            ))}
          </div>
        </Panel>

        <Panel title="Skill Levels" bodyClassName="max-h-[460px] overflow-y-auto">
          {SKILLS.map((s) => (
            <Stepper
              key={s}
              label={`${SKILL_ICONS[s] ?? '◆'} ${skillLabel(s)}`}
              sub={`${s} — base ${skillBaseAttr(s)}`}
              value={skills[s]}
              min={0}
              max={100}
              step={5}
              onChange={(v) => setSkills((prev) => ({ ...prev, [s]: v }))}
            />
          ))}
        </Panel>

        <Panel title="Starting Items">
          <InventoryGrid
            items={items}
            allTypes={ITEM_TYPES}
            iconFor={itemIcon}
            categoryFor={itemCategory}
            categories={CATEGORIES}
            onChange={setItems}
          />
        </Panel>
      </div>

      {/* --------------------------------------- spells + growth + gold */}
      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4 items-start">
        <Panel title="Starting Spells">
          <div className="flex flex-wrap gap-2 mb-3 min-h-8">
            {spells.map((s) => (
              <span key={s} className="sam-well px-2 py-1 text-sm inline-flex items-center gap-2"
                style={{ color: 'var(--color-parchment)' }}>
                ✨ {s}
                <button
                  type="button"
                  className="sam-step sam-remove"
                  style={{ width: 18, height: 18, fontSize: '0.7rem' }}
                  onClick={() => setSpells((prev) => prev.filter((x) => x !== s))}
                  aria-label={`remove ${s}`}
                >✕</button>
              </span>
            ))}
          </div>
          <div className="flex gap-2">
            <TextInput
              value={spellDraft}
              onChange={setSpellDraft}
              placeholder="SPELL_FORCEBOLT"
              onKeyDown={(e) => e.key === 'Enter' && addSpell()}
            />
            <GoldButton onClick={addSpell}>Add</GoldButton>
          </div>
          {spellError && <div className="sam-error text-sm mt-1">{spellError}</div>}
        </Panel>

        <Panel title="Stat Growth">
          <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
            Click to cycle: neutral → <span className="sam-ok">strong roll</span> →{' '}
            <span className="sam-error">weak roll</span>
          </div>
          <div className="grid grid-cols-3 gap-2">
            {ROLL_STATS.map((a) => {
              const state = growth[a];
              return (
                <button
                  key={a}
                  type="button"
                  className="sam-btn"
                  style={{
                    padding: '0.4rem 0.5rem',
                    borderColor: state === 'strong' ? '#5a7a3a' : state === 'weak' ? '#a03327' : undefined,
                    color: state === 'strong' ? '#9dc76a' : state === 'weak' ? '#e07a6a' : undefined,
                  }}
                  onClick={() => cycleGrowth(a)}
                >
                  {ATTR_ICONS[a]} {a}{state === 'strong' ? ' ▲' : state === 'weak' ? ' ▼' : ''}
                </button>
              );
            })}
          </div>
        </Panel>

        <Panel title="Purse">
          <Field label="Starting gold">
            <NumberInput value={gold} min={0} onChange={(v) => setGold(v === '' ? 0 : Math.max(0, v))} />
          </Field>
        </Panel>
      </div>

      {/* ------------------------------------------------------- save row */}
      <BalanceHints hints={hints} />
      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> to this session's mod — see Mod Builder.</SavedNote>}
        <GoldButton tone="red" onClick={save} disabled={!name.trim()}>
          🛡 Save Class
        </GoldButton>
      </div>

      {/* ------------------------------------------------------ live JSON */}
      <Panel title="Live JSON Preview" bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>
          {preview}
        </pre>
      </Panel>
    </div>
  );
}
