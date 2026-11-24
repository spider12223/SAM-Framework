# Item Move Test

An unattended test of gear on allies, in a real game.

```bash
tools/run_mod_test.sh ItemMoveTest
```

A second after the game starts it makes a human and a kobold follow player 0 (and leaves a goblin
wild), reads their inventories, and checks every refusal that needs no item. Then it hands the
human a helmet from player 0's backpack and checks the SAME uid is in its pockets, that a kobold
may not wear it (`race`) and a human may, that a handler's veto gives `mod_veto` with the
handler's own message, that equipping keeps the uid, that unequipping and taking it back work,
and that a cursed helmet goes on and then stays on. It ends with `sam_test_done`.

Only the player on this machine is tested. A player on another machine needs a second machine and
is played by hand; see `docs/ally-items.md`.
