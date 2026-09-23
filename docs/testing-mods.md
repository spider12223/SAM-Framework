# Testing a mod without playing it

A test mod writes what it found to `sam_log.txt`. Getting there used to mean launching Barony,
opening the Mods menu, loading the mod, starting a game, waiting, reading the log and quitting.
That is fine once. It is not fine as the thing standing between a change and knowing whether the
change broke anything, which is why in practice the tests got run rarely and late.

`-samtest` does all of it by itself.

```bash
tools/run_mod_test.sh HelloTest
```

```
running HelloTest (timeout 180s)...

  [18:02:26 +0:00:15] INFO  [SCRIPT  ] HELLO RESULT: 11 passed, 0 FAILED
  framework log: 0 error(s), 1 warning(s)

PASS  (16s)
```

`HelloTest` is in `examples/`: copy it into `mods/` first. It is the smallest mod that can be run
this way and the one to copy when you write your own.

The exit code is the answer, so this works in a script:

| code | meaning |
|---|---|
| 0 | every check passed |
| 1 | the mod reported a failed check |
| 2 | no mod finished before the watchdog stopped the run |
| 3 | the mods could not be loaded: a folder that would not mount, or a script that failed to parse or errored while running its top level (the `Failed to parse` / `disabled` lines in `sam_log.txt` say which) |
| other | the game itself failed to start, or crashed on the way out |

It is a real run of the real engine on a real dungeon: a small window opens and closes itself.
Nothing is simulated, which is the point. A test that passes here passed in Barony.

## The flags

`run_mod_test.sh` is a wrapper; the game takes these directly.

```
barony.exe -samtest=HelloTest -windowed -size=640x480
```

| flag | |
|---|---|
| `-samtest=<mod>[,<mod>...]` | mod FOLDER names under `mods/`, in load order. This flag alone is enough. |
| `-samtestclass=<class>` | the class to start as. Default `barbarian`. |
| `-samtestseed=<n>` | the dungeon seed. Fixed by default, so the same command twice walks the same dungeon and a failure can be looked at again. |
| `-samtestfloor=<n>` | take the stairs down to floor `n` before leaving the mod to it. |
| `-samtesttimeout=<s>` | the watchdog, default 180. `0` disables it, which you should not do unattended. |

**Without `-samtest` nothing here runs.** Every entry point is one bool, the game boots to its
menu exactly as before, and the exit code is whatever the engine was going to return.

## What a mod has to do

One line, at the end of its checks:

```lua
sam_test_done(pass, fail)
```

That writes the verdict to the log, ends the run, and sets the exit code from `fail`. Outside a
`-samtest` run it does nothing at all and returns false, so it is safe to leave in a mod that
people play.

A run that never reaches it is stopped by the watchdog and reports **2**, not 0. A test that hangs
has failed; it has not passed. That distinction is the whole reason the watchdog reports its own
code instead of quietly returning success.

## Which mods can be run this way

**A mod that drives itself can.** `examples/HelloTest` is the example: it starts on
`game.on_game_start`, waits a second of game time in `on_tick` and runs its checks, so it needs
nobody.

**A mod whose checks run on arriving at a floor needs `--floor 1`.** `game.on_level_entered` is
fired from the engine's level-change path, and the floor a new game opens on does not go through
it, so a mod waiting on that event sits there forever. This is not a quirk of test mode: it is true
in an ordinary game too, and worth knowing when you write a mod.

**A mod that needs the player to move cannot be run this way.** `RulesTest` measures walking speed
by watching velocity while you walk, and there is no keyboard. Split that kind of mod: put the
checks that need no input in one mod and run it here, and keep the ones that do for a human. A
check nobody ever runs is not a check.

## Writing a mod that can be tested

- Do the work on an event, not on a key press, wherever the thing being tested allows it.
- Assert a **relationship**, not a type. `sam_get_hp(uid) ~= nil` passes on a function that returns
  a constant; `hp_after == hp_before - 10` does not. This framework has shipped test mods that
  passed on broken code, more than once.
- Count passes and failures and hand both to `sam_test_done`. Log the NAME of every failure, because
  the exit code tells you that something broke and the log is the only thing that tells you what.
- Make the test fail on purpose once, before you trust it. If you cannot make it go red, it is not
  testing anything.

## Multiplayer

Not yet. A co-op test needs two processes, a host and a joiner, and the lobby has a readiness
handshake and a countdown between them. `docs/multiplayer.md` has the manual two-copy procedure
in the meantime.
