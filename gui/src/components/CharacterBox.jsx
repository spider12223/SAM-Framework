/*
 * CharacterBox — a drag-to-rotate preview of a real Barony character.
 *
 * This draws the ACTUAL voxel models (757 voxels for a full character), not an
 * approximation. Rendering is a small software voxel painter on a 2D canvas:
 *   - zero dependencies, so no bundle cost on a static Pages site
 *   - works in every browser (a WebGL/three.js path would be far more machinery, and
 *     reading the user's own Barony folder for geometry is Chromium-only)
 *   - 757 cubes is nothing for canvas; painter's algorithm (sort back-to-front) is
 *     enough depth resolution for solid, axis-aligned voxels
 *
 * The geometry comes from Barony's .vox files, which are SLAB6-style (int32 sx,sy,sz;
 * voxel indices with 255 = empty; a trailing 768-byte 6-bit VGA palette that needs <<2)
 * — NOT MagicaVoxel, so an off-the-shelf parser would not have worked. They're baked to
 * JSON at author time rather than parsed in the browser.
 */
import { useEffect, useRef, useState, useCallback, useMemo } from 'react';
import VOXELS from '@/data/characterVoxels.json';

/*
 * Model axes, read off the models rather than assumed:
 *   size[0] = x = DEPTH   (front-to-back)
 *   size[1] = y = WIDTH   (shoulder-to-shoulder)
 *   size[2] = z = HEIGHT, growing DOWNWARD — z=0 is the TOP of a model.
 *
 * Both facts are load-bearing, and each has a distinct failure mode:
 *  - WIDTH vs DEPTH: the "looking along X" view of MaleTorso shows broad shoulders
 *    tapering to the waist — i.e. that is the FRONT, so y is width, not depth. Getting
 *    this backwards renders the figure as a stack of squashed slabs.
 *  - Z DIRECTION: MaleHead1 keeps its hair at z=0..6 with skin (the neck) at z=7, and
 *    MaleLegRight has the boot as a solid dark mass at its HIGHEST z. So z points down.
 *    Treating it as up flips every limb about its own centre while the layout offsets
 *    below stay correct — a well-posed character assembled from upside-down parts.
 *
 * Offsets in layout() are therefore written in render space (+z is up) and model z is
 * negated once, on the way in, so the two conventions never have to be juggled at once.
 */
const DEPTH = 0, WIDTH = 1, HEIGHT = 2;

/** Where each limb sits, in voxel units, relative to the torso centre. */
function layout(sex, headModel) {
  const body = VOXELS.body[sex] ?? VOXELS.body.male;
  const t = body.torso;
  const parts = [];
  // o = [depth, width, height] offset of this limb's CENTRE from the torso's centre.
  const add = (m, od, ow, oh) => { if (m) parts.push({ m, o: [od, ow, oh] }); };

  const halfT = t.size[HEIGHT] / 2;
  add(body.torso, 0, 0, 0);
  if (headModel) add(headModel, 0, 0, halfT + headModel.size[HEIGHT] / 2 - 1);

  // Arms hang either side of the shoulders, hips carry the legs. These offsets are
  // chosen to read as a body; they are NOT the engine's focal points, which live in a
  // per-race limbs.txt and would be a guess dressed as accuracy if copied here.
  // Arms hang OUTSIDE the shoulders — offsetting by half the torso MINUS half the arm
  // buries them inside the torso, where the depth sort quietly hides them.
  const armW = body.arm_right ? body.arm_right.size[WIDTH] : 2;
  const armH = body.arm_right ? body.arm_right.size[HEIGHT] : 8;
  const armOff = t.size[WIDTH] / 2 + armW / 2 - 0.5;
  const armZ = halfT - armH / 2;
  add(body.arm_right, 0, -armOff, armZ);
  add(body.arm_left, 0, armOff, armZ);

  const legW = body.leg_right ? body.leg_right.size[WIDTH] : 3;
  const legH = body.leg_right ? body.leg_right.size[HEIGHT] : 9;
  add(body.leg_right, 0, -(legW / 2 + 0.5), -(halfT + legH / 2) + 1);
  add(body.leg_left, 0, legW / 2 + 0.5, -(halfT + legH / 2) + 1);
  return parts;
}

