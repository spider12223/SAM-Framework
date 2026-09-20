/*
 * Runs the block builder's REAL generateLua() output through a REAL Lua 5.4.7 interpreter.
 *
 * Why this exists: every builder bug so far has been a script that parses fine and does the
 * wrong thing quietly. Reading generated Lua does not catch those. Executing it does — the
 * "for a while" revert destroyed up to 98 STR permanently for a year and looked correct on
 * the page, because the bug only appears when sam_set_stat's clamp truncates the buff and
 * the revert subtracts the un-truncated amount.
 *
 * Requires: a lua interpreter on PATH or at $SAM_LUA. Build one from the framework's own
 * vendored copy so the version matches what the game runs:
 *   cl /O2 /MD /I<src> <src>\*.c (minus luac.c) /Fe:lua.exe   [framework/lua54, Lua 5.4.7]
 * Skips (does not fail) when no interpreter is present, so it never blocks a GUI-only build.
 */
import { execFileSync } from 'node:child_process';
import { writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { generateLua } from '../src/lib/codegen.js';
import { ACTIONS, CONDITIONS } from '../src/data/blocks.js';
import { SNIPPETS } from '../src/data/snippets.js';
import { untilCandidates, findAction, ENGINE_WRITTEN_STATS, conditionsFor, findTrigger, findCondition } from '../src/data/blocks.js';
import { TRIGGERS, EVERY_SECONDS, triggerHasPlayer, triggerPlayerMayBeMissing, PLAYER_MAY_BE_MISSING, registerCustom } from '../src/data/blocks.js';
import { toCatalogEntry } from '../src/lib/customBlocks.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const TMP = join(HERE, '.tmp');
mkdirSync(TMP, { recursive: true });

function findLua() {
  // tools/lua/lua.exe is the 5.4.7 interpreter built from framework/lua54, checked in so
  // `npm test` actually RUNS instead of printing SKIP and exiting 0 -- a green skip reads
  // exactly like a pass in CI and in a terminal you glanced at.
  const vendored = fileURLToPath(new URL('../../tools/lua/lua' + (process.platform === 'win32' ? '.exe' : ''), import.meta.url));
  const candidates = [process.env.SAM_LUA, vendored, 'lua', 'lua5.4', 'lua54'].filter(Boolean);
  for (const c of candidates) {
    try { execFileSync(c, ['-v'], { stdio: 'pipe' }); return c; } catch { /* keep looking */ }
  }
  return null;
}
const LUA = findLua();

let pass = 0, fail = 0;
const failures = [];

/**
 * Build a script from `rules`, run `driver` against it in the simulator, and compare the
 * reported state to `expect`. The driver is Lua, so a test reads like the thing it tests.
 */
function check(name, rules, driver, expect) {
  const lua = generateLua({ rules });
  const script = join(TMP, 'gen.lua');
  writeFileSync(script, lua);
  const harness = `
    local S = dofile(${JSON.stringify(join(HERE, 'samsim.lua').replace(/\\/g, '/'))})
    S.reset(${expect.start || '{}'})
    local f = io.open(${JSON.stringify(script.replace(/\\/g, '/'))}, 'r')
    local src = f:read('a'); f:close()
    local ok, err = S.load(src)
    if not ok then print('LOADFAIL\\t' .. err) os.exit(0) end
    ${driver}
    local st = S.state()
    print('OK\\t' .. string.format('%s|%s|%s|%d|%s',
      tostring(st.stats.STR), tostring(st.stats.HP), tostring(st.move_speed),
      #S.timers(), #S.errors() > 0 and S.errors()[1] or '-'))
  `;
  const hpath = join(TMP, 'h.lua');
  writeFileSync(hpath, harness);
  let out;
  try { out = execFileSync(LUA, [hpath], { encoding: 'utf8' }).trim(); }
  catch (e) { out = 'CRASH\t' + (e.stdout || e.message); }

  const [tag, payload] = out.split('\t');
  if (tag !== 'OK') { fail++; failures.push({ name, why: out, lua }); return; }
  const [STR, HP, speed, timers, err] = payload.split('|');
  const got = { STR, HP, speed, timers: Number(timers), err };
  const bad = Object.entries(expect.want).filter(([k, v]) => String(got[k]) !== String(v));
  if (bad.length) {
    fail++;
    failures.push({ name, why: bad.map(([k, v]) => `${k}: want ${v}, got ${got[k]}`).join('; '), lua });
  } else { pass++; }
}

const rule = (trigger, actions, conditions = []) => ({
  key: 't', trigger: { id: trigger, params: {} }, conditions, actions,
});
const act = (id, params) => ({ id, params });

// ---------------------------------------------------------------------------------
// Every generated script must at minimum PARSE. A syntax error loads nothing at all.
// ---------------------------------------------------------------------------------
check('permanent stat change parses and applies',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })])],
  `S.fire('player.on_hit', {player=0})`,
  { start: '{STR=10}', want: { STR: 15 } });

