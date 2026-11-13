# SPDX-License-Identifier: MIT
"""restab: cook the campaign resource table (campaign.rtb).

Reads a staged content directory and the game's definition files and writes
docs/formats/rtb-format.md: every model, sprite and sound any map or the
game refers to (stable res_t ids, deterministic order: kind, then name),
the cart file table, the sentences and groups in use, the lightstyle
pattern table, per-map targetnames (route and debug name resolution),
classes and global states.

Every input is untrusted: parse errors and unresolved names stop the cook
with the file, line or record that caused them (precache-and-resources.md
R1).

  python3 -m tools.restab.restab --content DIR --defs DIR -o campaign.rtb
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from typing import Dict, List, Optional, Set, Tuple

from . import rtb
from tools.studio import s64

MAX_ENTS_TEXT = 4 << 20       # entity lump text cap per map (bytes)
MAX_DEF_LINES = 65535
MAX_SENTENCE_FILE = 1 << 20
MAX_PATTERN = 255
MAX_SEQGROUPS = 15
# entity keys whose "!NAME" value names a sentence (ambient_generic message,
# scripted_sentence sentence, noise keys)
SENTENCE_KEYS = frozenset(("message", "sentence", "noise", "noise1", "noise2", "noise3",
                           "UseSentence", "UnUseSentence"))
# keys whose plain value names a sentence group (talk monsters' use lines)
GROUP_KEYS = frozenset(("UseSentence", "UnUseSentence"))
# HLSDK scriptevent.h: a studio event whose options name a sentence group
SCRIPT_EVENT_SENTENCE_RND1 = 1009


class CookError(Exception):
    pass


# --------------------------------------------------------------------------
# CWG3 (docs/formats/cwg-format.md): only the map names are needed here.

def read_cwg_maps(data: bytes) -> List[str]:
    """Map names of a CWG3 file, in map index order. Validates the header
    and the exact file size; rejects names outside the string table."""
    if len(data) < 32:
        raise CookError("campaign.cwg: shorter than the header")
    magic, version, _flags = struct.unpack_from(">IHH", data, 0)
    if magic != 0x43574733 or version != 3:
        raise CookError("campaign.cwg: not CWG3")
    nmaps, nlm, nlinks = struct.unpack_from(">HHH", data, 8)
    nseams, ntwins, nsect = struct.unpack_from(">HHH", data, 20)
    (strbytes,) = struct.unpack_from(">I", data, 28)
    for c in (nmaps, nlm, nlinks, nseams, ntwins, nsect):
        if c > 0xFFFE:
            raise CookError("campaign.cwg: count over 0xFFFE")
    size = (32 + 48 * nmaps + 16 * nlm + 8 * nlinks + 136 * nseams
            + 4 * ntwins + 2 * nsect + strbytes)
    if size != len(data):
        raise CookError(f"campaign.cwg: size {len(data)} != {size} from its counts")
    strs = data[len(data) - strbytes:]
    names = []
    for i in range(nmaps):
        (off,) = struct.unpack_from(">H", data, 32 + 48 * i)
        end = strs.find(b"\0", off)
        if off >= len(strs) or end < 0:
            raise CookError(f"campaign.cwg: map {i} name outside the string table")
        name = strs[off:end].decode("ascii", "strict") if strs[off:end].isascii() else None
        if not name or not re.fullmatch(r"[A-Za-z0-9_\-]+", name):
            raise CookError(f"campaign.cwg: map {i} has a bad name")
        names.append(name)
    if len(set(names)) != len(names):
        raise CookError("campaign.cwg: duplicate map name")
    return names


# --------------------------------------------------------------------------
# entity lump text (the BSP entity lump verbatim)

def parse_entities(text: bytes, where: str) -> List[List[Tuple[str, str]]]:
    """Entities as ordered (key, value) lists; index = BSP lump index."""
    if len(text) > MAX_ENTS_TEXT:
        raise CookError(f"{where}: entity text over {MAX_ENTS_TEXT} bytes")
    s = text.decode("latin-1")
    ents: List[List[Tuple[str, str]]] = []
    i, n, line = 0, len(s), 1
    cur: Optional[List[Tuple[str, str]]] = None
    pending: Optional[str] = None
    while i < n:
        c = s[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r" or c == "\0":
            i += 1
        elif c == "{":
            if cur is not None:
                raise CookError(f"{where}:{line}: nested '{{'")
            cur = []
            i += 1
        elif c == "}":
            if cur is None or pending is not None:
                raise CookError(f"{where}:{line}: unexpected '}}'")
            ents.append(cur)
            cur = None
            i += 1
        elif c == '"':
            end = s.find('"', i + 1)
            nl = s.find("\n", i + 1)
            if end < 0 or (0 <= nl < end):
                raise CookError(f"{where}:{line}: unterminated string")
            tok = s[i + 1:end]
            i = end + 1
            if cur is None:
                raise CookError(f"{where}:{line}: string outside an entity")
            if pending is None:
                pending = tok
            else:
                cur.append((pending, tok))
                pending = None
        else:
            raise CookError(f"{where}:{line}: unexpected character {c!r}")
    if cur is not None:
        raise CookError(f"{where}: missing final '}}'")
    if len(ents) > 0xFFFF:
        raise CookError(f"{where}: more than 65535 entities")
    return ents


# --------------------------------------------------------------------------
# game/defs (X-macro lines; docs/formats/rtb-format.md "Definition files")

_STR = r'"([^"\\\n]*)"'
_DEF_RE = {
    "classes.def": re.compile(r"CLASS\(\s*([A-Z][A-Z0-9_]*)\s*,\s*" + _STR + r"\s*\)"),
    "res.def": re.compile(r"RES\(\s*(MODEL|SPRITE|SOUND|SENTENCE|GROUP)\s*,\s*"
                          r"([A-Z][A-Z0-9_]*)\s*,\s*" + _STR + r"\s*\)"),
    "lightstyles.def": re.compile(r"LIGHTSTYLE\(\s*([0-9]{1,2})\s*,\s*" + _STR + r"\s*\)"),
}


def read_defs(path: str, fname: str) -> List[Tuple[int, tuple]]:
    """(line, groups) per definition line; missing file = no lines."""
    full = os.path.join(path, fname)
    if not os.path.exists(full):
        return []
    with open(full, "rb") as f:
        data = f.read(1 << 20)
    out = []
    idents: Set[str] = set()
    for ln, raw in enumerate(data.decode("latin-1").splitlines(), 1):
        if ln > MAX_DEF_LINES:
            raise CookError(f"{fname}: more than {MAX_DEF_LINES} lines")
        s = raw.split("//", 1)[0].strip()
        if not s:
            continue
        m = _DEF_RE[fname].fullmatch(s)
        if not m:
            raise CookError(f"{fname}:{ln}: not a definition line: {raw.strip()!r}")
        ident = m.group(1) if fname != "res.def" else m.group(2)
        if fname != "lightstyles.def":
            if ident in idents:
                raise CookError(f"{fname}:{ln}: duplicate {ident}")
            idents.add(ident)
        out.append((ln, m.groups()))
    return out


# --------------------------------------------------------------------------
# sentences.txt (the player's sound/sentences.txt)

@dataclass
class Sentence:
    name: str
    line: int
    words: List[Tuple[str, Dict[str, int]]]    # (dir/word, modifiers)


_MODS = {"p": "pitch", "v": "volume", "s": "start", "e": "end", "t": "time"}
_WORD_RE = re.compile(r"[A-Za-z0-9_!\-]+")


def _parse_mods(body: str, where: str) -> Dict[str, int]:
    mods: Dict[str, int] = {}
    for tok in body.split():
        m = re.fullmatch(r"([pvset])([0-9]{1,3})", tok)
        if not m:
            raise CookError(f"{where}: bad modifier {tok!r}")
        mods[_MODS[m.group(1)]] = int(m.group(2))
    return mods


def parse_sentence_line(name: str, text: str, where: str) -> List[Tuple[str, Dict[str, int]]]:
    """Words of one sentence: (dir/word, modifiers). A '(...)' group
    standing alone sets defaults for the following words; one attached to a
    word applies to that word only; ',' and '.' are the pause words _comma
    and _period of the current directory. A stray ')' is ignored (the
    retail file has one, WILD8)."""
    words: List[Tuple[str, Dict[str, int]]] = []
    cur_dir = "vox"
    defaults: Dict[str, int] = {}
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace() or c == ")":
            i += 1
            continue
        if c == "(":
            end = text.find(")", i)
            if end < 0:
                raise CookError(f"{where}: unterminated '('")
            defaults = {**defaults, **_parse_mods(text[i + 1:end], where)}
            i = end + 1
            continue
        if c in ",.":
            words.append((f"{cur_dir}/{'_comma' if c == ',' else '_period'}", dict(defaults)))
            i += 1
            continue
        j = i
        while j < n and not text[j].isspace() and text[j] not in "(),.":
            j += 1
        tok = text[i:j]
        i = j
        if "/" in tok:
            d, _, tok = tok.rpartition("/")
            if not re.fullmatch(r"[A-Za-z0-9_\-]+", d):
                raise CookError(f"{where}: bad directory {d!r}")
            cur_dir = d
        if not tok:
            continue
        if not _WORD_RE.fullmatch(tok):
            raise CookError(f"{where}: bad word {tok!r}")
        mods = dict(defaults)
        if i < n and text[i] == "(":
            end = text.find(")", i)
            if end < 0:
                raise CookError(f"{where}: unterminated '('")
            mods.update(_parse_mods(text[i + 1:end], where))
            i = end + 1
        words.append((f"{cur_dir}/{tok}", mods))
    if not words:
        raise CookError(f"{where}: sentence {name} has no words")
    return words


def read_sentences(data: bytes) -> List[Sentence]:
    if len(data) > MAX_SENTENCE_FILE:
        raise CookError("sentences.txt: larger than 1 MiB")
    out: List[Sentence] = []
    for ln, raw in enumerate(data.decode("latin-1").splitlines(), 1):
        s = raw.strip()
        if not s or s.startswith("//"):
            continue
        parts = s.split(None, 1)
        name = parts[0]
        if not re.fullmatch(r"[A-Za-z0-9_]+", name) or len(parts) < 2:
            raise CookError(f"sentences.txt:{ln}: bad sentence line")
        out.append(Sentence(name, ln, parse_sentence_line(
            name, parts[1], f"sentences.txt:{ln}")))
    return out


def group_name(sentence: str) -> str:
    """HLSDK grouping: a sentence belongs to the group named by its name
    without trailing digits; consecutive same-named sentences form a group."""
    return sentence.rstrip("0123456789") or sentence


# --------------------------------------------------------------------------
# titles.txt (the player's titles.txt as staged: env_message / chapter titles)

MAX_TITLES_FILE = 1 << 20


def read_titles(data: bytes) -> Tuple[List[rtb.Title], List[str]]:
    """Messages in file order (title id = index) and their names. `$`
    directives set the parameters of the messages after them (GoldSrc
    client_textmessage_t: $position x y, $effect n, $color / $color2
    r g b [a], $fadein, $fadeout, $holdtime, $fxtime); a message is a name
    line, `{`, text lines, `}`; `//` lines are comments. A repeated name
    keeps the first message."""
    if len(data) > MAX_TITLES_FILE:
        raise CookError("titles.txt: larger than 1 MB")
    lines = data.decode("latin-1").splitlines()
    cur = rtb.Title()
    titles: List[rtb.Title] = []
    names: List[str] = []
    seen: Set[str] = set()
    i = 0
    name: Optional[str] = None

    def num(v: str, where: str) -> float:
        try:
            f = float(v)
        except ValueError:
            raise CookError(f"{where}: {v!r} is not a number") from None
        if not abs(f) < rtb.TITLE_VALUE_MAX:
            raise CookError(f"{where}: {v!r} out of range")
        return f

    while i < len(lines):
        s = lines[i].strip()
        where = f"titles.txt:{i + 1}"
        i += 1
        if not s or s.startswith("//"):
            continue
        if s.startswith("$"):
            w = s.split()
            d, args = w[0].lower(), w[1:]
            if d == "$position" and len(args) == 2:
                cur.x, cur.y = num(args[0], where), num(args[1], where)
            elif d == "$effect" and len(args) == 1:
                cur.effect = int(num(args[0], where))
                if not 0 <= cur.effect <= 2:
                    raise CookError(f"{where}: effect {cur.effect} is not 0-2")
            elif d in ("$color", "$color2") and len(args) in (3, 4):
                c = tuple(int(num(a, where)) for a in args) + ((0,) if len(args) == 3 else ())
                if any(not 0 <= v <= 255 for v in c):
                    raise CookError(f"{where}: colour out of range")
                if d == "$color":
                    cur.color1 = c
                else:
                    cur.color2 = c
            elif d in ("$fadein", "$fadeout", "$holdtime", "$fxtime") and len(args) == 1:
                setattr(cur, d[1:], num(args[0], where))
            else:
                raise CookError(f"{where}: unknown or malformed directive {s!r}")
            continue
        if s.startswith("{"):
            if name is None:
                raise CookError(f"{where}: '{{' without a message name")
            body: List[str] = []
            while i < len(lines) and not lines[i].strip().startswith("}"):
                body.append(lines[i].rstrip("\r"))
                i += 1
            if i >= len(lines):
                raise CookError(f"{where}: message {name} has no closing '}}'")
            i += 1
            text = "\n".join(body).encode("latin-1")
            if len(text) > rtb.TITLE_TEXT_MAX or b"\0" in text:
                raise CookError(f"{where}: message {name} longer than "
                                f"{rtb.TITLE_TEXT_MAX} bytes")
            if name.upper() not in seen:
                seen.add(name.upper())
                titles.append(rtb.Title(cur.effect, cur.color1, cur.color2, cur.x, cur.y,
                                        cur.fadein, cur.fadeout, cur.holdtime, cur.fxtime,
                                        text))
                names.append(name)
            name = None
            continue
        if name is not None or len(s.split()) != 1:
            raise CookError(f"{where}: expected a message name, then '{{'")
        name = s
    if name is not None:
        raise CookError(f"titles.txt: message {name} has no text")
    if len(titles) > 0xFFFF:
        raise CookError("titles.txt: more than 65535 messages")
    return titles, names

# --------------------------------------------------------------------------
# the cook

class Content:
    """Case-insensitive index of the staged content directory."""

    def __init__(self, root: str):
        self.root = root
        self.by_lower: Dict[str, List[str]] = {}
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            for fn in sorted(filenames):
                rel = os.path.relpath(os.path.join(dirpath, fn), root).replace(os.sep, "/")
                self.by_lower.setdefault(rel.lower(), []).append(rel)

    def find(self, rel: str) -> Optional[str]:
        hits = self.by_lower.get(rel.lower())
        if not hits:
            return None
        if len(hits) > 1:
            raise CookError(f"content: {rel} matches several files differing in case: {hits}")
        return hits[0]


def _norm(name: str) -> str:
    """lower case, "/" separators, no leading "/" (map keys write
    "/models/mbarrel.mdl" for models/mbarrel.mdl)"""
    return name.strip().replace("\\", "/").lstrip("/").lower()


def res_kind_of_value(value: str) -> Optional[Tuple[int, str]]:
    """(kind, normalised name) when an entity key value names a resource."""
    v = _norm(value)
    if v.endswith(".mdl") and not v.startswith("*"):
        return rtb.KIND_MODEL, v
    if v.endswith(".spr"):
        return rtb.KIND_SPRITE, v
    if v.endswith(".wav"):
        return rtb.KIND_SOUND, v.lstrip("*")
    return None


def cart_file_of(kind: int, name: str) -> str:
    """Staged file holding a resource (rtb-format.md "Resource files"):
    MODEL / ANIM of a cooked studio model are models/<x>.s64 / .a64
    (studio-format.md); a MODEL without them is the raw MDL."""
    if kind == rtb.KIND_MODEL:
        return name
    if kind == rtb.KIND_ANIM:
        return name[:-4] + ".a64"
    if kind == rtb.KIND_SPRITE:
        return name[:-4] + ".sc4"
    return "sound/" + name + "64"     # x.wav -> sound/x.wav64


def event_resource(event: int, options: str) -> Optional[Tuple[int, str]]:
    """(kind, name) a studio event's option text names: a .wav sound, a
    !SENTENCE, or the sentence group of SCRIPT_EVENT_SENTENCE_RND1."""
    o = options.strip()
    if o.lower().endswith(".wav"):
        return rtb.KIND_SOUND, _norm(o).lstrip("*")
    if o.startswith("!") and len(o) > 1:
        return rtb.KIND_SENTENCE, o[1:].upper()
    if event == SCRIPT_EVENT_SENTENCE_RND1 and o:
        return rtb.KIND_GROUP, o.upper()
    return None


def cook(content_dir: str, defs_dir: str, map_names: Optional[List[str]] = None) -> rtb.Table:
    """The table of a staged content directory. With map_names (the HLE1 id
    step, tools/w64c/hleids.py, before the map packages exist) the id spaces
    only: no campaign graph, no map packages in the file table; ids depend on
    names alone, so they equal the final table's."""
    content = Content(content_dir)
    t = rtb.Table()

    def need(rel: str, why: str) -> str:
        hit = content.find(rel)
        if hit is None:
            raise CookError(f"{why}: no file {rel} in the content directory")
        return hit

    # maps
    if map_names is None:
        cwg_path = need("maps/campaign.cwg", "campaign graph")
        with open(os.path.join(content_dir, cwg_path), "rb") as f:
            cwg = f.read()
        t.cwg_crc = zlib.crc32(cwg) & 0xFFFFFFFF
        t.map_names = read_cwg_maps(cwg)
    else:
        t.map_names = sorted(map_names)

    refs: Dict[Tuple[int, str], str] = {}          # (kind, name) -> first referrer
    sentence_refs: Dict[str, str] = {}            # upper-case name -> referrer
    group_refs: Dict[str, str] = {}
    patterns_map: Set[str] = set()
    globals_: Set[str] = set()
    targets: List[Tuple[int, str, List[int], int]] = []

    for mi, mname in enumerate(t.map_names):
        ent_rel = need(f"maps/{mname}.ent", f"map {mname}")
        with open(os.path.join(content_dir, ent_rel), "rb") as f:
            ents = parse_entities(f.read(MAX_ENTS_TEXT + 1), ent_rel)
        names: Dict[str, List[int]] = {}
        spawned: Dict[str, List[int]] = {}     # monstermaker netname -> spawners
        for lump, kvs in enumerate(ents):
            is_maker = ("classname", "monstermaker") in kvs
            # speaker message: a sentence group name (HLSDK CSpeaker, unless
            # preset chooses a built-in group)
            is_speaker = ("classname", "speaker") in kvs and not any(
                k == "preset" and v not in ("", "0") for k, v in kvs)
            for key, value in kvs:
                where = f"{mname}:{lump} {key}"
                if key == "targetname" and value:
                    if lump not in names.setdefault(value, []):
                        names[value].append(lump)
                elif key == "netname" and value and is_maker:
                    if lump not in spawned.setdefault(value, []):
                        spawned[value].append(lump)
                elif key == "globalstate" and value:
                    globals_.add(value)
                elif key == "pattern" and value:
                    if not re.fullmatch(r"[a-z]{1,%d}" % MAX_PATTERN, value):
                        raise CookError(f"{where}: pattern {value!r} is not 1-255 letters a-z")
                    patterns_map.add(value)
                elif key in SENTENCE_KEYS and value.startswith("!") and len(value) > 1:
                    sentence_refs.setdefault(value[1:].upper(), where)
                elif (key == "message" and is_speaker or key in GROUP_KEYS) and value:
                    group_refs.setdefault(value.upper(), where)
                else:
                    rk = res_kind_of_value(value)
                    if rk:
                        refs.setdefault(rk, where)
        for name in sorted(set(names) | set(spawned)):
            sp = spawned.get(name, [])
            targets.append((mi, name, names.get(name, []) + sp, len(sp)))

    # a sound a map key names that the player's data lacks (the stage warned:
    # tools/cook/stage.py stage_sounds) is absent, as GoldSrc plays nothing
    # for it: no id, the key's HLE1 id is 0xFFFF (RES_NONE at run time)
    for rk in sorted(k for k in refs if k[0] == rtb.KIND_SOUND):
        if not content.find(cart_file_of(*rk)):
            print(f"restab: note: {refs[rk]}: no {cart_file_of(*rk)}: absent, no id",
                  file=sys.stderr)
            del refs[rk]

    # definitions (game/defs arrives with game/ in M3; absent = no lines)
    if not os.path.isdir(defs_dir):
        print(f"restab: note: no definitions directory {defs_dir}: "
              f"no classes, game resources or standard lightstyles", file=sys.stderr)
    t.class_names = [g[1] for _, g in read_defs(defs_dir, "classes.def")]
    if len(set(t.class_names)) != len(t.class_names):
        raise CookError("classes.def: duplicate classname")
    res_lines = read_defs(defs_dir, "res.def")
    kind_by_word = {"MODEL": rtb.KIND_MODEL, "SPRITE": rtb.KIND_SPRITE,
                    "SOUND": rtb.KIND_SOUND, "SENTENCE": rtb.KIND_SENTENCE,
                    "GROUP": rtb.KIND_GROUP}
    def_keys: List[Tuple[int, str]] = []
    def_where: Dict[Tuple[int, str], str] = {}
    for ln, (kw, _ident, value) in res_lines:
        kind = kind_by_word[kw]
        where = f"res.def:{ln}"
        if kind in (rtb.KIND_SENTENCE, rtb.KIND_GROUP):
            name = value.upper()
            (sentence_refs if kind == rtb.KIND_SENTENCE else group_refs).setdefault(name, where)
        else:
            rk = res_kind_of_value(value)
            if rk is None or rk[0] != kind:
                raise CookError(f"{where}: {value!r} is not a {kw} name")
            name = rk[1]
            refs.setdefault(rk, where)
        def_keys.append((kind, name))
        def_where.setdefault((kind, name), where)
    style_lines = read_defs(defs_dir, "lightstyles.def")

    # cooked studio models (tools/studio, studio-format.md): every model has
    # models/<x>.s64 + .a64, or none has (raw MDL tables, no ANIM ids). Their
    # events' option texts name sounds, sentences and groups: staged ones
    # become resources, and their ids are written into the .s64 below.
    model_list = sorted(n for k, n in refs if k == rtb.KIND_MODEL)
    cooked: Dict[str, str] = {}
    for name in model_list:
        rel = content.find(name[:-4] + ".s64")
        if rel:
            cooked[name] = rel
    if cooked and len(cooked) != len(model_list):
        raise CookError("studio: models without a cooked .s64: "
                        + ", ".join(n for n in model_list if n not in cooked))
    studio_events: Dict[str, Tuple[bytes, List[Tuple[int, Tuple[int, str]]]]] = {}
    soft_sentences: Set[str] = set()
    soft_groups: Set[str] = set()
    for name, rel in cooked.items():
        with open(os.path.join(content_dir, rel), "rb") as f:
            data = f.read()
        try:
            info = s64.read_model(data)
        except (s64.S64Error, struct.error) as e:
            raise CookError(f"{rel}: {e}") from None
        evs = []
        for ev in info.events:
            er = event_resource(ev.event, ev.options)
            if er is None:
                continue
            if er[0] == rtb.KIND_SOUND:
                if not content.find(cart_file_of(rtb.KIND_SOUND, er[1])):
                    continue
                refs.setdefault(er, f"{name}: event {ev.event}")
            elif er[0] == rtb.KIND_SENTENCE:
                soft_sentences.add(er[1])
            else:
                soft_groups.add(er[1])
            evs.append((ev.res_at, er))
        studio_events[name] = (data, evs)
        refs.setdefault((rtb.KIND_ANIM, name), refs[(rtb.KIND_MODEL, name)])

    # sentences: those referenced plus every member of a referenced group
    sent_path = content.find("sound/sentences.txt")
    all_sent: List[Sentence] = []
    if sent_path:
        with open(os.path.join(content_dir, sent_path), "rb") as f:
            all_sent = read_sentences(f.read(MAX_SENTENCE_FILE + 1))
    by_name: Dict[str, Sentence] = {}
    for s in all_sent:
        by_name.setdefault(s.name.upper(), s)
    for name in sorted(soft_sentences):
        if name in by_name:
            sentence_refs.setdefault(name, "studio event")
    for name in sorted(soft_groups):
        if any(group_name(s.name).upper() == name for s in all_sent):
            group_refs.setdefault(name, "studio event")
    keep: List[Sentence] = []
    dropped_sentences: Set[str] = set()
    for s in all_sent:
        if s.name.upper() in sentence_refs or group_name(s.name).upper() in group_refs:
            if by_name[s.name.upper()] is s:
                # GoldSrc skips a word whose sample is missing: drop it, and
                # the sentence when no word remains (its group skips it)
                words = []
                for word, mods in s.words:
                    if content.find(cart_file_of(rtb.KIND_SOUND, _norm(word) + ".wav")):
                        words.append((word, mods))
                    else:
                        print(f"restab: warning: sentences.txt:{s.line} {s.name}: word {word} "
                              f"has no sound/{_norm(word)}.wav64: dropped", file=sys.stderr)
                if not words:
                    print(f"restab: warning: sentences.txt:{s.line} {s.name}: no word left: "
                          f"sentence dropped", file=sys.stderr)
                    dropped_sentences.add(s.name.upper())
                    continue
                keep.append(Sentence(s.name, s.line, words))

    # a sentence or group only maps (or studio events) name that the
    # player's sentences.txt lacks is absent, as one whose words are all
    # missing (the stage warned): no id; res.def names stay required
    for name, where in sorted(sentence_refs.items()):
        if name not in by_name:
            if (rtb.KIND_SENTENCE, name) in def_where:
                raise CookError(f"{def_where[(rtb.KIND_SENTENCE, name)]}: sentence {name} "
                                f"not in sound/sentences.txt")
            print(f"restab: note: {where}: sentence {name} not in sound/sentences.txt: "
                  f"absent, no id", file=sys.stderr)
    groups_present = {group_name(s.name).upper() for s in keep}
    groups_listed = {group_name(s.name).upper() for s in all_sent}
    for name, where in sorted(group_refs.items()):
        if name not in groups_listed:
            if (rtb.KIND_GROUP, name) in def_where:
                raise CookError(f"{def_where[(rtb.KIND_GROUP, name)]}: sentence group {name} "
                                f"not in sound/sentences.txt")
            print(f"restab: note: {where}: sentence group {name} not in sound/sentences.txt: "
                  f"absent, no id", file=sys.stderr)
        elif name not in groups_present:
            print(f"restab: warning: {where}: sentence group {name}: no sentence left "
                  f"(missing sounds): no id", file=sys.stderr)
    for kind, name in def_keys:
        if (kind == rtb.KIND_SENTENCE and name in dropped_sentences) or \
                (kind == rtb.KIND_GROUP and name not in groups_present):
            raise CookError(f"res.def: {rtb.KIND_NAMES[kind]} {name}: none of its words has a "
                            f"staged sound")
    for s in keep:
        for word, _ in s.words:
            refs.setdefault((rtb.KIND_SOUND, _norm(word) + ".wav"),
                            f"sentences.txt:{s.line} {s.name}")

    # resources: deterministic order kind, then name
    model_names = {n for k, n in refs if k == rtb.KIND_MODEL}
    files: List[rtb.FileEntry] = []
    file_index: Dict[str, int] = {}

    def add_file(rel: str) -> int:
        if rel in file_index:
            raise CookError(f"content: {rel} used by two table entries")
        full = os.path.join(content_dir, rel)
        with open(full, "rb") as f:
            data = f.read()
        if len(data) > 0xFFFFFFFF:
            raise CookError(f"content: {rel} larger than 4 GiB")
        file_index[rel] = len(files)
        files.append(rtb.FileEntry(rel, len(data), zlib.crc32(data) & 0xFFFFFFFF))
        return file_index[rel]

    if map_names is None:
        for mname in t.map_names:
            t.map_files.append(add_file(need(f"maps/{mname}.w64", f"map {mname}")))

    # ids are fixed by name order before any file is added, so the studio
    # events' ids can be written into their .s64 first (idempotent)
    sound_ids = {n: rtb.res_t(rtb.KIND_SOUND, i) for i, n in
                 enumerate(sorted(n for k, n in refs if k == rtb.KIND_SOUND))}
    sent_ids = {s.name.upper(): rtb.res_t(rtb.KIND_SENTENCE, i) for i, s in enumerate(keep)}
    group_ids: Dict[str, int] = {}
    nruns = 0
    gprev = None
    for s in keep:          # one group per run of equal group names (SGRP)
        g = group_name(s.name).upper()
        if g != gprev:
            group_ids.setdefault(g, rtb.res_t(rtb.KIND_GROUP, nruns))
            nruns += 1
            gprev = g
    for name, (data, evs) in studio_events.items():
        patched = bytearray(data)
        for at, (kind, rname) in evs:
            r = {rtb.KIND_SOUND: sound_ids, rtb.KIND_SENTENCE: sent_ids,
                 rtb.KIND_GROUP: group_ids}[kind].get(rname, 0)
            struct.pack_into(">H", patched, at, r)
        if patched != data:
            with open(os.path.join(content_dir, cooked[name]), "wb") as f:
                f.write(patched)

    res_id: Dict[Tuple[int, str], int] = {}
    for kind in rtb.FILE_KINDS:
        names = sorted(n for k, n in refs if k == kind)
        if len(names) > rtb.INDEX_MAX + 1:
            raise CookError(f"more than 8192 {rtb.KIND_NAMES[kind]} resources")
        for idx, name in enumerate(names):
            if kind == rtb.KIND_MODEL and name in cooked:
                rel = cooked[name]
            else:
                rel = need(cart_file_of(kind, name), refs[(kind, name)])
            flags = 0
            fi = add_file(rel)
            if kind == rtb.KIND_MODEL and name not in cooked:
                base = name[:-4]
                companions = []
                tex = content.find(base + "t.mdl")
                if tex:
                    if (base + "t.mdl") in model_names:
                        raise CookError(f"{refs[(kind, name)]}: {base}t.mdl is both a model "
                                        f"and {name}'s texture file")
                    companions.append(tex)
                    flags |= rtb.RES_F_EXTTEX
                nseq = 0
                while nseq < MAX_SEQGROUPS:
                    seq = content.find(f"{base}{nseq + 1:02d}.mdl")
                    if not seq or f"{base}{nseq + 1:02d}.mdl" in model_names:
                        break
                    companions.append(seq)
                    nseq += 1
                flags |= nseq << rtb.RES_F_SEQ_SHIFT
                for c in companions:
                    add_file(c)
            t.res.append(rtb.ResEntry(kind, flags, fi, 0, files[fi].size))
            t.res_names.append(name)
            res_id[(kind, name)] = rtb.res_t(kind, idx)
    t.files = files
    if len(files) > 0xFFFF:
        raise CookError("more than 65535 files")

    # sentence and group tables (sentence ids follow sentences.txt order)
    for s in keep:
        first = len(t.words)
        if len(s.words) > 255:
            raise CookError(f"sentences.txt:{s.line}: more than 255 words")
        for word, mods in s.words:
            w = rtb.Word(res_id[(rtb.KIND_SOUND, _norm(word) + ".wav")], **mods)
            if not (1 <= w.pitch <= 255 and w.volume <= 255 and w.start <= 100
                    and w.end <= 100 and w.time <= 100):
                raise CookError(f"sentences.txt:{s.line}: modifier out of range")
            t.words.append(w)
        t.sentences.append((first, len(s.words)))
        t.sentence_names.append(s.name.upper())
        g = group_name(s.name).upper()
        if t.group_names and t.group_names[-1] == g:
            f0, c0 = t.groups[-1]
            t.groups[-1] = (f0, c0 + 1)
        else:
            t.groups.append((len(t.sentences) - 1, 1))
            t.group_names.append(g)
    if len(t.sentences) > rtb.INDEX_MAX + 1 or len(t.words) > 0xFFFF:
        raise CookError("too many sentences or sentence words")

    # definitions -> res_t
    for kind, name in def_keys:
        if kind in rtb.FILE_KINDS:
            t.defs.append(res_id[(kind, name)])
        else:
            r = t.res_of(kind, name)
            assert r is not None
            t.defs.append(r)

    # lightstyle patterns: definition lines first (their order is their id),
    # then every map pattern not already present, sorted
    seen_styles: Set[int] = set()
    pat_ids: Dict[str, int] = {}
    for ln, (num, pat) in style_lines:
        n = int(num)
        if n > 63 or n in seen_styles:
            raise CookError(f"lightstyles.def:{ln}: style {n} out of range or repeated")
        if not re.fullmatch(r"[a-z]{1,%d}" % MAX_PATTERN, pat):
            raise CookError(f"lightstyles.def:{ln}: pattern is not 1-255 letters a-z")
        seen_styles.add(n)
        pat_ids.setdefault(pat, len(t.patterns))
        t.patterns.append(bytes(ord(c) - 97 for c in pat))
    for pat in sorted(patterns_map):
        if pat not in pat_ids:
            pat_ids[pat] = len(t.patterns)
            t.patterns.append(bytes(ord(c) - 97 for c in pat))
    if len(t.patterns) > 0xFFFF:
        raise CookError("more than 65535 lightstyle patterns")

    for mi, name, lumps, spawners in targets:
        t.targets.append((mi, lumps, spawners))
        t.target_names.append(name)
    t.global_names = sorted(globals_)

    # titles: every message of the staged titles.txt, file order
    tpath = content.find("titles.txt")
    if tpath:
        with open(os.path.join(content_dir, tpath), "rb") as f:
            t.titles, t.title_names = read_titles(f.read(MAX_TITLES_FILE + 1))
    return t


