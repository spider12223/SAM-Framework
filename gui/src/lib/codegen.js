/*
 * Turns a block spec into Lua.
 *
 * The generated shape is always correct, because the ways to get it wrong are silent and
 * cost people hours:
 *
 *   1. S.A.M calls exactly TWO functions: on_event(event) and on_tick(event). Every
 *      other hook name — on_action_pressed, on_hit — is an event.name INSIDE on_event.
 *      Writing `function on_action_pressed(event)` registers nothing and the script
 *      does nothing, with no error. The builder can't emit that.
 *   2. on_tick is the mirror image: it IS its own function and is never an event.name.
 *      Handling ticks inside on_event silently never fires.
 *   3. A script may define on_event ONCE. Two abilities are two BRANCHES of one handler,
 *      not two handlers — so the builder takes a list of rules and emits a single
 *      on_event / on_tick pair covering all of them. Concatenating two generated scripts
 *      by hand is a syntax error ("<eof> expected near 'end'"), and a syntax error means
 *      the whole file fails to load, so even the ability that worked alone stops working.
 *
 * It also only ever reads event fields the chosen trigger actually carries (see
 * actionsFor() in blocks.js), so it can't generate a read of a field that's always nil.
 */
import { findTrigger, findCondition, findAction, triggerHasPlayer, EVERY_SECONDS, TICKS_PER_SECOND } from '@/data/blocks.js';

const HEADER = '-- Built with the S.A.M block builder. Edit freely — this is just Lua.';

/** Wrap prose into `-- ` comment lines. Some manifest gotchas are a paragraph long, and
 *  one 600-character comment line is worse than no comment at all. */
function comment(text, width = 78, indent = '') {
  const out = [];
  let line = '';
  for (const word of String(text).split(/\s+/).filter(Boolean)) {
    if (line && (line.length + word.length + 1) > width) { out.push(`${indent}-- ${line}`); line = word; }
    else { line = line ? `${line} ${word}` : word; }
  }
  if (line) out.push(`${indent}-- ${line}`);
  return out;
}

/**
 * Render one condition as a POSITIVE Lua expression.
 *
 * This used to emit early-return guards — `if not (sam_has_effect(...)) then return end`.
 * Correct, but a double negative: the block said "is under an effect" and the code said
 * "if NOT under the effect, leave". The first person to read their own generated script
 * asked "if not is if it has or it has not?", which is the question this tab exists to
 * prevent. Conditions now read the same direction as the block that made them.
 *
 * `not (...)` keeps its parentheses on purpose: several conditions are comparisons
 * (`a == b`), and `not a == b` parses as `(not a) == b` in Lua — silently wrong.
 */
function condExpr(row) {
  const def = findCondition(row.id);
  if (!def) return null;
  const expr = def.lua(row.params || {});
  return row.negate ? `not (${expr})` : expr;
}

/** An action is one or more lines — "for a while" needs an apply plus a revert timer. */
function actionLines(row) {
  const def = findAction(row.id);
  if (!def) return null;
  const out = def.lua(row.params || {});
  return Array.isArray(out) ? out : [out];
}

/** Does any rule use a temporary change, needing the unique-timer-id counter? */
function needsSeq(rules) {
  return rules.some((r) => (r.actions || []).some((row) => {
    const def = findAction(row.id);
    return typeof def?.needsSeq === 'function' && def.needsSeq(row.params || {});
  }));
}

/** One rule's IF + DO, indented. Conditions join with `and`; actions sit inside. */
function ruleBody(rule, indent) {
  const conds = (rule.conditions || []).map(condExpr).filter(Boolean);
  const acts = (rule.actions || []).map(actionLines).filter(Boolean).flat();
  if (!acts.length) return [`${indent}-- (add an action)`];
  if (!conds.length) return acts.map((a) => `${indent}${a}`);
  return [
    `${indent}if ${conds.join(' and ')} then`,
    ...acts.map((a) => `${indent}  ${a}`),
    `${indent}end`,
  ];
}

const SEQ_PREAMBLE = [
  '-- Counter for one-off timer ids. Each temporary change reverts on its own timer, so',
  '-- two of them overlapping both undo themselves — a shared id would replace the first',
  '-- revert and quietly leak the change.',
  '--',
  '-- Move speed is the deliberate exception: it is one slider per player rather than a',
  '-- stack, so every speed duration shares one id and re-applying REFRESHES it.',
  'local _tmp_seq = 0',
  '',
];

/**
 * spec = { rules: [{ trigger:{id,params}, conditions:[...], actions:[...] }, ...] }
 *
 * Rules sharing a trigger become one branch containing both — NOT two `elseif`s on the
 * same event name, where the second could never run.
 */
