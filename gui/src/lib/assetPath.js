/*
 * Where an uploaded picture ships in the mod: the path typed into the editor's path box or, when a PNG
 * was uploaded and the box is empty (never filled, or cleared afterwards), the editor's default path.
 * One function, so the JSON preview, the saved entry, the exported zip and the note under the Upload
 * button cannot disagree about whether a file ships and where. The Race, Spell and Class editors use it.
 */
export function assetPath(typed, hasUpload, fallback) {
  const t = (typed ?? '').trim();
  if (t) return t;
  return hasUpload ? fallback : '';
}
