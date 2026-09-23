# Loot and shops

Most random items that come out of the item sheet pass through one engine function, and the
framework sits beside it. Six tables shape what it returns, nine events cover what the tables
cannot, and a handful of functions rewrite a chest, a shop or a monster's pockets outright.

```lua
-- everything vanilla never rolls (item level -1) joins the pool from floor 0
for _, it in ipairs(sam_list_items()) do
	if it.level == -1 and it.category ~= "SPELL_CAT" then sam_patch_item(it.type, { level = 0 }) end
end
sam_set_loot_weight("GEM_ROCK", 0)                   -- never rolled (garbage chests still hold rocks)
sam_set_loot_weight("ARTIFACT_SWORD", 3)             -- three times as likely as a vanilla entry
sam_set_loot_context("ARTIFACT_SWORD", "shop", false) -- drops, but no shop stocks it
sam_set_loot_fallback("WEAPON", "IRON_SWORD")        -- an empty window gives a sword, not a rock
```

## The six tables

They are the same on every machine, and you set them when your mod loads.

| function | |
|---|---|
| `sam_set_loot_weight(item, weight)` | per item: 0 never, 1 vanilla, 5 five times as likely |
| `sam_set_loot_category_weight(category, weight)` | for the "any category" draw a floor, a chest or a general store makes |
| `sam_set_loot_floor_range(item, min_floor [, max_floor])` | a floor window per item, with the ceiling vanilla never had. `sam_patch_item { level }` remains the minimum-floor knob |
| `sam_set_loot_context(item, context, allowed)` | an allow-list per context: `floor`, `chest`, `shop`, `monster`, `recipe`, `console`, `other` (a `sam_roll_loot` with no context) |
| `sam_set_loot_fallback(category, item [, context])` | what an empty pool returns instead of a rock: one named item. It does not refill an empty window, so for the Hamlet rock shop widen the window in `world.on_before_loot_roll` instead |
| `sam_set_spell_droppable(spell, allowed [, min_floor])` | whether a spell's book may come out of the spellbook re-draw. This is how a class spellbook enters the pool |

`sam_get_loot_pool(category, min_level, max_level [, context])` is the candidate set the level window
and the tables allow, with each entry's weight, so you never have to guess from `sam_list_items`.
The roll itself then applies the engine's own exceptions: nine gem rolls in ten return glass before
the pool is looked at, five items drop out by chance, and a shop's type excludes some stock.

The tables do not reach items the game picks by name or from a fixed range (a goblin's weapon, the
apothecary's potions, a garbage chest's rocks) or the gold-value draw (the lockpick reward,
automatons, cockatrices), which fires `world.on_before_loot_roll` and uses the fallback but not the
weights, windows or contexts. `world.on_loot_rolled`, `world.on_monster_inventory` and the
container functions below reach most of those.
`sam_roll_loot(category, min_level, max_level [, context])` draws from it the way the engine does,
for your own drops.

A table that only removes items (weight 0, a floor window, a context) consumes the random generator
exactly as vanilla does, so everything the dungeon drew before the first affected roll is the same;
the item at that roll differs, and because a spellbook is re-drawn from the spell table, the rolls
after it can differ too. That is not a byte-identical dungeon, and a mod that promises other players
the same floors must not remove anything. A weight above 1 turns the roll into a weighted draw and
changes the sequence from there on.

## The events

| event | when | writable |
|---|---|---|
| `world.on_before_loot_roll` | before the engine draws | `category`, `min_level`, `max_level`, `min_value`, `max_value` (the gold-value draw), `item_type` (set it to skip the draw) |
| `world.on_loot_rolled` | the finished item, with `is_fallback` | `item_type`, `status`, `beatitude`, `count`, `appearance`, `identified` |
| `world.on_before_spellbook_reroll` | the item roll picked a spellbook and the engine is about to re-draw it from the spell table | `keep`, `spell_id`, `allow_hidden` |
| `world.on_before_chest_fill` | a chest is about to be filled; cancel to leave it empty and fill it yourself | `chest_type`, `min_quality` |
| `world.on_chest_filled` | the chest is done | |
| `world.on_before_shop_stock` | a shopkeeper is about to be stocked; cancel to stock it yourself | `store_type`, `num_items`, `shop_level`, `blessed` |
| `world.on_shop_stocked` | the shop is done | |
| `world.on_monster_inventory` | a creature has its starting gear, which is also its drop | |
| `world.on_fixture_loot` | the fountain, the sink, the boulder luckstone, the lockpick reward | `item_type`, `status`, `beatitude`, `count`; cancel for no item |

`world.on_before_loot_roll`, `world.on_before_spellbook_reroll` and `world.on_loot_rolled` carry
`context` (`"floor"`, `"chest"`, `"shop"`, `"monster"`, `"recipe"`, `"console"`, `"other"`) and the
uid and type of the chest, shop or monster the roll is about when there is one, so a handler can
say "only shops". `world.on_before_chest_fill` and `world.on_before_shop_stock` are already about
one chest or one shopkeeper: they carry its uid (`chest_uid`, `shopkeeper_uid`) and no `context`.

```lua
if e.name == "world.on_before_loot_roll" and e.context == "shop" then
	e.min_level = 0   -- a deep store asks for level 10+; do not let its pool go empty
end
```

## The one limit that matters

The level generator runs on a worker thread while the loading screen draws, on every machine, and
scripts cannot be entered from there. So the events fire for every roll made **during play** (a
shopkeeper's first tick, a monster's first tick, a chest summoned by a spell, the lockpick reward,
`sam_roll_loot`) and **never for the items and chests a floor is generated with**. Those are
governed by the tables, which is what the tables are for. A mod that wants to rewrite a generated
floor's chests does it from `game.on_level_entered` with `sam_find_entities` and the container
functions below.

## Containers and shops

| function | |
|---|---|
| `sam_add_item_to_container(uid, item [, count, status, beatitude, identified, appearance])` | into a chest (the client is told), a monster or a shopkeeper (the shop's layout is redone) |
| `sam_remove_item_from_container(uid, item_or_uid [, count])` | the reverse |
| `sam_set_shop_stock(shopkeeper_uid, items)` | clear the stock (the consumables row stays) and stock the list, each entry `{ item, count, status, beatitude, identified }` |
| `sam_set_shop_type(shopkeeper_uid, store_type)` | before its first tick only; after that use `world.on_before_shop_stock` |

`sam_get_container_items(uid)` reads any of them.

## Multiplayer

A client rolls its own copy of every floor from the same seed and keeps its own floor items, so the
six table writers are `all` calls, like `sam_patch_item`: call them on the host or at load, they
reach every S.A.M client and a client that joins later, and everyone's floor agrees. The host also
sends its loot tables whole to every joiner when they say hello, so a table written outside the
current run (the main menu, a settings handler, an earlier run) reaches them too. The events fire
on the host only. The container and shop writers are host-only; a chest's contents are served to
whoever opens it and a shop's when it is entered, so nothing else needs sending. A player browsing a
shop from another machine has the window closed (the engine's own SHPC) when its stock changes; an
item added to a chest they have open arrives as a new stack. A joiner without
the mod plays vanilla loot on their own floor.

`/loaditems` and the data reload at game start used to put the sheet back over every
`sam_patch_item` level; they now re-apply every live patch after the read.

## A whole mod

`examples/ItemRandomizer` is the community mod of the same name rebuilt on this, with the shop fix
and a Lite toggle. Its acceptance test is `LootTest`, which caught a real bug in `sam_roll_loot`
before anyone played it.
