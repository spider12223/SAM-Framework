# Crafting recipes

Make your own items craftable at a bench.

The framework ships its own bench, the **Hunter's Workbench**. Every player who has the
framework already has it: nothing to download, nothing to depend on, no art to ship. You can
also turn one of your own items into a bench of its own.

**The vanilla tinkering kit is off limits.** A mod cannot add to it, re-price it, or hide
anything on it. Vanilla crafting is always exactly what the game ships, whatever is installed.

---

## A recipe

`recipes/bone_axe.json`

```json
{
  "item": "mymod:bone_axe",
  "kit": "sam:hunters_workbench",
  "materials": [ { "item": "mymod:monster_bone", "count": 3 } ],
  "skill_level": 0
}
```

Then list it in `mod.json`:

```json
"recipes": [ "recipes/bone_axe.json" ]
```

That is the whole thing. `item` is what it makes, `kit` is the bench it appears on.

---

## kit is required

A recipe with no `kit` **appears nowhere**, and the log says so. That is deliberate: the
alternatives would be changing vanilla's grid, or picking a bench on your behalf.

| `kit` | Meaning |
|---|---|
| `sam:hunters_workbench` | The framework's bench. Every player has it. |
| `mymod:my_bench` | One of your own items becomes a bench. Using it opens a grid holding only the recipes bound to it. |

Only your **own** items can have recipes. Naming a vanilla item is ignored, since that would
mean changing the vanilla kit.

---

## Paying for a craft

Either scrap or your own items, not both.

**Scrap** is the vanilla currency, salvaged from junk:

```json
{ "item": "mymod:bone_axe", "kit": "sam:hunters_workbench", "metal_cost": 6, "magic_cost": 2 }
```

The total must be at least 1. A recipe costing nothing renders in the grid and then
permanently fails to craft, because the engine's affordability check bails when both are zero.

**Your own items** — up to two kinds, any quantity:

```json
"materials": [
  { "item": "mymod:monster_bone", "count": 3 },
  { "item": "mymod:sinew", "count": 1 }
]
```

Give each material an `icon` in its item JSON. The crafting panel draws that icon beside the
number; without one it falls back to the scrap icon, which reads as the wrong material.

---

## Skill requirement

`skill_level` is a **tier from 0 to 5**, not a raw number. Each tier is 20 points of
(Tinkering skill + PER), so the tiers are 0/20/40/60/80/100. The crafting screen recovers the
number by multiplying the tier by 20, so anything in between would display wrong.

---

## The grid

A bench has a fixed **5 x 4 = 20 cells**, and a custom bench gets all 20 to itself. Recipes
fill the next free cell left-to-right, top-to-bottom. Pin one if you want:

```json
"slot": { "x": 2, "y": 1 }
```

A recipe with nowhere to go is **invisible and unclickable in game**. The Recipe Editor's grid
preview shows the free-cell count per bench, so you find that out before shipping rather than
after.

---

## Giving the player the bench

They need it in hand to open it. Put `sam:hunters_workbench` in a class's starting gear (it is
in the Class Editor's picker), or hand it out from a script.

---

## Making your own bench

Any custom item can be one: name it as a recipe's `kit`. It then opens its own grid holding
only its own recipes. Add a `kit_ui` block to that item to give it your own panel art:

```json
"kit_ui": {
  "base": "ui/my_base.png",
  "drawer": "ui/my_drawer.png",
  "cost_backing": "ui/my_cost.png"
}
```

Every role is optional; anything left out keeps the vanilla panel art. Art must match the
vanilla pixel sizes:

| Role | Size |
|---|---|
| `base` | 334 x 312 |
| `drawer` | 210 x 256 |
| `name_plate` | 220 x 42 |
| `cost_backing` | 144 x 34 |
| `scrap_backing` | 176 x 36 |
| `item_surround` | 27 x 27 |
| `prompt_left` / `prompt_right` | 56 x 26 |
| `filter_left` / `filter_center` / `filter_right` (+ `_high`) | 74 / 94 / 82 x 36 |
| `arrow_left` / `arrow_right` | 30 x 44 |
| `close` / `close_high` / `close_press` | 26 x 26 |

---

## Changed in v1.8.0

Two forms from v1.7.0 were removed, because the Hunter's Workbench removes the need for them:

- Naming a **vanilla item** to re-price its recipe. Ignored now.
- `"remove": "..."` to hide a vanilla recipe and free a cell. Ignored now.

Both are reported in the log rather than failing silently, and `kit` became required.
