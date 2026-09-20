/*
 * What each editor OWNS, and what it must therefore leave alone.
 *
 * Every editor builds its definition from scratch on save: `buildDef()` starts with `{}` and
 * adds the fields it has a control for. Anything the author put in the file that the editor
 * has no control for is simply not in the new object, so saving DELETES it, silently, with a
 * green "Saved" next to it. RaceEditor lost `first_person` and `extra_limbs` that way --
 * `first_person` being the direct companion to bent arms, so "open a race, add bent to both
 * arms, save" was exactly the workflow that stripped the first-person arms. ItemEditor lost
 * `model_states` / `model_fp_states`, ClassEditor `blood_diet`, SpellEditor `difficulty`.
 *
 * The fix cannot be "also copy first_person and extra_limbs": that is the same list one
 * release later. It has to be the other way round -- an editor states the keys it OWNS, and
 * everything else on the definition it opened is carried through untouched. A field added to
 * a schema after this editor was written is then safe by default, which is the only version
 * of this that stays true.
 *
 * Why own-keys rather than "spread the old def under the new one": spreading would make a
 * field impossible to REMOVE. Clear the description box and `buildDef` stops emitting
 * `description`; the old value would come straight back from the spread. Deleting exactly
 * the keys the editor can write, and only those, keeps "clear the box" meaning what it says.
 *
 * A key here must be a real schema property -- the test cross-checks both lists, because a
 * typo would silently move a field into the carried set and stop the editor being able to
 * clear it.
 */

export const EDITOR_KEYS = {
  // ClassEditor.jsx buildDef(). `blood_diet` is in class.schema.json and has no control.
  class: [
    'id', 'name', 'stats', 'description', 'skills', 'starting_items', 'starting_spells',
    'stat_growth', 'hp_mp_growth', 'mp_regen', 'appearance', 'gold', 'portrait',
    'portrait_selected', 'ratings',
  ],
  // ItemEditor.jsx buildDef(). `model_states` / `model_fp_states` have no control.
  item: [
    'id', 'name_identified', 'name_unidentified', 'description', 'category', 'slot',
    'weapon_skill', 'traits', 'weight', 'gold_value', 'level', 'model', 'model_fp',
    'model_from_item', 'icon', 'attributes', 'stackable', 'magic_level', 'sounds',
  ],
  // MonsterEditor.jsx buildDef() -- complete against monster.schema.json today.
  monster: [
    'id', 'name', 'base_type', 'sex', 'appearance', 'stats', 'random_stats', 'proficiencies',
    'properties', 'traits', 'body', 'equipped_items', 'inventory_items', 'followers',
    'shopkeeper_properties', 'spawn', 'sounds', 'music',
  ],
  // SpellEditor.jsx buildDef(). `difficulty` has no control.
  spell: [
    'id', 'name', 'payload', 'description', 'mana_cost', 'projectile_type', 'damage_min',
    'damage_max', 'range', 'speed', 'on_hit_effect', 'on_hit_duration', 'on_hit_chance',
    'icon', 'starting_spell',
  ],
  // EffectEditor.jsx buildDef() -- complete against effect.schema.json today.
  effect: [
    'id', 'name', 'tooltip', 'icon', 'duration_ticks', 'stat_modifiers', 'move_speed_mult',
    'hp_per_second', 'mp_per_second', 'ac_mod', 'damage_mult', 'grants', 'hud_hidden', 'curable',
  ],
  // RaceEditor.jsx buildDef(). `first_person` and `extra_limbs` have no control.
  race: [
    'id', 'name', 'host_body', 'description', 'stat_modifiers', 'blood_diet',
    'starting_spells', 'limb_models', 'allies', 'enemies',
  ],
  // RecipeEditor.jsx buildDef() -- complete against recipe.schema.json today.
  recipe: ['id', 'item', 'kit', 'skill_level', 'materials', 'metal_cost', 'magic_cost', 'status'],
  // PatchEditor.jsx buildDef() -- complete against patch.schema.json today.
  patch: ['target', 'operations'],
};

/**
 * The definition to save: everything `next` says, plus everything on `prev` that this editor
 * has no say in (`$schema` included -- it is a real key an author may have written by hand).
 *
 * `prev` is the definition the editor opened, or null for a brand new one.
 */
export function carryUnknown(prev, next, kind) {
  if (!prev || typeof prev !== 'object' || Array.isArray(prev)) return next;
  const owned = EDITOR_KEYS[kind];
  if (!owned) return next;
  // Start from what the editor built, so the familiar key order (and the preview) is
  // unchanged and a value the editor produced always wins over the old one.
  const out = { ...next };
  for (const [k, v] of Object.entries(prev)) {
    if (!owned.includes(k) && !(k in next)) out[k] = v;
  }
  return out;
}