// ---------------------------------------------------------------------------------
// THE CLAMP BUG. sam_set_stat clamps attributes to 248 (stat.hpp:550). A revert by the
// nominal amount subtracts more than was ever added.
// ---------------------------------------------------------------------------------
check('temporary stat returns EXACTLY to baseline',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'for a while', seconds: 3 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(4)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

check('temporary stat is lossless even when the +100 CLAMPS at 248',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 100, duration: 'for a while', seconds: 3 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(4)`,
  { start: '{STR=200}', want: { STR: 200, timers: 0 } });

check('overlapping temporary stats all revert (20 in one tick)',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'for a while', seconds: 3 })])],
  `for _=1,20 do S.fire('player.on_hit', {player=0}) end S.seconds(4)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

// ---------------------------------------------------------------------------------
// Move speed: an ABSOLUTE setter. Re-applying must REFRESH, not stack a second revert.
// ---------------------------------------------------------------------------------
check('temporary move speed reverts to 1.0',
  [rule('player.on_hit', [act('move_speed', { mult: 3, duration: 'for a while', seconds: 3 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(4)`,
  { want: { speed: '1.0', timers: 0 } });

check('re-applying move speed REFRESHES the duration',
  [rule('player.on_hit', [act('move_speed', { mult: 3, duration: 'for a while', seconds: 3 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(2)
   S.fire('player.on_hit', {player=0}) S.seconds(2)`,
  { want: { speed: '3.0' } });   // still fast: the second hit pushed the revert out

check('move speed does eventually expire after a refresh',
  [rule('player.on_hit', [act('move_speed', { mult: 3, duration: 'for a while', seconds: 3 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(2)
   S.fire('player.on_hit', {player=0}) S.seconds(4)`,
  { want: { speed: '1.0', timers: 0 } });

// ---------------------------------------------------------------------------------
// solidius's decay: "100 strength for 100 seconds, every second 1 strength down".
// ---------------------------------------------------------------------------------
check('fading stat is at full strength immediately',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 100, duration: 'fading away', seconds: 100 })])],
  `S.fire('player.on_hit', {player=0})`,
  { start: '{STR=10}', want: { STR: 110 } });

check('fading stat is halfway back at the halfway point',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 100, duration: 'fading away', seconds: 100 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(50)`,
  { start: '{STR=10}', want: { STR: 60 } });

check('fading stat lands exactly on baseline and stops',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 100, duration: 'fading away', seconds: 100 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(120)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

check('fading stat never undershoots baseline, even long after',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 100, duration: 'fading away', seconds: 100 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(600)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

check('fading stat is lossless when the buff CLAMPS on the way up',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 100, duration: 'fading away', seconds: 100 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(600)`,
  { start: '{STR=200}', want: { STR: 200, timers: 0 } });

check('overlapping fades all give back exactly what they took',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 50, duration: 'fading away', seconds: 20 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(5)
   S.fire('player.on_hit', {player=0}) S.seconds(5)
   S.fire('player.on_hit', {player=0}) S.seconds(120)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

check('fading move speed glides back to 1.0',
  [rule('player.on_hit', [act('move_speed', { mult: 3, duration: 'fading away', seconds: 10 })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(12)`,
  { want: { speed: '1.0', timers: 0 } });

// ---------------------------------------------------------------------------------
// solidius: "is it possible to set condition for REMOVING an action?"
// ---------------------------------------------------------------------------------
check('until-condition holds the buff while the condition is false',
  [rule('player.on_hit', [act('set_stat', {
    stat: 'STR', amount: 50, duration: 'until', until_id: 'stat_cmp',
    until_params: { stat: 'HP', op: '<', value: 10 },
  })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(10)`,
  { start: '{STR=10, HP=100}', want: { STR: 60 } });

check('until-condition reverts once the condition becomes true',
  [rule('player.on_hit', [act('set_stat', {
    stat: 'STR', amount: 50, duration: 'until', until_id: 'stat_cmp',
    until_params: { stat: 'HP', op: '<', value: 10 },
  })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(2)
   sam_set_stat(0, 'HP', 5) S.seconds(2)`,
  { start: '{STR=10, HP=100}', want: { STR: 10, timers: 0 } });

check('until-condition does not leave a poller running after it fires',
  [rule('player.on_hit', [act('set_stat', {
    stat: 'STR', amount: 50, duration: 'until', until_id: 'has_effect',
    until_params: { effect: 'POISONED' },
  })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(1)
   sam_apply_effect(0, 'POISONED', 100) S.seconds(2)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

// ---------------------------------------------------------------------------------
// Multi-ability + tick, the shape that produced "<eof> expected near 'end'" by hand.
// ---------------------------------------------------------------------------------
check('two abilities on different triggers both fire',
  [
    rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })]),
    rule('every_seconds', [act('move_speed', { mult: 2, duration: 'permanently' })]),
  ],
  `S.fire('player.on_hit', {player=0}) S.seconds(6)`,
  { start: '{STR=10}', want: { STR: 15, speed: '2.0' } });

check('two abilities sharing a trigger merge into one branch',
  [
    rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })]),
    rule('player.on_hit', [act('move_speed', { mult: 2, duration: 'permanently' })]),
  ],
  `S.fire('player.on_hit', {player=0})`,
  { start: '{STR=10}', want: { STR: 15, speed: '2.0' } });

check('a temporary and a fading ability coexist without id collision',
  [
    rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'for a while', seconds: 3 })]),
    rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 10, duration: 'fading away', seconds: 5 })]),
  ],
  `S.fire('player.on_hit', {player=0}) S.seconds(1)
   S.fire('player.on_hit', {player=0}) S.seconds(30)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

// ---------------------------------------------------------------------------------
// Conditions must not be inverted by `not a == b` precedence.
// ---------------------------------------------------------------------------------
check('a negated condition gates correctly (effect absent -> fires)',
  [rule('player.on_hit',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'has_effect', params: { effect: 'POISONED' }, negate: true }])],
  `S.fire('player.on_hit', {player=0})`,
  { start: '{STR=10}', want: { STR: 15 } });

check('a negated condition gates correctly (effect present -> blocked)',
  [rule('player.on_hit',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'has_effect', params: { effect: 'POISONED' }, negate: true }])],
  `sam_apply_effect(0, 'POISONED', 100) S.fire('player.on_hit', {player=0})`,
  { start: '{STR=10}', want: { STR: 10 } });

// ---------------------------------------------------------------------------------
// solidius's stack flag: "one at a time" vs "each stacks separately".
// ---------------------------------------------------------------------------------
check('stacking=stack: two quick hits stack, then both unwind',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'for a while', seconds: 3, stacking: 'stack' })])],
  `S.fire('player.on_hit', {player=0})
   S.fire('player.on_hit', {player=0})   -- +10 total while both are up
   local mid = S.state().stats.STR
   S.seconds(4)
   assert(mid == 20, 'expected +10 stacked, got ' .. (mid-10))`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

check('stacking=one: while it is running, a second hit adds nothing',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'for a while', seconds: 3, stacking: 'one' })])],
  `S.fire('player.on_hit', {player=0})
   S.seconds(1)
   S.fire('player.on_hit', {player=0})   -- ignored: one already active
   assert(S.state().stats.STR == 15, 'expected +5 only, got ' .. (S.state().stats.STR-10))
   S.seconds(4)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

check('stacking=one: the lock releases so a LATER hit works again',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'for a while', seconds: 3, stacking: 'one' })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(4)   -- runs and ends
   S.fire('player.on_hit', {player=0})                -- fires fresh
   assert(S.state().stats.STR == 15, 'expected re-fire to apply, got ' .. (S.state().stats.STR-10))
   S.seconds(4)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

check('stacking=one on a fade: mid-fade re-fire does not double it',
  [rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 20, duration: 'fading away', seconds: 20, stacking: 'one' })])],
  `S.fire('player.on_hit', {player=0}) S.seconds(5)
   S.fire('player.on_hit', {player=0})   -- ignored while fading
   S.seconds(30)`,
  { start: '{STR=10}', want: { STR: 10, timers: 0 } });

check('two DIFFERENT one-at-a-time abilities do not share a lock',
  [
    rule('player.on_hit', [act('set_stat', { stat: 'STR', amount: 5, duration: 'for a while', seconds: 3, stacking: 'one' })]),
    rule('player.on_hit', [act('set_stat', { stat: 'DEX', amount: 5, duration: 'for a while', seconds: 3, stacking: 'one' })]),
  ],
  `S.fire('player.on_hit', {player=0})
   assert(S.state().stats.STR == 15 and S.state().stats.DEX == 15, 'both should fire independently')
   S.seconds(4)`,
  { start: '{STR=10, DEX=10}', want: { STR: 10, timers: 0 } });

// ---------------------------------------------------------------------------------
// v1.2.8: additive move speed stacks (solidius), real level-up (Pizza).
// ---------------------------------------------------------------------------------
check('add_move_speed stacks onto the current multiplier',
  [rule('player.on_hit', [act('add_move_speed', { delta: 0.1 })])],
  `S.fire('player.on_hit', {player=0}) S.fire('player.on_hit', {player=0})`,
  { want: { speed: '1.2' } });   // 1.0 + 0.1 + 0.1, not a SET to 0.1

check('add_move_speed clamps at the 3.0 cap',
  [rule('player.on_hit', [act('add_move_speed', { delta: 5 })])],
  `S.fire('player.on_hit', {player=0})`,
  { want: { speed: '3.0' } });

check('level_up advances the level count times without a timer',
  [rule('player.on_hit', [act('level_up', { count: 2 })])],
  `S.fire('player.on_hit', {player=0})
   assert(S.state().stats.LVL == 3, 'two levels: 1 -> 3')`,
  { start: '{LVL=1}', want: { timers: 0 } });

// ---------------------------------------------------------------------------------
// v1.2.9: effect duration/strength conditions + apply_effect strength (solidius's
// "downsides that scale by how much is left" / tiered effects).
// ---------------------------------------------------------------------------------
check('effect_duration_cmp blocks when the effect is absent (0s left)',
  [rule('player.on_hit',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'effect_duration_cmp', params: { effect: 'SLOW', op: '>', seconds: 2 } }])],
  `S.fire('player.on_hit', {player=0})`,
  { start: '{STR=10}', want: { STR: 10 } });   // no SLOW -> 0 ticks, not > 100

check('effect_duration_cmp fires when enough time remains',
  [rule('player.on_hit',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'effect_duration_cmp', params: { effect: 'SLOW', op: '>', seconds: 2 } }])],
  `sam_apply_effect(0, 'SLOW', 200) S.fire('player.on_hit', {player=0})`, // 200 ticks > 100
  { start: '{STR=10}', want: { STR: 15 } });

check('effect_strength_cmp reads the tier set by apply_effect strength',
  [rule('player.on_hit',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'effect_strength_cmp', params: { effect: 'POISONED', op: '>=', value: 3 } }])],
  `sam_apply_effect(0, 'POISONED', 100, 3) S.fire('player.on_hit', {player=0})`,
  { start: '{STR=10}', want: { STR: 15 } });

// stat_cmp "% of max" (solidius): compare HP/MP to a fraction of MAX, not a flat number.
check('stat_cmp % of max fires when HP is below 10% of max',
  [rule('player.on_hit',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'stat_cmp', params: { stat: 'HP', op: '<', value: 10, unit: '% of max' } }])],
  `S.fire('player.on_hit', {player=0})`,
  { start: '{MAXHP=100, HP=5, STR=10}', want: { STR: 15 } });   // 5 < 100*0.1

check('stat_cmp % of max does NOT fire at half HP',
  [rule('player.on_hit',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'stat_cmp', params: { stat: 'HP', op: '<', value: 10, unit: '% of max' } }])],
  `S.fire('player.on_hit', {player=0})`,
  { start: '{MAXHP=100, HP=50, STR=10}', want: { STR: 10 } });   // 50 < 10 is false

// Event-field conditions (solidius): react to THIS pickup's gold / the identified item.
check('gold_amount_cmp fires on a big pickup (reads event.amount, not the total)',
  [rule('player.on_gold_collected',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'gold_amount_cmp', params: { op: '>=', value: 10 } }])],
  `S.fire('player.on_gold_collected', {player=0, amount=25})`,
  { start: '{STR=10}', want: { STR: 15 } });

check('gold_amount_cmp does NOT fire on a small pickup',
  [rule('player.on_gold_collected',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'gold_amount_cmp', params: { op: '>=', value: 10 } }])],
  `S.fire('player.on_gold_collected', {player=0, amount=3})`,
  { start: '{STR=10}', want: { STR: 10 } });

check('event_item_is matches the identified item',
  [rule('player.on_item_identified',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'event_item_is', params: { item: 'GEM_DIAMOND' } }])],
  `S.fire('player.on_item_identified', {player=0, item_type='GEM_DIAMOND'})`,
  { start: '{STR=10}', want: { STR: 15 } });

