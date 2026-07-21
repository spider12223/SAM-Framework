/*
 * Recipe Editor — tinkering-kit craftables (recipe.schema.json).
 *
 * A recipe either ADDS a craftable (your own custom item, or a vanilla one at a new price
 * and skill tier) or REMOVES a vanilla craft entry to free a grid cell.
 *
 * The centrepiece is the LIVE GRID PREVIEW. Barony's crafting drawer is a fixed 5x4 = 20
 * cells and vanilla already fills 16, so only 4 recipes fit until you hide some. In game an
 * overflowing recipe is silently invisible AND unclickable, so this preview is the only
 * place a modder can discover that limit before shipping. It mirrors the engine's placement
 * exactly: drop suppressed entries, then fill each recipe's pinned cell if free, else the
 * next free cell scanning left-to-right, top-to-bottom.
 */
import { useMemo, useState } from 'react';
import { ITEM_TYPES } from '@/data/schemas.js';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import {
  Panel, Field, NumberInput, Select, GoldButton, ErrorList, SavedNote, SearchSelect,
} from '@/components/ui.jsx';

/* Barony's built-in craftable grid, in the engine's own order and cells
 * (interface.cpp tinkeringCreateCraftableItemList). 16 of the 20 cells. */
const VANILLA_GRID = [
  { type: 'TOOL_DECOY', x: 0, y: 0 }, { type: 'TOOL_LOCKPICK', x: 1, y: 0 },
  { type: 'TOOL_GLASSES', x: 2, y: 0 }, { type: 'POTION_EMPTY', x: 3, y: 0 },
  { type: 'TOOL_LANTERN', x: 4, y: 0 },
  { type: 'TOOL_FREEZE_BOMB', x: 0, y: 1 }, { type: 'TOOL_SLEEP_BOMB', x: 1, y: 1 },
  { type: 'TOOL_DUMMYBOT', x: 2, y: 1 }, { type: 'TOOL_GYROBOT', x: 3, y: 1 },
  { type: 'TOOL_BEARTRAP', x: 4, y: 1 },
  { type: 'TOOL_FIRE_BOMB', x: 0, y: 2 }, { type: 'TOOL_TELEPORT_BOMB', x: 1, y: 2 },
  { type: 'TOOL_SENTRYBOT', x: 2, y: 2 }, { type: 'TOOL_SPELLBOT', x: 3, y: 2 },
  { type: 'MASK_TECH_GOGGLES', x: 4, y: 2 },
  { type: 'CLOAK_BACKPACK', x: 0, y: 3 },
];
const GRID_W = 5;
const GRID_H = 4;

const STATUSES = ['BROKEN', 'DECREPIT', 'WORN', 'SERVICABLE', 'EXCELLENT'];
/* Each tier is 20 points of (Tinkering skill + PER). Tiers, not raw numbers: the crafting
 * screen recovers the requirement by multiplying the tier by 20, so anything in between
 * would display wrong. */
const TIERS = [0, 1, 2, 3, 4, 5];

const short = (t) => String(t ?? '').replace(/^(TOOL_|MASK_|CLOAK_|POTION_)/, '').replace(/_/g, ' ').toLowerCase();

