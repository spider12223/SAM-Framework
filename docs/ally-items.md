# Gear on allies (unreleased)

A mod can already make a monster follow a player (`sam_make_follower`, `sam_party_spawn`). This
adds what it could not do with one: read what an ally carries and wears, ask whether it may wear
something, dress and undress it, and move items between a player's backpack and the ally.

Every function and event is in [function-reference.md](function-reference.md) under **Party** and
**item**. This page is the guide. All of it runs on the host: monsters are the host's.

## What an ally is

A creature that follows a player, whichever mod made it. `sam_get_follower_leader(uid)` gives the
player index (0 to 3) it follows, or nil for a wild monster, a player, or a uid that is nothing.

An ally has *pockets* (a list of items it carries) and *worn slots*: helmet, breastplate, gloves,
shoes, shield, weapon, cloak, amulet, ring, mask. Barony counts the pockets as full at 9 items
(5 for an automaton).

```lua
local inv = sam_monster_get_inventory(uid)
-- inv.pockets = { { uid, type, name, count, beatitude, status, identified, equipped, appearance }, ... }
-- inv.worn    = { helmet = { ...same fields... }, weapon = { ... } }   only the slots that are filled
```

An item an ally wears is listed under `worn` only, even when the engine also keeps it in the pocket
list.

## The identity rule

The item you move is the SAME item: equipping, unequipping and handing a whole stack to an ally
keep its uid, so a uid you stored still names it. Two exceptions, both because the engine makes a
new item there:

- a part of a stack (`count` smaller than the stack) makes a new item for the part; the rest stays
  under the old uid;
- an item taken out of the pockets into a backpack is placed by Barony's own pick-up, which may
  join a stack. `item.on_ally_transfer_done` tells you the uid it ended up under (`new_item_uid`).

For a player on another machine the item is made new on the ally's side as well (see below).

## Wearing

```lua
local r = sam_monster_can_equip(uid, itemUidOrNameOrType, "helmet")   -- { ok, reason, text }
local r = sam_monster_equip_item(uid, "helmet", itemUid)            -- the item is in the pockets
local r = sam_monster_unequip_to_inventory(uid, "helmet")
```

`can_equip` moves nothing. It checks, in this order, and answers the reason of the first that
fails: `no_monster`, `no_ally`, `bad_slot`, `unknown_item`, `wrong_slot` (a helmet in the shoe
slot), `race` (the species rules the engine itself uses for what a monster can wear),
`cursed_in_slot` (what is worn there is cursed and cannot come off), then
`item.on_before_monster_equip`. `equip_item` adds `not_in_pockets`. `unequip` can answer
`empty_slot`, `cursed` and `pockets_full`. `text` is a sentence you can show a player.

Whatever was worn in the slot goes to the pockets; nothing is dropped.

### A rule of your own

The framework has no rules of its own about who may wear what beyond the engine's. A mod adds
one by listening:

```lua
function on_event(e)
  if e.name == "item.on_before_monster_equip" and e.item_name == "IRON_HELM" then
    e.message = "Only knights wear that."
    return false        -- the caller gets reason mod_veto and your message
  end
end
```

Every mod receives the event: check `sam_party_get_info(e.uid)` or your own tag before deciding
for an ally that is not yours.

## Moving between a player and an ally

```lua
local r = sam_give_item_to_ally(player, allyUid, itemUid, count)   -- from the player's backpack
local r = sam_take_item_from_ally(player, allyUid, itemUid, count) -- from the ally's pockets
```

`count` left out, or 0, is the whole stack. `r` is `{ ok, reason, text, ticket }`. **ok means
accepted, not done.** The outcome always arrives as an event with the same `ticket`, so a mod has
one code path:

- `item.on_ally_transfer_done { player, uid, ticket, item_uid, new_item_uid, count }`
- `item.on_ally_transfer_failed { player, uid, ticket, item_uid, reason }`

```lua
local mine = {}
local r = sam_give_item_to_ally(0, ally, itemUid, 0)
if r.ok then mine[r.ticket] = true else sam_log("refused: " .. r.reason) end

function on_event(e)
  if e.name == "item.on_ally_transfer_done" and mine[e.ticket] then
    sam_log("moved " .. e.count .. ", now uid " .. e.new_item_uid)
  end
end
```

Nothing moves until the next game tick, so the call is safe from any event handler. Everything is
checked again then, because the game went on in between.

Refused at the call: `no_ally`, `not_owner` (the ally follows someone else, or the player is not
here), `unknown_item`, `equipped` (take it off first), `cursed`, `busy` (that item is already
being moved), `pockets_full`, `backpack_full`, `bad_count`, `stock_peer`.
Failed after being accepted: `ally_gone`, `player_gone`, `item_gone`, `equipped`, `cursed`, `bad_count`,
`pockets_full`, `backpack_full`, `stock_peer`, and for a player on another machine `link_reset`.

`backpack_full` is Barony's own test (the backpack has no free slot); it refuses even an item that
would have joined a stack.

### A player on another machine

The item is in that player's own game, so the host asks it: it takes the item off itself (the
whole stack or the part) and answers with every field, and the ally's copy is made when the answer
arrives, a few ticks later. `new_item_uid` is then the new pocket item. A take to such a player is
delivered through the game's own pick-up packet, the player's game gives it its own uid, so
`new_item_uid` is 0.

Both need the player's game to run S.A.M (a mod item would arrive as a rock in a stock game), so a
player who has not said hello answers `stock_peer`, and nothing is sent.

If the player's connection is repaired, or a new run begins, while the question is out, the
transfer fails with `link_reset` and the log says the item **may** already have left the player.
That is the one case where "nothing is lost" does not hold; a player who leaves gives
`player_gone`.

## Turning it off

Nothing here changes a game that never calls it, and nothing is sent to a client unless a mod calls
`sam_give_item_to_ally` or `sam_take_item_from_ally` for that player. A stock player can join a
game that uses it.

## What was and was not run

Run: the engine-level checks (real framework code against the real game code, a check for each
feature) for the reasons, the identity rule, curses, species rules, the veto, both directions of
a transfer on the host's own player, and every ticket outcome including the remote-player
protocol against a faked channel. On a Windows build, `examples/ItemMoveTest` passes in a running
game (56 checks), and fails on the shipped 3.12.1 exe, which has none of these calls.

Not run: anything online with two machines; whether other players see an ally's new helmet after the ally is relinked or the game
is saved and loaded (read in the code, not watched).
