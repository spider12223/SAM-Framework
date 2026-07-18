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
import { Panel, Field, TextInput, NumberInput, Select, GoldButton, ErrorList, SavedNote } from '@/components/ui.jsx';
import ScriptEditor from '@/components/ScriptEditor.jsx';

const ATTRS = ['STR', 'DEX', 'CON', 'INT', 'PER', 'CHR'];
const ATTR_ICONS = { STR: '💪', DEX: '🪶', CON: '❤️', INT: '📖', PER: '👁️', CHR: '🎭' };

// The 18 monster bodies with a dedicated first-person arm (correct in both views).
const HOST_BODIES = [
  'human', 'skeleton', 'vampire', 'succubus', 'incubus', 'goblin',
  'automaton', 'insectoid', 'goatman', 'gnome', 'gremlin', 'dryad',
  'myconid', 'salamander', 'troll', 'spider', 'imp', 'rat',
];
const cap = (s) => s.charAt(0).toUpperCase() + s.slice(1);

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

export default function RaceEditor() {
  const { meta, races, scripts, editing, dispatch } = useMod();
  const editDef = editing?.kind === 'race' ? races.find((r) => r.id === editing.id) : null;
  const existingScript = editDef ? scripts[editDef.id] : null;

  const [name, setName] = useState(editDef?.name ?? '');
  const [description, setDescription] = useState(editDef?.description ?? '');
  const [hostBody, setHostBody] = useState(editDef?.host_body ?? 'skeleton');
  const [mods, setMods] = useState(() =>
    Object.fromEntries([...ATTRS, 'HP', 'MP'].map((a) => [a, editDef?.stat_modifiers?.[a] ?? 0])));
  const [scriptLang, setScriptLang] = useState(existingScript?.lang ?? 'lua');
  const [scriptCode, setScriptCode] = useState(existingScript?.code ?? '');
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
    return def;
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const res = validate('race', def);
    if (!res.valid) { setErrors(res.errors); return; }
    setErrors([]);
    dispatch({ type: 'saveRace', def });
    dispatch({ type: 'saveScript', classId: def.id, lang: scriptLang, code: scriptCode });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [name, description, hostBody, mods, namespace]);
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

      <Panel title="Behavior Script (optional)">
        <div className="text-xs mb-2" style={{ color: '#8a7749' }}>
          Ships as <span className="sam-mono">races/{slugify(name)}.{scriptLang}</span> next to the race JSON and auto-loads
          in-game — the same freedom class scripts have. React to any event hook and gate to this race with{' '}
          <span className="sam-mono">sam_get_race(player) == "{raceId}"</span>.
        </div>
        <ScriptEditor code={scriptCode} onCode={setScriptCode} lang={scriptLang} onLang={setScriptLang} />
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
