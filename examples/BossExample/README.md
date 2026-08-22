# Boss Example

A complete custom-model boss. Copy the folder, change `namespace` in `mod.json`, swap the
`.vox`, and you have your own.

```
BossExample/
  mod.json                     the manifest, and where you declare your .vox
  models/
    wyvern.vox                 your model (see below)
  monsters/
    wyvern.json                the creature: body, boss traits, stats
  spells/
    searing_breath.json        the breath attack
  items/
    wyvern_scale.json          crafting drop
    wyvern_blade.json          the reward weapon
  main.lua                     phases, telegraphed attack, adds, death drop
```

## About the XP

There is no way to make a boss award *extra* XP. `xp_award_percent` is a percentage of the
normal award and the engine clamps it to 100, so it can only ever reduce. An earlier version
of this example set it to 400, which failed the schema and did nothing at all.

## About the model

One `.vox` file, in Barony's **slab** format.

This is the single most common way a custom model fails, so read this bit twice. MagicaVoxel
is fine for *building* the model, but its own `.vox` export is a completely different format
that happens to share the extension. Barony cannot read it and the load is refused with a log
line saying so. Export or convert to slab `.vox`.

**Build it big.** Do not try to scale it up in the game: model scale is capped just under 2x
and in multiplayer a scale of exactly 2.0 wraps to zero and the creature vanishes. Size it in
your voxel editor instead.

**Use `rat` as the `base_type` for anything large.** This matters more than it sounds. Most
Barony creatures are assembled from separate limb models: a skeleton is ten pieces, a human
ten, a troll five. Your body model replaces the main one, and the base creature's limbs still
render around it, which looks broken. The rat is a single model with no limbs, so a
rat-based boss is 100% your model.

## About animation

**Barony has no animation system in the sense you are probably imagining.** There are no
frames, no rigs, no skeletal animation, for vanilla creatures or modded ones. A `.vox` is a
static mesh.

What vanilla creatures do instead is move separate limb models around in code. That is why a
skeleton is ten objects. Your single model does not deform.

What your boss *does* get:

- it walks, turns to face you, and is pushed around by knockback
- the base creature's motion (bobbing, tilting) applies to your whole model
- everything in `main.lua`: screen shake, flashes, roars, telegraphs, phase changes

In practice this reads fine, because what players register as "boss animation" is mostly the
telegraph and the impact, not mesh deformation. A wind-up roar, a shake, then the breath, is
worth more than any amount of rigging you cannot do anyway.

## What you cannot do, and what to do instead

**Flight.** Positions are on a tile grid and entity updates only go out about six times a
second, so a scripted swoop stutters for everyone else in multiplayer. Instead: hover.
`main.lua` puts the boss into levitation at phase 2, and a wyvern that hovers, roars and
breathes fire reads as flying.

**Its own AI.** How a creature chases, paths and swings is one enormous function in the
engine keyed to creature type, with no way to substitute your own. Your boss will chase and
bite like a rat. So: pick the base whose movement suits you, and put the character in the
set-pieces. Phases, telegraphs, adds and an enrage are what players describe afterwards.

## Multiplayer

The host sees your model. Other players see the base creature (a rat). Nothing breaks and
nobody desyncs, but model parity across the network is not in yet. The fight logic itself,
including phases and attacks, is host-authoritative and correct for everyone.

## Testing it

```
/sam_spawn bossexample:wyvern
```

Then check `sam_log.txt`. At load you should see the body model resolve:

```
[MONSTERS] Monster 'Ashen Wyvern' body model 'bossexample:wyvern' -> model index N
```

If instead you see a warning that the model is not registered, the id in `monsters/wyvern.json`
does not match the id in `mod.json`'s `models` list. That is the single most common mistake,
and it is why the framework logs both.
