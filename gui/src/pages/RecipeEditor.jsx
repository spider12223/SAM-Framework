/*
 * Recipe Editor — craftables for a bench (recipe.schema.json).
 *
 * A recipe makes ONE OF YOUR OWN items craftable at a bench you name. The vanilla tinkering
 * kit is off limits: a mod cannot add to it, re-price it or hide anything on it, so vanilla
 * crafting is always exactly what the game ships. Everything goes on the framework's built-in
 * Hunter's Workbench, or on a bench made from one of your own items.
 *
 * The centrepiece is the LIVE GRID PREVIEW. A bench's drawer is a fixed 5x4 = 20 cells, and
 * an overflowing recipe is silently invisible AND unclickable in game, so this is the only
 * place a modder can discover that limit before shipping. It mirrors the engine's placement:
 * fill each recipe's pinned cell if free, else the next free cell left-to-right, top-to-bottom.
 * Each bench gets its own grid, so the preview is per bench.
 */
import { useMemo, useState } from 'react';
import { validate } from '@/lib/validate.js';
import { useMod } from '@/state/ModContext.jsx';
import {
  Panel, Field, NumberInput, Select, GoldButton, ErrorList, SavedNote, SearchSelect,
} from '@/components/ui.jsx';

const GRID_W = 5;
const GRID_H = 4;

/* The framework's own bench. Always available, ships inside the framework, and needs no
 * mod of its own — so it is the default and the common case. */
const BUILTIN_KIT = 'sam:hunters_workbench';

const STATUSES = ['BROKEN', 'DECREPIT', 'WORN', 'SERVICABLE', 'EXCELLENT'];
/* Each tier is 20 points of (Tinkering skill + PER). Tiers, not raw numbers: the crafting
 * screen recovers the requirement by multiplying the tier by 20, so anything in between
 * would display wrong. */
const TIERS = [0, 1, 2, 3, 4, 5];

const short = (t) => String(t ?? '').split(':').pop().replace(/_/g, ' ').toLowerCase();

