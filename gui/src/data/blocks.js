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
import { SAM_EVENTS } from '@/data/samApi.js';

/** Bare effect names samEffectNameToId() accepts (sam_lua_runtime.cpp). Case-insensitive. */
export const EFFECTS = [
  'FAST', 'SLOW', 'POISONED', 'BLEEDING', 'CONFUSED', 'BLIND', 'ASLEEP',
  'PARALYZED', 'INVISIBLE', 'LEVITATING', 'DRUNK', 'GREASY', 'VOMITING', 'WEBBED',
];

/** Slot names samEquippedSlot() understands. ARMOR==BREASTPLATE, BOOTS==SHOES. */
export const SLOTS = [
  'WEAPON', 'SHIELD', 'HELMET', 'ARMOR', 'GLOVES', 'BOOTS', 'RING', 'AMULET', 'CLOAK', 'MASK',
];

/** Attributes sam_get_stat / sam_set_stat accept (mirrors the manifest's value list). */
export const STATS = ['STR', 'DEX', 'CON', 'INT', 'PER', 'CHR', 'HP', 'MAXHP', 'MP', 'MAXMP', 'GOLD', 'LEVEL', 'EXP'];

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
    id: 'heal', label: 'heal (or hurt) the player', category: 'Player',
    params: [{ name: 'amount', type: 'number', default: 5, label: 'HP (negative to hurt)' }],
    lua: (p) => `sam_set_stat(player, "HP", sam_get_stat(player, "HP") + (${Number(p.amount) || 0}))`,
    note: 'sam_set_stat clamps to MAXHP, so this can\'t overheal.',
  },
  {
    id: 'restore_mp', label: 'restore (or drain) mana', category: 'Player',
    params: [{ name: 'amount', type: 'number', default: 5, label: 'MP (negative to drain)' }],
    lua: (p) => `sam_set_stat(player, "MP", sam_get_stat(player, "MP") + (${Number(p.amount) || 0}))`,
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

/** Actions valid for a trigger: those needing an event field it doesn't carry are hidden. */
export function actionsFor(trigger) {
  const fields = triggerFields(trigger);
  return ACTIONS.filter((a) => {
    if (a.onlyOn && !a.onlyOn.includes(trigger?.id)) return false;
    if (a.needs && !fields.includes(a.needs)) return false;
    return true;
  });
}

export const findTrigger = (id) => TRIGGERS.find((t) => t.id === id);
export const findCondition = (id) => CONDITIONS.find((c) => c.id === id);
export const findAction = (id) => ACTIONS.find((a) => a.id === id);
