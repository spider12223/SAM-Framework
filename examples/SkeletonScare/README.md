# Running Skeleton

A worked example of the picture API (v1.10.3). One skeleton spawns on every floor and
runs you down. When it reaches you, the mod's own picture takes the whole screen.

## Make it yours

1. Replace `art/scare.png` with your picture. Any PNG or JPG works. Something around
   1280x720 is a good size; it is stretched to the screen, so match your monitor's shape
   unless you want it squashed.
2. Change `SPOT_SOUND` at the top of `main.lua` to whatever you want it to shout.
3. Change `SCARE_MS` if 0.9 seconds is too long or too short.

That is the whole mod. Nothing else has to change.

One thing worth knowing while you iterate: the game caches an image the first time it
draws it, so if you overwrite `art/scare.png` while Barony is running you keep seeing the
old one. `/images_cache_dump` in the console clears that, or just restart.

## How the picture works

Declare it once in `mod.json`:

```json
"images": [
  { "id": "runner:scare", "file": "art/scare.png" }
]
```

Then show it:

```lua
sam_show_image(0, "runner:scare", 900)
```

Player 0, the picture, 900 milliseconds. It removes itself when the time is up, so there
is nothing to clean up and nothing to forget.

Declaring the image in `mod.json` is worth doing rather than passing the raw path. The
file is checked when the mod loads, so a typo is one line in `sam_log.txt` at load time
instead of a picture that silently never appears while you wonder why.

## The chase

The skeleton is a plain vanilla skeleton. What makes it run is `sam_monster_charge`,
which is a dash that ends by itself, re-issued on a timer:

```lua
sam_monster_face(runner, px, py)
sam_monster_charge(runner, 45)
```

It only hunts what it can actually see. `sam_line_of_sight` is the engine's own trace, so
it agrees with what is drawn on screen rather than guessing from distance.

## Multiplayer

`sam_show_image` takes a player number, and the picture appears on that player's screen
even when that player is on another machine. Scripts run on the host, so the host sends
the image name and the client draws it from its own copy of the mod.
