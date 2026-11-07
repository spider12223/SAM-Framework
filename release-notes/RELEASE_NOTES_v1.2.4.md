# S.A.M Framework v1.2.4 — every status effect, not 14 of them

## Fixed

- **Scripts can now name any of the engine's 135 status effects.** They could only name
  **14**. The other **121 were unreachable** — `STUNNED`, `FEAR`, `ROOTED`, `TELEPATH`,
  `MAGICREFLECT`, `VAMPIRICAURA`, `HP_REGEN`, `MP_REGEN`, `THORNS`, `POLYMORPH`, the
  shrine buffs, the ration flavours, the whole foci/divine set, and more. Asking for one
  got "unknown effect" and did nothing, with no hint that the effect was real and just
  unexposed.

  `sam_apply_effect`, `sam_has_effect` and `sam_remove_effect` all accept them, still
  case-insensitively, and custom slots (`"135"` / `"CUSTOM:135"`) work exactly as before:

  ```lua
  if sam_has_effect(player, "STUNNED") then
    sam_remove_effect(player, "STUNNED")
  end
  sam_apply_effect(player, "THORNS", 500)
  ```

- **The Lua and JS runtimes now share one effect table.** They each kept their own copy,
  and both had drifted to the same 14 names. There is now a single table, so they can't
  disagree about what an effect is called.

- The Mod Builder's effect dropdowns list all 135 as well, sourced from the same manifest.

## Worth knowing

Some of those 135 are internal engine bookkeeping (`MIMIC_LOCKED`, `SHAPESHIFT`,
`DISTRACTED_COOLDOWN`) and won't do anything sensible if you *apply* them by hand —
the engine has its own guards. Reading and removing is always safe.

Also: an effect you remove can be put straight back by whatever applied it. A beartrap,
for example, re-applies `PARALYZED` while you're standing in it — so a script that clears
paralysis on damage looks broken when it's actually working and being immediately undone.
`sam_log.txt` logs every removal, which is the quickest way to tell the two apart.

## Unchanged from v1.2.3

Hunger, the visual script builder, `sam_get_class`, move speed, stat sync, custom `.vox`
models and class looks.

## How to get it

- **Installer:** grab the v1.2.4 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.4 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
