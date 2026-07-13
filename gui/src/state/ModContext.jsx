/*
 * Session-only mod state (no localStorage by design — see SAM_WORKFLOW):
 *  - meta: the mod.json fields being built
 *  - classes / items / monsters: schema-shaped objects saved from the editors
 *  - assets: mod-relative path -> data URL (e.g. uploaded portraits) shipped in the zip
 *  - baseline: snapshot taken at export/import time, drives the Mod Builder diff panel
 * The Mod Builder page bundles all of it into a zip.
 */
import { createContext, useContext, useMemo, useReducer } from 'react';

const ModContext = createContext(null);

const initialState = {
  meta: {
    namespace: '',
    name: '',
    author: '',
    version: '1.0.0',
    framework_min_version: '0.1.0',
    description: '',
  },
  classes: [],  // class.schema.json-shaped objects
  items: [],    // item.schema.json-shaped objects
  monsters: [], // monster.schema.json-shaped objects
  assets: {},   // 'portraits/x.png' -> 'data:image/png;base64,...'
  baseline: null, // { meta, classes, items, monsters } snapshot or null
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
    case 'removeClass':
      return { ...state, classes: state.classes.filter((c) => c.id !== action.id) };
    case 'saveItem':
      return { ...state, items: upsert(state.items, action.def) };
    case 'removeItem':
      return { ...state, items: state.items.filter((it) => it.id !== action.id) };
    case 'saveMonster':
      return { ...state, monsters: upsert(state.monsters, action.def) };
    case 'removeMonster':
      return { ...state, monsters: state.monsters.filter((m) => m.id !== action.id) };
    case 'setAsset':
      return { ...state, assets: { ...state.assets, [action.path]: action.dataUrl } };
    case 'removeAsset': {
      const assets = { ...state.assets };
      delete assets[action.path];
      return { ...state, assets };
    }
    case 'loadMod':
      // Wholesale replace (zip import) — setMeta only merges patches, so a
      // dedicated action keeps import semantics unambiguous.
      return {
        ...state,
        meta: action.meta,
        classes: action.classes ?? [],
        items: action.items ?? [],
        monsters: action.monsters ?? [],
        assets: action.assets ?? {},
      };
    case 'setBaseline':
      // Snapshot for the diff panel. Deep-cloned so later edits can't mutate it.
      return {
        ...state,
        baseline: structuredClone({
          meta: state.meta,
          classes: state.classes,
          items: state.items,
          monsters: state.monsters,
        }),
      };
    default:
      return state;
  }
}

export function ModProvider({ children }) {
  const [state, dispatch] = useReducer(reducer, initialState);
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
