# S.A.M Framework v3.0.0

**Multiplayer.** A mod used to be a singleplayer mod that happened to run while other people were
in the game. Its HUD drew on the host's screen and nobody else's. It read the host's backpack when
you asked about a joiner's. It never heard a joiner press a key. This release makes every one of
the 316 functions and 76 events say what it does in co-op, and makes the game enforce it.

Twenty-nine new functions on top of that, taking the API from 287 to 316: sounds and music you can
drop in a folder, stat modifiers that stack between mods, immunities, an XP curve, and a camera.

## You write one script

You do not write a host version and a client version. You write the mod as if for one machine,
name the player you mean, and the framework carries each call to the machine that has to run it.

```lua
function on_event(e)
  if e.name == "player.on_kill" then
    sam_apply_effect(e.player, "FAST", 200)                            -- the host decides
    sam_hud_text("combo", 40, 40, "Nice hit!", 0xFFFFFFFF, e.player)   -- their screen
    sam_grant_spell(e.player, "spell_fireball")                        -- their spell list
  end
end
```

Three functions, three different machines, one obvious script. Before this release the second line
drew on the host's screen and the third silently did nothing.

## Every function has a stated kind, and the game applies it

Each function is one of seven kinds, written down in `framework/sam_mp_contracts.inc`, printed on
every entry in the function reference, and applied by the game before the function body runs.

| kind | what it means |
|---|---|
| `host` | The host decides and the result reaches everyone. |
| `owner` | The state lives on that player's own machine. The call is carried there. |
| `screen` | Shows something to one player, on their screen. |
| `read` | Reads a player. The host reads anyone; a client reads only itself. |
| `all` | Changes a table every machine keeps its own copy of, and is replayed to late joiners. |
| `local` | Answers for the machine running it: its clock, its keyboard, its music. |
| `any` | The same answer everywhere. |

Because the kind is enforced rather than described, the documented behaviour is the real one. A
call that cannot be honoured logs one warning naming the function and returns a refusal value the
reference spells out, chosen so it can never be mistaken for a real answer.

**This is the part meant to last.** A new function cannot be added without a contract: the ship
gate refuses to publish a function that does not register through the trampoline and name its kind.
The old failure mode, where a function was written for singleplayer and quietly did the wrong thing
in co-op for a year, is now a build error.

## What the host could not see before

The host does not hold a joiner's backpack or spell list. It only ever had their equipped copies.
So `sam_get_items(1)` answered with the host's own inventory and `sam_grant_item(1, ...)` put the
item nowhere. There is now a mirror of each remote player's items and spells on the host, kept up
to date by that player's own machine, and writes are carried back to the owner.

A joiner's keys and bound actions reach the host too, so `player.on_key_pressed` and
`player.on_action_pressed` fire for everyone in the game rather than for player 0 only. Panels a
joiner clicks fire their click on the host, with `e.player` set to the joiner.

## Players who are not running S.A.M

A player without the framework, or with the framework but without your mod, still plays. Nothing
S.A.M specific is sent to them: the game keeps using its own packets, so a stock unmodified Barony
5.0.2 client can join a S.A.M host and play the game normally. What that player cannot have is the
part of your mod that only exists on their machine, and the reference says per function what they
get instead.

A player who joins after the game started is caught up: everything of the `all` kind is replayed to
them in order, so class patches, item patches and species rules are the same for them as for
everyone who was there at the start.

## Sounds and music

```
mymod/sounds/zap.ogg              -> "mymod:zap", no JSON
mymod/sounds/replace/Casting.ogg  -> every spell in the game now casts with your sound
mymod/music/boss.ogg              -> sam_play_music("mymod:boss")
mymod/music/replace/mines.ogg     -> the Mines play your track
```

Six functions: `sam_play_music`, `sam_stop_music`, `sam_get_music`, `sam_list_music`,
`sam_list_sounds`, `sam_stop_sound`. A monster or an item can carry its own sound map, so a custom
rat squeaks its own squeak and a pair of boots has its own footsteps.

Replacements are resolved at the moment a sound plays, on each machine, so the network still
carries the vanilla sound. A player with your mod hears your sound and a player without it hears
the original. Nobody hears the wrong one.

A vanilla name can mean one sound (`SwingWeapon3V1`) or a whole group (`SwingWeapon`, all five
swings). `docs/vanilla-sounds.md` lists every name the game has.

## Rules that stack between mods

```lua
sam_add_stat_modifier(player, "STR", "bearform", 4, 1.0)
sam_add_stat_modifier(player, "SPEED", "bearform", 0, 1.25)
sam_remove_stat_modifier(player, "bearform")
```

Adds are summed and multipliers multiplied across every mod, then applied as
`(base + adds) * multipliers`. Two mods each giving +2 STR give +4. Two mods each halving speed
give a quarter. Neither has to know the other exists, and each takes back only its own.

Immunities work the same way, per player, per creature or per species:
`sam_set_immunity`, `sam_set_monster_immunity`, `sam_set_species_immunity`, `sam_is_immune`,
`sam_clear_immunities`. Skeletons that cannot be poisoned; automatons that cannot be charmed; and a
script that can ask before wasting a cast, which vanilla cannot.

