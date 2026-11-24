# S.A.M Framework v3.14.0

**Party members, lobby cards and gear on allies, contributed by RumpleIVSkin.** A mod can now put a creature in the
party the way a player is in it, let the host pick it in the lobby from a player slot, and dress it and hand it items.
21 new script functions and 9 new events: 365 functions, 96 events. It was reviewed, fixed and tested here before
shipping, and RumpleIVSkin is credited in the README.

## Party members: `sam_party_*`

```lua
local uid = sam_party_spawn(0, { name = "Brienne", race = "human", class = "warrior", sex = "female" })
```

That is a whole companion. It starts from a player's baseline stats with its class's stats and kit, worn where a
player wears it (a S.A.M custom class works too, through the same hook a player's does), and follows player 0. Call it
on the host once the game has started (a tick after `game.on_game_start`): with no leader standing in the world yet it
returns nil and says why in the log.

- It levels up through its class's growth (an option, on by default), not a monster's roll. It is left out of the
  follower count when its leader recruits with Leadership; a jewel recruit still counts it.
- It stays on the players' side when its leader goes down, takes every order in the follower menu whatever the
  leader's Leadership, and cannot be charmed or dominated away from the party.
- Every exit waits for it (`wait_at_exit`, on by default): `party.on_waiting_at_exit` fires and the missing members are
  sent to the exit. A mod can let the party go; if nothing does, a try 20 seconds into the wait goes anyway, and the
  member still comes along, as every follower does.
- When it dies, `party.on_member_died` fires before its gear drops (return false and it stays dead). With `respawn` on
  it comes back beside its leader on the next floor, at full health, and with no gear unless Keep Inventory is on.
- `sam_party_get_info`, `_set_options`, `_list`, `_fallen` and `_is_member` read and change members;
  `sam_party_choices`, `_choice_allowed` and `_random_name` give a mod what the character screen knows.
- A member whose player leaves the game follows a player still in it.

**When every player is down.** Off unless a mod asks: with `carry_on = true` on a member that still stands, the run
does not end when the last player falls. The fallen wait as ghosts while the members walk to an exit (never a secret
one) and take the party down; on the next floor the fallen stand up at half health, with co-op's rule for their gear.
When the last such member falls the run ends as usual, and `game.on_game_end` fires then, once (before that member's
own `party.on_member_died`). Online this needs S.A.M on every player's machine; with a player on a stock game, the run
ends with the last player, as in vanilla.

**Difficulty.** `sam_party_set_difficulty_count(n)`, with n from 0 to 3, called while the lobby is open, builds the run
for the connected players plus n (at most 4 in all), the same way Barony scales for each connected player. The host
decides it when the run starts, and only when every player runs a S.A.M that reads it, so every machine builds the
same dungeon; otherwise the count is 0 and the host's log says why.

## Lobby cards: `sam_lobby_*`

`sam_lobby_add_slot_action` puts a link on the host's empty player slots ("+ Add Companion"): up to 4 links across
all mods, of which an empty slot draws the first 2. `sam_lobby_set_card` claims a slot and shows your own card in it: a
title the host can type over, and up to five rows of text, cycles (< and >) and buttons, with `lobby.on_slot_action`
and `lobby.on_card_input` telling your mod what the host pressed. A player whose game runs S.A.M sees the card
read-only; a player on a stock game sees a locked slot. Cards exist only while a lobby is open (a lobby for a saved
game takes none), so keep what the host chose in your script and use it from `game.on_game_start`.

## Gear on allies

`sam_monster_get_inventory` reads what any monster carries and wears. For an ally, `sam_monster_can_equip`,
`sam_monster_equip_item` and `sam_monster_unequip_to_inventory` dress it under the game's own rules (slot, body,
curses), with `item.on_before_monster_equip` for a mod's own rules. `sam_give_item_to_ally` and `sam_take_item_from_ally`
move an item between a player's backpack and an ally's pockets. A whole stack handed over from a backpack on the host's
machine stays the same item, with the same uid; otherwise `item.on_ally_transfer_done` gives the uid it ended up under.
The outcome arrives as `item.on_ally_transfer_done` or `_failed`, never from the call itself, so the call is safe from
any handler. `sam_get_follower_leader` tells you whose ally a monster is.

