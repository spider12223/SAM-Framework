# S.A.M Framework v3.5.0

**Summons that fight for you, and custom spells that do what they say.** A modder asked how to
make a spell that summons two rats, and why a custom spell that applied their own status effect
did nothing. Answering that turned up one missing function and several things that were wrong.

## `sam_make_follower`

```lua
-- player: a player index, 0 to 3. x, y: map tile coordinates, not pixels
-- (sam_get_position(sam_get_player_uid(player)) returns them).
local rat = sam_spawn_monster(x, y, "rat")          -- a wild rat, for the moment
if rat and not sam_make_follower(player, rat) then
  sam_remove_entity(rat)                            -- refused: do not leave a wild one behind
end
```

`sam_make_follower(leader, uid)` turns a monster into a real follower of a player, the way the
Scroll of Summon does: it fights on the player's side, stops hunting them, shows on the ally panel
and takes orders from the follower menu. `leader` is a player index (0 to 3) or the uid of a
player's entity. A number from 0 to 3 is always read as an index, so if a player's own uid falls
in that range, pass the index. It runs on the host only, which is where your events and timers
already run. Called on a joining player's machine it returns `false` and logs one warning. A player
on another machine can be the leader when the host makes the call, but that path is untested (see
below).

Before this, no function took a monster you had spawned and made it a follower. The nearest a
script came was firing a Dominate bolt at it with `sam_cast_spell_at`, which has to hit, takes mana
from the caster equal to the monster's health, and stops at Dominate's own limit on dominated
followers. `sam_spawn_monster` and `sam_spawn_monsters` make ordinary monsters that hunt the
player.

It is **stricter than the scroll**, on purpose. It refuses (returns `false` and logs one line in
`sam_log.txt`) a boss (a mod monster flagged as a boss counts), a shopkeeper or other trader, a
mimic, a hologram, a bat, a tinkering construct (a mod monster flagged as one counts), an Inner
Demon or a summoned ally. These are the same creatures the Dominate spell refuses. It also refuses
a monster that already has a living leader, even the same player's, and a player who already leads
as many followers as their Leadership allows: 4 below Leadership 60, 6 from 60, 8 from 80, the same
cap as recruiting by hand (tinkering bots do not count). A monster that is already dying is
refused too, but without a log line. The Scroll of Summon ignores that cap, so a spell that
summons several should check the result, as above.

The call itself gives no Leadership experience and records no recruit in the Compendium (the Scroll
of Summon does record the recruit there). After that the monster is an ordinary follower: when it
kills something, its leader gets Leadership experience and the kill is credited to the leader in
the Compendium, as with any follower. Spawn with `sam_spawn_monster`, which returns the uid;
`sam_spawn_monsters` returns only a count. Lua and JavaScript run the same code.

The full recipe, a spell that summons two allied rats, is in
[docs/scripting-reference.md](docs/scripting-reference.md) under "A custom spell that does
something new".

## Custom spells and custom effects

- **A custom spell's `on_hit_effect` now also lands for the poison, acid spray, steal weapon, drain
  soul and charm payloads, which used to skip it.** Those payloads report their hit to the game
  under the vanilla spell's number, so the custom spell's effect was skipped: a poison bolt
  poisoned the monster and it never got the effect. Now the effect arrives whenever the game
  counts the bolt as a hit. For poison, acid spray and drain soul that is every hit that lands.
  Steal weapon counts only when it actually takes a weapon, and charm only when the monster is
  charmed or pacified, so a bolt that is refused gives no effect, such as a steal weapon bolt at an
  unarmed monster or a charm bolt at a boss. Two payloads never report a hit, `ghost_bolt` and
  `dig`, so an effect on those does nothing. Locking and opening report a hit only on a mimic. For
  the payloads that apply a status of their own (slow, confuse, sleep and similar) the effect still
  arrives only when that status lands too. Of the five payloads, only the poison bolt has been run
  in the game.
- **`spell_name` is the spell's own id.** `player.on_spell_cast`, `player.on_spell_failed` and
  `player.on_spell_learned` now give a mod spell as the id its JSON declared, `"mymod:summon_rats"`,
  the same form `sam_get_spells` reports. Before, they gave an internal name like
  `spell_sam_mymod_summon_rats`, so a script comparing against the id never matched. A vanilla
  spell still answers with the game's name, such as `"spell_forcebolt"`. **This changes what a
  script sees:** one that compared against the internal name stops matching. None of the example
  mods or tests in this repository did.
