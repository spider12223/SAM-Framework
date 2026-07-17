/*
 * Turns a block spec into Lua.
 *
 * The whole point of this file is that the generated shape is always correct, because
 * the two ways to get it wrong are silent and cost people hours:
 *
 *   1. S.A.M calls exactly TWO functions: on_event(event) and on_tick(event). Every
 *      other hook name — on_action_pressed, on_hit — is an event.name INSIDE on_event.
 *      Writing `function on_action_pressed(event)` registers nothing and the script
 *      does nothing, with no error. The builder can't emit that.
 *   2. on_tick is the mirror image: it IS its own function and is never an event.name.
 *      Handling ticks inside on_event silently never fires. The builder can't emit that
 *      either.
 *
 * It also only ever reads event fields the chosen trigger actually carries (see
 * actionsFor() in blocks.js), so it can't generate a read of a field that's always nil.
 */
import { findTrigger, findCondition, findAction, triggerHasPlayer, EVERY_SECONDS, TICKS_PER_SECOND } from '@/data/blocks.js';

const HEADER = '-- Built with the S.A.M block builder. Edit freely — this is just Lua.';

/** Wrap prose into `-- ` comment lines. Some manifest gotchas are a paragraph long, and
 *  one 600-character comment line is worse than no comment at all. */
function comment(text, width = 78) {
  const out = [];
  let line = '';
  for (const word of String(text).split(/\s+/).filter(Boolean)) {
    if (line && (line.length + word.length + 1) > width) { out.push(`-- ${line}`); line = word; }
    else { line = line ? `${line} ${word}` : word; }
  }
  if (line) out.push(`-- ${line}`);
  return out;
}

/** Render one condition as a Lua early-return guard. */
function guard(row) {
  const def = findCondition(row.id);
  if (!def) return null;
  const expr = def.lua(row.params || {});
  // negate flips the guard: "is under an effect" -> "is NOT under an effect"
  return row.negate ? `  if ${expr} then return end` : `  if not (${expr}) then return end`;
}

function actionLine(row) {
  const def = findAction(row.id);
  if (!def) return null;
  return `  ${def.lua(row.params || {})}`;
}

/**
 * spec = {
 *   trigger:    { id, params },
 *   conditions: [{ id, params, negate }],
 *   actions:    [{ id, params }],
 * }
 */
export function generateLua(spec) {
  const t = findTrigger(spec?.trigger?.id);
  if (!t) return `${HEADER}\n-- Pick a trigger to start.`;

  const conds = (spec.conditions || []).map(guard).filter(Boolean);
  const acts = (spec.actions || []).map(actionLine).filter(Boolean);
  const body = [];

  if (t.id === EVERY_SECONDS) {
    const secs = Number(spec.trigger.params?.seconds) || 1;
    const ticks = Math.max(1, Math.round(secs * TICKS_PER_SECOND));
    const lines = [
      HEADER,
      `-- Runs every ${secs} second${secs === 1 ? '' : 's'}.`,
      '--',
      '-- Ticks arrive on their own on_tick(event) handler — never as an event through',
      '-- on_event. A tick carries no player, so this acts on the local host (player 0),',
      '-- which is you in single-player.',
      'function on_tick(event)',
      `  if event.tick_count % ${ticks} ~= 0 then return end`,
      '  local player = 0',
    ];
    body.push(...lines, ...conds, ...acts, 'end');
    return body.join('\n') + '\n';
  }

  const lines = [HEADER, ...comment(`Fires when ${t.whenFired}.`)];
  if (t.gotcha) lines.push('--', ...comment(`NOTE: ${t.gotcha}`));
  lines.push('function on_event(event)', `  if event.name ~= ${JSON.stringify(t.id)} then return end`);
  if (triggerHasPlayer(t)) {
    lines.push('  local player = event.player');
  } else {
    lines.push('  -- This event carries no player; in single-player the local host is you.');
    lines.push('  local player = 0');
  }
  body.push(...lines, ...conds, ...acts, 'end');
  return body.join('\n') + '\n';
}

/** A plain-English read-back of the spec, so you can sanity-check without reading Lua. */
export function describeSpec(spec) {
  const t = findTrigger(spec?.trigger?.id);
  if (!t) return 'Pick a trigger to start.';
  const when = t.id === EVERY_SECONDS
    ? `Every ${Number(spec.trigger.params?.seconds) || 1} second(s)`
    : `When ${t.whenFired}`;
  const conds = (spec.conditions || []).map((r) => {
    const d = findCondition(r.id);
    return d ? `${r.negate ? 'the player is NOT' : 'the player'} ${d.label.replace(/^is /, '')}` : null;
  }).filter(Boolean);
  const acts = (spec.actions || []).map((r) => findAction(r.id)?.label).filter(Boolean);
  let s = when;
  if (conds.length) s += `, and ${conds.join(', and ')}`;
  s += acts.length ? ` → ${acts.join(', then ')}.` : ' → (add an action).';
  return s;
}