// v1.2.10: item-category condition (identify any GEM) — uses sam_get_item_category.
check('event_item_category_is fires for a gem',
  [rule('player.on_item_identified',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'event_item_category_is', params: { category: 'GEM' } }])],
  `S.fire('player.on_item_identified', {player=0, item_type='GEM_DIAMOND'})`,
  { start: '{STR=10}', want: { STR: 15 } });

check('event_item_category_is does NOT fire for a non-gem',
  [rule('player.on_item_identified',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'event_item_category_is', params: { category: 'GEM' } }])],
  `S.fire('player.on_item_identified', {player=0, item_type='IRON_SWORD'})`,
  { start: '{STR=10}', want: { STR: 10 } });

// ---------------------------------------------------------------------------------
// v1.3.1 (solidius): abilities kept firing after death (gold counter still ticking on a
// corpse). The "player is alive" condition gates it, and the tick handler now skips dead
// players on its own. samsim's stats are global (sam_get_stat ignores the player arg), so
// the tick body runs once per living slot — an idempotent action (move_speed SET) makes
// that observable without 4x-compounding.
check('player_alive gate fires while alive',
  [rule('player.on_hit',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'player_alive', params: {} }])],
  `S.fire('player.on_hit', {player=0})`,
  { start: '{STR=10, HP=50}', want: { STR: 15 } });

check('player_alive gate blocks when dead (HP 0)',
  [rule('player.on_hit',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'player_alive', params: {} }])],
  `S.fire('player.on_hit', {player=0})`,
  { start: '{STR=10, HP=0}', want: { STR: 10 } });

check('a timer does NOT keep firing after death (HP 0 skips the tick guard)',
  [rule('every_seconds', [act('move_speed', { mult: 2, duration: 'permanently' })])],
  `S.seconds(2)`,
  { start: '{HP=0}', want: { speed: '1.0' } });

check('a timer DOES fire while alive',
  [rule('every_seconds', [act('move_speed', { mult: 2, duration: 'permanently' })])],
  `S.seconds(2)`,
  { start: '{HP=50}', want: { speed: '2.0' } });

// Event-level xp / hunger (solidius: "individual values for hunger and exp") — the amount
// from THIS gain / the hunger at THIS change, distinct from the running-total stat compare.
check('xp_amount_cmp fires on a big xp gain (reads event.amount)',
  [rule('player.on_xp_gained',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'xp_amount_cmp', params: { op: '>=', value: 10 } }])],
  `S.fire('player.on_xp_gained', {player=0, amount=25, new_total=100})`,
  { start: '{STR=10}', want: { STR: 15 } });

check('xp_amount_cmp does NOT fire on a small xp gain',
  [rule('player.on_xp_gained',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'xp_amount_cmp', params: { op: '>=', value: 10 } }])],
  `S.fire('player.on_xp_gained', {player=0, amount=3, new_total=50})`,
  { start: '{STR=10}', want: { STR: 10 } });

check('hunger_now_cmp fires when hunger is low',
  [rule('player.on_hunger_change',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'hunger_now_cmp', params: { op: '<=', value: 250 } }])],
  `S.fire('player.on_hunger_change', {player=0, hunger=100})`,
  { start: '{STR=10}', want: { STR: 15 } });

check('hunger_now_cmp does NOT fire when well-fed',
  [rule('player.on_hunger_change',
    [act('set_stat', { stat: 'STR', amount: 5, duration: 'permanently' })],
    [{ id: 'hunger_now_cmp', params: { op: '<=', value: 250 } }])],
  `S.fire('player.on_hunger_change', {player=0, hunger=900})`,
  { start: '{STR=10}', want: { STR: 10 } });

// ---------------------------------------------------------------------------------
// Guards the adversarial design panel demanded — these are pure-JS asserts, no Lua needed.
// ---------------------------------------------------------------------------------
function assert(name, cond) {
  if (cond) { pass++; } else { fail++; failures.push({ name, why: 'assertion failed', lua: '' }); }
}
{
  const until = untilCandidates().map((c) => c.id);
  assert('random chance is NOT offered as an until-condition', !until.includes('chance'));
  assert('a real condition IS offered as an until-condition', until.includes('stat_cmp'));

  const setStat = findAction('set_stat');
  assert('timed change to HP warns', !!setStat.warn({ stat: 'HP', duration: 'for a while' }));
  assert('timed change to STR does NOT warn', !setStat.warn({ stat: 'STR', duration: 'for a while' }));
  assert('permanent change to HP does NOT warn', !setStat.warn({ stat: 'HP', duration: 'permanently' }));
  assert('every engine-written stat warns when timed',
    ENGINE_WRITTEN_STATS.every((s) => !!setStat.warn({ stat: s, duration: 'fading away' })));

  // grant_item's optional beatitude/status/count must only emit the args it needs, so a
  // plain grant stays the simple 2-arg call and never regresses.
  const grant = findAction('grant_item');
  assert('plain grant emits the 2-arg form',
    grant.lua({ item: 'IRON_DAGGER', beatitude: '0', count: 1 }) === 'sam_grant_item(player, "IRON_DAGGER")');
  assert('blessed grant passes just the beatitude',
    grant.lua({ item: 'IRON_DAGGER', beatitude: '2', count: 1 }) === 'sam_grant_item(player, "IRON_DAGGER", 2)');
  assert('cursed multi-grant passes beatitude, status filler (4), and count',
    grant.lua({ item: 'IRON_DAGGER', beatitude: '-1', count: 3 }) === 'sam_grant_item(player, "IRON_DAGGER", -1, 4, 3)');
  assert('a count with no blessing still fills beatitude 0 before the count',
    grant.lua({ item: 'IRON_DAGGER', beatitude: '0', count: 2 }) === 'sam_grant_item(player, "IRON_DAGGER", 0, 4, 2)');

  // apply_effect's optional strength must only emit the 4th arg when set.
  const applyEff = findAction('apply_effect');
  assert('apply_effect stays 3-arg when strength is 0',
    applyEff.lua({ effect: 'FAST', ticks: 100, strength: 0 }) === 'sam_apply_effect(player, "FAST", 100)');
  assert('apply_effect passes the 4th strength arg when set',
    applyEff.lua({ effect: 'POISONED', ticks: 100, strength: 3 }) === 'sam_apply_effect(player, "POISONED", 100, 3)');

  // Event-field conditions are only offered under a trigger that carries the field.
  const goldTrig = findTrigger('player.on_gold_collected');
  const hitTrig = findTrigger('player.on_hit');
  assert('gold_amount_cmp is offered under on_gold_collected',
    conditionsFor(goldTrig).some((c) => c.id === 'gold_amount_cmp'));
  assert('gold_amount_cmp is hidden under on_hit (no amount field)',
    !conditionsFor(hitTrig).some((c) => c.id === 'gold_amount_cmp'));
  assert('event_item_is is hidden under on_gold_collected (no item_type)',
    !conditionsFor(goldTrig).some((c) => c.id === 'event_item_is'));
  assert('a plain condition (stat_cmp) is still offered everywhere',
    conditionsFor(hitTrig).some((c) => c.id === 'stat_cmp'));
  // Event-field conditions must NOT be offered as until-conditions (event is stale in a timer).
  assert('gold_amount_cmp is not an until-candidate',
    !untilCandidates().some((c) => c.id === 'gold_amount_cmp'));

  // v1.2.10 category + monster-effect conditions: correct codegen + trigger gating.
  const identTrig = findTrigger('player.on_item_identified');
  const monTrig = findTrigger('on_monster_damaged');
  assert('event_item_category_is codegen',
    findCondition('event_item_category_is').lua({ category: 'GEM' }) === 'sam_get_item_category(event.item_type) == "GEM"');
  assert('monster_has_effect codegen',
    findCondition('monster_has_effect').lua({ effect: 'POISONED' }) === 'sam_monster_has_effect(event.monster_uid, "POISONED")');
  assert('event_item_category_is offered under on_item_identified',
    conditionsFor(identTrig).some((c) => c.id === 'event_item_category_is'));
  assert('event_item_category_is hidden under on_hit',
    !conditionsFor(hitTrig).some((c) => c.id === 'event_item_category_is'));
  assert('monster_has_effect offered under on_monster_damaged',
    conditionsFor(monTrig).some((c) => c.id === 'monster_has_effect'));
  assert('monster_has_effect hidden under on_hit (no monster_uid)',
    !conditionsFor(hitTrig).some((c) => c.id === 'monster_has_effect'));

  // v1.3.1 (solidius): "is alive" gate + event-level xp/hunger conditions, right gating.
  const xpTrig = findTrigger('player.on_xp_gained');
  const hungerTrig = findTrigger('player.on_hunger_change');
  assert('player_alive codegen',
    findCondition('player_alive').lua({}) === 'sam_get_stat(player, "HP") > 0');
  assert('player_alive is a plain condition offered under every trigger',
    conditionsFor(hitTrig).some((c) => c.id === 'player_alive')
      && conditionsFor(goldTrig).some((c) => c.id === 'player_alive'));
  assert('xp_amount_cmp offered under on_xp_gained',
    conditionsFor(xpTrig).some((c) => c.id === 'xp_amount_cmp'));
  assert('xp_amount_cmp hidden under on_hit (no new_total field)',
    !conditionsFor(hitTrig).some((c) => c.id === 'xp_amount_cmp'));
  assert('hunger_now_cmp offered under on_hunger_change',
    conditionsFor(hungerTrig).some((c) => c.id === 'hunger_now_cmp'));
  assert('hunger_now_cmp hidden under on_hit',
    !conditionsFor(hitTrig).some((c) => c.id === 'hunger_now_cmp'));
  // Retightened gold gate (needs total_gold): no longer leaks onto other amount-carrying events.
  assert('gold_amount_cmp still offered under on_gold_collected',
    conditionsFor(goldTrig).some((c) => c.id === 'gold_amount_cmp'));
  assert('gold_amount_cmp no longer leaks onto on_xp_gained',
    !conditionsFor(xpTrig).some((c) => c.id === 'gold_amount_cmp'));
}

