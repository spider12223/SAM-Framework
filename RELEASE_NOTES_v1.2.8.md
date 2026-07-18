# S.A.M Framework v1.2.8 — blessed/cursed grants, stacking speed, real level-ups, custom-item details

Four things testers asked for, plus a fix for custom items showing no stats.

## New script functions

- **`sam_grant_item(player, name[, beatitude, status, count])`** — grant blessed or
  cursed gear. The old two-argument call is unchanged (a plain, uncursed item), but you
  can now pass a beatitude (`+1`/`+2`/… blessed, `-1`/`-2`/… cursed), a condition
  (`0` broken … `4` excellent), and a stack count. It also resolves custom
  `"namespace:item"` gear now, not just vanilla names — so you can hand out your own
  items the same way.

- **`sam_add_move_speed(player, delta)`** — add to a player's move-speed multiplier
  instead of setting it. `sam_set_move_speed` overwrites; this stacks onto whatever the
  multiplier already is, so two speed abilities build up (one sets 2.0, another adds
  `+0.1` → 2.1) rather than clobbering each other. The result is clamped to 0.1–3.0.

- **`sam_level_up(player[, count])`** — level a player up *for real*, through the engine's
  own path: attribute rolls, HP/MP gain, the level-up screen and sound, and full client
  sync — the actual benefits. Bumping the `LEVEL` stat with `sam_set_stat` only ever
  changed the number and gave nothing; this does the real thing and fires the
  `player.on_level_up` hook once per level. Host-only.

- **`sam_set_stat(player, "EXP", …)` now accepts up to 255.** It was capped at 99, which
  silently made it impossible to level a player by crediting EXP. 100+ now triggers the
  engine's real level-up on the host's next tick, same as `sam_level_up`.

## Fixed

- **Custom items now show their ATK, weight, value and a real hover tooltip.** A custom
  item used to clone the bare `tooltip_default`, which carries no stat rows — so it
  displayed "This item does not have a tooltip yet!" and none of its details. It now
  clones the matching **category** tooltip (a sword clones the sword tooltip, a
  breastplate the armour tooltip, …), which is what renders the ATK / bonus / durability
  rows and the weight+value footer. The engine also clears the whole tooltip table when
  it loads its data files at game start, wiping the custom entries; those are now
  re-injected right after that pass, so the details survive into the game.

## Also in the Mod Builder (already live on the site)

- The visual block builder gained blocks for all of the above: **give an item** now has
  a blessing dropdown and a count, plus **add to move speed** and **level the player up**.
- The Script API reference lists the new functions (now **55** host functions).

## How to get it

- **Installer:** grab the v1.2.8 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.8 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
