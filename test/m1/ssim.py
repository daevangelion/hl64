# SPDX-License-Identifier: MIT
"""SSIM of two 320x240 screenshots (Wang et al. 2004: luminance, 11x11
Gaussian window, sigma 1.5, K1 0.01, K2 0.03, range 255), averaged over
the pixels outside the masked rectangles. numpy + Pillow only.

  python3 test/m1/ssim.py TEST.ppm REF.png [--mask x0,y0,x1,y1 ...] [--diff out.png]

Prints `ssim <value>`; masks are inclusive pixel rectangles (things the
reference shows that are not world geometry: HUD, studio models, harness
bars).
"""
import argparse

import numpy as np
from PIL import Image


def luma(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64)
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def blur(a, w):
    r = len(w) // 2
    p = np.pad(a, r, mode='reflect')
    p = sum(w[i] * p[:, i:i + a.shape[1]] for i in range(len(w)))
    return sum(w[i] * p[i:i + a.shape[0], :] for i in range(len(w)))


def ssim_map(x, y):
    g = np.exp(-0.5 * (np.arange(11) - 5) ** 2 / 1.5 ** 2)
    g /= g.sum()
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mx, my = blur(x, g), blur(y, g)
    sxx = blur(x * x, g) - mx * mx
    syy = blur(y * y, g) - my * my
    sxy = blur(x * y, g) - mx * my
    return ((2 * mx * my + c1) * (2 * sxy + c2)) / ((mx * mx + my * my + c1) * (sxx + syy + c2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('test')
    ap.add_argument('ref')
    ap.add_argument('--mask', action='append', default=[])
    ap.add_argument('--diff')
    a = ap.parse_args()
    x, y = luma(a.test), luma(a.ref)
    if x.shape != y.shape:
        raise SystemExit('size %s vs %s' % (x.shape, y.shape))
    keep = np.ones(x.shape, bool)
    for m in a.mask:
        x0, y0, x1, y1 = (int(v) for v in m.split(','))
        keep[y0:y1 + 1, x0:x1 + 1] = False
    s = ssim_map(x, y)
    print('ssim %.4f' % s[keep].mean())
    if a.diff:
        d = np.clip(np.abs(x - y) * 2, 0, 255)
        d[~keep] = 64
        Image.fromarray(d.astype(np.uint8)).save(a.diff)


if __name__ == '__main__':
    main()
