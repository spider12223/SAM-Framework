# Worlds and Assets

Three things S.A.M mods can do that almost nobody knows about, because they were never
written down. Two of them need no framework support at all: they work because a mod folder
is mounted ahead of the game's own files, so anything you ship wins.

1. [Ship your own dungeon](#ship-your-own-dungeon)
2. [Replace any texture or font](#replace-any-texture-or-font)
3. [Make your own lava and water](#make-your-own-lava-and-water)

At the end: [driving monsters from a script](#driving-monsters-from-a-script), which is new
in 1.10.2.

---

## Ship your own dungeon

Barony's level generator is simpler than it looks. To build a floor it does two things:

1. loads `maps/<set>.lmp` as an open, fully floored arena
2. stamps the prefab rooms `maps/<set>00.lmp`, `<set>01.lmp`, `<set>02.lmp` ... into that
   arena at random until it runs out of space

That is the whole algorithm. Which means **a mod can ship an entire procedurally generated
dungeon branch as data**, with no scripting and no engine support.

### The smallest possible branch

Three files in your mod folder:

```
maps/verdant.lmp      the arena: an open floored rectangle, nothing else
maps/verdant00.lmp    always placed first, so this is your entrance room
maps/verdant01.lmp    one more room
```

Two rooms is the minimum. Below that the generator refuses to run.

Build all three in Barony's own level editor. The arena is just a large empty floor: the
generator writes rooms on top of it, so anything you leave in there stays as background.

### Rooms repeat

A room is not consumed when it is used. The generator picks from your list over and over
until the arena is full, so adding rooms increases **variety**, not density. Ten rooms does
not mean ten rooms per floor.

Room sizes are free-form. They only have to be multiples of 7 if you set the adjacent-rooms
flag on the map.

### Reaching your dungeon

Place a **custom portal** in any map with the level editor and set its destination to your
levelset name (`verdant`). When a player steps through, S.A.M sees that `verdant00.lmp`
exists, recognises it as a generated set rather than a single map, and generates a floor.

This is the part that needed engine support, and it is the only part. Before 1.10.2 a portal
could only reach one fixed map, so a generated branch loaded its bare arena and looked like
an empty room.

You do **not** need to edit `levels.txt`. That matters, because `levels.txt` is one file:
if two mods both rewrote it, the last one to load would silently win.

Multi-floor branches chain naturally. Put a portal to the next levelset inside one of the
rooms.

### What you get for free

- **Secret exits.** Add `secret%: N` and the engine looks for `<set>secret.lmp`.
- **Treasure vaults.** Files named `<set>_lockNN.lmp` are picked up automatically.
- **Which monsters spawn.** Add an entry to your `monstercurve.json`, keyed on the map name.
- **Shops, darkness, minotaurs.** All data, via `gameplaymodifiers.json`.
- **The ceiling and skybox.** Both are set in the map file itself.

### Traps to know about

**Name your levelset something neutral.** Several parts of the engine match on the *start*
of a map name to decide monsters and music. A branch called `Hell Something` will inherit
hell behaviour you did not ask for. `verdant` is fine; `Sanctum` is not.

**Keep the name short.** Under 100 characters. It travels to multiplayer clients in a fixed
buffer.

**Steam achievements switch off** on any floor built from a mod-supplied map. That is
Barony's normal modded-content behaviour, not a bug.

---

## Replace any texture or font

Your mod folder is mounted **ahead of** the game's own files. Anything you ship at the same
path as a game file is used instead of it, with no declaration and no framework support.

```
images/ui/HUD/hotbar_slot.png     replaces that exact HUD image
lang/en.ttf                        replaces the game font
```

There are about 4,700 images under `images/ui` alone, and every one of them can be replaced
this way. There is no checksum anywhere in the pipeline to fight.

Two caveats:

- **Linux and Steam Deck are case-sensitive.** `Images/UI/Foo.png` and `images/ui/foo.png`
  are the same file on Windows and different files on a Deck. Match the game's casing
  exactly or your pack works for you and nobody else.
- Replacing art affects **every** mod and the base game, so keep a resource pack in its own
  mod rather than bundling it with content.

---

## Make your own lava and water

This one is unusual: **the behaviour comes from the filename**, not from any table.

When the game loads its tile list it looks at each tile's image path and decides:

| The filename contains | The tile becomes |
|---|---|
| `lava` | real lava: it burns, and monsters refuse to path across it |
| `water` or `swimtile` | real water: you swim, and it slows you |
| ends in a digit | an animation frame |

So a tile image you ship called `mytiles/green_lava_pool.png` is not lava-*coloured*. It is
lava, in about ninety-five places in the engine, including pathfinding. Monsters will walk
around it without a line of AI code.

That makes custom biomes far cheaper than they sound. An acid marsh is a water tile with
different art plus a script that damages anyone standing in it.

---

## Driving monsters from a script

New in 1.10.2, and worth understanding *why* it is shaped this way.

Barony has **no per-species AI**. There is one large shared state machine that every
creature runs, with occasional exceptions for particular types. A troll and a rat run the
same code. This is why a custom monster built on a bat moves like a bat: not because it
inherited a bat brain, but because there is only one brain and nobody could steer it.

These four functions are that steering. They are thin wrappers over machinery the engine
already had, so they behave exactly like the game's own movement.

```lua
sam_monster_path_to(uid, tileX, tileY)  -- real pathfinding, returns false if unreachable
sam_monster_face(uid, tileX, tileY)     -- turn to look at a tile
sam_monster_attack(uid)                 -- swing now, with the right pose for its weapon
sam_monster_charge(uid, ticks)          -- straight-line charge
```

Coordinates are **tile** coordinates, matching `sam_get_position`.

`sam_monster_charge` drives a charge behaviour that has been sitting in the engine fully
written and completely unreachable. It aims at the monster's target if it can see it, and
otherwise charges along its current facing, so face it first to aim. It stops on its own
when the time runs out or the instant it hits anything, so you cannot wedge a monster in a
wall with it.

A boss that lines up and charges, in nine lines:

```lua
local ME = 0

function on_tick(e)
  for _, uid in ipairs(sam_get_nearby_entities(sam_get_player_uid(ME), 20)) do
    if sam_monster_has_trait(uid, "boss") and math.random(120) == 1 then
      local px, py = sam_get_position(sam_get_player_uid(ME))
      sam_monster_face(uid, px, py)
      sam_monster_charge(uid, 60)
    end
  end
end
```

All four are host-only. In multiplayer they run on the host and the result replicates
normally. Use `sam_is_host()` to branch instead of letting a client spam the log:

```lua
if not sam_is_host() then return end
```

`sam_player_count()` and `sam_local_player()` are the other two halves of that. Note that
`sam_local_player()` is **not** always 0 on a client, which is the assumption most
mods tested only in singleplayer quietly bake in.
