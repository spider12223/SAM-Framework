# S.A.M Framework v0.9.1 — Carousel Fix

A small but important fix for anyone using mods that add a lot of custom classes.

## Fixed

- **Class-select carousel now scrolls properly with many custom classes.** The
  character-creation class grid sized its scroll area from the vanilla class count
  only, so once enough custom classes were loaded to spill onto a new row, the extra
  classes were clipped out of the scroll region and couldn't be selected. The scroll
  height now accounts for custom classes, so **every** custom class is reachable no
  matter how many a mod (or several mods together) add.

## Unchanged from v0.9.0

- Steam online multiplayer (Steamworks-enabled build) and everything else from v0.8.0
  (custom classes/items/monsters/spells, Lua/JS/TS scripting, content patching, hooks).

## How to get it

- **Installer:** grab the v0.9.1 installer from the release below — it backs up your
  current `barony.exe` and installs the S.A.M build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v0.9.1 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next to
the exe (the installer and Workshop item handle this).*