For a give from a player on another machine, the host asks that player's game, which takes the item off at once and
answers; the ally's copy is made from the answer (the outcome comes when it arrives), and a game that does not answer
in 5 seconds fails the move with `no_answer`. A take to that player is handed over as the game's ordinary pick-up.

## Also in this release

- **The Mod Builder's API Reference page showed only 15 of its categories**, so 191 of the 365 functions never appeared
  there, among them every camera, combat, loot and panel function. It now shows every category.
- `tools/run_mod_test.sh --probe <name>` runs one of the framework's own engine-level checks in a test run.

## Numbers

365 script functions (21 new), 96 events (9 new). Three new framework modules (party, lobby, ally items), and S.A.M
call sites in 11 engine source files. The lobby adds one message of its own (`'SAML'`, in the lobby only); the
difficulty count rides the game's own start packet; party members and item moves to another machine use S.A.M's
existing multiplayer channel. Between two S.A.M games the lobby always trades that one short hello, cards or not; a
player on a stock game is sent nothing new.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:**

- `examples/PartyTest`, 45 checks: spawns three members, reads and changes them, checks the refusals, kills them (one
  comes back, one has respawn off, one is kept dead by a handler), takes the stairs, and checks that only the first
  rejoined beside player 0. Lobby cards, carry_on and online play are not in it.
- `examples/ItemMoveTest`, 56 checks, on the host's own player: a human and a kobold ally, an item handed over and the
  same uid found in the pockets, the race rule, a handler's veto, equipping and unequipping by uid, taking it back, and
  a cursed helmet that stays on.
- `tests/PartyFixTest`, 13 checks and 2 log checks: a Lua uid above 32 bits is refused instead of reaching another
  member; with a carry_on member standing, killing the player fires no `game.on_game_end`, and killing the member then
  fires it once, won 0, naming the player; and the player's half of a give from another machine takes the item off
  at once, whole or in part. On the build before these fixes 5 of its checks fail, and its log check fails when the
  old queued removal is put back.
- On 3.13.0 none of these runs: the functions do not exist.

The regression script (`handoff/run_regress.sh`) was run on the release build, with no reruns: Hello 11, Speed 41,
Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27,
`QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with its 28 log checks), `MoveTest` 28, `SwayTest` 13,
`RootSwayTest` 21, `CompendiumTest` 2 (with its 27 log checks), `TauntTest` 4 (with its 6 log checks), `EmoteTest` 3,
3 and 1 for its three races (with 2, 2 and 3 log checks), `MomentumTest` 26, `PartyTest` 45, `ItemMoveTest` 56,
`PartyFixTest` 13 (with its 2 log checks) and the installer layout check 14, with no failures. Three of these tests
have checks that need a person at the keyboard and nobody did them: Vision's sneaking checks, the tin in both Neutral
runs, and HelmTwin's checks where the player wears the items. `ChargeTest`, which needs a person to swing at a goblin,
is not in the script and was not run.

**Not tested:**

- **Anything online with two machines**: lobby cards and links, the hello, the difficulty count, gear moving to and from
  a player on another machine, a member following another player when its own leaves, carry_on online, and the rule
  that a stock player ends the run. All of it is read in the code, not run.
- **carry_on past the moment checked above**: the ghosts, the members walking to an exit, the next floor, the gear
  rule: read in the code, not run.
- **The lobby card itself**: drawing it and pressing its controls needs a lobby, and nobody here opened one with a mod
  using it.
- **Read in the code, not run**: the charm and Dominate guard, the host taking the Attack order, secret exits being
  passed over, the 20-second exit fallback, and the `no_answer` timeout.

**Known limits:**

- A member that died and has not come back yet is not in a mid-floor save.
- A saved member keeps its class and race by number, so adding or removing a mod with classes or races between saving
  and loading can change them.
- The host cannot check the room in a backpack on another machine, so a take to that player is handed over the way the
  game hands them any item.
- If a player's connection is repaired, or they leave, while their give is on its way, the item may be lost (the log
  says so).
- After an online run that counted party members toward difficulty, a Hall of Trials or demo started in the same
  session still uses that count.
- A card's buttons are reached with the mouse; controller navigation onto them is not wired yet.

Thanks to RumpleIVSkin for the whole feature.
