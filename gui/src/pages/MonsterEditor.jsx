/*
 * Monster Editor — build a custom monster variant (monster.schema.json).
 * Mirrors ClassEditor: identity up top, schema-driven panels, live JSON preview,
 * advisory balance hints, Save at the bottom. A monster is a VARIANT of an
 * existing base_type — its stats/gear/behaviour override the base creature.
 * Every enum here comes from the schema at runtime (see data/schemas.js).
 */
import { useMemo, useState } from 'react';
import {
  MONSTER_BASE_TYPES, MONSTER_STAT_KEYS, MONSTER_RANDOM_STAT_KEYS,
  MONSTER_PROFICIENCIES, MONSTER_EQUIP_SLOTS, MONSTER_ITEM_STATUSES,
  MONSTER_FLAG_KEYS, STORE_TYPES, SPAWN_MODES, ITEM_TYPES_LOWER,
} from '@/data/schemas.js';
import { validate } from '@/lib/validate.js';
import { checkBalance } from '@/lib/balance.js';
import { useMod } from '@/state/ModContext.jsx';
import {
  Panel, Field, TextInput, NumberInput, Select, Stepper, GoldButton,
  SearchSelect, ErrorList, SavedNote, BalanceHints,
} from '@/components/ui.jsx';

function slugify(name) {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'unnamed';
}

/** Prettify a snake_case flag key: force_player_enemy -> "Force player enemy". */
function flagLabel(k) {
  const s = k.replace(/_/g, ' ');
  return s.charAt(0).toUpperCase() + s.slice(1);
}

/** Only the keys whose value is a real number (drops '' and null). */
function numericMap(obj) {
  const out = {};
  for (const [k, v] of Object.entries(obj)) {
    if (v !== '' && v !== null && v !== undefined && !Number.isNaN(v)) out[k] = v;
  }
  return out;
}

/** Turn an editor item row into a clean itemEntry (drops empty fields). */
function cleanEntry(e) {
  const o = {};
  if (e.type && e.type.trim()) o.type = e.type.trim();
  if (e.status) o.status = e.status;
  if (e.count !== '' && e.count != null) o.count = e.count;
  if (e.beatitude !== '' && e.beatitude != null) o.beatitude = e.beatitude;
  if (e.spawn_percent_chance !== '' && e.spawn_percent_chance != null) o.spawn_percent_chance = e.spawn_percent_chance;
  if (e.drop_percent_chance !== '' && e.drop_percent_chance != null) o.drop_percent_chance = e.drop_percent_chance;
  if (e.slot_weighted_chance !== '' && e.slot_weighted_chance != null) o.slot_weighted_chance = e.slot_weighted_chance;
  return o;
}

const blankEntry = (slot) => ({
  slot, type: '', status: '', count: '', beatitude: '',
  spawn_percent_chance: '', drop_percent_chance: '', slot_weighted_chance: '',
});

const blankSpawn = () => ({
  level_name: '', mode: 'random', base_species: '',
  weighted_chance: 1, dungeon_depth_minimum: 0, dungeon_depth_maximum: 99,
  variant_weight: 1, default_weight: 1,
});

