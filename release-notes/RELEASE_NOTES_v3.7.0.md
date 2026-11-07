# S.A.M Framework v3.7.0

**A race that recruits what you choose, spells with a school and real requirements, and a handful of
fixes to things that were quietly broken.** The modder who sent the three requests in 3.6.0 sent a
list of nine more. This release builds the ones that were small: the race recruit list, the spell
school and requirements, spellbooks from the Spell Editor, the spell icon that fell back to the generic
orb with nothing in the log, the weapon tooltip, and a Class Editor fix. The larger ones are not in:
see "Not in this release". There is no new script function; it is 339 functions and 87 events, as in
3.6.0.

## A race's `recruits` list

```json
{ "id": "mymod:ratkin", "name": "Ratkin", "host_body": "human", "recruits": ["rat", "spider"] }
```

The game decides who a player may recruit by pressing Use on a creature from the player's body: a
human body recruits whatever is friendly, a spider body recruits spiders, scarabs and scorpions, and so
on. A race on another body could recruit only what its body's row names, plus a few creatures any body
can (gyrobots, automata except on a dryad, myconid or gremlin body, and at Legendary Leadership also
goblins, goatmen and insectoids) and any creature the map marks as recruitable. A
human-bodied race that made rats friendly with `allies` recruited them below Legendary Leadership but
lost them at that rank, where the body rows decide and friendship does not. `recruits` adds the
creatures you name, for any body, in both of those cases.

- A listed type is also the race's ally: the game counts it as a friend and not an enemy. Effects
  such as confusion can still make it attack. (Before, a rat-friendly human race had to list the type
  under `allies`, and that only worked below Legendary Leadership.)
- Your Leadership still caps how many followers you can lead. The game's own count and refusals apply.
- At load, an entry that is also under `enemies` or `neutral` is dropped from `recruits` with a warning
  in `sam_log.txt`, because those lists contradict it. So is a type that can never be made a follower
  (the list the Dominate spell and `sam_make_follower` refuse): shopkeeper, minotaur, lich, lichice, lichfire, devil, shadow, mimic, minimimic, the four tinkering bots
  (sentrybot, spellbot, gyrobot, dummybot), hologram and bat.
- The list never recruits a boss, a trader, a mimic, a tinkering construct or a summoned ally. (A human
  body's own rule of recruiting any friendly creature does not check this, so a creature that is a boss
  by its map or traits can still be recruited that way.) `sam_make_follower` asks the same
  can-this-ever-be-a-follower question through one shared function; its behaviour is unchanged.
- It does nothing for a player who ticked "disable monster abilities", as for the other allegiance lists.
  It does not look at shapeshifting, so a shapeshifted player can still recruit by it.
- The Race Editor has a "Can recruit" list that does not offer the types above.
- The Leadership panel's list of recruitable creatures is a fixed table in the game and does not show
  these. For any other rule, handle `player.on_callout` or an action press in a script and call
  `sam_make_follower`.

## Spells: school, level, requirements and spellbooks

```json
{
  "id": "mymod:summon_rat", "name": "Summon Rat", "payload": "force",
  "school": "mysticism",
  "difficulty": 40,
  "learn_requirements": { "stats": { "CHR": 10 }, "skills": { "PRO_LEADERSHIP": 20 } }
}
```

- **`school`** is `sorcery` (the default, and what every custom spell was before), `mysticism` or
  `thaumaturgy`. It sets the skill that `difficulty` is checked against (a spell with difficulty 0 checks
  none), the skill that casting the spell trains, the stats that add to its power, the robes that help it,
  and the school shown in its tooltip. An unknown word is reported and the spell stays Sorcery.
- **`difficulty`** has worked since 1.9.0: your skill in the spell's school plus INT must reach it to
  learn the spell from a book, and 0 means anyone can. Before 3.7.0 the school was always Sorcery. It also
  sets the spell's level, shown as tier I to VI: 0 to 19 is I, 20 to 39 is II, 40 to 59 is III, and so on,
  with 100 or more as VI. At skill 100 the game counts your skill plus INT as exactly 100, so keep
  difficulty at 100 or below. The Spell Editor now has a box for it that shows the level.
