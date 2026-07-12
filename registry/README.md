# Namespace Registry

Every S.A.M mod claims a **namespace** — a lowercase prefix (`^[a-z][a-z0-9_]*$`) that stamps every class and item id it ships (e.g. namespace `darkblade` → `darkblade:assassin`). Registering yours here keeps two mods from ever colliding on the same prefix.

## Reserve a namespace

1. Fork this repo.
2. Add an entry to [`namespaces.json`](namespaces.json), keeping the list sorted by `namespace`:
   ```json
   {
     "namespace": "yourmod",
     "author": "your_name_or_handle",
     "description": "One line about what your mod adds",
     "workshop_id": "1234567890"
   }
   ```
   (`workshop_id` is your Steam Workshop item id — leave `""` until published.)
3. Open a pull request. Keep it to a single new entry; don't modify others'.

The registry is advisory — S.A.M doesn't enforce it at runtime — but respecting it is how the community avoids id clashes.
