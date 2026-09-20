/*
 * Working mod state, autosaved to localStorage so a refresh/crash never loses
 * work (Tier 3). Holds every content type the framework supports:
 *  - meta: mod.json fields (identity, version gating, dependencies)
 *  - classes / items / monsters / spells / patches: schema-shaped objects
 *  - sounds / music: mod.json "sounds" / "music" entries (exported inline into mod.json)
 *  - scripts: classId -> { lang: 'lua'|'js'|'ts', code } behavior script that
 *    ships next to the class JSON (classes/<stem>.<lang>)
 *  - assets: mod-relative path -> data URL (portraits/icons) shipped in the zip
 *  - editing: { kind, id } handoff so a Mod Builder "Edit" loads a saved def
 *    back into its editor
 *  - baseline: snapshot for the Mod Builder diff panel
 * The Mod Builder bundles all of it into a zip (see lib/exportZip).
 */
import { createContext, useContext, useEffect, useMemo, useReducer } from 'react';
import { reducer, initialState } from '@/state/modReducer.js';

const ModContext = createContext(null);

const STORAGE_KEY = 'sam-mod-state';


/** Upsert `def` into `list` by id (replace if the id exists, else append). */


/** Lazy initializer: rehydrate the last session from localStorage if present. */
function init(base) {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return base;
    const saved = JSON.parse(raw);
    // Merge over defaults so a newer schema shape (added fields) stays valid.
    return {
      ...base,
      ...saved,
      meta: { ...base.meta, ...(saved.meta ?? {}) },
      editing: null,   // transient — never restore mid-edit handoff
      baseline: null,  // diff resets on reload
    };
  } catch {
    return base;
  }
}

/*
 * How much asset data (data-URL characters) the autosave tries to keep. localStorage holds
 * about 5M characters per site, and everything else in the mod is small next to audio.
 *
 * Without a budget, one 30 MB music track (40M characters of base64) made every autosave
 * fail, and the fallback then saved NO assets at all -- so a reload lost every portrait, icon
 * and sound too, not just the track. It also meant stringifying 40 MB on every keystroke only
 * to throw it away. Now the largest assets are left out first, until the rest fits; whatever
 * is left out stays in memory for this session (export works) and the Mod Builder flags it
 * as "not bundled" after a reload.
 */
const PERSIST_ASSET_BUDGET = 4_000_000;

function persistableAssets(assets) {
  const entries = Object.entries(assets || {});
  let total = entries.reduce((n, [, v]) => n + String(v).length, 0);
  if (total <= PERSIST_ASSET_BUDGET) return assets || {};
  const kept = { ...assets };
  for (const [k, v] of [...entries].sort((a, b) => String(b[1]).length - String(a[1]).length)) {
    if (total <= PERSIST_ASSET_BUDGET) break;
    delete kept[k];
    total -= String(v).length;
  }
  return kept;
}

/** Persist the durable slice; drop assets first if we blow the quota. */
function persist(state) {
  const durable = {
    meta: state.meta,
    classes: state.classes,
    items: state.items,
    monsters: state.monsters,
    spells: state.spells,
    effects: state.effects,
    races: state.races,
    sounds: state.sounds,
    music: state.music,
    recipes: state.recipes,
    patches: state.patches,
    scripts: state.scripts,
    assets: persistableAssets(state.assets),
  };
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(durable));
  } catch {
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify({ ...durable, assets: {} }));
    } catch {
      /* give up quietly — export/Test-in-Barony is still the durable output */
    }
  }
}

export function ModProvider({ children }) {
  const [state, dispatch] = useReducer(reducer, initialState, init);

  useEffect(() => { persist(state); }, [state]);

  const value = useMemo(() => ({ ...state, dispatch }), [state]);
  return <ModContext.Provider value={value}>{children}</ModContext.Provider>;
}

export function useMod() {
  const ctx = useContext(ModContext);
  if (!ctx) {
    throw new Error('useMod must be used inside <ModProvider>');
  }
  return ctx;
}
