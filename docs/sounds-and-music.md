# Sounds and music

Everything here works without writing a script. Put audio files in the right folder of your mod
and they are loaded when the game starts. Barony plays `.ogg`, `.wav`, `.mp3` and `.flac`.

| You want to... | Do this |
|---|---|
| add a sound effect | `sounds/zap.ogg` -- it is now `yourmod:zap` |
| replace one of the game's sounds | `sounds/replace/RatDie.ogg` |
| replace a whole family of them | `sounds/replace/SwingWeapon.ogg` (all five swings) |
| replace a music track | `music/replace/mines.ogg` |
| add a track for a script or a boss | `music/boss.ogg` -- it is now `yourmod:boss` |
| give a floor its own music | an entry in mod.json `"music"` with `"floors"` |
| give a monster its own sounds | a `"sounds"` map in the monster's JSON |
| give a boss a theme | `"music": "boss"` in the monster's JSON |
| give a weapon its own swing | a `"sounds"` map in the item's JSON |

`yourmod` is the `namespace` in your mod.json. After loading, `sam_log.txt` lists every sound and
track that loaded, every replacement it made, and exactly what went wrong with any that did not.

## Sound effects

### Add one

Drop the file in `sounds/`. `sounds/zap.ogg` becomes `yourmod:zap`. The name is the file name in
lower case, with anything other than letters, digits and `_` turned into `_`.

Play it from a script:

```lua
sam_play_sound("zap")                       -- every player hears it, flat
sam_play_sound_at("zap", 10, 12)            -- at a tile, quieter with distance
sam_play_sound_entity("zap", uid)           -- at a creature
```

From your own scripts the bare name works; another mod's sound needs its full id.

### Replace one of the game's

Drop a file named after the vanilla sound in `sounds/replace/`:

```
sounds/replace/RatDie.ogg          every rat dies with your sound
sounds/replace/SwingWeapon.ogg     every weapon swing, all five variants
sounds/replace/LeatherSteps.ogg    every leather footstep
```

The names are the game's own sound files without the extension. A name without its number is
the whole group: `SwingWeapon1V1` is one swing, `SwingWeapon` is all of them. The full list is in
[vanilla-sounds.md](vanilla-sounds.md), with the ones people ask for first at the top.

Every sound also has a number (the list shows it), and the number works as a name:
`sounds/replace/170.ogg`. You only need it for seven names that are both a group and one sound
in it: `Casting`, `LevelUp`, `bell_crash`, `slam`, `bolas_throw`, `thunder` and `foci_ice`.
`sounds/replace/Casting.ogg` changes all four casting sounds; to change only the one called
`Casting`, use its number.

**Find a name in game:** open the console and type `/sam_sounds vanilla swing`. Then
`/sam_playsound SwingWeapon` plays it -- through your replacement, so you can hear it worked.

A replacement only changes what players who have your mod hear. Someone without it still hears
the original, so a replacement can never make anybody hear the wrong sound.

### More control: mod.json

The folders cover most things. For several files picked at random, a volume, or a loop, list the
sound in mod.json:

```json
"sounds": [
  { "id": "step", "file": ["sounds/step1.ogg", "sounds/step2.ogg", "sounds/step3.ogg"], "volume": 0.7 },
  { "id": "rain", "file": "sounds/rain.ogg", "loop": true },
  { "replace": "SwingWeapon", "file": ["sounds/whoosh1.ogg", "sounds/whoosh2.ogg"] }
]
```

| Field | Meaning |
|---|---|
| `id` | the name scripts use; give either this or `replace` |
| `replace` | a vanilla sound name, group, or index to replace instead |
| `file` | one file, or a list -- one is picked at random each time it plays |
| `volume` | multiplies how loud the game plays it; `0.5` is half. Above 1 raises a quiet use, up to the game's maximum |
| `loop` | loop until `sam_stop_sound("rain")` -- otherwise it plays until the floor changes |

An entry here wins over a folder file with the same name.

### Sounds on monsters and items

A custom monster sounds like the creature it is based on. Give it its own:

```json
{
  "id": "mymod:dire_rat",
  "base_type": "rat",
  "sounds": { "RatDie": "squeak_die", "RatSpot": "hiss" }
}
```

