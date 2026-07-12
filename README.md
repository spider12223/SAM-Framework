<div align="center">

# S.A.M Framework
### Support All Mods — a JSON modding framework for [Barony](https://store.steampowered.com/app/371970/Barony/)

**[🛠 Open the Mod Builder](https://spider12223.github.io/SAM-Framework/)** · **[📜 Schema Reference](https://spider12223.github.io/SAM-Framework/docs/schema-reference.html)** · **[⬇ Releases](https://github.com/spider12223/SAM-Framework/releases)**

</div>

---

## What is S.A.M?

**S.A.M (Support All Mods)** is a patched Barony executable that turns Barony into a data-driven modding platform. Instead of writing and compiling C++, modders describe custom **classes**, **items**, and **weapons** in plain JSON files. S.A.M reads those files at launch — from any mod folder Barony already mounts — and registers the content into the running game: custom classes appear on the character-select screen with their own portraits, stats, skills, starting gear and spells; custom items slot into the game's item table. A built-in multiplayer sync check warns players when a lobby's mods don't match. No C++ knowledge required.

> S.A.M is a **framework**, not a standalone game. You need to own **[Barony on Steam](https://store.steampowered.com/app/371970/Barony/)** to use it.

---

## Install

S.A.M is a **patched Barony executable**, so you need to own **Barony on Steam**.

- **Via Steam Workshop** *(intended channel, once published)* — subscribe to S.A.M on the Workshop; it delivers the patched build and keeps it in step with the mods that depend on it.
- **Build it yourself** *(available now)* — follow [Build from source](#build-from-source-contributors) to produce a patched `barony.exe` against your own copy of Barony, back up your existing one, and drop the S.A.M build into:
  ```
  …/steamapps/common/Barony/barony.exe
  ```

Launch Barony as usual — S.A.M initializes automatically and writes `sam_log.txt` to the Barony folder. To go back to vanilla, verify the game files through Steam (or restore your backup).

> **This repository ships S.A.M's source, schemas, GUI and docs — not a prebuilt game binary.** Barony and any patched build of it belong to [Turning Wheel LLC](https://github.com/TurningWheel/Barony) and aren't ours to redistribute; S.A.M is built against a copy of Barony you already own.

---

## Use the Mod Builder (GUI)

The easiest way to make a mod is the web-based builder — no install, runs entirely in your browser:

### → **https://spider12223.github.io/SAM-Framework/**

- **Class Editor** — attributes, all 16 skills, starting items (searchable over every vanilla item), spells, stat growth, gold, and a custom portrait.
- **Item Editor** — category, equip slot, weight, value, attributes.
- **Mod Builder** — set your namespace + manifest, then export a ready-to-use `.zip`.
- **Validator** — check any JSON against the schemas with precise error paths.

Every dropdown in the tool is generated from the [schemas](schemas/) at runtime, so it always matches what the framework accepts.

---

## Make & publish a mod

A S.A.M mod is just a folder of JSON (plus optional portrait/model images):

```
my_mod/
  mod.json                 ← manifest: namespace, name, version, what it ships
  classes/
    my_class.json          ← conforms to class.schema.json
  items/
    my_item.json           ← conforms to item.schema.json
  portraits/
    my_class.png           ← optional 54×54 class-select icon
```

1. Build it in the [Mod Builder](https://spider12223.github.io/SAM-Framework/) (or by hand against the [schemas](schemas/)) and export the `.zip`.
2. **Reserve your namespace** by submitting a PR to [`registry/namespaces.json`](registry/namespaces.json) so two mods never collide on the same prefix.
3. **Test locally**: unzip into `…/steamapps/common/Barony/mods/<your_mod>/` and enable it from Barony's Mods menu.
4. **Publish to the Steam Workshop** as a Barony mod, and **list S.A.M as a required dependency** in your Workshop description so subscribers know they need the S.A.M build. Since S.A.M mods are JSON-only, they carry no compiled code — S.A.M does all the work at runtime.

Multiplayer: S.A.M sends a mod fingerprint from host to client on join and **warns** (never hard-blocks) when the two don't match, so mismatches are obvious before a run.

New to this? Follow **[Getting Started →](docs/getting-started.md)** to build a class + item in ~5 minutes.

### Autocomplete as you type

S.A.M publishes its schemas so **VS Code validates and autocompletes your JSON** — every field's valid options in a dropdown, red squiggles on typos before you ever launch. No extension needed. Either add a `$schema` line to each file:

```json
{ "$schema": "https://spider12223.github.io/SAM-Framework/schemas/class.schema.json", "id": "mymod:myclass", ... }
```

…or copy this repo's [`.vscode/settings.json`](.vscode/settings.json) into your mod folder to map every file automatically. The [Mod Builder](https://spider12223.github.io/SAM-Framework/) stamps `$schema` into everything it exports, so exported mods get this for free.

---

## Schema reference

All content is validated against JSON Schemas (draft-07), which are the single source of truth for the framework, the GUI, and editor autocomplete:

| Schema | Describes |
|---|---|
| [`mod.schema.json`](schemas/mod.schema.json) | The `mod.json` manifest |
| [`class.schema.json`](schemas/class.schema.json) | A custom class |
| [`item.schema.json`](schemas/item.schema.json) | A custom item / weapon |
| [`patch.schema.json`](schemas/patch.schema.json) | A layered patch to an existing data file |

A human-readable, always-in-sync field reference is generated from these: **[Schema Reference →](https://spider12223.github.io/SAM-Framework/docs/schema-reference.html)** (or open [`docs/schema-reference.html`](docs/schema-reference.html) locally).

---

## Build from source (contributors)

S.A.M is a set of C++ files compiled **into** a patched Barony, plus a standalone React GUI.

**The GUI** (pure frontend, no backend):
```bash
cd gui
npm install
npm run dev      # http://localhost:5173
npm run build    # static site in dist/
```

**The framework** (`framework/*.cpp`) is compiled into Barony's build. In short: clone [Barony's source](https://github.com/TurningWheel/Barony), add the S.A.M `framework/` files to the game target, and apply the small set of interception hooks (loader, class/item registration, character-select carousel, multiplayer sync). The framework code is deliberately decoupled — the logger, workshop scanner, loader, and the parsing halves of the class/item registries include zero Barony headers. See [`framework/`](framework/) and the header comments for the hook points.

> Barony's own source is **not** included in this repository and is not ours to redistribute — it belongs to [Turning Wheel LLC](https://github.com/TurningWheel/Barony). S.A.M only ships its own framework code, schemas, GUI, and docs.

---

## Repository layout

```
framework/     C++ compiled into the patched Barony (logger, loader, classes, items, sync)
gui/           React + Vite mod-builder web app (deployed to GitHub Pages)
schemas/       JSON Schema definitions — the source of truth
docs/          Generated schema reference
registry/      Public namespace registry (submit a PR to reserve yours)
model-library/ Bundled generic models
workshop/      Steam Workshop packaging files
```

## License

S.A.M's own code, schemas, GUI, and docs are released under the [MIT License](LICENSE). Barony is © Turning Wheel LLC; you must own it to use S.A.M.