- **A spell tells you when its effect does not match.** For one of your own effects,
  `on_hit_effect` needs the effect's exact id: lowercase, with the namespace, `"mymod:plague"`.
  `plague`, `Plague` and `EFF_PLAGUE` match nothing. A built-in effect can be written `EFF_POISONED`
  or `poisoned`, in any case. A name that matches nothing was already ignored, and 3.4.0 already
  logged an "unknown, ignored" warning for it. That warning now also names the form to use, and a
  spell whose effect did resolve writes an info line with the id you wrote and the effect slot it
  found. Each of these lines appears twice per launch, once while the mods load and once after
  the game data reloads. The spell's tooltip shows its status tag only when the effect resolved,
  where before it showed one for a name that matched nothing.
- **An effect icon that is not in the mod folder** now logs a warning and uses the default icon.
  Before, the effect had no icon at all.
- **The Mod Builder's Spell Editor** lists your own effects and the built-in ones in the On-hit
  effect box, and warns in the box when what you typed will not match. If it is a near miss of one
  of your own effects (wrong case, or the namespace left off), the warning names the id to use. For
  any other text it only says to pick one from the list. The editor knows only the effects of the
  mod you are editing, so an id from another mod also gets a warning, even though the game accepts
  it when that mod is loaded.
- **Text that had gone stale is corrected.** The spell schema and the Spell Editor no longer say
  that casting and the spell screen are still to come, the Start Here page no longer says the
  spell's on-hit effect is not wired up, and the monster schema no longer says a monster flagged
  friendly "spawns allied" (it does not attack, but it is not a follower). The Effect Editor and
  the Start Here page now say a custom effect's speed change does nothing to a monster.

A cancelled cast is **not always free** in single-player. `return false` from
`player.on_spell_cast` stops the bolt, but in single-player the casting animation takes the spell's
mana one point at a time while it plays, and the event fires at the end of it. What the animation
had already taken is not given back and the rest is never taken, so a one mana spell (the default
for a custom spell) costs nothing and a bigger one costs part of its price. Online nothing has been
taken yet. This is read from the code, not measured. The docs said it cost nothing; that is true
online only.

## Also

- [CONTRIBUTING.md](CONTRIBUTING.md), a map of the project for anyone who wants to help: where
  things live, how to add a function, an event or a field, and the checks to run before a pull
  request. It is built on one written by rumpleivskin (Kyle), whose text it keeps in large part,
  corrected for how S.A.M is really released.
- A fix from Kyle in the engine source: the `/sam_alchemy` console command declared a function
  inside a namespace, where GCC and Clang read it as a different function from the one the game
  defines, so the link fails (Visual Studio links it). It is now declared at file scope, so it
  names the game's own function. That should link on GCC and Clang, but we did not build with
  either. The fix is in the 3.5.0 engine source, which is not on GitHub yet: the public Barony-SAM
  repository still has the 3.4.0 code, so someone building the engine themselves does not get it
  until that repository is updated. The shipped `barony.exe` is built with Visual Studio and is not
  affected.
- The README's paragraph on building from source named the wrong way to get the patched engine; it
  now points to Barony-SAM.

## Numbers

337 functions to 338 (`sam_make_follower`), still 87 events. Nothing was removed. Apart from the
`spell_name` change above, a mod written for 3.4.0 keeps working. Three things look different
without touching a mod: a custom spell built on poison, acid spray, steal weapon, drain soul or
charm now applies the `on_hit_effect` its JSON already asked for, on the hits where that payload
does its own job; a spell whose `on_hit_effect` matches nothing no longer shows a status tag in its
tooltip; and an effect whose icon file is missing now gets the default icon, with a warning in
`sam_log.txt`. `sam_log.txt` also has a few new lines about spells and effects.

## What has and has not been tested

In the real game, in singleplayer, started and run by the test runner with nobody at the keyboard
(the spells in these tests are cast by a script, not by a person through the spell menu):

