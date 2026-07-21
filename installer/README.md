# S.A.M Framework Installer (source)

A tkinter + PyInstaller single-exe Windows installer. It auto-detects the Barony
install, backs up the player's original `barony.exe` → `barony_vanilla.exe`,
installs the S.A.M-patched `barony.exe`, and deploys **`typescript.js`** (the
TypeScript compiler used for `.ts` mod scripts) next to it.

## Where the payload comes from

Since **v1.1.0** the installer is **lean**: it ships no payload and downloads the
newest release assets at install time from the permanent GitHub "latest" redirect:

```
https://github.com/spider12223/SAM-Framework/releases/latest/download/barony.exe
https://github.com/spider12223/SAM-Framework/releases/latest/download/typescript.js
```

That URL needs no API call, so there's no token and no rate limit. The payoff:
**publishing a release updates every installer already in the wild** — cut the
release and you're done. No rebuilding or re-uploading an installer per version.

Two constraints follow from this, and both matter:

- **Don't rename those release assets.** Old installers ask for `barony.exe` and
  `typescript.js` by name, forever. Renaming them breaks every copy already out there.
- **It needs internet at install time.** Downloads are verified before anything is
  touched (size + `MZ` PE header), and the game folder is only written after a
  complete, valid download — a dropped connection can't leave a broken `barony.exe`.
  `typescript.js` is treated as optional: if only it fails, the install still succeeds
  (only `.ts` mod scripts need it).

`resolve_payload()` still honours a bundled payload if one is present, so an
offline/bundled build is a one-line `.spec` change away (see below).

## Build

0. `python ../tools/check_versions.py` — fails if any file still carries the old
   version. The Workshop description silently sat five releases behind once; this is the
   guard so it can't happen quietly again.
1. `pip install pyinstaller`
2. `pyinstaller SAM_Framework_Installer.spec`
3. Output: `dist/SAM_Framework_Installer.exe` (~11 MB — that's PyInstaller's
   Python + tkinter runtime; the payload is no longer inside)

### Building an offline/bundled installer instead

Populate `payload/` with the two runtime files (release artifacts, **not** committed
— see `.gitignore`) and point the spec's `datas` at them:

- `payload/sam_barony.exe` — the S.A.M-patched Barony Release build
- `payload/typescript.js`  — from `../framework/typescript/typescript.js`

```python
datas=[('payload/sam_barony.exe', '.'), ('payload/typescript.js', '.')],
```

`resolve_payload()` prefers a bundled payload when it finds one, so no code changes
are needed — but you're back to rebuilding the installer for every release.

## Self-tests

```
python installer.py --selftest           # Steam/Barony detection + which payload source is active
python installer.py --selftest-download  # exercise the real download + verification, touching no game files
```