And the experience curve. Barony charges a flat 100 experience at every level, with the scaling
version commented out beside it in the engine, so every progression mod has had to write EXP
directly and fake the rest. `sam_set_xp_curve` sets what the next level costs, `sam_grant_xp` gives
experience that levels a player up naturally, and the XP bar follows the curve on clients too.

There is also a new hook, `player.on_before_effect_applied`, which can change or refuse a status
effect before it lands.

## The camera

```lua
sam_set_camera_offset(player, 2.5, 1.2, 0)   -- a third person camera, in one call
sam_show_own_body(player, true)
```

Eight functions: an offset camera that follows the player's look and shortens its boom against
walls, a fixed position in the world, a fixed angle, a target it keeps pointed at, collision on or
off, a read of where the camera actually is, and a reset that hands it back to the engine.

`sam_show_own_body` is the other half. Normally the engine hides your body, because the camera is
inside your head, and draws a first person weapon instead. Any camera outside the head needs the
body shown and that weapon hidden.

## Arms that bend

A custom race can now declare the arm it draws while that hand is holding something:

```json
"limb_models": {
  "arm_right": { "model": "mymod:arm_right", "bent": "mymod:arm_right_bent" },
  "arm_left":  { "model": "mymod:arm_left",  "bent": "mymod:arm_left_bent" }
}
```

The right arm bends for a weapon and during a spell windup, the left for a shield, lantern or
quiver but not a spellbook, exactly as the built in races do. The engine gets this by stepping two
indices forward through its own four arm models, which a mod's single appended model cannot do, so
a custom race used to hold everything in one frozen pose.

## The Mod Builder stops losing your work

Three of these were found by auditing this release rather than by anyone reporting them, which
means they had been quietly costing people work for a while.

- **An editor used to delete any field it had no control for.** Open a race in the Race Editor and
  save it, and `first_person` and `extra_limbs` were gone: the editor rebuilt the definition from
  the boxes on screen and nothing else. The same went for `model_states` on an item, `blood_diet`
  and `difficulty` on a monster. Editors now carry through every key they do not own, including
  keys from a schema newer than the editor.
- **A tinkering-kit item did not survive an export and re-import**, because `kit_ui` was missing
  from the item schema while the schema refuses unknown keys. One round trip and the item was gone.
- **The Race Editor mangled any race that used the object form of a limb** (`scale`, `offset`,
  `pitch`, `roll`) when you reopened it.
- The block builder no longer generates code that assumes `event.player` is a real player on the
  four events that fire for monsters and traps, where it is -1. A blank parameter in a custom block
  used to generate `{name}`, which is a Lua table constructor, so a boolean flag silently turned
  itself on.
- New Music editor; the sound picker offers a whole group as well as the single sounds in it; and
  "Changes Since Last Export" now notices changes to music, sounds, races, spells, recipes and
  patches, which it had been reporting as "no changes".

## Also

- `sam_log_2.txt`: a second copy of the game running on the same computer writes its own log, which
  is what makes testing co-op on one machine bearable.
- `docs/multiplayer.md` is new, and every entry in the function reference now ends with a
  **Multiplayer:** line generated from the same table the game enforces.
- `sam_get_inventory()` now reports a custom item's real id (`yourmod:thing`) where it used to say
  `custom`. If your script filters on that field, this is the one change worth checking.
- In JavaScript, a refused read now answers `undefined` rather than `null`. That matters because
  `null < 10` is true and `undefined < 10` is false: a mod testing a value it was never given used
  to take the branch. Wherever Lua answers `nil`, JavaScript now answers `undefined`, with no
  exceptions.
- A player index that names no seat is refused with a warning instead of quietly using the caller's
  own screen. `sam_ui_open(..., 5)` used to open the panel on the host and report success, in
  singleplayer too.
- A player index given as a string (`"2"`) now means player 2 in JavaScript, as it always did in
  Lua. Before, the two languages sent the same call to different machines. A fractional index is
  refused in both, rather than meaning player 0 in Lua and player 2 in JavaScript.
- A missing required argument now raises in JavaScript the way it does in Lua, in the 37 functions
  where it did not. Three of them used to answer with a plausible wrong number:
  `sam_get_distance_to` and `sam_is_tile_diggable` measured to tile (x, 0) when you left out `y`,
  and `sam_grant_gold` with no amount granted nothing, sent the packet, and returned true.
- `sam_identify_item` works again on an item that is not in anybody's backpack: a fresh drop, a
  chest, a shop. It had started refusing them.

## Numbers

287 functions to 316. 74 events to 76. Nothing was removed and no argument list changed meaning, so
a mod written for 2.8.0 keeps working.

## What has and has not been tested

The singleplayer halves of this release were tested in game: the rules batch, the audio batch and the
camera each have a test mod that runs inside Barony and checks its own results, and each passed. Those
runs were made while the batch was being built, not against this exact build. **The multiplayer layer
has not been played at all yet.** It builds, it passes its gates, and it has been through
an audit that found a genuine channel-stalling bug and thirty smaller ones, all fixed, but finding
bugs by reading is not the same as playing a game. If something is wrong in co-op, `sam_log.txt` on
the host will say so, and I would rather hear about it than have you work around it.
