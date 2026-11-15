# SPDX-License-Identifier: MIT
"""hl_music: the CD music of the cooked maps (docs/specs/sound.md 6).

GoldSrc maps play music with trigger_cdaudio / target_cdaudio: HLSDK
PlayCDTrack sends the client `cd play N` for the entity's "health" key
(tracks -1..30; -1 = `cd stop`). The client plays track N from the
player's valve/media/*.mp3 by the table below (documentation: TWHL
"Tutorial: Making Music Play in Maps" track table; Valve Developer
Community "List of Half-Life 1 music": track 2 Half-Life01 "Adrenaline
Horror", 3 Prospero01, ...). Tracks 1 (the CD's data track) and 30 (CD
audio only) have no file; 29 is gamestartup.mp3, which the client plays
at launch (retail valve/media lacks it).

Only the tracks the maps' entities name are cooked: media/cdNN.wav64 (NN
= track) from the player's MP3, at SND_HZ, stereo, VADPCM 4-bit without
Huffman (Music slice bench on ares, per 1/30 s of a stereo stream at
22050 Hz: CPU 0.29 ms, CPU + RSP 0.59 ms, 1.49 MB of ROM per minute;
3-bit VADPCM is 22 % smaller but 5-6 dB noisier on dense tracks, Huffman
VADPCM costs 0.8 ms more CPU, ULC 1.3 ms CPU + 0.9 ms RSP, Opus 16 ms
CPU). A track whose MP3 the player lacks is a warning and absent: the
engine plays nothing for it.
"""
from __future__ import annotations

import hashlib
import subprocess
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

from tools.cook import hl_sound_classes

# GoldSrc CD track -> media/<name>.mp3
TRACKS: Dict[int, str] = {
    2: "Half-Life01", 3: "Prospero01", 4: "Half-Life12", 5: "Half-Life07",
    6: "Half-Life10", 7: "Suspense01", 8: "Suspense03", 9: "Half-Life09",
    10: "Half-Life02", 11: "Half-Life13", 12: "Half-Life04", 13: "Half-Life15",
    14: "Half-Life14", 15: "Half-Life16", 16: "Suspense02", 17: "Half-Life03",
    18: "Half-Life08", 19: "Prospero02", 20: "Half-Life05", 21: "Prospero04",
    22: "Half-Life11", 23: "Half-Life06", 24: "Prospero03", 25: "Half-Life17",
    26: "Prospero05", 27: "Suspense05", 28: "Suspense07", 29: "gamestartup",
}

CLASSES = ("trigger_cdaudio", "target_cdaudio")
SPEC = "vadpcm,huffman=false"
RATE = 22050


class MusicError(Exception):
    pass


def track_of(ent: Dict[str, str]) -> int:
    """The track an entity plays: (int)pev->health, as HLSDK; 0 none."""
    try:
        return int(float(ent.get("health", "0")))
    except ValueError:
        return 0


def referenced(ents: Dict[str, List[Dict[str, str]]]) -> Dict[int, str]:
    """{track: first referrer} of the tracks with a file the maps play."""
    out: Dict[int, str] = {}
    for m in sorted(ents):
        for e in ents[m]:
            if e.get("classname", "").lower() not in CLASSES:
                continue
            t = track_of(e)
            if t in TRACKS and t not in out:
                out[t] = f"{m} {e.get('classname')} {e.get('targetname', '(no name)')}"
    return out


def content_name(track: int) -> str:
    return f"media/cd{track:02d}.wav64"


def convert(audioconv: str, mp3: Path, out_dir: Path) -> Path:
    """mp3 -> out_dir/<stem>.wav64 (SPEC at RATE, stereo kept)."""
    r = subprocess.run([audioconv, "--wav-compress", SPEC, "--wav-resample", str(RATE),
                        "-o", str(out_dir), str(mp3)],
                       stdin=subprocess.DEVNULL, capture_output=True)
    out = out_dir / (mp3.stem + ".wav64")
    if r.returncode or not out.is_file():
        raise MusicError(f"audioconv64 failed on {mp3}: {r.stderr.decode(errors='replace')[-300:]}")
    return out


def stage(data, ents, staging: Path, put, log) -> List[Tuple[int, str, int]]:
    """Cook the referenced tracks: put(rel, path) each media/cdNN.wav64;
    returns [(track, mp3 name, wav64 bytes)]. Conversions are cached in
    staging/media by the MP3's hash, the spec and the encoder."""
    refs = referenced(ents)
    done: List[Tuple[int, str, int]] = []
    if not refs:
        return done
    try:
        audioconv = hl_sound_classes.find_audioconv()
        enc = hl_sound_classes.encoder_id()
    except hl_sound_classes.SoundError as e:
        raise MusicError(str(e)) from None
    sdir = staging / "media"
    sdir.mkdir(parents=True, exist_ok=True)
    for t in sorted(refs):
        name = TRACKS[t] + ".mp3"
        mp3 = data.read("media/" + name)
        if mp3 is None:
            log(f"    warning: media/{name} (CD track {t}): not in the game data, played by "
                f"{refs[t]}: absent")
            continue
        key = f"{hashlib.sha256(mp3).hexdigest()} {SPEC} {RATE} {enc}\n"
        src = sdir / f"cd{t:02d}.mp3"
        out = src.with_suffix(".wav64")
        stamp = src.with_suffix(".plan")
        if not (stamp.is_file() and stamp.read_text() == key and out.is_file()):
            stamp.unlink(missing_ok=True)
            src.write_bytes(mp3)
            log(f"    converting media/{name} (CD track {t}, audioconv64 {SPEC} {RATE} Hz)")
            convert(audioconv, src, sdir)
            src.unlink()
            stamp.write_text(key)
        put(content_name(t), out)
        done.append((t, name, out.stat().st_size))
    return done


def unknown_tracks(ents) -> Iterable[Tuple[int, str]]:
    """(track, referrer) the maps name outside TRACKS (-1 stop excluded)."""
    for m in sorted(ents):
        for e in ents[m]:
            if e.get("classname", "").lower() in CLASSES:
                t = track_of(e)
                if t != -1 and t not in TRACKS:
                    yield t, f"{m} {e.get('classname')} {e.get('targetname', '(no name)')}"
