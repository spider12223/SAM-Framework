# Getting Started — your first S.A.M mod in 5 minutes

You'll make a tiny mod that adds one class and one item — all JSON, no C++.

## 1. Folder layout

```
my_first_mod/
  mod.json
  classes/
    warden.json
  items/
    oakstaff.json
```

## 2. Turn on autocomplete (recommended)

S.A.M publishes JSON Schemas so your editor validates and autocompletes as you type — you'll see the valid options for every field and get red squiggles on typos *before* you ever launch the game.

**Easiest (per file):** put a `$schema` line at the top of each JSON (see the examples below). Works in VS Code with no setup — its built-in JSON support fetches the schema and lights up autocomplete. (If you prefer YAML-style hints, the `redhat.vscode-yaml` extension also honors it, but it isn't required for JSON.)

**Or (whole folder):** drop a `.vscode/settings.json` into your mod folder — copy [the one in this repo](../.vscode/settings.json). It maps `mod.json`, `classes/*.json`, `items/*.json`, and `patches/*.json` to their schemas automatically, so you don't need a `$schema` line in every file.

> Tip: the [**Mod Builder** GUI](https://spider12223.github.io/SAM-Framework/) already stamps `$schema` into every file it exports — so if you build your mod there, autocomplete just works when you open the files.

## 3. mod.json

```json
{
  "$schema": "https://spider12223.github.io/SAM-Framework/schemas/mod.schema.json",
  "namespace": "myfirstmod",
  "name": "My First Mod",
  "author": "you",
  "version": "1.0.0",
  "framework_min_version": "0.1.0",
  "classes": ["classes/warden.json"],
  "items": ["items/oakstaff.json"]
}
```

## 4. classes/warden.json

```json
{
  "$schema": "https://spider12223.github.io/SAM-Framework/schemas/class.schema.json",
  "id": "myfirstmod:warden",
  "name": "Warden",
  "description": "A sturdy guardian of the forest.",
  "stats": { "STR": 2, "CON": 2, "INT": -2 },
  "skills": { "PRO_POLEARM": 40, "PRO_SHIELD": 30 },
  "starting_items": [
    { "type": "IRON_SPEAR", "equip": true, "hotbar_slot": 0 },
    { "type": "WOODEN_SHIELD", "equip": true }
  ],
  "gold": 75
}
```

## 5. items/oakstaff.json

```json
{
  "$schema": "https://spider12223.github.io/SAM-Framework/schemas/item.schema.json",
  "id": "myfirstmod:oakstaff",
  "name_identified": "Oak Staff",
  "name_unidentified": "wooden staff",
  "category": "MAGICSTAFF",
  "slot": "EQUIPPABLE_IN_SLOT_WEAPON",
  "weight": 3,
  "gold_value": 40,
  "level": -1
}
```

## 6. Install & play

1. Zip/copy the `my_first_mod/` folder into `…/steamapps/common/Barony/mods/` (with the S.A.M build installed).
2. Launch Barony → **Mods** → enable **My First Mod** → **Play**.
3. Pick **Warden** on the class-select screen. Check `sam_log.txt` in the Barony folder if anything doesn't show up — S.A.M logs exactly which file and field to fix, with a "did you mean?" for typos.

## Next steps

- **Full field reference:** [schema-reference.html](https://spider12223.github.io/SAM-Framework/docs/schema-reference.html)
- **Rebalance vanilla items** (that stack with other mods): add a `patches/` file — see `patch.schema.json`
- **Reserve your namespace:** open a PR to [`registry/namespaces.json`](../registry/namespaces.json)
