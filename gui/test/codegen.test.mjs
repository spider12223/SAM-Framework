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
import { untilCandidates, findAction, ENGINE_WRITTEN_STATS } from '../src/data/blocks.js';

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
