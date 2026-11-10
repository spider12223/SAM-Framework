# S.A.M Framework v3.10.0

**A tail can swing from the end that joins the body.** After 3.9.1 added `"sway": "side"`, a modder tried it on
a catfolk tail and it "swivels forward and back": an extra limb turned about its own middle, and a tail that
hangs down, turned about an upright line, only twists in place. This release adds one field to an extra limb,
`root`, and makes `"side"` swing the far end left and right for a limb that hangs, stands up or sticks out
behind. There is no new
script function: 343 functions, 87 events, as in 3.9.1.

## A limb that turns about its root

```json
"extra_limbs": [
  { "model": "models/catfolk/tail.vox", "attach": "torso", "offset": { "forward": -1 }, "root": "top", "sway": "side" }
]
```

- `"root"` names the end of the model that joins the body: `"top"`, `"bottom"`, `"front"`, `"back"`, `"left"` or
  `"right"`. S.A.M takes the box around the model's filled voxels (so empty space left around the model in the
  .vox does not count), finds the middle of that side of it, and puts that point where the limb is placed: its
  attach point, moved by its `offset`. The limb then turns about that end, for its `pitch`, `roll` and
  `yaw_offset` and for its sway.
- Top and bottom are as the model shows in game. Front, back, left and right are the model's own, so if
  `yaw_offset` turns the model they turn with it.
- `focal` still works, and with a root it is added on top, as a nudge.
- A misspelled root is reported in `sam_log.txt` and not applied: the limb turns about its middle. S.A.M older
  than 3.10.0 ignores `root` the same way.

## "side" swings the far end left and right

`"sway": "side"` now swings about the line at right angles to both the player's left-right direction and the
line from the point the limb turns about to the middle of its filled voxels.

- A limb that hangs down or stands up swings like a pendulum: the tip moves left and right.
- A limb that sticks out behind swings left and right, as before. With no `pitch` it stays level; a pitched
  limb swings in a plane tilted by that pitch.
- A limb that points straight out to the player's side has no left and right to swing in: it turns about the
  upright line.
- A limb with neither `root` nor `focal` turns about its own middle exactly as it did in 3.9.1.
- This also changes a limb given a `focal` by hand: in 3.9.1 one hanging below the point it turns about
  swivelled; now it swings like a pendulum.

## Placing a tail

`"attach": "torso"` puts the point a limb turns about at the chest limb's own position and follows the chest as
it moves; `offset` moves it from there. In an extra limb's `offset`, `"up"` moves the limb **down** (a positive
number lowers it); it works that way in 3.9.1 too, and this release does not change it. This was read in the
code, not measured.

## Fixed documentation

- The schema described `focal` as "the point the limb rotates around". It is the other way round: focal moves the
  model away from the point it turns about, in the limb's own directions (x forward, y right, z down, in world
  units, where a voxel is half a unit). The schema now says so.
- The scripting reference's sideways-tail example placed the tail with `offset` alone, which is exactly the setup
  that turns about the tail's middle. It now uses `root`.

## Numbers

343 script functions, 87 events (unchanged). One new JSON field: an extra limb's `root`. Engine file touched:
`actplayer.cpp` (where an extra limb is placed and how it swings); the rest is in the framework
(`sam_races.cpp`). Nothing new crosses the network: every machine works a limb's root and swing out from the
race for itself.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:** a new test, `RootSwayTest`, hangs six sticks on a race:
a quarterstaff hanging from its top, one standing on its bottom, a crossbow bolt sticking out behind from its
front, a root with a focal nudge, a misspelled root, and a bolt with `"side"` and no root. The focal each one
should get was worked out from the model files, not from S.A.M, and where each stick is drawn is rebuilt from
`sam_get_limb` in the order the renderer draws it, in the player's own frame. Over 200 still ticks the three
roots stay put (the check allows 0.02 of a unit; each logged 0.000), each far end swings from side to side by more than half a unit without moving forward
or back (the bolt stays level), the hanging staff stays below its root and the standing one above, the misspelled
root is not applied (its focal stays 0, 0, 0; the warning it should get was in `sam_log.txt`, which the test
itself does not read), and the bolt with no root only turns, as in 3.9.1. 19 passed; on the
3.9.1 build 7 of those 19 fail. The first build failed one check, the bolt dipping as it swung, because the
swing was worked out from the model's origin rather than the middle of what is drawn; that was fixed before this
release. The rest of the suite was run on the release build: Hello 11, Speed 41, Settings 88, Loot 90,
Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27 (on a rerun: the
first run lost 4 checks that wait for a goblin to hit the player, which it did not do in time),
`QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with its 28 log checks), `MoveTest` 28 and
`SwayTest` 13, with no failures. Three of these tests have checks that need a person at the keyboard and nobody
did them: Vision's sneaking checks, the tin in both Neutral runs, and HelmTwin's checks where the player wears
the items.

**Seen in the game:** two salamander tails hanging from a player's back, one with `"root": "front"` and one
without, watched from behind: the one with a root swings from it. Where they sat (one to each side, tilted
down) was the check's own offsets, and it was not quite the right spot for a tail.

**Not tested:**

- **A modder's own tail with each root.** Only the salamander tail and two straight item models were used.
- **A root on a limb attached to the head**, and roots together with `yaw_offset`. Read in the code.
- **What `"up"` does.** Read in the code, as above.
- **Multiplayer.** Nothing was tried with two machines.
