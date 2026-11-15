# SPDX-License-Identifier: MIT
"""bake_logo: the player's media/logo.avi -> gfx/logo.lg4 (the title menu's
animated banner, engine/ui/menu.c).

logo.avi is the 1998 menu's banner: 640 x 100, 24 fps, MS RLE over an
8-bit grey palette. ffmpeg (when it is on PATH) decodes it from stdin to
grey frames; each frame is box-filtered 2 x 2 to 320 x 50 (the 640 x 480
menu's top on the 320 x 240 screen) and quantised to 4 bits with a 4 x 4
ordered dither. Without ffmpeg, or without logo.avi, the file holds one
still frame: gfx/lambda.bmp (the player's 128 x 128 8-bit lambda) scaled
to 50 x 50 at the left; the cook says which one it wrote.

LG4 (big-endian): u32 magic 'LG4 ', u16 version 1, u16 frames, u16 w,
u16 h, u16 fps, u16 stride (bytes a row), u32 frame_bytes, u32 data_ofs
(32), u32 file_size, u32 0, then the frames at data_ofs + i x frame_bytes:
I4 texels, two a byte, high nibble first, stride = (w + 15) / 16 x 8 (the
draw2d sheet layout, so the menu DMAs a frame straight into its sheet).

  python3 -m tools.cook.bake_logo --data DIR -o logo.lg4
"""
from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Tuple

LG4_MAGIC = 0x4C473420  # 'LG4 '
SRC_W, SRC_H = 640, 100
W, H = 320, 50
FPS = 24
HDR = 32
MAX_FRAMES = 512
BAYER4 = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)


class LogoError(ValueError):
    pass


def stride(w: int) -> int:
    return (w + 15) // 16 * 8


def pack_i4(grey: List[int], w: int, h: int) -> bytes:
    """8-bit grey (w x h) -> dithered I4 rows of stride(w) bytes."""
    s = stride(w)
    out = bytearray(s * h)
    for y in range(h):
        for x in range(w):
            v = grey[y * w + x] * 15 + BAYER4[(y & 3) * 4 + (x & 3)] * 255 // 16
            q = min(15, v // 255)
            out[y * s + x // 2] |= q << 4 if x % 2 == 0 else q
    return bytes(out)


def half(frame: bytes) -> List[int]:
    """640 x 100 grey -> 320 x 50, 2 x 2 box filter (rounded)."""
    out = [0] * (W * H)
    for y in range(H):
        a = frame[2 * y * SRC_W:(2 * y + 1) * SRC_W]
        b = frame[(2 * y + 1) * SRC_W:(2 * y + 2) * SRC_W]
        for x in range(W):
            out[y * W + x] = (a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2
    return out


def decode_avi(avi: bytes, ffmpeg: str) -> List[bytes]:
    p = subprocess.run([ffmpeg, "-loglevel", "error", "-f", "avi", "-i", "pipe:0",
                        "-f", "rawvideo", "-pix_fmt", "gray", "pipe:1"],
                       input=avi, capture_output=True, check=False)
    if p.returncode != 0:
        raise LogoError(f"ffmpeg: {p.stderr.decode(errors='replace').strip()}")
    n = len(p.stdout) // (SRC_W * SRC_H)
    if n < 1 or n > MAX_FRAMES or len(p.stdout) != n * SRC_W * SRC_H:
        raise LogoError(f"logo.avi: {len(p.stdout)} B of frames is not n x {SRC_W}x{SRC_H}")
    return [p.stdout[i * SRC_W * SRC_H:(i + 1) * SRC_W * SRC_H] for i in range(n)]


def lambda_frame(bmp: bytes) -> List[int]:
    """gfx/lambda.bmp (8-bit BMP) -> one 320 x 50 grey frame, lambda at the left."""
    if len(bmp) < 54 or bmp[:2] != b"BM":
        raise LogoError("gfx/lambda.bmp: not a BMP")
    pix, = struct.unpack_from("<I", bmp, 10)
    hsize, = struct.unpack_from("<I", bmp, 14)
    bw, bh, _, bpp = struct.unpack_from("<iiHH", bmp, 18)
    ncol, = struct.unpack_from("<I", bmp, 46)
    ncol = ncol or 256
    if bpp != 8 or bw <= 0 or bh == 0 or 14 + hsize + 4 * ncol > len(bmp):
        raise LogoError("gfx/lambda.bmp: not an 8-bit BMP")
    pal = bmp[14 + hsize:14 + hsize + 4 * ncol]
    ah = abs(bh)
    row = (bw + 3) & ~3
    if pix + row * ah > len(bmp):
        raise LogoError("gfx/lambda.bmp: short pixel data")
    out = [0] * (W * H)
    for y in range(H):
        sy = y * ah // H
        sy = ah - 1 - sy if bh > 0 else sy
        for x in range(H):
            i = bmp[pix + sy * row + x * bw // H]
            if i < ncol:
                b, g, r = pal[4 * i], pal[4 * i + 1], pal[4 * i + 2]
                out[y * W + x] = max(r, g, b)
    return out


def build(frames: List[List[int]], fps: int) -> bytes:
    s = stride(W)
    fb = (s * H + 15) & ~15
    body = b"".join(pack_i4(f, W, H).ljust(fb, b"\0") for f in frames)
    size = HDR + len(body)
    hdr = struct.pack(">IHHHHHHIIII", LG4_MAGIC, 1, len(frames), W, H, fps, s, fb, HDR, size, 0)
    return hdr + body


def bake(avi: Optional[bytes], bmp: Optional[bytes],
         ffmpeg: Optional[str] = None) -> Tuple[bytes, str]:
    """(lg4 bytes, what the player gets) from logo.avi, else lambda.bmp."""
    ffmpeg = ffmpeg if ffmpeg is not None else shutil.which("ffmpeg")
    if avi is not None and ffmpeg:
        raw = decode_avi(avi, ffmpeg)
        return build([half(f) for f in raw], FPS), f"logo.avi: {len(raw)} frames at {FPS} fps"
    if bmp is None:
        raise LogoError("neither media/logo.avi (with ffmpeg) nor gfx/lambda.bmp")
    why = "no ffmpeg" if avi is not None else "no media/logo.avi"
    return build([lambda_frame(bmp)], 0), f"still gfx/lambda.bmp ({why})"


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", type=Path, required=True)
    ap.add_argument("-o", type=Path, required=True)
    a = ap.parse_args(argv)
    avi, bmp = a.data / "media" / "logo.avi", a.data / "gfx" / "lambda.bmp"
    try:
        b, what = bake(avi.read_bytes() if avi.is_file() else None,
                       bmp.read_bytes() if bmp.is_file() else None)
    except LogoError as e:
        print(f"bake_logo: error: {e}", file=sys.stderr)
        return 1
    a.o.write_bytes(b)
    print(f"bake_logo: {what}, {len(b)} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
