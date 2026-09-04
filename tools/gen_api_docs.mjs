// Generate the public API reference and the TypeScript definitions from samApi.js,
// and fail loudly if samApi.js has drifted from the two runtimes.
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
//   node tools/gen_api_docs.mjs          write the outputs
//   node tools/gen_api_docs.mjs --check  verify only, non-zero exit on drift (for the ship gate)

import { readFileSync, writeFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..')
const CHECK_ONLY = process.argv.includes('--check')

const LUA_SRC = join(ROOT, 'framework', 'sam_lua_runtime.cpp')
const JS_SRC = join(ROOT, 'framework', 'sam_js_runtime.cpp')
const OUT_MD = join(ROOT, 'docs', 'function-reference.md')
const OUT_DTS = join(ROOT, 'gui', 'public', 'sam.d.ts')

const api = await import('file://' + join(ROOT, 'gui', 'src', 'data', 'samApi.js').replace(/\\/g, '/'))
const FUNCS = api.SAM_FUNCTIONS
const EVENTS = api.SAM_EVENTS

// ---------------------------------------------------------------- drift check
const names = (src, re) => {
  const out = new Set()
  for (const m of readFileSync(src, 'utf8').matchAll(re)) out.add(m[1])
  return out
}
const lua = names(LUA_SRC, /lua_setglobal\(L,\s*"(sam_[A-Za-z0-9_]+)"/g)
const js = names(JS_SRC, /JS_SetPropertyStr\(ctx,\s*\w+,\s*"(sam_[A-Za-z0-9_]+)"/g)
const declared = new Set(FUNCS.map(f => f.name))

const problems = []
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

// Events fired by the engine must appear in the catalog for the same reason.
const evFired = new Set()
for (const src of [LUA_SRC, JS_SRC, join(ROOT, '..', 'Barony', 'src')]) {
  // only the two runtime files are read directly; engine-side names are checked by the
  // shell gate, which can walk the tree. Keeping this tool dependency-free is deliberate.
  if (src.endsWith('.cpp')) {
    for (const m of readFileSync(src, 'utf8').matchAll(/"((?:player|world|game|item|monster|mod|ui)\.on_[a-z_]+)"/g)) evFired.add(m[1])
  }
}
const evDeclared = new Set(EVENTS.map(e => e.name))
diff(evFired, evDeclared, 'event fired but MISSING from samApi.js')
dupes(FUNCS, 'function')
dupes(EVENTS, 'event')

if (problems.length) {
  console.error('API DRIFT (' + problems.length + '):')
  for (const p of problems) console.error('  ' + p)
  process.exit(1)
}
console.log(`ok  ${lua.size} functions and ${EVENTS.length} events agree across both runtimes and samApi.js`)
if (CHECK_ONLY) process.exit(0)

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
const sig = f => `${f.name}(${(f.params || []).map(p => p.name).join(', ')})`

let md = `# S.A.M function reference

Every script function the framework exposes: **${FUNCS.length} functions** and **${EVENTS.length} events**.
All of them work identically in Lua, JavaScript and TypeScript.

This page is generated from the API definition, so it cannot fall behind the code. If a
function is missing here it is missing from the framework.

**Host-only** means the call is refused on a multiplayer client, where it becomes a logged
no-op rather than a crash. Read the value on the host and send it on if a client needs it.

For guides and worked examples, see [scripting-reference.md](scripting-reference.md).

## Contents

${[...byCat.keys()].sort().map(c => `- [${c}](#${c.toLowerCase().replace(/[^a-z0-9]+/g, '-')}) (${byCat.get(c).length})`).join('\n')}
- [Events](#events) (${EVENTS.length})

`

for (const cat of [...byCat.keys()].sort()) {
  md += `\n## ${cat}\n\n`
  for (const f of byCat.get(cat).sort((a, b) => a.name.localeCompare(b.name))) {
    md += `### \`${sig(f)}\`\n\n`
    if (f.hostOnly) md += `> Host-only.\n\n`
    md += `${f.desc || ''}\n\n`
    if ((f.params || []).length) {
      md += `| argument | type |\n|---|---|\n`
      for (const p of f.params) md += `| \`${p.name}\` | ${p.type}${p.values ? ` — one of: ${p.values.map(v => `\`${v}\``).join(', ')}` : ''} |\n`
      md += `\n`
    }
    md += `**Returns:** ${f.returns || 'nothing'}\n\n`
  }
}

md += `\n## Events\n\nHandle these in \`on_event(e)\`. Every script receives every event; check \`e.name\`.\n\n`
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
}

// ---------------------------------------------------------------- the .d.ts
const tsType = t => {
  const s = String(t || '').toLowerCase()
  if (s.startsWith('int') || s.startsWith('number')) return 'number'
  if (s.startsWith('bool')) return 'boolean'
  if (s.startsWith('table') || s.startsWith('object') || s.startsWith('array')) return 'any'
  return 'string'
}
const tsRet = r => {
  const s = String(r || '').toLowerCase()
  if (!s || s === 'nothing') return 'void'
  if (s.includes('boolean')) return 'boolean'
  if (s.includes('array') || s.includes('table')) return 'any[]'
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

let dts = `// TypeScript definitions for the S.A.M Framework scripting API.
// Generated from the API definition; do not edit by hand.
//
// Drop this beside your mod's .ts files, or reference it:
//   /// <reference path="sam.d.ts" />
//
// ${FUNCS.length} functions, ${EVENTS.length} events.

declare global {
`
for (const f of FUNCS.slice().sort((a, b) => a.name.localeCompare(b.name))) {
  const doc = (f.desc || '').replace(/\*\//g, '*\\/')
  dts += `  /**\n   * ${doc}${f.hostOnly ? '\n   *\n   * Host-only: refused on a multiplayer client.' : ''}\n   */\n`
  dts += `  function ${f.name}(${(f.params || []).map(p => `${safeParam(p.name)}${/optional/i.test(p.type) ? '?' : ''}: ${tsType(p.type)}`).join(', ')}): ${tsRet(f.returns)};\n\n`
}
dts += `  /** Every event name the engine fires. */\n  type SamEventName =\n${EVENTS.map(e => `    | ${JSON.stringify(e.name)}`).sort().join('\n')};\n\n`
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

writeFileSync(OUT_MD, md, 'utf8')
writeFileSync(OUT_DTS, dts, 'utf8')
console.log(`wrote docs/function-reference.md (${md.split('\n').length} lines)`)
console.log(`wrote gui/public/sam.d.ts (${dts.split('\n').length} lines)`)