/** Compact editor for one item entry (used for equipment + inventory). */
function EntryRow({ entry, showSlot, onChange, onRemove }) {
  const set = (patch) => onChange({ ...entry, ...patch });
  return (
    <div className="sam-well p-2 space-y-2">
      <div className="flex items-center gap-2">
        {showSlot && (
          <Select
            className="w-32 shrink-0"
            value={entry.slot}
            onChange={(v) => set({ slot: v })}
            options={MONSTER_EQUIP_SLOTS}
          />
        )}
        <div className="flex-1 min-w-0">
          <SearchSelect
            options={ITEM_TYPES_LOWER}
            value={entry.type}
            onPick={(v) => set({ type: v })}
            placeholder="item — e.g. steel_sword"
            allowCustom
          />
        </div>
        <button type="button" className="sam-step sam-remove shrink-0" onClick={onRemove} aria-label="remove item">✕</button>
      </div>
      <div className="grid grid-cols-2 sm:grid-cols-3 gap-2">
        <Field label="Status">
          <Select
            value={entry.status}
            onChange={(v) => set({ status: v })}
            options={['', ...MONSTER_ITEM_STATUSES]}
          />
        </Field>
        <Field label="Count">
          <NumberInput value={entry.count} min={1} onChange={(v) => set({ count: v })} />
        </Field>
        <Field label="Beatitude">
          <NumberInput value={entry.beatitude} onChange={(v) => set({ beatitude: v })} />
        </Field>
        <Field label="Spawn %">
          <NumberInput value={entry.spawn_percent_chance} min={0} max={100} onChange={(v) => set({ spawn_percent_chance: v })} />
        </Field>
        <Field label="Drop %">
          <NumberInput value={entry.drop_percent_chance} min={0} max={100} onChange={(v) => set({ drop_percent_chance: v })} />
        </Field>
        <Field label="Pick weight" hint="for alternatives">
          <NumberInput value={entry.slot_weighted_chance} min={1} onChange={(v) => set({ slot_weighted_chance: v })} />
        </Field>
      </div>
    </div>
  );
}

