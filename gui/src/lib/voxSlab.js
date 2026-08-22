/*
 * Barony SLAB .vox validation, in the browser.
 *
 * Shipping a MagicaVoxel .vox is by far the most common way a custom model fails, and until
 * now nothing caught it until the player launched the game and read sam_log.txt. The whole
 * check is 12 bytes of header plus a length comparison, so there is no reason not to do it
 * the moment the file is picked.
 *
 * FORMAT
 *   0   int32 LE  width
 *   4   int32 LE  height
 *   8   int32 LE  depth
 *   12  width*height*depth bytes, one palette index per voxel (255 = empty)
 *   ..  768 bytes, 256 * (r, g, b), 6-bit values the engine shifts left by 2
 *
 * Total is therefore exactly 12 + w*h*d + 768. Verified against all 600 models Barony ships.
 * This mirrors samValidateSlab in framework/sam_models.cpp; keep the two in step.
 */

export const SLAB_HEADER_BYTES = 12;
export const SLAB_PALETTE_BYTES = 768;
export const VOX_EMPTY_INDEX = 255;

/** Largest dimension we accept. Barony's own models are far below this. */
const MAX_DIM = 1024;

/**
 * @param {ArrayBuffer} buffer
 * @returns {{ok: boolean, reason?: string, width?: number, height?: number, depth?: number,
 *            solid?: number, expected?: number, size?: number}}
 */
export function inspectSlabVox(buffer) {
  const size = buffer.byteLength;
  if (size < SLAB_HEADER_BYTES) {
    return { ok: false, size, reason: `The file is only ${size} bytes, too short to be a .vox at all. Did the copy finish?` };
  }
  const bytes = new Uint8Array(buffer);

  // MagicaVoxel's own format is RIFF-ish and begins with the ASCII magic "VOX ". It shares an
  // extension with Barony's slab and nothing else.
  if (bytes[0] === 0x56 && bytes[1] === 0x4f && bytes[2] === 0x58 && bytes[3] === 0x20) {
    return {
      ok: false,
      size,
      reason: 'This is a MagicaVoxel .vox, which Barony cannot read. The two formats share an '
        + 'extension and nothing else — export or convert it to Barony slab format.',
    };
  }

  const view = new DataView(buffer);
  const width = view.getInt32(0, true);
  const height = view.getInt32(4, true);
  const depth = view.getInt32(8, true);

  if (width <= 0 || height <= 0 || depth <= 0
      || width > MAX_DIM || height > MAX_DIM || depth > MAX_DIM) {
    return {
      ok: false, size, width, height, depth,
      reason: `The header says this model is ${width}×${height}×${depth}, which is not a real `
        + 'size. This is not a Barony slab .vox.',
    };
  }

  const expected = SLAB_HEADER_BYTES + width * height * depth + SLAB_PALETTE_BYTES;
  if (size !== expected) {
    return {
      ok: false, size, width, height, depth, expected,
      reason: `The header says ${width}×${height}×${depth}, so a Barony slab would be exactly `
        + `${expected} bytes, but this file is ${size}. It is either truncated or not a slab .vox.`,
    };
  }

  let solid = 0;
  const end = SLAB_HEADER_BYTES + width * height * depth;
  for (let i = SLAB_HEADER_BYTES; i < end; i++) {
    if (bytes[i] !== VOX_EMPTY_INDEX) solid++;
  }
  if (solid === 0) {
    return {
      ok: false, size, width, height, depth, expected, solid,
      reason: `This is a valid ${width}×${height}×${depth} slab, but every voxel is empty — `
        + 'it would load and draw nothing at all.',
    };
  }

  return { ok: true, size, width, height, depth, expected, solid };
}

/** Short human summary of a good file, for the UI. */
export function describeSlab(info) {
  if (!info?.ok) return '';
  return `${info.width}×${info.height}×${info.depth}, ${info.solid.toLocaleString()} solid voxels, `
    + `${(info.size / 1024).toFixed(1)} KB`;
}