export default function RecipeEditor() {
  const { meta, items: modItems, recipes, dispatch } = useMod();

  const [item, setItem] = useState('');
  const [kit, setKit] = useState(BUILTIN_KIT);
  const [payWith, setPayWith] = useState('scrap');    // 'scrap' | 'materials'
  const [metal, setMetal] = useState(4);
  const [magic, setMagic] = useState(2);
  const [matA, setMatA] = useState('');
  const [matACount, setMatACount] = useState(3);
  const [matB, setMatB] = useState('');
  const [matBCount, setMatBCount] = useState(0);
  const [tier, setTier] = useState(0);
  const [status, setStatus] = useState('EXCELLENT');
  const [errors, setErrors] = useState([]);
  const [savedAs, setSavedAs] = useState('');

  const namespace = meta.namespace || 'mymod';
  /* Only your OWN items can be crafted or spent — vanilla items cannot be given a recipe,
   * because that would mean changing the vanilla kit. */
  const ownItems = useMemo(() => (modItems ?? []).map((i) => i.id), [modItems]);
  /* Any of your items can double as a bench, alongside the built-in one. */
  const kitOptions = useMemo(() => [BUILTIN_KIT, ...ownItems], [ownItems]);

  /* Which benches are actually in use, so the preview can show one grid each. */
  const benches = useMemo(() => {
    const set = new Set((recipes ?? []).filter((r) => r.item && r.kit).map((r) => r.kit));
    set.add(kit || BUILTIN_KIT);
    return [...set];
  }, [recipes, kit]);

  /* Simulate the engine's placement, per bench, so the preview matches what you'll see. */
  const layoutFor = (benchId) => {
    const cells = new Array(GRID_W * GRID_H).fill(null);
    const overflow = [];
    for (const r of (recipes ?? []).filter((x) => x.item && x.kit === benchId)) {
      let idx = -1;
      const pinned = r.slot && Number.isInteger(r.slot.x) && Number.isInteger(r.slot.y)
        ? r.slot.x + r.slot.y * GRID_W : -1;
      if (pinned >= 0 && pinned < cells.length && !cells[pinned]) idx = pinned;
      else idx = cells.findIndex((c) => !c);
      if (idx < 0) { overflow.push(r.item); continue; }
      cells[idx] = { label: short(r.item), full: r.item };
    }
    return { cells, overflow, free: cells.filter((c) => !c).length };
  };

  const usingMaterials = payWith === 'materials';

  const buildDef = () => {
    const stem = String(item || 'recipe').split(':').pop().toLowerCase().replace(/[^a-z0-9]+/g, '_');
    const def = {
      id: `${namespace}:${stem}_recipe`,
      item,
      kit: kit || BUILTIN_KIT,
      skill_level: Number(tier) || 0,
    };
    if (usingMaterials) {
      const mats = [];
      if (matA) mats.push({ item: matA, count: Number(matACount) || 1 });
      if (matB) mats.push({ item: matB, count: Number(matBCount) || 1 });
      if (mats.length) def.materials = mats;
    } else {
      def.metal_cost = Number(metal) || 0;
      def.magic_cost = Number(magic) || 0;
    }
    if (status !== 'EXCELLENT') def.status = status;
    return def;
  };

  const save = () => {
    setSavedAs('');
    if (!item) {
      setErrors([{ path: 'item', message: 'Pick one of your own items for this recipe to make.' }]);
      return;
    }
    if (!kit) {
      setErrors([{ path: 'kit', message: 'Pick the bench this recipe appears on. Without one it appears nowhere.' }]);
      return;
    }
    if (usingMaterials && !matA) {
      setErrors([{ path: 'materials', message: 'Pick at least one material, or switch to paying in scrap.' }]);
      return;
    }
    // The engine refuses a free recipe: it renders in the grid and then permanently fails
    // to craft, because the affordability check bails when both costs are zero.
    if (!usingMaterials && (Number(metal) || 0) + (Number(magic) || 0) < 1) {
      setErrors([{ path: 'metal_cost', message: 'A recipe must cost at least 1 scrap in total, or it can never be crafted.' }]);
      return;
    }
    const def = buildDef();
    const res = validate('recipe', def);
    if (!res.valid) { setErrors(res.errors); return; }
    setErrors([]);
    dispatch({ type: 'saveRecipe', def });
    setSavedAs(def.id);
  };

  const preview = useMemo(() => JSON.stringify(buildDef(), null, 2),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [item, kit, payWith, metal, magic, matA, matACount, matB, matBCount, tier, status, namespace]);

  return (
    <div className="space-y-4 max-w-5xl mx-auto">
      <Panel title="Crafting Recipes">
        <div className="text-sm mb-3" style={{ color: 'var(--color-parchment)' }}>
          Make one of your items craftable at a bench. The <b>Hunter's Workbench</b> ships inside
          the framework, so every player already has it and your mod needs to ship nothing.
          The vanilla tinkering kit is never changed.
        </div>

        <Field label="Item this recipe makes" hint="One of your own custom items. Make it in the Item Editor first.">
          <SearchSelect
            options={ownItems.filter((t) => t !== item)}
            onPick={setItem}
            placeholder="Search your items…"
            allowCustom
          />
        </Field>
        {item && (
          <div className="sam-well px-3 py-2 mb-3 text-sm" style={{ color: 'var(--color-parchment)' }}>
            makes: <span className="sam-mono">{item}</span>
          </div>
        )}

        <Field label="Bench it appears on" hint="The built-in Hunter's Workbench, or one of your own items to use as a bench of its own.">
          <Select
            value={kit}
            onChange={setKit}
            options={kitOptions.map((k) => ({
              value: k,
              label: k === BUILTIN_KIT ? "Hunter's Workbench (built in)" : `${k} (your own bench)`,
            }))}
          />
        </Field>

        <div className="flex gap-2 my-3">
          <GoldButton tone={payWith === 'scrap' ? 'green' : 'gold'} onClick={() => setPayWith('scrap')}>
            ⚙ Costs scrap
          </GoldButton>
          <GoldButton tone={payWith === 'materials' ? 'green' : 'gold'} onClick={() => setPayWith('materials')}>
            🦴 Costs your own items
          </GoldButton>
        </div>

        {usingMaterials ? (
          <>
            <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
              <Field label="Material" hint="One of your own items, consumed per craft.">
                <SearchSelect options={ownItems.filter((t) => t !== matA)} onPick={setMatA} placeholder="Search your items…" allowCustom />
              </Field>
              <Field label="How many" hint="Consumed per craft.">
                <NumberInput value={matACount} min={1} onChange={setMatACount} />
              </Field>
              <Field label="Second material" hint="Optional. The panel has room for two.">
                <SearchSelect options={ownItems.filter((t) => t !== matB)} onPick={setMatB} placeholder="Optional…" allowCustom />
              </Field>
              <Field label="How many" hint="Only used if a second material is set.">
                <NumberInput value={matBCount} min={0} onChange={setMatBCount} />
              </Field>
            </div>
            {matA && (
              <div className="sam-well px-3 py-2 mt-2 text-sm" style={{ color: 'var(--color-parchment)' }}>
                costs: <span className="sam-mono">{matACount} x {matA}</span>
                {matB && <> + <span className="sam-mono">{matBCount || 1} x {matB}</span></>}
                <div className="text-xs mt-1" style={{ color: '#8a7749' }}>
                  Give each material an <b>icon</b> in the Item Editor, or the crafting panel shows
                  the scrap icon beside the number instead of your material.
                </div>
              </div>
            )}
          </>
        ) : (
          <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
            <Field label="⚙ Metal scrap" hint="Metal scrap consumed per craft.">
              <NumberInput value={metal} min={0} onChange={setMetal} />
            </Field>
            <Field label="💎 Magic scrap" hint="Magic scrap consumed per craft.">
              <NumberInput value={magic} min={0} onChange={setMagic} />
            </Field>
            <div />
            <div />
          </div>
        )}

        <div className="grid grid-cols-2 sm:grid-cols-4 gap-3 mt-3">
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

        {!usingMaterials && (Number(metal) || 0) + (Number(magic) || 0) < 1 && (
          <div className="sam-error text-sm mt-2">
            A recipe must cost at least 1 scrap in total. A free recipe shows in the grid but can never be crafted.
          </div>
        )}

        <div className="flex items-center justify-end gap-3 mt-4">
          {savedAs && <SavedNote>Saved <span className="sam-mono">{savedAs}</span> — see Mod Builder.</SavedNote>}
          <GoldButton tone="red" onClick={save}>🔧 Save Recipe</GoldButton>
        </div>
        <ErrorList errors={errors} />
      </Panel>

      {/* ---------------------------------------------- the live grid preview */}
      <Panel title="Bench Grid Preview">
        <div className="text-xs mb-3" style={{ color: '#8a7749' }}>
          Each bench has a fixed <b>5 x 4 = 20 cells</b>, all of them yours. A recipe with
          nowhere to go is <b>invisible in game</b>, so watch the count.
        </div>
        {benches.map((b) => {
          const lay = layoutFor(b);
          return (
            <div key={b} className="mb-4">
              <div className="text-sm mb-1" style={{ color: 'var(--color-parchment)' }}>
                {b === BUILTIN_KIT ? "Hunter's Workbench" : b}
              </div>
              <div className="sam-well p-3" style={{ display: 'inline-block' }}>
                {Array.from({ length: GRID_H }, (_, y) => (
                  <div key={y} className="flex gap-1 mb-1">
                    {Array.from({ length: GRID_W }, (_, x) => {
                      const c = lay.cells[x + y * GRID_W];
                      return (
                        <div
                          key={x}
                          title={c ? c.full : 'empty cell'}
                          style={{
                            width: 92, height: 46,
                            background: c ? 'rgba(157,199,106,0.18)' : 'transparent',
                            border: c ? '1px solid #9dc76a' : '1px dashed #6b5a35',
                            borderRadius: 3,
                            display: 'flex', alignItems: 'center', justifyContent: 'center',
                            fontSize: '0.62rem', textAlign: 'center', padding: 2, overflow: 'hidden',
                            color: c ? '#c8e6a0' : '#5a4a2a',
                          }}
                        >
                          {c ? c.label : '—'}
                        </div>
                      );
                    })}
                  </div>
                ))}
              </div>
              <div className="mt-2 text-sm">
                <span style={{ color: lay.free > 0 ? '#9dc76a' : '#e07a6a' }}>
                  {lay.free} free {lay.free === 1 ? 'cell' : 'cells'}
                </span>
              </div>
              {lay.overflow.length > 0 && (
                <div className="sam-error text-sm mt-2">
                  ⚠ No room for {lay.overflow.length} recipe(s): {lay.overflow.join(', ')}.
                  They will NOT appear in game.
                </div>
              )}
            </div>
          );
        })}
      </Panel>

      {/* ------------------------------------------------- saved recipe list */}
      {(recipes ?? []).length > 0 && (
        <Panel title={`Saved Recipes (${recipes.length})`}>
          <div className="space-y-2">
            {recipes.map((r) => (
              <div key={r.id} className="sam-well px-3 py-2 flex items-center gap-3">
                <span style={{ flex: 1, color: 'var(--color-parchment)' }} className="text-sm">
                  🔧 <span className="sam-mono">{r.item}</span>
                  <span style={{ color: '#8a7749' }}>
                    {' '}on {r.kit === BUILTIN_KIT ? "Hunter's Workbench" : r.kit}
                    {' '}— {r.materials
                      ? r.materials.map((m) => `${m.count ?? 1} x ${String(m.item).split(':').pop()}`).join(' + ')
                      : `${r.metal_cost ?? 0} metal, ${r.magic_cost ?? 0} magic`}
                    , tier {r.skill_level ?? 0}
                  </span>
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
