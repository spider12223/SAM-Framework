/*
 * Where a race's icon PNG ships in the mod: the path typed in the Race Editor or, when a PNG was
 * uploaded and the path box is empty (never filled, or cleared afterwards), races/<slug>.png.
 * One function, so the JSON preview, the saved race, the exported zip and the note under the
 * Upload PNG button cannot disagree about whether a file ships and where.
 */
export function raceIconPath(iconText, hasUpload, slug) {
  const typed = (iconText ?? '').trim();
  if (typed) return typed;
  return hasUpload ? `races/${slug}.png` : '';
}
