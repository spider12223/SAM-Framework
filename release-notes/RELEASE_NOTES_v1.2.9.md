# S.A.M Framework v1.2.9 — a real move-speed ceiling + tools for conditional abilities

Follow-ups from the community: move speed that can actually go faster than vanilla, and
the missing verbs for building abilities with conditions, upsides and downsides.

## Move speed can now break past the old cap — safely

`sam_set_move_speed` / `sam_add_move_speed` used to clamp at 3.0, but that was a dead end:
the engine also caps a player's per-frame velocity at 5.0 to stop you moving far enough in
one frame to clip through a wall, and a fast character already hit that at ~3x — so a bigger
number did nothing.

- The script multiplier cap is raised **3.0 → 5.0**.
- For a script-boosted player only, the engine's velocity clamp now lifts from 5.0 to a
  **tunnel-safe 7.0**, so a high multiplier actually translates into more speed. Normal
  play, dash and knockback keep the vanilla 5.0 cap, and 7.0 stays well under the 16-unit
  wall thickness collision tests against — no wall-clipping.

Net: a boosted player can move meaningfully faster than vanilla's maximum. 7.0 is the
hard ceiling; past it you'd risk phasing through walls, so it's capped there.

## New script functions for conditional abilities

- **`sam_get_effect_duration(player, effect)`** → remaining ticks (0 if inactive, -1 if
  permanent). Lets a buff or debuff scale/decay by how much time is left.
- **`sam_get_effect_strength(player, effect)`** → the effect's tier/magnitude (0 if
  inactive), for effects that carry one.
- **`sam_get_effects(player)`** → the player's whole active-effect list at once, as
  `{ name, ticks, strength }` — react to "any debuff" or strip all buffs without polling
  ~130 effects by name. Custom pseudo-effect slots show as `"CUSTOM:<id>"`.
- **`sam_apply_effect(player, effect, ticks[, strength])`** → the optional 4th argument
  applies an effect at a chosen tier (GROWTH stacks, etc.). Old 3-arg calls are unchanged.
- **`sam_set_player_data` / `sam_get_player_data`** → a per-player, in-memory, per-session
  scratchpad for cooldowns, ability flags and stack counters. Unlike `sam_save_data` it
  never touches disk and clears on a new game — the right tool for something you tick often.
- **`player.on_effect_applied`** now includes a **`strength`** field.

All of the above are in both the Lua and JavaScript/TypeScript runtimes.

## Also in the Mod Builder (already live on the site)

- New condition blocks: **effect time left compares to** and **effect strength compares
  to** (usable as `until` conditions too), so you can build "stronger while the debuff is
  fresh" or "fast until slow has < 1s left."
- The **apply a status effect** block gained a strength/tier field.
- Script API reference now lists **60** host functions.

## How to get it

- **Installer:** grab the v1.2.9 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.9 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
