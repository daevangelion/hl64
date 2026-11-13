# SPDX-License-Identifier: MIT
"""Synthetic restab inputs for the restab and routec unit tests.

Builds a small staged content directory (CWG3 graph, two maps' entity text,
model / sprite / sound files, sentences.txt) and a definitions directory.
No game data: every name and byte here is made up.
"""
from __future__ import annotations

import os
import struct
from typing import Dict, List


def make_cwg(names: List[str]) -> bytes:
    """Minimal valid CWG3: maps only, no landmarks, links or seams."""
    strs = bytearray()
    offs = []
    for n in names:
        offs.append(len(strs))
        strs += n.encode() + b"\0"
    hdr = struct.pack(">IHHHHHHHHHHHHI", 0x43574733, 3, 0, len(names), 0, 0,
                      0, 0xFFFF, 1, 0, 0, 0, 0, len(strs))
    maps = b""
    for o in offs:
        maps += struct.pack(">HHHHHH", o, 0, 0, 0, 0, 0) + bytes(36)
    return hdr + maps + bytes(strs)


ENTS_A = b"""{
"classname" "worldspawn"
"wad" "\\half-life\\valve\\halflife.wad"
}
{
"classname" "func_door"
"targetname" "door1"
"noise1" "doors/doormove1.wav"
"model" "*1"
}
{
"classname" "monster_scientist"
"targetname" "sci"
"model" "models/Bar.mdl"
}
{
"classname" "env_sprite"
"model" "sprites/glow.spr"
"targetname" "door1"
}
{
"classname" "ambient_generic"
"message" "!SC_HELLO0"
}
{
"classname" "light"
"targetname" "lamp"
"pattern" "aazz"
"style" "32"
}
{
"classname" "env_global"
"globalstate" "dish_on"
}
"""

ENTS_B = b"""{
"classname" "worldspawn"
}
{
"classname" "func_button"
"targetname" "door1"
"noise" "doors/doormove1.wav"
}
{
"classname" "light"
"pattern" "mmnn"
}
{
"classname" "light"
"pattern" "m"
}
{
"classname" "monstermaker"
"targetname" "maker"
"netname" "garg"
"monstertype" "monster_scientist"
}
{
"classname" "func_wall"
"netname" "notachild"
}
"""

SENTENCES = b"""// test sentences
SC_HELLO0 vox/(p110) hello, hello(v80 e50).
SC_HELLO1 hello
HG_ALERT0 hgrunt/alert!(t20) clik
UNUSED0 vox/missingword
"""

DEFS: Dict[str, bytes] = {
    "classes.def": b"// classes\nCLASS(WORLDSPAWN, \"worldspawn\")\n"
                   b"CLASS(MONSTER_SCIENTIST, \"monster_scientist\")\n"
                   b"CLASS(WEAPON_GUN, \"weapon_gun\")\n",
    "res.def": b"RES(MODEL, GUN, \"models/gun.mdl\")\n"
               b"RES(SOUND, CLICK, \"weapons/click.wav\")  // trailing comment\n"
               b"RES(GROUP, HG_ALERT, \"HG_ALERT\")\n"
               b"RES(SENTENCE, SC_HELLO1, \"SC_HELLO1\")\n",
    "lightstyles.def": b"LIGHTSTYLE(0, \"m\")\nLIGHTSTYLE(1, \"mmnmmommommnonmmonqnmmo\")\n"
                       b"LIGHTSTYLE(63, \"a\")\n",
}

FILES: Dict[str, bytes] = {
    "maps/mapa.w64": b"W64L" + bytes(60),
    "maps/mapb.w64": b"W64L" + bytes(92),
    "maps/mapa.ent": ENTS_A,
    "maps/mapb.ent": ENTS_B,
    "models/bar.mdl": b"IDST" + bytes(10),
    "models/bart.mdl": b"IDST" + bytes(11),
    "models/bar01.mdl": b"IDSQ" + bytes(12),
    "models/gun.mdl": b"IDST" + bytes(13),
    "sprites/glow.spr": b"IDSP",
    "sprites/glow.sc4": b"SC4" + bytes(5),
    "sound/doors/doormove1.wav64": b"WV64" + bytes(20),
    "sound/weapons/click.wav64": b"WV64" + bytes(21),
    "sound/vox/hello.wav64": b"WV64" + bytes(22),
    "sound/vox/_comma.wav64": b"WV64" + bytes(23),
    "sound/vox/_period.wav64": b"WV64" + bytes(24),
    "sound/hgrunt/alert!.wav64": b"WV64" + bytes(25),
    "sound/hgrunt/clik.wav64": b"WV64" + bytes(26),
    "sound/sentences.txt": SENTENCES,
}


def build(root: str, files: Dict[str, bytes] = None, defs: Dict[str, bytes] = None,
          maps: List[str] = None) -> tuple:
    """Write content/ and defs/ under root; returns (content_dir, defs_dir)."""
    content = os.path.join(root, "content")
    defs_dir = os.path.join(root, "defs")
    all_files = dict(FILES if files is None else files)
    all_files.setdefault("maps/campaign.cwg", make_cwg(maps or ["mapa", "mapb"]))
    for rel, data in all_files.items():
        p = os.path.join(content, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
    os.makedirs(defs_dir, exist_ok=True)
    for name, data in (DEFS if defs is None else defs).items():
        with open(os.path.join(defs_dir, name), "wb") as f:
            f.write(data)
    return content, defs_dir
