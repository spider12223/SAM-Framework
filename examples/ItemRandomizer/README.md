# Item Randomizer

Every item in the game joins the common loot pool on every floor: artifacts, orbs, keys, tinkering
products, class gear, all of it, on the ground, in chests and in shops from the first floor down.
Class and race spellbooks join it wherever an item is rolled during play: a shop stocking, a
creature's pockets, a chest summoned by a spell. The books a floor is generated with are rolled on
the level loader's thread, where no script runs, and stay the ordinary spells' books. A *Lite*
toggle in Settings > General keeps artifacts and orbs out of shops.

## Why this is a script and not an items.json

The community mod of the same name replaces `items.json`. That has two costs its own comments
document: it conflicts with every mod that adds an item (there is only one sheet), and the deep
Arms & Armour stores sell nothing but rocks, because the store asks for a rare tier, the edited
sheet has nothing at that tier, and the engine's fallback for an empty roll is a rock.

This folder changes the loot **rules** instead:

| what | how |
|---|---|
| an item that vanilla never rolls becomes eligible | `sam_patch_item(item, { level = 0 })` for every item whose level is -1 |
| every listed spell's book may come out of the spellbook re-draw, from floor 0 | `sam_set_spell_droppable(spell, true, 0)` for every spell `sam_list_spells` returns |
| a class or race spellbook the item roll picked survives the re-draw | `world.on_before_spellbook_reroll` with `e.keep = 1` (those spells are hidden from the list, so the table cannot name them) |
| artifacts never become alchemy recipes, or (Lite) never sell | `sam_set_loot_context(item, "recipe" or "shop", false)`; the Lite rule is applied from `game.on_game_start` on the host, because a setting is per machine and the shop table is shared (see `docs/mod-settings.md`) |
| the deep store's minimum level no longer empties the pool | `world.on_before_loot_roll` with `e.context == "shop"`: `e.min_level = 0` |

Because none of that touches the sheet, a mod that adds its own items composes with it: custom items
enter the pool through the same rules. And a player who joins without this mod still plays, they
just find vanilla loot on their own floor.

## Finer control, if you want it

The same tables go further than the original could: `sam_set_loot_weight` makes an item rarer or
commoner (0 = never), `sam_set_loot_category_weight` shifts whole categories, `sam_set_loot_floor_range`
gives an item a ceiling floor as well as a minimum, `sam_set_loot_fallback` replaces the rock, and
`world.on_before_chest_fill`, `world.on_before_shop_stock`, `world.on_monster_inventory` and
`world.on_fixture_loot` let a script rewrite a container or reward as it is generated during play.
The chests and items a floor is generated with are governed by the tables only, because the level
generator runs on a worker thread where no script can be entered. See `docs/loot-and-shops.md`.

## Try it

Copy this folder to `mods/`, load it, start a game. The first chest you open will tell you.
