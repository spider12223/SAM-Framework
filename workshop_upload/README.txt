===============================================================================
  S.A.M FRAMEWORK  v0.2.0
  Support All Mods  —  a JSON modding framework for Barony
  by spider12223
===============================================================================

WHAT IS THIS?
-------------
S.A.M ("Support All Mods") lets Barony be modded with plain JSON files — no C++,
no compiler, no engine knowledge required. It reads simple .json files from a
mod folder at launch and turns them into real, working:

  * Custom CLASSES   (attributes, skills, starting gear, spells, portraits)
  * Custom ITEMS     (weapons, armor, effects — registered into the item table)
  * Custom MONSTERS  (stat/gear/behaviour variants of existing creatures, with
                      world spawns and followers)
  * Layered PATCHES  (additively edit vanilla data files so mods stack instead
                      of overwriting each other)

S.A.M IS A DEPENDENCY, NOT A PLAYABLE MOD.
By itself it adds nothing you can play. It is the engine that OTHER mods are
built on. You install S.A.M once; then any mod made with S.A.M just works.

-------------------------------------------------------------------------------
IMPORTANT — HOW S.A.M IS DELIVERED
-------------------------------------------------------------------------------
S.A.M works by extending Barony's own executable so it can read the JSON. That
means S.A.M ships as a patched "barony.exe" (included in this folder), NOT as a
normal Workshop data folder. See INSTALL.md for the exact, safe install steps.

  barony.exe in this folder  =  Barony v5.0.2 + the S.A.M loader compiled in.
  Everything else is vanilla — no game files are altered on disk at runtime.

-------------------------------------------------------------------------------
QUICK INSTALL (see INSTALL.md for the full guide)
-------------------------------------------------------------------------------
  1. Own Barony on Steam and run it once (so the base files exist).
  2. Back up your original barony.exe.
  3. Drop the S.A.M barony.exe into your Barony install folder.
  4. Launch. A "sam_log.txt" appears next to the game — that means S.A.M is live.
  5. Subscribe to / install any mod made with S.A.M and enable it in the
     in-game Mods menu.

-------------------------------------------------------------------------------
MAKE YOUR OWN MOD — NO CODING
-------------------------------------------------------------------------------
Use the free browser tool (nothing to install):

    GUI Mod Builder:  https://spider12223.github.io/SAM-Framework/

Build a class / item / monster with sliders and dropdowns, export a ready-to-go
.zip, drop it in Barony/mods/, and play. The tool validates everything against
the same schemas the game uses, so if it exports, it loads.

-------------------------------------------------------------------------------
LINKS
-------------------------------------------------------------------------------
  GUI Mod Builder ....... https://spider12223.github.io/SAM-Framework/
  Source & docs ......... https://github.com/spider12223/SAM-Framework
  Schema reference ...... https://spider12223.github.io/SAM-Framework/docs/schema-reference.html

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
