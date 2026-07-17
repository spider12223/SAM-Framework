# S.A.M Framework v1.2.1 — sam_get_class fix (custom classes)

A targeted fix that makes class-aware scripts possible. Recommended for anyone writing
class mods; required by mods that gate abilities per class (e.g. Vanilla Vocations).

## Fixed

- **`sam_get_class(player)` now returns the correct name for custom classes.** It was
  routing custom class ids through the vanilla language lookup, which computed an
  out-of-place index and returned an unrelated UI string — so a script checking
  `sam_get_class(p) == "MyClass"` never matched. Custom ids now resolve from the class
  registry (the same source the class-select screen uses), returning the class's real
  `name`. Vanilla classes are unaffected. Fixed in both the Lua and JS runtimes.

## Why this matters for class mods

S.A.M dispatches every event to **every** loaded class script, regardless of who is
playing what. A class's signature ability therefore has to check *"is this player
actually my class?"* — and `sam_get_class` is how you check. With this fix, a script
can gate its effect so it only fires for its own class:

```lua
local CLASS = "MyClass"
local function mine(p) return sam_get_class(p) == CLASS end

function on_event(event)
  if event.name == "player.on_hit" and mine(event.player) then
    -- ability logic, runs only for a real MyClass player
  end
end
```

## Unchanged from v1.2.0

- The per-player move-speed API, `sam_set_stat` client sync, custom `.vox` models +
  per-class heads, and everything else from v1.2.0.

## How to get it

- **Installer:** grab the v1.2.1 installer from the release below. Installers already
  in the wild auto-update to this build too.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.1 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