When this monster would play `RatDie`, it plays `yourmod:squeak_die` instead. Other rats are not
affected. The keys are vanilla names or groups -- the names say whose they are (`RatDie`,
`GoblinSpot`, `SkeletonSpot`), and [vanilla-sounds.md](vanilla-sounds.md) lists them all. The
values are your sounds.

An item works the same way, for whoever holds or wears it:

```json
{ "id": "mymod:thunder_blade", "sounds": { "SwingWeapon": "thunder_swing" } }
{ "id": "mymod:iron_boots",    "sounds": { "LeatherSteps": "clank" } }
```

## Music

### Replace a track

Drop a file named after the track in `music/replace/`:

```
music/replace/mines.ogg          the Mines level music
music/replace/mines_combat.ogg   the Mines fight music
music/replace/shop.ogg           inside a shop
music/replace/mainmenu.ogg       the main menu
music/replace/gameover.ogg       death
```

| Name | Plays |
|---|---|
| `mines`, `swamp`, `labyrinth`, `ruins`, `underworld`, `hell`, `caves`, `citadel`, `fortress` | that area's level tracks |
| `mines_combat`, `swamp_combat`, ... | that area's fight track |
| `mines01` ... `mines04`, `swamp01`, ... | one exact track |
| `minetown`, `library`, `temple`, `shop`, `intermission`, `tutorial` | those places |
| `herx`, `devil`, `minotaur`, `minotaur_maze`, `sanctum`, `escape` | bosses and set pieces |
| `mainmenu`, `intro`, `introduction`, `story`, `splash`, `endgame`, `gameover` | menus and scenes |

`/sam_music vanilla` in the console prints every name.

A vanilla area has several tracks that take turns. To give it several of yours, list them:

```json
"music": [ { "replace": "mines", "file": ["music/mine_a.ogg", "music/mine_b.ogg", "music/mine_c.ogg"] } ]
```

### Floors and maps of your own

```json
"music": [
  { "id": "deep", "file": "music/deep.ogg", "floors": [7, 8], "combat": "music/deep_fight.ogg" },
  { "id": "hub",  "file": "music/hub.ogg",  "maps": ["My Hub"] }
]
```

`floors` uses the same numbers as `sam_get_floor()`. `maps` matches a level's name -- a vanilla
one like `"Minetown"`, or your own level's. `combat` is optional and plays while monsters are after
the player there.

### Boss music

```json
{ "id": "mymod:warden", "base_type": "skeleton", "traits": ["boss"], "music": "warden_theme" }
```

The track plays while the monster is alive on the floor, the way Herx and the devil have theirs.
To start it only when a player gets close: `"music": { "track": "warden_theme", "range": 12 }`.

### From a script

```lua
sam_play_music("warden_theme")          -- every player; crossfades in over 1.5 seconds
sam_play_music("sting", 0, false)       -- cut straight in, play once, then hand back
sam_stop_music()                        -- the game crossfades back to what it would play
print(sam_get_music())                  -- "mymod:warden_theme", or a vanilla name like "mines02"
```

A script's track lasts until `sam_stop_music()` or the floor changes (pass `true` as the fourth
argument to keep it across floors). It outranks a monster's theme.

## Multiplayer

Every player needs your mod, as with any mod content. Replacements (`sounds/replace/`,
`music/replace/`) are applied on each player's own machine. Everything else is sent to the other
players **by name**, never as a slot number, so nobody hears a different sound because their mods
loaded in a different order.

What a player **without** your mod hears depends on whether the sound has a vanilla stand-in:

- **A monster's or item's own sound (a sound map)** is defined as a replacement for a vanilla
  sound, so it always has one. That player hears the vanilla sound it stands in for, not silence.
- **A sound a script plays** by its own id has no stand-in, so that player hears nothing for it
  and the reason is written once to their log.

Music a script starts reaches every player by name.

## When something does not play

Read `sam_log.txt`. Each of these is reported by name:

- **The file is not there.** Paths are relative to your mod folder: `"sounds/zap.ogg"`.
- **The file would not load.** The line includes the reason. Re-export it as `.ogg`.
- **Not a vanilla name.** The line points back here; check the spelling against
  [vanilla-sounds.md](vanilla-sounds.md) or `/sam_sounds vanilla`.
- **Unknown sound in a script.** `sam_list_sounds()` and `/sam_sounds` list every id that loaded.
