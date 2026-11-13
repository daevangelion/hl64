# SPDX-License-Identifier: MIT
"""hl_sound_classes: per-WAV encoding plan and the wav64 conversion.

Imported from the quakegeneric tree (tools/hl_sound_classes.py and the
conversion loop of tools/convert_hl_sounds.sh, provenance-audit.md entries
CLEAN). Each WAV gets the encoding of its class in the rules file
(tools/cook/hl_sound_classes.txt, format in its header): mono VADPCM at the
source rate (capped by the class), Huffman only where the rules say it
pays. A WAV with a "cue " chunk loops from its first cue point (GoldSrc
loops a sample iff its WAV has a cue chunk). audioconv64 asserts on some
short or silent samples: a class encoding that fails falls back to plain
VADPCM, then to PCM; an output under 64 bytes is a failure.

  python3 -m tools.cook.hl_sound_classes <rules> <sound_dir>   # print the plan
"""
from __future__ import annotations

import fnmatch
import os
import resource
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Dict, List, Optional, Tuple

RULES = Path(__file__).resolve().with_name("hl_sound_classes.txt")
MIN_WAV64 = 64


class SoundError(Exception):
    pass


def parse_rules(path: Path):
    classes: Dict[str, Tuple[str, int, bool]] = {}
    matches: List[Tuple[str, str]] = []
    loop_cls: Optional[str] = None
    default: Optional[str] = None
    for n, line in enumerate(path.read_text().splitlines(), 1):
        f = line.split("#", 1)[0].split()
        if not f:
            continue
        kind, args = f[0], f[1:]
        if kind == "class" and len(args) in (3, 4) and args[3:] in ([], ["exact"]) \
                and args[2].isdigit():
            classes[args[0]] = (args[1], int(args[2]), args[3:] == ["exact"])
        elif kind == "match" and len(args) >= 2:
            matches += [(args[0], g) for g in args[1:]]
        elif kind == "loop" and len(args) == 1:
            loop_cls = args[0]
        elif kind == "default" and len(args) == 1:
            default = args[0]
        else:
            raise SoundError(f"{path}:{n}: bad rule: {line.strip()}")
    for c in [m[0] for m in matches] + [loop_cls, default]:
        if c is not None and c not in classes:
            raise SoundError(f"{path}: class {c} used but not defined")
    if default is None:
        raise SoundError(f"{path}: no default class")
    return classes, matches, loop_cls, default


def wav_info(b: bytes, what: str) -> Tuple[int, Optional[int]]:
    """(sample rate, first cue point or None) of a RIFF WAVE file."""
    if len(b) < 12 or b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise SoundError(f"{what}: not a RIFF WAVE file")
    rate, cue = 0, None
    i = 12
    while i + 8 <= len(b):
        cid, n = b[i:i + 4], struct.unpack("<I", b[i + 4:i + 8])[0]
        if i + 8 + n > len(b):
            break                      # truncated last chunk: as the game reads it
        if cid == b"fmt " and n >= 16:
            rate = struct.unpack("<I", b[i + 12:i + 16])[0]
        elif cid == b"cue " and n >= 28 and cue is None:
            cue = struct.unpack("<I", b[i + 8 + 24:i + 8 + 28])[0]
        i += 8 + n + (n & 1)
    if rate == 0:
        raise SoundError(f"{what}: no fmt chunk")
    return rate, cue


def plan(rules, rel: str, data: bytes):
    """(class, --wav-compress spec, resample Hz or 0, loop start or None)."""
    classes, matches, loop_cls, default = rules
    rate, cue = wav_info(data, rel)
    rel = rel.lower()
    cls = next((c for c, g in matches if fnmatch.fnmatchcase(rel, g)), None)
    if cls is None:
        cls = loop_cls if cue is not None and loop_cls else default
    spec, rate_cap, exact = classes[cls]
    resample = rate_cap if rate > rate_cap or (exact and rate != rate_cap) else 0
    return cls, spec, resample, cue


def _no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def convert_one(audioconv: str, wav: Path, spec: str, resample: int,
                cue: Optional[int]) -> Tuple[Path, str]:
    """wav -> <wav dir>/<stem>.wav64; returns (output, encoding used)."""
    out = wav.with_suffix(".wav64")
    args: List[str] = []
    if resample:
        args += ["--wav-resample", str(resample)]
    if cue is not None:
        args += ["--wav-loop", "true"]
        if cue:
            args += ["--wav-loop-offset", str(cue)]
    tries = [spec] + [s for s in ("1", "0") if s != spec]
    for s in tries:
        r = subprocess.run([audioconv, "--wav-compress", s, "--wav-mono", *args,
                            "-o", str(wav.parent), str(wav)],
                           stdin=subprocess.DEVNULL, capture_output=True,
                           preexec_fn=_no_core)
        if r.returncode == 0 and out.is_file() and out.stat().st_size >= MIN_WAV64:
            return out, s
        out.unlink(missing_ok=True)
    raise SoundError(f"audioconv64 failed on {wav}: {r.stderr.decode(errors='replace')[-300:]}")


def find_audioconv() -> str:
    hit = shutil.which("audioconv64")
    if hit:
        return hit
    inst = os.environ.get("N64_INST")
    if inst and os.access(Path(inst, "bin", "audioconv64"), os.X_OK):
        return str(Path(inst, "bin", "audioconv64"))
    raise SoundError("audioconv64 not found (set N64_INST to the libdragon SDK)")


def convert_all(jobs: List[Tuple[Path, str, int, Optional[int]]], threads: int):
    """[(wav, spec, resample, cue)] -> [(wav64 path, encoding used)]."""
    audioconv = find_audioconv()
    with ThreadPoolExecutor(max_workers=max(1, threads)) as ex:
        return list(ex.map(lambda j: convert_one(audioconv, *j), jobs))


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        rules = parse_rules(Path(argv[0]))
        root = Path(argv[1])
        for wav in sorted(root.rglob("*.wav")):
            rel = wav.relative_to(root).as_posix()
            cls, spec, resample, cue = plan(rules, rel, wav.read_bytes())
            print(f"{wav}\t{cls}\t{spec}\t{resample}\t{'-' if cue is None else cue}")
    except (SoundError, OSError) as e:
        print(f"hl_sound_classes: error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
