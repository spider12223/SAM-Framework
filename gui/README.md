# S.A.M Framework — GUI Tool

A pure-frontend React app for building Barony JSON mods (classes + items) for the
S.A.M Framework — no C++, no backend. Everything lives in React state for the
session (no localStorage by design); the output is a drop-in mod `.zip`.

## Run

```bash
cd SAM-Framework/gui
npm install
npm run dev      # http://localhost:5173
npm run build    # static bundle in dist/
```

## Pages

| Route | Purpose |
|---|---|
| `/dashboard` | Welcome, live session status, quick links, framework/version info |
| `/class-editor` | Attributes (sliders), 16 skills (steppers), starting items (searchable over all vanilla ItemTypes), spells, stat growth, gold → validates against `class.schema.json` |
| `/item-editor` | Name/category/slot/weight/value/level/model/attributes → validates against `item.schema.json` |
| `/mod-builder` | Set the manifest (namespace/name/author/version…), review bundled classes/items, export a `.zip` (`mod.json` + `classes/*.json` + `items/*.json`) |
| `/validator` | Paste/upload any JSON, auto-detect or pick a schema, see errors by JSON-pointer path |

## Schemas are the source of truth

Every enum the UI offers — the 524 vanilla item types, 16 `PRO_` skills, 15
categories, 11 equip slots, statuses, roll stats, and all id/version/namespace
patterns — is **read at runtime** from the three schema files in
`SAM-Framework/schemas/` (aliased as `@schemas`). Nothing is hardcoded: update a
schema on the C++ side and the editors follow automatically.

## Export → play

Export a mod in the Mod Builder, unzip it into
`…/steamapps/common/Barony/mods/<namespace>/`, then enable it from the game's
Mods menu. S.A.M loads it at runtime (see the framework's `sam_loader`).

## Stack

React 18 · Vite 6 · Tailwind v4 · ajv (draft-07 validation) · jszip (export).
