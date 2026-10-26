/*
 * Spell Editor — build a custom spell (spell.schema.json). S.A.M registers it at
 * a reserved runtime id (>=2000) so a class's starting_spells (or sam_grant_spell)
 * can reference it by "namespace:spell". It casts like the vanilla spell its payload names,
 * and its on-hit effect can be a built-in status or one of this mod's own custom effects.
 */
import { useEffect, useMemo, useRef, useState } from 'react';
import { SPELL_PAYLOADS, SPELL_PROJECTILE_TYPES, ITEM_TYPES } from '@/data/schemas.js';
import { EFFECTS } from '@/data/samApi.js';
import { validate } from '@/lib/validate.js';
import { carryUnknown } from '@/lib/editorKeys.js';
import { checkOnHitEffect, describeOnHitEffect } from '@/lib/spellEffectRef.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, NumberInput, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

const MAX_ICON_BYTES = 512 * 1024; // a spell icon is a small picture; anything bigger is a mistake

// The three schools of magic. A custom spell was always Sorcery; the others change which skill is
// needed to learn it, which skill a cast trains, and which stats add to its power.
const SCHOOLS = [
  { id: 'sorcery', label: 'Sorcery', hint: 'Power from INT. The default.' },
  { id: 'mysticism', label: 'Mysticism', hint: 'Power from INT and CHR.' },
  { id: 'thaumaturgy', label: 'Thaumaturgy', hint: 'Power from INT and CON.' },
];
const REQ_STATS = ['STR', 'DEX', 'CON', 'INT', 'PER', 'CHR'];
const REQ_SKILLS = ['PRO_LOCKPICKING', 'PRO_STEALTH', 'PRO_TRADING', 'PRO_APPRAISAL', 'PRO_THAUMATURGY',
  'PRO_LEADERSHIP', 'PRO_MYSTICISM', 'PRO_SORCERY', 'PRO_RANGED', 'PRO_SWORD', 'PRO_MACE',
  'PRO_AXE', 'PRO_POLEARM', 'PRO_SHIELD', 'PRO_UNARMED', 'PRO_ALCHEMY'];
const skillWord = (k) => k.replace(/^PRO_/, '').charAt(0) + k.replace(/^PRO_/, '').slice(1).toLowerCase();
const TIERS = ['I', 'II', 'III', 'IV', 'V', 'VI'];
const BOOK_MODELS = ITEM_TYPES.filter((t) => /^SPELLBOOK_/.test(t));