export default function CharacterBox({ sex = 'male', onSexChange, headId = '', headLabel = '' }) {
  const canvasRef = useRef(null);
  const [rot, setRot] = useState({ x: -0.35, y: 0.6 });
  const drag = useRef(null);

  // A custom "ns:model" has no geometry we can know here, so fall back to that sex's
  // default head and say so — better than drawing nothing or pretending.
  const headModel = useMemo(() => {
    const m = VOXELS.heads[String(headId).trim()];
    if (m) return { model: m, real: true };
    const fallback = VOXELS.heads[sex === 'female' ? '125' : '113'];
    return { model: fallback, real: false };
  }, [headId, sex]);

  const parts = useMemo(() => layout(sex, headModel.model), [sex, headModel]);

  useEffect(() => {
    const cv = canvasRef.current;
    if (!cv) return;
    const ctx = cv.getContext('2d');
    const dpr = window.devicePixelRatio || 1;
    const W = cv.clientWidth, H = cv.clientHeight;
    cv.width = W * dpr; cv.height = H * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, W, H);

    // Gather every voxel into one list in model space, so limbs interleave correctly
    // when sorted — sorting per-limb would let an arm draw over the torso from behind.
    const pts = [];
    for (const { m, o } of parts) {
      const sd = m.size[DEPTH], sw = m.size[WIDTH], sh = m.size[HEIGHT];
      for (let i = 0; i < m.v.length; i += 4) {
        pts.push([
          m.v[i + DEPTH] - sd / 2 + o[0],      // depth
          m.v[i + WIDTH] - sw / 2 + o[1],      // width
          -(m.v[i + HEIGHT] - sh / 2) + o[2],  // height: model z grows down, render up
          m.colors[m.v[i + 3]],
        ]);
      }
    }

    const cy = Math.cos(rot.y), sy_ = Math.sin(rot.y);
    const cx = Math.cos(rot.x), sxr = Math.sin(rot.x);
    const S = 7.5;

    const proj = pts.map(([d, w, h, c]) => {
      // yaw spins the figure in the (width, depth) plane about the vertical axis
      const w1 = w * cy - d * sy_;
      const d1 = w * sy_ + d * cy;
      // then pitch tilts (depth, height)
      const d2 = d1 * cx - h * sxr;
      const h2 = d1 * sxr + h * cx;
      return { sx: w1 * S, sy: -h2 * S, depth: d2, c };
    });
    // painter's algorithm: far to near
    proj.sort((a, b) => b.depth - a.depth);

    const ox = W / 2, oy = H / 2 + 20;
    const sizeP = S * 1.06; // slight overlap so no seams show between voxels
    for (const p of proj) {
      ctx.fillStyle = p.c;
      ctx.fillRect(ox + p.sx - sizeP / 2, oy + p.sy - sizeP / 2, sizeP, sizeP);
    }
  }, [parts, rot]);

  const onDown = useCallback((e) => {
    drag.current = { px: e.clientX, py: e.clientY, ...rot };
    e.currentTarget.setPointerCapture?.(e.pointerId);
  }, [rot]);
  const onMove = useCallback((e) => {
    if (!drag.current) return;
    setRot({
      x: Math.max(-1.4, Math.min(1.4, drag.current.x - (e.clientY - drag.current.py) * 0.01)),
      y: drag.current.y + (e.clientX - drag.current.px) * 0.01,
    });
  }, []);
  const onUp = useCallback(() => { drag.current = null; }, []);

  return (
    <div>
      <div className="flex items-center gap-2 mb-2">
        {['male', 'female'].map((s) => (
          <GoldToggle key={s} active={sex === s} onClick={() => onSexChange?.(s)}>
            {s === 'male' ? '♂ Male' : '♀ Female'}
          </GoldToggle>
        ))}
        <span className="text-xs ml-auto" style={{ color: '#6b5a35' }}>drag to rotate</span>
      </div>
      <canvas
        ref={canvasRef}
        onPointerDown={onDown}
        onPointerMove={onMove}
        onPointerUp={onUp}
        onPointerLeave={onUp}
        className="sam-well"
        style={{ width: '100%', height: 300, touchAction: 'none', cursor: 'grab', display: 'block' }}
      />
      <div className="text-xs mt-1" style={{ color: '#6b5a35' }}>
        {headLabel ? <>Head → {headLabel}</> : 'Head → (unset — players keep their own head)'}
        {!headModel.real && headId && (
          <span className="sam-error"> — custom .vox can’t be previewed; showing the default head</span>
        )}
      </div>
    </div>
  );
}

/** Small local toggle so this component doesn't depend on the editor's button set. */
function GoldToggle({ active, onClick, children }) {
  return (
    <button
      type="button" onClick={onClick} className="sam-btn"
      style={{
        padding: '0.25rem 0.7rem', fontSize: '0.78rem',
        borderColor: active ? '#d4a84b' : undefined,
        color: active ? '#f0cd7a' : undefined,
      }}
    >
      {children}
    </button>
  );
}