- **`learn_requirements`** asks for more: a least `STR`, `DEX`, `CON`, `INT`, `PER` or `CHR` (gear and
  effects count) and a least level in a skill by its `PRO_` name. A character who lacks one is told the
  first thing they lack and the book is not read. It is checked only when a spellbook or tome of the spell
  is read. A spell handed over by a class, a race or `sam_grant_spell` is never checked against it. A stat
  or skill name the game does not know is reported at load and ignored.
- **A spellbook for the spell** is an item with `"category": "SPELLBOOK"`, `"model_from_item"` set to a
  vanilla spellbook and `"attributes": { "spellbook_spell": "mymod:summon_rat" }`. Give it a `level` of 0
  or more as well if it should drop: that puts it in the random pool for chests, the general store and the
  packs of monsters whose map slot is a random spellbook. Chests and the general store can hand it out from
  about 5 or 6 floors before its level, monster packs from its level (read in the code, not run); the
  bookstore does not stock it. Teaching a spell by name worked before (`TraitTest` has
  one) but only by hand. The Spell Editor now has an "Also make a spellbook" tick box that writes the item,
  and the Item Editor takes a spell name for the four spell attribute keys (`spellbook_spell`,
  `tome_spell`, `foci_spell`, `magicstaff_spell`), where it took only numbers. If an item with the id
  `<namespace>:spellbook_of_<spell name>` is already in the mod, saving only points it at the spell and
  leaves the rest as it was; a book under another id is not found and a second one is made.
- Not built: touch spells and spells aimed at a tile (a custom spell is still a bolt or a three-way
  spread; `projectile_type` "none" only labels the tooltip as a self spell and the spell still fires),
  and a target position on `player.on_spell_cast`.

## Spell icons: a wrong path is reported, and the popup shows the picture

A custom spell's `icon` was resolved quietly: a path the game could not find fell back to the generic
orb with nothing in the log, a `mods/<folder>/` prefix never worked, the icon file was opened again on
every frame for each mod spell on the hotbar that named one, and the "new spell learned" popup had no
picture for a mod spell at all.

- The icon is now looked up once, when the mod loads, the way an item's icon is. `sam_log.txt` says
  `custom icon -> <path>` when it is found, and names the path it looked for when it is not.
- The path is written from the folder that holds `mod.json` (`spells/shadow_bolt.png`). A
  `mods/<folder>/` prefix still finds the file when the mod is installed by hand, with a warning; on a
  Steam Workshop install it is not found and the generic orb is used. A path that climbs out of the mod
  folder is refused with a warning. Restart the game after changing the file: it is read at start.
- The learn popup now shows the mod's icon, or the generic orb when there is none.
- The Spell Editor can upload the PNG and explains the path rule next to the box.

## A weapon's tooltip lists its on-hit effect

An item with an `on_hit_effect` now says so in its tooltip once it is identified: `25% chance on hit:
[Slowed] for 5s`, or `On hit: [Chill] for 3.5s` when it lands every time. On a sword, axe, mace or polearm
style tooltip the line sits under the stock effect line; on other tooltips it goes after the attributes
heading, or last. An item whose `on_hit_chance` is 0 gets no line. The line states what the item
declares, so it does not know about a target's immunities or a longer timer the target already has. Only a
melee hit applies the effect, so on a bow or a thrown weapon the line names an effect that never lands. If
you already wrote the effect into the item's `description`, it now shows twice: remove it from the
description. `sam_log.txt` prints the line built for each item whose effect is known. The duration uses the
same code the hit uses to choose its timer.

Also fixed in the text: the schema said an item `attributes` key other than the recognised ones is "never
looked at", and the load warning said it is not read by the engine and has no effect. Neither was true: the
game reads a few more keys on its own items (for example `UNSELLABLE`, `UNVOIDABLE`, `UNVANDALISABLE`,
`UNBURNABLE`, `no_stack`), which S.A.M has not tested on a mod item. Stat keys (`STR` to `CHR`) and the
`EFF_` names the game's own armour and ring tooltips have a line for are printed in the tooltip as stat
lines (read in the code, not seen in game) but give no bonus, so they look like a bonus that is not there. Use a script
(`sam_add_stat_modifier`) for that. The schema and the load warning also said a ranged weapon cannot be
custom, which contradicted the `ranged` trait; both now point to the trait.

