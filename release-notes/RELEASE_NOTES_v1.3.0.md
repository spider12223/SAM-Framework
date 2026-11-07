# S.A.M Framework v1.3.0 — custom effects, races, sounds & world tools

The biggest content release yet: modders can now define their own **status effects**,
their own **playable races** (with full behavior scripting), and their own **sound
effects**, plus a batch of **world-manipulation** script functions and in-game test
commands. Every addition is a pure no-op when no mod uses it — vanilla behaviour is
unchanged.

## Custom status effects

Define an effect in JSON and S.A.M registers it into a reserved engine effect slot, with
**real mechanics** while it's active:

- flat attribute modifiers (STR/DEX/CON/INT/PER/CHR)
- a move-speed multiplier and per-second HP/MP drain or regen
- a HUD icon + tooltip

Apply one with `sam_apply_effect(player, "namespace:effect", ticks)`, from a script or the
visual block builder. New **Effect Editor** in the Mod Builder.

## Custom playable races

Make a race that rides an existing monster body — play as a skeleton, automaton, goatman,
imp, and more (**18 supported bodies**, correct in both third- and first-person). A race
carries flat attribute/HP/MP bonuses and appears in the character-select race picker
automatically. A vanilla or mod-less client opens such a character safely as Human.

**Races have the same scripting freedom as classes:** ship a `races/<name>.lua|js|ts`
behavior script that auto-loads and reacts to any of the ~35 event hooks. The new
**`sam_get_race(player)`** lets a race script gate its logic to players of that race. New
**Race Editor** (host-body picker + stat bonuses + behavior script).

## Custom sounds

Bundle your own `.ogg`/`.wav` and play it by name: `sam_play_sound("namespace:sound")`
(the id now takes a custom sound name or the old numeric index). S.A.M loads the file with
FMOD and appends it onto the engine sound table. New **Sound Editor** (upload the file,
optional loop).

## World-manipulation script functions

New host functions (Lua + JavaScript/TypeScript), all under a new **World** group in the
API reference:

- **`sam_get_position(uid)` / `sam_set_position(uid, x, y)`** — read or move any entity by
  map tile. Players teleport through the safe path (no tunnelling into walls).
- **`sam_get_player_uid(player)`** — bridge a player index to a uid for the above.
- **`sam_spawn_monster(x, y, "name" [, shopType])`** — summon a monster (including a working
  `shopkeeper`) and get its uid back.
- **`sam_get_inventory(player)` / `sam_remove_item(itemUid)`** — list a player's inventory
  and safely remove an item (refuses equipped items).

## In-game test commands

For quickly trying mods (type them into the console, no cheats flag needed):

- `/sam_effects`, `/sam_effect <ns:id> [ticks]`, `/sam_clear_effects`
- `/sam_races`, `/sam_setrace <ns:id>`
- `/sam_sounds`, `/sam_playsound <ns:id>`
- `/sam_pos`, `/sam_tp <x> <y>`, `/sam_spawn <name> [shopType]`, `/sam_give <item> [count]`,
  `/sam_inventory`, `/sam_remove <uid>`

## How to get it

- **Installer:** grab the v1.3.0 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.3.0 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next to
the exe (the installer and Workshop item handle this). Everything is backward compatible —
existing mods keep working unchanged.*
