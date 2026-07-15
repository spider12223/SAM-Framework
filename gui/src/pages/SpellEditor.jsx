/*
 * Spell Editor — build a custom spell (spell.schema.json). S.A.M registers it at
 * a reserved runtime id (>=2000) so a class's starting_spells (or sam_grant_spell)
 * can reference it by "namespace:spell". NOTE: casting behavior lands in a later
 * engine update; metadata is parsed + stored now.
 */
import { useEffect, useMemo, useState } from 'react';
import { SPELL_PAYLOADS, SPELL_PROJECTILE_TYPES } from '@/data/schemas.js';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, NumberInput, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

export default function SpellEditor() {
  const { meta, spells, editing, dispatch } = useMod();
  const editDef = editing?.kind === 'spell' ? spells.find((s) => s.id === editing.id) : null;

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
  const [startingSpell, setStartingSpell] = useState(editDef?.starting_spell ?? false);
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
    return def;
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const res = validate('spell', def);
    if (!res.valid) { setErrors(res.errors); return; }
    setErrors([]);
    dispatch({ type: 'saveSpell', def });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [name, description, manaCost, projectile, payload, damageMin, damageMax, range, speed, onHitEffect, onHitDuration, onHitChance, icon, startingSpell, namespace]);
  const preview = useMemo(() => JSON.stringify(def, null, 2), [def]);

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
          <Field label="On-hit effect" hint="An EFF_ status name applied to a struck target (e.g. EFF_POISONED).">
            <TextInput value={onHitEffect} onChange={setOnHitEffect} placeholder="EFF_SLOW" />
          </Field>
          <div className="grid grid-cols-2 gap-3">
            <Field label="Effect duration (ticks)" hint="50 ticks = 1 second."><NumberInput value={onHitDuration} min={0} onChange={setOnHitDuration} /></Field>
            <Field label="Effect chance (%)"><NumberInput value={onHitChance} min={0} onChange={setOnHitChance} /></Field>
          </div>
          <div className="sam-divider" />
          <Field label="Icon path (mod-relative)" hint="PNG shown for the spell (when the spell UI lands).">
            <TextInput value={icon} onChange={setIcon} placeholder="spells/shadow_bolt.png" />
          </Field>
          <label className="flex items-center gap-2 mt-3 cursor-pointer text-sm" style={{ color: 'var(--color-parchment)' }}>
            <input type="checkbox" className="sam-check" checked={startingSpell} onChange={(e) => setStartingSpell(e.target.checked)} />
            Intended as a class starting spell
          </label>
          <div className="text-xs mt-2" style={{ color: '#6b5a35' }}>
            To actually grant it, add <span className="sam-mono">{spellId}</span> to a class's Starting Spells.
          </div>
        </Panel>
      </div>

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