export default function MonsterEditor() {
  const { meta, dispatch } = useMod();

  const [name, setName] = useState('');
  const [baseType, setBaseType] = useState(MONSTER_BASE_TYPES[0]);
  const [sex, setSex] = useState('');            // '' | '0' | '1'
  const [appearance, setAppearance] = useState('');
  const [stats, setStats] = useState({});        // key -> number | ''
  const [randomStats, setRandomStats] = useState({});
  const [profs, setProfs] = useState(() => Object.fromEntries(MONSTER_PROFICIENCIES.map((s) => [s, 0])));
  const [flags, setFlags] = useState({});        // flag key -> bool
  const [propNums, setPropNums] = useState({ xp_award_percent: '', spellbook_cast_cooldown: '' });
  const [equip, setEquip] = useState([]);        // [{slot, ...entry}]
  const [inventory, setInventory] = useState([]); // [{...entry}]
  const [numFollowers, setNumFollowers] = useState(0);
  const [followerVariants, setFollowerVariants] = useState([]); // [{key, weight}]
  const [shop, setShop] = useState({
    store_types: [], generate_default_shop_items: false,
    num_generated_items_min: '', num_generated_items_max: '', generated_item_blessing_max: '',
  });
  const [spawn, setSpawn] = useState([]);        // [blankSpawn()]
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');

  const namespace = meta.namespace || 'mymod';
  const monsterId = `${namespace}:${slugify(name)}`;

  const buildDef = () => {
    const def = { id: monsterId, name: name.trim(), base_type: baseType };
    if (sex !== '') def.sex = Number(sex);
    if (appearance !== '') def.appearance = appearance;

    const s = numericMap(stats);
    if (Object.keys(s).length) def.stats = s;
    const rs = numericMap(randomStats);
    if (Object.keys(rs).length) def.random_stats = rs;

    const p = Object.fromEntries(Object.entries(profs).filter(([, v]) => v > 0));
    if (Object.keys(p).length) def.proficiencies = p;

    const props = {};
    for (const f of MONSTER_FLAG_KEYS) if (flags[f]) props[f] = true;
    if (propNums.xp_award_percent !== '') props.xp_award_percent = propNums.xp_award_percent;
    if (propNums.spellbook_cast_cooldown !== '') props.spellbook_cast_cooldown = propNums.spellbook_cast_cooldown;
    if (Object.keys(props).length) def.properties = props;

    const equipped = {};
    for (const slot of MONSTER_EQUIP_SLOTS) {
      const entries = equip.filter((e) => e.slot === slot && e.type.trim()).map(cleanEntry);
      if (entries.length === 1) equipped[slot] = entries[0];
      else if (entries.length > 1) equipped[slot] = entries; // weighted array
    }
    if (Object.keys(equipped).length) def.equipped_items = equipped;

    const inv = inventory.filter((e) => e.type.trim()).map(cleanEntry);
    if (inv.length) def.inventory_items = inv;

    const fv = {};
    for (const r of followerVariants) if (r.key.trim()) fv[r.key.trim()] = r.weight;
    if (numFollowers > 0 || Object.keys(fv).length) {
      def.followers = { num_followers: numFollowers };
      if (Object.keys(fv).length) def.followers.follower_variants = fv;
    }

    if (baseType === 'shopkeeper') {
      const sp = {};
      const st = {};
      for (const r of shop.store_types) if (r.key) st[r.key] = r.weight;
      if (Object.keys(st).length) sp.store_type_chances = st;
      if (shop.generate_default_shop_items) sp.generate_default_shop_items = true;
      if (shop.num_generated_items_min !== '') sp.num_generated_items_min = shop.num_generated_items_min;
      if (shop.num_generated_items_max !== '') sp.num_generated_items_max = shop.num_generated_items_max;
      if (shop.generated_item_blessing_max !== '') sp.generated_item_blessing_max = shop.generated_item_blessing_max;
      if (Object.keys(sp).length) def.shopkeeper_properties = sp;
    }

    const spawns = spawn.filter((b) => b.level_name.trim()).map((b) => {
      const o = {
        level_name: b.level_name.trim(),
        mode: b.mode,
        weighted_chance: b.weighted_chance,
        dungeon_depth_minimum: b.dungeon_depth_minimum,
        dungeon_depth_maximum: b.dungeon_depth_maximum,
        variant_weight: b.variant_weight,
        default_weight: b.default_weight,
      };
      if (b.base_species.trim()) o.base_species = b.base_species.trim();
      return o;
    });
    if (spawns.length) def.spawn = spawns;

    return def;
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    const result = validate('monster', def);
    if (!result.valid) {
      setErrors(result.errors);
      return;
    }
    setErrors([]);
    dispatch({ type: 'saveMonster', def });
    setSavedAs(def.id);
  };

  const def = useMemo(buildDef,
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [name, baseType, sex, appearance, stats, randomStats, profs, flags, propNums,
      equip, inventory, numFollowers, followerVariants, shop, spawn, namespace]);
  const preview = useMemo(() => JSON.stringify(def, null, 2), [def]);
  const hints = useMemo(() => checkBalance('monster', def), [def]);

  return (
    <div className="space-y-4 max-w-7xl mx-auto">
      {/* ------------------------------------------------ name + emblem */}
      <div className="flex items-center gap-4">
        <div
          className="sam-panel flex items-center justify-center shrink-0"
          style={{ width: 84, height: 84, fontSize: '2.4rem' }}
          aria-hidden
        >
          👹
        </div>
        <div className="flex-1">
          <TextInput
            value={name}
            onChange={setName}
            placeholder="Monster name — e.g. Goblin Captain"
            style={{ fontSize: '1.5rem', padding: '0.7rem 1rem' }}
            aria-label="Monster name"
          />
          <div className="mt-1 text-xs" style={{ color: '#6b5a35' }}>
            id: <span className="sam-mono">{monsterId}</span>{' '}(namespace comes from the Mod Builder)
          </div>
        </div>
      </div>

      {/* ------------------------------------------------ identity + stats */}
      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4 items-start">
        <Panel title="Identity">
          <Field label="Base creature" hint="sprite, AI & limbs come from this vanilla type">
            <Select value={baseType} onChange={setBaseType} options={MONSTER_BASE_TYPES} />
          </Field>
          <div className="grid grid-cols-2 gap-3 mt-3">
            <Field label="Sex">
              <Select
                value={sex}
                onChange={setSex}
                options={[{ value: '', label: '— default —' }, { value: '0', label: 'Male' }, { value: '1', label: 'Female' }]}
              />
            </Field>
            <Field label="Appearance">
              <NumberInput value={appearance} min={0} onChange={setAppearance} />
            </Field>
          </div>
        </Panel>

        <Panel title="Stats" bodyClassName="max-h-[460px] overflow-y-auto">
          <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>Leave blank to keep the base type's value.</div>
          <div className="grid grid-cols-2 gap-2">
            {MONSTER_STAT_KEYS.map((k) => (
              <Field key={k} label={k}>
                <NumberInput value={stats[k] ?? ''} onChange={(v) => setStats((p) => ({ ...p, [k]: v }))} />
              </Field>
            ))}
          </div>
        </Panel>

        <Panel title="Random Variance" bodyClassName="max-h-[460px] overflow-y-auto">
          <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>Adds a random 0..N to the stat on spawn.</div>
          <div className="grid grid-cols-2 gap-2">
            {MONSTER_RANDOM_STAT_KEYS.map((k) => (
              <Field key={k} label={k.replace('RANDOM_', '')}>
                <NumberInput value={randomStats[k] ?? ''} min={0} onChange={(v) => setRandomStats((p) => ({ ...p, [k]: v }))} />
              </Field>
            ))}
          </div>
        </Panel>
      </div>

      {/* ------------------------------------------- proficiencies + properties */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="Proficiencies" bodyClassName="max-h-[420px] overflow-y-auto">
          {MONSTER_PROFICIENCIES.map((s) => (
            <Stepper
              key={s}
              label={s}
              value={profs[s]}
              min={0}
              max={100}
              step={5}
              onChange={(v) => setProfs((p) => ({ ...p, [s]: v }))}
            />
          ))}
        </Panel>

        <Panel title="Behaviour">
          <div className="grid grid-cols-1 sm:grid-cols-2 gap-2">
            {MONSTER_FLAG_KEYS.map((f) => (
              <GoldButton
                key={f}
                tone={flags[f] ? 'green' : 'gold'}
                className="justify-start text-left"
                onClick={() => setFlags((p) => ({ ...p, [f]: !p[f] }))}
              >
                {flags[f] ? '✓' : '○'} {flagLabel(f)}
              </GoldButton>
            ))}
          </div>
          <div className="sam-divider" />
          <div className="grid grid-cols-2 gap-3">
            <Field label="XP award %" hint="0-100">
              <NumberInput value={propNums.xp_award_percent} min={0} max={100} onChange={(v) => setPropNums((p) => ({ ...p, xp_award_percent: v }))} />
            </Field>
            <Field label="Spellbook cooldown" hint="ticks">
              <NumberInput value={propNums.spellbook_cast_cooldown} min={0} onChange={(v) => setPropNums((p) => ({ ...p, spellbook_cast_cooldown: v }))} />
            </Field>
          </div>
        </Panel>
      </div>

      {/* ------------------------------------------------ equipment + inventory */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="Equipped Items">
          <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
            Two+ entries in the same slot become a weighted random pick.
          </div>
          <div className="space-y-2 mb-3">
            {equip.length === 0 && (
              <div className="text-sm py-2 text-center" style={{ color: '#6b5a35' }}>No equipment yet.</div>
            )}
            {equip.map((e, i) => (
              <EntryRow
                key={i}
                entry={e}
                showSlot
                onChange={(next) => setEquip((prev) => prev.map((x, j) => (j === i ? next : x)))}
                onRemove={() => setEquip((prev) => prev.filter((_, j) => j !== i))}
              />
            ))}
          </div>
          <GoldButton onClick={() => setEquip((prev) => [...prev, blankEntry('weapon')])}>+ Add equipment</GoldButton>
        </Panel>

        <Panel title="Inventory">
          <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
            Carried items — e.g. spellbooks it casts, potions, loot to drop.
          </div>
          <div className="space-y-2 mb-3">
            {inventory.length === 0 && (
              <div className="text-sm py-2 text-center" style={{ color: '#6b5a35' }}>No inventory yet.</div>
            )}
            {inventory.map((e, i) => (
              <EntryRow
                key={i}
                entry={e}
                showSlot={false}
                onChange={(next) => setInventory((prev) => prev.map((x, j) => (j === i ? next : x)))}
                onRemove={() => setInventory((prev) => prev.filter((_, j) => j !== i))}
              />
            ))}
          </div>
          <GoldButton onClick={() => setInventory((prev) => [...prev, blankEntry()])}>+ Add inventory item</GoldButton>
        </Panel>
      </div>

      {/* ------------------------------------------------ followers + shopkeeper */}
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 items-start">
        <Panel title="Followers">
          <Field label="Number of followers">
            <NumberInput value={numFollowers} min={0} onChange={(v) => setNumFollowers(v === '' ? 0 : v)} />
          </Field>
          <div className="sam-divider" />
          <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
            Weighted picks. Reference other monsters as <span className="sam-mono">namespace:monster</span>, or <span className="sam-mono">default</span> for a vanilla one.
          </div>
          <div className="space-y-2 mb-3">
            {followerVariants.map((r, i) => (
              <div key={i} className="flex items-center gap-2">
                <div className="flex-1">
                  <TextInput
                    value={r.key}
                    onChange={(v) => setFollowerVariants((prev) => prev.map((x, j) => (j === i ? { ...x, key: v } : x)))}
                    placeholder="mymod:rat_swarm"
                  />
                </div>
                <NumberInput
                  className="w-20"
                  value={r.weight}
                  min={0}
                  onChange={(v) => setFollowerVariants((prev) => prev.map((x, j) => (j === i ? { ...x, weight: v === '' ? 0 : v } : x)))}
                />
                <button type="button" className="sam-step sam-remove" onClick={() => setFollowerVariants((prev) => prev.filter((_, j) => j !== i))} aria-label="remove follower">✕</button>
              </div>
            ))}
          </div>
          <GoldButton onClick={() => setFollowerVariants((prev) => [...prev, { key: '', weight: 1 }])}>+ Add follower type</GoldButton>
        </Panel>

        {baseType === 'shopkeeper' ? (
          <Panel title="Shopkeeper">
            <div className="space-y-2 mb-3">
              {shop.store_types.map((r, i) => (
                <div key={i} className="flex items-center gap-2">
                  <Select
                    className="flex-1"
                    value={r.key}
                    onChange={(v) => setShop((s) => ({ ...s, store_types: s.store_types.map((x, j) => (j === i ? { ...x, key: v } : x)) }))}
                    options={['', ...STORE_TYPES]}
                  />
                  <NumberInput
                    className="w-20"
                    value={r.weight}
                    min={0}
                    onChange={(v) => setShop((s) => ({ ...s, store_types: s.store_types.map((x, j) => (j === i ? { ...x, weight: v === '' ? 0 : v } : x)) }))}
                  />
                  <button type="button" className="sam-step sam-remove" onClick={() => setShop((s) => ({ ...s, store_types: s.store_types.filter((_, j) => j !== i) }))} aria-label="remove store type">✕</button>
                </div>
              ))}
            </div>
            <GoldButton onClick={() => setShop((s) => ({ ...s, store_types: [...s.store_types, { key: '', weight: 1 }] }))}>+ Add store type</GoldButton>
            <div className="sam-divider" />
            <Field label="Generate default shop items">
              <GoldButton
                tone={shop.generate_default_shop_items ? 'green' : 'gold'}
                onClick={() => setShop((s) => ({ ...s, generate_default_shop_items: !s.generate_default_shop_items }))}
              >
                {shop.generate_default_shop_items ? '✓ Yes' : '✗ No'}
              </GoldButton>
            </Field>
            <div className="grid grid-cols-3 gap-2 mt-3">
              <Field label="Min items"><NumberInput value={shop.num_generated_items_min} min={0} onChange={(v) => setShop((s) => ({ ...s, num_generated_items_min: v }))} /></Field>
              <Field label="Max items"><NumberInput value={shop.num_generated_items_max} min={0} onChange={(v) => setShop((s) => ({ ...s, num_generated_items_max: v }))} /></Field>
              <Field label="Max bless"><NumberInput value={shop.generated_item_blessing_max} onChange={(v) => setShop((s) => ({ ...s, generated_item_blessing_max: v }))} /></Field>
            </div>
          </Panel>
        ) : (
          <Panel title="World Spawns">
            <SpawnEditor spawn={spawn} setSpawn={setSpawn} />
          </Panel>
        )}
      </div>

      {/* Spawns get their own row when the shopkeeper panel took the slot above */}
      {baseType === 'shopkeeper' && (
        <Panel title="World Spawns">
          <SpawnEditor spawn={spawn} setSpawn={setSpawn} />
        </Panel>
      )}

      {/* ------------------------------------------------------- save row */}
      <BalanceHints hints={hints} />
      <ErrorList errors={errors} />
      <div className="flex items-center justify-end gap-3">
        {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> to this session's mod — see Mod Builder.</SavedNote>}
        <GoldButton tone="red" onClick={save} disabled={!name.trim()}>
          👹 Save Monster
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

/** The spawn-block list editor (shared between the two layouts). */
function SpawnEditor({ spawn, setSpawn }) {
  const set = (i, patch) => setSpawn((prev) => prev.map((x, j) => (j === i ? { ...x, ...patch } : x)));
  return (
    <>
      <div className="text-xs mb-2" style={{ color: '#6b5a35' }}>
        Listing a level makes S.A.M manage its random spawns — keep <span className="sam-mono">default weight</span> &gt; 0 so vanilla monsters still appear.
      </div>
      <div className="space-y-3 mb-3">
        {spawn.length === 0 && (
          <div className="text-sm py-2 text-center" style={{ color: '#6b5a35' }}>
            No spawns — this monster will only appear via followers, map editor, or the console.
          </div>
        )}
        {spawn.map((b, i) => (
          <div key={i} className="sam-well p-2 space-y-2">
            <div className="flex items-center gap-2">
              <div className="flex-1">
                <TextInput value={b.level_name} onChange={(v) => set(i, { level_name: v })} placeholder="Level — e.g. The Mines" />
              </div>
              <Select className="w-28" value={b.mode} onChange={(v) => set(i, { mode: v })} options={SPAWN_MODES} />
              <button type="button" className="sam-step sam-remove" onClick={() => setSpawn((prev) => prev.filter((_, j) => j !== i))} aria-label="remove spawn">✕</button>
            </div>
            <div className="grid grid-cols-2 sm:grid-cols-3 gap-2">
              <Field label="Base species" hint="defaults to base type"><TextInput value={b.base_species} onChange={(v) => set(i, { base_species: v })} placeholder="goblin" /></Field>
              <Field label="Species weight"><NumberInput value={b.weighted_chance} min={1} onChange={(v) => set(i, { weighted_chance: v === '' ? 1 : v })} /></Field>
              <Field label="Depth min"><NumberInput value={b.dungeon_depth_minimum} min={0} onChange={(v) => set(i, { dungeon_depth_minimum: v === '' ? 0 : v })} /></Field>
              <Field label="Depth max"><NumberInput value={b.dungeon_depth_maximum} min={0} onChange={(v) => set(i, { dungeon_depth_maximum: v === '' ? 99 : v })} /></Field>
              <Field label="Variant weight"><NumberInput value={b.variant_weight} min={1} onChange={(v) => set(i, { variant_weight: v === '' ? 1 : v })} /></Field>
              <Field label="Default weight" hint="vanilla version"><NumberInput value={b.default_weight} min={0} onChange={(v) => set(i, { default_weight: v === '' ? 0 : v })} /></Field>
            </div>
          </div>
        ))}
      </div>
      <GoldButton onClick={() => setSpawn((prev) => [...prev, blankSpawn()])}>+ Add spawn level</GoldButton>
    </>
  );
}
