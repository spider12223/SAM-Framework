# S.A.M Framework v3.12.2

**Enemies call a player of a custom race by the race's own name.** A modder making a Catfolk race noticed that when a
human turns on a Catfolk player it shouts "Foul human! En Guarde!". The game took that word from the body a race is
built on, and a Catfolk is built on the human body. Now it uses the race's `"name"`: "Foul Catfolk! En Guarde!". No new
field and no new script function: 343 functions, 87 events, as in 3.12.1.

## What changed

- A human or a shopkeeper that turns on a player of a custom race calls the player by the race's `"name"`, written as
  it is in the race file: "Thou art mine, vile Catfolk!", "Begone from my shop, filthy Catfolk!". The same goes for what
  it says when the player talks to it after it has turned on them.
- The Wanted status tooltip ("Decreed an enemy of the Merchant's Guild for your Catfolk crimes") and the line another
  player reads on examining the player in a co-op game ("You see a Catfolk named Bob.") use the race's name too, as does
  the rare line a follower says about a player it attacks.
- Only while the player is in the race's own body. With "disable monster abilities" ticked at character creation, or
  while polymorphed or shapeshifted, the game's own word stays, as it does for the game's own races (with the box
  ticked, a taunt calls them a human too).
- The game's own races are not changed, and monsters and followers are still called what they are.

```json
{ "id": "mymod:catfolk", "name": "Catfolk", "host_body": "human" }
```

## Numbers

343 script functions, 87 events (unchanged). No new JSON field: the race's existing `"name"` is used. The engine already
had a function that names a creature's kind and could be handed the creature's stats; 15 places that name a player now
hand it the player's, and S.A.M answers with the race's name when the rule above holds (`SAMRaces::speciesNameFor`).
Nothing new crosses the network: in multiplayer the host writes the speech bubbles and sends them as written, and the
tooltip and the examine line are written on each player's own machine from its own copy of the mod.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:** `tests/TauntTest` plays a test race on the human body, makes a
human and a shopkeeper turn on the player, and reads the game's own log, where the game also writes each speech bubble it
shows the player: "Damned Whiskerkin! En Guarde!" and "Begone from my shop, villainous Whiskerkin!", and no taunt
calls the player human. The same run logs what the engine would call the player in each case (the race's name in its
own body; the game's word with its abilities off, polymorphed, shapeshifted, as another body, and as the game's own
human race). 6 log checks pass; on the 3.12.1 exe 4 of them fail ("Damned human! En Guarde!").

The regression script (`handoff/run_regress.sh`) was run on the release build, with no reruns: Hello 11, Speed 41,
Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27,
`QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with its 28 log checks), `MoveTest` 28, `SwayTest` 13,
`RootSwayTest` 21, `CompendiumTest` 2 (with its 27 log checks) and `TauntTest` 4 (with its 6 log checks), with no
failures. Three of these tests have checks that need a person at the keyboard and nobody did them: Vision's sneaking
checks, the tin in both Neutral runs, and HelmTwin's checks where the player wears the items. `ChargeTest`, which needs
a person to swing at a goblin, is not in the script and was not run.

**Not tested:**

- **Talking to a human or shopkeeper that has already turned on the player**, the **Wanted tooltip**, the
  **examine line** and the **follower's line**: read in the code, not tried.
- **The "disable monster abilities" box, a polymorph and a shapeshift** were checked by asking the naming function
  with them set on the player, not by playing them.
- **A race on a body other than the human one.**
- **Multiplayer.** Nothing was tried with two machines.
- **The bubble on screen** was not looked at by a person; the log holds the text it shows.
