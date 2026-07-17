/*
 * Block catalog for the visual script builder ("Basic" tab).
 *
 * DESIGN RULE: derive from samApi.js wherever possible instead of re-listing things.
 * The manifest already carries every event with its real payload fields and every
 * function with its real params, so a hand-written copy here would just drift out of
 * sync and start offering hooks and functions that don't exist — which is precisely
 * the class of bug this builder is meant to make impossible.
 *
 * The three lists below (effects / slots / spells) can't be derived — they're engine
 * tables the manifest doesn't enumerate — so they're transcribed from the source and
 * annotated with where to re-check them.
 */
import { SAM_EVENTS, PLAYER_STATS, EFFECTS as API_EFFECTS, SPELLS as API_SPELLS } from '@/data/samApi.js';

/** Effect names the engine knows — from the manifest, so this can't drift from the
 *  runtime's table (it did: both sat at 14 of 135 for months). */
export const EFFECTS = API_EFFECTS;

/** Slot names samEquippedSlot() understands. ARMOR==BREASTPLATE, BOOTS==SHOES. */
export const SLOTS = [
  'WEAPON', 'SHIELD', 'HELMET', 'ARMOR', 'GLOVES', 'BOOTS', 'RING', 'AMULET', 'CLOAK', 'MASK',
];

/**
 * Attributes sam_get_stat / sam_set_stat accept — taken from the manifest so this can't
 * drift. HUNGER is 0..1500 (0 = starving); the tier edges are per-race, so scripts read
 * the raw number and player.on_hunger_change tells you when a tier is crossed.
 */
export const STATS = PLAYER_STATS.filter((s) => s !== 'LVL'); // LVL is just an alias of LEVEL

/** Castable spells — from the manifest, so this can't drift into a second copy. */
export const SPELLS = API_SPELLS;

export const TICKS_PER_SECOND = 50; // the engine's tick rate; "every N seconds" -> N*50 ticks

/** The synthetic trigger: on_tick throttled to an interval. Not an event — its own handler. */
export const EVERY_SECONDS = 'every_seconds';

/**
 * Triggers = every real event, straight from the manifest, plus the tick.
 * `payload` tells the builder which event fields exist, which is how we stop it from
 * emitting e.g. event.target_uid on an event that has no such field.
 */
export const TRIGGERS = [
  {
    id: EVERY_SECONDS,
    label: 'Every N seconds',
    category: 'tick',
    whenFired: 'repeatedly, on a timer',
    payload: [],
    params: [{ name: 'seconds', type: 'number', default: 5, min: 0.1, label: 'seconds' }],
    gotcha: 'Runs on the tick handler, which carries no player — actions use the local host (player 0).',
  },
  ...SAM_EVENTS
    .filter((e) => e.name !== 'on_tick')
    .map((e) => ({
      id: e.name,
      label: e.name,
      category: e.category,
      whenFired: e.whenFired,
      payload: e.payload || [],
      params: [],
      gotcha: e.gotcha || '',
    })),
];

/** Does this trigger hand us a player, or do we have to assume the host? */
export function triggerHasPlayer(trigger) {
  return (trigger?.payload || []).some((f) => f.field === 'player');
}
/** Field names this trigger's event actually carries (for uid-taking actions). */
export function triggerFields(trigger) {
  return (trigger?.payload || []).map((f) => f.field);
}

const q = (s) => JSON.stringify(String(s ?? ''));

/** How an amount is read: a flat number, or a share of the max / the current value. */
export const MODES = ['flat', '% of max', '% of current'];
/** Whether an HP change is allowed to be the thing that kills you. */
export const KILL_MODES = ['can kill', 'cannot kill'];

/**
 * How a change ENDS. Every one of these is a real question a modder asked out loud:
 *   "how to make an action temporal, aka the thing it changes is not infinite"   -> for a while
 *   "is it possible to set condition for REMOVING an action?"                    -> until
 *   "100 strength for 100 seconds, every second lost is 1 strength down"         -> fading away
 * Before this, only "change a stat" had any of it, and only the first one.
 */
