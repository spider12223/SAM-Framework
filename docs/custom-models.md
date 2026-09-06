# Custom models

Everything about putting your own `.vox` into Barony: the format, the three places a model
can go, and the four ways people get it wrong.

If a model is not showing up, run **`/sam_models`** in game. It lists every model that
registered, the id it registered under, its engine index, and the file it came from, and it
marks anything that came out of the base game rather than your mod.

---

## The format: slab, and MagicaVoxel

Barony reads a format usually called **slab** `.vox`. MagicaVoxel's own `.vox` export is a
completely different format that happens to use the same extension, and the engine's own
loader cannot read it.

**Since 2.5 you can ship either.** A MagicaVoxel file is converted in memory when the mod
loads. Nothing is written to disk, your file is left exactly as you saved it, and every
machine converts the same bytes to the same voxels, so it changes nothing about multiplayer.
The log says which happened:

```
[MODELS ] Converted MagicaVoxel model 'models/mymod/wyvern.vox' (32x28x40) for [mymod:wyvern].
```

Two things to know about MagicaVoxel files:

- **One model per file.** MagicaVoxel can hold several; Barony has one model per file, so only
  the first is used and the log says so.
- **Save with a palette.** If you never recolour anything, MagicaVoxel writes no palette chunk
  because the model is on its untouched default palette. There is no way to guess what you saw,
  so the load is refused with an explanation. Recolour any single voxel and save again.

Orientation is handled for you. MagicaVoxel is Z-up; a Barony slab has Z pointing **down**
(array index 0 is the top of the model, which is why a vanilla chair has its legs at the high
end). The converter rotates the model so it arrives the right way up. It rotates rather than
mirrors, so a left boot stays a left boot; the model may end up facing the other way in the
horizontal plane, which `yaw_offset` exists to correct.

A slab file is exactly `12 + width*height*depth + 768` bytes: three little-endian `int32`
dimensions, one palette-index byte per voxel (255 means empty), then a 768-byte RGB palette
whose channels are 6-bit. The framework checks that before it hands a slab to the engine,
because the engine's loader trusts those dimensions and will crash on a file that is neither.

---

## Where a model can go

### On an item

```json
"model": "models/mymod/blade.vox",
"model_fp": "models/mymod/blade_fp.vox"
```

A **mod-relative path**. The path is also the model's id, which is why you should put your
files under `models/<yourmod>/` — two mods that both ship `models/sword.vox` would collide.

`model_fp` is the first-person model you see in your own hand.

### On a class, as a whole body

```json
"appearance": { "body_model": "models/mymod/jet.vox" }
```

This replaces the **entire body** on any race, with any armour equipped, and hides every
limb. A vehicle, a construct, a floating shape.

`body_model` accepts three forms:

| What you write | What it means |
|---|---|
| `"models/mymod/jet.vox"` | a path to a `.vox` your mod ships |
| `"mymod:jet"` | an id you declared in `mod.json` under `"models"` |
| `"1025"` | a raw vanilla model index |

A bare name like `"gharbad"` is none of those and will not resolve.

### On a monster, as a body

```json
"body": { "model": "mymod:rathalos_body" }
```

Here the id **must** be one you declared in `mod.json` under `"models"`. This is different
from items, which register under their path. Declare it:

```json
"models": [
  { "id": "mymod:rathalos_body", "file": "models/mymod/rathalos.vox" }
]
```

---

## The five ways this goes wrong

### 1. Pointing at a vanilla creature file

This is the big one, and it is silent unless you look.

Model paths are resolved through the game's virtual filesystem, and **the base game is in
there too**. So writing a real vanilla path works — it loads the actual file and registers a
second copy of a stock model as though you shipped it.

The trap is that almost every vanilla creature is **not one model**. It is a pile of separate
limbs. Gharbad, for instance:

```
gharbad_torso.vox   gharbad_head.vox
gharbad_armleft.vox gharbad_armleftbent.vox
gharbad_armright.vox gharbad_armrightbent.vox
gharbad_legleft.vox gharbad_legright.vox
```

A skeleton is ten pieces, a human ten, a troll five. There is no "whole gharbad" file to
point at. Set `body_model` to one of those and you get a floating torso, because `body_model`
means *this one model is the entire body, hide the limbs*.

The framework now warns when a model path resolves to a base-game file:

```
[MODELS ] Model 'models/creatures/goatman/goatman_named/gharbad_head.vox' for [mymod:goat]
          is a BASE GAME file, not one your mod ships. It will load, but you are registering
          a second copy of a stock model. Note that vanilla creature models are single LIMBS
          (a head, an arm), not whole bodies -- using one as a body model draws a floating
          limb. If you meant to ship your own .vox, put it under your mod folder.
```

