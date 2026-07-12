/*
 * Session-only mod state (no localStorage by design — see SAM_WORKFLOW):
 *  - meta: the mod.json fields being built
 *  - classes / items: schema-shaped objects saved from the editors
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
  classes: [], // class.schema.json-shaped objects
  items: [],   // item.schema.json-shaped objects
};

function reducer(state, action) {
  switch (action.type) {
    case 'setMeta':
      return { ...state, meta: { ...state.meta, ...action.patch } };
    case 'saveClass': {
      // replace by id if it exists, else append
      const i = state.classes.findIndex((c) => c.id === action.def.id);
      const classes = i >= 0
        ? state.classes.map((c, j) => (j === i ? action.def : c))
        : [...state.classes, action.def];
      return { ...state, classes };
    }
    case 'removeClass':
      return { ...state, classes: state.classes.filter((c) => c.id !== action.id) };
    case 'saveItem': {
      const i = state.items.findIndex((it) => it.id === action.def.id);
      const items = i >= 0
        ? state.items.map((it, j) => (j === i ? action.def : it))
        : [...state.items, action.def];
      return { ...state, items };
    }
    case 'removeItem':
      return { ...state, items: state.items.filter((it) => it.id !== action.id) };
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
