# S.A.M Framework v0.1.0

First public build of **S.A.M (Support All Mods)** — a patched Barony that loads custom classes and items from JSON mods, no C++ required.

## What's in v0.1.0

- **Runtime mod loader** — reads `mod.json` manifests from every mod folder Barony mounts and registers their content at launch. Rebuilds cleanly on every "Play" (no double-registration), unloads back to vanilla.
- **Custom classes** — attributes, all 16 skill proficiencies, starting items, starting spells, level-up stat growth and starting gold, applied when a player picks the class. Registered at runtime ids ≥ 1000, fully selectable on the character-select carousel with correct name and description.
- **Custom class portraits** — an optional `portrait` PNG shows as the class's carousel icon (54×54), with a safe placeholder fallback if the image is missing.
- **Custom items / weapons** — registered into the game's item table at reserved slots (≥ 5000) via a dedicated slot count, leaving every vanilla loop and random-loot table untouched. Placeholder model/icon until custom art lands.
- **Multiplayer mod sync** — the host sends a mod fingerprint (namespace + version of every loaded mod) to each joining client, which compares against its own and **warns** on a mismatch — naming the exact missing/wrong-version mods — without ever hard-blocking or crashing the connection.
- **Console + file logging** — every step is traced to `sam_log.txt` in the Barony folder.
- **Mod Builder GUI** — a browser-based tool (class editor, item editor, mod builder, validator) that exports a drop-in mod `.zip`. Live at **https://spider12223.github.io/SAM-Framework/**.
- **Three JSON Schemas** (draft-07) as the single source of truth for the framework and the GUI, plus a generated schema reference.

## Verified

- Custom class (Assassin) selectable on character creation with its custom portrait; stats, gear and spells apply correctly; a custom item registers and the class starts a real dungeon run with no crashes.
- 2-player LAN game: fingerprint exchanged, match confirmed, full multiplayer run.

## Install

You need **Barony on Steam**. Back up your `barony.exe`, then drop this build's `barony.exe` into `…/steamapps/common/Barony/`. See the [README](https://github.com/spider12223/SAM-Framework#install) for details.

## Known limitations

- Custom **item models/icons** and **class portraits in the 3D preview** are placeholders — image loading for the carousel icon works; full custom .vox models are a later milestone.
- Plugin (`.dll`) loading is declared in the manifest but not yet implemented.

---

*Barony is © Turning Wheel LLC. S.A.M ships only its own framework code, schemas, GUI and docs; you must own Barony to use it.*
