# Party members and lobby cards

Two things a mod could not do before 3.14.0: put a creature in the party the way a player is in
it, and let the host choose one in the lobby, in a player slot.

- **Party members** (`sam_party_*`): a monster on the players' side that starts with a class's
  kit, grows like that class, is not anybody's recruit, stays loyal when the player it follows
  goes down, is waited for at exits, and comes back on the next floor.
- **Lobby cards** (`sam_lobby_*`): a link on every empty player slot ("+ Add Companion"), and a
  card of your own in a slot you claim, with text, cycles and buttons the host presses.

Both are general. Companions are the obvious use (a companion mod is the obvious use), but a
hireling you pay at a shop, a pet that starts the run with you, or a "bring a friend's ghost"
mod need exactly the same two things.

Every function and event is in [function-reference.md](function-reference.md) under **Party**
and **Lobby**. This page is the guide.

---

## A party member in ten lines

```lua
function on_event(e)
  if e.name == "game.on_game_start" and e.player == 0 then
    sam_set_timer("mymod_spawn", 1, function()
      local uid = sam_party_spawn(0, { name = "Brienne", race = "human", class = "warrior", sex = "female" })
      sam_log("Brienne is uid " .. tostring(uid))
    end)
  end
end
```

That is a whole companion. She starts from a player's baseline stats with the warrior's stats
and kit (spear, shield, helm, breastpiece, boots, the food), follows player 0, fights what player
0 fights, levels up like a warrior, and the stairs wait for her.

Spawn a tick after `game.on_game_start` rather than inside it: the leader has to be standing in
the world for her to stand beside.

### What `sam_party_spawn` takes

| key | default | |
|---|---|---|
| `name` | a random player name | up to 31 characters |
| `race` | `"human"` | an id from `sam_party_choices()`: `"goblin"`, `"automaton"`, or a S.A.M custom race (`"mymod:lizardfolk"`). A number works too. |
| `class` | `"warrior"` | an id from `sam_party_choices()`, or a S.A.M custom class (`"mymod:knight"`) |
| `sex` | random | `"male"` or `"female"` |
| `appearance` | 0 | 0 to 17, the character screen's looks (humans only) |
| `tag` | your namespace | anything up to 31 characters you want back later: `"mymod:2"` |
| `wait_at_exit` | true | every exit waits until it is within 4 tiles |
| `respawn` | true | after it dies, it comes back beside its leader on the next floor |
| `grow_like_class` | true | level-ups raise stats the way its class does for a player |
| `carry_on` | false | when every player is down and it still stands, the run goes on (see below) |
| `pickup` | `"unowned"` | what it picks up off the floor: `"unowned"`, `"all"` or `"none"` |

A custom class works the way it does for a player: its stats, passives and overrides come
through the same `initClassStats` hook, and its `starting_items` are its kit. Two things a
monster does not do: learn a class's starting spells (it casts from the spellbooks it carries),
and take the race-specific extras some vanilla kits add for one race.

### Reading and changing one

```lua
local info = sam_party_get_info(uid)
-- { uid, name, class, class_id, race, race_id, sex, appearance, leader, tag, mod,
--   level, hp, max_hp, wait_at_exit, respawn, grow_like_class, carry_on }

sam_party_set_options(uid, { respawn = false })   -- its next death is final
sam_party_list()                                  -- every living member on this floor
sam_party_fallen()                                -- the ones coming back next floor
sam_party_is_member(uid)                          -- one integer test; cheap
```

`leader` stays the player it follows while that player is down, so a fallen member comes back beside them. A member whose player leaves the game follows a player still in it within a second (that player is told), so it crosses the next floor with the party instead of staying behind.

---

## What makes it a party member and not a pet

| | a recruited follower | a party member |
|---|---|---|
| starting gear | the species' random gear | its class's kit, worn where a player wears it |
| level-ups | a monster's random roll | its class's growth (`playerStatIncrease`) |
| follower limit | counts against its leader's | left out when its leader recruits with Leadership (a jewel recruit still counts it) |
| its leader dies | turns hostile, or vanishes if summoned | stays on the players' side |
| an exit | comes along | waits for it (`wait_at_exit`) |
| it dies | gone | back next floor (`respawn`) |
| difficulty | nothing | can count as a player (below) |

**Loyalty.** Players, party members and anyone's followers are friends before anything else is
asked, so a mask or confusion does not change that answer. For every other creature, while its
leader is alive the normal follower rules decide friend and foe, masks and confusion included.
While its leader is down, it judges those creatures the way a living player would, by asking that
player's own `checkEnemy` / `checkFriend`. That runs the race arms for that player's race, so a
custom race's declared allegiances still apply to the whole party. (Read in the code, not run.)

