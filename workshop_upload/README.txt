===============================================================================
  S.A.M FRAMEWORK
  Support All Mods  -  a modding framework for Barony
  by spider12223
===============================================================================

WHAT IS THIS?
-------------
S.A.M ("Support All Mods") lets Barony be modded with plain JSON files. No C++,
no compiler, no engine knowledge required. It reads .json files from a mod
folder at launch and turns them into real, working content:

  * CLASSES     attributes, skills, starting gear, spells, portraits
  * ITEMS       weapons, armour, tools, your own icons and 3D models
  * MONSTERS    stats, gear, behaviour, world spawns, followers, boss bars
  * RACES       play as a custom creature, with your own body and limbs
  * SPELLS      your own spells, with your own projectiles and effects
  * EFFECTS     your own status effects, buffs and debuffs
  * SOUNDS      add your own, or replace any sound in the game by name
  * MUSIC       give a floor, a map or a boss its own track
  * RECIPES     add to what the tinkering kit builds, or ship your own bench
  * PATCHES     edit vanilla data files additively, so mods stack instead of
                overwriting each other

If you want a mod to THINK as well as exist, it can carry a script in Lua,
JavaScript or TypeScript: hundreds of functions and dozens of events, with a
full reference (see LINKS below). No script is required for any of the above.

MULTIPLAYER
-----------
Every function has a defined behaviour in co-op, and the game enforces it. You
write one script, name players by number, and S.A.M carries each call to the
machine it has to run on: a line of text to that player's screen, a spell into
their own game, a rule change to everybody. A player who joins part way through
is caught up on what they missed. A player running plain unmodified Barony can
still join the game and play.

S.A.M IS A DEPENDENCY, NOT A PLAYABLE MOD.
By itself it adds nothing you can play. It is the engine that OTHER mods are
built on. You install S.A.M once, then any mod made with S.A.M just works.

-------------------------------------------------------------------------------
IMPORTANT: HOW S.A.M IS DELIVERED
-------------------------------------------------------------------------------
S.A.M works by extending Barony's own executable so it can read the JSON. That
means S.A.M ships as a patched "barony.exe" (included in this folder), NOT as a
normal Workshop data folder. See INSTALL.md for the exact, safe install steps.

  barony.exe in this folder  =  Barony v5.0.2 + the S.A.M loader compiled in.
  Everything else is vanilla. No game files are altered on disk at runtime.

-------------------------------------------------------------------------------
QUICK INSTALL (see INSTALL.md for the full guide)
-------------------------------------------------------------------------------
  1. Own Barony on Steam and run it once (so the base files exist).
  2. Back up your original barony.exe.
  3. Drop the S.A.M barony.exe into your Barony install folder.
  4. Launch. A "sam_log.txt" appears next to the game, which means S.A.M is live.
  5. Subscribe to or install any mod made with S.A.M and enable it in the
     in-game Mods menu.

In co-op, every player needs S.A.M, and every player needs the mod. A player
with neither can still join and play the ordinary game.

-------------------------------------------------------------------------------
MAKE YOUR OWN MOD, NO CODING
-------------------------------------------------------------------------------
Use the free browser tool (nothing to install):

    GUI Mod Builder:  https://spider12223.github.io/SAM-Framework/

Build a class, item, monster, race, spell, effect, sound or recipe with sliders
and dropdowns, export a ready-to-go .zip, drop it in Barony/mods/, and play. The
tool validates everything against the same schemas the game uses, so if it
exports, it loads.

-------------------------------------------------------------------------------
LINKS
-------------------------------------------------------------------------------
  GUI Mod Builder ....... https://spider12223.github.io/SAM-Framework/
  Source & docs ......... https://github.com/spider12223/SAM-Framework
  Schema reference ...... https://spider12223.github.io/SAM-Framework/docs/schema-reference.html

Two files in this folder are for mod authors and are safe to ignore otherwise:

  function-reference.md   every script function and event, with arguments,
                          return values and what each one does in co-op
  sam.d.ts                the same, as TypeScript definitions, so an editor
                          autocompletes and type-checks a .ts mod script

-------------------------------------------------------------------------------
VERIFY IT'S WORKING
-------------------------------------------------------------------------------
After launching with the S.A.M exe, look for  sam_log.txt  in your Barony folder.
It should contain a line like:

    [SAM INFO ][CORE    ] S.A.M initializing... (Barony v5.0.2)

If you see that, S.A.M is running. If not, see the Troubleshooting section of
INSTALL.md.

-------------------------------------------------------------------------------
LICENSE & ATTRIBUTION
-------------------------------------------------------------------------------
Built on Barony (BSD 2-Clause) (c) 2013-2020 Turning Wheel LLC.

Barony is licensed under the BSD 2-Clause License; its copyright notice is
reproduced here as that license requires for binary distribution. Full
third-party license texts (Barony, Dear ImGui, SDL, RapidJSON) are in
NOTICE.txt, distributed with the framework source at
https://github.com/spider12223/SAM-Framework .

S.A.M Framework code is original work by spider12223 (written with AI
assistance), released under the MIT License. S.A.M is an unofficial community
framework and is not affiliated with or endorsed by Turning Wheel LLC.
===============================================================================
