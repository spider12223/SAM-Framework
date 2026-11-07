# S.A.M Framework v1.2.3 — hunger, and the visual script builder

## New — hunger is scriptable

`HUNGER` now works in `sam_get_stat` / `sam_set_stat`, in both runtimes.

It was already a field on the player's Stat struct, and there was already an event
(`player.on_hunger_change`) telling you when it moved — but no script could actually
read or set it. So you could react to hunger and never touch it. Now you can:

```lua
if sam_get_stat(player, "HUNGER") < 250 then          -- getting hungry
  sam_set_stat(player, "HUNGER", 1500)                -- a full meal
end
```

Range is **0–1500** (0 = starving), matching the engine's own clamps. Values are
clamped for you. Host-side writes are pushed to the owning client via the engine's
`HNGR` update, so it works in multiplayer like every other stat.

The hunger *tiers* (hungry / weak / starving / oversatiated) are computed per-race by
the engine, so scripts read the raw number; `player.on_hunger_change` still tells you
when a tier edge is crossed.

## New — the visual script builder (Mod Builder)

The script editor now has **Basic** and **Advanced** tabs.

**Basic** builds a script from blocks: pick a trigger (any of the 49 hooks, or
"every N seconds"), add conditions and actions from dropdowns, each with a `+` to add
another, and it writes the Lua for you. There's a live code preview and a plain-English
read-back of what you built.

It's not just convenience — the builder can't produce the mistakes that fail silently:

- **Handlers are always right.** It emits `on_event`/`event.name` (and a real `on_tick`
  handler for "every N seconds") — the pair that's easy to swap, where a wrong guess
  registers nothing and does nothing.
- **Nothing is invented.** Triggers, their payload fields, and the API come from the
  same manifest the API Reference uses, so it can't offer a hook or function that
  doesn't exist.
- **No reads of fields that aren't there.** Actions declare the event field they need,
  so `on_monster_died` offers "damage the monster" (it carries `monster_uid`) and hides
  "damage the target" (it doesn't carry `target_uid`).

**Advanced** is the full editor, unchanged. Generation is one-way (blocks → Lua) and
says so; Basic is the default only for a brand-new script.

New condition blocks: move speed, time played, hunger (via the stat comparison).

*Thanks to solidius for the idea and the sketch.*

## Unchanged from v1.2.2

Everything else — the script/attribute diagnostics, `sam_get_class`, move speed,
stat sync, custom `.vox` models and class looks.

## How to get it

- **Installer:** grab the v1.2.3 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.3 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
