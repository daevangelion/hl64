#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Save latency of a run log (test/save/determinism.sh, architecture.md 14).

For every save that commits or fails in the log: its slot, the frames from
the request to the sealed image (`coded in N frames` on the seq line:
settle, snapshot, the coder's steps), the chip time of its flash writes
(the `flash: wrote ... N ms` lines between the seq line and the end; a Pak
save has none) and the whole time from the request to the commit (the
`save: save slot N ok|FAILED: ... N ms` line).

  latency.py [--auto-chip-ms A] [--manual-ms M] LOG...

prints one `latency: ...` line per save and exits 1 when an autosave
(slot 0) took more than A ms of chip time or a manual save more than M ms
from request to commit (limits off when not given: host runs, where the
image file finishes every operation at once).
"""
import argparse
import re
import sys

FLASH = re.compile(r"^flash: wrote 0x[0-9a-f]+ \+ (\d+) B: .*, (\d+) ms, longest step")
SNAP = re.compile(r"^save: slot (\d+) snapshot: ")
SEQ = re.compile(r"^save: slot (\d+) seq \d+: .*?(?:, coded in (\d+) frames)?$")
END = re.compile(r"^save: save slot (\d+) (ok|FAILED)(?: \(-?\d+\))?: (\d+) frames, .* (\d+) ms$")


def saves(path):
    """[(slot, coded frames or None, chip ms, written B, whole ms, ok)] of the log."""
    out, cur = [], None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = SNAP.match(line)
            if m:
                cur = {"slot": int(m.group(1)), "coded": None, "chip": 0, "bytes": 0}
                continue
            m = SEQ.match(line)
            if m:
                if cur is None or cur["slot"] != int(m.group(1)):
                    cur = {"slot": int(m.group(1)), "coded": None, "chip": 0, "bytes": 0}
                if m.group(2):
                    cur["coded"] = int(m.group(2))
                continue
            m = FLASH.match(line)
            if m and cur is not None:
                cur["bytes"] += int(m.group(1))
                cur["chip"] += int(m.group(2))
                continue
            m = END.match(line)
            if m:
                slot = int(m.group(1))
                c = cur if cur is not None and cur["slot"] == slot else {"coded": None, "chip": 0, "bytes": 0}
                out.append((slot, c["coded"], c["chip"], c["bytes"], int(m.group(4)), m.group(2) == "ok"))
                cur = None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--auto-chip-ms", type=int)
    ap.add_argument("--manual-ms", type=int)
    ap.add_argument("logs", nargs="+")
    a = ap.parse_args()
    bad = 0
    for path in a.logs:
        for slot, coded, chip, nb, whole, ok in saves(path):
            kind = "autosave" if slot == 0 else "manual"
            why = []
            if a.auto_chip_ms is not None and slot == 0 and chip > a.auto_chip_ms:
                why.append(f"chip {chip} ms > {a.auto_chip_ms}")
            if a.manual_ms is not None and slot != 0 and whole > a.manual_ms:
                why.append(f"request->commit {whole} ms > {a.manual_ms}")
            bad += bool(why)
            print(f"latency: {path}: slot {slot} {kind} {'ok' if ok else 'FAILED'}: coded in "
                  f"{coded if coded is not None else '?'} frames, chip {chip} ms ({nb} B), "
                  f"request->commit {whole} ms" + (f": FAIL ({'; '.join(why)})" if why else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
