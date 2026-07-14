<div align="center">

# S.A.M Framework
### Support All Mods — a JSON modding framework for [Barony](https://store.steampowered.com/app/371970/Barony/)

**[🎮 Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3763844472)** · **[⬇ Download Installer](https://github.com/spider12223/SAM-Framework/releases/latest)** · **[🛠 Mod Builder](https://spider12223.github.io/SAM-Framework/)** · **[📜 Schema Reference](https://spider12223.github.io/SAM-Framework/docs/schema-reference.html)**

</div>

---

## Quick Install

**Playing a S.A.M mod?** Get the framework in four steps:

1. **[Subscribe on Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3763844472)** — so Steam pulls S.A.M in for mods that require it.
2. **[Download the installer](https://github.com/spider12223/SAM-Framework/releases/latest)** from GitHub Releases (`SAM_Framework_Installer_v0.9.0.exe`).
3. **Run it once** — it auto-detects your Barony install, backs up your original `barony.exe`, and installs S.A.M (`barony.exe` + `typescript.js`, the compiler for TypeScript mod scripts). Nothing is deleted.
4. **Launch Barony** — enable mods in the **Mods** menu. A `sam_log.txt` in your Barony folder confirms S.A.M is running.

> Direct download: **[SAM_Framework_Installer_v0.9.0.exe](https://github.com/spider12223/SAM-Framework/releases/download/v0.9.0/SAM_Framework_Installer_v0.9.0.exe)** &nbsp;·&nbsp; To revert to vanilla, rename `barony_vanilla.exe` back to `barony.exe`.

---

## What is S.A.M?

**S.A.M (Support All Mods)** is a patched Barony executable that turns Barony into a data-driven modding platform. Instead of writing and compiling C++, modders describe custom **classes**, **items**, and **weapons** in plain JSON files. S.A.M reads those files at launch — from any mod folder Barony already mounts — and registers the content into the running game: custom classes appear on the character-select screen with their own portraits, stats, skills, starting gear and spells; custom items slot into the game's item table. A built-in multiplayer sync check warns players when a lobby's mods don't match. No C++ knowledge required.

> S.A.M is a **framework**, not a standalone game. You need to own **[Barony on Steam](https://store.steampowered.com/app/371970/Barony/)** to use it.

---

## Install

S.A.M is a **patched Barony executable**, so you need to own **Barony on Steam**.

- **Via Steam Workshop** *(intended channel, once published)* — subscribe to S.A.M on the Workshop; it delivers the patched build and keeps it in step with the mods that depend on it.
- **Build it yourself** *(available now)* — follow [Build from source](#build-from-source-contributors) to produce a patched `barony.exe` against your own copy of Barony, back up your existing one, and drop the S.A.M build into:
  ```
  …/steamapps/common/Barony/barony.exe
  ```

Launch Barony as usual — S.A.M initializes automatically and writes `sam_log.txt` to the Barony folder. To go back to vanilla, verify the game files through Steam (or restore your backup).

> **This repository ships S.A.M's source, schemas, GUI and docs — not a prebuilt game binary.** Barony and any patched build of it belong to [Turning Wheel LLC](https://github.com/TurningWheel/Barony) and aren't ours to redistribute; S.A.M is built against a copy of Barony you already own.

---

## Use the Mod Builder (GUI)

The easiest way to make a mod is the web-based builder — no install, runs entirely in your browser:

### → **https://spider12223.github.io/SAM-Framework/**

- **Class Editor** — attributes, all 16 skills, starting items (searchable over every vanilla item), spells, stat growth, gold, and a custom portrait.
- **Item Editor** — category, equip slot, weight, value, attributes.
- **Mod Builder** — set your namespace + manifest, then export a ready-to-use `.zip`.
- **Validator** — check any JSON against the schemas with precise error paths.

Every dropdown in the tool is generated from the [schemas](schemas/) at runtime, so it always matches what the framework accepts.

---

## Scripting — Lua, JavaScript & TypeScript (v0.6.0)

**JSON defines what your content *is*. A script defines how it *behaves*.**

S.A.M embeds three sandboxed scripting runtimes and picks by file extension — write in whichever you know. Drop a `.lua`, `.js`, or `.ts` file next to any class JSON and S.A.M loads it automatically; no C++, no compiler, no build step. **All matching scripts load, and all receive every event** — one class can even mix languages. Beginners can ignore scripting entirely and everything still works.

| Language | Runtime | Since |
|---|---|---|
| **Lua 5.4** | vendored Lua | v0.3.0 |
| **JavaScript** | quickjs-ng | v0.4.0 |
| **TypeScript** | quickjs-ng + `typescript` | v0.4.0 — transpiled to JS once at load, cached by content hash |

```
mods/my_mod/
  classes/assassin.json     ← stats, skills, starting gear   (WHAT it is)
  classes/assassin.ts       ← behavior in TypeScript   ┐
  classes/assassin.js       ← behavior in JavaScript   ├ any / all of these
  classes/assassin.lua      ← behavior in Lua          ┘
```

### Writing a behavior script

Define an `on_event(event)` function. S.A.M calls it with a copied, primitive-only event (never engine pointers). Same shape in every language:

```ts
// TypeScript
function on_event(event: { name: string; player: number; amount: number }): void {
  if (event.name === "player.on_level_up") {
    sam_log("Leveled up to " + event.amount);
    sam_grant_item(event.player, "IRON_DAGGER");
  }
}
```
```lua
-- Lua
function on_event(event)
  if event.name == "player.on_level_up" then
    sam_log("Leveled up to " .. tostring(event.amount))
    sam_grant_item(event.player, "IRON_DAGGER")
  end
end
```

### Hooks & API

**41 gameplay hooks and 26 host API functions** — every hook fires in Lua, JavaScript and TypeScript alike, host-authoritative (server/singleplayer only). The tables below are the v0.3–v0.5 core; **[New in v0.6.0](#new-in-v060)** adds 30 hooks and 14 host APIs (timers, persistent data, custom cross-runtime hooks, player queries).

| Hook | Fires when | Event fields |
|------|-----------|--------------|
| `player.on_level_up` | a player gains a level | `player`, `level`, `amount`, `stats` |
| `player.on_hit` | a player's melee weapon hits an entity | `player`, `target_uid`, `target_type`, `damage`, `weapon_type`, `lethal` |
| `player.on_kill` | a player's melee blow kills an entity | `player`, `target_uid`, `target_type`, `was_lethal` |
| `player.on_damage_taken` | a player takes damage (any source) | `player`, `damage`, `hp`, `maxhp`, `lethal`, `source_uid`, `source_type` |
| `player.on_death` | a player dies | `player`, `killer_type`, `killer_uid`, `killer_monster`, `obituary` |
| `player.on_equip` | a player equips an item | `player`, `item_type`, `slot` |
| `player.on_unequip` | a player unequips an item | `player`, `item_type`, `item_count`, `slot` |
| `player.on_item_use` | a player uses a consumable (potion / scroll / food) | `player`, `item_type`, `item_count`, `category` |
| `player.on_spell_cast` | a player casts a spell | `player`, `spell_id`, `spell_name`, `target_uid` |
| `player.on_gold_collected` | a player picks up gold | `player`, `amount`, `total_gold` |
| `player.on_floor_change` | a player descends to a new floor | `player`, `old_floor`, `new_floor` |

| API function | What it does |
|--------------|--------------|
| `sam_log(msg)` | write a line to `sam_log.txt` |
| `sam_message(player, text)` | show a message in the player's in-game log |
| `sam_grant_item(player, "ITEM_NAME")` | give a vanilla item to a player |
| `sam_grant_gold(player, amount)` | give gold to a player |
| `sam_spawn_item(x, y, "ITEM_NAME")` | spawn a ground item at a map tile |
| `sam_get_stat(player, "STAT")` → number | read `STR`/`DEX`/`CON`/`INT`/`PER`/`CHR`/`HP`/`MAXHP`/`MP`/`MAXMP`/`GOLD`/`LEVEL`/`EXP` |
| `sam_set_stat(player, "STAT", value)` | set a stat (bounded — never exceeds max, etc.) |
| `sam_apply_effect(player, "EFFECT", ticks)` | apply a status effect (`LEVITATING`, `INVISIBLE`, `POISONED`, …) for N ticks (50/sec) |
| `sam_remove_effect(player, "EFFECT")` | clear a status effect |
| `sam_get_floor()` → number | current dungeon floor (0-based) |
| `sam_play_sound(soundId [, vol])` | play a sound effect for all players |
| `sam_get_nearby_entities(player, radius)` → array | UIDs of creatures within `radius` tiles (max 32) |

<a id="new-in-v060"></a>

### New in v0.6.0 — 30 hooks + 14 APIs

**30 new gameplay hooks**, grouped by area (all fire in Lua/JS/TS, host-authoritative):

**Combat**
| Hook | Fires when | Event fields |
|------|-----------|--------------|
| `player.on_attack_start` | a player starts an attack swing (any weapon) | `player`, `weapon_type`, `target_uid` |
| `player.on_block` | a player fully blocks a hit with a shield | `player`, `attacker_uid`, `attacker_type`, `damage_blocked` |
| `player.on_miss` | a player's melee swing connects with nothing | `player`, `target_uid`, `weapon_type` |
| `player.on_bleed_tick` | bleeding ticks damage on a player | `player`, `damage`, `stacks_remaining` |
| `player.on_poison_tick` | poison ticks damage on a player | `player`, `damage`, `stacks_remaining` |

**Items & inventory**
| Hook | Fires when | Event fields |
|------|-----------|--------------|
| `player.on_item_identified` | a player identifies an item | `player`, `item_type`, `item_name` |
| `player.on_item_dropped` | a player drops an item | `player`, `item_type`, `floor_x`, `floor_y` |
| `player.on_item_broken` | a player's equipped item breaks | `player`, `item_type`, `slot` |
| `player.on_chest_opened` | a player opens a chest | `player`, `chest_uid`, `floor_x`, `floor_y` |
| `player.on_shop_entered` | a player opens trade with a shopkeeper | `player`, `shopkeeper_uid` |
| `player.on_item_bought` | a player buys from a shop | `player`, `item_type`, `gold_spent` |
| `player.on_item_sold` | a player sells to a shop | `player`, `item_type`, `gold_received` |

**Magic & effects**
| Hook | Fires when | Event fields |
|------|-----------|--------------|
| `player.on_spell_learned` | a player learns a spell | `player`, `spell_id`, `spell_name` |
| `player.on_spell_failed` | a cast fizzles / is blocked | `player`, `spell_id`, `spell_name`, `reason` |
| `player.on_effect_applied` | a status effect is newly applied | `player`, `effect_name`, `duration_ticks` |
| `player.on_effect_removed` | a status effect ends (cleared or expired) | `player`, `effect_name` *(+`effect` id on expiry)* |

**World & exploration**
| Hook | Fires when | Event fields |
|------|-----------|--------------|
| `world.on_trap_triggered` | an arrow/spike/magic trap fires | `trap_type`, `player`, `floor_x`, `floor_y`, `damage`\|`spell` |
| `world.on_monster_spawned` | a monster is summoned at runtime | `monster_uid`, `monster_type`, `monster_name`, `floor_x`, `floor_y`, `floor` |
| `world.on_chest_found` | a player first comes near a chest | `player`, `chest_uid`, `floor_x`, `floor_y` |
| `world.on_boulder_triggered` | a boulder trap launches | `floor_x`, `floor_y` |

**Player state**
| Hook | Fires when | Event fields |
|------|-----------|--------------|
| `player.on_hunger_change` | hunger crosses a tier edge | `player`, `hunger`, `hunger_level`, `old_hunger_level` |
| `player.on_xp_gained` | a player gains XP from a kill | `player`, `amount`, `source_type`, `new_total`, `monster_type` |
| `player.on_proficiency_increased` | a skill rank goes up | `player`, `proficiency`, `proficiency_name`, `old_rank`, `new_rank` |
| `player.on_status_effect_tick` | active effect ticks *(throttled to 1/sec)* | `player`, `effect`, `effect_name`, `ticks_remaining` |

**Multiplayer & lifecycle**
| Hook | Fires when | Event fields |
|------|-----------|--------------|
| `player.on_player_joined` | a client joins the lobby | `player_index`, `player_name`, `class_id`, `race` |
| `player.on_player_left` | a client disconnects / times out | `player_index`, `player_name` |
| `player.on_player_revived` | a downed player is revived on a new floor | `player`, `revived_by`, `floor`, `revive_type` |
| `game.on_level_entered` | a floor finishes loading | `player`, `floor`, `level_name` |
| `game.on_game_start` | a new game begins | `player`, `class_id`, `class_name`, `race`, `race_name` |
| `game.on_game_end` | the game is won or the party wipes | `player`, `won`, `floor_reached`, `kills`, `time_played` |

**14 new host API functions:**

*Persistent mod data* — a per-mod key/value store on disk (`savegames/sam_mod_data/<namespace>/`), survives restarts:
| API function | What it does |
|--------------|--------------|
| `sam_save_data(key, value)` | persist a value (number, string, or table/object) for your mod |
| `sam_load_data(key)` → value | read it back (`nil`/`null` if unset) |
| `sam_delete_data(key)` | remove a key |

*Timers* — schedule callbacks by game tick (50/sec), host-only:
| API function | What it does |
|--------------|--------------|
| `sam_set_timer(id, delay_ticks, fn)` | run `fn` once after `delay_ticks` |
| `sam_set_repeating_timer(id, interval_ticks, fn)` | run `fn` every `interval_ticks` until cancelled |
| `sam_cancel_timer(id)` | cancel a pending timer by id |

*Custom hooks* — define your own events, delivered **cross-runtime** (Lua↔JS↔TS) to every loaded mod, so mods can talk to each other:
| API function | What it does |
|--------------|--------------|
| `sam_register_hook("namespace:name")` | declare a namespaced custom hook |
| `sam_fire_hook("namespace:name", event)` → count | fire it; returns how many scripts received it |

*Player queries*:
| API function | What it does |
|--------------|--------------|
| `sam_get_class(player)` → string | class name (vanilla **or** custom) |
| `sam_get_kills(player)` → number | kills this session |
| `sam_get_time_played()` → number | ticks since the run started (50/sec) |
| `sam_get_equipped_item(player, slot)` → string | item in `weapon`/`shield`/`helmet`/`armor`/`gloves`/`boots`/`ring`/`amulet`/`cloak`/`mask` (`nil` if empty) |
| `sam_get_inventory_count(player, "ITEM")` → number | how many of an item the player holds |
| `sam_has_effect(player, "EFFECT")` → bool | whether a status effect is active |

```lua
-- v0.6.0 in action: persist a counter, run a timer, react to a new hook
function on_event(event)
  if event.name == "player.on_kill" then
    local kills = (sam_load_data("kills") or 0) + 1
    sam_save_data("kills", kills)                      -- survives restarts
    if kills == 10 then
      sam_register_hook("mymod:rampage")
      sam_fire_hook("mymod:rampage", { player = event.player })   -- any mod can listen
      sam_set_timer("cooldown", 250, function()        -- fire once in 5s
        sam_message(event.player, "Rampage over.")
      end)
    end
  end
end
```

All state-changing APIs are host-authoritative and validate their inputs; a bad call is a logged no-op, never a crash.

### Safety sandbox (all three runtimes)

Every script runs locked-down so a broken or malicious mod can't take down the game:

- **10 MB** memory cap per VM; a per-callback instruction/time budget with a watchdog — an infinite loop is killed in milliseconds (on JS the kill is even uncatchable by the script).
- No filesystem / network / OS: Lua has `os`/`io`/`dofile`/`loadfile`/`require` stripped; the JS/TS engine never links quickjs-libc, so there is no `fs`/`require`/`fetch`/`process`.
- A script error disables **only that script** — it never crashes the host.
- Scripts only ever receive **copied primitives** (ints, strings, UIDs), never a raw `Entity`/`Item` pointer — so a freed game object can't cause a use-after-free.
- Gameplay hooks run **host-authoritative only** (server/singleplayer), so multiplayer stays in sync.

Working examples: [`examples/lua/assassin.lua`](examples/lua/assassin.lua) · [`examples/js/assassin.js`](examples/js/assassin.js) · [`examples/typescript/assassin.ts`](examples/typescript/assassin.ts).

---

## Make & publish a mod

A S.A.M mod is just a folder of JSON (plus optional portrait/model images):

```
my_mod/
  mod.json                 ← manifest: namespace, name, version, what it ships
  classes/
    my_class.json          ← conforms to class.schema.json
  items/
    my_item.json           ← conforms to item.schema.json
  portraits/
    my_class.png           ← optional 54×54 class-select icon
```

1. Build it in the [Mod Builder](https://spider12223.github.io/SAM-Framework/) (or by hand against the [schemas](schemas/)) and export the `.zip`.
2. **Reserve your namespace** by submitting a PR to [`registry/namespaces.json`](registry/namespaces.json) so two mods never collide on the same prefix.
3. **Test locally**: unzip into `…/steamapps/common/Barony/mods/<your_mod>/` and enable it from Barony's Mods menu.
4. **Publish to the Steam Workshop** as a Barony mod, and **list S.A.M as a required dependency** in your Workshop description so subscribers know they need the S.A.M build. Since S.A.M mods are JSON-only, they carry no compiled code — S.A.M does all the work at runtime.

Multiplayer: S.A.M sends a mod fingerprint from host to client on join and **warns** (never hard-blocks) when the two don't match, so mismatches are obvious before a run.

New to this? Follow **[Getting Started →](docs/getting-started.md)** to build a class + item in ~5 minutes.

### Autocomplete as you type

S.A.M publishes its schemas so **VS Code validates and autocompletes your JSON** — every field's valid options in a dropdown, red squiggles on typos before you ever launch. No extension needed. Either add a `$schema` line to each file:

```json
{ "$schema": "https://spider12223.github.io/SAM-Framework/schemas/class.schema.json", "id": "mymod:myclass", ... }
```

…or copy this repo's [`.vscode/settings.json`](.vscode/settings.json) into your mod folder to map every file automatically. The [Mod Builder](https://spider12223.github.io/SAM-Framework/) stamps `$schema` into everything it exports, so exported mods get this for free.

---

## Schema reference

All content is validated against JSON Schemas (draft-07), which are the single source of truth for the framework, the GUI, and editor autocomplete:

| Schema | Describes |
|---|---|
| [`mod.schema.json`](schemas/mod.schema.json) | The `mod.json` manifest |
| [`class.schema.json`](schemas/class.schema.json) | A custom class |
| [`item.schema.json`](schemas/item.schema.json) | A custom item / weapon |
| [`patch.schema.json`](schemas/patch.schema.json) | A layered patch to an existing data file |

A human-readable, always-in-sync field reference is generated from these: **[Schema Reference →](https://spider12223.github.io/SAM-Framework/docs/schema-reference.html)** (or open [`docs/schema-reference.html`](docs/schema-reference.html) locally).

---

## Build from source (contributors)

S.A.M is a set of C++ files compiled **into** a patched Barony, plus a standalone React GUI.

**The GUI** (pure frontend, no backend):
```bash
cd gui
npm install
npm run dev      # http://localhost:5173
npm run build    # static site in dist/
```

**The framework** (`framework/*.cpp`) is compiled into Barony's build. In short: clone [Barony's source](https://github.com/TurningWheel/Barony), add the S.A.M `framework/` files to the game target, and apply the small set of interception hooks (loader, class/item registration, character-select carousel, multiplayer sync). The framework code is deliberately decoupled — the logger, workshop scanner, loader, and the parsing halves of the class/item registries include zero Barony headers. See [`framework/`](framework/) and the header comments for the hook points.

> Barony's own source is **not** included in this repository and is not ours to redistribute — it belongs to [Turning Wheel LLC](https://github.com/TurningWheel/Barony). S.A.M only ships its own framework code, schemas, GUI, and docs.

---

## Repository layout

```
framework/     C++ compiled into the patched Barony (logger, loader, classes, items, sync)
gui/           React + Vite mod-builder web app (deployed to GitHub Pages)
schemas/       JSON Schema definitions — the source of truth
docs/          Generated schema reference
registry/      Public namespace registry (submit a PR to reserve yours)
model-library/ Bundled generic models
workshop/      Steam Workshop packaging files
```

## License

S.A.M's own code, schemas, GUI, and docs are released under the [MIT License](LICENSE). Barony is © Turning Wheel LLC; you must own it to use S.A.M.

## License & Attribution

S.A.M Framework is built on Barony's open source release.
Barony is licensed under the BSD 2-Clause License.
Copyright (c) 2013-2020, Turning Wheel LLC

S.A.M Framework code is original work by spider12223, written with AI assistance. The framework source is available at:
https://github.com/spider12223/SAM-Framework

When S.A.M is distributed as a patched binary, the BSD 2-Clause copyright notice above is reproduced (as that license requires). Full third-party license texts for Barony and its bundled libraries (Dear ImGui, SDL, RapidJSON) are reproduced in [NOTICE.txt](NOTICE.txt).