- `FollowerTest` and `FollowerTestJS` (60 checks): a spawned monster is made a follower and is
  afterwards on the player's side. Two recruited rats placed a few tiles from the player were
  watched for four seconds, in Lua only: at every check (five a second) neither had the player as
  its target and the player lost no health, while a wild rat placed the same way did go for the
  player within five seconds. A monster is refused when it is a minotaur, a shopkeeper, a sentry
  bot or a mimic, when it already follows the same player, and when the player is at the follower
  cap, which the test computes from the Leadership the player really had (4). It is also refused
  for bad arguments: a player index with nobody behind it, -1, a monster's uid as the leader,
  uid 0, a uid that is not in the world, and the player's own uid as the monster. The before and
  after, the refusals and the cap run a second time through JavaScript. The test was checked by
  breaking the code on purpose in five ways (doing nothing, no cap, no ally flag, no "already has a
  leader" check, no boss refusal): each made it fail. On a build without the function it fails at
  once, which proves little by itself; the five breakages are what show the checks can fail.
- `SpellEffectTest` (15 checks): a custom spell applies a custom effect to a monster on a force
  bolt and on a poison bolt (poisoned, and the effect too); a misspelled id applies nothing while
  the bolt still hits; for a mod spell, `player.on_spell_cast` as heard by a Lua script gives
  `spell_name` as the id its JSON declared and `spell_id` as the runtime id (2000 or more), and no
  cast gave the internal `spell_sam_` name; a vanilla spell still reports the game's name; and the
  recipe in the docs, a script on `player.on_spell_cast` that spawns two rats, makes them
  followers and returns `false`, runs with no missile and no damage. An earlier 11-check version
  of this test, run on the shipped 3.4.0 build, failed four checks: the declared `spell_name`, its
  runtime id, the missing internal name, and poisoned plus the effect. The 15-check version was
  not run on 3.4.0; by reading it, at least seven of its checks cannot pass there: those four and
  the three recipe checks (two rats spawned, both are allies, no missile), because 3.4.0 reports
  the internal spell name, so the recipe's handler never matches.
- The regression suite on the release build: Hello 11, Speed 41, Settings 88, Loot 90, Audio 31,
  Vision 17, Neutral 11 and 3, HelmTwin 55, with no failures. The errors those tests leave in the
  log are the refusals they provoke on purpose. Three of those tests have checks that need a
  person at the keyboard, and nobody did them this time: Vision's four sneaking checks (21 in all
  with Sneak held), the tin in both Neutral runs (nobody ate it, so `can_open_tins` was not
  exercised), and HelmTwin's three checks where the player wears the hats, masks and gloves.

Outside the game:

- The Mod Builder's Spell Editor, run once from the dev server in one browser, with one custom
  effect added by script rather than made in the Effect Editor. Read from the page text, not
  judged by eye: the On-hit effect list held that effect first, ahead of the 135 built-in ones;
  entering `plague` showed the warning naming `mymod:plague`; and entering `mymod:plague` showed a
  tick. The dropdown itself was never opened, and no other browser, and none of the other pages or
  text that changed (Start Here, the Effect Editor hints, the schema text), was looked at.
- The Mod Builder's own tests pass: 171 for the block builder and 25 new ones for the effect-name
  check.

**Not tested:**

- Multiplayer, every part of it: a remote player as `leader` (the game's own recruit packet and
  ally panel are used, but nobody watched them), and a stock player on a S.A.M host.
  `spell_name` on the two events a joiner's own game names and reports to the host,
  `player.on_spell_learned` and `player.on_spell_failed` when a cast fails for lack of mana (the
  host passes that name on as sent, so a joiner still on 3.4.0 would report the old internal name;
  a joiner's cast and its fizzle are named by the host).
- In singleplayer, the ally panel and the follower menu: nobody looked at the panel or gave a
  recruited monster an order. The tests see the follower list only through the cap, and the panel
  draws from that list. They also read the friend and enemy answers, the ally colour, who the
  monster targets and the player's health. Nothing reads the ally index that the follower menu
  goes by, so that part rests on the call copying the steps of the Scroll of Summon. Which orders
  the menu offers depends on the player's Leadership and Charisma, as for any follower.
- The other refusals were read in the code but never provoked: a hologram, an Inner Demon, a
  summoned ally, a bat, a monster that is dying, a trader other than the shopkeeper, the gyrobot,
  spellbot and dummybot, and the rule that tinkering bots do not count toward the cap.
- The follower cap at 6 and at 8: the test runner builds its character before any mod loads, so a
  class patch cannot reach it, no script function sets a skill on a character already in the game,
  and the runner's `-samtestclass` only matches the classes from Barbarian to Monk (any other name
  is ignored without a warning), so only the cap of 4 was exercised.