export const DURATIONS = ['permanently', 'for a while', 'fading away', 'until'];
export const DURATION_LABELS = {
  permanently: 'permanently',
  'for a while': 'for a while, then snap back',
  'fading away': 'fading away gradually',
  until: 'until something happens',
};

export const POLL_TICKS = 10;       // 5 checks/sec — an "until" that reacts within 0.2s
export const FADE_STEP_TICKS = 50;  // fades give a step back once a second

/**
 * ONE timer id for every move-speed duration in a script, on purpose.
 *
 * Move speed is a single slider per player, not a stack. Two abilities cannot each own a
 * private copy of it, so the honest model is last-writer-wins with a refreshing timer —
 * exactly how Barony's own status effects behave when you re-apply them. Giving each
 * application a unique id (the way stat deltas correctly do) would schedule two reverts
 * to 1.0, and the FIRST one would cancel a buff the second application had just renewed.
 */
const SPEED_TIMER_ID = '"sam_move_speed"';
const NEUTRAL_SPEED = 1;

/**
 * Emit a change that can expire.
 *
 * Two shapes, because there are two kinds of change and conflating them is how you ship a
 * duration that quietly corrupts a save:
 *
 *   ADDITIVE (a stat delta) — the revert is "subtract what we added", per application.
 *     Overlapping buffs each undo their own contribution, so they stack correctly.
 *
 *     It must subtract what LANDED, not what was asked for. sam_set_stat clamps attributes
 *     to MAX_PLAYER_STAT_VALUE (248, stat.hpp:550), so at STR 200 a "+100" only moves you
 *     48 — and a revert of -100 leaves you at 148. That is 52 points of strength destroyed
 *     permanently, every time the ability fires. Measuring `applied` after the write is the
 *     whole fix, and it costs one extra sam_get_stat.
 *
 *   ABSOLUTE (move speed) — there is one value. "Revert" means "put it back to neutral",
 *     and re-applying must REFRESH rather than queue a second revert. See SPEED_TIMER_ID.
 *
 * `until` polls on a repeating timer and cancels itself once it fires. Self-cancelling from
 * inside the callback is safe: tickTimers takes its own reference for the due list for
 * precisely this case (sam_lua_runtime.cpp:2500).
 */
