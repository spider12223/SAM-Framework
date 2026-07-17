# S.A.M Framework v1.2.2 — stop failing silently

Two diagnostics. No behaviour changes, no new APIs — this release just makes S.A.M
tell you when you've made a mistake it used to swallow.

## Fixed

- **A script that names a handler after an event now says so.** S.A.M only ever calls
  `on_event(event)` and `on_tick(event)`. Every other hook — `on_action_pressed`,
  `on_hit`, `on_monster_died` — arrives as `event.name` *inside* `on_event`. Writing
  `function on_action_pressed(event)` registers nothing, so the script loads, is
  disabled, and does absolutely nothing. The old warning said only "defines neither
  on_event nor on_tick", which didn't hint at the actual mistake. Now it names it:

  ```
  Script 'staffshield.lua' defines on_action_pressed() — that is an EVENT NAME, not a
  handler, so S.A.M never calls it and this script does nothing. S.A.M only calls
  on_event(event) and on_tick(event). Write it as:
      function on_event(event) if event.name == "<the event>" then ... end end
  ```

  This trips people because `on_tick` **is** its own function while everything else is
  an `event.name` — the two are mirror images and easy to swap. Both runtimes.

- **Item attributes the engine ignores are now called out.** Only `ATK` and `AC` are
  ever read from an item's `attributes`. An invented key like `"EFF_UNBREAKABLE": 1`
  used to pass validation and then be silently ignored forever. Two changes:
  - the game logs a warning naming the useless key at load;
  - **`item.schema.json` now only accepts `ATK` and `AC`**, so the Mod Builder and any
    schema-aware editor reject it *before* you ever launch.

  Item behaviour comes from a companion script, never from an attribute key.

## Upgrading

Drop-in. No mod changes required. If a mod of yours starts warning, it was already
broken — the warning is new, the bug isn't.

## Unchanged from v1.2.1

- `sam_get_class` custom-class fix, the move-speed API, `sam_set_stat` client sync,
  custom `.vox` models + per-class heads, and everything else.

## How to get it

- **Installer:** grab the v1.2.2 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.2 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
