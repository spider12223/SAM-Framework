# S.A.M Framework v1.2.6 — non-human custom-class saves load again

## Fixed

- **A save made with a custom class and a non-human race would not load**, showing
  "Could not verify required DLC installed to load this save file" even when you own
  the DLC. This is the real fix for the problem v1.2.5 took a first swing at.

  What was actually happening: every custom S.A.M class is stored in the save as
  `char_class: 1000`. The game's save-load check has a list of the vanilla classes and
  which race each is allowed to pair with; a custom class isn't on that list, so for a
  non-human character it fell through to "invalid character" and refused to load. (A
  tester confirmed it by hand-editing the save's `char_class` from `1000` to a vanilla
  id, which loaded fine.) The check now recognises custom-class ids and treats them
  like a base class, after the character's race DLC has already been verified. It is
  not a bypass: an owned non-human race is still required, exactly as in vanilla.

  If you hand-edited a save to get around this, you don't need to keep doing that after
  updating; original custom-class saves load as-is.

## Also in this line (from v1.2.5)

- The DLC ownership check now accepts owned-but-not-"installed" DLC (it reads Steam
  subscription as well as install state), so an unticked DLC box in Steam's properties
  no longer hides a pack you own.
- 8 new world hooks scripts can react to: picking an item up off the ground, opening
  doors, projectile hits, fountains, sinks, levers, orb pedestals, teleporters. They
  also appear as triggers in the visual script builder.

## How to get it

- **Installer:** grab the v1.2.6 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.6 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
