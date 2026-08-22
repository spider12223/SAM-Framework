#!/usr/bin/env python3
"""
Write a Barony SLAB .vox file.

This doubles as the reference for the format, because "export as slab" is easy to say and
hard to find a spec for. MagicaVoxel's .vox is a different format that shares the extension;
Barony cannot read it.

    LAYOUT
      offset 0    int32 little-endian   width   (x)
      offset 4    int32 little-endian   height  (y)
      offset 8    int32 little-endian   depth   (z)
      offset 12   width*height*depth bytes, one palette INDEX per voxel
      then        768 bytes, 256 * (r, g, b)

    Total size is therefore exactly  12 + w*h*d + 768.  Measured across every .vox the game
    ships: 2410 of 2411 match. The one exception, models/decorations/ceiling_bbrick.vox, is a
    12-byte header-only stub with no data -- and it is not listed in models.txt, so the engine
    never loads it either.

    VOXEL ORDER is x-major, then y, then z:
        data[(x * height + y) * depth + z]
    which is the order files.cpp walks when it builds geometry.

    PALETTE INDEX 255 MEANS EMPTY. files.cpp skips any voxel whose value is 255 and draws
    every other value using palette[value]. So 0..254 are usable colours and 255 is air.

    PALETTE VALUES ARE 6-BIT. loadVoxel does `palette[c][i] << 2` after reading, so a byte
    of 63 becomes 252. Write `rgb // 4`, not `rgb`.

THIS SCRIPT DOES NOT CONVERT ANYTHING. It writes a placeholder and it validates files. If you
have a MagicaVoxel .vox, converting it is a separate job -- this will not do it, and pointing
this at your model would only overwrite it (which is why it now refuses to clobber).

Usage:
    python make_slab_vox.py out.vox            # writes the built-in placeholder
    python make_slab_vox.py out.vox --force    # ... overwriting out.vox if it exists
    python make_slab_vox.py --info a.vox b.vox # validate/describe existing files
"""
import os
import struct
import sys

EMPTY = 255


class Slab:
    def __init__(self, w, h, d):
        self.w, self.h, self.d = w, h, d
        self.data = bytearray([EMPTY]) * (w * h * d)
        self.palette = [(0, 0, 0)] * 256

    def _i(self, x, y, z):
        return (x * self.h + y) * self.d + z

    def set(self, x, y, z, idx):
        if 0 <= x < self.w and 0 <= y < self.h and 0 <= z < self.d:
            self.data[self._i(x, y, z)] = idx

    def box(self, x0, y0, z0, x1, y1, z1, idx):
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                for z in range(z0, z1 + 1):
                    self.set(x, y, z, idx)

    def write(self, path):
        out = bytearray()
        out += struct.pack('<iii', self.w, self.h, self.d)
        out += self.data
        for (r, g, b) in self.palette:
            # 6-bit: the loader shifts these left by 2.
            out += bytes((min(63, r // 4), min(63, g // 4), min(63, b // 4)))
        assert len(out) == 12 + self.w * self.h * self.d + 768, 'size formula broken'
        with open(path, 'wb') as f:
            f.write(bytes(out))
        return len(out)


def info(path):
    raw = open(path, 'rb').read()
    if len(raw) < 12:
        print('%s: too short to be a .vox (%d bytes)' % (path, len(raw)))
        return False
    if raw[:4] == b'VOX ':
        print('%s: MagicaVoxel format -- Barony CANNOT read this. Convert to slab.' % path)
        return False
    w, h, d = struct.unpack('<iii', raw[:12])
    expect = 12 + w * h * d + 768
    ok = (expect == len(raw)) and 0 < w <= 1024 and 0 < h <= 1024 and 0 < d <= 1024
    solid = sum(1 for b in raw[12:12 + max(0, w * h * d)] if b != EMPTY) if ok else 0
    print('%s: %dx%dx%d  %d bytes (expected %d)  %s  solid voxels: %d'
          % (path, w, h, d, len(raw), expect, 'OK' if ok else 'MISMATCH', solid))
    return ok


def placeholder():
    """A small winged creature. Readable at a glance, and obviously a placeholder."""
    w, h, d = 24, 24, 16
    m = Slab(w, h, d)
    BODY, WING, EYE, HORN = 1, 2, 3, 4
    m.palette[BODY] = (110, 40, 40)
    m.palette[WING] = (70, 25, 30)
    m.palette[EYE] = (255, 200, 60)
    m.palette[HORN] = (220, 210, 190)

    cy = h // 2
    # body: a tapered mass along x
    m.box(7, cy - 3, 5, 16, cy + 2, 10, BODY)
    m.box(17, cy - 2, 6, 20, cy + 1, 9, BODY)      # neck/head end
    m.box(3, cy - 2, 6, 6, cy + 1, 9, BODY)        # tail root
    m.box(0, cy - 1, 7, 2, cy, 8, BODY)            # tail tip
    # wings: flat plates either side
    m.box(8, cy - 10, 8, 15, cy - 4, 9, WING)
    m.box(8, cy + 3, 8, 15, cy + 9, 9, WING)
    # horns + eyes on the head end
    m.box(21, cy - 2, 10, 22, cy - 2, 11, HORN)
    m.box(21, cy + 1, 10, 22, cy + 1, 11, HORN)
    m.set(20, cy - 2, 9, EYE)
    m.set(20, cy + 1, 9, EYE)
    return m


if __name__ == '__main__':
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    if args[0] == '--info':
        # all() short-circuits, which meant a bad file hid every file after it. Describe
        # them all, then report.
        results = [info(p) for p in args[1:]]
        sys.exit(0 if all(results) else 1)
    out = args[0]
    force = '--force' in args[1:]
    if os.path.exists(out) and not force:
        print('refusing to overwrite %s (pass --force if you really mean to).' % out)
        print('note: this script does not convert MagicaVoxel files -- it only writes a')
        print('      placeholder and validates. Use --info to check a file instead.')
        sys.exit(1)
    n = placeholder().write(out)
    print('wrote %s (%d bytes)' % (out, n))
    info(out)
