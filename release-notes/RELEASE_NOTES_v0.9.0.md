# S.A.M Framework v0.9.0 — Steam Multiplayer

**The big one: online multiplayer over Steam now works.**

Every prior S.A.M build was compiled with Steamworks disabled, so S.A.M users were forced onto Radmin / Hamachi / other LAN tools to play together. v0.9.0 ships a Steamworks‑enabled `barony.exe`, so S.A.M multiplayer now uses Steam's real networking — **friend invites, lobby join, and NAT traversal**, exactly like vanilla Barony's Online Co‑op.

## What's new

- **Steam online multiplayer.** Host and join games through Steam (invite a friend via the overlay, or join a lobby). No more LAN tunneling.
- Everything from v0.8.0 is unchanged (custom classes, items, monsters, spells, Lua/JS/TS scripting, content patching, ~47 hooks / ~44 host APIs).

## How to get it

- **Installer:** grab the v0.9.0 installer from the release below — it backs up your current `barony.exe` and drops in the S.A.M build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v0.9.0 exe.

## Requirements / notes

- **Both players need the S.A.M v0.9.0 build**, and each must own Barony on Steam. Steam authenticates your own copy — nothing to configure.
- **Launch through Steam** (Play button / accept a `steam://joinlobby/...` invite) so the Steam overlay and friend‑invite panel are available.
- The Steam library (`steam_api64.dll`) is the one already in your Barony install — S.A.M does **not** ship or replace it.
- Use the game's **online host/join** flow (Steam lobby), not Direct‑IP, for internet play.

## Known issues

- **Custom equippable items (e.g. a modded shield) can render/behave incorrectly** — a custom armor/shield item currently inherits a generic item identity in some cases. This is a separate item‑system bug from the multiplayer work and is being investigated for a follow‑up release; it does not affect vanilla content or multiplayer.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next to the exe (the installer and Workshop item handle this).*