export default function SpellEditor() {
  const { meta, spells, effects, items, editing, dispatch } = useMod();
  const editDef = editing?.kind === 'spell' ? spells.find((s) => s.id === editing.id) : null;

  // The def as it was when this page opened. `editDef` is derived from `editing`, and the
  // effect below clears `editing` on mount -- so by the time Save is clicked `editDef` is
  // already null. Saving has to carry the fields this editor cannot show, so it needs the
  // definition it started from, captured once.
  const [openedDef] = useState(() => editDef ?? null);

  const [name, setName] = useState(editDef?.name ?? '');
  const [description, setDescription] = useState(editDef?.description ?? '');
  const [manaCost, setManaCost] = useState(editDef?.mana_cost ?? 1);
  const [projectile, setProjectile] = useState(editDef?.projectile_type ?? 'missile');
  const [payload, setPayload] = useState(editDef?.payload ?? SPELL_PAYLOADS[0]);
  const [damageMin, setDamageMin] = useState(editDef?.damage_min ?? '');
  const [damageMax, setDamageMax] = useState(editDef?.damage_max ?? '');
  const [range, setRange] = useState(editDef?.range ?? '');
  const [speed, setSpeed] = useState(editDef?.speed ?? '');
  const [onHitEffect, setOnHitEffect] = useState(editDef?.on_hit_effect ?? '');
  const [onHitDuration, setOnHitDuration] = useState(editDef?.on_hit_duration ?? '');
  const [onHitChance, setOnHitChance] = useState(editDef?.on_hit_chance ?? '');
  const [icon, setIcon] = useState(editDef?.icon ?? '');
  // The picture itself, only when one was uploaded in this session. A spell reopened from the
  // mod already has its PNG among the mod's assets, so there is nothing to re-send for it.
  const [iconData, setIconData] = useState('');
  const [iconError, setIconError] = useState('');
  const iconFileRef = useRef(null);
  const [startingSpell, setStartingSpell] = useState(editDef?.starting_spell ?? false);
  const [school, setSchool] = useState(editDef?.school ?? 'sorcery');
  const [difficulty, setDifficulty] = useState(editDef?.difficulty ?? '');
  // learn_requirements: blank = no requirement. Stats are six boxes; skills are a short list.
  const [reqStats, setReqStats] = useState(() =>
    Object.fromEntries(REQ_STATS.map((s) => [s, editDef?.learn_requirements?.stats?.[s] ?? ''])));
  const [reqSkills, setReqSkills] = useState(() =>
    Object.entries(editDef?.learn_requirements?.skills ?? {}).map(([skill, value]) => ({ skill, value })));
  const [skillPick, setSkillPick] = useState(REQ_SKILLS[0]);
  const [skillVal, setSkillVal] = useState(10);
  // "Also make a spellbook": on save, an item that teaches this spell and can drop in the dungeon.
  const [makeBook, setMakeBook] = useState(false);
  const [bookModel, setBookModel] = useState('SPELLBOOK_FORCEBOLT');
  const [bookInLoot, setBookInLoot] = useState(true);
  const [bookLevel, setBookLevel] = useState(3);
  const [bookGold, setBookGold] = useState(100);
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');

  // Consume the edit handoff so a later navigation starts blank.
  useEffect(() => {
    if (editing?.kind === 'spell') dispatch({ type: 'clearEditing' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const namespace = meta.namespace || 'mymod';
  const spellId = `${namespace}:${slugify(name)}`;

  const num = (v) => (v === '' || v == null ? undefined : Number(v));

  const buildDef = () => {
    const def = { id: spellId, name: name.trim(), payload };
    if (description.trim()) def.description = description.trim();
    if (manaCost !== '' && manaCost != null) def.mana_cost = Number(manaCost);
    if (projectile !== 'missile') def.projectile_type = projectile;
    if (num(damageMin) != null) def.damage_min = num(damageMin);
    if (num(damageMax) != null) def.damage_max = num(damageMax);
    if (num(range) != null) def.range = num(range);
    if (num(speed) != null) def.speed = num(speed);
    if (onHitEffect.trim()) def.on_hit_effect = onHitEffect.trim();
    if (num(onHitDuration) != null) def.on_hit_duration = num(onHitDuration);
    if (num(onHitChance) != null) def.on_hit_chance = num(onHitChance);
    if (icon.trim()) def.icon = icon.trim();
    if (startingSpell) def.starting_spell = true;
    if (school !== 'sorcery') def.school = school;
    if (num(difficulty) != null && num(difficulty) > 0) def.difficulty = num(difficulty);
    const lrStats = Object.fromEntries(REQ_STATS.filter((s) => num(reqStats[s]) > 0).map((s) => [s, num(reqStats[s])]));
    const lrSkills = Object.fromEntries(reqSkills.filter((r) => num(r.value) > 0).map((r) => [r.skill, num(r.value)]));
    const lr = {};
    if (Object.keys(lrStats).length) lr.stats = lrStats;
    if (Object.keys(lrSkills).length) lr.skills = lrSkills;
    if (Object.keys(lr).length) def.learn_requirements = lr;
    // Carry anything this editor has no control for straight through -- a field the
    // author hand-wrote, or one a later schema adds, must survive a save here.
    return carryUnknown(openedDef, def, 'spell');
  };

  // The id the Item Editor itself would give a book named "Spellbook of <name>" (it derives the id from
  // the name), so opening the book there and saving it later changes this item instead of adding a twin.
  const bookId = `${namespace}:spellbook_of_${slugify(name)}`;
  const existingBook = (items ?? []).find((it) => it.id === bookId);
  const bookDef = () => {
    if (existingBook) {
      return { ...existingBook, attributes: { ...(existingBook.attributes ?? {}), spellbook_spell: spellId } };
    }
    const flavour = bookModel.replace(/^SPELLBOOK_/, '').toLowerCase().replace(/_/g, ' ');
    return {
      id: bookId,
      name_identified: `Spellbook of ${name.trim()}`,
      name_unidentified: `${flavour} spellbook`,
      description: `Reading it teaches ${name.trim()}.`,
      category: 'SPELLBOOK',
      model_from_item: bookModel,
      weight: 3,
      gold_value: num(bookGold) ?? 0,
      level: bookInLoot ? (num(bookLevel) ?? 0) : -1,
      attributes: { spellbook_spell: spellId },
    };
  };

  const onIconFile = (e) => {
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    if (file.size > MAX_ICON_BYTES) {
      setIconError(`That PNG is ${Math.round(file.size / 1024)} KB. A spell icon is a small picture; keep it under 512 KB.`);
      return;
    }
    const reader = new FileReader();
    reader.onload = () => {
      setIconError('');
      setIcon((p) => p.trim() || `spells/${slugify(name)}.png`);
      setIconData(String(reader.result ?? ''));
    };
    reader.readAsDataURL(file);
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const res = validate('spell', def);
    if (!res.valid) { setErrors(res.errors); return; }
    setErrors([]);
    // The spellbook that teaches it, made in the same save so the two can never drift apart. A
    // book that already exists keeps everything the author set on it; only its spell is refreshed.
    if (makeBook) {
      const book = bookDef();
      const bres = validate('item', book);
      if (!bres.valid) { setErrors(bres.errors); return; }
      dispatch({ type: 'saveItem', def: book });
    }
    // Ship the uploaded PNG as a mod asset at the icon path.
    if (def.icon && iconData) dispatch({ type: 'setAsset', path: def.icon, dataUrl: iconData });
    dispatch({ type: 'saveSpell', def });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [name, description, manaCost, projectile, payload, damageMin, damageMax, range, speed, onHitEffect, onHitDuration, onHitChance, icon, startingSpell, school, difficulty, reqStats, reqSkills, namespace]);
  const preview = useMemo(() => JSON.stringify(def, null, 2), [def]);

  // The mod's own effects, by the exact id the game looks up. The text box accepts anything, so
  // say plainly when what was typed will not find an effect (the game ignores it, silently).
  const customEffectIds = useMemo(() => (effects ?? []).map((e) => e.id), [effects]);
  const onHitNote = describeOnHitEffect(checkOnHitEffect(onHitEffect, customEffectIds));

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <div>
        <TextInput value={name} onChange={setName} placeholder="Spell name — e.g. Shadow Bolt"
          style={{ fontSize: '1.5rem', padding: '0.7rem 1rem' }} aria-label="Spell name" />
        <div className="mt-1 text-xs" style={{ color: '#6b5a35' }}>
          id: <span className="sam-mono">{spellId}</span> (namespace from the Mod Builder) · runtime id assigned ≥2000
        </div>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="Core">
          <Field label="Payload (effect element)" hint="Maps 1:1 to a real Barony spell element.">
            <select className="sam-input" value={payload} onChange={(e) => setPayload(e.target.value)}>
              {SPELL_PAYLOADS.map((p) => <option key={p} value={p}>{p}</option>)}
            </select>
          </Field>
          <Field label="Projectile" hint="How the spell is delivered.">
            <select className="sam-input" value={projectile} onChange={(e) => setProjectile(e.target.value)}>
              {SPELL_PROJECTILE_TYPES.map((p) => <option key={p} value={p}>{p}</option>)}
            </select>
          </Field>
          <div className="grid grid-cols-2 gap-3">
            <Field label="Mana cost"><NumberInput value={manaCost} min={0} onChange={setManaCost} /></Field>
            <Field label="Range"><NumberInput value={range} min={0} onChange={setRange} /></Field>
            <Field label="Damage min"><NumberInput value={damageMin} min={0} onChange={setDamageMin} /></Field>
            <Field label="Damage max"><NumberInput value={damageMax} min={0} onChange={setDamageMax} /></Field>
            <Field label="Projectile speed"><NumberInput value={speed} min={0} onChange={setSpeed} /></Field>
          </div>
          <div className="sam-divider" />
          <Field label="Description"><textarea className="sam-input" rows={2} value={description} onChange={(e) => setDescription(e.target.value)} placeholder="A bolt of condensed shadow…" /></Field>
        </Panel>

        <Panel title="On-Hit & Presentation">
          <Field
            label="On-hit effect"
            hint={(
              <>
                A status applied to whatever the spell hits: pick one of your own effects from the
                list (they look like <span className="sam-mono">{namespace}:plague</span>) or a built-in one
                like <span className="sam-mono">EFF_POISONED</span>.
                {onHitNote && (
                  <span className={onHitNote.tone === 'warn' ? 'sam-error block mt-1' : 'block mt-1'}
                    style={onHitNote.tone === 'ok' ? { color: '#9dc76a' } : undefined}>
                    {onHitNote.tone === 'ok' ? '✓ ' : '⚠ '}{onHitNote.text}
                  </span>
                )}
              </>
            )}
          >
            <TextInput value={onHitEffect} onChange={setOnHitEffect} placeholder="EFF_SLOW" list="sam-onhit-effects" />
            <datalist id="sam-onhit-effects">
              {customEffectIds.map((id) => <option key={id} value={id} />)}
              {EFFECTS.map((n) => <option key={n} value={`EFF_${n}`} />)}
            </datalist>
          </Field>
          <div className="grid grid-cols-2 gap-3">
            <Field label="Effect duration (ticks)" hint="50 ticks = 1 second."><NumberInput value={onHitDuration} min={0} onChange={setOnHitDuration} /></Field>
            <Field label="Effect chance (%)"><NumberInput value={onHitChance} min={0} onChange={setOnHitChance} /></Field>
          </div>
          <div className="sam-divider" />
          <Field
            label="Icon path (mod-relative)"
            hint="PNG drawn for the spell on the hotbar, in the spell list and in tooltips. Write the path from the folder that holds mod.json (spells/shadow_bolt.png), with no leading slash and no mods/yourfolder/ in front. If the game cannot find the file it shows the generic spell orb and sam_log.txt names the path it looked for. Restart the game after changing the picture."
          >
            <TextInput value={icon} onChange={setIcon} placeholder="spells/shadow_bolt.png" />
          </Field>
          <div className="flex items-center gap-3 mt-2">
            <GoldButton onClick={() => iconFileRef.current?.click()}>🖼 Upload PNG</GoldButton>
            <input ref={iconFileRef} type="file" accept="image/png" className="hidden" onChange={onIconFile} />
            {iconData && <img src={iconData} alt="Spell icon" style={{ width: 40, height: 40, imageRendering: 'pixelated' }} />}
            {iconData && (
              <span className="text-xs" style={{ color: '#6b5a35' }}>
                PNG loaded. It will export at <span className="sam-mono">{icon.trim() || `spells/${slugify(name)}.png`}</span>.
              </span>
            )}
          </div>
          {iconError && <div className="sam-error text-sm mt-1">{iconError}</div>}
          <label className="flex items-center gap-2 mt-3 cursor-pointer text-sm" style={{ color: 'var(--color-parchment)' }}>
            <input type="checkbox" className="sam-check" checked={startingSpell} onChange={(e) => setStartingSpell(e.target.checked)} />
            Intended as a class starting spell
          </label>
          <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>
            To actually grant it, add <span className="sam-mono">{spellId}</span> to a class's Starting Spells.
          </div>
        </Panel>
      </div>

      <Panel title="Learning & Spellbook">
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
          <div>
            <Field label="School" hint={SCHOOLS.find((s) => s.id === school)?.hint}>
              <select className="sam-input" value={school} onChange={(e) => setSchool(e.target.value)}>
                {SCHOOLS.map((s) => <option key={s.id} value={s.id}>{s.label}</option>)}
              </select>
            </Field>
            <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
              The school picks the skill the learn difficulty is checked against (a difficulty of 0 checks none)
              and the skill a cast trains, and the stats that add to the spell's power. It also picks the school
              icon in the tooltip.
            </div>
            <Field label="Learn difficulty"
              hint="Your skill in the spell's school plus INT must reach this to learn it from a book. 0 means anyone can learn it. It also sets the spell's level (difficulty divided by 20).">
              <NumberInput value={difficulty} min={0} onChange={setDifficulty} />
            </Field>
            <div className="text-xs" style={{ color: '#8a7749' }}>
              Spell level: <span className="sam-mono">{TIERS[Math.min(5, Math.floor((Number(difficulty) || 0) / 20))]}</span>
            </div>
          </div>
          <div>
            <div className="sam-label mb-1" style={{ fontSize: '0.78rem' }}>Also needs, to learn it from a book</div>
            <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>Leave a box empty for no requirement.</div>
            <div className="grid grid-cols-3 gap-3">
              {REQ_STATS.map((s) => (
                <Field key={s} label={s}>
                  <NumberInput value={reqStats[s]} min={0} onChange={(v) => setReqStats((p) => ({ ...p, [s]: v }))} />
                </Field>
              ))}
            </div>
            <div className="flex flex-wrap gap-2 my-2 min-h-8">
              {reqSkills.length === 0 && <div className="text-sm" style={{ color: '#6b5a35' }}>No skill requirement.</div>}
              {reqSkills.map((r) => (
                <span key={r.skill} className="sam-well px-2 py-1 text-sm inline-flex items-center gap-2" style={{ color: 'var(--color-parchment)' }}>
                  {skillWord(r.skill)} {r.value}
                  <button type="button" className="sam-step sam-remove" style={{ width: 18, height: 18, fontSize: '0.7rem' }}
                    onClick={() => setReqSkills((p) => p.filter((x) => x.skill !== r.skill))} aria-label={`remove ${r.skill}`}>✕</button>
                </span>
              ))}
            </div>
            <div className="flex gap-2 items-end">
              <Field label="Skill" className="flex-1">
                <select className="sam-input" value={skillPick} onChange={(e) => setSkillPick(e.target.value)}>
                  {REQ_SKILLS.map((k) => <option key={k} value={k}>{skillWord(k)}</option>)}
                </select>
              </Field>
              <Field label="Level" className="w-24"><NumberInput value={skillVal} min={1} max={100} onChange={setSkillVal} /></Field>
              <GoldButton onClick={() => {
                const v = Math.trunc(Number(skillVal) || 0);
                if (v < 1) return;
                setReqSkills((p) => [...p.filter((x) => x.skill !== skillPick), { skill: skillPick, value: v }]);
              }}>Add</GoldButton>
            </div>
            <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>
              Only checked when a spellbook of this spell is read. A spell a class, a race or a script hands over is always learned.
            </div>
          </div>
        </div>
        <div className="sam-divider" />
        <label className="flex items-center gap-2 cursor-pointer text-sm" style={{ color: 'var(--color-parchment)' }}>
          <input type="checkbox" className="sam-check" checked={makeBook} onChange={(e) => setMakeBook(e.target.checked)} />
          Also make a spellbook that teaches this spell
        </label>
        {makeBook && (
          existingBook ? (
            <div className="text-xs mt-2" style={{ color: '#8a7749' }}>
              <span className="sam-mono">{bookId}</span> already exists in this mod. Saving points it at this spell and leaves everything else on it as you set it.
            </div>
          ) : (
            <div className="mt-3 grid grid-cols-1 lg:grid-cols-2 gap-3">
              <Field label="Book look" hint="Borrows the model of a vanilla spellbook.">
                <select className="sam-input" value={bookModel} onChange={(e) => setBookModel(e.target.value)}>
                  {BOOK_MODELS.map((m) => <option key={m} value={m}>{m.replace(/^SPELLBOOK_/, '').toLowerCase().replace(/_/g, ' ')}</option>)}
                </select>
              </Field>
              <Field label="Gold value"><NumberInput value={bookGold} min={0} onChange={setBookGold} /></Field>
              <label className="flex items-center gap-2 cursor-pointer text-sm" style={{ color: 'var(--color-parchment)' }}>
                <input type="checkbox" className="sam-check" checked={bookInLoot} onChange={(e) => setBookInLoot(e.target.checked)} />
                Can be found in the dungeon
              </label>
              {bookInLoot && (
                <Field label="Appears from dungeon level" hint="Chests, shops and monsters can carry it from this floor on.">
                  <NumberInput value={bookLevel} min={0} onChange={setBookLevel} />
                </Field>
              )}
              <div className="text-xs lg:col-span-2" style={{ color: '#6b5a35' }}>
                Creates <span className="sam-mono">{bookId}</span> as an item you can still edit in the Item Editor.
              </div>
            </div>
          )
        )}
      </Panel>

      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> — see Mod Builder.</SavedNote>}
        <GoldButton tone="red" onClick={save} disabled={!name.trim()}>✨ Save Spell</GoldButton>
      </div>

      <Panel title="Live JSON Preview" bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>{preview}</pre>
      </Panel>
    </div>
  );
}
