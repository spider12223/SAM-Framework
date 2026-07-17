/*
 * Custom blocks — solidius's "lego pack edition".
 *
 * A custom block is a named brick you define once and reuse: a label, some params, and a
 * Lua template with {placeholders}. They live in localStorage and export/import as a JSON
 * "pack" so you can hand a friend a set of bricks and let them build without writing code.
 *
 * THE IMPORTANT PART — a pack must not smuggle the bugs back in. The whole reason the
 * builder is safe is that it only emits real functions; a custom template is free-text
 * Lua, so it could easily call sam_bogus() and silently do nothing forever. So every
 * template is linted against the real API manifest before it can be saved or imported,
 * with the same linter the Advanced editor uses.
 *
 * Templates are substituted, never evaluated. Nothing here runs the pack's code in the
 * browser — it only ever becomes text in the generated script.
 */
import { lintScript } from '@/lib/lintScript.js';

const KEY = 'sam.customBlocks.v1';
export const PARAM_TYPES = ['text', 'number', 'select'];

/** Fill {name} placeholders from params. Unknown placeholders are left visible on purpose
 *  so a broken template looks broken instead of silently emitting nothing. */
export function renderTemplate(tpl, params) {
  return String(tpl || '').replace(/\{(\w+)\}/g, (m, k) =>
    (params && params[k] !== undefined && params[k] !== '') ? String(params[k]) : m);
}

/**
 * Validate a block definition. Returns { ok, errors[] }.
 * Lints the template with placeholders filled by their defaults, so we check the shape of
 * the real emitted line rather than the raw template (where {x} would trip the parser).
 */
export function validateBlock(b) {
  const errors = [];
  if (!b?.label?.trim()) errors.push('Give the block a name.');
  if (!b?.lua?.trim()) errors.push('The Lua template is empty.');
  if (!['condition', 'action'].includes(b?.kind)) errors.push('Pick whether it is a condition or an action.');

  const names = (b.params || []).map((p) => (p.name || '').trim());
  if (names.some((n) => !n)) errors.push('Every parameter needs a name.');
  if (new Set(names).size !== names.length) errors.push('Two parameters share a name.');
  for (const n of names) {
    if (n && !/^[a-z][a-z0-9_]*$/i.test(n)) errors.push(`"${n}" is not a valid parameter name (letters, digits, _).`);
  }

  // Placeholders must resolve to a declared param, or the emitted Lua carries a literal {x}.
  const used = [...String(b.lua || '').matchAll(/\{(\w+)\}/g)].map((m) => m[1]);
  for (const u of new Set(used)) {
    if (!names.includes(u)) errors.push(`Template uses {${u}} but there is no parameter called "${u}".`);
  }

  // The real check: does the emitted line only call functions that exist?
  const filled = renderTemplate(b.lua, Object.fromEntries((b.params || []).map((p) => [p.name, p.default ?? '0'])));
  const probe = b.kind === 'condition' ? `if ${filled} then end` : filled;
  for (const d of lintScript(probe)) {
    if (d.severity === 'warn') errors.push(d.message);
  }
  if (b.kind === 'condition' && /\bsam_set_|\bsam_grant_|\bsam_deal_/.test(filled)) {
    errors.push('A condition should only TEST something — this one changes game state. Make it an action.');
  }
  return { ok: errors.length === 0, errors };
}

/** Turn a stored definition into the shape the builder's catalog expects. */
export function toCatalogEntry(b) {
  return {
    id: `custom:${b.id}`,
    label: `${b.label} (custom)`,
    negatable: b.kind === 'condition',
    custom: true,
    params: (b.params || []).map((p) => ({
      name: p.name,
      type: p.type || 'text',
      default: p.default ?? '',
      label: p.label || p.name,
      values: p.type === 'select'
        ? String(p.values || '').split(',').map((s) => s.trim()).filter(Boolean)
        : undefined,
    })),
    lua: (vals) => renderTemplate(b.lua, vals),
    note: b.note || 'Custom block.',
  };
}

export function loadBlocks() {
  try {
    const raw = JSON.parse(localStorage.getItem(KEY) || '[]');
    return Array.isArray(raw) ? raw : [];
  } catch { return []; }
}
export function saveBlocks(list) {
  try { localStorage.setItem(KEY, JSON.stringify(list)); return true; } catch { return false; }
}

/** Export as a shareable pack. */
export function exportPack(list, name = 'My Lego Pack') {
  return JSON.stringify({ sam_block_pack: 1, name, blocks: list }, null, 2);
}

/**
 * Import a pack. Every block is validated — an invalid one is REPORTED, not silently
 * dropped and not silently accepted, because a pack you can't see the code of is exactly
 * where a broken brick would hide.
 */
export function importPack(text) {
  let data;
  try { data = JSON.parse(text); } catch { return { ok: false, errors: ['That is not valid JSON.'], blocks: [] }; }
  if (!data || data.sam_block_pack !== 1 || !Array.isArray(data.blocks)) {
    return { ok: false, errors: ['Not a S.A.M block pack (expected { "sam_block_pack": 1, "blocks": [...] }).'], blocks: [] };
  }
  const good = [], errors = [];
  for (const b of data.blocks) {
    const v = validateBlock(b);
    if (v.ok) good.push({ ...b, id: b.id || Math.random().toString(36).slice(2, 9) });
    else errors.push(`"${b?.label || 'unnamed'}": ${v.errors.join(' ')}`);
  }
  return { ok: good.length > 0, errors, blocks: good, name: data.name };
}
