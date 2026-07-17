# S.A.M Framework v1.2.7 — hotbar pins stick, /spawnitem finds custom items

## Fixed

- **A class's starting hotbar layout now actually applies in-game.** The Mod Builder let
  you pin starting items to hotbar slots, and the exported JSON was correct, but the pins
  never showed up: the engine writes them, then Barony's own class-hotbar loader runs a
  few frames later and zeroes the whole bar. The pins are now re-applied after that pass,
  so a class starts with the hotbar you set.

- **/spawnitem and /spawnitem2 can spawn custom S.A.M items.** The vanilla command only
  searches Barony's built-in item table, so modded gear never matched. It now falls back
  to the registered custom items: type an exact `namespace:item` id or the item's own
  name (exact then partial). /spawnitem2 keeps the beatitude and status you pass.

## Also in the Mod Builder (already live on the site)

- The class builder **autosaves your in-progress work** to your browser, so a refresh or
  closed tab no longer loses an unsaved class, script, portrait, or loadout.
- The visual block builder now **puts its Lua into the script automatically** as you
  build — you no longer have to click "Use this script" before saving.
- **Item quality reads in the item's own words**: a potion shows a Look (cloudy / plain /
  bubbly), a gem a Quality (flawed / flawless), food a Freshness — not weapon "condition".
- Exported mod **zips now contain a `<name>/` folder** so they unzip cleanly into
  Barony/mods/ instead of spilling loose files.
- Iron pauldrons (and shawls) are classed as **body armour**, not a cloak.

## How to get it

- **Installer:** grab the v1.2.7 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.7 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
