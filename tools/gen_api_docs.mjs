// Generate the public API reference and the TypeScript definitions from samApi.js,
// and fail loudly if samApi.js has drifted from the two runtimes or from the multiplayer
// contracts.
//
// WHY THIS EXISTS. samApi.js has claimed "Auto-generated from sam_lua_runtime.cpp /
// sam_js_runtime.cpp" since it was written, and nothing generated it. By v2.5.0 the drift
// had reached 136 of 184 shipped functions missing from the public reference and 16 missing
// from the Mod Builder's blocks, which meant the newest two releases were unreachable in the
// visual builder and most of the API was invisible to anyone reading the docs. Functions
// nobody can find are not features.
//
// The model is deliberately NOT "generate samApi.js from the C++": the descriptions are the
// valuable part and a machine cannot write them. samApi.js stays hand-maintained and is the
// single source of truth; this tool checks it against the runtimes and derives every other
// surface from it.
//
// MULTIPLAYER. Every function is registered through samLuaRegister / samJsRegister, which wrap
// it in the trampoline that applies its contract from framework/sam_mp_contracts.inc (refuse on
// the wrong machine, carry the call to the right one). A function registered any other way would
// silently skip all of that, and a function with no contract line runs unchecked on every
// machine, so this tool refuses both, and refuses a samApi.js `mp` that disagrees with the
// contract: the kind a modder reads must be the kind the game enforces.
//
//   node tools/gen_api_docs.mjs          write the outputs
//   node tools/gen_api_docs.mjs --check  verify only, non-zero exit on drift (for the ship gate)

import { existsSync, readFileSync, readdirSync, writeFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join, sep } from 'node:path'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..')
const CHECK_ONLY = process.argv.includes('--check')

const FW = join(ROOT, 'framework')
const LUA_SRC = join(FW, 'sam_lua_runtime.cpp')
const JS_SRC = join(FW, 'sam_js_runtime.cpp')
const CONTRACTS = join(FW, 'sam_mp_contracts.inc')
const OUT_MD = join(ROOT, 'docs', 'function-reference.md')
const OUT_DTS = join(ROOT, 'gui', 'public', 'sam.d.ts')

// Every copy of these two files that ships, not just the two canonical ones.
//
// This tool wrote OUT_MD and OUT_DTS and stopped, and there are SIX copies in the tree.
// The one sync_workshop.sh reads as its SOURCE (../workshop_upload) was not written, so the
// Workshop payload went out advertising 184 functions while samApi.js declared 245: exactly
// the drift this tool exists to prevent, reintroduced one directory over. gui/dist is a
// build output, but it is the copy GitHub Pages actually serves, so a stale one is a stale
// public download until someone remembers to rebuild.
//
// THEN IT HAPPENED AGAIN, one directory the OTHER way. There are two workshop_upload folders:
// the sync SOURCE at REPO (the workflow folder, untracked) and the TRACKED copy at ROOT
// (SAM-Framework/workshop_upload), which is what anyone browsing the GitHub repo downloads.
// Only the first was listed here, so a TypeScript author taking sam.d.ts off GitHub got the
// previous release's file, with none of the new functions, and --check reported "ok all 5
// generated file(s) match" because it never looked at the sixth. sync_workshop.sh does copy
// source over tracked, so a ship that runs it is fine -- but nothing forces that, and a gate
// that only passes when a separate script is remembered is not a gate. Both are listed now.
//
// Each entry is skipped when its directory is absent, so a checkout of SAM-Framework on its
// own still works.
const REPO = join(ROOT, '..')
const EXTRA = [
  [join(REPO, 'workshop_upload', 'function-reference.md'), 'md', 'sync source read by sync_workshop.sh'],
  [join(REPO, 'workshop_upload', 'sam.d.ts'), 'dts', 'sync source read by sync_workshop.sh'],
  [join(ROOT, 'workshop_upload', 'function-reference.md'), 'md', 'the tracked copy people download from the repo'],
  [join(ROOT, 'workshop_upload', 'sam.d.ts'), 'dts', 'the tracked copy people download from the repo'],
  [join(ROOT, 'gui', 'dist', 'sam.d.ts'), 'dts', 'the copy Pages serves'],
]

const api = await import('file://' + join(ROOT, 'gui', 'src', 'data', 'samApi.js').replace(/\\/g, '/'))
const FUNCS = api.SAM_FUNCTIONS
const EVENTS = api.SAM_EVENTS

