/*
 * Class Editor — mirrors the S.A.M design reference:
 * portrait + name up top; CORE ATTRIBUTES / SKILL LEVELS / STARTING ITEMS
 * panels side by side; spells, stat growth and gold below; SAVE CLASS bottom.
 * Every list here comes from the schemas at runtime (see data/schemas.js).
 */
import { useEffect, useMemo, useRef, useState } from 'react';
import {
  CORE_ATTRIBUTES, OFFSET_STATS, SKILLS, ITEM_TYPES, ROLL_STATS, CATEGORIES,
  CLASS_SPELL_REF_PATTERN, skillLabel, skillBaseAttr,
} from '@/data/schemas.js';
import { ITEM_ICONS } from '@/data/itemIcons.js';
import { validate } from '@/lib/validate.js';
import { checkBalance } from '@/lib/balance.js';
import { useMod } from '@/state/ModContext.jsx';
import ScriptEditor from '@/components/ScriptEditor.jsx';
import {
  Panel, Field, TextInput, NumberInput, GoldSlider, Stepper, GoldButton,
  InventoryGrid, ItemIcon, ErrorList, SavedNote, BalanceHints,
} from '@/components/ui.jsx';

const MAX_PORTRAIT_BYTES = 256 * 1024; // portraits are 54x54 — anything big is a mistake

// Per-level HP/MP growth row (Barony's ClassBaseGrowths). Vanilla rows run ~1-5 per
// field; 3 is the engine's "default" row. Distinct from `stat_growth`, which is about
// which ATTRIBUTES roll high/low on level-up.
const HPMP_GROWTH_KEYS = ['HP', 'MP', 'HP_REGEN', 'MP_REGEN'];
const HPMP_GROWTH_LABELS = {
  HP: 'HP / level', MP: 'MP / level',
  HP_REGEN: 'HP regen', MP_REGEN: 'MP regen',
};

const ATTR_ICONS = { STR: '💪', DEX: '🪶', CON: '❤️', INT: '📖', PER: '👁️', CHR: '🎭' };

const SKILL_ICONS = {
  PRO_SWORD: '⚔️', PRO_AXE: '🪓', PRO_MACE: '🔨', PRO_POLEARM: '🔱',
  PRO_RANGED: '🏹', PRO_SHIELD: '🛡️', PRO_UNARMED: '👊', PRO_STEALTH: '🌑',
  PRO_LOCKPICKING: '🗝️', PRO_APPRAISAL: '🔍', PRO_TRADING: '💰',
  PRO_LEADERSHIP: '👑', PRO_SORCERY: '🔮', PRO_MYSTICISM: '✨',
  PRO_THAUMATURGY: '📿', PRO_ALCHEMY: '⚗️',
};

