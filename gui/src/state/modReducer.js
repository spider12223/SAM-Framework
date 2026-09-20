/*
 * The mod-state reducer, on its own so it can be tested.
 *
 * It used to live inside ModContext.jsx, which meant nothing could import it: Node cannot load
 * .jsx. That mattered more than it sounds. A page that dispatches the wrong action shape does
 * not throw -- the reducer spreads `undefined`, state comes back unchanged, and the UI looks
 * like it saved. ModelEditor shipped exactly that (it sent { meta } where this reads { patch })
 * and every model it declared was silently discarded. Driving the reducer directly is the only
 * level at which that is catchable, so it lives here.
 */
import { audioKey, audioFiles, upsertAudio, pruneAudioAssets } from '@/lib/audio.js';

export const initialState = {
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
  // Sounds and music are exported INLINE into mod.json ("sounds" / "music"), one object each.
  // An entry either adds ({ id }) or replaces a vanilla one ({ replace }); its key is
  // audioKey() in lib/audio.js. The audio itself lives in assets under sounds/ and music/.
  sounds: [],    // sound.schema.json-shaped objects
  music: [],     // mod.schema.json "music" item-shaped objects
  recipes: [],   // recipe.schema.json-shaped objects (tinkering kit craftables)
  patches: [],   // patch.schema.json-shaped objects
  scripts: {},   // classId -> { lang, code }
  assets: {},    // 'portraits/x.png' -> 'data:image/png;base64,...'
  editing: null, // { kind: 'class'|'item'|'monster'|'spell'|'patch', id } | null
  baseline: null,
};

function upsert(list, def) {
  const i = list.findIndex((x) => x.id === def.id);
  return i >= 0 ? list.map((x, j) => (j === i ? def : x)) : [...list, def];
}

// Exported for tests. A page dispatching the wrong action shape is silently a no-op --
// ModelEditor sent { meta } where the reducer reads { patch }, so every model it
// declared was discarded with no error anywhere. That is only catchable by driving the
// reducer directly.
export function reducer(state, action) {
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
    case 'saveRecipe':
      return { ...state, recipes: upsert(state.recipes, action.def) };
    case 'deleteRecipe':
      return { ...state, recipes: state.recipes.filter((r) => r.id !== action.id) };
    // Sounds and music are updated IN PLACE: `prevKey` is the key the editor opened the entry
    // with, so renaming it replaces that entry instead of adding a second one. Audio the entry
    // no longer points at is dropped from assets unless another entry still uses it -- a file
    // left in sounds/ would still ship, and the engine's folder scan would still register it.
    case 'saveSound': {
      const { list, old } = upsertAudio(state.sounds ?? [], action.def, action.prevKey);
      return { ...state, sounds: list, assets: pruneAudioAssets(state.assets, audioFiles(old), list, state.music) };
    }
    case 'removeSound': {
      const key = action.key ?? (action.id ? audioKey({ id: action.id }) : '');
      const all = state.sounds ?? [];
      const gone = all.filter((s) => audioKey(s) === key);
      const sounds = all.filter((s) => audioKey(s) !== key);
      return { ...state, sounds, assets: pruneAudioAssets(state.assets, gone.flatMap(audioFiles), sounds, state.music) };
    }
    case 'saveMusic': {
      const music = state.music ?? [];
      const { list, old } = upsertAudio(music, action.def, action.prevKey);
      return { ...state, music: list, assets: pruneAudioAssets(state.assets, audioFiles(old), state.sounds ?? [], list) };
    }
    case 'removeMusic': {
      const music = state.music ?? [];
      const gone = music.filter((m) => audioKey(m) === action.key);
      const next = music.filter((m) => audioKey(m) !== action.key);
      return { ...state, music: next, assets: pruneAudioAssets(state.assets, gone.flatMap(audioFiles), state.sounds ?? [], next) };
    }
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
        // `blocks` is the visual block-builder's rule list — editor-only metadata so the
        // Basic tab can restore the exact bricks on reopen (solidius: "the builder doesn't
        // save"). It rides in GUI state / the draft only; the zip export ships just `code`.
        const entry = { lang: action.lang, code: action.code };
        if (Array.isArray(action.blocks) && action.blocks.length) entry.blocks = action.blocks;
        scripts[action.classId] = entry;
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
        music: action.music ?? [],
        recipes: action.recipes ?? [],
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
          music: state.music,
          recipes: state.recipes,
          patches: state.patches,
        }),
      };
    default:
      return state;
  }
}
