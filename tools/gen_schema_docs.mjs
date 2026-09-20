// Generate the public Schema Reference page from schemas/*.schema.json.
//
// WHY THIS EXISTS. docs/schema-reference.html said at the bottom that it was "generated from
// schemas/*.schema.json", and no generator existed anywhere in the tree. It was written once by
// hand on 2026-07-13 and then frozen: by 2026-09 it documented 5 of the 10 schemas, with no
// section at all for races, spells, effects, sounds or recipes, no `bent`, and no `music` -- all
// of which the Workshop page advertises as things you can make. README.md called it
// "always-in-sync" and four public places linked to it, so a new mod author looking up how to
// declare a custom race found nothing and concluded the framework could not do it.
//
// A page a person maintains by hand next to ten machine-readable files will always lose. This
// tool reads the schemas themselves, so the page cannot say anything the schemas do not, and
// --check makes the ship gate fail rather than letting it rot again.
//
//   node tools/gen_schema_docs.mjs          write the page (every copy that ships)
//   node tools/gen_schema_docs.mjs --check  verify only, non-zero exit on drift (ship gate)

import { existsSync, readFileSync, writeFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join, sep } from 'node:path'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..')
const CHECK_ONLY = process.argv.includes('--check')

// mod.json first because it is the file every mod must have, then the content kinds in the
// order the Mod Builder's sidebar lists them.
const ORDER = [
  ['mod', 'Mod Manifest'],
  ['class', 'Custom Class'],
  ['item', 'Custom Item'],
  ['monster', 'Custom Monster'],
  ['race', 'Custom Race'],
  ['spell', 'Custom Spell'],
  ['effect', 'Custom Status Effect'],
  ['sound', 'Sound'],
  ['recipe', 'Tinkering Recipe'],
  ['patch', 'Data Patch'],
]

// Every copy of this page that ships. docs/ is what GitHub renders from the repo;
// gui/public/docs/ is the one copied into gui/dist and served by Pages, which is the URL
// README.md, getting-started.md, workshop_upload/README.txt and INSTALL.md all point at.
// gui/dist is a build output and is only written when it is already there.
const OUTPUTS = [
  [join(ROOT, 'docs', 'schema-reference.html'), 'the copy in the repo'],
  [join(ROOT, 'gui', 'public', 'docs', 'schema-reference.html'), 'the copy Pages serves'],
  [join(ROOT, 'gui', 'dist', 'docs', 'schema-reference.html'), 'the built copy'],
]

// The page is CRLF and has been since it was first written; keep it that way so a regeneration
// is a content diff and not a whole-file one.
const NL = '\r\n'
const toCRLF = s => s.replace(/\r\n/g, '\n').replace(/\n/g, NL)

