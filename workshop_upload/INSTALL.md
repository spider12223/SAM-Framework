# Installing the S.A.M Framework (v0.4.0)

**S.A.M — "Support All Mods" — is a modding framework for Barony.** It lets mods
add classes, items, and monsters from plain JSON. It is a **dependency**: on its
own it does nothing you can play, but any mod built with S.A.M needs it installed.

Because S.A.M works by extending Barony's own program, it is delivered as a
**patched `barony.exe`** (included in this package), not as a normal mod folder.
A second file, **`typescript.js`**, is also included — it is the TypeScript
compiler S.A.M uses for `.ts` mod scripts. (Lua and JavaScript mod scripts work
without it; only TypeScript needs it.)

---

## 1. Prerequisites

- **You own Barony on Steam** and have launched it at least once (so the game's
  files are fully installed).
- You know how to reach your Barony install folder:
  **Steam → right-click Barony → Manage → Browse local files.**
- Windows (this build is a Windows `barony.exe`). Barony version **v5.0.2**.

---

## 2. Get S.A.M

You have two ways to obtain the framework:

### Option A — From this package (recommended, offline)
This folder already contains the ready-to-use `barony.exe`. Skip to step 3.

### Option B — From Steam Workshop
> **Workshop link:** _TBD — paste your published item URL here after uploading._

1. Open the S.A.M Framework Workshop page and click **Subscribe**.
2. Follow the Workshop page's install note (it links to the patched `barony.exe`,
   because Steam Workshop can deliver mod *content* but not a replacement game
   program — see the note at the bottom of this file).

Either way, you end up with the S.A.M **`barony.exe`**.

---

## 3. Back up your original executable

1. Browse local files (**Manage → Browse local files**).
2. Find **`barony.exe`**.
3. Copy it and rename the copy to **`barony_vanilla.exe`** (keep it right there).

> This is your one-click path back to the unmodified game. Do not skip it.

---

## 4. Install the S.A.M files

1. Close Barony completely.
2. Copy the S.A.M **`barony.exe`** from this package into your Barony install
   folder, **overwriting** the original.
3. Copy **`typescript.js`** from this package into the **same** folder (right next
   to `barony.exe`). This enables TypeScript (`.ts`) mod scripts; Lua and
   JavaScript scripts work without it.
4. That's it — nothing else changes.

> Prefer one click? The **installer** (`SAM_Framework_Installer.exe`) does
> all of this automatically — it backs up your original `barony.exe`, installs the
> S.A.M one, and deploys `typescript.js`.

---

## 5. Launch and verify

1. Start Barony (from Steam as usual).
2. Close it after the main menu loads.
3. Browse local files again and look for a new file: **`sam_log.txt`**.
4. Open it. Near the top you should see:

   ```
   [SAM INFO ][CORE    ] S.A.M initializing... (Barony v5.0.2)
   [SAM INFO ][CORE    ] Scanning N mounted mod path(s) for mod.json...
   ```

If `sam_log.txt` exists and shows those lines, **S.A.M is installed and running.**

---

## 6. Install and enable a mod that uses S.A.M

S.A.M is only useful with mods built on it. To add one:

1. Get a S.A.M mod folder (from the Workshop, from a friend, or one you built at
   the **[S.A.M Mod Builder](https://spider12223.github.io/SAM-Framework/)**).
2. Place the mod folder in **`Barony/mods/<mod_name>/`** so that
   `Barony/mods/<mod_name>/mod.json` exists.
3. Launch Barony → **Mods** menu → enable the mod → **Play**.
4. Check `sam_log.txt` — you should see the mod get scanned and its content
   registered, e.g.:

   ```
   [SAM INFO ][WORKSHOP] Found mod: My Cool Pack [mycoolpack] v1.0.0 (1 classes, 2 items, 1 monsters, 0 plugins)
   [SAM INFO ][CORE    ] S.A.M load complete. 1 mod(s) loaded, ...
   ```

---

## 7. Build your own mod (no coding)

Open the free browser tool — nothing to install:

**https://spider12223.github.io/SAM-Framework/**

Design a class / item / monster with sliders and dropdowns, **Export** the `.zip`,
unzip it into `Barony/mods/`, and play. It even has a one-click **"Test in
Barony"** that writes the mod straight into your `mods/` folder (Chrome/Edge).

---

## Troubleshooting

**`sam_log.txt` never appears.**
- You probably launched the original exe. Confirm the S.A.M `barony.exe` actually
  overwrote the one in the Steam folder (check the file's Date Modified).
- Make sure Steam didn't re-verify and restore the vanilla exe: if you ran
  *Verify integrity of game files*, Steam replaces `barony.exe`. Re-copy the
  S.A.M exe afterward.

**The game won't start / crashes on launch.**
- Restore `barony_vanilla.exe` (rename it back to `barony.exe`) to confirm the
  base game is fine, then re-apply the S.A.M exe.
- Make sure your Barony is version **v5.0.2**. A different game version can be
  incompatible with this build.

**My mod doesn't show up / doesn't load.**
- The folder must be `Barony/mods/<name>/mod.json` (not zipped, not nested one
  level too deep).
- Enable it in the in-game **Mods** menu, then start a new game.
- Open `sam_log.txt` — S.A.M logs a clear reason for anything it skips
  (bad JSON, unknown field, missing dependency) with a "did you mean?" hint.

**Steam updated Barony and now S.A.M is gone.**
- A game update replaces `barony.exe`. Re-copy the S.A.M exe after any update
  (and grab a fresh S.A.M build if the base game version changed).

**Multiplayer says mods don't match.**
- Every player in the lobby needs the S.A.M exe **and** the same enabled mods.
  S.A.M compares a fingerprint and warns if they differ.

---

## Note on Steam Workshop delivery

Steam Workshop items are **content folders that the game mounts** — they cannot
replace the game's executable. Since S.A.M is compiled into `barony.exe`, the
patched executable must be delivered as a file (in this package, or via a link
on the Workshop page / GitHub release), while the Workshop item itself serves as
the discoverable "required dependency" other mods point to. See
`workshop_checklist.txt` for how to set that up.

---

- **Mod Builder:** https://spider12223.github.io/SAM-Framework/
- **Source & docs:** https://github.com/spider12223/SAM-Framework
- **Schema reference:** https://spider12223.github.io/SAM-Framework/docs/schema-reference.html

*Barony © Turning Wheel LLC. S.A.M is an unofficial community framework, not
affiliated with or endorsed by Turning Wheel LLC.*