function temporal({ p, apply, read, write, absolute, from, idPrefix }) {
  const dur = p.duration || 'permanently';
  // `apply` takes the expression to build from: the live value when nothing needs to
  // remember it, or `_before` once we have to measure what the clamp let through.
  // Passing the wrong one is not a style question — a permanent change that reads
  // `_before` reads a nil global and errors on every single fire.
  if (dur === 'permanently') return apply(read);

  const secs = Math.max(0.1, Number(p.seconds) || 1);
  const lines = [apply(absolute ? null : '_before')].flat();

  if (absolute) {
    // One value, one owner. No _tmp_seq: a fresh id per application is exactly the bug.
    const back = `${write(NEUTRAL_SPEED)}`;
    if (dur === 'for a while') {
      return [...lines,
        `sam_set_timer(${SPEED_TIMER_ID}, ${Math.max(1, Math.round(secs * TICKS_PER_SECOND))}, function()`,
        `  ${back}`,
        'end)'];
    }
    if (dur === 'until') {
      const cond = untilExpr(p);
      if (!cond) return lines;
      return [...lines,
        `sam_set_repeating_timer(${SPEED_TIMER_ID}, ${POLL_TICKS}, function()`,
        `  if ${cond} then`,
        `    ${back}`,
        `    sam_cancel_timer(${SPEED_TIMER_ID})`,
        '  end',
        'end)'];
    }
    // fading: glide from the applied value to neutral over `secs`, in 1-second steps.
    const steps = Math.max(1, Math.round(secs));
    return [...lines,
      `local _k = 0`,
      `sam_set_repeating_timer(${SPEED_TIMER_ID}, ${FADE_STEP_TICKS}, function()`,
      '  _k = _k + 1',
      `  if _k >= ${steps} then`,
      `    ${back}`,
      `    sam_cancel_timer(${SPEED_TIMER_ID})`,
      '  else',
      `    ${write(`${from} + (${NEUTRAL_SPEED} - ${from}) * _k / ${steps}`)}`,
      '  end',
      'end)'];
  }

  // ---- additive ------------------------------------------------------------------
  // "until" with no condition chosen yet is just a permanent change — don't emit the
  // measure-and-revert scaffolding for a revert that will never run.
  if (dur === 'until' && !untilExpr(p)) return [apply(read)];

  // Measure what the clamp actually let through, and hand back exactly that.
  const pre = [
    `local _before = ${read}`,
    ...lines,
    `local _applied = ${read} - _before`,
  ];
  const idExpr = `"${idPrefix}_" .. _tmp_seq`;

  if (dur === 'for a while') {
    return [...pre,
      '_tmp_seq = _tmp_seq + 1',
      `sam_set_timer(${idExpr}, ${Math.max(1, Math.round(secs * TICKS_PER_SECOND))}, function()`,
      `  ${write(`${read} - _applied`)}`,
      'end)'];
  }

  if (dur === 'until') {
    const cond = untilExpr(p);
    if (!cond) return pre;
    return [...pre,
      '_tmp_seq = _tmp_seq + 1',
      `local _id = ${idExpr}`,
      `sam_set_repeating_timer(_id, ${POLL_TICKS}, function()`,
      `  if ${cond} then`,
      `    ${write(`${read} - _applied`)}`,
      '    sam_cancel_timer(_id)',
      '  end',
      'end)'];
  }

  // fading: hand it back over exactly `secs` seconds, whatever amount actually landed.
  // _held tracks what is still owed, so the total returned always equals _applied — a
  // buff that clamped on the way up still fades to precisely the baseline.
  const steps = Math.max(1, Math.round(secs));
  return [...pre,
    '_tmp_seq = _tmp_seq + 1',
    `local _id = ${idExpr}`,
    'local _held, _k = _applied, 0',
    `sam_set_repeating_timer(_id, ${FADE_STEP_TICKS}, function()`,
    '  _k = _k + 1',
    `  local _want = _k >= ${steps} and 0 or math.floor(_applied * (${steps} - _k) / ${steps})`,
    '  if _held ~= _want then',
    `    ${write(`${read} - (_held - _want)`)}`,
    '    _held = _want',
    '  end',
    `  if _k >= ${steps} then sam_cancel_timer(_id) end`,
    'end)'];
}

/** The condition chosen inside a "until…" duration, as a Lua expression. */
function untilExpr(p) {
  const def = findCondition(p.until_id);
  if (!def) return null;
  const expr = def.lua(p.until_params || {});
  // Parens are load-bearing: `not a == b` parses as `(not a) == b` in Lua.
  return p.until_negate ? `not (${expr})` : expr;
}

/**
 * Params every expiring action shares. One definition, so the wording can't drift.
 *
 * Only actions that change a LASTING VALUE get these. "give an item", "cast a spell" and
 * "show a message" are one-off events — there is nothing to expire. "apply a status effect"
 * already takes its own duration in ticks, because the engine expires it for you.
 */
const durationParams = () => [
  { name: 'duration', type: 'select', values: DURATIONS, labels: DURATION_LABELS, default: 'permanently', label: 'lasts' },
  {
    name: 'seconds', type: 'number', default: 5, min: 0.1, label: 'seconds',
    showIf: (p) => p.duration === 'for a while' || p.duration === 'fading away',
  },
  { name: 'until', type: 'condition', label: 'until', showIf: (p) => p.duration === 'until' },
];

/**
 * Build a HP/MP change. Handles solidius's two asks:
 *   - percentages ("lose 50% hp every 5 seconds") instead of only flat amounts, and
 *   - a can-kill flag, which floors the result at 1 exactly like the engine's own
 *     buddhamode ("Buddhas never die!") does inside Entity::setHP.
 *
 * "% of current" decays — halving 100 gives 50, 25, 12… it approaches death without a
 * flat guarantee. "% of max" is a constant bite. They feel very different, so the
 * builder makes you choose rather than guessing for you.
 *
 * math.floor is deliberate: on a LOSS it rounds away from you (loses the extra
 * fraction), so a percentage tick can always make progress instead of stalling at 0.
 */