**If you want to look like an existing creature, use a custom race instead**, which keeps all
the limbs. See `race.schema.json` and its `host_body` field. Read point 5 below first if anyone
is going to see you online.

A race can also bring its OWN models for each limb, with `limb_models`:

```json
"host_body": "goatman",
"limb_models": {
  "head": "gharbad_head.vox",      "torso": "gharbad_torso.vox",
  "arm_right": "gharbad_armright.vox", "arm_left": "gharbad_armleft.vox",
  "leg_right": "gharbad_legright.vox", "leg_left": "gharbad_legleft.vox"
}
```

Name the model, not its number. A bare filename resolves when it is unique, and most are; when
it is not you are told, and you add the folder (`models/creatures/goatman/goatman_named/...`).
Raw indices still work for every mod already written with them, but `1025` says nothing about
what it is, and the line-N-is-index-N-1 subtraction below is one of the easiest things here to
get wrong by one.

`host_body` still decides the skeleton -- the animation, the limb positions, which slots
exist -- and `limb_models` decides what is drawn in each slot. Omit a limb and it keeps the
host body's model, so this covers "just the head" and "a whole new body" with one field.

Two things follow from that, and neither is a bug:

- **Everything except the head is only visible when that armour slot is empty.** A custom
  torso shows bare-chested and a breastplate covers it. That is how every race in the game
  already works, and it is why a whole-body `body_model` is the wrong tool here.
- **Pick limbs that fit the host body.** They are drawn at the host's focal points. Models
  from the same creature family line up; a troll arm on a gnome frame will not.

The **first-person** arm keeps the host body's model. The game ships a first-person arm per
playable race, not per creature, so for a race built from vanilla creature models there is
nothing to swap it for.

### 2. The model index is not the line number

`models.txt` line **N** is model index **N-1**. Grep the file, see `1026:...gharbad_head.vox`,
write `1026`, and you get the model on line 1027.

An index past the end of the table used to be accepted silently and rendered the player as
nothing at all. It is now refused with the real count in the message, for both `body_model`
and a per-race `head`.

Index **0** is also refused: that slot is `models/system/null.vox`, the engine's empty model,
so it loads perfectly and draws nothing. Real content starts at index 1.

### 3. Editing a `.vox` while the game is running

Models are loaded once and kept for the life of the process. `/sam_reload` re-reads your JSON
but **not** a `.vox` you have changed on disk. Restart the game to see an edited model. A
*new* model is picked up fine.

### 4. Scaling up in game

Model scale is capped just under 2x, and in multiplayer a scale of exactly 2.0 wraps to zero
and the thing vanishes. Build it at the size you want it.

### 5. Assuming a custom race works for players who do not have your mod

It does not, and this one is worth reading twice, because the obvious workaround does not
work either.

**Everyone who is meant to see your race needs the S.A.M exe AND your mod.** Without both, they
see the plain host body instead. That is the whole rule, and the rest of this section is why.

A game decides whether something is a player by looking at its head model. It accepts the
vanilla player heads, plus the head of any race **that machine has registered**. Your race is
only registered on machines that loaded your mod. To everybody else your head is just some
model, so your character is not recognised as a player at all: it is never given a body, never
moved, and stands frozen and invisible where it spawned. Other players can still walk into it
and hit it.

**Pointing your race at a vanilla model does not save you.** This is the trap, because it feels
like it should. If your `limb_models` name a stock creature's limbs, every machine already has
those files, so it is tempting to assume multiplayer is fine. It is not. The other player was
never missing the artwork. They are missing the RACE, and that is what the recognition depends
on. A stock Gharbad head is still not a player head to someone who has not loaded a mod that
says so.

Since 2.6.1 this degrades properly instead of breaking: the host sends the host body's ordinary
player head, so a player without your mod sees a normal, moving creature of whatever
`host_body` you chose. They still will not see your race's own look, because they do not have
it. What they will no longer see is an invisible statue.

If you are testing this, test it the way it will actually be played: **one machine with the mod
hosting, one machine without it joining.** Two modded machines will never show you the problem.

---

## Checking your work

```
/sam_models
```

```
S.A.M: 4 custom model(s), engine table is 2415 models:
  models/mymod/blade.vox                    -> index 2413   models/mymod/blade.vox
  mymod:rathalos_body                       -> index 2414   models/mymod/rathalos.vox
  sam_builtin/workbench/Hunters_Toolkit.vox -> index 2411   sam_builtin/workbench/Hunters_Toolkit.vox
  sam_builtin/workbench/Hunters_Toolkit_FP.vox -> index 2412 …
```

The two `sam_builtin/` entries are the framework's own Hunter's Workbench and are always
there. Your own models are the ones with your namespace or your paths.

If a model you expected is missing from that list, it never registered, and `sam_log.txt` has
a `[MODELS]` line saying why.
