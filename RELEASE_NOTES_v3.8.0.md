# S.A.M Framework v3.8.0

**A script can now move a player through the game's own controls.** Until now a script could only place
or shove a player (`sam_set_position`, `sam_move_entity`, `sam_apply_force`): the body changes place
without walking, and none of them can turn the player or press Use. A modder asked for a way to "hold W"
from a script. Four new functions do that: a script holds one of the game's controls down as if its key
were held, taps it, lets go, or turns the player to face an angle. The held control goes through the same
code as the real key, so a player on keyboard and mouse walks with the walk animation and turns, and Use
picks up what they are looking at. For a player on a game controller, walking, turning and looking come
from the stick and a held one does nothing (see below). 343 functions (four new), 87 events (unchanged).

## The four functions

| function | what it does |
|---|---|
| `sam_hold_input(player, action [, strength])` | holds a game control down until it is released |
| `sam_press_input(player, action [, ticks])` | taps it: held for `ticks` game ticks (3 if left out, 1 to 3000), then let go by itself |
| `sam_release_input(player [, action])` | lets go of one control, or of every control the scripts hold for that player |
| `sam_set_facing(player, yaw)` | turns the player to face an angle at once, in radians, the number `sam_get_facing` gives |

```lua
-- walk player 0 toward tile (tx, ty), then press Use on what is in front of them
-- (to pick up something on the floor, hold "Look Down" for a moment first)
local x, y = sam_get_position(sam_get_player_uid(0))
sam_set_facing(0, math.atan(ty - y, tx - x))
sam_hold_input(0, "Move Forward")
-- later, once the player has arrived:
sam_release_input(0)
sam_press_input(0, "Use")
```