function hpChange(stat, p) {
  const amt = Number(p.amount) || 0;
  const maxStat = stat === 'HP' ? 'MAXHP' : 'MAXMP';
  let delta;
  if (p.mode === '% of max') delta = `math.floor(sam_get_stat(player, ${q(maxStat)}) * ${amt} / 100)`;
  else if (p.mode === '% of current') delta = `math.floor(sam_get_stat(player, ${q(stat)}) * ${amt} / 100)`;
  else delta = `(${amt})`;
  const target = `sam_get_stat(player, ${q(stat)}) + ${delta}`;
  // sam_set_stat already clamps the top end to MAX, so only the floor needs handling.
  return p.can_kill === 'cannot kill'
    ? `sam_set_stat(player, ${q(stat)}, math.max(1, ${target}))`
    : `sam_set_stat(player, ${q(stat)}, ${target})`;
}

/**
 * Conditions — guards placed before the actions. Each maps to a real API call.
 * `negatable` gives solidius's "having no effect" for free.
 */
export const CONDITIONS = [
  {
    id: 'has_effect', label: 'is under an effect', negatable: true,
    params: [{ name: 'effect', type: 'select', values: EFFECTS, default: 'POISONED' }],
    lua: (p) => `sam_has_effect(player, ${q(p.effect)})`,
    phrase: (p) => `the player has ${p.effect}`,
    phraseNeg: (p) => `the player does NOT have ${p.effect}`,
  },
  {
    id: 'is_defending', label: 'is blocking / guarding', negatable: true,
    params: [],
    lua: () => 'sam_is_defending(player)',
    phrase: () => 'the player is blocking',
    phraseNeg: () => 'the player is NOT blocking',
  },
  {
    id: 'equipped_is', label: 'has a specific item equipped', negatable: true,
    params: [
      { name: 'slot', type: 'select', values: SLOTS, default: 'SHIELD' },
      { name: 'item', type: 'text', default: 'IRON_SHIELD', label: 'item name or ns:item' },
    ],
    lua: (p) => `sam_get_equipped_item_id(player, ${q(p.slot)}) == sam_item_id(${q(p.item)})`,
    phrase: (p) => `${p.slot} is ${p.item}`,
    phraseNeg: (p) => `${p.slot} is NOT ${p.item}`,
  },
  {
    id: 'slot_empty', label: 'has a slot empty', negatable: true,
    params: [{ name: 'slot', type: 'select', values: SLOTS, default: 'WEAPON' }],
    lua: (p) => `sam_get_equipped_item_id(player, ${q(p.slot)}) == nil`,
    phrase: (p) => `${p.slot} is empty`,
    phraseNeg: (p) => `${p.slot} is NOT empty`,
  },
  {
    id: 'stat_cmp', label: 'stat compares to a value', negatable: false,
    params: [
      { name: 'stat', type: 'select', values: STATS, default: 'HP' },
      { name: 'op', type: 'select', values: ['<', '<=', '==', '>=', '>'], default: '<' },
      { name: 'value', type: 'number', default: 10 },
    ],
    lua: (p) => `sam_get_stat(player, ${q(p.stat)}) ${p.op} ${Number(p.value) || 0}`,
    phrase: (p) => `${p.stat} ${p.op} ${Number(p.value) || 0}`,
  },
  {
    id: 'class_is', label: 'is playing a class', negatable: true,
    params: [{ name: 'name', type: 'text', default: 'My Class', label: 'class name (exactly as in its JSON)' }],
    lua: (p) => `sam_get_class(player) == ${q(p.name)}`,
    phrase: (p) => `the player is a ${p.name}`,
    phraseNeg: (p) => `the player is NOT a ${p.name}`,
    note: 'Class scripts get every event for every player, so this is how an ability stays yours.',
  },
  {
    id: 'floor_cmp', label: 'dungeon floor compares to', negatable: false,
    params: [
      { name: 'op', type: 'select', values: ['<', '<=', '==', '>=', '>'], default: '>=' },
      { name: 'value', type: 'number', default: 5 },
    ],
    lua: (p) => `sam_get_floor() ${p.op} ${Number(p.value) || 0}`,
    phrase: (p) => `the floor is ${p.op} ${Number(p.value) || 0}`,
  },
  {
    id: 'chance', label: 'random chance', negatable: false,
    // NOT offered as an "until" condition: an until-poll runs 5×/sec, so "until a 25%
    // roll" comes up in a fraction of a second — the user means "25% per second", which
    // is a different thing. It is a fine IF-guard (rolled once when the ability fires).
    pollable: false,
    params: [{ name: 'percent', type: 'number', default: 25, min: 1, max: 100, label: '% of the time' }],
    lua: (p) => `math.random(100) <= ${Number(p.percent) || 0}`,
    phrase: (p) => `a ${Number(p.percent) || 0}% roll comes up`,
  },
  {
    id: 'move_speed_cmp', label: 'move speed compares to', negatable: false,
    params: [
      { name: 'op', type: 'select', values: ['<', '<=', '==', '>=', '>'], default: '>' },
      { name: 'value', type: 'number', default: 1, label: 'multiplier (1 = normal)' },
    ],
    lua: (p) => `sam_get_move_speed(player) ${p.op} ${Number(p.value) || 0}`,
    phrase: (p) => `move speed is ${p.op} ${Number(p.value) || 0}`,
  },
  {
    id: 'time_played_cmp', label: 'time in this run compares to', negatable: false,
    params: [
      { name: 'op', type: 'select', values: ['<', '<=', '>=', '>'], default: '>' },
      { name: 'seconds', type: 'number', default: 60, label: 'seconds' },
    ],
    // sam_get_time_played returns TICKS, not seconds — convert so the input reads naturally.
    lua: (p) => `sam_get_time_played() ${p.op} ${Math.round((Number(p.seconds) || 0) * TICKS_PER_SECOND)}`,
    phrase: (p) => `time in the run is ${p.op} ${Number(p.seconds) || 0}s`,
    note: 'Measured from the start of the run. 50 ticks = 1 second.',
  },
];

