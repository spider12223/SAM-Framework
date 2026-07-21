# Tinkering recipes

Add your own items to the tinkering kit's crafting grid, re-price a vanilla recipe, or hide
one to make room.

The tinkering kit is Barony's crafting item. A player salvages junk into **metal scrap** and
**magic scrap**, then spends that scrap to build gadgets. Until now a mod could add an item
but had no way to let the player *build* it. Recipes fix that.

---

## Declaring recipes

Add a `recipes` array to `mod.json`, listing one JSON file per recipe:

```jsonc
{
  "namespace": "steelworks",
  "items":   ["items/steel_bomb.json"],
  "recipes": [
    "recipes/steel_bomb.json",
    "recipes/cheap_trap.json",
    "recipes/hide_backpack.json"
  ]
}
```

### Add a recipe

```jsonc
{
  "item": "steelworks:steel_bomb",  // your own item, or a vanilla name
  "metal_cost": 12,
  "magic_cost": 6,
  "skill_level": 2,                 // tier 0-5, see below
  "status": "EXCELLENT",            // optional, this is the default
  "slot": { "x": 1, "y": 3 }        // optional; omit to take the next free cell
}
```

### Re-price a vanilla recipe

Name a vanilla item and it re-prices and re-gates the **existing** entry. It does not add a
second one, and it does not use up a grid cell.

```jsonc
{ "item": "TOOL_FIRE_BOMB", "metal_cost": 1, "magic_cost": 1, "skill_level": 0 }
```

### Hide a vanilla recipe

```jsonc
{ "remove": "CLOAK_BACKPACK" }
```

---

## The grid holds 20, and vanilla uses 16

The crafting drawer is a fixed **5 x 4 = 20 cells**. Vanilla fills 16 of them, so **only 4
recipes fit** until you hide some. Hiding a vanilla recipe frees its cell, which is why
`remove` matters as much as `item`.

If a recipe has nowhere to go it is **invisible and unclickable in game**. S.A.M writes a
loud warning to `sam_log.txt` when that happens, and the **Recipe Editor** in the Mod Builder
shows a live grid preview with a free-cell count, so check there before shipping.

---

## Skill is a tier, not a raw number

`skill_level` is **0 to 5**. Each tier is 20 points of the player's Tinkering skill plus PER:

| tier | needs |
|---|---|
| 0 | 0 |
| 1 | 20 |
| 2 | 40 |
| 3 | 60 |
| 4 | 80 |
| 5 | 100 |

Tiers rather than raw numbers because the crafting screen displays the requirement as
`tier x 20`. A number in between would show the wrong figure to the player.

Note the skill is called **Tinkering** in game but `PRO_LOCKPICKING` internally, so a class
that should craft well wants points in Lockpicking.

---

## Salvage yield

What an item breaks down into is a property of the **item**, not the recipe:

```jsonc
{
  "id": "steelworks:steel_bomb",
  "attributes": { "TINKER_SALVAGE_METAL": 4, "TINKER_SALVAGE_MAGIC": 2 }
}
```

Omit these and the item cannot be salvaged. **Keep the yield below the craft cost**, or a
player can craft and salvage the same item forever to mint infinite scrap.

---

## Gotchas

**A recipe must cost at least 1 scrap.** The engine's affordability check bails when both
costs are zero, so a free recipe would show in the grid and then permanently fail to craft.
S.A.M rejects it at load with an error instead.

**Item names are not what you might guess.** Barony gives some items a different name in its
lookup table than the one used elsewhere, and a third name on screen. The one you write in
JSON is the lookup name:

| you write | shows in game as |
|---|---|
| `TOOL_FIRE_BOMB` | flame trap |
| `TOOL_DETONATED_CHARGE` | detonator charge |
| `SPELLBOOK_CHARM` | spellbook of charm monster |
| `QUIVER_HEAVY` | knockback quiver |

Pick items from the Mod Builder's dropdowns and you always get the right name. If you hand
write JSON and the item never appears, check `sam_log.txt` for
`Recipe for 'X' could not be resolved`.

**Hiding a recipe hides crafting only.** The item can still be salvaged and repaired, and
still spawns in the world normally.

**Scrap types are fixed.** Recipes cost metal and magic scrap. Custom currencies are not
supported.

**Multiplayer.** Recipes never travel over the network, so a modded client shows extra
recipes and the host neither knows nor cares. But a crafted **custom item** does travel when
dropped or equipped, so everyone in the lobby needs the same mods for custom-item recipes.

---

## Iterating quickly

`/sam_reload` re-reads every loaded mod's JSON and scripts from disk without restarting the
game. Edit a recipe, run it, reopen the kit. Singleplayer only. A brand-new `.vox` model, or
a change to a mod's item list while you are holding one of its items, still wants a restart.

Useful test commands:

```
/sam_give tool_tinkering_kit
/sam_give tool_metal_scrap 100
/sam_give tool_magic_scrap 100
```
