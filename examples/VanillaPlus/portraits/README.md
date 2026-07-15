# VanillaPlus — class portraits

Each class can point at a `portrait` PNG on the character-select carousel via the
`"portrait": "portraits/<name>.png"` field in its class JSON. **These are optional** —
if a PNG is missing, S.A.M falls back to a placeholder icon (the classes still work).

## Spec (matches vanilla class icons)
- **54 × 54 px PNG**, transparent background.
- Drawn centered at native size and cropped to the 54×54 carousel button, so other
  sizes still work but 54×54 is exact.

## Placeholders to create (one per class)
| File | Class | Art direction |
|------|-------|---------------|
| `ranger.png`      | Ranger      | Green hood + bow; woodland browns/greens. |
| `necromancer.png` | Necromancer | Dark hood, skull motif; purples/bone-white. |
| `paladin.png`     | Paladin     | Helm + shield, holy cross; golds/steel-blue. |
| `berserker.png`   | Berserker   | Bare-chested, axe, red war paint; reds. |
| `trickster.png`   | Trickster   | Masked/hooded rogue, daggers; muted greys/teal. |

Until real art exists, leave these out (placeholder icon is used) or drop in any
54×54 PNG with the matching filename above and add `"portrait": "portraits/<name>.png"`
to that class's JSON.