/**
 * Actions — what happens. `needs` names an event field the action requires, so an
 * action that damages a target is only offered on triggers that actually carry one.
 * That is exactly the bug class where a script reads event.killer_uid on an event
 * that has no killer_uid and silently does nothing forever.
 */
export const ACTIONS = [
  {
    id: 'grant_item', label: 'give an item', category: 'Rewards',
    params: [{ name: 'item', type: 'text', default: 'FOOD_BREAD', label: 'item name or ns:item' }],
    lua: (p) => `sam_grant_item(player, ${q(p.item)})`,
  },
  {
    id: 'grant_gold', label: 'give gold', category: 'Rewards',
    params: [{ name: 'amount', type: 'number', default: 10 }],
    lua: (p) => `sam_grant_gold(player, ${Number(p.amount) || 0})`,
  },
  {
    id: 'heal', label: 'change HP (heal or hurt)', category: 'Player',
    params: [
      { name: 'amount', type: 'number', default: 5, label: 'amount (negative to hurt)' },
      { name: 'mode', type: 'select', values: MODES, default: 'flat' },
      { name: 'can_kill', type: 'select', values: KILL_MODES, default: 'can kill' },
    ],
    lua: (p) => hpChange('HP', p),
    note: 'Percent of CURRENT halves and halves (it decays); percent of MAX is a steady bite. '
        + '“cannot kill” floors you at 1 HP — the same thing the engine\'s own buddhamode does.',
  },
  {
    id: 'restore_mp', label: 'change mana', category: 'Player',
    params: [
      { name: 'amount', type: 'number', default: 5, label: 'amount (negative to drain)' },
      { name: 'mode', type: 'select', values: MODES, default: 'flat' },
    ],
    lua: (p) => hpChange('MP', { ...p, can_kill: 'can kill' }), // MP can't kill you
  },
  {
    id: 'set_stat', label: 'change a stat', category: 'Player',
    params: [
      { name: 'stat', type: 'select', values: STATS, default: 'STR' },
      { name: 'amount', type: 'number', default: 1, label: 'add this much' },
      ...durationParams(),
    ],
    needsSeq: (p) => p.duration && p.duration !== 'permanently',
    lua: (p) => {
      const stat = q(p.stat);
      const amt = Number(p.amount) || 0;
      const read = `sam_get_stat(player, ${stat})`;
      return temporal({
        p,
        read,
        write: (v) => `sam_set_stat(player, ${stat}, ${v})`,
        apply: (base) => `sam_set_stat(player, ${stat}, ${base} + (${amt}))`,
        idPrefix: `tmp_${p.stat}`,
      });
    },
    note: 'Each application reverts on its own timer, so overlapping buffs stack and unstack '
        + 'correctly. It hands back what the game ACTUALLY gave you: attributes stop at 248, '
        + 'so a +100 on a strong character adds less than 100 — and gives back less than 100.',
    // The engine writes HP/MP/GOLD/EXP/HUNGER on its own. A timed revert of one fights it:
    // it subtracts what it added against a number that has since moved for other reasons.
    warn: (p) => (p.duration && p.duration !== 'permanently' && ENGINE_WRITTEN_STATS.includes(p.stat)
      ? `${p.stat} changes on its own as you play (damage, regen, spending). A timed change `
        + `to it will fight the game over the same number — use a plain "change HP / change `
        + `mana" for those, and keep timed changes to STR/DEX/CON/INT/PER/CHR.`
      : null),
  },
  {
    id: 'apply_effect', label: 'apply a status effect', category: 'Player',
    params: [
      { name: 'effect', type: 'select', values: EFFECTS, default: 'FAST' },
      { name: 'ticks', type: 'number', default: 100, label: 'ticks (50 = 1 second)' },
    ],
    lua: (p) => `sam_apply_effect(player, ${q(p.effect)}, ${Number(p.ticks) || 0})`,
  },
  {
    id: 'remove_effect', label: 'remove a status effect', category: 'Player',
    params: [{ name: 'effect', type: 'select', values: EFFECTS, default: 'POISONED' }],
    lua: (p) => `sam_remove_effect(player, ${q(p.effect)})`,
  },
  {
    id: 'cast_spell', label: 'cast a spell', category: 'Magic',
    params: [{ name: 'spell', type: 'select', values: SPELLS, default: 'SPELL_FORCEBOLT' }],
    lua: (p) => `sam_cast_spell(player, ${q(p.spell)})`,
    note: 'Casts free and ignores the blocking guard, so it works while your shield is up.',
  },
  {
    id: 'move_speed', label: 'set move speed', category: 'Player',
    params: [
      { name: 'mult', type: 'number', default: 1.5, min: 0.1, max: 3, label: 'multiplier (0.1-3)' },
      ...durationParams(),
    ],
    lua: (p) => {
      const mult = Number(p.mult) || 1;
      return temporal({
        p,
        absolute: true,
        from: mult,
        write: (v) => `sam_set_move_speed(player, ${v})`,
        apply: () => `sam_set_move_speed(player, ${mult})`,
      });
    },
    note: 'Move speed is one slider per player, not a stack — so unlike a stat, re-applying '
        + 'REFRESHES the timer instead of queuing a second revert, and two speed abilities '
        + 'will override each other (the most recent wins). 3 is the engine cap; it clamps.',
  },
  {
    id: 'damage_target', label: 'damage the target', category: 'Combat',
    needs: 'target_uid',
    params: [{ name: 'amount', type: 'number', default: 5 }],
    lua: (p) => `sam_deal_damage(event.target_uid, ${Number(p.amount) || 0})`,
  },
  {
    id: 'damage_monster', label: 'damage the monster', category: 'Combat',
    needs: 'monster_uid',
    params: [{ name: 'amount', type: 'number', default: 5 }],
    lua: (p) => `sam_deal_damage(event.monster_uid, ${Number(p.amount) || 0})`,
  },
  {
    id: 'kill_target', label: 'kill the target outright', category: 'Combat',
    needs: 'target_uid',
    params: [],
    lua: () => 'sam_kill_monster(event.target_uid)',
  },
  {
    id: 'block_damage', label: 'cancel the incoming damage', category: 'Combat',
    needs: 'damage',
    onlyOn: ['on_before_damage'],
    params: [{ name: 'to', type: 'number', default: 0, label: 'reduce the hit to' }],
    lua: (p) => `sam_modify_damage(player, ${Number(p.to) || 0})`,
    note: 'Only on_before_damage can be changed — it is the one cancellable hook.',
  },
  {
    id: 'message', label: 'show a message', category: 'Feedback',
    params: [{ name: 'text', type: 'text', default: 'Something happened!' }],
    lua: (p) => `sam_message(player, ${q(p.text)})`,
  },
  {
    id: 'play_sound', label: 'play a sound', category: 'Feedback',
    params: [{ name: 'id', type: 'number', default: 28, label: 'sound id' }],
    lua: (p) => `sam_play_sound(player, ${Number(p.id) || 0})`,
  },
];