**Death.** `party.on_member_died` fires before its gear drops. Return `false` and it stays dead,
which is all a permadeath option needs. Gear follows a player's rule: with Keep Inventory on it
keeps everything and the body drops nothing; without it, the body drops everything as usual and
it comes back with nothing. `e.message` is what every player is told; set it to translate it, or
to `""` for silence.

**Exits.** Every kind of exit (ladders and every portal) asks after vanilla's own "assemble your
party" check has passed. A member set to wait that is more than 4 tiles away is sent to the exit,
and the player is told who is missing. `party.on_waiting_at_exit` fires each time; `e.attempt`
counts the tries at this exit, and returning `false` lets the party go now, so a member stuck
behind a wall cannot hold everybody up (Barony still brings every follower, party members
included, to the next floor beside its leader). If nothing lets the party go, a try 20 seconds
after the first goes anyway, so a member that cannot get there never strands the players. Check
`sam_party_get_info(e.first_uid).mod` so you only give up on your own members:

```lua
if e.name == "party.on_waiting_at_exit" and e.attempt >= 3 then
  local info = sam_party_get_info(e.first_uid)
  if info and info.mod == "mymod" then return false end
end
```

**Coming back.** On the next floor, once the followers are placed, every fallen member is put
beside its leader (or any living player) and `party.on_member_spawned` fires with
`respawned = 1`. One with nowhere to stand waits another floor.

**Orders.** A party member takes every order in the follower menu (follow, wait, move to,
attack, items) whatever its leader's Leadership skill: it is a companion, not a recruit. The
host applies the same rule when the order arrives. In online play the host tells each
S.A.M client which creatures are party members, so the same holds for a friend's own companion.

**Charms.** A party member cannot be charmed or dominated away from its party: a monster's Charm
Monster, which may steal anyone else's follower, pacifies it instead, and Dominate answers as it
does for a creature it cannot take. (Read in the code, not run.)

**When every player is down.** This is off unless a mod turns it on: give a member `carry_on = true` (in `sam_party_spawn`, or later with `sam_party_set_options`). A game with no such member ends when the last player falls, as vanilla does. While a `carry_on` member still stands, the run is not over:

- The fallen get the "your party carries on" game-over window. Dismiss makes them a ghost (in
  solo too, as in co-op); the host tells every S.A.M client, so a friend's window says the same.
- Each standing member finishes the fight it is in, then walks to an exit it can reach: the
  one another member is already making for if there is one, otherwise the nearest it has a
  path to, never a secret ladder or portal (a member does not take the party to a secret level
  on its own).
- A member at an exit takes the party through, as a player would. The exit waits up to 20
  seconds for members still on their way; it says nothing and fires no
  `party.on_waiting_at_exit` meanwhile (nobody clicked).
- On the next floor the fallen stand up at half health, with co-op's rule for their gear (Keep
  Inventory), and the members are placed beside them as usual.
- Until then nothing of the end happens: the save is kept, the game-over music and `player.on_game_over`
  wait, and a solo player's gear follows Keep Inventory as in co-op instead of dropping.
- If the last standing `carry_on` member falls, or goes without dying (a mod removed it) and stays gone
  for two seconds, the run ends: every fallen player gets the final game-over window, the save is
  deleted, the music plays and the run's scores are posted, as when the last player dies.
  `player.on_game_over` fires for a player whose window was the final one; one who fell earlier
  while another player still stood has already had it fire for that death, and gets it again here.
  `game.on_game_end` (won 0) fires here, once, naming the last player who fell; it does not fire
  when that player fell, because the run had not ended then.
- `sam_party_set_options` refuses to turn `carry_on` on once no player is standing: that run has ended.
- Online, this needs S.A.M on every machine. With a player on a stock game, which would show its
  own player the final game over and could not follow the run, the run ends with the last player,
  as in vanilla, and the host's log says why.

Turning `carry_on` off on the last member that carries ends the run after the same two seconds. A floor whose every exit is out
of reach (locked, or past a boss) leaves the members waiting; the pause menu can still end the run.

---

## Difficulty

Barony makes each floor harder for each connected player: more monsters, faster hunger, more of
the Devil's shadows. A mod can ask for party members to count the same way:

```lua
sam_party_set_difficulty_count(2)   -- in the lobby: this run is built for players + 2
```

Every machine builds every floor itself from a shared seed, so the count must be the same on
every machine or the players end up in different dungeons with nothing to say so. The host
therefore decides when the run starts, and uses the count only when every connected player runs
a S.A.M that reads it (their lobby said hello). The number then rides the game's own start packet.
With a player on a stock game the run is balanced for the connected players alone, and the host's
`sam_log.txt` says why. The count goes back to 0 whenever a lobby opens; players plus count is
capped at 4.