## The Class Editor: worn and on the hotbar

The game already starts a character with an item in hand and on a hotbar key when one `starting_items`
entry has `"equip": true` and `"hotbar_slot": 0`; the Wizard starts with the quarterstaff that way. The
Class Editor treated the two as either/or, so a weapon put in the Weapon slot could not also be given a
hotbar key, and a custom weapon came back from a saved class in the red "slot already taken" tray. Now:

- An equipped item has a Hotbar box, and the hotbar row shows a worn item with a small hand mark (a worn
  stack shows its count instead). The X on it takes it off the bar and leaves it equipped.
- A custom item reopens in the cell for its slot.
- Adding a custom item from the "your custom items" row replaces whatever is worn in that slot, which goes
  to the backpack, and takes over its hotbar key.

## Also

- The Spell Editor owns `difficulty`, `school` and `learn_requirements`, and the Race Editor owns
  `recruits`: clearing one in the editor now clears it from the file instead of carrying the old value
  through.
- The Mod Builder Dashboard's "New in" panel describes 3.7.0.
- Documentation: the scripting reference has sections for the spell fields and the race list, the
  getting-started page explains `equip` with `hotbar_slot`, and the spellbook example shows a
  `"namespace:spell"` name instead of a number.

## Numbers

339 script functions, 87 events (unchanged). New JSON fields: spell `school` and `learn_requirements`,
race `recruits`. Engine files touched: `actmonster.cpp` (two checks inside the recruit code, both behind
a custom-race test), `item_usage_funcs.cpp` (the book and tome read), `GameUI.cpp` (the learn popup).
Each does nothing unless a custom race, a spell id of 2000 or more, or a mod is involved, and nothing
new crosses the network.

## What has and has not been tested

**Played by a person, in singleplayer** (`QuickWinsPlay`, a mod that hands out items and spells, spawns the
monsters and makes you immune to damage): the spell icons on the hotbar (a purple square for a custom icon,
the pink orb for the generic fallback), the learn popup showing a picture for a mod spell, the school and
level in the spell tooltip (Thaumaturgy I, Mysticism III, Sorcery I, each with its school icon), the
weapon tooltip line for the 25% blade on a sword-style tooltip, the gated book refusing ("You need 99
Charisma to learn Gated Bolt (you have 0).") and the open book teaching Open Bolt. Recruiting a rat and a
spider by pressing Use as a human-bodied Ratkin, and the cap of four followers, also worked; but that goes
through the game's own rule that a human body recruits what is friendly, so the new checks in
`actmonster.cpp` were not exercised by that run. A second run as a Skelkin (`quickwins:skelkin`, a skeleton body, which vanilla lets recruit ghouls and the few creatures every body can, so a rat can be recruited there only through the list) was played too: pressing Use on a rat and on a spider made them followers, the fifth was refused at the cap of four, and the rats did not attack while the goblin did. That run goes through the check in the ordinary Use branch; the check in the Legendary Leadership branch was not reached. It was reported by the player as all checking out; nothing more detailed (for example which follower was refused) was recorded.

**Automated, in the real game, nobody at the keyboard** (`QuickWinsTest`, with a Ratkin rogue): 10 checks
pass, and 23 checks of lines read from `sam_log.txt` pass (one of those only repeats the verdict of the
10). In game, a human-bodied race that lists rat and spider under recruits has them as friends and not
enemies, a goblin listed under recruits and enemies is still an enemy, and an unlisted troll is not a
friend. The log checks cover the three icon outcomes (found, missing, path out of the folder) and the
prefixed path, the school and requirements as the game read them (school, skill id, difficulty and
requirements), a misspelt school, stat and skill, what recruits drops (a shopkeeper, a minotaur and an
enemy), and the on-hit line the game logged for a 25% blade, an every-hit blade with a custom effect and a
chance-0 blade. The check that a blade with no effect is not mentioned cannot fail, because that blade
never gets a log line; it says nothing about its tooltip. On the shipped 3.6.0 build the same test fails 5
of the 10 in-game checks and 20 of the 23 log checks (the other three are "does not appear" checks, which
pass there trivially).

**The regression suite on the release build:** Hello 11, Speed 41, Settings 88, Loot 90, Audio 31, Vision 17, Neutral 11 and 3, HelmTwin 55, SpellEffect 15, Follower 60, Addons 27, and `QuickWinsTest` 10 as a Ratkin and 10 as a Skelkin (each with its 23 log checks), with no failures. An earlier run of the whole suite, on a build made before the last text-only changes, had `AddonsTest` fail one check once (26 passed, 1 failed); it passed 27 of 27 in the five reruns and in this final run, and which check failed was not recorded, so treat it as a flaky goblin-AI test, which is not proven. Three of the tests have checks that need a person at the keyboard and nobody did them: Vision's sneaking checks, the tin in both Neutral runs, and HelmTwin's checks where the player wears the items.

**Outside the game:** the Mod Builder's tests pass, 180 for the block builder (ten new checks, one old one
replaced: six check that the schema accepts the new fields and refuses wrong values, two that it still
accepts a worn item on the hotbar and a spellbook named by spell id, one that clearing a spell field in the
editor clears it, and one that a key no editor owns still survives a spell save) and 25 for the effect-name
check, and it builds. None of the editor pages has an automated test; the tests above cover the schemas and
the code that carries unknown keys. Scripted clicks drove the Spell Editor (school, difficulty and
requirements in the JSON, the spellbook tick box writing the item into the mod), the Item Editor (a spell
name for `tome_spell`), the Race Editor (the recruits list in the JSON) and the Class Editor (equip with a
hotbar key, the hand mark, the X taking it off the bar, a custom weapon replacing the one in hand and
reopening in its cell) in the Mod Builder dev server. The PNG upload was not driven.

