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
import { SAM_EVENTS, PLAYER_STATS, EFFECTS as API_EFFECTS } from '@/data/samApi.js';

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

/**
 * Spells that are actually castable by a player — the engine's spell_t globals.
 * Barony's items.json lists ~227 spell_* names, but most are monster-only and would
 * silently do nothing here, so this is the intersection that really works.
 */
export const SPELLS = [
  'SPELL_FORCEBOLT', 'SPELL_MAGICMISSILE', 'SPELL_FIREBALL', 'SPELL_LIGHTNING', 'SPELL_COLD',
  'SPELL_BLEED', 'SPELL_POISON', 'SPELL_STONEBLOOD', 'SPELL_STRIKE', 'SPELL_GHOST_BOLT',
  'SPELL_HEALING', 'SPELL_EXTRAHEALING', 'SPELL_CUREAILMENT', 'SPELL_REMOVECURSE',
  'SPELL_SLEEP', 'SPELL_CONFUSE', 'SPELL_SLOW', 'SPELL_SPEED', 'SPELL_FEAR', 'SPELL_WEAKNESS',
  'SPELL_CHARM', 'SPELL_DOMINATE', 'SPELL_INVISIBILITY', 'SPELL_LEVITATION', 'SPELL_FLUTTER',
  'SPELL_DASH', 'SPELL_TELEPORTATION', 'SPELL_DIG', 'SPELL_OPENING', 'SPELL_LOCKING',
  'SPELL_IDENTIFY', 'SPELL_MAGICMAPPING', 'SPELL_LIGHT', 'SPELL_SALVAGE', 'SPELL_SUMMON',
  'SPELL_SLIME_ACID', 'SPELL_SLIME_FIRE', 'SPELL_SLIME_METAL', 'SPELL_SLIME_TAR', 'SPELL_SLIME_WATER',
];

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
  },
  {
    id: 'is_defending', label: 'is blocking / guarding', negatable: true,
    params: [],
    lua: () => 'sam_is_defending(player)',
  },
  {
    id: 'equipped_is', label: 'has a specific item equipped', negatable: true,
    params: [
      { name: 'slot', type: 'select', values: SLOTS, default: 'SHIELD' },
      { name: 'item', type: 'text', default: 'IRON_SHIELD', label: 'item name or ns:item' },
    ],
    lua: (p) => `sam_get_equipped_item_id(player, ${q(p.slot)}) == sam_item_id(${q(p.item)})`,
  },
  {
    id: 'slot_empty', label: 'has a slot empty', negatable: true,
    params: [{ name: 'slot', type: 'select', values: SLOTS, default: 'WEAPON' }],
    lua: (p) => `sam_get_equipped_item_id(player, ${q(p.slot)}) == nil`,
  },
  {
    id: 'stat_cmp', label: 'stat compares to a value', negatable: false,
    params: [
      { name: 'stat', type: 'select', values: STATS, default: 'HP' },
      { name: 'op', type: 'select', values: ['<', '<=', '==', '>=', '>'], default: '<' },
      { name: 'value', type: 'number', default: 10 },
    ],
    lua: (p) => `sam_get_stat(player, ${q(p.stat)}) ${p.op} ${Number(p.value) || 0}`,
  },
  {
    id: 'class_is', label: 'is playing a class', negatable: true,
    params: [{ name: 'name', type: 'text', default: 'My Class', label: 'class name (exactly as in its JSON)' }],
    lua: (p) => `sam_get_class(player) == ${q(p.name)}`,
    note: 'Class scripts get every event for every player, so this is how an ability stays yours.',
  },
  {
    id: 'floor_cmp', label: 'dungeon floor compares to', negatable: false,
    params: [
      { name: 'op', type: 'select', values: ['<', '<=', '==', '>=', '>'], default: '>=' },
      { name: 'value', type: 'number', default: 5 },
    ],
    lua: (p) => `sam_get_floor() ${p.op} ${Number(p.value) || 0}`,
  },
  {
    id: 'chance', label: 'random chance', negatable: false,
    params: [{ name: 'percent', type: 'number', default: 25, min: 1, max: 100, label: '% of the time' }],
    lua: (p) => `math.random(100) <= ${Number(p.percent) || 0}`,
  },
  {
    id: 'move_speed_cmp', label: 'move speed compares to', negatable: false,
    params: [
      { name: 'op', type: 'select', values: ['<', '<=', '==', '>=', '>'], default: '>' },
      { name: 'value', type: 'number', default: 1, label: 'multiplier (1 = normal)' },
    ],
    lua: (p) => `sam_get_move_speed(player) ${p.op} ${Number(p.value) || 0}`,
  },
  {
    id: 'time_played_cmp', label: 'time in this run compares to', negatable: false,
    params: [
      { name: 'op', type: 'select', values: ['<', '<=', '>=', '>'], default: '>' },
      { name: 'seconds', type: 'number', default: 60, label: 'seconds' },
    ],
    // sam_get_time_played returns TICKS, not seconds — convert so the input reads naturally.
    lua: (p) => `sam_get_time_played() ${p.op} ${Math.round((Number(p.seconds) || 0) * TICKS_PER_SECOND)}`,
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
    ],
    lua: (p) => `sam_set_stat(player, ${q(p.stat)}, sam_get_stat(player, ${q(p.stat)}) + (${Number(p.amount) || 0}))`,
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
    params: [{ name: 'mult', type: 'number', default: 1.5, min: 0.1, max: 3, label: 'multiplier (0.1-3)' }],
    lua: (p) => `sam_set_move_speed(player, ${Number(p.mult) || 1})`,
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