const esc = s => String(s)
  .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
  .replace(/"/g, '&quot;').replace(/'/g, '&#x27;')

// Descriptions are prose written for a person, and several of them name a field or a literal
// value in `backticks` or "quotes". Turn the backticked spans into <code> and leave the rest
// alone rather than inventing markup the schema did not ask for.
const prose = s => esc(s).replace(/`([^`]+)`/g, (_, x) => `<code>${x}</code>`)

function typeName (s) {
  if (!s || typeof s !== 'object') return 'any'
  if (s.enum) return Array.isArray(s.type) ? s.type.join(' or ') : (s.type || 'string')
  if (s.oneOf) return s.oneOf.map(typeName).join(' or ')
  if (Array.isArray(s.type)) return s.type.join(' or ')
  if (s.type === 'array') {
    const it = s.items
    if (!it) return 'array'
    if (it.oneOf) return `array of (${it.oneOf.map(typeName).join(' or ')})`
    return `array of ${typeName(it)}`
  }
  return s.type || 'any'
}

// The constraints a modder can actually trip over. Kept to one line under the description so
// the table stays readable; the schema itself remains the exact statement.
function constraints (s) {
  const bits = []
  if (s.pattern) bits.push(`pattern <code>${esc(s.pattern)}</code>`)
  if (s.format) bits.push(`format <code>${esc(s.format)}</code>`)
  if (s.minLength !== undefined) bits.push(`minLen ${s.minLength}`)
  if (s.maxLength !== undefined) bits.push(`maxLen ${s.maxLength}`)
  if (s.minimum !== undefined) bits.push(`min ${s.minimum}`)
  if (s.maximum !== undefined) bits.push(`max ${s.maximum}`)
  if (s.minItems !== undefined) bits.push(`minItems ${s.minItems}`)
  if (s.maxItems !== undefined) bits.push(`maxItems ${s.maxItems}`)
  if (s.default !== undefined) bits.push(`default <code>${esc(JSON.stringify(s.default))}</code>`)
  return bits.length ? `<div class='cons'>${bits.join(' &middot; ')}</div>` : ''
}

function enumChips (s) {
  if (!s.enum) return ''
  const chips = s.enum.map(v => `<span class='chip'>${esc(String(v))}</span>`).join('')
  // A long enum (the 20 host bodies, the 14 item categories) buries the next field if it is
  // always open, so fold anything past a dozen.
  if (s.enum.length > 12) {
    return `<details class='enum-details'><summary>${s.enum.length} allowed values</summary><div class='enum'>${chips}</div></details>`
  }
  return `<div class='enum'>${chips}</div>`
}

// One field's description cell: prose, then the values it accepts, then its constraints, then
// whatever structure hangs off it. Depth is bounded because a schema that nests deeper than
// this is telling you to read the schema file itself.
function detail (s, depth) {
  let out = ''
  if (s.description) out += prose(s.description)
  out += enumChips(s)
  out += constraints(s)
  if (depth >= 4) return out

  if (s.oneOf) {
    out += `<div class='nested'>`
    s.oneOf.forEach((branch, i) => {
      out += `<div class='sub'>form ${i + 1}: ${esc(typeName(branch))}</div>`
      out += detail(branch, depth + 1)
    })
    out += `</div>`
    return out
  }

  const obj = s.type === 'object' || s.properties
  if (obj && s.properties) {
    out += `<div class='nested'>${table(s, depth + 1)}</div>`
  } else if (obj && s.additionalProperties && typeof s.additionalProperties === 'object') {
    out += `<div class='nested'><div class='sub'>any key, each a ${esc(typeName(s.additionalProperties))}</div>`
    out += detail(s.additionalProperties, depth + 1)
    out += `</div>`
  }

  if (s.type === 'array' && s.items && typeof s.items === 'object') {
    if (s.items.properties || s.items.oneOf) {
      out += `<div class='nested'><div class='sub'>each entry:</div>${detail(s.items, depth + 1)}</div>`
    } else if (s.items.enum) {
      out += enumChips(s.items)
    }
  }
  return out
}

function table (schema, depth) {
  const props = schema.properties || {}
  const req = new Set(schema.required || [])
  let rows = ''
  for (const [name, s] of Object.entries(props)) {
    const badge = req.has(name)
      ? `<span class='req'>required</span>`
      : `<span class='opt'>optional</span>`
    rows += `<tr><td class='pname'><code>${esc(name)}</code>${badge}</td>`
      + `<td class='ptype'>${esc(typeName(s))}</td>`
      + `<td class='pdesc'>${detail(s, depth)}</td></tr>`
  }
  if (!rows) return ''
  return `<table class='fields'><thead><tr><th>Field</th><th>Type</th><th>Description</th></tr></thead><tbody>${rows}</tbody></table>`
}

// ---------------------------------------------------------------- build

const sections = []
const navLinks = []
const missing = []

for (const [base, label] of ORDER) {
  const p = join(ROOT, 'schemas', `${base}.schema.json`)
  if (!existsSync(p)) { missing.push(`${base}.schema.json`); continue }
  const schema = JSON.parse(readFileSync(p, 'utf8'))
  const req = (schema.required || []).map(r => `<code>${esc(r)}</code>`).join(', ')
  let s = `<section id='${base}'><h2>${base}.json &mdash; ${esc(label)}</h2>`
  if (schema.description) s += `<p class='blurb'>${prose(schema.description)}</p>`
  s += `<p class='req-line'>Required: ${req || 'nothing'}`
  if (schema.additionalProperties === false) {
    // This is the sentence that matters most in practice: a key the schema does not list is
    // not ignored, it fails validation, and the Mod Builder drops the whole file on import.
    s += ` &middot; any key not listed below is rejected`
  }
  s += `</p>`
  s += table(schema, 1)
  s += `</section>`
  sections.push(s)
  navLinks.push(`<a href='#${base}'>${base}.json</a>`)
}

// A schema the game reads but this page does not cover is the exact failure this tool exists
// to end, so say so loudly rather than quietly printing nine sections.
if (missing.length) {
  console.error(`schemas missing from schemas/: ${missing.join(', ')}`)
  process.exit(1)
}

const nav = `<nav>${navLinks.join(' &middot; ')} &middot; `
  + `<a href="https://spider12223.github.io/SAM-Framework/">Mod Builder</a> &middot; `
  + `<a href="https://github.com/spider12223/SAM-Framework">GitHub</a></nav>`

const css = readFileSync(join(HERE, 'schema_docs.css'), 'utf8').trim()

// The stylesheet beside this file is LF like every other source file here, so normalise the
// whole page at the end rather than leaving a block of bare LFs inside a CRLF document.
const html = toCRLF(
  `<!doctype html>${NL}` +
  `<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">${NL}` +
  `<title>S.A.M Framework &mdash; Schema Reference</title><style>${NL}${css}${NL}</style>${NL}` +
  `<body>${NL}` +
  `<header><h1>S.A.M Framework</h1><div class="tagline">&mdash; Schema Reference &mdash;</div>${NL}` +
  `<p class="sub">Every field of every JSON file a mod can ship, taken straight from the schemas the game validates against.</p></header>${NL}` +
  nav + NL +
  `<main>${sections.join('')}</main>${NL}` +
  `<footer>Generated from <code>schemas/*.schema.json</code> (JSON Schema draft-07) by ` +
  `<code>tools/gen_schema_docs.mjs</code>. Barony &copy; Turning Wheel LLC.</footer>${NL}` +
  `</body></html>`)

