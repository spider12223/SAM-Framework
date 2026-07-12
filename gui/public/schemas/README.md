# Deployed schema copies

These are **build-time copies** of the canonical schemas in
[`../../../schemas/`](../../../schemas/) — do **not** edit them here. They live under
`public/` so the deployed site serves them at
`https://spider12223.github.io/SAM-Framework/schemas/*.json`, which the
`.vscode/settings.json` autocomplete config and mod `$schema` fields point at.

Edit the originals in `SAM-Framework/schemas/`, then refresh these copies
(`cp ../../schemas/*.json .`) before building the GUI.
