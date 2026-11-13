# SPDX-License-Identifier: MIT
"""Dither check of the M1 gate: the same pose shot with r_dither 0 (OFF)
and r_dither 1 (ON), 320x240 16-bit framebuffer dumps.

The RDP's magic-square dither adds d in 0..7 (uniform over a 4x4 cell)
to each 8-bit channel v before cutting it to 5 bits, so the 5-bit result
averages v / 8 exactly; without dithering it is floor(v / 8), short by the
residual (v mod 8) / 8, 3.5 / 8 on average. The dump expands c to
(c << 3) | (c >> 2) = 8.25 c, so ON is brighter than OFF by 8.25 x 3.5 / 8
= 3.61 per channel on average, and the dither is live and unbiased when
the measured mean difference is that expectation within 1.0. Pixels whose
OFF value is 0 in a channel are left out of that channel (black: v < 8,
the residual is not uniform there), as are the masked rectangles.

  python3 test/m1/dither.py OFF.ppm ON.ppm [--mask x0,y0,x1,y1 ...]

Prints `dither dR dG dB n` and exits 0 when every channel is within
tolerance, 1 otherwise.
"""
import argparse
import sys

import numpy as np
from PIL import Image

EXPECT = 8.25 * 3.5 / 8
TOL = 1.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('off')
    ap.add_argument('on')
    ap.add_argument('--mask', action='append', default=[])
    a = ap.parse_args()
    off = np.asarray(Image.open(a.off).convert('RGB'), dtype=np.float64)
    on = np.asarray(Image.open(a.on).convert('RGB'), dtype=np.float64)
    if off.shape != on.shape:
        raise SystemExit('size %s vs %s' % (off.shape, on.shape))
    keep = np.ones(off.shape[:2], bool)
    for m in a.mask:
        x0, y0, x1, y1 = (int(v) for v in m.split(','))
        keep[y0:y1 + 1, x0:x1 + 1] = False
    d, n = [], []
    for k in range(3):
        sel = keep & (off[..., k] > 0)
        n.append(int(sel.sum()))
        d.append(float((on[..., k] - off[..., k])[sel].mean()) if n[-1] else float('nan'))
    print('dither %.3f %.3f %.3f %d (expect %.3f +- %.1f)' % (d[0], d[1], d[2], min(n), EXPECT, TOL))
    ok = all(abs(x - EXPECT) <= TOL for x in d)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