const problems = []

// ---------------------------------------------------------------- registration
const luaText = readFileSync(LUA_SRC, 'utf8')
const jsText = readFileSync(JS_SRC, 'utf8')
const names = (text, re) => {
  const out = new Set()
  for (const m of text.matchAll(re)) out.add(m[1])
  return out
}
// The only registration a function may have: through the trampoline. `\w+` for the state /
// context / global so a renamed local still counts, but the call itself must be this one.
const lua = names(luaText, /samLuaRegister\(\s*\w+\s*,\s*"(sam_[A-Za-z0-9_]+)"/g)
const js = names(jsText, /samJsRegister\(\s*\w+\s*,\s*\w+\s*,\s*"(sam_[A-Za-z0-9_]+)"/g)

// A RAW registration skips the contract entirely: no refusal on a client, no carrying the
// call to the player's machine, no stripping of the optional player. It would work in
// singleplayer and be quietly wrong in co-op, which is exactly the class of bug the
// multiplayer overhaul exists to end. A commented-out line does not count.
const RAW = [
  [LUA_SRC, luaText, /lua_setglobal\(\s*\w+\s*,\s*"sam_[A-Za-z0-9_]*"/g],
  [LUA_SRC, luaText, /lua_register\(\s*\w+\s*,\s*"sam_[A-Za-z0-9_]*"/g],
  [JS_SRC, jsText, /JS_SetPropertyStr\(\s*\w+\s*,\s*[^,"]+?,\s*"sam_[A-Za-z0-9_]*"/g],
  [JS_SRC, jsText, /JS_DefinePropertyValueStr\(\s*\w+\s*,\s*[^,"]+?,\s*"sam_[A-Za-z0-9_]*"/g],
]
for (const [file, text, re] of RAW) {
  for (const m of text.matchAll(re)) {
    const lineStart = text.lastIndexOf('\n', m.index) + 1
    if (text.slice(lineStart, m.index).includes('//')) continue
    const line = text.slice(0, m.index).split('\n').length
    problems.push(`raw registration bypasses the multiplayer trampoline (use samLuaRegister / samJsRegister): ${file.replace(ROOT + sep, '')}:${line}  ${m[0]}`)
  }
}

const declared = new Set(FUNCS.map(f => f.name))

// A duplicate entry renders twice in the reference and silently shadows the better of the
// two in the Mod Builder. This caught a real one the first time it ran.
const dupes = (list, label) => {
  const seen = new Map()
  for (const x of list) seen.set(x.name, (seen.get(x.name) || 0) + 1)
  for (const [n, c] of seen) if (c > 1) problems.push(`${label} declared ${c} times: ${n}`)
}
const diff = (a, b, msg) => [...a].filter(x => !b.has(x)).sort().forEach(x => problems.push(`${msg}: ${x}`))

// Lua/JS parity is non-negotiable in this project: a function in one runtime and not the
// other is a mod that works in Lua and silently fails in JS, or the reverse.
diff(lua, js, 'registered in Lua but NOT in JS')
diff(js, lua, 'registered in JS but NOT in Lua')
diff(lua, declared, 'shipped but MISSING from samApi.js (invisible to docs and the Mod Builder)')
diff(declared, lua, 'declared in samApi.js but NOT registered in the runtime (would 404 for a modder)')

// ---------------------------------------------------------------- contracts
// SAM_MP(function, kind, target, argument, refusal), one per line; see the file's header.
const KINDS = ['Host', 'Owner', 'Screen', 'Read', 'All', 'Local', 'Any']
const TARGETS = ['None', 'Player', 'Uid', 'Item', 'OptPlayer']
const REFUSALS = ['False', 'Nil', 'Zero', 'Empty', 'None']
const contract = new Map()
{
  const text = readFileSync(CONTRACTS, 'utf8')
  text.split('\n').forEach((raw, i) => {
    const line = raw.replace(/\/\/.*$/, '').trim()
    if (!line) return
    const m = line.match(/^SAM_MP\(\s*(sam_[A-Za-z0-9_]+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*(\w+)\s*\)$/)
    if (!m) { problems.push(`sam_mp_contracts.inc:${i + 1} is not a SAM_MP(...) line: ${line}`); return }
    const [, name, kind, target, arg, refusal] = m
    if (contract.has(name)) problems.push(`contract declared twice: ${name} (sam_mp_contracts.inc:${i + 1})`)
    if (!KINDS.includes(kind)) problems.push(`contract for ${name} has an unknown kind "${kind}" (one of ${KINDS.join(', ')})`)
    if (!TARGETS.includes(target)) problems.push(`contract for ${name} has an unknown target "${target}" (one of ${TARGETS.join(', ')})`)
    if (!REFUSALS.includes(refusal)) problems.push(`contract for ${name} has an unknown refusal "${refusal}" (one of ${REFUSALS.join(', ')})`)
    if ((target === 'None') !== (+arg === 0)) problems.push(`contract for ${name}: target ${target} with argument ${arg} (None takes 0, every other target a 1-based position)`)
    contract.set(name, { kind, target, arg: +arg, refusal })
  })
}
const registered = new Set([...lua, ...js])
diff(registered, new Set(contract.keys()), 'registered but has NO multiplayer contract in sam_mp_contracts.inc (it would run unchecked on every machine)')
diff(new Set(contract.keys()), registered, 'sam_mp_contracts.inc names a function that is not registered')

// samApi.js must say what the contract enforces, or the reference and the Mod Builder would
// tell a modder one thing while the game does another.
const HOSTISH = new Set(['host', 'owner', 'all'])
for (const f of FUNCS) {
  const c = contract.get(f.name)
  if (!c) continue
  const want = c.kind.toLowerCase()
  if (f.mp !== want) problems.push(`samApi.js mp for ${f.name} is ${JSON.stringify(f.mp)} but its contract kind is ${c.kind} (mp: "${want}")`)
  if (!!f.hostOnly !== HOSTISH.has(want)) problems.push(`samApi.js hostOnly for ${f.name} is ${!!f.hostOnly} but mp "${want}" means ${HOSTISH.has(want)} (true exactly for host, owner and all)`)
  // The argument the trampoline reads must be the one the reference shows there, and a player
  // argument must be called player, so a reader can find which argument names the machine.
  if (c.arg > 0) {
    const p = (f.params || [])[c.arg - 1]
    if (!p) problems.push(`contract for ${f.name} reads argument ${c.arg}, but samApi.js lists only ${(f.params || []).length}`)
    else if ((c.target === 'Player' || c.target === 'OptPlayer') && p.name !== 'player') problems.push(`contract for ${f.name} reads argument ${c.arg} as a player, but samApi.js calls it "${p.name}"`)
    if (c.target === 'OptPlayer') {
      if (p && !samOptional(p)) problems.push(`${f.name}: the trailing player (OptPlayer) must be marked optional in samApi.js`)
      if ((f.params || []).length !== c.arg) problems.push(`${f.name}: OptPlayer is the LAST argument (position ${c.arg}), but samApi.js lists ${(f.params || []).length}`)
    }
  }
}

// Events fired by the engine must appear in the catalog for the same reason. Two things this
// scan used to be blind to, and both are the drift the tool exists to stop:
//
//   * It required a NAMESPACE (player./world./game./...), so it never saw one of the 14
//     bare-named events -- on_tick, on_packet, on_key_pressed, on_action_pressed,
//     on_before_damage, on_monster_died and the rest, the family this multiplayer work is
//     full of. A new bare-named event could reach the game and miss samApi.js in silence.
//   * It read framework/*.cpp only, and said engine names "are checked by the shell gate,
//     which can walk the Barony tree". No such gate exists: sync_workshop.sh copies files
//     and compares hashes and counts. Meanwhile ../Barony/src fires 53 of these names.
//
// So: find the FIRE SITES rather than any string that looks like an event name. A site is a
// SamEvent construction, a setName(), a SAMNet::sendEventToHost() or an AllowEvent -- which
// also takes the ternary form `SamEvent ev(down ? "on_key_pressed" : "on_key_released")` --
// and only string literals shaped like an event name are taken from it. That is precise
// enough to have no false positives across both trees, where a plain "looks like on_*" scan
// picked up JSON field names (on_hit_effect, on_degraded, on_use) and doc comments.
//
// The Barony tree is scanned only when it is there, like EXTRA above, so a checkout of
// SAM-Framework on its own still works. The tool stays dependency-free either way.
const EV_SITE = /(?:SamEvent\s+\w+\s*\(|\.setName\s*\(|sendEventToHost\s*\(|AllowEvent\s+\w+\s*\()([^;{}]*)/g
const EV_NAME = /"((?:[a-z]+\.)?on_[a-z_]+)"/g
const evFired = new Set()
const scanEvents = (dir) => {
  for (const ent of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, ent.name)
    if (ent.isDirectory()) { scanEvents(p); continue }
    if (!/\.(cpp|hpp|inc)$/.test(ent.name)) continue
    const text = readFileSync(p, 'utf8')
    for (const m of text.matchAll(EV_SITE)) {
      // A commented-out line does not count, the same rule the raw-registration scan uses:
      // both headers carry `SamEvent e("player.on_x")` in their explanatory comments.
      const lineStart = text.lastIndexOf('\n', m.index) + 1
      if (text.slice(lineStart, m.index).includes('//')) continue
      for (const n of m[1].matchAll(EV_NAME)) evFired.add(n[1])
    }
  }
}
scanEvents(FW)
const ENGINE_SRC = join(REPO, 'Barony', 'src')
if (existsSync(ENGINE_SRC)) scanEvents(ENGINE_SRC)
else console.warn('note: ../Barony/src not found beside the repo, skipped the engine-side event scan')
const evDeclared = new Set(EVENTS.map(e => e.name))
diff(evFired, evDeclared, 'event fired but MISSING from samApi.js')
dupes(FUNCS, 'function')
dupes(EVENTS, 'event')
// Which machine an event fires on is the first thing a co-op mod needs to know about it.
for (const e of EVENTS) if (!e.fires || !String(e.fires).trim()) problems.push(`event without fires (which machine it fires on, and for whom): ${e.name}`)

if (problems.length) {
  console.error('API DRIFT (' + problems.length + '):')
  for (const p of problems) console.error('  ' + p)
  process.exit(1)
}
console.log(`ok  ${lua.size} functions and ${EVENTS.length} events agree across both runtimes and samApi.js`)
console.log(`ok  every function registers through the trampoline and has a contract; samApi.js mp matches all ${contract.size}; every event says where it fires`)
// NOT an early exit any more. --check used to stop here, which meant the ship gate never
// reached the TypeScript parse or the parameter-type check below: the only two checks that
// look at what mod authors actually receive. It now runs everything and skips the writes.

// ---------------------------------------------------------------- multiplayer text
// One sentence per kind, built from the contract itself, so the reference cannot describe a
// function's multiplayer behaviour differently from what the trampoline enforces. The
// function's own mpNote (samApi.js) follows it.
const REFUSED_AS = {
  False: 'false', Nil: 'nil (undefined in JavaScript)', Zero: '0',
  Empty: 'an empty table (an empty array in JavaScript)',
  // Not just "nothing": a modder has to know which nothing. Lua gets no value at all (so
  // `tostring()` on it raises rather than printing "0"), JavaScript gets `undefined`, which is
  // the one value that compares false in every relational operator and is !== 0.
  None: 'nothing at all -- no value in Lua, `undefined` in JavaScript',
}
function samOptional(p) { return !!(p.optional || /optional/i.test(p.type || '')) }
function mpText(f) {
  const c = contract.get(f.name)
  const refused = REFUSED_AS[c.refusal]
  const p = c.arg > 0 ? (f.params || [])[c.arg - 1] : null
  const arg = p ? '`' + p.name + '`' : ''
  const optional = p && samOptional(p)
  let s
  switch (c.kind) {
    case 'Host':
      s = `\`host\`. Runs on the host (and in singleplayer). A client's call is refused with a one-time warning and returns ${refused}.`
      break
    case 'Owner':
      s = `\`owner\`. The state lives on the player's own machine: on the host, a call about a player on another machine (named by ${arg}) is carried to that machine and done there, and returns true once it is sent. A player whose game does not run S.A.M is refused with a warning. A client's call is refused with a one-time warning and returns ${refused}.`
      break
    case 'Screen':
      s = c.target === 'OptPlayer'
        ? '`screen`. Shows on one player\'s own screen, picked by the optional last argument `player`: left out, the player the current event is about, else this machine\'s own; -1 is every player.'
        : `\`screen\`. Shows on the screen of the player ${arg} names${optional ? ' (left out: the player the current event is about, else this machine\'s own)' : ''}; -1 is every player in a multiplayer game.`
      s += ' On the host, a call for a player on another machine is carried there and returns true once it is sent; a player whose game does not run S.A.M is refused with a warning. A client can show things only on its own screen.'
      break
    case 'Read':
      if (c.target === 'Uid') s = `\`read\`. The host can read every creature; a client can read only its own player's uid (${arg}), and anything else is refused with a one-time warning and returns ${refused}.`
      else if (c.target === 'Item') s = `\`read\`. Answers for the items this machine can see. On the host that includes every player's: a remote player's items (the uids sam_get_inventory gives the host) are read from the copy their game reports. A client sees only its own.`
      else s = `\`read\`. The host can read every player; a client can read only its own player (${arg}), and asking about another is refused with a one-time warning and returns ${refused}.${optional ? ` Left out, ${arg} is the player the current event is about, else this machine's own.` : ''}`
      break
    case 'All':
      s = `\`all\`. Changes a table every machine keeps its own copy of: a host call runs on the host and on every S.A.M client, and is replayed to a client that joins later. A client's call is refused with a one-time warning and returns ${refused}.`
      break
    case 'Local':
      s = '`local`. Answers for the machine running the script.'
      if (c.target === 'Player' || c.target === 'OptPlayer') s += ` Given a player on another machine (${arg}), it is refused with a one-time warning and returns ${refused}.`
      if (c.target === 'Uid') s += ' Each machine answers from its own copy.'
      break
    case 'Any':
      s = '`any`. The same answer on every machine; safe to call anywhere, including a client\'s on_packet handler.'
      break
  }
  return f.mpNote ? s + ' ' + f.mpNote : s
}
const evMpText = e => 'Fires ' + String(e.fires).trim()

// ---------------------------------------------------------------- the reference
// samApi.js grew case-drifted category names over many releases ("combat" and "Combat",
// "world" and "World", "player" and "Player state"), which would render as duplicate
// sections in the contents. Canonicalise for display rather than rewriting 184 entries.
const CANON = {
  combat: 'Combat', world: 'World', input: 'Input', player: 'Player state',
  lifecycle: 'Lifecycle', tick: 'Lifecycle', custom: 'Custom events',
}
const catOf = f => CANON[f.category] || f.category || 'Other'

const byCat = new Map()
for (const f of FUNCS) {
  const c = catOf(f)
  if (!byCat.has(c)) byCat.set(c, [])
  byCat.get(c).push(f)
}
// `optional: true` is what samApi.js actually marks a parameter with, and the markdown ignored it
// exactly as the .d.ts line did -- so the reference on GitHub, the page a mod author actually reads,
// showed `sam_apply_force(uid, force, angle, ticks)` with no hint that ticks can be left out.
// Brackets are the convention a reader already knows from any CLI or man page, so it needs no legend.
const sig = f => `${f.name}(${(f.params || []).map(p => samOptional(p) ? `[${p.name}]` : p.name).join(', ')})`

const kindCount = k => FUNCS.filter(f => f.mp === k).length
let md = `# S.A.M function reference

Every script function the framework exposes: **${FUNCS.length} functions** and **${EVENTS.length} events**.
All of them work identically in Lua, JavaScript and TypeScript.

This page is generated from the API definition, so it cannot fall behind the code. If a
function is missing here it is missing from the framework.

## Multiplayer

Your script's runtime code (its events, on_tick and timers) runs on the **host**. You name
players by index, and S.A.M carries each call to the machine it has to run on. Every function
below ends with a **Multiplayer:** line that starts with one of seven kinds. The game enforces
the kind, so the line is always what really happens:

| kind | what it means for your script |
|---|---|
| \`host\` (${kindCount('host')}) | Runs on the host, where your events and timers already run. A client's call is refused with a one-time warning. The result reaches every player. |
| \`owner\` (${kindCount('owner')}) | Changes something that lives on the player's own machine (their backpack, their spells). Call it on the host; S.A.M carries it to that player's machine. |
| \`screen\` (${kindCount('screen')}) | Shows something on one player's screen. Name the player, or leave it out inside an event about a player; -1 means every player. S.A.M carries it to their machine. |
| \`read\` (${kindCount('read')}) | Reads a player. The host can read everyone; a client can read only its own player. |
| \`all\` (${kindCount('all')}) | Changes a table every machine keeps (class and item patches, species resists). A host call runs everywhere and reaches players who join later. |
| \`local\` (${kindCount('local')}) | Answers for the machine running it: its clock, its files, its music. |
| \`any\` (${kindCount('any')}) | The same answer on every machine. Safe anywhere. |

Every event ends with a **Multiplayer:** line that says which machine it fires on and for
whom. The whole model, with examples, and how to test co-op on one computer:
[multiplayer.md](multiplayer.md).

For guides and worked examples, see [scripting-reference.md](scripting-reference.md).

## Contents

${[...byCat.keys()].sort().map(c => `- [${c}](#${c.toLowerCase().replace(/[^a-z0-9]+/g, '-')}) (${byCat.get(c).length})`).join('\n')}
- [Events](#events) (${EVENTS.length})

`

for (const cat of [...byCat.keys()].sort()) {
  md += `\n## ${cat}\n\n`
  for (const f of byCat.get(cat).sort((a, b) => a.name.localeCompare(b.name))) {
    md += `### \`${sig(f)}\`\n\n`
    md += `${f.desc || ''}\n\n`
    // A gotcha is written in samApi.js for a reader, and this page never printed it: the one
    // sentence that says what will surprise you was invisible everywhere but the source.
    if (f.gotcha) md += `${f.gotcha}\n\n`
    if ((f.params || []).length) {
      md += `| argument | type |\n|---|---|\n`
      for (const p of f.params) md += `| \`${p.name}\`${samOptional(p) ? ' *(optional)*' : ''} | ${p.type}${p.values ? ` — one of: ${p.values.map(v => `\`${v}\``).join(', ')}` : ''} |\n`
      md += `\n`
    }
    md += `**Returns:** ${f.returns || 'nothing'}\n\n`
    md += `**Multiplayer:** ${mpText(f)}\n\n`
  }
}

md += `\n## Events\n\nHandle these in \`on_event(e)\`. Every script receives every event; check \`e.name\`.\nMost events fire only on the host, which is where your script's runtime code runs; the\n**Multiplayer:** line under each says where and for whom.\n\n`
for (const e of [...EVENTS].sort((a, b) => a.name.localeCompare(b.name))) {
  md += `### \`${e.name}\`\n\n`
  if (e.cancellable) md += `> Cancellable: return \`false\` to stop it.\n\n`
  md += `Fires ${e.whenFired || 'during play'}.\n\n`
  if ((e.payload || []).length) {
    md += `| field | type |\n|---|---|\n`
    for (const p of e.payload) md += `| \`${p.field}\` | ${p.type} |\n`
    md += `\n`
  }
  if (e.notes) md += `${e.notes}\n\n`
  if (e.gotcha) md += `${e.gotcha}\n\n`
  md += `**Multiplayer:** ${evMpText(e)}\n\n`
}

// ---------------------------------------------------------------- the .d.ts
// Every spelling a `params` type may start with, and what it becomes in TypeScript.
//
// This used to end in `return 'string'`, so anything unrecognised became a string parameter
// without a word: "uid" on five functions (sam_cast_spell_at among them), "colour (optional)"
// on four UI functions whose documented value is the number 0xRRGGBBAA, "any" on fifteen, and
// "function(uid)" on sam_register_behavior. A TypeScript mod written from the documentation
// would not compile against a single one of them.
const TS_TYPES = [
  // "int|string" first: a sound or model argument that takes either an index or a "ns:name".
  // The rule below used to claim it by its first word, so TypeScript rejected "mymod:boom".
  [/^(int|number)\s*\|\s*string|^string\s*\|\s*(int|number)/, 'number | string'],
  [/^(int|number|float|uid|colour|color)/, 'number'],
  [/^bool/, 'boolean'],
  [/^(table|object|array|any)/, 'any'],
  [/^function/, '(...args: any[]) => any'],
  [/^string/, 'string'],
]
// A spelling nobody anticipated now stops the tool instead of quietly becoming a string.
const tsUnknown = new Set()
const tsType = t => {
  const s = String(t || '').toLowerCase()
  for (const [re, out] of TS_TYPES) { if (re.test(s)) return out }
  tsUnknown.add(s)
  return 'any'
}
// An entry may state its TypeScript return type outright with `ts:`, and that always
// wins. Without it this guesses from the English prose, which is fine for a plain number
// or boolean and wrong for anything shaped: five functions that return arrays or objects
// were typed `number` and `boolean`, so correct TypeScript failed to compile and incorrect
// TypeScript compiled silently. Prose is not a type system; `ts:` is the escape hatch.
// "or nil" in a `returns` string is not prose: the binding really pushes nil there, and a
// definition that promises `number` lets `sam_get_item_value(uid) + 1` compile against a value
// that is not a number at runtime. The JavaScript side of every one of these answers
// `undefined`, never `null` -- `null < 10` is TRUE, so a refused numeric read used to take the
// branch it was meant to fail, while `undefined < 10` is false. So the widening is
// `| undefined` throughout, appended to whatever the base type works out to.
// A contract whose refusal is None gives a refused call NOTHING back: no value in Lua,
// `undefined` in JavaScript. That is a value a TypeScript mod really receives -- only in a
// client-side handler, but `sam_get_stat(e.who, "HP") <= 0` in one is the exact mistake the
// refusal was changed to catch -- and until the declaration says so, strict null checks say
// nothing about it. It is the same widening, so a return that is both only gets it once.
// Widened only where there is a value to widen: `void` has none and `any` already includes it.
const tsRet = (r, explicit, c) => {
  let t = explicit
  if (!t) {
    const base = tsRetBase(r)
    const s = (r || '').toLowerCase()
    t = (base !== 'void' && base !== 'any' && /\b(nil|null)\b/.test(s)) ? base + ' | undefined' : base
  }
  if (c && c.refusal === 'None' && t !== 'void' && t !== 'any' && !/\bundefined\b/.test(t)) t += ' | undefined'
  return t
}
const tsRetBase = (r) => {
  const s = String(r || '').toLowerCase()
  if (!s || s === 'nothing') return 'void'
  // Shaped returns are tested BEFORE boolean: "a table/object with ... (booleans)" is an
  // object, and the old order matched the word "boolean" inside it first.
  if (s.includes('array') || s.includes('table') || s.includes('object')) return 'any'
  if (s.includes('boolean')) return 'boolean'
  if (s.includes('number') || s.includes('int')) return 'number'
  if (s.includes('string')) return 'string'
  return 'any'
}

// A parameter named `class`, `function`, `new` and so on is legal in Lua and in samApi.js
// and is a syntax error in a .d.ts. The generated file is validated against the TypeScript
// compiler the framework itself ships, which is how these were found. Suffix rather than
// rename, so the identifier still reads as the documented argument.
const TS_RESERVED = new Set(['break', 'case', 'catch', 'class', 'const', 'continue', 'debugger',
  'default', 'delete', 'do', 'else', 'enum', 'export', 'extends', 'false', 'finally', 'for',
  'function', 'if', 'import', 'in', 'instanceof', 'new', 'null', 'return', 'super', 'switch',
  'this', 'throw', 'true', 'try', 'typeof', 'var', 'void', 'while', 'with', 'yield',
  'implements', 'interface', 'let', 'package', 'private', 'protected', 'public', 'static'])
const safeParam = n => {
  let id = String(n).replace(/[^A-Za-z0-9_]/g, '_').replace(/^([0-9])/, '_$1')
  if (!id) id = 'arg'
  return TS_RESERVED.has(id) ? id + '_' : id
}
// Text inside a /** */ block: a stray "*/" would end the comment early.
const jsdoc = s => String(s || '').replace(/\*\//g, '*\\/').replace(/`/g, '')

let dts = `// TypeScript definitions for the S.A.M Framework scripting API.
// Generated from the API definition; do not edit by hand.
//
// Drop this beside your mod's .ts files, or reference it:
//   /// <reference path="sam.d.ts" />
//
// ${FUNCS.length} functions, ${EVENTS.length} events. Each function's "Multiplayer:" line starts with
// its kind (host, owner, screen, read, all, local, any); see docs/multiplayer.md.

declare global {
`
for (const f of FUNCS.slice().sort((a, b) => a.name.localeCompare(b.name))) {
  dts += `  /**\n   * ${jsdoc(f.desc)}\n   *\n   * Multiplayer: ${jsdoc(mpText(f))}\n   */\n`
  // `optional: true` is the property samApi.js actually uses, and this line only ever regex-tested
  // the TYPE STRING for the word "optional" -- so a parameter marked properly came out REQUIRED in
  // the .d.ts and a TypeScript mod written from the docs would not compile. sam_set_on_fire escaped
  // only because its type string happens to contain the word. The --check gate cannot see this: the
  // file it produces parses perfectly and is simply wrong about what you may leave out.
  dts += `  function ${f.name}(${(f.params || []).map(p => `${safeParam(p.name)}${samOptional(p) ? '?' : ''}: ${tsType(p.type)}`).join(', ')}): ${tsRet(f.returns, f.ts, contract.get(f.name))};\n\n`
}
dts += `  /** Every event name the engine fires. */\n  type SamEventName =\n`
for (const e of [...EVENTS].sort((a, b) => a.name.localeCompare(b.name))) {
  dts += `    /** Fires ${jsdoc(e.whenFired || 'during play')}. Multiplayer: ${jsdoc(evMpText(e))} */\n    | ${JSON.stringify(e.name)}\n`
}
dts = dts.replace(/\n$/, ';\n\n')
dts += `  interface SamEvent {\n    name: SamEventName;\n    [field: string]: any;\n  }\n`
dts += `}\n\nexport {};\n`

// Validate the declarations with the very compiler the framework ships to mod authors, so a
// broken .d.ts can never reach anyone. The first run of this found 23 errors: parameters
// named after TypeScript keywords. Skipped with a warning if the bundle is not beside us,
// because the generator must still work from a bare checkout.
const TS_BUNDLE = join(ROOT, '..', 'workshop_upload', 'typescript.js')
try {
  const { createRequire } = await import('node:module')
  const ts = createRequire(import.meta.url)(TS_BUNDLE)
  const sf = ts.createSourceFile('sam.d.ts', dts, ts.ScriptTarget.ES2020, true)
  const errs = sf.parseDiagnostics || []
  if (errs.length) {
    console.error(`sam.d.ts FAILED to parse under TypeScript ${ts.version} (${errs.length} errors):`)
    for (const e of errs.slice(0, 10)) console.error('  ' + ts.flattenDiagnosticMessageText(e.messageText, ' '))
    process.exit(1)
  }
  console.log(`ok  sam.d.ts parses cleanly under the shipped TypeScript ${ts.version}`)
} catch (e) {
  console.warn('note: typescript.js not found beside the repo, skipped .d.ts validation')
}

if (tsUnknown.size) {
  console.error(`samApi.js uses ${tsUnknown.size} parameter type spelling(s) this tool does not`
    + ' recognise, so it cannot choose a TypeScript type and would have silently used `any`:')
  for (const t of [...tsUnknown].sort()) console.error(`  "${t}"`)
  console.error('Add the spelling to TS_TYPES in this file, or use one that is already there.')
  process.exit(1)
}
console.log('ok  every parameter type maps to a real TypeScript type')

// Every output this tool writes, with the text it should hold right now.
const OUTPUTS = [
  [OUT_MD, md, 'the reference on GitHub'],
  [OUT_DTS, dts, 'the definitions the Mod Builder hands out'],
  ...EXTRA.map(([p, kind, why]) => [p, kind === 'md' ? md : dts, why]),
]

if (CHECK_ONLY) {
  // THE CHECK THAT WAS MISSING. --check used to verify samApi.js against the runtimes and
  // then exit 0 without ever looking at the files it writes -- so the ship gate could pass
  // green while docs/function-reference.md, gui/public/sam.d.ts, the two workshop_upload
  // copies and the gui/dist one were all from an earlier release. That is the exact drift
  // this tool exists to prevent, and it shipped once already (the header above): a Workshop
  // payload advertising 184 functions against a samApi.js declaring 245.
  //
  // Comparing the generated text with what is on disk costs nothing and cannot be fooled.
  const stale = []
  for (const [p, want, why] of OUTPUTS) {
    if (!existsSync(dirname(p))) continue      // a bare checkout; a real run skips it too
    const where = p.replace(REPO + sep, '')
    if (!existsSync(p)) { stale.push(`${where} does not exist (${why})`); continue }
    if (readFileSync(p, 'utf8') !== want) { stale.push(`${where} is STALE (${why})`) }
  }
  if (stale.length) {
    console.error(`GENERATED FILES OUT OF DATE (${stale.length}):`)
    for (const s of stale) console.error('  ' + s)
    console.error('Run: node tools/gen_api_docs.mjs   (no --check) to bring them back in step.')
    process.exit(1)
  }
  console.log(`ok  all ${OUTPUTS.filter(([p]) => existsSync(p)).length} generated file(s) match what this run would write`)
  process.exit(0)
}

writeFileSync(OUT_MD, md, 'utf8')
writeFileSync(OUT_DTS, dts, 'utf8')
for (const [p, kind, why] of EXTRA) {
  if (!existsSync(dirname(p))) continue
  writeFileSync(p, kind === 'md' ? md : dts, 'utf8')
  console.log(`  also wrote ${p.replace(REPO + sep, '')}  (${why})`)
}
console.log(`wrote docs/function-reference.md (${md.split('\n').length} lines)`)
console.log(`wrote gui/public/sam.d.ts (${dts.split('\n').length} lines)`)