**Not tested:**

- **Multiplayer**, for all of it. The recruit check runs on the host, which already knows a joiner's
  custom race. A joiner's book read is replayed on the host, so the book check can run there too, and what
  it does for a remote player was not worked out. Neither was tried with two machines.
- **Recruiting** at Legendary Leadership, with abilities turned off, while shapeshifted, a boss or summoned
  ally refused when you press Use, a type also under `neutral` being dropped, and the types dropped at load
  other than shopkeeper, minotaur and an enemy (the liches, devil, shadow, mimics, the bots, hologram, bat).
- **`learn_requirements`** when the requirement is met (the gated book was refused; the book that taught
  had none), a skill requirement, two requirements on one spell, that gear and effects count toward a stat,
  and a spell handed over by a class or a race. Mystic Bolt (difficulty 40) was only handed over by a
  script, so the learn gate reading its skill was not seen: the log shows the built spell carries skill 6,
  and that the gate, a cast and the robes use it was read in the code.
- **School** training and power: that Mysticism or Thaumaturgy trains the right skill, adds the right stats
  to a cast and gets the robe bonus.
- **The weapon tooltip:** the every-hit "On hit" form, the chance-0 blade (logged
  only), the plain blade (not even logged), the line on an unidentified item, on a weapon that is not sword-style, and the double line when
  the description repeats the effect.
- **Spell icons** in the spell list and in tooltips, a picture that is not a PNG or that the game cannot
  decode (not tried, expected to show blank), and a Steam Workshop install.
- **A spellbook written by the Spell Editor** was never loaded in a game, and no book was seen dropping
  from a chest, shop or monster's pack. The test books are hand written with level -1.

## Not in this release

The same list asked for more, and these are not done: bows that fire like a real shortbow or crossbow (a
weapon with the `ranged` trait and a bow model should already fire arrows from a quiver, going by the code,
but it was not tried in the game; it fires once every 0.8 seconds with no draw, and its tooltip is the
sword's), items that give Strength to Charisma, mana regeneration or magic resistance while worn (possible
today with a script; AC and ATK already work from the item file), a script box in the Item, Spell and
Effect editors, spells aimed at a tile or a touched creature, and attack animations for a custom weapon (a
staff that only swings should already work with `weapon_skill` set to polearm, axe or mace, going by the
code; not tried in the game).