export default function RecipeEditor() {
  const { meta, items: modItems, recipes, dispatch } = useMod();

  const [mode, setMode] = useState('add');            // 'add' | 'remove'
  const [item, setItem] = useState('');
  const [metal, setMetal] = useState(4);
  const [magic, setMagic] = useState(2);
  const [tier, setTier] = useState(0);
  const [status, setStatus] = useState('EXCELLENT');
  const [removeItem, setRemoveItem] = useState('');
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');

  const namespace = meta.namespace || 'mymod';
  // Everything craftable: vanilla item types plus this mod's own custom items.
  const pickable = useMemo(
    () => [...ITEM_TYPES, ...(modItems ?? []).map((i) => i.id)],
    [modItems],
  );
  const vanillaCraftables = useMemo(() => VANILLA_GRID.map((v) => v.type), []);

  /* Simulate the engine's placement so the preview matches what you'll actually see. */
  const layout = useMemo(() => {
    const suppressed = new Set(
      (recipes ?? []).filter((r) => r.remove).map((r) => r.remove),
    );
    const cells = new Array(GRID_W * GRID_H).fill(null);
    for (const v of VANILLA_GRID) {
      if (suppressed.has(v.type)) continue;
      cells[v.x + v.y * GRID_W] = { label: short(v.type), kind: 'vanilla', full: v.type };
    }
    const overflow = [];
    const recosted = [];
    for (const r of (recipes ?? []).filter((x) => x.item)) {
      // Naming an item that's already craftable is a RE-COST of that entry, not a new cell.
      // (The engine answers cost/skill by item type, so it needs no extra grid slot.)
      if (cells.some((c) => c && c.full === r.item)) { recosted.push(r.item); continue; }
      let idx = -1;
      const pinned = r.slot && Number.isInteger(r.slot.x) && Number.isInteger(r.slot.y)
        ? r.slot.x + r.slot.y * GRID_W : -1;
      if (pinned >= 0 && pinned < cells.length && !cells[pinned]) idx = pinned;
      else idx = cells.findIndex((c) => !c);
      if (idx < 0) { overflow.push(r.item); continue; }
      cells[idx] = { label: short(r.item), kind: 'custom', full: r.item };
    }
    return { cells, overflow, recosted, free: cells.filter((c) => !c).length, suppressed };
  }, [recipes]);

  const buildDef = () => {
    if (mode === 'remove') {
      return { id: `${namespace}:hide_${(removeItem || 'item').toLowerCase()}`, remove: removeItem };
    }
    const stem = String(item || 'recipe').split(':').pop().toLowerCase().replace(/[^a-z0-9]+/g, '_');
    const def = {
      id: `${namespace}:${stem}_recipe`,
      item,
      metal_cost: Number(metal) || 0,
      magic_cost: Number(magic) || 0,
      skill_level: Number(tier) || 0,
    };
    if (status !== 'EXCELLENT') def.status = status;
    return def;
  };

  const save = () => {
    setSavedAs('');
    const def = buildDef();
    if (mode === 'add' && !item) {
      setErrors([{ path: 'item', message: 'Pick the item this recipe produces.' }]);
      return;
    }
    if (mode === 'remove' && !removeItem) {
      setErrors([{ path: 'remove', message: 'Pick the vanilla recipe to hide.' }]);
      return;
    }
    // The engine refuses a free recipe: it renders in the grid and then permanently fails
    // to craft, because the affordability check bails when both costs are zero.
    if (mode === 'add' && (Number(metal) || 0) + (Number(magic) || 0) < 1) {
      setErrors([{ path: 'metal_cost', message: 'A recipe must cost at least 1 scrap in total, or it can never be crafted.' }]);
      return;
    }
    const res = validate('recipe', def);
    if (!res.valid) { setErrors(res.errors); return; }
    setErrors([]);
    dispatch({ type: 'saveRecipe', def });
    setSavedAs(def.id);
  };

  const preview = useMemo(() => JSON.stringify(buildDef(), null, 2),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [mode, item, metal, magic, tier, status, removeItem, namespace]);

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <Panel title="Tinkering Recipes">
        <div className="text-sm mb-3" style={{ color: 'var(--color-parchment)' }}>
          Add your own items to the tinkering kit's crafting grid, re-price a vanilla recipe,
          or hide one to make room. Players craft these from metal and magic scrap.
        </div>
        <div className="flex gap-2 mb-3">
          <GoldButton tone={mode === 'add' ? 'green' : 'gold'} onClick={() => setMode('add')}>
            ➕ Add / re-cost
          </GoldButton>
          <GoldButton tone={mode === 'remove' ? 'green' : 'gold'} onClick={() => setMode('remove')}>
            🚫 Hide a vanilla recipe
          </GoldButton>
        </div>

        {mode === 'add' ? (
          <>
            <Field label="Item this recipe makes" hint="One of your own custom items, or a vanilla item to re-price its existing recipe.">
              <SearchSelect
                options={pickable.filter((t) => t !== item)}
                onPick={setItem}
                placeholder="Search items… (yours appear as namespace:item)"
                allowCustom
              />
            </Field>
            {item && (
              <div className="sam-well px-3 py-2 mb-3 text-sm" style={{ color: 'var(--color-parchment)' }}>
                makes: <span className="sam-mono">{item}</span>
              </div>
            )}
            <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
              <Field label="⚙ Metal scrap" hint="Metal scrap consumed per craft.">
                <NumberInput value={metal} min={0} onChange={setMetal} />
              </Field>
              <Field label="💎 Magic scrap" hint="Magic scrap consumed per craft.">
                <NumberInput value={magic} min={0} onChange={setMagic} />
              </Field>
              <Field label="Skill tier" hint="Tinkering tier needed. Each tier is 20 points of Tinkering + PER.">
                <Select
                  value={String(tier)}
                  onChange={(v) => setTier(Number(v))}
                  options={TIERS.map((t) => ({ value: String(t), label: `Tier ${t}  (needs ${t * 20})` }))}
                />
              </Field>
              <Field label="Comes out as" hint="Condition of the crafted item.">
                <Select
                  value={status}
                  onChange={setStatus}
                  options={STATUSES.map((s) => ({ value: s, label: s[0] + s.slice(1).toLowerCase() }))}
                />
              </Field>
            </div>
            {(Number(metal) || 0) + (Number(magic) || 0) < 1 && (
              <div className="sam-error text-sm mt-2">
                A recipe must cost at least 1 scrap in total. A free recipe shows in the grid but can never be crafted.
              </div>
            )}
          </>
        ) : (
          <Field label="Vanilla recipe to hide" hint="Frees its grid cell. The item can still be salvaged and repaired; only the crafting entry disappears.">
            <SearchSelect
              options={vanillaCraftables.filter((t) => t !== removeItem)}
              onPick={setRemoveItem}
              placeholder="Search vanilla craftables…"
            />
          </Field>
        )}

        <div className="flex items-center justify-end gap-3 mt-4">
          {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> — see Mod Builder.</SavedNote>}
          <GoldButton tone="red" onClick={save}>🔧 Save Recipe</GoldButton>
        </div>
        <ErrorList errors={errors} />
      </Panel>

      {/* ---------------------------------------------- the live grid preview */}
      <Panel title="Crafting Grid Preview">
        <div className="text-xs mb-3" style={{ color: '#8a7749' }}>
          The kit's grid is a fixed <b>5 x 4 = 20 cells</b> and vanilla fills 16 of them. Hide a
          vanilla recipe to free a cell. A recipe with nowhere to go is <b>invisible in game</b>,
          so keep an eye on the count below.
        </div>
        <div className="sam-well p-3" style={{ display: 'inline-block' }}>
          {Array.from({ length: GRID_H }, (_, y) => (
            <div key={y} className="flex gap-1 mb-1">
              {Array.from({ length: GRID_W }, (_, x) => {
                const c = layout.cells[x + y * GRID_W];
                const bg = !c ? 'transparent' : (c.kind === 'custom' ? 'rgba(157,199,106,0.18)' : 'rgba(255,255,255,0.05)');
                const bd = !c ? '1px dashed #6b5a35' : (c.kind === 'custom' ? '1px solid #9dc76a' : '1px solid #4a3f28');
                return (
                  <div
                    key={x}
                    title={c ? c.full : 'empty cell'}
                    style={{
                      width: 92, height: 46, background: bg, border: bd, borderRadius: 3,
                      display: 'flex', alignItems: 'center', justifyContent: 'center',
                      fontSize: '0.62rem', textAlign: 'center', padding: 2, overflow: 'hidden',
                      color: c ? (c.kind === 'custom' ? '#c8e6a0' : '#9b8a5a') : '#5a4a2a',
                    }}
                  >
                    {c ? c.label : '—'}
                  </div>
                );
              })}
            </div>
          ))}
        </div>
        <div className="mt-3 text-sm">
          <span style={{ color: layout.free > 0 ? '#9dc76a' : '#e07a6a' }}>
            {layout.free} free {layout.free === 1 ? 'cell' : 'cells'}
          </span>
          <span style={{ color: '#6b5a35' }}>
            {' '}· {layout.suppressed.size} vanilla hidden · {(recipes ?? []).filter((r) => r.item).length - layout.recosted.length} added
            {layout.recosted.length > 0 && <> · {layout.recosted.length} re-costed (no cell used)</>}
          </span>
        </div>
        {layout.overflow.length > 0 && (
          <div className="sam-error text-sm mt-2">
            ⚠ No room for {layout.overflow.length} recipe(s): {layout.overflow.join(', ')}.
            They will NOT appear in game. Hide a vanilla recipe to free a cell.
          </div>
        )}
      </Panel>

      {/* ------------------------------------------------- saved recipe list */}
      {(recipes ?? []).length > 0 && (
        <Panel title={`Saved Recipes (${recipes.length})`}>
          <div className="space-y-2">
            {recipes.map((r) => (
              <div key={r.id} className="sam-well px-3 py-2 flex items-center gap-3">
                <span style={{ flex: 1, color: 'var(--color-parchment)' }} className="text-sm">
                  {r.remove ? (
                    <>🚫 hides <span className="sam-mono">{r.remove}</span></>
                  ) : (
                    <>
                      🔧 <span className="sam-mono">{r.item}</span>
                      <span style={{ color: '#8a7749' }}>
                        {' '}— {r.metal_cost ?? 0} metal, {r.magic_cost ?? 0} magic, tier {r.skill_level ?? 0}
                      </span>
                    </>
                  )}
                </span>
                <button
                  type="button" className="sam-step sam-remove" style={{ width: 24, height: 24 }}
                  onClick={() => dispatch({ type: 'deleteRecipe', id: r.id })}
                  aria-label={`delete ${r.id}`}
                >✕</button>
              </div>
            ))}
          </div>
        </Panel>
      )}

      <Panel title="Live JSON Preview" bodyClassName="p-0">
        <pre className="sam-mono m-0 p-4 overflow-x-auto text-xs" style={{ color: '#9b8a5a' }}>{preview}</pre>
      </Panel>
    </div>
  );
}
