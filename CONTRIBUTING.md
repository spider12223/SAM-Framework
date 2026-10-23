# Contributing to S.A.M

Thanks for helping. This page is the map: where things live, how a feature travels from a mod's
file into the game, and the checks to run before a pull request. If you only want to **make a mod**, you do
not need any of this: start with [docs/getting-started.md](docs/getting-started.md) and copy a
folder from [examples/](examples/).

S.A.M was created by **spider12223 (SAMBOSA)**. This page is built on one written by
**rumpleivskin**, whose text it keeps in large part.

---

## The two repositories

S.A.M is two halves that build together:

```
workspace/
  SAM-Framework/   this repository: the framework's C++ (framework/), schemas, docs, GUI, examples
  Barony/          Barony-SAM: Barony's own source with the engine hooks that call into the framework
```

- **SAM-Framework** holds everything that is S.A.M's own: the C++ in `framework/`, the JSON
  schemas, the docs, the Mod Builder GUI and the example mods. MIT licensed.
- **[Barony-SAM](https://github.com/spider12223/Barony-SAM)** is Barony 5.0.2 with S.A.M's changes
  to the engine: mostly short calls into the framework ("hooks"), most of them marked with an
  `S.A.M` comment, plus some larger blocks such as the S.A.M console commands. Its
  `SAM_ENGINE.md` explains the branches and how to see exactly what changed with `git diff`. It
  keeps Barony's BSD 2-Clause licence.

Barony-SAM's CMake adds `../SAM-Framework/framework` to the game, so the two folders must sit side
by side and the framework folder must be named `SAM-Framework`. Then build Barony as upstream does
(see `INSTALL.md` in Barony-SAM), using a Barony-SAM checkout that matches the framework release
you are on. If you have the engine, name its folder `Barony` (`git clone` calls it `Barony-SAM`, so
rename it) and keep it next to `SAM-Framework`: `node tools/gen_api_docs.mjs --check` reads
`../Barony/src` to find where events fire, and without that folder it wrongly reports dozens of
events as never fired. If you have no engine checkout, expect those lines and say so in your pull
request. The Mod Builder is a separate, pure front end: see the README.

---

## Where things live

| You want to change... | Look in |
|---|---|
| what a `mod.json`, class, item, monster, race, spell... file may contain | `schemas/*.schema.json` (the source of truth), then the registry that reads it (`sam_classes`, `sam_items`, `sam_monsters`, `sam_races`, `sam_spells`, `sam_effects`, `sam_recipes`, `sam_sounds`, `sam_music`, `sam_models`, `sam_rooms`) |
| how mods are found and loaded | `sam_workshop` (finds manifests), `sam_loader` (the entry point), `sam_patcher` (layered JSON patches) |
| a script function (`sam_*`) | `gui/src/data/samApi.js` (its description), `sam_lua_runtime.cpp` and `sam_js_runtime.cpp` (its two bodies), `sam_mp_contracts.inc` (who may call it in multiplayer) |
| an event (`player.on_*`, `world.on_*`...) | `samApi.js` (its payload) and a `SamEvent` at the point in the engine or framework where it happens (`sam_event.hpp`) |
| multiplayer | `sam_net` (the channels and op blocks), `sam_mp_input`, `sam_mp_inventory`, `sam_mp_entities`, `sam_sync` (the mod-list check) |
| the numbers the engine uses (stats, XP curve, speed, loot) | `sam_rules`, `sam_speed`, `sam_loot`, `sam_combat` |
| on-screen things scripts draw | `sam_ui`, `sam_hud`, `sam_camera`, `sam_images` |
| settings a mod adds | `sam_settings` |
| terrain, doors, levers, line of sight and what is inside a container | `sam_world` |
| state saved inside one character's savegame, as opposed to the global mod data file | `sam_world_state` |
| how a living creature's body is drawn (the custom body override) | `sam_bodies` |
| the built-in Hunter's Workbench | `sam_workbench` |
| listing the game's items, monsters and spells for a script | `sam_catalog` |
| base stat overrides for a monster type | `sam_monster_patches` |
| the once-a-day save backup taken when mods are on | `sam_backup` |
| messages a modder sees when something is wrong | `sam_errors`, `sam_logger` (`sam_log.txt`) |
| running a mod's own tests | `sam_test` (`-samtest=Mod`, see [docs/testing-mods.md](docs/testing-mods.md)) |

Every header starts with a comment saying what the module is for and how it is used; read that
first. In Barony-SAM, search for `S.A.M` to find the hooks: most call sites carry a comment saying
why they are there. Some do not (the shop events in `shops.cpp` and the arrow, magic and spear trap
events, for example), so also search for `SAM[A-Za-z]*::` (a regular expression) and `SamEvent` to
catch those.

---

## Recipes

### Add a script function

1. **Describe it** in `gui/src/data/samApi.js`, in its category: `name`, `category`, `params`,
   `returns`, `desc` and `mp` (who may call it in multiplayer), plus `hostOnly`, which is true
   exactly when `mp` is `host`, `owner` or `all`. Add `gotcha` and `mpNote` when a modder needs them. The description is what modders read, in the
   docs and in the Mod Builder, so write it for them: what it does, what it returns when it cannot,
   one gotcha if there is one.
2. **Decide its multiplayer kind** and add a `SAM_MP(...)` line to `framework/sam_mp_contracts.inc`
   (the comment at the top of that file explains the kinds and how to pick a refusal value). Most
   functions that change the world are `Host`; ones about one player's screen are `Screen`.
3. **Write both bodies**: `lua_sam_x` in `sam_lua_runtime.cpp` and `js_sam_x` in
   `sam_js_runtime.cpp`, registered with `samLuaRegister` / `samJsRegister` (never directly: the
   register call is what applies the contract), with the same argument count and the same
   behaviour. Lua and JS parity is not optional. Put the real work in one C++ function both bodies
   call, so the two languages cannot disagree.
4. **Refuse helpfully.** A bad call returns the refusal value and logs one line that says what was
   wrong and what to do. Never fail silently.
5. **Regenerate the docs:** `node tools/gen_api_docs.mjs`. It writes `docs/function-reference.md`
   and the TypeScript definitions, and `--check` fails if anything drifted (argument counts, a
   missing contract, an event with no fire site).

### Add an event

Declare it in `samApi.js` (payload, `cancellable`, `writable`, where it fires and on which
machine), then fire it where the thing happens, in both runtimes (a `SamEvent` does both from one
call). Fire on the host for anything that changes the game, once per thing that happened.

### Add a field to a JSON file

Read it in the C++ parser, then add it to the schema in `schemas/` (the schemas say
`additionalProperties: false`, so a key missing from the schema is rejected by the Mod Builder even
when the engine accepts it). The copy in `gui/public/schemas/` is generated from `schemas/`, so do
not edit it by hand: run `cd gui && npm run sync:schemas` (`npm run build` does it too) and commit
the refreshed copy. Also add the key to `gui/src/lib/editorKeys.js` and the matching editor page,
then run `node tools/gen_schema_docs.mjs` so the schema reference matches.

### Add an engine hook (Barony-SAM)

Keep it to one line where you can: call a framework function and let the framework decide. Comment
it `// S.A.M: <why>`. With no mod loaded it must do exactly what vanilla does, and it must not
change what goes over the wire to a stock Barony player (a stock client has to be able to join a
S.A.M host). Put new engine behaviour in a framework module, not in the engine file. Mind each
file's line endings: Barony's files are mixed, some LF and some CRLF, and an edit that flips one
turns a ten-line change into a whole-file diff.

---

## Checks before you open a pull request

| Check | Command |
|---|---|
| the Mod Builder's tests (the block builder's generated Lua is run through a real Lua 5.4 only if one is found: set `SAM_LUA`, put `lua` on your PATH, or build one from `framework/lua54`; with none, those checks print SKIP and `npm test` still exits 0, so look for a line ending "(lua: ...)" in the output) | `cd gui && npm test` |
| the Mod Builder builds | `cd gui && npm run build` |
| the API reference matches the code | `node tools/gen_api_docs.mjs --check` |
| the schema reference matches the schemas | `node tools/gen_schema_docs.mjs --check` |
| a mod's own tests, in the real game, with nobody at the keyboard | `bash tools/run_mod_test.sh <Mod>` (see [docs/testing-mods.md](docs/testing-mods.md)) |