# --------------------------------------------------------------------------
# the cart's fixed regions (architecture.md 4.3: cook-proved)

DEFAULT_LIMITS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "include", "hl64", "limits.h")


def read_limit(path: str, name: str) -> int:
    """A `#define NAME <int>` of include/hl64/limits.h."""
    with open(path, "r", encoding="utf-8") as f:
        m = re.search(r"^#define\s+" + re.escape(name) + r"\s+(\d+)\b", f.read(), re.M)
    if not m:
        raise CookError(f"{path}: no #define {name}")
    return int(m.group(1))


def engine_table_bytes(t: rtb.Table) -> int:
    """Bytes engine/res/res.c res_install takes in the RESTAB region for this
    table: the same allocations in the same order, each rounded up to 4."""
    def a4(n: int) -> int:
        return (n + 3) & ~3
    nppol = sum(len(p) for p in t.patterns)
    nttxt = sum(len(x.text) + 1 for x in t.titles)
    return sum(a4(n) for n in (
        8 * len(t.files), 2 * len(t.map_files), 12 * len(t.res), 4 * len(t.sentences),
        8 * len(t.words), 4 * len(t.groups), 4 * len(t.patterns), nppol, 2 * len(t.defs),
        40 * len(t.titles), nttxt, (len(t.res) + len(t.sentences) + len(t.groups) + 7) // 8,
        2 * t.kind_count(rtb.KIND_SPRITE)))


