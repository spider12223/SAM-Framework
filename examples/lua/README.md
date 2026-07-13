# Lua behavior example

`assassin.lua` is a complete S.A.M behavior script (v0.3.0+).

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