function itemEmoji(type) {
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

/* Real in-game icon URL for an ItemType, or null if we don't have one.
 * import.meta.env.BASE_URL is '/' in dev and '/SAM-Framework/' in the Pages build. */
function iconSrc(type) {
  const file = ITEM_ICONS[type];
  return file ? `${import.meta.env.BASE_URL}item-icons/${file}` : null;
}

/* What the grid + picker render for an item: the real Barony PNG when it's
 * available locally, otherwise the emoji (so the public build still works). */
function itemIcon(type) {
  return <ItemIcon src={iconSrc(type)} emoji={itemEmoji(type)} />;
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

export default function ClassEditor() {
  const { meta, classes, spells: modSpells, scripts, editing, dispatch } = useMod();

  // "Edit" handoff from the Mod Builder: seed the form from a saved class.
  const editDef = editing?.kind === 'class' ? classes.find((c) => c.id === editing.id) : null;
  const existingScript = editDef ? scripts[editDef.id] : null;

  const [name, setName] = useState(editDef?.name ?? '');
  const [description, setDescription] = useState(editDef?.description ?? '');
  const [attrs, setAttrs] = useState(() =>
    Object.fromEntries(CORE_ATTRIBUTES.map((a) => [a, editDef?.stats?.[a] ?? 10]))
  );
  const [offsets, setOffsets] = useState(() =>
    Object.fromEntries(OFFSET_STATS.map((s) => [s, editDef?.stats?.[s] ?? 0]))
  );
  const [skills, setSkills] = useState(() =>
    Object.fromEntries(SKILLS.map((s) => [s, editDef?.skills?.[s] ?? 0]))
  );
  const [items, setItems] = useState(() =>
    (editDef?.starting_items ?? []).map((si) => ({ type: si.type, count: si.count ?? 1, equip: !!si.equip }))
  );
  const [spells, setSpells] = useState(editDef?.starting_spells ?? []);
  const [spellDraft, setSpellDraft] = useState('');
  const [spellError, setSpellError] = useState('');
  const [growth, setGrowth] = useState(() => {
    const g = {};
    for (const a of editDef?.stat_growth?.strong_rolls ?? []) g[a] = 'strong';
    for (const a of editDef?.stat_growth?.weak_rolls ?? []) g[a] = 'weak';
    return g;
  });
  // Per-level HP/MP growth + regen. 3 is the engine's "default" row — which is exactly
  // what every custom class silently used before the growth lookup was fixed — so the
  // form starts there and only emits JSON once you change something.
  const [hpMpGrowth, setHpMpGrowth] = useState(() => ({
    HP: editDef?.hp_mp_growth?.HP ?? 3,
    MP: editDef?.hp_mp_growth?.MP ?? 3,
    HP_REGEN: editDef?.hp_mp_growth?.HP_REGEN ?? 3,
    MP_REGEN: editDef?.hp_mp_growth?.MP_REGEN ?? 3,
  }));
  const [mpRegenBase, setMpRegenBase] = useState(editDef?.mp_regen?.base ?? 0);
  const [mpRegenMult, setMpRegenMult] = useState(editDef?.mp_regen?.multiplier ?? 1);
  const [mpRegenScaling, setMpRegenScaling] = useState(() => {
    const s = {};
    for (const a of ROLL_STATS) s[a] = editDef?.mp_regen?.stat_scaling?.[a] ?? '';
    return s;
  });
  const [gold, setGold] = useState(editDef?.gold ?? 0);
  const [portrait, setPortrait] = useState({ path: editDef?.portrait ?? '', dataUrl: '' });
  const [portraitError, setPortraitError] = useState('');
  const [scriptLang, setScriptLang] = useState(existingScript?.lang ?? 'lua');
  const [scriptCode, setScriptCode] = useState(existingScript?.code ?? '');
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');
  const portraitRef = useRef(null);

  // Consume the edit handoff so a later fresh visit starts blank.
  useEffect(() => {
    if (editing?.kind === 'class') dispatch({ type: 'clearEditing' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

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

  const addSpell = (raw) => {
    const input = (typeof raw === 'string' ? raw : spellDraft).trim();
    if (!input) return;
    const s = input.includes(':') ? input.toLowerCase() : input.toUpperCase();
    if (!CLASS_SPELL_REF_PATTERN.test(s)) {
      setSpellError('Use a SPELL_X constant or a custom "namespace:spell" id.');
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
    // Only emit hp_mp_growth once it differs from the engine default row, so an
    // untouched class keeps producing byte-identical JSON to before.
    const growthNum = (k) => (hpMpGrowth[k] === '' ? 3 : Number(hpMpGrowth[k]));
    if (HPMP_GROWTH_KEYS.some((k) => growthNum(k) !== 3)) {
      def.hp_mp_growth = Object.fromEntries(HPMP_GROWTH_KEYS.map((k) => [k, growthNum(k)]));
    }
    // Same for mp_regen: omit entirely unless the modder actually tuned something.
    const scaling = Object.fromEntries(
      Object.entries(mpRegenScaling)
        .filter(([, v]) => v !== '' && Number(v) !== 0)
        .map(([k, v]) => [k, Number(v)])
    );
    const regenBase = mpRegenBase === '' ? 0 : Number(mpRegenBase);
    const regenMult = mpRegenMult === '' ? 1 : Number(mpRegenMult);
    if (regenBase !== 0 || regenMult !== 1 || Object.keys(scaling).length) {
      def.mp_regen = {};
      if (regenBase !== 0) def.mp_regen.base = regenBase;
      if (Object.keys(scaling).length) def.mp_regen.stat_scaling = scaling;
      if (regenMult !== 1) def.mp_regen.multiplier = regenMult;
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
    dispatch({ type: 'saveScript', classId: def.id, lang: scriptLang, code: scriptCode });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [name, description, attrs, offsets, skills, items, spells, growth, gold, portrait, namespace,
      hpMpGrowth, mpRegenBase, mpRegenMult, mpRegenScaling]);
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
              placeholder="SPELL_FORCEBOLT or mymod:spell"
              onKeyDown={(e) => e.key === 'Enter' && addSpell()}
            />
            <GoldButton onClick={() => addSpell()}>Add</GoldButton>
          </div>
          {spellError && <div className="sam-error text-sm mt-1">{spellError}</div>}
          {modSpells.length > 0 && (
            <div className="mt-3">
              <div className="sam-label mb-1" style={{ color: '#8a6d2e' }}>Your custom spells</div>
              <div className="flex flex-wrap gap-1">
                {modSpells.map((sp) => (
                  <button key={sp.id} type="button" className="sam-btn" style={{ padding: '0.2rem 0.5rem', fontSize: '0.75rem' }}
                    onClick={() => addSpell(sp.id)} title={`add ${sp.id}`}>✨ {sp.name}</button>
                ))}
              </div>
            </div>
          )}
        </Panel>

        <Panel title="HP / MP Growth">
          <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
            Gained per level. Vanilla runs 1–5 (Wizard MP 5, Barbarian MP 2). 3 is the
            engine default — leave them all at 3 and nothing is written to your JSON.
          </div>
          <div className="grid grid-cols-2 gap-3">
            {HPMP_GROWTH_KEYS.map((k) => (
              <Field key={k} label={HPMP_GROWTH_LABELS[k]}>
                <NumberInput
                  value={hpMpGrowth[k]}
                  min={0}
                  onChange={(v) => setHpMpGrowth((p) => ({ ...p, [k]: v }))}
                />
              </Field>
            ))}
          </div>
          <div className="sam-divider" />
          <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
            <span className="sam-ok">Mana regen tuning</span> (optional). Heads up: vanilla
            scales mana regen off <b>PER</b> and <b>CHR</b> only — <b>INT does nothing</b>.
            Add an INT scaling below to make it matter.
          </div>
          <div className="grid grid-cols-2 gap-3">
            <Field label="Flat MP/min" hint="added before the multiplier">
              <NumberInput value={mpRegenBase} onChange={setMpRegenBase} />
            </Field>
            <Field label="Multiplier" hint="applied last; 1 = unchanged">
              <NumberInput value={mpRegenMult} min={0} onChange={setMpRegenMult} />
            </Field>
          </div>
          <div className="grid grid-cols-3 gap-2 mt-2">
            {ROLL_STATS.map((a) => (
              <Field key={a} label={`${ATTR_ICONS[a]} ${a}`} hint="MP/min per point">
                <NumberInput
                  value={mpRegenScaling[a]}
                  onChange={(v) => setMpRegenScaling((p) => ({ ...p, [a]: v }))}
                />
              </Field>
            ))}
          </div>
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

      {/* ------------------------------------------------- behavior script */}
      <Panel title="Behavior Script">
        <div className="text-xs mb-3" style={{ color: '#6b5a35' }}>
          Optional — this is what makes the class actually DO things. Ships as{' '}
          <span className="sam-mono">classes/{slugify(name)}.{scriptLang}</span> next to the class JSON and auto-loads in-game.
          Click a function, event, or snippet on the right to insert it; the{' '}
          <span className="sam-mono">API Reference</span> page has the full surface.
        </div>
        <ScriptEditor code={scriptCode} onCode={setScriptCode} lang={scriptLang} onLang={setScriptLang} />
      </Panel>

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