/*
 * User-defined bricks ("lego packs") are merged in at runtime. They live in these arrays
 * alongside the built-ins so every lookup, dropdown and the generator treat them
 * identically — a custom brick is a first-class block, not a special case.
 *
 * Custom blocks carry no `needs`, so they're offered on every trigger. That's the one
 * guarantee they don't get: the builder can't know which event fields a hand-written
 * template reads. Their templates ARE linted against the real API before they can be
 * saved (see lib/customBlocks.js), so they can't call a function that doesn't exist.
 */
let CUSTOM_CONDITIONS = [];
let CUSTOM_ACTIONS = [];

/** Replace the registered custom blocks (called whenever the user's pack changes). */
export function registerCustom(entries) {
  CUSTOM_CONDITIONS = entries.filter((e) => e.kind === 'condition').map((e) => e.entry);
  CUSTOM_ACTIONS = entries.filter((e) => e.kind === 'action').map((e) => e.entry);
}

export const allConditions = () => [...CONDITIONS, ...CUSTOM_CONDITIONS];

/**
 * Conditions that are safe to use as a "lasts: until…" — i.e. safe to POLL on a timer.
 *
 * Two exclusions, both by DERIVATION rather than a hand-kept list so they can't rot:
 *   - `pollable: false` marks a condition whose truth is momentary. "until a 25% roll"
 *     polled 5×/sec fires almost instantly, which is never what the user means.
 *   - A condition whose Lua mentions `event`. The poll runs inside a closure that captured
 *     the on_event `event` as an upvalue, so by the time it fires that event is long stale.
 *     No built-in reads event; a custom brick can, so we render it and check.
 */