- **The controls** are the game's own Controls names: `Move Forward`, `Move Backward`, `Move Left`, `Move
  Right`, `Turn Left`, `Turn Right`, `Look Up`, `Look Down`, `Quick Turn`, `Use`, `Attack`, `Cast Spell`,
  `Defend` and `Sneak`. Any other name (the menus, chat, the pause key) is refused with a warning in
  `sam_log.txt`: a script that opened those would be fighting the player for the screen.
- **`strength`** (0 to 1, default 1) is how far an analog control is pushed, which sets how fast `Turn
  Left` and `Turn Right` turn. 0, a negative number or NaN counts as 1. In Lua a value that is not a number
  at all (a table, a boolean, text) is an argument error; leave `strength` out for the default.
- **The player's own keys keep working** alongside a held control, and `sam_release_input` never touches
  them.
- **A tap is counted from the frame the game first sees it**, not from the call, so even a one-tick tap
  reaches the game. Calling `sam_press_input` again for the same control restarts the tap.
- **A hold lasts until it is released**, or until a new run starts or the game ends (read in the code).
  Release it, or the player keeps walking into a wall.
- **A held `Use`, `Attack`, `Cast Spell`, `Defend`, `Sneak` or `Quick Turn` counts as held** for
  `sam_is_action_held` (the test saw it for `Use`), and fires `on_action_pressed` when it goes down and
  `on_action_released` when it ends, like the real key (read in the code, not run). A handler that presses
  `Use` when `Use` is pressed is not called again by its own press, because the control is already down;
  the hold just lasts longer. A handler that presses `Use` when `Use` is released calls itself again and
  again.
- **A player on a game controller is the exception for Move, Turn and Look**: the game moves, turns and
  looks from the stick for them, so a held `Move Forward`, `Turn Left`, `Look Up` and so on does nothing
  (read in the code, not tried). `Use`, `Attack`, `Cast Spell`, `Defend`, `Sneak` and `Quick Turn` are read
  from the control for them too. Otherwise a held control does what the real key does, and where the real
  key does nothing (the game paused, for example), a held one does nothing either.
- **No key needs to be bound**: the hold is added to the control itself, so it should also work for a
  control the player left unbound, such as `Quick Turn`, which has no keyboard key by default (read in the
  code, not tried).
- **`sam_set_facing`** wraps the angle into 0 to 2 pi, and refuses NaN, an angle beyond a million either
  way, and a player with no body (in Lua an argument that is not a number at all is an argument error). It
  turns only left and right; to look at something on the floor, hold `Look Down`. The camera turns with the
  body.
- **Multiplayer:** each of the four runs on the machine the player plays on, where their buttons are read.
  The host's call about a joiner is carried to the joiner's machine and returns true once it is sent; a
  joiner whose game does not run S.A.M is refused with a warning, and so is a call made by a script on a
  joiner's own machine. The joiner's machine then reports the new facing to everyone the way it reports any
  turn. All of this is read in the code, not tried.

## Also

- The scripting reference has a new section, "Moving a player from a script", with a worked example: a
  registered action that walks the player who pressed it two seconds forward, turns them around and
  presses Use.
- The function reference and `sam.d.ts` list the four functions, and the Mod Builder Dashboard's "New in"
  panel describes 3.8.0.

## Numbers

343 script functions (339 in 3.7.0), 87 events (unchanged). No new JSON field. One engine file touched:
`input.cpp`, one block in `Input::update` that adds a script's held control on top of the real keys. It is
skipped unless a script holds something, so without a mod the game reads its controls exactly as before,
and nothing new crosses the network unless a script drives a joiner.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard** (`MoveTest` with its JavaScript twin `MoveTestJS`,
singleplayer, keyboard and mouse): 28 checks pass.

- Facing: in Lua `sam_set_facing(0, 1.0)` reads back 1.0 from `sam_get_facing` and 7.0 is wrapped to about
  0.717; in JavaScript 2.0 reads back 2.0. NaN is refused in both.
- Walking: holding `Move Forward` for two seconds moved the player at least one tile along the direction
  they faced, and the legs swung while walking (the leg's pitch moved by more than 0.2), which a teleport
  never does.
- Stopping: after `sam_release_input` and a moment for the last of the momentum, the player did not change
  tile for half a second.
- Turning: `Turn Right` held for half a second turned the player by more than 0.2 radians, and at strength
  0.5 by between 30% and 70% of that. From JavaScript, `Turn Left` turned the player the other way.
- A tap: `sam_press_input(0, "Use", 10)` counted as held for `sam_is_action_held` three ticks later and was
  no longer held 30 ticks after that.
- Use: a ruby was put on the floor in front of the player, who looked down (`Look Down` held) and tapped
  `Use`; the ruby ended up in the backpack. That is the game's own pick-up, reached only through the
  control.
- Refusals: `Pause Game` is refused in Lua and in JavaScript, and a player who is not on this machine is
  refused in Lua. From JavaScript, releasing with no control named, and with `undefined`, both returned
  true; the test did not check that a hold ended.

On the shipped 3.7.0 build the test stops at its first check, because the four functions do not exist
there (0 passed, 1 failed), so that run only shows the test notices they are missing.

**The regression suite on the release build:** Hello 11, Speed 41, Settings 88, Loot 90, Audio 31, Vision
17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27, `QuickWinsTest` 10 as a Ratkin and
10 as a Skelkin (each with its 23 log checks), and `MoveTest` 28, with no failures. Three of the tests have
checks that need a person at the keyboard and nobody did them: Vision's sneaking checks, the tin in both
Neutral runs, and HelmTwin's checks where the player wears the items.

**Outside the game:** the Mod Builder's tests (180 for the block builder, 25 for the effect-name check)
pass and it builds; the function reference and `sam.d.ts` match the code.

**Not tested:**

- **Multiplayer**, for all four: a host driving a joiner, the facing reaching the other machines, and the
  refusal on a joiner's own machine. Splitscreen, where each seat has its own controls, was not tried
  either.
- **A player on a game controller**, for whom Move, Turn and Look holds do nothing (read in the code).
- **The controls that were not held by the test**: `Move Backward`, `Move Left`, `Move Right`, `Look Up`,
  `Quick Turn`, `Attack`, `Cast Spell`, `Defend` and `Sneak`. The hold is added to them by the same block of
  code as to the tested ones, but what the game does with each was not tried.
- **`Use` on a door, a chest or a creature**: only picking up an item was tried.
- **Footsteps**: they come from the same walking code as the leg swing the test measured (none for rat,
  spider or imp bodies, or while levitating or swimming), but nobody listened for them.
- **A paused game, an open menu or inventory**, an unbound control, a hold carrying over a floor change,
  and a hold ending when a new run starts (read in the code).
- **`on_action_pressed` and `on_action_released` firing for a held control**: read in the code, not run.

## Not in this release

Still open from earlier requests: an icon upload button in the Effect Editor, spells aimed at a tile or a
touched creature, bows that fire like a real shortbow, items that give stats while worn, a script box in the
Item, Spell and Effect editors, and attack animations for a custom weapon.
