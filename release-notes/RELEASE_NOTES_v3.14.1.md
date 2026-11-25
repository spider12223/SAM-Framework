# S.A.M Framework v3.14.1

**A spell with `"projectile_type": "none"` now sends nothing.** A modder turned a Leap into an innate spell and found
that a `"none"` spell still fired its bolt: the value only labelled the tooltip as a self spell. To stop the bolt the
script had to cancel the cast in `player.on_spell_cast` and charge the mana itself. Now a `"none"` spell is cast like
any other and simply sends no bolt. No new field and no new script function: 365 functions, 96 events, as in 3.14.0.

## What changed

- A mod's spell with `"projectile_type": "none"` goes through the whole cast: its mana is spent (in singleplayer the
  casting animation spends it, as for any spell), a beginner can still fumble it, the cast counts in the compendium,
  a spellbook it was cast from can wear as usual, and `player.on_spell_cast` fires. Only the bolt is left out, and
  with it the cast sound the bolt plays: play any sound the spell needs from the handler.
- So a mod does the spell's work in `player.on_spell_cast` and lets the cast go on:

```lua
function on_event(e)
  if e.name == "player.on_spell_cast" and e.spell_name == "mymod:leap" then
    -- the leap itself (and its sound); the game spends the mana the normal way
  end
end
```

- `player.on_spell_cast` fires before the game's last checks on a cast: a beginner can still fumble it after the event
  (`player.on_spell_failed` follows), and a cast while swimming is refused after it with no event. A handler that does
  the spell's work does it on those casts too.
- `sam_cast_spell`, `sam_cast_spell_at` and `sam_cast_spell_pos` of a `"none"` spell send nothing and take no mana
  (they never take mana), but fire `player.on_spell_cast` as any of their casts does, so do not call them from that
  handler. `sam_monster_cast_spell` of one sends nothing and fires no event.
- Cancelling the cast to stop the bolt is no longer needed. In singleplayer it was also costly: the casting animation
  has already spent most of the mana by the time `player.on_spell_cast` fires, and a cancelled cast keeps it, so a
  script that cancelled and then charged the mana itself made a solo cast cost about twice.
- The Spell Editor's Projectile hint says what `none` does.

## Numbers

365 script functions, 96 events (unchanged). No new JSON field. The change is one check in the engine's spell cast
(`magic/castSpell.cpp`): a mod spell whose `projectile_type` is `"none"` gets no propulsion, and every use of the
bolt after that point already checks the propulsion. Nothing new crosses the network: a joiner's cast runs on the
host, as before.

## What has and has not been tested

**Automated, in the real game, nobody at the keyboard:** `tests/NoneSpellTest`, 6 checks, as a wizard. A 10-mana
`"none"` spell is put in the player's hands and the script presses Cast Spell: `player.on_spell_cast` reports it once
and its mana is gone afterwards (the check allows two points of regeneration; it read 10). Then `sam_cast_spell` of a plain missile spell sends a bolt, and of the `"none"` spell
sends none. With the new check switched off, only the last of these fails.

The regression script (`handoff/run_regress.sh`) was run on the release build, with no reruns: Hello 11, Speed 41,
Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27,
`QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with its 28 log checks), `MoveTest` 28, `SwayTest` 13,
`RootSwayTest` 21, `CompendiumTest` 2 (with its 27 log checks), `TauntTest` 4 (with its 6 log checks), `EmoteTest` 3,
3 and 1 for its three races (with 2, 2 and 3 log checks), `MomentumTest` 26, `PartyTest` 45, `ItemMoveTest` 56,
`PartyFixTest` 13 (with its 2 log checks), `NoneSpellTest` 6 and the installer layout check 14, with no failures.
Three of these tests have checks that need a person at the keyboard and nobody did them: Vision's sneaking checks,
the tin in both Neutral runs, and HelmTwin's checks where the player wears the items. `ChargeTest`, which needs a
person to swing at a goblin, is not in the script and was not run.

**Not tested:**

- **Multiplayer.** Nothing was tried with two machines; the cast runs on the host, so this is read in the code.
- **A monster casting a `"none"` spell on its own.** `sam_monster_cast_spell` sends nothing (read in the code); whether
  a monster's own AI ever picks such a spell was not looked at.
- **A beginner's cast, an overcharged cast and a cast from a spellbook**: these take the same path as any other spell
  up to the bolt (read in the code).
