# S.A.M Framework v3.12.1

**A race that opens tins can now use them at the cooking screen too.** A player of a custom race with `"can_open_tins"` reported that
the race eats a tin without a tin opener but cannot cook one at a cauldron or frying pan without one. That was the
base game's rule: the game's own Goatman and Automaton eat a tin without an opener too, and still need one to cook a
tin, and `can_open_tins` copied it. Now the flag counts at the cooking screen as well. No new field and no new script function:
343 functions, 87 events, as in 3.12.0.

## What changed

- A race with `"can_open_tins": true` can put a tin in at the cooking screen of a cauldron or a frying pan (the
  frying pan's Cook option, or holding Block and pressing Use with it in the off hand) without a tin opener in the
  pack. Before, the tins sat greyed out there with a note that a tin opener is needed.
- Cooking itself was never what needed the opener: the game only checks for one before letting a tin into that
  screen, and that check is where the race flag now counts.
- The game's own Goatman and Automaton keep the game's rule at the cooking screen: they still need an opener there.
  A race on either of those bodies without the flag is the same.
- As with eating, the flag does nothing for a player who turned race abilities off at character creation.

```json
{ "id": "mymod:catfolk", "host_body": "human", "can_open_tins": true }
```

## Numbers

343 script functions, 87 events (unchanged). No new JSON field. The change is one check in the engine's cooking
screen (`interface.cpp`). Nothing crosses the network: the check runs on the cooking player's own machine.

## What has and has not been tested

**Seen in the game by a person:** a test race with `can_open_tins` on a human body, given three tins and a frying
pan and no tin opener (`tests/TinCookLook`): the frying pan's Cook option opened the cooking screen and the tins went
into it. Eating a tin without an opener still works.

**Automated, in the real game, nobody at the keyboard:** the cooking screen cannot be opened by a test with nobody at
the keyboard, so there is no automated check of this change. The regression script (`handoff/run_regress.sh`) was
run on the release build, with no reruns: Hello 11, Speed 41, Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11
and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27, `QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each
with its 28 log checks), `MoveTest` 28, `SwayTest` 13, `RootSwayTest` 21 and `CompendiumTest` 2 (with its 27 log checks), with no failures.
Three of these tests have checks that need a person at the keyboard and nobody did them: Vision's sneaking checks, the
tin in both Neutral runs, and HelmTwin's checks where the player wears the items. `ChargeTest`, which needs a person
to swing at a goblin, is not in the script and was not run.

**Not tested:**

- **The cooked result.** The person saw the tins go in; what came out was not checked.
- **A cauldron.** Only the frying pan was tried; a cauldron opens the same screen.
- **Multiplayer.** Nothing was tried with two machines.
