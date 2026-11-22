# S.A.M Framework v3.13.0

**A race can have its own noise, and a script can stop a player dead.** Two requests from a modder making a Catfolk
race, and an installer fix for a player who could not read its first screen. One new race field (`"emote_sounds"`)
and one new script function (`sam_stop_momentum`): 344 functions, 87 events.

## A race's own noise: `"emote_sounds"`

When a player presses Use on nothing, the game plays a noise picked by the body: a salamander growls, a dryad creaks,
a goblin grunts. The human body says nothing, so a Catfolk built on it was silent. A race can now list its own sounds:

```json
{ "id": "mymod:catfolk", "name": "Catfolk", "host_body": "human", "emote_sounds": ["purr", "hiss"] }
```

- Sound ids, declared like any other sound; a bare name is one of your own (`"purr"` is `"<namespace>:purr"`).
- One of them plays at random, and one of its files at random, at most once every five seconds on every body (the
  skeleton body's own noise can repeat every second; a race's sound cannot).
- Only in the race's own body: with "disable monster abilities" ticked, or while polymorphed or shapeshifted, the
  game's own noise stays.
- A race that lists sounds never makes its body's noise: an id no loaded mod registers is silent, with one line in
  `sam_log.txt`.
- In multiplayer the other players hear one too, picked on their own machine, so two players may hear different
  ones. The game's setting that mutes other players' monster noises treats it exactly as it treats the game's own
  (read in the code).
- The Race Editor has an Emote Sounds panel.

## Stopping a player dead: `sam_stop_momentum(player [, ticks])`

The modder's leap moved the player with `sam_move_entity`, and the walking momentum left over from before the leap
pushed the player off its path and into a wall. `sam_set_move_speed` only scales new steps: momentum already built up
slides on until the game's drag wears it off.

- `sam_stop_momentum(player)` clears the player's walking momentum at once, forward and sideways alike, and with it
  the push a heavy crossbow's recoil gives and any knockback (seen for walking forward and a shove; the rest read in
  the code).
- With `ticks` (50 to a second) the player is held still for that many game ticks after the call: held movement keys
  move them nowhere, and neither does `sam_apply_force` (both seen), a knockback from a hit or the game's own dash
  (read in the code). `sam_move_entity` still moves them (seen), and so does `sam_set_position` (read in the code), so
  a leap during a hold uses `sam_move_entity`.
- A new call replaces the hold, and a call without `ticks` ends one early. A pause uses none of the ticks, and the hold
  ends early if the player dies or takes the stairs.
- In multiplayer it runs on the player's own machine, where their movement is worked out: the host's call about a
  joiner is carried there. A player whose game does not run S.A.M is refused with a warning.

## The installer's first screen scrolls

A player reported that the "Please Read First" box could not be scrolled to. The installer's window grows to fit its
first screen but stops at the display's height, so on a short display, or one with Windows display scaling, the
bottom panels were cut off with nothing to scroll them. That screen now scrolls, with a scrollbar and the mouse wheel
when it does not fit, and the Install and Close buttons stay on screen. The installer is rebuilt: it is the
`SAM_Framework_Installer.exe` on this release.

## Numbers

344 script functions (one new: `sam_stop_momentum`), 87 events. One new race field: `emote_sounds`. In the engine: the
empty-Use noise in `collision.cpp`, the two packets that carry it in `net.cpp`, and one line at the end of the
player's movement in `actplayer.cpp`. No new network message: a race's noise rides the game's own two emote packets,
marked by a sound number past the game's own sounds, and `sam_stop_momentum` is carried to a joiner the way every
S.A.M call about a player's own machine already is.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:**

- `tests/EmoteTest`, as three races. A race on the human body with purr and hiss: a Use on nothing played one of them,
  a second Use at once played nothing, and one after five seconds played again. A race on the skeleton body: its sound
  did not repeat a second and a half later. A race on the goblin body whose only sound no mod registers: none of the
  mod's sounds played, and the engine's answer for its noise was "silent" (logged; the goblin's own noise cannot be
  heard by a test). The engine's answer was also checked with the race's abilities off, polymorphed, shapeshifted and
  in another body: the body's noise each time. On 3.12.2 none of the race's sounds plays.
- `tests/MomentumTest`, with a JavaScript twin, 26 checks. Without the call a player slides about a fifth of a tile
  after letting go of the key; with it they stop at once. A hold of 75 ticks keeps a held key from moving the player
  until 77 ticks after the call, and the JavaScript hold of 60 until 62. `sam_move_entity` moves a held player exactly
  as asked. A shove that moves a player 3.4 tiles moves a held one not at all, and a shove made just before a stop is
  wiped. A call without ticks ends a hold, and a negative count holds nobody. On 3.12.2 the function does not exist.
- `installer/layout_test.py` builds the real installer window with the display height faked. At 600 pixels the
  scrollbar shows, Install and Close are on screen, the end of "Please Read First" starts out of view and is reached
  by scrolling, the mouse wheel over a panel's text scrolls, and another screen drops the wheel. At 1400 pixels
  nothing scrolls and everything shows. 14 checks; the installer before this release fails 6 of them.

The regression script (`handoff/run_regress.sh`) was run on the release build, with no reruns: Hello 11, Speed 41,
Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27,
`QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with its 28 log checks), `MoveTest` 28, `SwayTest` 13,
`RootSwayTest` 21, `CompendiumTest` 2 (with its 27 log checks), `TauntTest` 4 (with its 6 log checks), `EmoteTest` 3,
3 and 1 for its three races (with 2, 2 and 3 log checks), `MomentumTest` 26 and the installer layout check 14, with no
failures. Three of these tests have checks that need a person at the keyboard and nobody did them: Vision's sneaking
checks, the tin in both Neutral runs, and HelmTwin's checks where the player wears the items. `ChargeTest`, which needs
a person to swing at a goblin, is not in the script and was not run.

**Not tested:**

- **Multiplayer**, for both: nothing was tried with two machines.
- **During a hold: walking sideways, the heavy crossbow's recoil, a knockback from a hit, the game's own dash,
  `sam_set_position`, a death, the stairs, a pause, and a new call with ticks replacing the hold**: read in the code.
- **The installer with a real mouse on a real short display**: the test fakes the display and sends the wheel itself.
  The installer error the same player reported was not seen: they did not say what it was.
- **The noise by ear**: the test finds the race's sound playing, but nobody listened to it.
