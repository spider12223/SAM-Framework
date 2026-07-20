/*
 * Effect Editor — build a custom status effect (effect.schema.json). S.A.M registers it
 * into a reserved effect slot (135-159) so it applies REAL mechanics: flat attribute
 * modifiers, a move-speed multiplier and per-second HP/MP deltas while active, plus a HUD
 * icon. Apply it in-game with sam_apply_effect(player, "namespace:effect", ticks), from a
 * script or the visual block builder's "apply a status effect" block.
 */
import { useEffect, useMemo, useState } from 'react';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, NumberInput, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';

const ATTRS = ['STR', 'DEX', 'CON', 'INT', 'PER', 'CHR'];
const ATTR_ICONS = { STR: '💪', DEX: '🪶', CON: '❤️', INT: '📖', PER: '👁️', CHR: '🎭' };
const GRANT_FLAGS = ['LEVITATING', 'INVISIBLE', 'TELEPATH'];

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

export default function EffectEditor() {
  const { meta, effects, editing, dispatch } = useMod();
  const editDef = editing?.kind === 'effect' ? effects.find((e) => e.id === editing.id) : null;

  const [name, setName] = useState(editDef?.name ?? '');
  const [tooltip, setTooltip] = useState(editDef?.tooltip ?? '');
  const [icon, setIcon] = useState(editDef?.icon ?? '');
  const [duration, setDuration] = useState(editDef?.duration_ticks ?? 500);
  const [mods, setMods] = useState(() =>
    Object.fromEntries(ATTRS.map((a) => [a, editDef?.stat_modifiers?.[a] ?? 0])));
  const [speedMult, setSpeedMult] = useState(editDef?.move_speed_mult ?? 1);
  const [hpSec, setHpSec] = useState(editDef?.hp_per_second ?? 0);
  const [mpSec, setMpSec] = useState(editDef?.mp_per_second ?? 0);
  const [hudHidden, setHudHidden] = useState(editDef?.hud_hidden ?? false);
  const [acMod, setAcMod] = useState(editDef?.ac_mod ?? 0);
  const [damageMult, setDamageMult] = useState(editDef?.damage_mult ?? 1);
  const [grants, setGrants] = useState(editDef?.grants ?? []);
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');

  // Consume the edit handoff so a later navigation starts blank.
  useEffect(() => {
    if (editing?.kind === 'effect') dispatch({ type: 'clearEditing' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const namespace = meta.namespace || 'mymod';
  const effectId = `${namespace}:${slugify(name)}`;
  const num = (v) => (v === '' || v == null ? undefined : Number(v));

  const buildDef = () => {
    const def = { id: effectId, name: name.trim() };
    if (tooltip.trim()) def.tooltip = tooltip.trim();
    if (icon.trim()) def.icon = icon.trim();
    if (num(duration) != null && Number(duration) > 0) def.duration_ticks = Number(duration);
    const sm = {};
    for (const a of ATTRS) { const v = num(mods[a]); if (v != null && v !== 0) sm[a] = v; }
    if (Object.keys(sm).length) def.stat_modifiers = sm;
    if (num(speedMult) != null && Number(speedMult) !== 1) def.move_speed_mult = Number(speedMult);
    if (num(hpSec) != null && Number(hpSec) !== 0) def.hp_per_second = Number(hpSec);
    if (num(mpSec) != null && Number(mpSec) !== 0) def.mp_per_second = Number(mpSec);
    if (num(acMod) != null && Number(acMod) !== 0) def.ac_mod = Number(acMod);
    if (num(damageMult) != null && Number(damageMult) !== 1) def.damage_mult = Number(damageMult);
    if (Array.isArray(grants) && grants.length) def.grants = grants;
    if (hudHidden) def.hud_hidden = true;
    return def;
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const res = validate('effect', def);
    if (!res.valid) { setErrors(res.errors); return; }
    setErrors([]);
    dispatch({ type: 'saveEffect', def });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [name, tooltip, icon, duration, mods, speedMult, hpSec, mpSec, hudHidden, acMod, damageMult, grants, namespace]);
  const preview = useMemo(() => JSON.stringify(def, null, 2), [def]);

  const setMod = (a, v) => setMods((prev) => ({ ...prev, [a]: v }));

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <div>
        <TextInput value={name} onChange={setName} placeholder="Effect name — e.g. Frostbite"
          style={{ fontSize: '1.5rem', padding: '0.7rem 1rem' }} aria-label="Effect name" />
        <div className="mt-1 text-xs" style={{ color: '#6b5a35' }}>
          id: <span className="sam-mono">{effectId}</span> · assigned an engine slot 135-159 · apply with{' '}
          <span className="sam-mono">sam_apply_effect(player, "{effectId}", ticks)</span>
        </div>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="While active — attribute modifiers">
          <div className="text-xs mb-2" style={{ color: '#8a7749' }}>
            Added to the affected creature's attributes while the effect is on (may be negative). 0 = no change.
          </div>
          <div className="grid grid-cols-2 gap-3">
            {ATTRS.map((a) => (
              <Field key={a} label={`${ATTR_ICONS[a]} ${a}`}>
                <NumberInput value={mods[a]} onChange={(v) => setMod(a, v)} />
              </Field>
            ))}
          </div>
        </Panel>

        <Panel title="While active — over time & movement">
          <Field label="Move speed multiplier" hint="1.0 = normal, 0.8 = 20% slower, 1.5 = 50% faster. The engine clamps the top end.">
            <NumberInput value={speedMult} min={0.1} max={3} step={0.1} onChange={setSpeedMult} />
          </Field>
          <div className="grid grid-cols-2 gap-3">
            <Field label="HP / second" hint="Negative drains (can kill), positive heals.">
              <NumberInput value={hpSec} onChange={setHpSec} />
            </Field>
            <Field label="MP / second" hint="Negative drains, positive restores.">
              <NumberInput value={mpSec} onChange={setMpSec} />
            </Field>
          </div>
          <div className="grid grid-cols-2 gap-3 mt-3">
            <Field label="Armor (AC) modifier" hint="Flat defense while active. Positive hardens, negative exposes.">
              <NumberInput value={acMod} onChange={setAcMod} />
            </Field>
            <Field label="Melee damage multiplier" hint="Scales the wearer's outgoing melee damage. 1.0 = normal, 1.5 = +50%, 0.5 = half.">
              <NumberInput value={damageMult} min={0} step={0.1} onChange={setDamageMult} />
            </Field>
          </div>
          <div className="sam-divider" />
          <Field label="Default duration (ticks)" hint="50 ticks = 1 second. The ticks passed to sam_apply_effect override this.">
            <NumberInput value={duration} min={0} onChange={setDuration} />
          </Field>
          <div className="sam-divider" />
          <Field label="Also grants (while active)" hint="The effect also applies these vanilla flags while it lasts, then they fade shortly after it ends.">
            <div className="flex flex-wrap gap-3 pt-1">
              {GRANT_FLAGS.map((g) => (
                <label key={g} className="flex items-center gap-1.5 cursor-pointer text-sm" style={{ color: 'var(--color-parchment)' }}>
                  <input type="checkbox" className="sam-check" checked={grants.includes(g)}
                    onChange={(e) => setGrants((prev) => e.target.checked ? [...prev, g] : prev.filter((x) => x !== g))} />
                  {g}
                </label>
              ))}
            </div>
          </Field>
        </Panel>
      </div>

      <Panel title="Presentation">
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
          <Field label="Tooltip" hint="Shown in the effect's HUD tooltip.">
            <textarea className="sam-input" rows={2} value={tooltip} onChange={(e) => setTooltip(e.target.value)} placeholder="Chilled to the bone…" />
          </Field>
          <div>
            <Field label="Icon path (mod-relative)" hint="Optional PNG for the HUD icon. A generic icon is used if left blank.">
              <TextInput value={icon} onChange={setIcon} placeholder="effects/frostbite.png" />
            </Field>
            <label className="flex items-center gap-2 mt-3 cursor-pointer text-sm" style={{ color: 'var(--color-parchment)' }}>
              <input type="checkbox" className="sam-check" checked={hudHidden} onChange={(e) => setHudHidden(e.target.checked)} />
              Hide from the status bar (still works, just no icon)
            </label>
          </div>
        </div>
      </Panel>

      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> — see Mod Builder.</SavedNote>}
        <GoldButton tone="red" onClick={save} disabled={!name.trim()}>🌀 Save Effect</GoldButton>
      </div>

      <Panel title="Live JSON Preview" bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>{preview}</pre>
      </Panel>
    </div>
  );
}