export function untilCandidates() {
  return allConditions().filter((c) => {
    if (c.pollable === false) return false;
    let sample;
    try { sample = c.lua(defaultsFor(c)); } catch { return true; } // can't render -> don't exclude
    return !/\bevent\b/.test(String(sample));
  });
}

function defaultsFor(def) {
  const out = {};
  for (const p of def?.params || []) out[p.name] = p.default;
  return out;
}

/**
 * Stats the engine writes on its own every tick — HP/MP from regen and damage, GOLD and
 * EXP as you play, HUNGER as it drains. A timer that "reverts" one of these is fighting the
 * engine over the same number: by the time it fires, `read - applied` subtracts against a
 * value that has moved for entirely unrelated reasons. The attributes (STR…CHR) and the
 * MAX/LEVEL ceilings don't drift, so a timed change to them is meaningful. This gates the
 * WARNING, not the feature — someone who knows what they're doing can still ship it.
 */
export const ENGINE_WRITTEN_STATS = ['HP', 'MP', 'GOLD', 'EXP', 'HUNGER'];

/** Actions valid for a trigger: those needing an event field it doesn't carry are hidden. */
export function actionsFor(trigger) {
  const fields = triggerFields(trigger);
  return [...ACTIONS, ...CUSTOM_ACTIONS].filter((a) => {
    if (a.onlyOn && !a.onlyOn.includes(trigger?.id)) return false;
    if (a.needs && !fields.includes(a.needs)) return false;
    return true;
  });
}

export const findTrigger = (id) => TRIGGERS.find((t) => t.id === id);
export const findCondition = (id) => allConditions().find((c) => c.id === id);
export const findAction = (id) => [...ACTIONS, ...CUSTOM_ACTIONS].find((a) => a.id === id);