// ---------------------------------------------------------------------------------
// Reducer action shapes.
//
// A page that dispatches the wrong key does not throw: the reducer spreads `undefined`, the
// state is unchanged, and the UI looks like it worked. ModelEditor shipped that way -- it
// sent { meta } where the reducer reads { patch }, so every declared model vanished on save.
// These drive the reducer directly, which is the only level that catches it.
{
  const { reducer } = await import('@/state/modReducer.js');
  const base = { meta: { namespace: 'mymod', models: [] }, assets: {} };

  const afterMeta = reducer(base, { type: 'setMeta', patch: { models: [{ id: 'mymod:a', file: 'models/mymod/a.vox' }] } });
  assert('setMeta patch actually changes meta', afterMeta.meta.models.length === 1);
  assert('setMeta patch keeps other meta keys', afterMeta.meta.namespace === 'mymod');

  // The exact bug: the wrong key must not silently look like success.
  const afterWrongKey = reducer(base, { type: 'setMeta', meta: { models: [{ id: 'x', file: 'y' }] } });
  assert('setMeta with the WRONG key is a no-op (so a test must catch it, not the user)',
    (afterWrongKey.meta.models || []).length === 0);

  const afterAsset = reducer(base, { type: 'setAsset', path: 'models/mymod/a.vox', dataUrl: 'data:,x' });
  assert('setAsset stores under its path', afterAsset.assets['models/mymod/a.vox'] === 'data:,x');
  const afterRemove = reducer(afterAsset, { type: 'removeAsset', path: 'models/mymod/a.vox' });
  assert('removeAsset drops it', afterRemove.assets['models/mymod/a.vox'] === undefined);
}

