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
import { untilCandidates, findAction, ENGINE_WRITTEN_STATS, conditionsFor, findTrigger, findCondition } from '../src/data/blocks.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const TMP = join(HERE, '.tmp');
mkdirSync(TMP, { recursive: true });

function findLua() {
  const candidates = [process.env.SAM_LUA, 'lua', 'lua5.4', 'lua54'].filter(Boolean);
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
