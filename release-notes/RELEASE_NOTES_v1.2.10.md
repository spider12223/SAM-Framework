# S.A.M Framework v1.2.10 — richer block-builder conditions

More ways for the visual builder to ask "when should this fire?", from community feedback.
Two of these needed a new script function, so this is an engine build.

## New script functions

- **`sam_get_item_category(item)`** — the category of an item (WEAPON / ARMOR / GEM /
  POTION / SCROLL / SPELLBOOK / …). `item` is a numeric id (e.g. an event's `item_type`)
  or a name (vanilla or `"namespace:item"`). Lets a script react to a whole *kind* of item.
- **`sam_monster_has_effect(uid, effect)`** — the monster counterpart of `sam_has_effect`.
  Pass a monster UID (from a monster event or `sam_get_nearby_entities`) to check whether
  it currently has a status effect.

Both are in the Lua and JavaScript/TypeScript runtimes.

## New conditions in the Mod Builder (already live on the site)

- **Stat as a "% of max"** — the "stat compares to a value" condition can compare HP or MP
  to a *fraction of max* ("HP below 10% of max"), not just a flat number.
- **"gold just picked up compares to"** — reads *this pickup's* amount (`event.amount`),
  so "pick up 10 gold → reward" works, distinct from the running gold total.
- **"the event's item is a specific item"** and **"the event's item is a category"** — on
  identify / drop / broken events, react to one exact item or a whole category (reward
  identifying *any* GEM).
- **"the monster has an effect"** — on monster events (on_monster_damaged / died), ask
  about the *monster's* status effects, e.g. "when I hit a monster that's already POISONED."

Event-based conditions only appear under a trigger that actually carries the field they
read, so the builder can't emit a condition that references a nonexistent event value.

## Also fixed on the site (v1.2.9.x web updates, no exe needed)

- **Class attributes are modifiers, not final values.** The editor was defaulting each
  attribute to 10 — silently making every class **+60** — and the balance hint had baked
  that in ("vanilla sits near 60"). Vanilla classes net roughly 0 to +5. New classes now
  default to 0, zero modifiers are omitted on export, the hint reflects real vanilla
  magnitudes, and the panel says these are +/- modifiers.
- The spell picker went from 40 to the full **142** player-facing spells; readable books
  have a **"Book / Contents"** chooser; and the apron / spellbook no longer land in the
  cloak slot.

## How to get it

- **Installer:** grab the v1.2.10 installer below. Installers already in the wild
  auto-update to this build.
- **Steam Workshop:** the `sam_framework` Workshop item is updated to the v1.2.10 exe.

---

*Engine changes ship inside the attached `barony.exe`. `typescript.js` must sit next
to the exe (the installer and Workshop item handle this).*