// ---------------------------------------------------------------- write or check

if (CHECK_ONLY) {
  const stale = []
  for (const [p, why] of OUTPUTS) {
    if (!existsSync(dirname(p))) continue          // a bare checkout, or dist not built yet
    const where = p.replace(ROOT + sep, '')
    if (!existsSync(p)) { stale.push(`${where} does not exist (${why})`); continue }
    if (readFileSync(p, 'utf8') !== html) stale.push(`${where} is STALE (${why})`)
  }
  if (stale.length) {
    console.error(`SCHEMA REFERENCE OUT OF DATE (${stale.length}):`)
    for (const s of stale) console.error('  ' + s)
    console.error('Run: node tools/gen_schema_docs.mjs   (no --check) to bring it back in step.')
    process.exit(1)
  }
  console.log(`ok  schema-reference.html matches the ${ORDER.length} schemas`)
  process.exit(0)
}

let wrote = 0
for (const [p, why] of OUTPUTS) {
  if (!existsSync(dirname(p))) { console.log(`  skipped ${p.replace(ROOT + sep, '')} (no such directory)`); continue }
  writeFileSync(p, html, 'utf8')
  console.log(`  wrote ${p.replace(ROOT + sep, '')}  (${why})`)
  wrote++
}
console.log(`schema-reference.html: ${ORDER.length} schemas, ${html.length} bytes, ${wrote} copy/copies`)
