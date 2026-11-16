#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The SD card image of test/run/sc64.sh (test/sc64/sc64test.c).

  mkimage.py make IMAGE     1 MiB: big-endian word pat_in(w) at every word w
  mkimage.py check IMAGE    after the run: sectors WRITE_LBA..+WRITE_N hold
                            pat_out (the ROM's SD_WRITE), every other word
                            is still pat_in
"""
import struct
import sys

SECTOR = 512
SECTORS = 2048
WRITE_LBA, WRITE_N = 1024, 16
M = 0xFFFFFFFF


def pat_in(w):
    return (w * 0x9E3779B1 ^ 0x5C64C0DE) & M


def pat_out(w):
    return ~(w * 0x85EBCA6B) & M


def words(n, f, base=0):
    return struct.pack(">%dI" % n, *(f(base + i) for i in range(n)))


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("make", "check"):
        sys.exit(__doc__)
    path = sys.argv[2]
    nwords = SECTORS * SECTOR // 4
    if sys.argv[1] == "make":
        with open(path, "wb") as f:
            f.write(words(nwords, pat_in))
        return
    want = bytearray(words(nwords, pat_in))
    lo, n = WRITE_LBA * SECTOR, WRITE_N * SECTOR
    want[lo:lo + n] = words(n // 4, pat_out)
    got = open(path, "rb").read()
    if len(got) != len(want):
        sys.exit("sc64 image: %d bytes, want %d" % (len(got), len(want)))
    if got != want:
        bad = next(i for i in range(len(want)) if got[i] != want[i])
        sys.exit("sc64 image: first difference at byte 0x%x (sector %d)" % (bad, bad // SECTOR))
    print("sc64 image: SD_WRITE sectors %d..%d hold the ROM's pattern, the rest unchanged"
          % (WRITE_LBA, WRITE_LBA + WRITE_N - 1))


if __name__ == "__main__":
    main()