---

## Lobby cards

```lua
sam_lobby_add_slot_action("add", "+ Add Companion")      -- top level, beside your settings

function on_event(e)
  if e.name == "lobby.on_slot_action" and e.mod == "mymod" and e.action == "add" then
    sam_lobby_set_card(e.slot, {
      title = "Brienne", edit_title = true,
      rows = {
        { cycle = "class", text = "Warrior" },
        { buttons = { "sex", "remove" }, labels = { "Female", "Remove" } },
      },
    })
  elseif e.name == "lobby.on_card_input" and e.mod == "mymod" then
    -- e.slot, e.control ("class", "sex", "remove", "title"), e.input ("prev", "next", "click", "text"), e.text
    -- change your own state, then call sam_lobby_set_card again (or sam_lobby_clear_card)
  end
end
```

**A card is a description, not a widget.** It has a title and up to five rows; a row is a line of
text, a cycle (`<` text `>`), or one to three buttons. Every press on the host fires
`lobby.on_card_input`, and you redraw by sending the whole card again. Nothing about a card is
kept except what you last sent, so there is no state to get out of step, and it works the same in
Lua and JavaScript.

| limit | |
|---|---|
| title | 31 bytes |
| rows | 5 |
| buttons in a row | 3 |
| a row's text, a label | 40 bytes (a letter outside plain English takes 2 to 4) |
| a control id | letters, digits, `_ . -`, up to 32; `"title"` is the title box's |
| slot actions | 4 across all mods; an empty slot draws the first 2 |

**Every mod receives every lobby event.** Check `e.mod` against your namespace first.

**Who sees what.** The host sees the card with its controls. A player whose game runs S.A.M sees
the same card read-only and follows every change. A player on a stock game sees a locked slot.
While you hold a slot it is locked, so nobody can join into it, and the player-count setting
leaves it alone until you give it back with `sam_lobby_clear_card`. Then the setting's word
holds again: a slot the host closed meanwhile by lowering the player count stays closed.

**What the slots hold.** `sam_lobby_get_slot(n)` answers `player` (with the name shown for them),
`card` (with the mod and title), `open` (you may claim it), or `closed` (the player-count setting,
or a saved game's lobby). A mod can use it for a "Follows: <player>" button of its own.

**When.** Cards exist only while a lobby is open: set them from `lobby.on_lobby_opened`,
`lobby.on_slot_action`, `lobby.on_card_input` or `mod.on_setting_changed`. They are forgotten when
the run starts, so keep what the host chose in your own script and use it from
`game.on_game_start`. A lobby for a saved game takes no cards and draws no links: its party is
whatever was saved.

---

## Multiplayer

| | kind | |
|---|---|---|
| `sam_party_spawn`, `_set_options`, `_get_info`, `_is_member`, `_list`, `_fallen` | host | party members are monsters, and monsters are the host's |
| `sam_party_set_difficulty_count` | host | read by the host when the run starts |
| `sam_party_choices`, `_choice_allowed`, `_random_name` | local | this machine's classes, races and DLC |
| `sam_lobby_add_slot_action` | all | every machine registers it as its mod loads; later, a host call is repeated on every client and a client's own call is refused |
| `sam_lobby_set_card`, `_clear_card` | host | the host draws cards and sends them on |
| `sam_lobby_get_slot` | local | a client knows the cards the host sent it |

Every new event fires on the host. On the wire: `'SAML'` in the lobby only (a client's hello, the
host's cards), vanilla `'LOCK'` for the slots a mod holds, and a 4-byte tail on vanilla `'STRT'` /
`'RSTR'` for the difficulty count. A host with no cards answers a hello with one short frame and
sends nothing else. On a direct connection the host takes a hello only from the slot it came
from; over Steam or EOS a client's slot is its own word, as it is in every vanilla packet.

---

## Known limits

- A party member that died and has not come back yet when the game is **saved** is not in the
  save; living members are saved as followers, as usual.
- A saved member keeps its class and race by number, and a mod's numbers follow the order the
  mods load in. Add or remove a mod with classes or races between saving and loading, and a
  member of a mod's class or race can come back as another one (a player's race is put right on
  load; a member's is not yet).
- After an online run that counted party members toward difficulty, a Hall of Trials or demo
  started in the same session still uses that count, until a run is started from a lobby again.
- In a lobby for a **saved game** there are no links and no cards, and the difficulty count is 0
  unless a mod sets it.
- Party members think with the vanilla follower AI: they fight, follow and take commands like any
  ally. Deciding when to drink a potion or cast is the next thing worth adding.
- The card is laid out for the 280 x 146 slot stone at the default UI scale, and its buttons are
  reached with the mouse; controller navigation onto them is not wired yet.
