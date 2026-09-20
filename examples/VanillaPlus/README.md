# Vanilla Plus — official S.A.M example mod

**Vanilla Plus** is the reference class pack for the S.A.M Framework: five new Barony
classes (Ranger, Necromancer, Paladin, Berserker, Trickster) that are tuned to sit
inside vanilla Barony's balance envelope, each pairing a **JSON class definition** with
a small **Lua behavior script**. It's meant to be read as a worked example of how the
two halves of a S.A.M mod fit together.

- **Play it:** [Vanilla Plus on Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3765143757)
- **Needs:** the S.A.M Framework installed first — [sam_framework on Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3763844472) / [installer on Releases](https://github.com/spider12223/SAM-Framework/releases/latest). (S.A.M is a patched `barony.exe`, so subscribing to a content mod alone isn't enough — install the framework once, then Workshop content works.)

## How a S.A.M class works (the two files)

```
VanillaPlus/
  mod.json                     # manifest: namespace + the class files it ships
  classes/
    ranger.json  ranger.lua    # JSON = data (stats/skills/items/spells)
    …                          # .lua  = behavior, auto-loaded as the companion script
  portraits/README.md          # 54×54 portrait spec (portraits are optional)
```

- **`classes/<name>.json`** — the class's stat modifiers, skill proficiencies, starting
  gear and spells. Validated against [`schemas/class.schema.json`](../../schemas/class.schema.json).
- **`classes/<name>.lua`** — the behavior script. S.A.M **auto-loads a script sitting
  next to a class JSON with the same base name** (`classes/ranger.json` → `classes/ranger.lua`).
  A script defines `function on_event(event)` (dispatch on `event.name`) and, optionally,
  a top-level `function on_tick(event)`.

## Each class demonstrates a different scripting pattern

| Class | Hooks used | Pattern it demonstrates |
|---|---|---|
| **Ranger** | `on_monster_died`, `player.on_floor_change` | **Persistent state** with `sam_save_data`/`sam_load_data` (a kill streak) → reward on a threshold with `sam_grant_item`, reset per floor. |
| **Necromancer** | `on_monster_died`, `on_before_damage`, `player.on_floor_change` | **Resource-on-kill** (`sam_set_stat` to restore MP) + an **emulated once-per-floor "cheat death"** (cancel a fatal hit with `sam_modify_damage`). |
| **Paladin** | `player.on_block`, `on_before_damage`, `player.on_floor_change` | **Defensive reactions** — heal on block (`sam_get_stat`→`sam_set_stat`) + an emulated last-stand that negates one blow while low. |
| **Berserker** | `on_tick`, `on_monster_died`, `on_before_damage` | **Per-tick state machine** keyed on HP% — live STR buff via `sam_set_stat`, haste via `sam_apply_effect`, plus time-boxed damage-immunity windows. |
| **Trickster** | `player.on_hit`, `player.on_equip` | **Proc-on-hit** (`math.random` → bonus `sam_deal_damage`) and **equip-triggered self-buffs** (cloak → `INVISIBLE`). |

## Patterns & gotchas the scripts illustrate

These are the non-obvious rules the comments in each `.lua` call out — worth reading if
you're writing your own behavior scripts:

- **`event.name` carries a category prefix** — it's `"player.on_hit"`, `"player.on_block"`,
  etc. The one exception is **`on_before_damage`** (no prefix), which is the *only* place
  you can cancel/reduce incoming damage (`sam_modify_damage`); `player.on_damage_taken`
  fires *after* HP is already gone and can't cancel.
- **`player.on_kill` is melee-only.** For ranged/magic kills, subscribe to
  **`on_monster_died`** and use its `killer_uid`.
- **Two separate calls for effects.** `sam_apply_effect(player, "FAST"|"INVISIBLE"|…)` is for a
  player; `sam_apply_monster_effect(uid, "BLIND"|"CONFUSED"|…, ticks)` is for a monster, and has
  been there since v0.7.0. A thrown `DUST_BALL` is still the better *feel* for a smoke bomb, which
  is why the Trickster uses one, but it is a choice now and not a limit.
- **There is no invincibility effect.** "Briefly invulnerable" is emulated by cancelling
  damage in `on_before_damage` for a time-boxed window (see Paladin/Berserker/Necromancer).
- **`sam_set_stat` edits the live player; `sam_patch_class` does not** — the latter only
  changes the class *definition* at character creation, so live buffs use `sam_set_stat`.
- **`sam_save_data` persists per-mod, not per player** — it writes one file for the whole mod on
  one machine, so in co-op every Paladin would share one "already intervened" flag. Per-player
  state belongs in `sam_set_player_data(player, key, value)`, which is in memory and is cleared
  when a run starts. All five scripts here use it, and reset on `player.on_floor_change`.
- **A loaded script gets every event, not only its own class's.** Dispatch is global, so each
  script's first line in every handler is `sam_get_class(player) == "vanillaplus:…"`. Without it
  a Ranger would collect the Paladin's damage negation and the Trickster's proc as well.
- **Read the player out of the event, never assume player 0.** `event.player` is whose event it
  is; `on_monster_died` gives you `killer_uid` instead, which these scripts match against each
  player's own body with `sam_get_player_uid`.
- **Every function says what it does in co-op.** Each entry in the
  [function reference](../../docs/function-reference.md) carries a **Multiplayer:** line, and
  [multiplayer.md](../../docs/multiplayer.md) explains the seven kinds.

## Balance

All five stay inside the vanilla envelope (STR cap +2, INT under Wizard's +3, signature
skills ≤ 55, net stat totals ~0…+1, every HP gain paid for in MP), so they read as
"belongs in vanilla" rather than power-crept. See each class JSON for the exact numbers.
