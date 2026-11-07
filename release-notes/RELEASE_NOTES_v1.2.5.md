# S.A.M Framework v1.2.5 — 8 new world hooks, and a DLC-save fix

## Fixed

- **Non-human-race saves would not load: "Could not verify required DLC installed".**
  If you own the DLC but a skeleton / vampire / goatman / automaton / etc. save refused
  to load on the S.A.M exe, this is fixed. The build was asking Steam whether the DLC was
  *installed* (`BIsDlcInstalled`) rather than whether you *own* it — and Barony's DLC is a
  license whose content ships in the base game, so on some clients (for example when the
  DLC checkbox is unticked in Steam → Barony → Properties → DLC) "installed" reads false
  even though you own it. It now accepts ownership (`BIsSubscribedApp`) as well, so an
  owned pack is recognised either way. This is not a bypass — it is still gated entirely
  on a Steam-confirmed entitlement.

  If you hit this before updating, the manual workaround still works: **Steam → right-click
  Barony → Properties → DLC → tick all three boxes, then restart** (and don't run in Steam
  offline mode).

## New — 8 world-interaction hooks (49 → 57)

Scripts can now react to eight more things players do in the world. Each fires
host-side and carries the player and the relevant entity, so a class or item script can
respond to them:

- **`player.on_item_pickup`** — picking an item up off the ground (item type, count, name).
- **`world.on_door_opened`** — opening a door.
- **`world.on_projectile_hit`** — a projectile striking something (shooter, target).
- **`world.on_fountain_used`** / **`world.on_sink_used`** — using a fountain or sink,
  including the sink's outcome (ring / slime / nutrition / damage).
- **`world.on_switch_toggled`** — flipping a lever, with the new on/off state.
- **`world.on_orb_placed`** — placing an orb on a pedestal (and whether it was the right one).
- **`world.on_teleport`** — using a teleporter.

```lua
function on_event(event)
  if event.name == "world.on_door_opened" then
    sam_message(event.player, "A door creaks open...")
  elseif event.name == "player.on_item_pickup" then
    sam_message(event.player, "Picked up " .. event.item_name)
  end
end
```

They show up in the visual script builder as triggers too — no code needed. The full
payload of each is in the Mod Builder's API reference.

## Unchanged from v1.2.4

Every status effect, the visual script builder, hunger, `sam_get_class`, move speed,
stat sync, custom `.vox` models and class looks.

## How to get it

- **Installer:** grab the v1.2.5 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.5 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