- A human follower (they roll a name); a follower across a floor change; the recipe with two real
  players.
- Spells and effects: a custom effect landing from an acid spray, steal weapon, drain soul or
  charm bolt (only force and poison bolts were fired; those four use the same fix as poison but no
  test cast them); `spell_name` on `player.on_spell_failed`, on `player.on_spell_learned` and in
  the JavaScript `player.on_spell_cast`, which were changed the same way but no test listens to;
  what the spell's status tag changes on screen (the game uses it only on the tooltip of a
  spellbook or tome that teaches the spell, and nobody opened one); the warning and default icon
  for an effect whose icon file is missing; a spell naming an effect from another mod; and that a
  cancelled cast in single-player keeps the mana the casting animation already spent. Casting by
  hand from the spell menu was not tested: every cast in `SpellEffectTest` is a script cast
  through `sam_cast_spell_at`, which is free and does not need the player to know the spell, so
  that a player has to know a spell before `player.on_spell_cast` fires for a hand cast also comes
  from reading the code.
- `ChargeTest` was not run again: it needs a person to swing, and nothing it covers was touched
  (only `castSpell.cpp`, `actmagic.cpp` and `consolecommand.cpp` changed in the engine).
- The engine fix for GCC and Clang was built with Visual Studio only.

**Known, not fixed here:**

- `sam_fire_hook` turns every number into a whole number by dropping the part after the decimal
  point (1.9 arrives as 1, and -1.9 as -1), and turns `true` into `1` and `false` into `0`, in both
  Lua and JavaScript. Strings cross unchanged. A field whose value is a table, an array or a
  function is dropped as well, with nothing in the log. Until it is fixed, send only numbers, flags
  as 1 or 0 (compare them with `== 1`, since 0 counts as true in a Lua `if`), and strings, and send
  decimals multiplied up and rounded.
- A custom effect on a **monster** is quieter than on a player, from reading the code. Its
  `move_speed_mult` does nothing to a monster, and its `hp_per_second` drains silently: no damage
  numbers, the health bar does not refresh, and a monster it kills gives nobody experience,
  because the drain records no killer. A custom effect's icon on the enemy health bar is a
  snapshot taken only when the bar updates (a hit, or a poison, bleed or burn tick, from you or an
  ally), so it shows only if the effect is on the monster by then, and the whole bar fades about
  2.4 seconds after the last update. Attribute changes, `ac_mod` and `damage_mult` do work on
  monsters.
- A spell can name an effect from another mod, but only if that mod loads first (mods load their
  dependencies first, then in alphabetical order of namespace). If the effect's mod loads after the
  spell's, a normal mod load logs a false "is unknown" warning for the spell and then resolves it
  once every mod is in, so the spell still works. `/sam_reload` does not do that second step, so
  after it the spell applies nothing until the mods are loaded again. Listing the effect's mod
  under dependencies makes it load first. This is from reading the code and has not been run.
- S.A.M still has no race setting that adds to the game's own table of which species each player
  body can recruit by hand (pressing Use). A rat race built on the gremlin body cannot recruit
  rats that way, whatever its `allies` list says. Recruiting is not decided by `allies` alone:
  below Legendary Leadership the game asks two questions in turn, whether the creature is friendly
  to the player (a race's `allies`, `neutral` and `enemies` lists count here) and whether the
  player's body type may recruit that species (a human body may recruit any friendly creature,
  other bodies only certain species, mostly their own kind). At Legendary Leadership (100) the
  friendly check is skipped and a separate fixed list of body types and species decides. This is
  from reading the code, not from a test. A script can still make any player lead a monster with
  `sam_make_follower`, which does not look at the player's body.
