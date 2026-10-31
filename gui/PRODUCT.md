# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users

Two groups, about equally: Barony players who make mod content (classes, items, monsters, races, spells,
effects, sounds, music, models, recipes, patches) through forms without writing code, and modders who also
script, who use the API reference, the script editor and the block builder. Both work on a computer, often
with Barony itself open alongside, and keep the tool open for long sessions.

## Product Purpose

The S.A.M Mod Builder is the browser half of S.A.M (Support All Mods), a patched barony.exe that loads JSON
content and Lua, JavaScript or TypeScript scripts. The builder lets someone make a mod's JSON files with
editors instead of by hand, checks them against the S.A.M schemas, and exports a ready-to-use mod .zip.
Success is a mod that loads in the game the first time.

## Positioning

It is the only editor built against S.A.M's own schemas and function catalogue: every field it writes is one
the patched game reads, and the API reference is generated from the same source as the game's bindings.

## Operating Context

- Hosted on GitHub Pages from the public `spider12223/SAM-Framework` repository; pushing `main` deploys it.
- Opened from the Workshop page and the README; used next to Barony and a text editor.
- Nothing is uploaded: work is kept in the browser and exported as a .zip.

## Capabilities and Constraints

- React 18, Vite, Tailwind 4, react-router (hash routes). Sixteen pages: Start Here, Dashboard, eleven
  editors, Mod Builder, Validator, API Reference.
- Schemas in `public/schemas/` are generated (`npm run sync:schemas`); the API catalogue is
  `src/data/samApi.js`. `npm test` runs the block builder's generated Lua through real Lua.
- Barony's own art and fonts belong to Turning Wheel. The user decided (2026-10-31) that the look is
  redrawn in code with freely licensed fonts; no menu images or fonts are copied from the game. The site
  already ships Barony item icons in `public/item-icons/` for item pickers; that is existing content, not a
  licence for more.

## Brand Commitments

- The name is "S.A.M Framework", expanded "Support All Mods". Credit people by username only.
- Binding visual direction from the user (2026-10-31): it should look and feel like Barony's own in-game
  menus.
- Type rule from the user: a pixel face for menu chrome (titles, tabs, buttons, labels), a plain readable
  face for typed values and long explanations.
- Plain English, no em or en dashes in public text.

## Evidence on Hand

- Barony's menu mockups in the game folder (`images/ui/Main Menus/*/…_SAMPLE…png`) as reference only.
- Real item icons in `public/item-icons/`.
- No testimonials, user counts or download numbers exist; none may be invented.

## Product Principles

1. Every field written is one the game reads; the editor never offers what the engine ignores.
2. A non-coder can finish a mod without reading code; a scripter is never slowed down by that.
3. Text in the tool says only what the code does.
4. It should feel like part of Barony, not a website about it.

## Accessibility & Inclusion

Long sessions over dense forms: body text must stay readable (the user ruled pixel text out for forms and
long explanations), keyboard focus must be visible, and reduced motion respected.