export function generateLua(spec) {
  const rules = (spec?.rules || []).filter((r) => findTrigger(r?.trigger?.id));
  if (!rules.length) return `${HEADER}\n-- Add an ability to start.`;

  const out = [HEADER];
  const tickRules = rules.filter((r) => r.trigger.id === EVERY_SECONDS);
  const eventRules = rules.filter((r) => r.trigger.id !== EVERY_SECONDS);
  if (rules.length > 1) {
    out.push(...comment(`This class has ${rules.length} abilities. They share one on_event `
      + '(and/or one on_tick) because a script may only define each handler once.'));
  }
  out.push(...(needsSeq(rules) ? SEQ_PREAMBLE : ['']));

  if (eventRules.length) {
    // Group by trigger, preserving the order abilities were added.
    const byTrigger = new Map();
    for (const r of eventRules) {
      if (!byTrigger.has(r.trigger.id)) byTrigger.set(r.trigger.id, []);
      byTrigger.get(r.trigger.id).push(r);
    }
    out.push('function on_event(event)');
    let first = true;
    for (const [tid, group] of byTrigger) {
      const t = findTrigger(tid);
      out.push(`  ${first ? 'if' : 'elseif'} event.name == ${JSON.stringify(tid)} then`);
      first = false;
      out.push(...comment(`Fires when ${t.whenFired}.`, 74, '    '));
      if (t.gotcha) out.push(...comment(`NOTE: ${t.gotcha}`, 74, '    '));
      if (triggerHasPlayer(t)) {
        out.push('    local player = event.player');
      } else {
        out.push('    -- This event carries no player; in single-player the local host is you.');
        out.push('    local player = 0');
      }
      for (const r of group) out.push(...ruleBody(r, '    '));
    }
    out.push('  end', 'end');
  }

  if (tickRules.length) {
    if (eventRules.length) out.push('');
    out.push(
      '-- Ticks arrive on their own on_tick(event) handler — never as an event through',
      '-- on_event. A tick carries no player, so this acts on the local host (player 0),',
      '-- which is you in single-player.',
      'function on_tick(event)',
      '  local player = 0',
    );
    for (const r of tickRules) {
      const secs = Number(r.trigger.params?.seconds) || 1;
      const ticks = Math.max(1, Math.round(secs * TICKS_PER_SECOND));
      out.push(`  if event.tick_count % ${ticks} == 0 then -- every ${secs}s`);
      out.push(...ruleBody(r, '    '));
      out.push('  end');
    }
    out.push('end');
  }

  return out.join('\n') + '\n';
}

/**
 * How long an action lasts, in words — appended to the read-back.
 *
 * The read-back is the only part of this tab a non-programmer actually reads, so it has to
 * carry the duration. "set move speed" and "set move speed for 3 seconds" are different
 * abilities, and a summary that renders both as "set move speed" is lying by omission.
 */
function durationPhrase(row) {
  const p = row.params || {};
  const secs = Number(p.seconds) || 1;
  if (p.duration === 'for a while') return ` for ${secs}s`;
  if (p.duration === 'fading away') return ` fading out over ${secs}s`;
  if (p.duration === 'until') {
    const said = conditionPhrase({ id: p.until_id, params: p.until_params, negate: p.until_negate });
    return said ? ` until ${said}` : ' until… (pick a condition)';
  }
  return '';
}

/**
 * A condition as an English clause, for the read-back.
 *
 * A block's `label` is dropdown-speak ("stat compares to a value") and does not survive
 * being dropped into a sentence — "until the player stat compares to a value" is not a
 * thing anyone says. So conditions carry a `phrase` that reads as a clause, with the params
 * filled in ("until HP < 20"). Custom bricks have no phrase, so they fall back to the label.
 */
function conditionPhrase(row) {
  const d = findCondition(row?.id);
  if (!d) return null;
  const p = row.params || {};
  try {
    if (row.negate && d.phraseNeg) return d.phraseNeg(p);
    if (!row.negate && d.phrase) return d.phrase(p);
  } catch { /* a half-filled param — fall through to the label */ }
  const bare = d.label.replace(/^is /, '');
  return `the player is ${row.negate ? 'NOT ' : ''}${bare}`;
}

/** A plain-English read-back, so you can sanity-check without reading Lua. */
export function describeRule(rule) {
  const t = findTrigger(rule?.trigger?.id);
  if (!t) return 'Pick a trigger to start.';
  const when = t.id === EVERY_SECONDS
    ? `Every ${Number(rule.trigger.params?.seconds) || 1} second(s)`
    : `When ${t.whenFired}`;
  const conds = (rule.conditions || []).map(conditionPhrase).filter(Boolean);
  const acts = (rule.actions || []).map((r) => {
    const d = findAction(r.id);
    return d ? `${d.label}${durationPhrase(r)}` : null;
  }).filter(Boolean);
  let s = when;
  if (conds.length) s += `, and ${conds.join(', and ')}`;
  s += acts.length ? ` → ${acts.join(', then ')}.` : ' → (add an action).';
  return s;
}