// Every setMeta dispatch in the app must use `patch`, since the reducer reads nothing else.
{
  const { readdirSync, readFileSync, statSync } = await import('node:fs');
  const { join } = await import('node:path');
  const SRC = fileURLToPath(new URL('../src/', import.meta.url));
  const walk = (d) => readdirSync(d).flatMap((f) => {
    const p = join(d, f);
    return statSync(p).isDirectory() ? walk(p) : [p];
  });
  const offenders = [];
  for (const file of walk(SRC).filter((f) => /\.(jsx?|mjs)$/.test(f))) {
    const text = readFileSync(file, 'utf8');
    const re = /dispatch\(\{\s*type:\s*'setMeta'\s*,\s*([A-Za-z_$][\w$]*)/g;
    let m;
    while ((m = re.exec(text))) {
      if (m[1] !== 'patch') offenders.push(`${file.slice(SRC.length)}: setMeta with '${m[1]}'`);
    }
  }
  assert(`no setMeta dispatch uses a key other than 'patch'${offenders.length ? ' -- ' + offenders.join('; ') : ''}`,
    offenders.length === 0);
}

// ---------------------------------------------------------------------------------
// sam_play_sound(sound, volume). There is no player argument. The block used to emit
// sam_play_sound(player, N), which the engine reads as "sound #<player> at volume N" -- sound
// #0 is a leather footstep -- and the stub ignored its arguments, so nothing noticed. The stub
// now records what it was asked for, the way lua_sam_play_sound reads it.
// ---------------------------------------------------------------------------------
check('play_sound passes the SOUND first, never the player',
  [rule('player.on_hit', [act('play_sound', { id: 28, volume: 128 })])],
  `S.fire('player.on_hit', {player=2})
   local c = S.state().sounds[1]
   assert(c, 'sam_play_sound was never called')
   assert(c.id == 28, 'argument 1 must be the sound (28), got ' .. tostring(c.id))
   assert(c.vol == 128, 'volume should be the default 128, got ' .. tostring(c.vol))`,
  { want: { err: '-' } });

check('play_sound plays one of the mod\'s own sounds by its id, at the chosen volume',
  [rule('player.on_hit', [act('play_sound', { id: 'mymod:boom', volume: 200 })])],
  `S.fire('player.on_hit', {player=0})
   local c = S.state().sounds[1]
   assert(c and c.id == 'mymod:boom', 'expected "mymod:boom", got ' .. tostring(c and c.id))
   assert(c.vol == 200, 'expected volume 200, got ' .. tostring(c.vol))`,
  { want: { err: '-' } });

check('play_sound rounds a fractional volume (the engine raises on 99.6)',
  [rule('player.on_hit', [act('play_sound', { id: 5, volume: 99.6 })])],
  `S.fire('player.on_hit', {player=0})
   local c = S.state().sounds[1]
   assert(c and c.id == 5 and c.vol == 100, 'got ' .. tostring(c and c.id) .. ' at ' .. tostring(c and c.vol))`,
  { want: { err: '-' } });

// The test above is only worth something if the OLD line fails it. Run the old line itself.
check('the stub reads the old argument order as sound #0 (so the tests above catch it)',
  [rule('player.on_hit', [act('message', { text: 'x' })])],
  `sam_play_sound(0, 28)
   local c = S.state().sounds[1]
   assert(c.id == 0 and c.vol == 28, 'stub no longer mirrors the engine')`,
  { want: { err: '-' } });

{
  const ps = findAction('play_sound');
  assert('play_sound: the default block emits the 1-argument form',
    ps.lua({ id: 28, volume: 128 }) === 'sam_play_sound(28)');
  assert('play_sound: a block saved before the fix ({ id } only) still generates the right call',
    ps.lua({ id: 28 }) === 'sam_play_sound(28)');
  assert('play_sound: a custom sound id is quoted',
    ps.lua({ id: 'mymod:boom', volume: 128 }) === 'sam_play_sound("mymod:boom")');
  assert('play_sound: volume is passed second, clamped and whole',
    ps.lua({ id: 3, volume: 999 }) === 'sam_play_sound(3, 255)');
  assert('play_sound: never passes the player', !/player/.test(ps.lua({ id: 28, volume: 90 })));
}

// ---------------------------------------------------------------------------------
// Sounds and music: export, the builder's own schema validation, import, and the reducer.
// ---------------------------------------------------------------------------------
{
  const { buildModFiles } = await import('@/lib/exportZip.js');
  const { parseModZip } = await import('@/lib/importZip.js');
  const { validate } = await import('@/lib/validate.js');
  const { reducer, initialState } = await import('@/state/modReducer.js');
  const audio = await import('@/lib/audio.js');
  const { VANILLA_SOUNDS } = await import('@/data/vanillaSounds.js');
  const JSZip = (await import('jszip')).default;

  const au = (s) => `data:audio/ogg;base64,${Buffer.from(s).toString('base64')}`;
  // One added sound, one replacement, one track (with floors, maps and a fight track), one
  // replaced track.
  const sample = {
    meta: { ...initialState.meta, namespace: 'mymod', name: 'Audio Test' },
    sounds: [
      { id: 'mymod:boom', file: 'sounds/boom.ogg', volume: 0.8 },
      { replace: 'SwingWeapon', file: ['sounds/swingweapon.ogg', 'sounds/swingweapon_2.wav'] },
    ],
    music: [
      { id: 'mymod:deep', file: 'music/deep.ogg', floors: [7, 8], maps: ['My Hub'], combat: 'music/deep_combat.ogg' },
      { replace: 'mines', file: ['music/mines.ogg', 'music/mines_2.mp3'], loop: true },
    ],
    assets: {
      'sounds/boom.ogg': au('boom'), 'sounds/swingweapon.ogg': au('sw1'), 'sounds/swingweapon_2.wav': au('sw2'),
      'music/deep.ogg': au('deep'), 'music/deep_combat.ogg': au('fight'), 'music/mines.ogg': au('m1'), 'music/mines_2.mp3': au('m2'),
    },
  };
  const files = buildModFiles(sample);
  const manifest = JSON.parse(files.find((f) => f.path === 'mod.json').text);
  const modRes = validate('mod', manifest);
  assert(`exported mod.json passes the builder's own mod schema${modRes.valid ? '' : ' -- ' + JSON.stringify(modRes.errors)}`, modRes.valid);
  assert('sounds are written INLINE into mod.json as objects',
    Array.isArray(manifest.sounds) && manifest.sounds.length === 2 && manifest.sounds.every((e) => e && typeof e === 'object'));
  assert('no per-sound JSON files are written', !files.some((f) => /^sounds\/.*\.json$/i.test(f.path)));
  assert('music is written inline into mod.json', manifest.music?.length === 2 && manifest.music[0].floors.join() === '7,8'
    && manifest.music[0].combat === 'music/deep_combat.ogg' && manifest.music[1].replace === 'mines');
  assert('every sound entry passes sound.schema.json', manifest.sounds.every((e) => validate('sound', e).valid));
  assert('every music entry passes the mod schema\'s music item', manifest.music.every((e) => validate('music', e).valid));
  assert('every audio file is in the bundle', Object.keys(sample.assets).every((p) => files.some((f) => f.path === p && f.base64)));
  assert('the schema rejects an entry with both id and replace (so the editor must never make one)',
    !validate('sound', { id: 'a', replace: 'Damage', file: 'a.ogg' }).valid);

  // Round trip through a real zip, wrapped in its <namespace>/ folder like an export.
  const zip = new JSZip();
  for (const f of files) {
    if (f.base64 !== undefined) zip.file(`mymod/${f.path}`, f.base64, { base64: true });
    else zip.file(`mymod/${f.path}`, f.text);
  }
  const back = await parseModZip(await zip.generateAsync({ type: 'uint8array' }));
  assert('round trip keeps both sounds exactly', JSON.stringify(back.sounds) === JSON.stringify(sample.sounds));
  assert('round trip keeps both tracks exactly', JSON.stringify(back.music) === JSON.stringify(sample.music));
  assert('round trip keeps every audio file byte for byte',
    Object.keys(sample.assets).every((p) => back.assets[p] && back.assets[p].split(',')[1] === sample.assets[p].split(',')[1]));
  assert('round trip gives audio a playable type, not octet-stream', back.assets['music/mines_2.mp3'].startsWith('data:audio/mpeg;'));
  assert(`round trip reports nothing${back.report.length ? ' -- ' + JSON.stringify(back.report) : ''}`, back.report.length === 0);

  // An older mod: "sounds" lists a sound JSON file and a bare audio path, plus a file dropped
  // in sounds/replace/ with no entry at all, and one broken inline entry.
  const legacy = new JSZip();
  legacy.file('mod.json', JSON.stringify({
    namespace: 'old', name: 'Old', version: '1.0.0', framework_min_version: '0.1.0',
    sounds: ['sounds/boom.json', 'sounds/bang.json', 'sounds/Zap Zap.ogg', { id: 'both', replace: 'Damage', file: 'sounds/x.ogg' }],
  }));
  legacy.file('sounds/boom.json', JSON.stringify({ $schema: 'x', id: 'old:boom', file: 'sounds/boom.ogg', loop: true }));
  legacy.file('sounds/bang.json', JSON.stringify({ id: 'bang', file: 'sounds/bang.wav' }));
  legacy.file('sounds/boom.ogg', 'b');
  legacy.file('sounds/bang.wav', 'g');
  legacy.file('sounds/Zap Zap.ogg', 'z');
  legacy.file('sounds/replace/DoorOpen.ogg', 'd');
  const old = await parseModZip(await legacy.generateAsync({ type: 'uint8array' }));
  const byKey = new Map(old.sounds.map((s) => [audio.audioKey(s), s]));
  assert('legacy: a sound JSON file listed in "sounds" imports',
    JSON.stringify(byKey.get('id:old:boom')) === JSON.stringify({ id: 'old:boom', file: 'sounds/boom.ogg', loop: true }));
  assert('legacy: a bare id in a sound JSON gets the namespace', byKey.get('id:old:bang')?.file === 'sounds/bang.wav');
  assert('legacy: a bare audio path imports under the name the engine gives it ("Zap Zap" -> zap_zap)',
    byKey.get('id:old:zap_zap')?.file === 'sounds/Zap Zap.ogg');
  assert('legacy: the bare audio path\'s FILE is kept (it used to be treated as declared JSON and dropped)',
    !!old.assets['sounds/Zap Zap.ogg']);
  assert('folder convention: sounds/replace/DoorOpen.ogg becomes a replacement entry',
    byKey.get('replace:dooropen')?.file === 'sounds/replace/DoorOpen.ogg');
  assert('a broken inline entry is reported and skipped, not imported',
    !byKey.has('id:old:both') && old.report.some((r) => /sounds\[3\]/.test(r.path)));
  const reFiles = buildModFiles({ ...old, meta: { ...old.meta } });
  const reManifest = JSON.parse(reFiles.find((f) => f.path === 'mod.json').text);
  assert('legacy: re-exporting writes every sound inline and still validates',
    reManifest.sounds.every((e) => typeof e === 'object') && validate('mod', reManifest).valid
    && !reFiles.some((f) => /^sounds\/.*\.json$/i.test(f.path)));

  // The reducer: edits happen IN PLACE, and audio nothing uses any more is dropped.
  let st = { ...initialState, assets: { 'sounds/boom.ogg': 'data:,1', 'sounds/other.ogg': 'data:,2' } };
  st = reducer(st, { type: 'saveSound', def: { id: 'm:boom', file: 'sounds/boom.ogg' } });
  st = reducer(st, { type: 'saveSound', def: { id: 'm:bang', file: 'sounds/boom.ogg' }, prevKey: 'id:m:boom' });
  assert('renaming a saved sound updates it in place instead of adding a second one',
    st.sounds.length === 1 && st.sounds[0].id === 'm:bang');
  assert('a rename keeps the audio the sound still uses', !!st.assets['sounds/boom.ogg']);
  st = reducer(st, { type: 'saveSound', def: { id: 'm:bang', file: 'sounds/other.ogg' }, prevKey: 'id:m:bang' });
  assert('swapping a sound\'s file drops the old file nothing else uses',
    !st.assets['sounds/boom.ogg'] && !!st.assets['sounds/other.ogg']);
  st = reducer(st, { type: 'saveSound', def: { id: 'm:copy', file: 'sounds/other.ogg' } });
  st = reducer(st, { type: 'removeSound', key: 'id:m:bang' });
  assert('removing a sound keeps audio another sound shares', !!st.assets['sounds/other.ogg'] && st.sounds.length === 1);
  st = reducer(st, { type: 'removeSound', key: 'id:m:copy' });
  assert('removing the last sound using a file drops the file (else it still ships and still registers)',
    !st.assets['sounds/other.ogg'] && st.sounds.length === 0);
  st = reducer(st, { type: 'saveSound', def: { replace: 'SwingWeapon', file: 'sounds/a.ogg' } });
  st = reducer(st, { type: 'saveSound', def: { replace: 'swingweapon', file: 'sounds/a.ogg' } });
  assert('two replacements of one game sound (in any case) are one entry, as the engine sees them',
    st.sounds.length === 1 && st.sounds[0].replace === 'swingweapon');
  st = { ...st, assets: { ...st.assets, 'music/t.ogg': 'data:,t', 'music/t_combat.ogg': 'data:,c' } };
  st = reducer(st, { type: 'saveMusic', def: { id: 'm:t', file: 'music/t.ogg', combat: 'music/t_combat.ogg' } });
  st = reducer(st, { type: 'saveMusic', def: { id: 'm:t2', file: 'music/t.ogg' }, prevKey: 'id:m:t' });
  assert('editing a track in place drops a fight track it no longer has',
    st.music.length === 1 && st.music[0].id === 'm:t2' && !st.assets['music/t_combat.ogg'] && !!st.assets['music/t.ogg']);
  st = reducer(st, { type: 'removeMusic', key: 'id:m:t2' });
  assert('removing a track drops its audio', st.music.length === 0 && !st.assets['music/t.ogg']);
  const afterLoad = reducer(initialState, { type: 'loadMod', ...back });
  assert('loadMod carries music', afterLoad.music.length === 2);

  // Where new uploads go: never onto a file another entry uses; an entry may reuse its own.
  const p1 = audio.assignAudioPaths({ files: [{ ext: 'ogg' }, { ext: 'wav' }], folder: 'sounds', slug: 'boom',
    assets: { 'sounds/boom.ogg': 'x' }, otherUse: ['sounds/boom.ogg'] });
  assert('a new upload never overwrites another sound\'s file', p1.join() === 'sounds/boom_2.ogg,sounds/boom_3.wav');
  const p2 = audio.assignAudioPaths({ files: [{ ext: 'ogg' }], folder: 'sounds', slug: 'boom',
    assets: { 'sounds/boom.ogg': 'x' }, ownOld: ['sounds/boom.ogg'] });
  assert('an entry replacing its own file may reuse its path', p2.join() === 'sounds/boom.ogg');
  // Renaming the "boom" sound to "bang": files the editor named after "boom" follow the new
  // name, or "bang" would ship as sounds/boom.ogg and the folder scan would register a stray
  // "boom". This is an ADD entry, so the third file has to leave sounds/replace/ as well --
  // it replaces nothing now, and a file left there replaces DoorOpen whatever mod.json says.
  const own = { 'sounds/boom.ogg': 'x', 'sounds/boom_2.wav': 'y', 'sounds/replace/DoorOpen.ogg': 'z' };
  const rows = Object.keys(own).map((path) => ({ path }));
  const plan = audio.planAudioRows(rows, { folder: 'sounds', oldSlug: 'boom', newSlug: 'bang', assets: own, otherUse: [], replaceTarget: null });
  const moved = audio.assignAudioPaths({ files: plan, folder: 'sounds', slug: 'bang', assets: own, ownOld: Object.keys(own) });
  assert('a rename carries the files the editor named after the old name',
    moved[0] === 'sounds/bang.ogg' && moved[1] === 'sounds/bang_2.wav'
    && plan[0].from === 'sounds/boom.ogg' && plan[1].from === 'sounds/boom_2.wav');
  assert('...and takes a drop-in with it rather than leaving a replacement behind',
    plan[2].from === 'sounds/replace/DoorOpen.ogg' && !/replace\//.test(moved[2]));
  const sharedPlan = audio.planAudioRows([{ path: 'sounds/boom.ogg' }], { folder: 'sounds', oldSlug: 'boom', newSlug: 'bang', assets: own, otherUse: ['sounds/boom.ogg'], replaceTarget: null });
  assert('a file another entry also plays is never moved out from under it', sharedPlan[0].path === 'sounds/boom.ogg');

  // The picker has to agree with SAMSounds::vanillaIndicesFor: a name that is both a group and
  // one sound in it means the whole group, and the one sound alone is stored as its number.
  assert('"SwingWeapon" is the whole group of five', audio.resolveVanillaSound('SwingWeapon')?.via === 'group'
    && audio.resolveVanillaSound('SwingWeapon').sounds.length === 5);
  const casting = audio.resolveVanillaSound('Casting');
  assert('"Casting" (a group AND one sound in it) is the whole group of four', casting?.via === 'group'
    && casting.sounds.length === 4 && audio.groupNameMeansGroup('Casting'));
  const baseCasting = casting.sounds.find((s) => s.name === 'Casting');
  const oneCasting = audio.singleSoundTarget(baseCasting);
  assert('picking the one base "Casting" stores its number, which resolves to just it',
    oneCasting === String(baseCasting.i) && audio.resolveVanillaSound(oneCasting).sounds.length === 1);
  const swing3 = audio.resolveVanillaSound(25).sounds[0];
  assert('picking an ordinary one sound stores its name', audio.singleSoundTarget(swing3) === 'SwingWeapon3V1');
  // "Punch" is one file the game lists twice: one sound in two slots, not a group to offer.
  const distinctNames = (g) => new Set(g.sounds.map((s) => s.name.toLowerCase())).size;
  assert('every group of more than one distinct sound offers "whole group"',
    VANILLA_SOUNDS.groups.every((g) => distinctNames(g) < 2 || audio.groupNameMeansGroup(g.group)));
  const punch = audio.resolveVanillaSound('Punch');
  assert('a file listed twice is one sound, and its name replaces both copies',
    punch?.via === 'sound' && punch.sounds.length === 2 && audio.singleSoundTarget(punch.sounds[1]) === 'Punch');
  assert('an index resolves to its sound', audio.resolveVanillaSound(25)?.sounds[0].name === 'SwingWeapon3V1');
  assert('an unknown name is flagged', audio.describeSoundTarget('NotASound') === null);
  assert('floors parse as whole numbers, junk is reported',
    JSON.stringify(audio.parseIntList('7, 8 x')) === JSON.stringify({ values: [7, 8], bad: ['x'] }));

  // Retargeting a drop-in must not leave the file it was named after still replacing a
  // sound. The engine's folder scan reads sounds/replace/<Name>.<ext> on its own, whatever
  // mod.json says, so a file left behind under the old name replaces that sound too.
  const dropIn = [{ path: 'sounds/replace/DoorOpen.ogg' }];
  const dropAssets = { 'sounds/replace/DoorOpen.ogg': 'data:audio/ogg;base64,AA==' };
  const retarget = audio.planAudioRows(dropIn, {
    folder: 'sounds', oldSlug: 'dooropen', newSlug: 'doorclose', assets: dropAssets, otherUse: [],
    replaceTarget: 'DoorClose',
  });
  assert('an imported drop-in moves when the entry stops replacing what it is named after',
    retarget[0].from === 'sounds/replace/DoorOpen.ogg');
  const movedDropIn = audio.assignAudioPaths({
    files: retarget, folder: 'sounds', slug: 'doorclose', assets: dropAssets,
    ownOld: ['sounds/replace/DoorOpen.ogg'],
  });
  assert('...to a path the folder scan cannot read as a replacement for the old sound',
    !/replace\/dooropen/i.test(movedDropIn[0]));
  const kept = audio.planAudioRows(dropIn, {
    folder: 'sounds', oldSlug: 'dooropen', newSlug: 'dooropen', assets: dropAssets, otherUse: [],
    replaceTarget: 'DoorOpen',
  });
  assert('a drop-in that still replaces its own name stays exactly where the author put it',
    kept[0].path === 'sounds/replace/DoorOpen.ogg');
  const toAdd = audio.planAudioRows(dropIn, {
    folder: 'sounds', oldSlug: 'dooropen', newSlug: 'creak', assets: dropAssets, otherUse: [],
    replaceTarget: null,
  });
  assert('switching a drop-in from Replace to Add moves it too, so nothing is replaced by accident',
    toAdd[0].from === 'sounds/replace/DoorOpen.ogg');
  const shared = audio.planAudioRows(dropIn, {
    folder: 'sounds', oldSlug: 'dooropen', newSlug: 'doorclose', assets: dropAssets,
    otherUse: ['sounds/replace/DoorOpen.ogg'], replaceTarget: 'DoorClose',
  });
  assert('a drop-in another entry also plays is still never moved out from under it',
    shared[0].path === 'sounds/replace/DoorOpen.ogg');
}

// ---------------------------------------------------------------------------------
// AN EDITOR MUST NOT DELETE WHAT IT CANNOT SHOW.
//
// Every editor rebuilds its definition from scratch on save, so a field with no control on
// the page was simply absent from the new object and vanished, with a green "Saved" next to
// it. The fix is stated the other way round -- each editor declares the keys it OWNS and
// everything else is carried -- so the thing to check is that the owned list is honest and
// that an unknown key really does survive.
// ---------------------------------------------------------------------------------
{
  const { EDITOR_KEYS, carryUnknown } = await import('../src/lib/editorKeys.js');
  const schemas = await import('../src/data/schemas.js');
  const SCHEMA_OF = {
    class: schemas.classSchema, item: schemas.itemSchema, monster: schemas.monsterSchema,
    spell: schemas.spellSchema, effect: schemas.effectSchema, race: schemas.raceSchema,
    recipe: schemas.recipeSchema, patch: schemas.patchSchema,
  };

  // A typo in an owned key is silent and nasty: the real key falls into the carried set, so
  // clearing that box in the editor stops clearing the field.
  const strays = [];
  for (const [kind, keys] of Object.entries(EDITOR_KEYS)) {
    const props = Object.keys(SCHEMA_OF[kind]?.properties ?? {});
    for (const k of keys) if (!props.includes(k)) strays.push(`${kind}.${k}`);
  }
  assert(`every key an editor claims to own is a real schema property${strays.length ? ` -- ${strays.join(', ')}` : ''}`,
    strays.length === 0);

  // The exact loss from the audit: a race with bent arms AND first_person, opened and saved.
  const opened = {
    $schema: '../schemas/race.schema.json',
    id: 'mymod:wraith', name: 'Wraith', host_body: 'skeleton',
    limb_models: { arm_right: { model: 'mymod:ar' } },
    first_person: { arm_right: 'mymod:ar_fp' },
    extra_limbs: [{ model: 'mymod:tail' }],
  };
  const saved = carryUnknown(opened, {
    id: 'mymod:wraith', name: 'Wraith', host_body: 'skeleton',
    limb_models: { arm_right: { model: 'mymod:ar', bent: 'mymod:ar_bent' } },
  }, 'race');
  assert('first_person survives a race save', JSON.stringify(saved.first_person) === JSON.stringify(opened.first_person));
  assert('extra_limbs survives a race save', saved.extra_limbs?.length === 1);
  assert('$schema survives a race save', saved.$schema === opened.$schema);
  assert('the edited field is the NEW one, not the carried one', saved.limb_models.arm_right.bent === 'mymod:ar_bent');

  // A key the editor OWNS must still be removable: clearing the description box has to clear
  // it, which is exactly what a blanket "spread the old def underneath" would have broken.
  const cleared = carryUnknown({ id: 'a:b', name: 'B', host_body: 'skeleton', description: 'old' },
    { id: 'a:b', name: 'B', host_body: 'skeleton' }, 'race');
  assert('clearing a field the editor owns still clears it', !('description' in cleared));

  // The same shape for the other editors named in the audit.
  const item = carryUnknown({ id: 'a:b', name_identified: 'B', model_states: { 1: 'a:m' } },
    { id: 'a:b', name_identified: 'B' }, 'item');
  assert('model_states survives an item save', item.model_states?.[1] === 'a:m');
  const cls = carryUnknown({ id: 'a:b', name: 'B', blood_diet: true }, { id: 'a:b', name: 'B' }, 'class');
  assert('blood_diet survives a class save', cls.blood_diet === true);
  const spell = carryUnknown({ id: 'a:b', name: 'B', difficulty: 40 }, { id: 'a:b', name: 'B' }, 'spell');
  assert('difficulty survives a spell save', spell.difficulty === 40);
  // kit_ui is read by the engine (sam_items.cpp) and is not in item.schema.json yet, so it is
  // the live example of a key no editor can know about.
  const kit = carryUnknown({ id: 'a:b', name_identified: 'B', kit_ui: { frame: 'images/x.png' } },
    { id: 'a:b', name_identified: 'B' }, 'item');
  assert('a key no schema mentions survives too', kit.kit_ui?.frame === 'images/x.png');

  assert('a brand new definition carries nothing', JSON.stringify(carryUnknown(null, { id: 'a:b' }, 'race')) === '{"id":"a:b"}');
}

// ---------------------------------------------------------------------------------
// "CHANGES SINCE LAST EXPORT" HAS TO SEE EVERY COLLECTION.
//
// canonicalize destructured four of them, so adding a sound, a track, a race, a spell, a
// recipe or a patch left the panel saying "no changes since baseline" -- on a release whose
// headline content feature is audio. The list is checked against the baseline snapshot the
// reducer actually takes, so the two cannot drift apart again.
// ---------------------------------------------------------------------------------
{
  const { canonicalize, diffLines, diffSummary, COLLECTIONS } = await import('../src/lib/jsonDiff.js');
  const { reducer, initialState } = await import('../src/state/modReducer.js');

  const withBaseline = reducer(initialState, { type: 'setBaseline' });
  const snapshot = Object.keys(withBaseline.baseline).filter((k) => k !== 'meta');
  const missing = snapshot.filter((k) => !COLLECTIONS.includes(k));
  const extra = COLLECTIONS.filter((k) => !snapshot.includes(k));
  assert(`the diff covers every collection the baseline holds${missing.length ? ` -- blind to ${missing.join(', ')}` : ''}`,
    missing.length === 0);
  assert(`the diff names no collection the baseline does not hold${extra.length ? ` -- ${extra.join(', ')}` : ''}`,
    extra.length === 0);

  // One added entry in each collection must show up as a change, one at a time, so a single
  // collection going blind cannot hide behind the others.
  const sample = {
    classes: { id: 'a:c' }, items: { id: 'a:i' }, monsters: { id: 'a:m' }, spells: { id: 'a:s' },
    effects: { id: 'a:e' }, races: { id: 'a:r' }, sounds: { replace: 'DoorOpen', file: 'sounds/replace/DoorOpen.ogg' },
    music: { id: 'a:t', file: 'music/t.ogg' }, recipes: { id: 'a:rec' }, patches: { target: 'items/items.json' },
  };
  const base = canonicalize(withBaseline.baseline);
  const blind = COLLECTIONS.filter((name) => {
    const after = canonicalize({ ...withBaseline.baseline, [name]: [sample[name]] });
    return diffSummary(diffLines(base, after)).added === 0;
  });
  assert(`adding one entry shows up as a change in all ${COLLECTIONS.length} collections`
    + (blind.length ? ` -- silent in ${blind.join(', ')}` : ''), blind.length === 0);

  // Sounds and music have no id when they replace something, so they need a sort key that is
  // not `id` -- otherwise every replacement sorts equal and a reorder reads as a change.
  const twoWays = ['a', 'b'].map((order) => canonicalize({
    ...withBaseline.baseline,
    sounds: order === 'a'
      ? [{ replace: 'DoorOpen', file: 'x.ogg' }, { replace: 'Casting', file: 'y.ogg' }]
      : [{ replace: 'Casting', file: 'y.ogg' }, { replace: 'DoorOpen', file: 'x.ogg' }],
  }));
  assert('two replacements in a different order are not reported as a change',
    diffSummary(diffLines(twoWays[0], twoWays[1])).added === 0);
}

// ---------------------------------------------------------------------------------
// EVERY block must generate Lua that at least PARSES.
//
// The cases above are hand written, so a block nobody wrote a case for was never run at
// all -- nine blocks were added at one point and the suite still reported the same number
// of tests. A typo in one of those would have shipped and only surfaced as a mod that
// silently did nothing. This sweeps the whole catalog with each block's own defaults, so
// a new block is covered the moment it exists.
//
// Parsing is a low bar on purpose: it cannot know what a block SHOULD do. It is here to
// catch the class of error that makes a block inert rather than wrong.
// ---------------------------------------------------------------------------------
if (LUA) {
  const broken = [];
  const probe = (kind, b) => {
    const p = {};
    for (const q of (b.params || [])) p[q.name] = q.default;
    let body;
    try { body = b.lua(p); }
    catch (e) { broken.push(`${kind} '${b.id}': generator threw -- ${e.message}`); return; }
    if (typeof body !== 'string' || !body.trim()) {
      broken.push(`${kind} '${b.id}': produced no Lua`); return;
    }
    // A condition is an expression; an action is a statement. Wrap each so it is valid
    // where the real codegen puts it, with the same locals in scope.
    const src = kind === 'condition'
      ? `local player, event = 0, {}
local _ = (${body})
`
      : `local player, event = 0, {}
${body}
`;
    const f = join(TMP, 'probe.lua');
    writeFileSync(f, src);
    const luaPath = JSON.stringify(f.split('\\').join('/'));
    const oneLiner = `local fn, err = loadfile(${luaPath}); if not fn then io.write(tostring(err)) end`;
    try {
      const err = execFileSync(LUA, ['-e', oneLiner], { encoding: 'utf8' }).trim();
      if (err) broken.push(`${kind} '${b.id}': ${err}`);
    } catch (e) { broken.push(`${kind} '${b.id}': lua crashed -- ${e.message}`); }
  };
  for (const b of ACTIONS) probe('action', b);
  for (const b of CONDITIONS) probe('condition', b);
  assert(`all ${ACTIONS.length} actions and ${CONDITIONS.length} conditions generate parseable Lua`
    + (broken.length ? ` -- ${broken.join(' | ')}` : ''), broken.length === 0);
}

// ---------------------------------------------------------------------------------
// EVERY TRIGGER must hand its actions a player the API will accept.
//
// TRIGGERS is the one list in blocks.js that is DERIVED -- it is built from SAM_EVENTS, so
// a new event in the manifest turns into a new trigger in the builder with no code change
// and no test of its own. The two sweeps above cover ACTIONS and CONDITIONS and say "a new
// block is covered the moment it exists"; neither of them ever touches TRIGGERS. That gap
// is how on_before_effect_applied shipped generating `local player = event.player` for an
// event whose player is -1 on every monster: the hook sits in Entity::setEffect, so it
// fires for monsters too, and every action the builder offers takes a player first.
//
// So: for each trigger, build the one-action ability a first-time user gets, fire the event
// the way the framework fires it -- with player = -1 on the triggers whose fire site can
// produce one -- and look at what actually happened.
//
// Two-sided on purpose. A real player must still receive the message, and a non-player must
// produce neither a message nor a call the engine would refuse. Half of this would pass on a
// generator that guarded everything away, and the other half passes today on the broken one.
// ---------------------------------------------------------------------------------
if (LUA) {
  /** A plausible value for an event field, by the manifest's declared type. */
  const sampleField = (f) => {
    const t = String(f.type || '');
    if (t.startsWith('string')) return `"x"`;
    return '1';               // int, uid and the annotated int variants
  };

  /** Run a generated script and report what the simulator saw, rather than a stat total. */
  const runTrigger = (t, playerValue) => {
    const rules = [{
      key: 'sweep',
      trigger: { id: t.id, params: Object.fromEntries((t.params || []).map((p) => [p.name, p.default])) },
      conditions: [],
      actions: [{ id: 'message', params: { text: 'fired' } }],
    }];
    const lua = generateLua({ rules });
    const script = join(TMP, 'trig.lua');
    writeFileSync(script, lua);
    const fields = (t.payload || [])
      .map((f) => `${f.field}=${f.field === 'player' ? String(playerValue) : sampleField(f)}`)
      .join(', ');
    const harness = `
      local S = dofile(${JSON.stringify(join(HERE, 'samsim.lua').replace(/\\/g, '/'))})
      S.reset({})
      local f = io.open(${JSON.stringify(script.replace(/\\/g, '/'))}, 'r')
      local src = f:read('a'); f:close()
      local ok, err = S.load(src)
      if not ok then print('LOADFAIL\\t' .. err) os.exit(0) end
      S.fire(${JSON.stringify(t.id)}, { ${fields} })
      print('OK\\t' .. #S.state().messages .. '|' .. #S.errors() .. '|' .. #S.refusedPlayerCalls()
        .. '|' .. (#S.errors() > 0 and S.errors()[1] or '-'))
    `;
    const hpath = join(TMP, 'trig_h.lua');
    writeFileSync(hpath, harness);
    let out;
    try { out = execFileSync(LUA, [hpath], { encoding: 'utf8' }).trim(); }
    catch (e) { return { bad: 'CRASH ' + (e.stdout || e.message), lua }; }
    const [tag, payload] = out.split('\t');
    if (tag !== 'OK') return { bad: out, lua };
    const [messages, errors, refusedCalls, firstError] = payload.split('|');
    return { messages: Number(messages), errors: Number(errors), refused: Number(refusedCalls), firstError, lua };
  };

  const eventTriggers = TRIGGERS.filter((t) => t.id !== EVERY_SECONDS);
  const wrong = [];
  let withPlayer = 0;
  for (const t of eventTriggers) {
    if (!triggerHasPlayer(t)) continue;   // these generate `local player = 0`; nothing to get wrong
    withPlayer++;

    // The ordinary case: a real player is in the event, so the ability must actually run.
    const good = runTrigger(t, 0);
    if (good.bad) { wrong.push(`${t.id}: ${good.bad}`); continue; }
    if (good.messages !== 1 || good.errors !== 0 || good.refused !== 0) {
      wrong.push(`${t.id} with player 0: expected 1 message and no refusals, got `
        + `${good.messages} message(s), ${good.errors} error(s) (${good.firstError}), ${good.refused} refused call(s)`);
    }

    // The case the framework can actually produce for this event.
    if (!triggerPlayerMayBeMissing(t)) continue;
    const none = runTrigger(t, -1);
    if (none.bad) { wrong.push(`${t.id}: ${none.bad}`); continue; }
    if (none.refused !== 0 || none.errors !== 0 || none.messages !== 0) {
      wrong.push(`${t.id} with player -1 (which ${t.id === 'on_before_effect_applied' ? 'every monster' : 'a monster or a trap'} produces): `
        + `expected the script to do nothing, got ${none.messages} message(s), `
        + `${none.errors} error(s) (${none.firstError}), ${none.refused} call(s) the engine would refuse`);
    }
  }
  assert(`all ${withPlayer} player-carrying triggers generate code that survives their own fire site`
    + (wrong.length ? ` -- ${wrong.join(' | ')}` : ''), wrong.length === 0);

  // The list above is only as good as its inventory: if PLAYER_MAY_BE_MISSING ever named an
  // event that no longer exists, the sweep would quietly stop testing it.
  const unknown = [...PLAYER_MAY_BE_MISSING].filter((n) => !TRIGGERS.some((t) => t.id === n));
  assert(`every event named in PLAYER_MAY_BE_MISSING still exists${unknown.length ? ` -- ${unknown.join(', ')}` : ''}`,
    unknown.length === 0);
  const noPlayer = [...PLAYER_MAY_BE_MISSING].filter((n) => !triggerHasPlayer(TRIGGERS.find((t) => t.id === n)));
  assert(`every event named in PLAYER_MAY_BE_MISSING still carries a player field${noPlayer.length ? ` -- ${noPlayer.join(', ')}` : ''}`,
    noPlayer.length === 0);
}

// ---------------------------------------------------------------------------------
// Every SNIPPET must be valid in BOTH languages.
//
// Snippets are copy-paste starters, so a broken one does not fail loudly -- it teaches a
// modder something wrong and costs them an evening. Both forms are checked because the
// pair is meant to be the same script in two languages, and it is easy to fix one and
// forget the other.
// ---------------------------------------------------------------------------------
if (LUA) {
  const bad = [];
  for (const sn of SNIPPETS) {
    const f = join(TMP, 'snippet.lua');
    writeFileSync(f, sn.lua || '');
    const luaPath = JSON.stringify(f.replace(/\\/g, '/'));
    try {
      const err = execFileSync(LUA, ['-e',
        `local fn, e = loadfile(${luaPath}); if not fn then io.write(tostring(e)) end`],
        { encoding: 'utf8' }).trim();
      if (err) bad.push(`'${sn.title}' lua: ${err}`);
    } catch (e) { bad.push(`'${sn.title}' lua crashed: ${e.message}`); }

    // The JS form goes through the same parser the browser will use.
    try { new Function(sn.js || ''); }
    catch (e) { bad.push(`'${sn.title}' js: ${e.message}`); }
  }
  assert(`all ${SNIPPETS.length} snippets are valid Lua and valid JS`
    + (bad.length ? ` -- ${bad.join(' | ')}` : ''), bad.length === 0);
}

// ---------------------------------------------------------------------------------
// A CUSTOM BLOCK WITH AN EMPTY BOX MUST NOT GENERATE A WORKING-LOOKING LINE.
//
// renderTemplate leaves an unfilled {name} in the output on purpose, so a broken template
// looks broken. It does not look broken: `{on}` is a Lua table constructor, the line parses,
// and a table is TRUE -- so an empty yes/no box turned the flag ON. This runs the real
// interpreter over the claim rather than restating it, then checks the generated script.
//
// Registered last, because registerCustom replaces the whole custom list and the sweeps
// above enumerate it.
// ---------------------------------------------------------------------------------
if (LUA) {
  const truthy = execFileSync(LUA, ['-e', `local on = {on} io.write(type(on), '/', tostring(not not on))`],
    { encoding: 'utf8' }).trim();
  assert(`an unfilled {placeholder} is a truthy Lua table, not an error (got ${truthy})`, truthy === 'table/true');

  const camera = {
    id: 'cam', kind: 'action', label: 'Camera collision',
    lua: 'sam_set_camera_collision(player, {on})',
    params: [{ name: 'on', type: 'text', default: '' }],
  };
  const near = {
    id: 'near', kind: 'condition', label: 'Camera is free',
    lua: 'sam_get_camera({who}) ~= nil',
    params: [{ name: 'who', type: 'text', default: '' }],
  };
  registerCustom([
    { kind: 'action', entry: toCatalogEntry(camera) },
    { kind: 'condition', entry: toCatalogEntry(near) },
  ]);

  const blank = generateLua({ rules: [rule('player.on_hit', [act('custom:cam', { on: '' })])] });
  assert('a blank custom-block box never reaches the generated script', !blank.includes('{on}'));
  assert('...and the script says which box is empty', blank.includes('"on"'));
  assert('...and does not call the function at all', !blank.includes('sam_set_camera_collision(player,'));
  assert('...and the row warns, naming the box',
    /Fill in "on"/.test(toCatalogEntry(camera).warn({})));

  const filled = generateLua({ rules: [rule('player.on_hit', [act('custom:cam', { on: 'true' })])] });
  assert('a FILLED custom-block box still generates the call',
    filled.includes('sam_set_camera_collision(player, true)'));
  assert('...and a filled block does not warn', toCatalogEntry(camera).warn({ on: 'true' }) === '');

  // Both refusals have to still LOAD: a condition sits inside `if <expr> then`, so it cannot
  // be a comment, and a script that does not parse loads nothing at all.
  const bothBlank = generateLua({
    rules: [{
      key: 'b', trigger: { id: 'player.on_hit', params: {} },
      conditions: [act('custom:near', { who: '' })],
      actions: [act('custom:cam', { on: '' })],
    }],
  });
  const f = join(TMP, 'custom.lua');
  writeFileSync(f, bothBlank);
  const luaPath = JSON.stringify(f.split('\\').join('/'));
  const perr = execFileSync(LUA, ['-e', `local fn, e = loadfile(${luaPath}); if not fn then io.write(tostring(e)) end`],
    { encoding: 'utf8' }).trim();
  assert(`a refused custom condition and action still parse${perr ? ` -- ${perr}` : ''}`, perr === '');
  assert('a refused custom condition reads as false, not as a truthy table', bothBlank.includes('if false '));
}

// ---------------------------------------------------------------------------------
if (!LUA) {
  console.log('SKIP: no lua interpreter found (set SAM_LUA=/path/to/lua.exe).');
  console.log('      Build one from framework/lua54 — it is the same 5.4.7 the game runs.');
  process.exit(0);
}
for (const f of failures) {
  console.log(`\nFAIL: ${f.name}\n  ${f.why}\n--- generated ---\n${f.lua}`);
}
console.log(`\n${pass} passed, ${fail} failed  (lua: ${LUA})`);
process.exit(fail ? 1 : 0);
