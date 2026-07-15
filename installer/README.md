# S.A.M Framework Installer (source)

A tkinter + PyInstaller single-exe Windows installer. It auto-detects the Barony
install, backs up the player's original `barony.exe` → `barony_vanilla.exe`,
installs the S.A.M-patched `barony.exe`, and deploys **`typescript.js`** (the
TypeScript compiler used for `.ts` mod scripts) next to it.

## Build

1. Populate `payload/` with the two runtime files (release artifacts, **not**
   committed — see `.gitignore`):
   - `payload/sam_barony.exe` — the S.A.M-patched Barony Release build
   - `payload/typescript.js`   — from `../framework/typescript/typescript.js`
2. `pip install pyinstaller`
3. `pyinstaller SAM_Framework_Installer_v0.9.6.spec`
4. Output: `dist/SAM_Framework_Installer_v0.9.6.exe`

`python installer.py --selftest` prints Steam/Barony auto-detection results
without building the GUI.
