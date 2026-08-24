/*
 * Race Editor — build a custom playable race (race.schema.json). S.A.M registers it
 * into a reserved race id (200-255) so it appears in the character-select race picker,
 * renders as an existing monster body in BOTH third- and first-person, and applies flat
 * attribute/HP/MP bonuses at character creation. Only the 18 host bodies below have a
 * proper first-person arm, so both views stay correct.
 */
import { useEffect, useMemo, useState } from 'react';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import { Panel, Field, TextInput, NumberInput, Select, SearchSelect, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';
import ScriptEditor from '@/components/ScriptEditor.jsx';
import { MONSTERS, SPELLS } from '@/data/samApi.js';
import { CLASS_SPELL_REF_PATTERN } from '@/data/schemas.js';

const ATTRS = ['STR', 'DEX', 'CON', 'INT', 'PER', 'CHR'];
const ATTR_ICONS = { STR: '💪', DEX: '🪶', CON: '❤️', INT: '📖', PER: '👁️', CHR: '🎭' };

// The 18 monster bodies with a dedicated first-person arm (correct in both views).
const HOST_BODIES = [
  'human', 'skeleton', 'vampire', 'succubus', 'incubus', 'goblin',
  'automaton', 'insectoid', 'goatman', 'gnome', 'gremlin', 'dryad',
  'myconid', 'salamander', 'troll', 'spider', 'imp', 'rat',
];
const cap = (s) => s.charAt(0).toUpperCase() + s.slice(1);

// The six limbs a race can supply its own model for. Order is head-down, so the panel
// reads like a body rather than like the engine's limb enum.
const LIMB_SLOTS = [
  ['head', 'Head', 'Always visible — no armour ever replaces it.'],
  ['torso', 'Torso', 'Covered by a breastplate.'],
  ['arm_right', 'Right arm', 'Covered by sleeves and gloves.'],
  ['arm_left', 'Left arm', 'Covered by sleeves and gloves.'],
  ['leg_right', 'Right leg', 'Covered by boots and greaves.'],
  ['leg_left', 'Left leg', 'Covered by boots and greaves.'],
];
const prettyMonster = (m) => m.split('_').map(cap).join(' ');

// A removable chip list. Same shape the class editor uses for starting spells, so the two
// editors read the same way.
function Chips({ items, icon, label = (x) => x, onRemove, empty }) {
  if (!items.length) {
    return <div className="text-xs mb-3 min-h-8" style={{ color: '#6b5a35' }}>{empty}</div>;
  }
  return (
    <div className="flex flex-wrap gap-2 mb-3 min-h-8">
      {items.map((x) => (
        <span key={x} className="sam-well px-2 py-1 text-sm inline-flex items-center gap-2"
          style={{ color: 'var(--color-parchment)' }}>
          {icon} {label(x)}
          <button type="button" className="sam-step sam-remove"
            style={{ width: 18, height: 18, fontSize: '0.7rem' }}
            onClick={() => onRemove(x)} aria-label={`remove ${x}`}>✕</button>
        </span>
      ))}
    </div>
  );
}

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

export default function RaceEditor() {
  const { meta, races, spells: modSpells, scripts, editing, dispatch } = useMod();
  const editDef = editing?.kind === 'race' ? races.find((r) => r.id === editing.id) : null;
  const existingScript = editDef ? scripts[editDef.id] : null;

  const [name, setName] = useState(editDef?.name ?? '');
  const [description, setDescription] = useState(editDef?.description ?? '');
  const [hostBody, setHostBody] = useState(editDef?.host_body ?? 'skeleton');
  const [mods, setMods] = useState(() =>
    Object.fromEntries([...ATTRS, 'HP', 'MP'].map((a) => [a, editDef?.stat_modifiers?.[a] ?? 0])));
  const [bloodDiet, setBloodDiet] = useState(editDef?.blood_diet ?? false);
  const [startingSpells, setStartingSpells] = useState(editDef?.starting_spells ?? []);
  // Declared allegiance. Empty is not "no allies" — it means "inherit the host body's
  // relations", which is why neither list is written to the JSON when it is empty.
  const [allies, setAllies] = useState(editDef?.allies ?? []);
  const [limbModels, setLimbModels] = useState(() =>
    Object.fromEntries(LIMB_SLOTS.map(([k]) => [k, editDef?.limb_models?.[k] ?? ''])));
  const [enemies, setEnemies] = useState(editDef?.enemies ?? []);
  const [spellError, setSpellError] = useState('');
  const [scriptLang, setScriptLang] = useState(existingScript?.lang ?? 'lua');
  const [scriptCode, setScriptCode] = useState(existingScript?.code ?? '');
  // Visual block-builder rules, so reopening restores the bricks. Editor-only (not exported).
  const [scriptBlocks, setScriptBlocks] = useState(existingScript?.blocks ?? null);
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');

  useEffect(() => {
    if (editing?.kind === 'race') dispatch({ type: 'clearEditing' });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const namespace = meta.namespace || 'mymod';
  const raceId = `${namespace}:${slugify(name)}`;
  const num = (v) => (v === '' || v == null ? undefined : Number(v));

  const buildDef = () => {
    const def = { id: raceId, name: name.trim(), host_body: hostBody };
    if (description.trim()) def.description = description.trim();
    const sm = {};
    for (const a of [...ATTRS, 'HP', 'MP']) { const v = num(mods[a]); if (v != null && v !== 0) sm[a] = v; }
    if (Object.keys(sm).length) def.stat_modifiers = sm;
    if (bloodDiet) def.blood_diet = true;
    if (startingSpells.length) def.starting_spells = startingSpells;
    const lm = {};
    for (const [k] of LIMB_SLOTS) { const v = (limbModels[k] ?? '').trim(); if (v) lm[k] = v; }
    if (Object.keys(lm).length) def.limb_models = lm;
    if (allies.length) def.allies = allies;
    if (enemies.length) def.enemies = enemies;
    return def;
  };

  // A type in both lists is an enemy in game (the engine says so in the log). Say it here
  // instead, while it is still one click to fix.
  const conflicts = allies.filter((m) => enemies.includes(m));

  const addSpell = (raw) => {
    const input = String(raw ?? '').trim();
    if (!input) return;
    const sp = input.includes(':') ? input.toLowerCase() : input.toUpperCase();
    if (!CLASS_SPELL_REF_PATTERN.test(sp)) {
      setSpellError('Use a SPELL_X constant or a custom "namespace:spell" id.');
      return;
    }
    setSpellError('');
    setStartingSpells((prev) => (prev.includes(sp) ? prev : [...prev, sp]));
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const res = validate('race', def);
    if (!res.valid) { setErrors(res.errors); return; }
    setErrors([]);
    dispatch({ type: 'saveRace', def });
    dispatch({ type: 'saveScript', classId: def.id, lang: scriptLang, code: scriptCode, blocks: scriptBlocks });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [name, description, hostBody, mods, bloodDiet, startingSpells, allies, enemies, limbModels, namespace]);
  const preview = useMemo(() => JSON.stringify(def, null, 2), [def]);
  const setMod = (a, v) => setMods((prev) => ({ ...prev, [a]: v }));

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <div>
        <TextInput value={name} onChange={setName} placeholder="Race name — e.g. Frostborn"
          style={{ fontSize: '1.5rem', padding: '0.7rem 1rem' }} aria-label="Race name" />
        <div className="mt-1 text-xs" style={{ color: '#6b5a35' }}>
          id: <span className="sam-mono">{raceId}</span> · assigned a race id 200-255 · appears in the
          {' '}character-select race picker
        </div>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="Body & flavour">
          <Field label="Host body" hint="The existing monster whose model this race wears — in both third- and first-person. These 18 are the fully-supported bodies.">
            <Select value={hostBody} onChange={setHostBody}
              options={HOST_BODIES.map((b) => ({ value: b, label: cap(b) }))} />
          </Field>
          <Field label="Description" hint="Shown in the picker's info panel.">
            <textarea className="sam-input" rows={3} value={description} onChange={(e) => setDescription(e.target.value)} placeholder="A nimble undead wanderer…" />
          </Field>
          <label className="flex items-center gap-2 mt-3 cursor-pointer text-sm" style={{ color: 'var(--color-parchment)' }}>
            <input type="checkbox" className="sam-check" checked={bloodDiet} onChange={(e) => setBloodDiet(e.target.checked)} />
            Blood diet (sustains on blood instead of food, like a vampire)
          </label>
        </Panel>

        <Panel title="Attribute bonuses">
          <div className="text-xs mb-2" style={{ color: '#8a7749' }}>
            Added at character creation on top of the class (may be negative). Vanilla races give no
            attribute bonuses, so these are the race's whole identity. 0 = no change.
          </div>
          <div className="grid grid-cols-2 gap-3">
            {ATTRS.map((a) => (
              <Field key={a} label={`${ATTR_ICONS[a]} ${a}`}>
                <NumberInput value={mods[a]} onChange={(v) => setMod(a, v)} />
              </Field>
            ))}
            <Field label="❤️ HP">
              <NumberInput value={mods.HP} onChange={(v) => setMod('HP', v)} />
            </Field>
            <Field label="✨ MP">
              <NumberInput value={mods.MP} onChange={(v) => setMod('MP', v)} />
            </Field>
          </div>
        </Panel>
      </div>

      <Panel title="Body (optional)">
        <div className="text-xs mb-3" style={{ color: '#8a7749' }}>
          Leave these blank and you wear the host body's own models, which is what most
          races want. Fill one in to replace just that limb, or all six for a body of your
          own. The host body still decides the skeleton: the animation, the limb positions,
          and which slots exist — these only change what is drawn in each slot.
          <br />
          Each takes a model you declared in <span className="sam-mono">mod.json</span>{' '}
          (<span className="sam-mono">{namespace}:name</span>), a path to your own .vox, or a
          raw vanilla model index. Remember <span className="sam-mono">models.txt</span> line
          N is index N-1.
        </div>
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
          {LIMB_SLOTS.map(([key, label, hint]) => (
            <Field key={key} label={label} hint={hint}>
              <TextInput value={limbModels[key]}
                onChange={(v) => setLimbModels((p) => ({ ...p, [key]: v }))}
                placeholder="blank = host body" />
            </Field>
          ))}
        </div>
        <div className="text-xs mt-3" style={{ color: '#6b5a35' }}>
          First-person view keeps the host body's arm — the game has no first-person model
          for most creatures, so there is nothing to swap it for.
        </div>
      </Panel>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="Allegiance">
          <div className="text-xs mb-3" style={{ color: '#8a7749' }}>
            Your race already inherits its host body's relations: a Goatman-bodied race is
            left alone by goatmen without doing anything here, and hated by humans and
            shopkeepers for the same reason. These two lists are for relations the host body
            does <b>not</b> have. Leave them empty to inherit its relations unchanged.
          </div>

          <div className="sam-label mb-1" style={{ color: '#8a6d2e' }}>Won't attack you</div>
          <Chips items={allies} icon="🤝" label={prettyMonster}
            onRemove={(m) => setAllies((p) => p.filter((x) => x !== m))}
            empty="Nothing added — the host body decides." />
          <SearchSelect
            options={MONSTERS.filter((m) => !allies.includes(m))}
            onPick={(m) => setAllies((p) => (p.includes(m) ? p : [...p, m]))}
            placeholder="Search creatures… e.g. gnome" />

          <div className="sam-label mb-1 mt-4" style={{ color: '#8a6d2e' }}>Always hostile</div>
          <Chips items={enemies} icon="⚔️" label={prettyMonster}
            onRemove={(m) => setEnemies((p) => p.filter((x) => x !== m))}
            empty="Nothing added — the host body decides." />
          <SearchSelect
            options={MONSTERS.filter((m) => !enemies.includes(m))}
            onPick={(m) => setEnemies((p) => (p.includes(m) ? p : [...p, m]))}
            placeholder="Search creatures… e.g. shopkeeper" />

          {conflicts.length > 0 && (
            <div className="sam-error text-sm mt-3">
              {conflicts.map(prettyMonster).join(', ')} {conflicts.length === 1 ? 'is' : 'are'} in
              both lists. In game that means hostile — remove it from one side to be sure.
            </div>
          )}
        </Panel>

        <Panel title="Innate Spells">
          <div className="text-xs mb-3" style={{ color: '#8a7749' }}>
            Known from character creation, whatever class you pick — the racial half of a
            spellbook. Vanilla races use this for things like a vampire's Bleed.
          </div>
          <Chips items={startingSpells} icon="✨"
            onRemove={(sp) => setStartingSpells((p) => p.filter((x) => x !== sp))}
            empty="No innate spells." />
          <SearchSelect
            options={SPELLS.filter((sp) => !startingSpells.includes(sp))}
            onPick={addSpell}
            placeholder="Search spells… (or type mymod:spell)"
            allowCustom />
          {spellError && <div className="sam-error text-sm mt-1">{spellError}</div>}
          {modSpells.length > 0 && (
            <div className="mt-3">
              <div className="sam-label mb-1" style={{ color: '#8a6d2e' }}>Your custom spells</div>
              <div className="flex flex-wrap gap-1">
                {modSpells.map((sp) => (
                  <button key={sp.id} type="button" className="sam-btn"
                    style={{ padding: '0.2rem 0.5rem', fontSize: '0.75rem' }}
                    onClick={() => addSpell(sp.id)} title={`add ${sp.id}`}>✨ {sp.name}</button>
                ))}
              </div>
            </div>
          )}
        </Panel>
      </div>

      <Panel title="Behavior Script (optional)">
        <div className="text-xs mb-2" style={{ color: '#8a7749' }}>
          Ships as <span className="sam-mono">races/{slugify(name)}.{scriptLang}</span> next to the race JSON and auto-loads
          in-game, with the same freedom class scripts have. React to any event hook and gate to this race with{' '}
          <span className="sam-mono">sam_get_race(player) == "{raceId}"</span>.
        </div>
        <ScriptEditor code={scriptCode} onCode={setScriptCode} lang={scriptLang} onLang={setScriptLang} pathHint="races/<race>"
          blocks={scriptBlocks} onBlocks={setScriptBlocks} />
      </Panel>

      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> — see Mod Builder.</SavedNote>}
        <GoldButton tone="green" onClick={save} disabled={!name.trim()}>🧬 Save Race</GoldButton>
      </div>

      <Panel title="Live JSON Preview" bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>{preview}</pre>
      </Panel>
    </div>
  );
}
