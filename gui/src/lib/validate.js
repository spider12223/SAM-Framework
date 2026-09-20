/*
 * ajv validation against the real S.A.M schemas (draft-07).
 * validate(kind, data) -> { valid, errors: [{ path, message }] }
 */
import Ajv from 'ajv';
import {
  modSchema, classSchema, itemSchema, monsterSchema, spellSchema, patchSchema, effectSchema, raceSchema, soundSchema, recipeSchema,
} from '@/data/schemas.js';

const ajv = new Ajv({ allErrors: true, strict: false });

const validators = {
  mod: ajv.compile(modSchema),
  class: ajv.compile(classSchema),
  item: ajv.compile(itemSchema),
  monster: ajv.compile(monsterSchema),
  spell: ajv.compile(spellSchema),
  patch: ajv.compile(patchSchema),
  effect: ajv.compile(effectSchema),
  race: ajv.compile(raceSchema),
  sound: ajv.compile(soundSchema),
  recipe: ajv.compile(recipeSchema),
  // Music has no schema file of its own: an entry only ever lives inside mod.json "music", so
  // one entry is checked against that item definition, straight out of mod.schema.json.
  music: ajv.compile(modSchema.properties.music.items),
};

export const SCHEMA_KINDS = Object.keys(validators);

export function validate(kind, data) {
  const fn = validators[kind];
  if (!fn) {
    throw new Error(`Unknown schema kind: ${kind}`);
  }
  const valid = fn(data);
  const errors = (fn.errors ?? []).map((e) => ({
    path: e.instancePath || '(root)',
    message: e.message ?? 'invalid',
    params: e.params,
  }));
  return { valid: !!valid, errors };
}
