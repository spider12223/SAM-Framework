# Party Test

An unattended test of party members, in a real game.

```bash
tools/run_mod_test.sh PartyTest
```

A second after the game starts it reads `sam_party_choices`, checks four refusals, then spawns
three party members beside player 0: Ash (a human warrior who comes back), Birch (a goblin
wizard with `respawn = false`) and Cedar (a healer whose `party.on_member_died` handler returns
false). It reads and changes them, kills all three, checks that only Ash is waiting to come
back, takes the stairs with `sam_travel_to_level`, and checks that Ash, and only Ash, rejoined
beside player 0. 45 checks, then `sam_test_done`.

The lobby half (links, cards, what a client sees) needs a lobby and a second machine, so it is
checked by hand; see `docs/party-and-lobby.md`. Here only the in-game answers of the lobby
functions are checked: no card outside a lobby, and slot 0 reads as a player.