Say in the pull request what you tested and what you could not (in-game, online with a second
machine). An honest "not tried online" is worth more than a silent one.

When you write a test, check something that would be **different if the code were broken**, not
merely that a value has the right type. "The speed is still 2 after a refused call" can fail; "the
speed is a number" cannot. [examples/HelloTest](examples/HelloTest) is a small model.

## Style

- Match the file you are in: tabs, braces on their own lines in engine-style code, the comment
  style around you.
- Comments say **why**, and what a reader would otherwise get wrong. The existing ones are good
  models.
- Names say what a thing is for, not how it works.
- Nothing a mod can do may crash the game. Check every pointer that comes from a uid or a slot.

## Versions and releases

Do not bump the version in a pull request. The version stays at the last shipped release while work
is in progress, and the maintainer bumps it when shipping. (`python tools/check_versions.py` is the
maintainer's pre-release check: it compares the number in `framework/sam_logger.hpp` with
`workshop_upload/framework_version.json` and `gui/src/pages/Dashboard.jsx`, and it looks for the
release notes in a release folder that only the maintainer has, so in a plain clone it always ends
with that one file reported as missing, which is expected.) If your change adds behaviour, say so in the pull request
and mark new text in the docs "(unreleased)"; the release notes are written at ship time.

## Reporting a bug

Open an issue with: what you did, what you expected, what happened, your `sam_log.txt` (see
[docs/getting-started.md](docs/getting-started.md) for where it is), the mods you had on, and solo
or online.
