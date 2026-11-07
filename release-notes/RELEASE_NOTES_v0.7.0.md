# S.A.M Framework v0.7.0

Target: **Barony 5.0.2** · Languages: **Lua 5.4 · JavaScript · TypeScript**

v0.7.0 is a big scripting release: it adds a real-time tick loop, damage interception,
keyboard input, full monster/NPC scripting, and runtime patching of existing classes,
items and monsters — plus a completely rewritten, structured `sam_log.txt`. Every new
hook and API works identically across all three scripting languages.

## New behavior hooks

- **`on_tick`** — fires every game tick (50/sec) for every script; `{ tick_count, delta_ticks }`. The foundation for stamina, cooldowns, DoT and timers-of-your-own.
- **`on_before_damage`** — fires *before* a player's HP is reduced, so a script can rewrite the incoming damage (Last-Stand, shields, invincibility).
- **`on_key_pressed` / `on_key_released`** — keyboard input for A–Z, 0–9, F1–F12 (gameplay, host-side).
- **`on_monster_damaged` / `on_monster_died`** — react to any monster taking damage or dying (`monster_uid`, `monster_type`, `hp`, `max_hp`, `killer_uid`, `floor`, …). Boss phases, on-death drops, kill tracking.

## New host APIs

**Damage:** `sam_modify_damage(player, value)` (inside `on_before_damage`), `sam_deal_damage(entity_uid, amount)`.

**Input:** `sam_is_key_held(key)`.

**Monsters / NPCs (UID-based, host-authoritative):** `sam_get_monster_stat`, `sam_set_monster_stat`, `sam_get_monster_data`, `sam_set_monster_data`, `sam_apply_monster_effect`, `sam_spawn_monsters` (max 8), `sam_kill_monster`, `sam_get_monster_target`, `sam_set_monster_target`.

**Modify existing content (all revert automatically when the mod unloads):**
- `sam_patch_class(class, { STR, DEX, …, MAXHP, skills })` / `sam_unpatch_class(class)` — override a class's *starting* stats (vanilla or custom).
- `sam_patch_item(item, { weight, value, attributes, … })` — override an item type's base fields, live.
- `sam_patch_monster(monster, { HP, MAXHP, STR, … })` — override a monster type's base stats for future spawns.
- `sam_add_class_passive(class, effect)` / `sam_remove_class_passive(class, effect)` — grant a class a permanent status effect at character creation.

## Structured logging

`sam_log.txt` is now a proper session log: a boxed header (version, session #, date/time, Barony version, PID), phase sections (INIT / MOD LOAD / GAMEPLAY / SESSION SUMMARY), a load-summary count table, `[HH:MM:SS]` + session-relative timestamps during gameplay, `!!!`-prefixed errors, a session summary (duration, hooks fired, API calls, script errors) written even on a hard window-close, and rotation that keeps the last 5 sessions.

## Notes for mod authors

- `sam_patch_class` / `sam_patch_item` operate on per-machine data and are **not** host-gated — in multiplayer, call them from a deterministic load hook on **every** peer so stats stay in sync. `sam_patch_monster` and the class-passive APIs are host-only (their apply sites already are).
- For an exact fixed monster HP, also zero the `RANDOM_*` spreads (e.g. `{ MAXHP = 300, RANDOM_HP = 0, RANDOM_MAXHP = 0 }`).
- Class-stat / class-passive patches affect characters created **after** the patch (they bake at creation). Run them at mod-load, before character select.

## Install

Run the installer (or drop the attached `barony.exe` + `typescript.js` into your Barony folder next to each other). To revert, restore your `barony_vanilla.exe` backup or verify game files through Steam.
