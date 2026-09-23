# Hello Test

The smallest mod `tools/run_mod_test.sh` can run, and a template for your own test mod.

```bash
tools/run_mod_test.sh HelloTest
```

A second after the game starts it checks a few things about game speed, the loot pool and its own
setting, writes each check to `sam_log.txt`, and ends with `sam_test_done(pass, fail)`. The runner
turns that into an exit code: 0 when everything passed. In an ordinary game `sam_test_done` does
nothing, so the mod is harmless to load: start a game and read the log.

Copy the folder, change the namespace in `mod.json`, and replace `runChecks` with your own. See
`docs/testing-mods.md` for the flags, the exit codes and which mods can be tested this way.
