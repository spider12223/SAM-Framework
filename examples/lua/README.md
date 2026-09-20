# Lua behavior example

`assassin.lua` is a complete S.A.M behavior script. It needs framework v0.5.0 or newer:
sam_spawn_item, sam_play_sound, sam_apply_effect, sam_get_floor and sam_get_nearby_entities
all arrived in that release.

Put a `.lua` file next to a class JSON with the **same base name** and S.A.M loads
it automatically at launch:

```
mods/my_mod/classes/assassin.json   ← WHAT the class is  (stats, skills, gear)
mods/my_mod/classes/assassin.lua    ← HOW it behaves     (this file)
```

The script defines `on_event(event)`, which S.A.M calls with a copied,
primitive-only event table. This example grants the Assassin an Iron Dagger on
every level-up.

See the **Lua Scripting** section of the [main README](../../README.md) for the
full list of hooks, API functions, and the safety-sandbox guarantees.
