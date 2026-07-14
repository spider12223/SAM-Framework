# S.A.M Framework v0.8.0 — Custom Spells

Target: **Barony 5.0.2** · Languages: **Lua 5.4 · JavaScript · TypeScript**

v0.8.0 adds a **custom spell system**: mods can define their own spells in JSON, and
S.A.M builds a real in-engine spell that is grantable, castable, and shows in the spell
list with its own name and icon — no C++ required.

## Define a spell

Drop a `spells/*.json` into your mod (conforming to `spell.schema.json`) and list it in
`mod.json`'s `spells[]`:

```json
{
  "id": "mymod:shadow_bolt",
  "name": "Shadow Bolt",
  "description": "A bolt of condensed shadow that saps life from its target.",
  "mana_cost": 8,
  "projectile_type": "missile",
  "payload": "drain_soul",
  "damage_min": 10,
  "damage_max": 18,
  "icon": "spells/shadow_bolt.png",
  "starting_spell": true
}
```

- **`payload`** picks the effect from 21 real Barony spell elements (force, fire, lightning,
  cold, magic_missile, poison, slow, confuse, sleep, dig, dominate, charm_monster,
  acid_spray, bleed, ghost_bolt, stoneblood, locking, opening, tele_pull, steal_weapon,
  drain_soul).
- **`projectile_type`** — `missile` (single bolt), `missile_trio` (3-way), or `none`.
- **`icon`** — a PNG from your mod folder; falls back to a generic magic orb if omitted.

## What it does

- Registers the spell into the engine's spell registry at a reserved runtime id (≥ 2000),
  rebuilt automatically whenever the game reloads spell data, and reverted on mod unload.
- **Grant it** three ways: a class's `starting_spells` (accepts `"namespace:spell"`),
  the `sam_grant_spell(player, "namespace:spell")` script API (Lua/JS/TS — also grants
  vanilla spells by name), or naturally as a class starting spell.
- The granted spell **appears in the spell list + hotbar** with its custom name and icon,
  and **casts** — firing its declared payload like any vanilla spell.

## Engine fixes that made it work

- Narrowed Barony's shapeshift appearance-remap (`appearance − 1000`) to the real
  shapeshift range so custom spell ids resolve to themselves.
- Excluded custom spells from the "unable to use in your current form" shapeshift gate.
- Hooked the spell-icon path to load a mod PNG by path (like class portraits).

## Install

Run the installer, or drop the attached `barony.exe` + `typescript.js` into your Barony
folder next to each other. To revert, restore your `barony_vanilla.exe` backup or verify
game files through Steam.
