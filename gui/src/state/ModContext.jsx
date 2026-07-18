/*
 * Working mod state, autosaved to localStorage so a refresh/crash never loses
 * work (Tier 3). Holds every content type the framework supports:
 *  - meta: mod.json fields (identity, version gating, dependencies)
 *  - classes / items / monsters / spells / patches: schema-shaped objects
 *  - scripts: classId -> { lang: 'lua'|'js'|'ts', code } behavior script that
 *    ships next to the class JSON (classes/<stem>.<lang>)
 *  - assets: mod-relative path -> data URL (portraits/icons) shipped in the zip
 *  - editing: { kind, id } handoff so a Mod Builder "Edit" loads a saved def
 *    back into its editor
 *  - baseline: snapshot for the Mod Builder diff panel
 * The Mod Builder bundles all of it into a zip (see lib/exportZip).
 */
import { createContext, useContext, useEffect, useMemo, useReducer } from 'react';

const ModContext = createContext(null);

const STORAGE_KEY = 'sam-mod-state';

const initialState = {
  meta: {
    namespace: '',
    name: '',
    author: '',
    version: '1.0.0',
    framework_min_version: '0.1.0',
    framework_max_version: '',
    barony_min_version: '',
    barony_max_version: '',
    incompatible_with_barony_version: '',
    dependencies: [],       // raw strings: "core", "?optional", "!incompatible", "core@1.0.0"
    description: '',
  },
  classes: [],   // class.schema.json-shaped objects
  items: [],     // item.schema.json-shaped objects
  monsters: [],  // monster.schema.json-shaped objects
  spells: [],    // spell.schema.json-shaped objects
  effects: [],   // effect.schema.json-shaped objects (custom status effects)
  races: [],     // race.schema.json-shaped objects (custom playable races)
  sounds: [],    // sound.schema.json-shaped objects (custom sounds; .ogg lives in assets)
  patches: [],   // patch.schema.json-shaped objects
  scripts: {},   // classId -> { lang, code }
  assets: {},    // 'portraits/x.png' -> 'data:image/png;base64,...'
  editing: null, // { kind: 'class'|'item'|'monster'|'spell'|'patch', id } | null
  baseline: null,
};

/** Upsert `def` into `list` by id (replace if the id exists, else append). */
function upsert(list, def) {
  const i = list.findIndex((x) => x.id === def.id);
  return i >= 0 ? list.map((x, j) => (j === i ? def : x)) : [...list, def];
}

function reducer(state, action) {
  switch (action.type) {
    case 'setMeta':
      return { ...state, meta: { ...state.meta, ...action.patch } };
    case 'saveClass':
      return { ...state, classes: upsert(state.classes, action.def) };
    case 'removeClass': {
      const scripts = { ...state.scripts };
      delete scripts[action.id]; // a class's behavior script goes with it
      return { ...state, classes: state.classes.filter((c) => c.id !== action.id), scripts };
    }
    case 'saveItem':
      return { ...state, items: upsert(state.items, action.def) };
    case 'removeItem':
      return { ...state, items: state.items.filter((it) => it.id !== action.id) };
    case 'saveMonster':
      return { ...state, monsters: upsert(state.monsters, action.def) };
    case 'removeMonster':
      return { ...state, monsters: state.monsters.filter((m) => m.id !== action.id) };
    case 'saveSpell':
      return { ...state, spells: upsert(state.spells, action.def) };
    case 'removeSpell':
      return { ...state, spells: state.spells.filter((s) => s.id !== action.id) };
    case 'saveEffect':
      return { ...state, effects: upsert(state.effects, action.def) };
    case 'removeEffect':
      return { ...state, effects: state.effects.filter((e) => e.id !== action.id) };
    case 'saveRace':
      return { ...state, races: upsert(state.races, action.def) };
    case 'removeRace': {
      const scripts = { ...state.scripts };
      delete scripts[action.id]; // a race's behavior script goes with it
      return { ...state, races: state.races.filter((r) => r.id !== action.id), scripts };
    }
    case 'saveSound':
      return { ...state, sounds: upsert(state.sounds, action.def) };
    case 'removeSound':
      return { ...state, sounds: state.sounds.filter((s) => s.id !== action.id) };
    case 'savePatch': {
      // Patches have no id; key them by target (one merged op-list per file).
      const i = state.patches.findIndex((p) => p.target === action.def.target);
      const patches = i >= 0
        ? state.patches.map((p, j) => (j === i ? action.def : p))
        : [...state.patches, action.def];
      return { ...state, patches };
    }
    case 'removePatch':
      return { ...state, patches: state.patches.filter((p) => p.target !== action.target) };
    case 'saveScript': {
      const scripts = { ...state.scripts };
      if (action.code && action.code.trim()) {
        scripts[action.classId] = { lang: action.lang, code: action.code };
      } else {
        delete scripts[action.classId];
      }
      return { ...state, scripts };
    }
    case 'setAsset':
      return { ...state, assets: { ...state.assets, [action.path]: action.dataUrl } };
    case 'removeAsset': {
      const assets = { ...state.assets };
      delete assets[action.path];
      return { ...state, assets };
    }
    case 'setEditing':
      return { ...state, editing: { kind: action.kind, id: action.id } };
    case 'clearEditing':
      return { ...state, editing: null };
    case 'loadMod':
      // Wholesale replace (zip import).
      return {
        ...state,
        meta: action.meta,
        classes: action.classes ?? [],
        items: action.items ?? [],
        monsters: action.monsters ?? [],
        spells: action.spells ?? [],
        effects: action.effects ?? [],
        races: action.races ?? [],
        sounds: action.sounds ?? [],
        patches: action.patches ?? [],
        scripts: action.scripts ?? {},
        assets: action.assets ?? {},
        editing: null,
      };
    case 'setBaseline':
      return {
        ...state,
        baseline: structuredClone({
          meta: state.meta,
          classes: state.classes,
          items: state.items,
          monsters: state.monsters,
          spells: state.spells,
          effects: state.effects,
          races: state.races,
          sounds: state.sounds,
          patches: state.patches,
        }),
      };
    default:
      return state;
  }
}

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
    patches: state.patches,
    scripts: state.scripts,
    assets: state.assets,
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
