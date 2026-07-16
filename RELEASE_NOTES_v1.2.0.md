# S.A.M Framework v1.2.0 — Move speed, stat sync, and custom character looks

The biggest engine update since v1.0: scripts can now change how fast a player
moves and reliably change player stats in multiplayer, plus classes can force a
custom character look — including your own `.vox` models.

## New — scripting

- **`sam_set_move_speed(player, mult)` / `sam_get_move_speed(player)`** — a
  per-player movement multiplier, clamped to `[0.1, 3.0]`. Host-only to set; the
  value is synced to the player it applies to, so it works in multiplayer. `1.0`
  is normal speed.
- **`sam_set_stat` now reaches other players.** Previously a scripted stat change
  only existed on the host and the affected client's sheet silently disagreed until
  something unrelated happened to refresh it. Attribute, level, EXP, max-HP/MP, and
  gold changes are now pushed to the owning client immediately.

## New — content

- **Custom `.vox` character models + per-class looks.** A class can force a head
  (any of the 60 vanilla heads, or your own `namespace:model` `.vox`) on everyone
  who plays it. Design it in the new **Character** panel of the web Class Editor,
  which shows a live 3D preview of the actual model. *(Head only — the game only
  assigns a body look when the armour slot is empty, so a forced body would vanish
  the moment the player equips anything.)*

## Fixed

- **Custom classes now work correctly in online multiplayer.** Class IDs were being
  truncated to 8 bits over the network, so custom classes (IDs ≥ 1000) broke for
  remote players. IDs are now sent in full.
- **Out-of-range appearance values from a client are clamped** instead of being
  trusted, closing a desync/crash vector during join.
- **`sam_set_stat` value ranges are now protocol-safe.** Max-HP/MP are clamped to
  what the wire can carry (they used to desync permanently at large values), and
  negative attributes are clamped to what the stat packet can actually represent.

## Multiplayer notes for mod authors

- The stat sync tells a client about **its own** stats. `sam_set_stat(2, "STR", …)`
  updates player 2's sheet on player 2's screen; it does not make player 2's STR
  visible on player 1's screen (the vanilla protocol has no channel for that).
- `sam_set_move_speed` and `sam_set_stat` are **host-only** — call them from a
  host-authoritative context (a hook or `on_tick`).

## How to get it

- **Installer:** grab the v1.2.0 installer from the release below. It backs up your
  current `barony.exe` → `barony_vanilla.exe` and installs the S.A.M build. Existing
  installers already in the wild auto-update to this build too.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.0 exe.

## Heads-up

If you had an older S.A.M `barony.exe`, update it — scripts written for v1.2 APIs
(move speed, the new class look block) do nothing on an older exe. The web tools now
mark features that need this build.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
