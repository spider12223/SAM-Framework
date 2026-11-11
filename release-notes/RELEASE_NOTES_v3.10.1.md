# S.A.M Framework v3.10.1

**A tail's root now lands on the part that meets the body.** In 3.10.0, `"root": "top"` put the middle of the
model's whole top side on the point the limb turns about. A modder's tail joins the torso at its top right
corner, so it still swung from the middle of its width; widening the .vox canvas to put that corner in the
middle changed nothing, because root ignores empty space. There is no new field and no new script function:
343 functions, 87 events, as in 3.10.0.

## What changed

- A root is now the average position of the voxels on that side's **outermost layer**: for `"top"`, the voxels that reach
  highest; for `"front"`, the ones that reach furthest forward; and so on. A tail whose top layer is only the bit
  that meets the body now turns about that bit.
- For a model whose outermost layer sits at the middle of the whole model's width and depth, such as a straight
  stick, nothing moves. Any other model moves to put that layer on the turning point, and a limb placed by eye
  under 3.10.0 may need its `offset` adjusted.
- If the outermost layer has filled voxels in two separate places, for example both ends of a U-shaped top, the
  root lands at the average position of all of them: between the two, nearer the bigger group. Use `focal` to
  nudge it.
- With `"root": "top"` the base game's salamander tail now gets a focal x of -1.25, the same forward value the
  game's own limb table uses for that tail (its z is 1.75; the root gives 2).

```json
"extra_limbs": [
  { "model": "models/catfolk/tail.vox", "attach": "torso", "root": "top", "sway": "side" }
]
```

## Numbers

343 script functions, 87 events (unchanged). No new JSON field. The change is in the framework
(`sam_races.cpp`); no engine file changed. Nothing crosses the network: every machine works the root out from
the model for itself.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:** `RootSwayTest` gains two salamander tails, one with
`"root": "top"` and one with `"root": "front"`. The focal each should get was worked out from the model file by a
separate script, not from S.A.M: the tail's top layer is a single voxel forward of the middle (focal -1.25, 0,
2), and its front layer is two voxels, one voxel above the middle of the tail (focal -2, 0, 1). The test now passes 21 of 21; on the
3.10.0 build those two checks fail (it gives 0, 0, 2 and -2, 0, 0.5) and the other 19 pass. The rest of the suite was run on the
release build: Hello 11, Speed 41, Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55,
SpellEffect 15, Follower 60, Addons 27, `QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with its 28 log
checks), `MoveTest` 28 and `SwayTest` 13, with no failures. Three of these tests have checks that need a person at
the keyboard and nobody did them: Vision's sneaking checks, the tin in both Neutral runs, and HelmTwin's checks
where the player wears the items.

**Not tested:**

- **The modder's own tail.** Only the salamander tail and two straight item models were used.
- **Watching it in the game.** The root was measured from the focal the engine uses, not looked at.
- **Multiplayer.** Nothing was tried with two machines.