def cart_part_bytes(data: bytes) -> int:
    """Bytes res_init reads into the scratch region at boot: the file up to
    the first test section (TARG)."""
    i = rtb.SECTION_INDEX["TARG"]
    return struct.unpack_from(">I", data, rtb.HEADER_SIZE + rtb.DIRENT_SIZE * i + 4)[0]


def check_limits(t: rtb.Table, data: bytes, limits: str) -> Tuple[int, int]:
    """Fail the cook when the engine's tables exceed HL64_RESTAB_KB or the
    cart part exceeds HL64_SCRATCH_KB; returns (tables, cart part) bytes."""
    tables, cart = engine_table_bytes(t), cart_part_bytes(data)
    restab_kb = read_limit(limits, "HL64_RESTAB_KB")
    scratch_kb = read_limit(limits, "HL64_SCRATCH_KB")
    if tables > restab_kb * 1024:
        raise CookError(f"engine tables {tables} B exceed HL64_RESTAB_KB {restab_kb} "
                        f"({restab_kb * 1024} B): raise it in {limits}")
    if cart > scratch_kb * 1024:
        raise CookError(f"cart part {cart} B exceeds HL64_SCRATCH_KB {scratch_kb} "
                        f"({scratch_kb * 1024} B, read there at boot): raise it in {limits}")
    return tables, cart


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--content", required=True, help="staged content directory")
    ap.add_argument("--defs", required=True, help="game/defs directory")
    ap.add_argument("-o", "--out", required=True, help="output campaign.rtb")
    ap.add_argument("--limits", default=DEFAULT_LIMITS,
                    help="include/hl64/limits.h (HL64_RESTAB_KB, HL64_SCRATCH_KB)")
    a = ap.parse_args(argv)
    try:
        t = cook(a.content, a.defs)
        data = rtb.write(t)
        build_id = rtb.read(data).build_id     # the writer's output must validate
        tables, cart = check_limits(t, data, a.limits)
    except (CookError, ValueError, OSError, rtb.RtbError) as e:
        print(f"restab: {e}", file=sys.stderr)
        return 1
    tmp = a.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, a.out)
    print(f"restab: {a.out}: {len(data)} B, build_id {build_id:08x}; "
          f"maps {len(t.map_names)} files {len(t.files)} models {t.kind_count(rtb.KIND_MODEL)} "
          f"sprites {t.kind_count(rtb.KIND_SPRITE)} sounds {t.kind_count(rtb.KIND_SOUND)} "
          f"sentences {len(t.sentences)} groups {len(t.groups)} patterns {len(t.patterns)} "
          f"targets {len(t.targets)} classes {len(t.class_names)} globals {len(t.global_names)}; "
          f"engine tables {tables} B, cart part {cart} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
