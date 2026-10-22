/*
 * Checks the text a spell's "on_hit_effect" holds against what S.A.M can actually resolve, so the
 * Spell Editor can say so BEFORE the mod is exported. The engine looks the text up in this order
 * (framework/sam_spells.cpp, samEffectNameToId in sam_lua_runtime.cpp):
 *   1. a custom effect's exact id, "namespace:effect" -- case-sensitive, namespace required;
 *   2. a vanilla effect name, with or without EFF_, any case;
 *   3. a raw custom slot, 135..159 or "CUSTOM:n".
 * Anything else is ignored with one line in sam_log.txt and nothing on screen, which is what made
 * "my custom effect just disappears" so hard to diagnose.
 */
import { EFFECTS } from '@/data/samApi.js';

const VANILLA = new Set(EFFECTS.map((e) => e.toUpperCase()));

/**
 * @param {string} text        what the author typed
 * @param {string[]} customIds the "namespace:effect" ids of the mod's own effects
 * @returns {{status: 'empty'|'custom'|'vanilla'|'slot'|'wrong-case'|'missing-namespace'|'unknown', id?: string}}
 *   `id` is the exact id to use when the text is a near miss of one of the mod's effects.
 */
export function checkOnHitEffect(text, customIds = []) {
  const t = (text ?? '').trim();
  if (!t) return { status: 'empty' };

  if (customIds.includes(t)) return { status: 'custom', id: t };

  const upper = t.toUpperCase();
  const bare = upper.startsWith('EFF_') ? upper.slice(4) : upper;
  if (VANILLA.has(bare)) return { status: 'vanilla' };

  const slot = /^(?:CUSTOM:)?(\d+)$/.exec(upper);
  if (slot && Number(slot[1]) >= 135 && Number(slot[1]) <= 159) return { status: 'slot' };

  const lower = t.toLowerCase();
  const sameIgnoringCase = customIds.find((id) => id.toLowerCase() === lower);
  if (sameIgnoringCase) return { status: 'wrong-case', id: sameIgnoringCase };

  // "plague", "Plague" or "EFF_PLAGUE" for an effect saved as "mymod:plague".
  const slug = lower.replace(/^eff_/, '');
  const sameSlug = customIds.find((id) => id.slice(id.indexOf(':') + 1).toLowerCase() === slug);
  if (sameSlug) return { status: 'missing-namespace', id: sameSlug };

  return { status: 'unknown' };
}

/** Sentence for the editor, or '' when there is nothing to say. `tone` is 'ok' | 'warn'. */
export function describeOnHitEffect(result) {
  switch (result.status) {
    case 'custom': return { tone: 'ok', text: `Your effect ${result.id}.` };
    case 'vanilla': return { tone: 'ok', text: 'A built-in status effect.' };
    case 'slot': return { tone: 'ok', text: 'A custom effect slot number.' };
    case 'wrong-case':
    case 'missing-namespace':
      return { tone: 'warn', text: `That does not match an effect. Custom effect ids need the namespace and exact lowercase spelling. Use ${result.id}.` };
    case 'unknown':
      return { tone: 'warn', text: 'That is not one of this mod\'s effects or a known built-in status, so the game will ignore it. Pick one from the list.' };
    default: return null;
  }
}
