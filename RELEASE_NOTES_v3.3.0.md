# S.A.M Framework v3.3.0

**Neutral creatures, and races that open tins by hand.** Two more requests from the Workshop
comments, both from someone building a custom race.

## Neutral

```json
{
  "id": "mymod:wanderer",
  "host_body": "human",
  "neutral": ["human"]
}
```

A race's JSON already had `allies` and `enemies`. `neutral` is the third answer: creatures that
will not attack the race on sight, and are not its allies either. It is the relation a Gremlin has
with Goblins in the base game.

It exists because the two lists could not express it. A race inherits its host body's relations,
and the base game makes human monsters the **allies** of a human body: they leave you alone, and you
can recruit them. A human-bodied race that humans should merely tolerate needed a way to take
humans out of its allies without making them hostile. `"neutral": ["human"]` is that.

What neutral means in play:

- A neutral creature does not attack you on sight, and your followers leave it alone.
- It is not your ally: it will not join you. Legendary Leadership (skill 100) is the exception,
  because the game then recruits by body type and ignores every allegiance list, the same way it
  lets a Gremlin recruit Goblins.
- Hit one and it fights back.
- A shopkeeper listed as neutral will trade with you, which is how a race on a body shops refuse
  (a goblin, say) gets served. Not while you are shapeshifted: the base game refuses service to a
  shapeshifted player, and that stands.
- A creature in more than one list gets the least friendly of them: enemies, then neutral, then
  allies. The game log says so when it happens.

Like the other allegiance lists, it is ignored for a player who turns the race's abilities off at
character creation.

## Tins without a tin opener

```json
"can_open_tins": true
```

The race eats a tin without a tin opener, as a Goatman or an Automaton does (a race on either of
those bodies already could). Like theirs, it covers eating: the alchemy table still wants an
opener.

## In the Mod Builder

The Race Editor has a **Neutral** list beside the other two, an **Opens tins** checkbox, and a
warning when a creature is in more than one list.

## Also

- A race that lists shopkeepers as an ally no longer keeps trading with you while you are
  shapeshifted; it only applies in the race's own body, as the base game intends.

## Numbers

Still 336 functions and 87 events: this release adds two race fields. Nothing was removed. The one
change in meaning is the shopkeeper fix under Also: a race's declared shopkeeper relation now holds
only in the race's own body and not while shapeshifted. Otherwise a mod written for 3.2.0 keeps
working.

## What has and has not been tested

In the real game, in singleplayer, with a test mod (`NeutralTest`) and two races:

- A human-bodied race with humans neutral: a human is neither its enemy nor its friend, in both
  directions. It stood beside the player for three seconds without targeting them, and kept the
  player as its target once one was given (what a hit does). The check that the human is not a
  friend fails when the neutral entry is removed, which shows it tests the right thing.
- The both-lists rules: ally and neutral gives neutral, enemy and neutral gives enemy.
- A goblin-bodied race with neutral shopkeepers is not a shopkeeper's enemy. Without the entry it
  is, and the check fails.
- The tin: a race with `can_open_tins` ate a tin with no opener in the backpack, used by hand in
  the game.

The other test mods (`HelloTest`, `SpeedTest`, `SettingsTest`, `LootTest`, `AudioTest`,
`VisionTest`) still pass. A review of the change traced every place the game decides friend or foe
and found no defect in how neutral behaves; it found the Legendary Leadership wording and the
shapeshifted shopkeeper above, both fixed.

**Not tested:** multiplayer, the shapeshifted shopkeeper case in play, and a race without the flag
being refused a tin by hand (that path is the base game's own and is unchanged). If something is
wrong in co-op, the host's `sam_log.txt` should say so.
