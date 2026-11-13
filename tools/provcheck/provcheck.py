#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""provcheck: the hl64 provenance gate (docs/PROVENANCE.md, architecture.md 17).

`check` scans every file git sees in the hl64 tree (tracked and untracked,
not ignored) and fails when
  1. code outside game/ matches Quake or Xash3D sources: token winnowing
     fingerprints, exact and identifier-normalised, plus comment word
     8-grams; game/ is compared the same way, but fingerprints that also
     occur in the Half-Life SDK are allowed there;
  2. engine/ uses a Quake/GoldSrc engine identifier;
  3. a code file lacks its licence line (SPDX MIT outside the HLSDK zones,
     Valve's HLSDK notice inside them) or carries a wrong one;
  4. a forbidden file is present (copied Quake/Xash file, QuakeC or its C
     translations, game data, ROMs, build outputs).
The forbidden sources are never stored: `build-db` reads local copies listed
in corpora.json and keeps only 48-bit hashes with a source location.
`scan` runs checks 1 on arbitrary paths (calibration), `controls` plants
known copies in scratch copies of the tree and verifies that check fails.
"""

import argparse
import array
import bisect
import collections
import fnmatch
import gzip
import hashlib
import glob
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DB = os.path.join(HERE, "db", "provcheck.db.gz")
DEFAULT_MANIFEST = os.path.join(HERE, "db", "manifest.json")
DEFAULT_CORPORA = os.path.join(HERE, "corpora.json")

MAGIC = b"HL64PC01"
MASK48 = (1 << 48) - 1
P61 = (1 << 61) - 1
BASE = 0x5BD1E9955BD1E995 % P61

F_QUAKE, F_XASH, F_HLSDK = 1, 2, 4

# Build parameters; `check` uses the values recorded in the DB header.
PARAMS = {
    "exact_k": 12, "exact_w": 6, "exact_min": 60,
    "norm_k": 24, "norm_w": 8, "norm_min": 100, "norm_distinct": 8,
    "norm_max_basenames": 8,
    "comment_k": 8, "comment_min": 10, "comment_max_basenames": 12,
    "comment_informative": 5,
    "region_gap": 8, "filehash_min_bytes": 64,
}

C_EXT = {".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hh", ".inl", ".s",
         ".S", ".def", ".qc", ".ld"}
SCRIPT_EXT = {".py", ".sh", ".mk", ".route", ".cmake"}
MAKE_NAMES = {"Makefile", "makefile", "GNUmakefile"}
NO_HEADER_EXT = {".patch"}

HLSDK_ZONES = ("game/", "tools/hlsdk/", "test/oracle/probe/")
HLSDK_ZONE_GLOBS = ("test/oracle/*.patch",)

FORBIDDEN_EXT = {
    ".bsp", ".mdl", ".spr", ".wad", ".pak", ".pk3", ".dem", ".sav", ".dat",
    ".z64", ".n64", ".v64", ".dfs", ".elf", ".o", ".a", ".so", ".dll",
    ".exe", ".w64", ".q64", ".wav64", ".t3dm", ".rtb", ".rbc",
}
FORBIDDEN_NAMES = {"progs.src", "progdefs.h", "progs.h", "progdefs.q1"}

# Quake/GoldSrc engine identifiers banned in engine/ regardless of the DB
# (task list, architecture.md 18.2).  Matching is case-sensitive.
CURATED_IDENTS = {
    "edict_t", "entvars_t", "globalvars_t", "qboolean", "cvar_t",
    "sizebuf_t", "model_t", "vec3_t", "trace_t", "link_t", "areanode_t",
    "moveclip_t", "hull_t", "mplane_t", "msurface_t", "mnode_t", "mleaf_t",
    "dclipnode_t", "client_t", "server_t", "server_static_t",
    "client_state_t", "client_static_t", "refdef_t", "entity_t",
    "cl_entity_t", "Z_Malloc", "Z_Free", "R_RecursiveWorldNode",
    "BoxOnPlaneSide", "VectorCopy", "VectorAdd", "VectorSubtract",
    "VectorScale", "VectorMA", "DotProduct", "CrossProduct", "AngleVectors",
    "pr_strings", "pr_global_struct", "host_frametime", "realtime",
}
IDENT_PREFIX_RE = re.compile(
    r"^(?:SV|CL|Host|Cvar|Cmd|Cbuf|Con|Hunk|Cache|Z|Mod|R|Draw|SCR|COM|MSG"
    r"|ED|PF|PR|SZ|S|V|M|D|VID|IN|Key|Sys|Sbar|NET|Netchan|GL|Q|Info|Mem"
    r"|FS|Image|Delta|CDAudio|Chase|SNDDMA|Studio|Pmove|PM)_(?:[A-Z][a-z]|[a-z])"
    r"|^pfn[A-Z]")
# hl64 module prefixes (architecture.md 18.2): names in this namespace are
# ours by convention and are not matched against the hashed name set.
HL64_PREFIXES = ("io_", "wld_", "col_", "phy_", "ent_", "res_", "snd_",
                 "ren_", "in_", "dbg_", "ui_", "sav_", "rt_", "prof_",
                 "sys_", "mem_", "hl64_")

# Lower-case snake_case names enter the hashed name set only under a Quake
# subsystem prefix (host_frametime, sv_player, r_refdef ...); plain words
# (leaf_count, run_case) are too generic to call Quake's.
QUAKE_LOWER_PREFIXES = {
    "host", "sv", "svs", "cl", "cls", "r", "d", "pr", "ed", "con", "key",
    "scr", "vid", "net", "com", "sb", "cvar", "cmd", "cbuf", "hunk", "cache",
    "z", "mod", "chase", "v", "s", "m", "q", "gl", "ref", "pm", "pmove",
    "progs", "edict", "entvars", "svc", "clc", "loadmodel", "currententity",
    "vec3", "box"}
THIRD_PARTY_RE = re.compile(r"^(?:SDL_|Mix_|gl[A-Z]|wgl[A-Z]|egl[A-Z]|al[A-Z]|AL_|ALC_|png_|jpeg_)")


def quake_style_name(n):
    """True for a defined name that identifies Quake/GoldSrc code: types
    and tags (`*_t`, `*_s`), mixed-case names, or snake_case names under a
    Quake subsystem prefix.  ALL-CAPS constants, hl64 module prefixes and
    third-party API names are excluded."""
    if len(n) < 4 or n.upper() == n or n.startswith(HL64_PREFIXES) or THIRD_PARTY_RE.match(n):
        return False
    if n.endswith(("_t", "_s")) or n != n.lower():
        return True
    return "_" in n and n.split("_", 1)[0] in QUAKE_LOWER_PREFIXES


C_KEYWORDS = set("""auto break case char const continue default do double else
enum extern float for goto if inline int long register restrict return short
signed sizeof static struct switch typedef union unsigned void volatile while
_Bool _Complex _Imaginary bool true false class public private protected
virtual template typename namespace using new delete operator this throw try
catch friend explicit mutable nullptr asm define include ifdef ifndef endif
elif undef pragma defined uint8_t uint16_t uint32_t uint64_t int8_t int16_t
int32_t int64_t size_t NULL""".split())

TOK_RE = re.compile(r"""
 (?P<ws>[ \t\r\f\v]+|\\\r?\n)
|(?P<nl>\n)
|(?P<lc>//[^\n]*)
|(?P<bc>/\*.*?(?:\*/|\Z))
|(?P<str>"(?:[^"\\\n]|\\.)*"?)
|(?P<chr>'(?:[^'\\\n]|\\.)*'?)
|(?P<id>[A-Za-z_]\w*)
|(?P<num>\.?\d(?:[eEpP][+-]|[\w.])*)
|(?P<op>>>=|<<=|\.\.\.|->|\+\+|--|<<|>>|[-+*/%&|^!=<>]=|&&|\|\||::|\#\#|\S)
""", re.X | re.S)
INCLUDE_RE = re.compile(r"^[ \t]*#[ \t]*include\b[^\n]*", re.M)
WORD_RE = re.compile(r"[a-z0-9]+")


# ---------------------------------------------------------------- lexing

def lex_c(text):
    """Tokens [(kind, text, line)] and comments [(text, line)] of C text."""
    text = INCLUDE_RE.sub("", text)
    toks, comments = [], []
    line = 1
    for m in TOK_RE.finditer(text):
        kind = m.lastgroup
        s = m.group()
        if kind == "nl":
            line += 1
            continue
        if kind == "ws":
            line += s.count("\n")
            continue
        if kind in ("lc", "bc"):
            comments.append((s[2:-2] if kind == "bc" else s[2:], line))
            line += s.count("\n")
            continue
        toks.append((kind, s, line))
    return toks, comments


PY_DOC_RE = re.compile(r'("""|\'\'\')(.*?)(\1|\Z)', re.S)
HASH_COMMENT_RE = re.compile(r"""(?:^|[\s;])#(?![!])(.*)$""", re.M)


def lex_script(text):
    """Comments [(text, line)] of shell/Python/make/route text."""
    out = []
    for m in HASH_COMMENT_RE.finditer(text):
        out.append((m.group(1), text.count("\n", 0, m.start()) + 1))
    for m in PY_DOC_RE.finditer(text):
        out.append((m.group(2), text.count("\n", 0, m.start()) + 1))
    out.sort(key=lambda c: c[1])
    return out


# licence notices and tool-generated boilerplate are not expression
SKIP_COMMENT_LINE_RE = re.compile(
    r"licen[cs]e|copyright|\(c\)|\bGPL\b|generated (include )?file|NO_DEPENDENCIES", re.I)


BREAK = "|"   # never produced by WORD_RE; no counted n-gram spans it


def comment_words(comments):
    """[(word, line)] in file order; a skipped notice line leaves a BREAK
    so that no n-gram joins the text around it."""
    words = []
    for text, line in comments:
        for i, ln in enumerate(text.split("\n")):
            if SKIP_COMMENT_LINE_RE.search(ln):
                if words and words[-1][0] != BREAK:
                    words.append((BREAK, line + i))
                continue
            for w in WORD_RE.findall(ln.lower()):
                words.append((w, line + i))
    return words


def comment_gram_ok(wtext, i, k, informative):
    gram = wtext[i:i + k]
    return BREAK not in gram and \
        sum(len(w) >= 3 and not w.isdigit() for w in gram) >= informative


# ---------------------------------------------------------------- hashing

_TH = {}


def thash(s):
    h = _TH.get(s)
    if h is None:
        h = int.from_bytes(hashlib.blake2b(s.encode("latin-1", "replace"),
                                           digest_size=8).digest(), "little") % P61
        _TH[s] = h
    return h


def name_hash(s):
    return int.from_bytes(hashlib.blake2b(b"id:" + s.encode(),
                                          digest_size=8).digest(), "little") & MASK48


def kgrams(seq, k):
    """48-bit rolling hashes of every k-gram of a string sequence."""
    n = len(seq)
    if n < k:
        return []
    th = [thash(s) for s in seq]
    bk = pow(BASE, k - 1, P61)
    h = 0
    for i in range(k):
        h = (h * BASE + th[i]) % P61
    out = [h & MASK48]
    for i in range(k, n):
        h = ((h - th[i - k] * bk) * BASE + th[i]) % P61
        out.append(h & MASK48)
    return out


def winnow(hs, w):
    """Indices chosen by winnowing (rightmost minimum per window of w)."""
    if not hs:
        return []
    if len(hs) < w:
        m = min(hs)
        return [max(i for i, h in enumerate(hs) if h == m)]
    dq = collections.deque()
    sel, last = [], -1
    for i, h in enumerate(hs):
        while dq and hs[dq[-1]] >= h:
            dq.pop()
        dq.append(i)
        if dq[0] <= i - w:
            dq.popleft()
        if i >= w - 1 and dq[0] != last:
            last = dq[0]
            sel.append(last)
    return sel


def norm_tok(kind, s):
    if kind == "id":
        return s if s in C_KEYWORDS else "I"
    if kind == "num":
        return "N"
    if kind in ("str", "chr"):
        return "S"
    return s


def token_streams(toks):
    exact = [t[1] for t in toks]
    norm = [norm_tok(t[0], t[1]) for t in toks]
    return exact, norm


DECL_OPS = {";", ",", "[", "]", "*", "{", "}", "(", ")", "#", "##"}
CTRL_KW = {"if", "else", "for", "while", "do", "switch", "case", "return",
           "goto", "break", "continue", "sizeof"}


def code_prefix(toks):
    """Prefix counts of statement tokens (operators other than declaration
    punctuation and unary signs, control keywords).  A k-gram without one is
    a declaration or data run -- struct field lists, prototypes, enum lists,
    number tables, i.e. interface layout or data, not expression (audit:
    "format mimicry") -- and is not fingerprinted."""
    out = [0]
    n = 0
    prev = ""
    for kind, s, _ in toks:
        if kind == "op" and s not in DECL_OPS:
            if not (s in ("-", "+") and prev in ("{", ",", "(", "[", "=")):
                n += 1
        elif kind == "id" and s in CTRL_KW:
            n += 1
        out.append(n)
        prev = s
    return out


def file_hash(data):
    stripped = re.sub(rb"\s+", b"", data)
    if len(stripped) < PARAMS["filehash_min_bytes"]:
        return None
    return int.from_bytes(hashlib.blake2b(stripped, digest_size=8).digest(),
                          "little") & MASK48


def path_hash(p):
    return name_hash("path:" + p.lower())


def defined_names(toks):
    """Names defined by a C file: macros, tags, typedefs, functions,
    file-scope variables, enum constants."""
    names = set()
    depth = 0
    pdepth = 0
    enum_levels = []
    n = len(toks)
    for i, (kind, s, _) in enumerate(toks):
        if kind == "op":
            if s == "{":
                depth += 1
                pdepth = 0
                j = i - 1
                if j >= 0 and toks[j][0] == "id" and j - 1 >= 0 and toks[j - 1][1] == "enum":
                    enum_levels.append(depth)
                elif j >= 0 and toks[j][1] == "enum":
                    enum_levels.append(depth)
            elif s == "}":
                if enum_levels and enum_levels[-1] == depth:
                    enum_levels.pop()
                depth = max(0, depth - 1)
                pdepth = 0
            elif s == "(":
                pdepth += 1
            elif s == ")":
                pdepth = max(0, pdepth - 1)
            elif s == ";":
                pdepth = 0
            continue
        if kind != "id":
            continue
        prev = toks[i - 1][1] if i > 0 else ""
        prevk = toks[i - 1][0] if i > 0 else ""
        nxt = toks[i + 1][1] if i + 1 < n else ""
        if prev == "define" and i > 1 and toks[i - 2][1] == "#":
            names.add(s)
        elif prev in ("struct", "union", "enum"):
            names.add(s)
        elif enum_levels and enum_levels[-1] == depth and prev in ("{", ",") \
                and nxt in (",", "=", "}"):
            names.add(s)
        elif depth == 0 and pdepth == 1 and prev == "*" and i > 1 and toks[i - 2][1] == "(" \
                and nxt == ")":
            names.add(s)
        elif depth == 0 and pdepth == 0:
            if nxt == "(" and (prevk == "id" or prev == "*") and prev not in C_KEYWORDS - {
                    "void", "int", "float", "double", "char", "short", "long",
                    "unsigned", "signed", "const", "static", "inline", "extern"}:
                names.add(s)
            elif nxt in (";", "=", "[", ",") and (prevk == "id" or prev in ("*", "}")):
                names.add(s)
    return names


# ---------------------------------------------------------------- corpora

def expand(p):
    return os.path.expanduser(os.path.expandvars(p))


def corpus_root(c):
    env = c.get("env")
    if env and os.environ.get(env):
        return os.environ[env]
    if c["path"] == "@hl64":
        return repo_root()
    return expand(c["path"])


def git_rev(path):
    r = subprocess.run(["git", "-C", path, "rev-parse", "HEAD"],
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def iter_dir(root, subdirs):
    for sub in subdirs or [""]:
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            raise SystemExit(f"provcheck: corpus directory missing: {base}")
        for dp, dns, fns in os.walk(base):
            dns[:] = sorted(d for d in dns if d != ".git")
            for fn in sorted(fns):
                fp = os.path.join(dp, fn)
                if os.path.isfile(fp) and not os.path.islink(fp):
                    with open(fp, "rb") as f:
                        yield os.path.relpath(fp, root), f.read()


def iter_git_rev(repo, rev):
    data = subprocess.run(["git", "-C", repo, "archive", "--format=tar", rev],
                          capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        for m in tf.getmembers():
            if m.isfile():
                yield m.name, tf.extractfile(m).read()


RANGE_RE = re.compile(r"(\d+)\s*[-\u2013]\s*(\d+)")
LABEL_RE = re.compile(r"Quake-tools|QUAKE|Quake|XASH|Xash|HLSDK|QCtrans|HLSDK/Xash|weak|low-confidence")


def audit_ranges(audit_text):
    """{path: (lines, [(lo, hi, flag)])} from the audit's CONTAINS-QUAKE /
    CONTAINS-XASH rows, plus {path: lines} for FORBIDDEN rows."""
    regions, forbidden = {}, {}
    row_re = re.compile(r"^\| ([^|]+?) \| (\d+) \| [^|]* \| (CQ|CX|FB) \| [^|]* \| ([^|]*) \|", re.M)
    for m in row_re.finditer(audit_text):
        path, lines, cls, cont = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        if cls == "FB":
            forbidden[path] = lines
            continue
        default = F_QUAKE if cls == "CQ" else F_XASH
        out = []
        if "/" in cont and re.search(r"\d+\s*/\s*\d+", cont) is None:
            # "Quake: a-b, ... / xash / hlsdk / qc" column form
            parts = cont.split(" / ")
            spans = [(parts[0], F_QUAKE)] + ([(parts[1], F_XASH)] if len(parts) > 1 else [])
        else:
            spans = [(cont, default)]
        for text, flag in spans:
            label = flag
            pos = 0
            for lm in list(LABEL_RE.finditer(text)) + [None]:
                end = lm.start() if lm else len(text)
                for rm in RANGE_RE.finditer(text, pos, end):
                    lo, hi = int(rm.group(1)), int(rm.group(2))
                    if label and 0 < lo <= hi <= lines:
                        out.append((lo, hi, label))
                if lm is None:
                    break
                word = lm.group()
                label = {"QUAKE": F_QUAKE, "Quake": F_QUAKE, "Quake-tools": F_QUAKE,
                         "XASH": F_XASH, "Xash": F_XASH, "HLSDK/Xash": F_XASH,
                         "HLSDK": 0, "QCtrans": F_QUAKE, "weak": label,
                         "low-confidence": label}[word]
                pos = lm.end()
        if out:
            regions[path] = (lines, out)
    return regions, forbidden


def load_corpus_files(c, hl64_root):
    """Yield (relpath, bytes, region list or None) for a corpus entry."""
    kind = c["kind"]
    root = corpus_root(c)
    if kind == "dir":
        if c.get("rev"):
            got = git_rev(root)
            if got != c["rev"]:
                raise SystemExit(f"provcheck: {c['name']} at {root} is {got}, corpora.json pins {c['rev']}")
        for rel, data in iter_dir(root, c.get("subdirs")):
            yield rel, data, None
    elif kind == "git":
        for rel, data in iter_git_rev(root, c["rev"]):
            yield rel, data, None
    elif kind == "glob":
        for pat in c["globs"]:
            for fp in sorted(glob.glob(os.path.join(root, pat))):
                if os.path.isfile(fp):
                    with open(fp, "rb") as f:
                        yield os.path.relpath(fp, root), f.read(), None
    elif kind == "audit":
        with open(os.path.join(hl64_root, c["audit"]), encoding="utf-8") as f:
            regions, forbidden = audit_ranges(f.read())
        for rel in sorted(set(regions) | set(forbidden)):
            fp = os.path.join(root, rel)
            if not os.path.isfile(fp):
                AUDIT_SKIPPED.append(f"{rel}: missing")
                continue
            with open(fp, "rb") as f:
                data = f.read()
            nlines = data.count(b"\n") + (0 if data.endswith(b"\n") else 1)
            want = regions[rel][0] if rel in regions else forbidden[rel]
            if nlines != want:
                AUDIT_SKIPPED.append(f"{rel}: {nlines} lines, audit snapshot {want}")
                continue
            yield rel, data, (regions[rel][1] if rel in regions else None)
    else:
        raise SystemExit(f"provcheck: unknown corpus kind {kind}")


AUDIT_SKIPPED = []


def ext_of(path):
    base = os.path.basename(path)
    if base in MAKE_NAMES:
        return ".mk"
    return os.path.splitext(base)[1]


# ---------------------------------------------------------------- build-db

class Section:
    def __init__(self):
        self.d = {}   # hash -> [flags, fileidx, line, set(basenames)]

    def add(self, h, flag, fidx, line, base=None):
        e = self.d.get(h)
        if e is None:
            self.d[h] = [flag, fidx, line, {base} if base is not None else None]
        else:
            e[0] |= flag
            if base is not None:
                e[3].add(base)


def build_db(args):
    hl64_root = repo_root()
    with open(args.corpora, encoding="utf-8") as f:
        cfg = json.load(f)
    p = PARAMS
    secs = {n: Section() for n in ("exact", "norm", "comment", "filehash", "path", "qcname")}
    ident_names = set()
    generic_names = set(C_KEYWORDS)
    allow = {"exact": set(), "norm": set(), "comment": set()}     # HLSDK
    generic = {"exact": set(), "norm": set(), "comment": set()}   # libdragon...
    hlsdk_stems = set()
    files = []
    qcnames = []
    repetitive = set()
    manifest_corpora = []
    for c in cfg["corpora"]:
        label = c["label"]
        flag = {"QUAKE": F_QUAKE, "XASH": F_XASH, "AUDIT": F_QUAKE}.get(label, 0)
        forbidden = flag != 0
        nfiles = nbytes = 0
        for rel, data, regions in load_corpus_files(c, hl64_root):
            ext = ext_of(rel)
            nfiles += 1
            nbytes += len(data)
            if label == "HLSDK":
                hlsdk_stems.add(os.path.splitext(os.path.basename(rel))[0].lower())
            if forbidden:
                fidx = len(files)
                files.append(f"{c['name']}:{rel}")
                if regions is None:
                    fh = file_hash(data)
                    if fh is not None:
                        secs["filehash"].add(fh, flag, fidx, 1)
                if c.get("paths") and ext in C_EXT:
                    secs["path"].add(path_hash(rel), flag, fidx, 1)
                if c.get("qcnames"):
                    qcnames.append((os.path.basename(rel), fidx))
            if label == "GENERIC-TEXT":
                # licence texts: their phrases are boilerplate wherever quoted
                wtext = [w for w, _ in comment_words([(data.decode("latin-1"), 1)])]
                generic["comment"].update(kgrams(wtext, p["comment_k"]))
                continue
            if ext not in C_EXT:
                continue
            text = data.decode("latin-1")
            toks, comments = lex_c(text)
            if label in ("GENERIC", "GENERIC-NAMES"):
                generic_names.update(t[1] for t in toks if t[0] == "id")
                if label == "GENERIC-NAMES":
                    continue
            if c.get("names"):
                ident_names.update(defined_names(toks))
            if regions is not None:
                keep = bytearray(max(t[2] for t in toks) + 2 if toks else 1)
                flags_at = {}
                for lo, hi, fl in regions:
                    for ln in range(lo, min(hi, len(keep) - 1) + 1):
                        keep[ln] = 1
                        flags_at[ln] = fl
                toks = [t for t in toks if t[2] < len(keep) and keep[t[2]]]
                comments = [cm for cm in comments if cm[1] < len(keep) and keep[cm[1]]]
                rflag = lambda ln: flags_at.get(ln, F_QUAKE)
            else:
                rflag = lambda ln: flag
            exact, norm = token_streams(toks)
            words = comment_words(comments)
            wtext = [w for w, _ in words]
            if label in ("HLSDK", "GENERIC"):
                tgt = allow if label == "HLSDK" else generic
                tgt["exact"].update(kgrams(exact, p["exact_k"]))
                tgt["norm"].update(kgrams(norm, p["norm_k"]))
                tgt["comment"].update(kgrams(wtext, p["comment_k"]))
                continue
            base = os.path.basename(rel).lower()
            code = code_prefix(toks)
            hs = kgrams(exact, p["exact_k"])
            for i in winnow(hs, p["exact_w"]):
                if code[i + p["exact_k"]] > code[i]:
                    secs["exact"].add(hs[i], rflag(toks[i][2]), fidx, toks[i][2])
            hs = kgrams(norm, p["norm_k"])
            seen_here = collections.Counter()
            for i in winnow(hs, p["norm_w"]):
                if code[i + p["norm_k"]] > code[i] and \
                        len(set(norm[i:i + p["norm_k"]])) >= p["norm_distinct"]:
                    seen_here[hs[i]] += 1
                    secs["norm"].add(hs[i], rflag(toks[i][2]), fidx, toks[i][2], base)
            repetitive.update(h for h, n in seen_here.items() if n > 1)
            hs = kgrams(wtext, p["comment_k"])
            for i, h in enumerate(hs):
                if comment_gram_ok(wtext, i, p["comment_k"], p["comment_informative"]):
                    secs["comment"].add(h, rflag(words[i][1]), fidx, words[i][1], base)
        manifest_corpora.append({
            "name": c["name"], "label": label, "kind": c["kind"],
            "path": c["path"], "env": c.get("env"), "rev": c.get("rev"),
            "subdirs": c.get("subdirs"), "files": nfiles, "bytes": nbytes})
        print(f"provcheck: corpus {c['name']}: {nfiles} files, {nbytes} bytes", file=sys.stderr)
    # comment n-grams shared by many unrelated files are boilerplate;
    # normalised k-grams that repeat inside one file (field-by-field
    # copies, if/else ladders) or recur across many unrelated files are
    # idioms, not expression
    sec = secs["comment"]
    for h in [h for h, e in sec.d.items() if len(e[3]) >= p["comment_max_basenames"]]:
        del sec.d[h]
    sec = secs["norm"]
    for h in [h for h, e in sec.d.items()
              if h in repetitive or len(e[3]) >= p["norm_max_basenames"]]:
        del sec.d[h]
    dropped = {}
    for name in ("exact", "norm", "comment"):
        sec = secs[name]
        g = [h for h in sec.d if h in generic[name]]
        for h in g:
            del sec.d[h]
        dropped[name] = len(g)
        for h, e in sec.d.items():
            if h in allow[name]:
                e[0] |= F_HLSDK
    # QuakeC-translation file names, minus names that collide with an HLSDK
    # file stem (game.c vs game.cpp); the content checks still cover those
    for base, fidx in qcnames:
        if os.path.splitext(base)[0].lower() not in hlsdk_stems:
            secs["qcname"].add(path_hash(base), F_QUAKE, fidx, 1)
    names = sorted(n for n in ident_names if quake_style_name(n) and n not in generic_names)
    secs["ident"] = Section()
    for n in names:
        secs["ident"].add(name_hash(n), F_QUAKE, 0, 0)
    for n in sorted(CURATED_IDENTS):
        secs["ident"].add(name_hash(n), F_QUAKE, 0, 0)
    header = {"params": p, "files": files, "sections": []}
    blobs = []
    order = ("exact", "norm", "comment", "filehash", "path", "qcname", "ident")
    for name in order:
        items = sorted(secs[name].d.items())
        hs = array.array("Q", (h for h, _ in items))
        fl = bytes(e[0] for _, e in items)
        fi = array.array("H", (min(e[1], 65535) for _, e in items))
        ln = array.array("H", (min(e[2], 65535) for _, e in items))
        for a in (hs, fi, ln):
            if sys.byteorder != "little":
                a.byteswap()
        header["sections"].append({"name": name, "count": len(items)})
        blobs += [hs.tobytes(), fl, fi.tobytes(), ln.tobytes()]
    hj = json.dumps(header, sort_keys=True).encode()
    raw = MAGIC + len(hj).to_bytes(4, "little") + hj + b"".join(blobs)
    os.makedirs(os.path.dirname(args.db), exist_ok=True)
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0, compresslevel=9) as gz:
        gz.write(raw)
    out = buf.getvalue()
    with open(args.db, "wb") as f:
        f.write(out)
    manifest = {
        "db": os.path.basename(args.db),
        "db_sha256": hashlib.sha256(out).hexdigest(),
        "params": p,
        "corpora": manifest_corpora,
        "counts": {s["name"]: s["count"] for s in header["sections"]},
        "generic_dropped": dropped,
        "audit_rows_skipped": AUDIT_SKIPPED,
    }
    with open(args.manifest, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
    print(f"provcheck: wrote {args.db} ({len(out)} bytes) {manifest['counts']}", file=sys.stderr)
    return 0


# ---------------------------------------------------------------- DB load

class DB:
    def __init__(self, path, manifest_path):
        try:
            with open(manifest_path, encoding="utf-8") as f:
                manifest = json.load(f)
            with open(path, "rb") as f:
                blob = f.read()
        except OSError as e:
            raise SystemExit(f"provcheck: ERROR cannot read DB: {e}") from None
        if hashlib.sha256(blob).hexdigest() != manifest.get("db_sha256"):
            raise SystemExit("provcheck: ERROR DB sha256 does not match manifest.json")
        raw = gzip.decompress(blob)
        if raw[:8] != MAGIC:
            raise SystemExit("provcheck: ERROR bad DB magic")
        hl = int.from_bytes(raw[8:12], "little")
        header = json.loads(raw[12:12 + hl])
        self.params = header["params"]
        self.files = header["files"]
        off = 12 + hl
        self.sec = {}
        for s in header["sections"]:
            n = s["count"]
            hs = array.array("Q"); hs.frombytes(raw[off:off + 8 * n]); off += 8 * n
            fl = raw[off:off + n]; off += n
            fi = array.array("H"); fi.frombytes(raw[off:off + 2 * n]); off += 2 * n
            ln = array.array("H"); ln.frombytes(raw[off:off + 2 * n]); off += 2 * n
            if sys.byteorder != "little":
                for a in (hs, fi, ln):
                    a.byteswap()
            self.sec[s["name"]] = (hs, fl, fi, ln)
        if off != len(raw):
            raise SystemExit("provcheck: ERROR DB length mismatch")
        for name in ("exact", "norm", "comment", "filehash", "ident"):
            if not self.sec.get(name) or not len(self.sec[name][0]):
                raise SystemExit(f"provcheck: ERROR DB section {name} empty")

    def get(self, name, h):
        hs, fl, fi, ln = self.sec[name]
        i = bisect.bisect_left(hs, h)
        if i < len(hs) and hs[i] == h:
            return fl[i], fi[i], ln[i]
        return None


# ---------------------------------------------------------------- checks

def repo_root():
    r = subprocess.run(["git", "-C", HERE, "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("provcheck: ERROR not inside a git work tree")
    return r.stdout.strip()


def repo_files(root):
    r = subprocess.run(["git", "-C", root, "ls-files", "-z", "-co", "--exclude-standard"],
                       capture_output=True, check=True)
    return sorted(set(p.decode() for p in r.stdout.split(b"\0") if p))


def label_of(flags):
    out = []
    if flags & F_QUAKE:
        out.append("QUAKE")
    if flags & F_XASH:
        out.append("XASH")
    return "+".join(out) or "?"


def regions_of(matches, n, k, gap):
    """matches: list of (start, flags, fidx, refline).  Returns regions
    [(first, last, covered, flags, ref)] over a stream of n items."""
    cov = bytearray(n)
    for s, _, _, _ in matches:
        cov[s:s + k] = b"\1" * min(k, n - s)
    out = []
    i = 0
    while i < n:
        if not cov[i]:
            i += 1
            continue
        j = i
        last = i
        covered = 0
        while j < n and j - last <= gap + 1:
            if cov[j]:
                covered += 1
                last = j
            j += 1
        out.append((i, last, covered))
        i = last + 1
    res = []
    for first, last, covered in out:
        inside = [m for m in matches if first <= m[0] <= last]
        flags = 0
        refs = collections.Counter()
        for _, fl, fi, rl in inside:
            flags |= fl
            refs[fi] += 1
        fi, _ = refs.most_common(1)[0]
        rl = min(m[3] for m in inside if m[2] == fi)
        res.append((first, last, covered, flags, (fi, rl)))
    return res


def similarity(db, text, is_c, hlsdk_zone, comments=None):
    """Findings [(kind, line_lo, line_hi, size, flags, ref, snippet)]."""
    p = db.params
    found = []
    drop = F_HLSDK if hlsdk_zone else 0
    if is_c:
        toks, comments = lex_c(text)
        exact, norm = token_streams(toks)
        for mode, stream in (("exact", exact), ("norm", norm)):
            k = p[mode + "_k"]
            ms = []
            hs = kgrams(stream, k)
            # normalised k-grams that recur inside the file being checked
            # (field-by-field copies, case ladders) carry no provenance
            again = collections.Counter(hs) if mode == "norm" else {}
            for i, h in enumerate(hs):
                e = db.get(mode, h)
                if e and not (e[0] & drop) and e[0] & (F_QUAKE | F_XASH) and again.get(h, 1) == 1:
                    ms.append((i, e[0], e[1], e[2]))
            for first, last, cov, fl, ref in regions_of(ms, len(stream), k, p["region_gap"]):
                if cov >= p[mode + "_min"]:
                    found.append((mode, toks[first][2], toks[min(last, len(toks) - 1)][2],
                                  cov, fl, ref, ""))
    elif comments is None:
        comments = lex_script(text)
    words = comment_words(comments)
    k = p["comment_k"]
    ms = []
    wtext = [w for w, _ in words]
    for i, h in enumerate(kgrams(wtext, k)):
        if not comment_gram_ok(wtext, i, k, p["comment_informative"]):
            continue
        e = db.get("comment", h)
        if e and not (e[0] & drop) and e[0] & (F_QUAKE | F_XASH):
            ms.append((i, e[0], e[1], e[2]))
    for first, last, cov, fl, ref in regions_of(ms, len(words), k, 0):
        if cov >= p["comment_min"]:
            snippet = " ".join(w for w, _ in words[first:last + 1] if w != BREAK)
            found.append(("comment", words[first][1], words[last][1], cov, fl, ref,
                          f" | {snippet[:160]}"))
    return found


def in_hlsdk_zone(path):
    return path.startswith(HLSDK_ZONES) or any(fnmatch.fnmatch(path, g) for g in HLSDK_ZONE_GLOBS)


SPDX_MIT_RE = re.compile(r"SPDX-License-Identifier:\s*MIT\b")
VALVE_RE = re.compile(r"Copyright\s*\(c\)\s*[0-9 ,\-]*Valve", re.I)
WRONG_RE = re.compile(r"GNU General Public|\bGPL\b|quakegeneric contributors|Xash3D", re.I)
IDSOFT_RE = re.compile(r"id Software", re.I)
NOTICE_LINE_RE = re.compile(r"licen[cs]e|copyright|\(c\)|\bGPL\b", re.I)


def licence_findings(path, text):
    ext = ext_of(path)
    if ext in NO_HEADER_EXT or not (ext in C_EXT or ext in SCRIPT_EXT):
        return []
    head = "\n".join(text.split("\n")[:20])
    hz = in_hlsdk_zone(path)
    out = []
    notice = "\n".join(ln for ln in head.split("\n") if NOTICE_LINE_RE.search(ln))
    if WRONG_RE.search(notice):
        out.append("header names GPL / quakegeneric / Xash3D")
    if hz:
        if not VALVE_RE.search(head):
            out.append("HLSDK zone file lacks Valve's HLSDK notice (Copyright (c) 1996-2002, Valve LLC)")
        if SPDX_MIT_RE.search(head):
            out.append("HLSDK zone file carries an MIT licence line")
    else:
        if not SPDX_MIT_RE.search("\n".join(text.split("\n")[:10])):
            out.append("missing 'SPDX-License-Identifier: MIT' in the first 10 lines")
        if VALVE_RE.search(head) or IDSOFT_RE.search(notice):
            out.append("MIT file carries a Valve / id Software notice")
    return out


def forbidden_findings(db, path, data):
    out = []
    base = os.path.basename(path)
    ext = os.path.splitext(base)[1].lower()
    if ext in FORBIDDEN_EXT:
        out.append(f"forbidden file type {ext} (game data / ROM / cooked / build output)")
    if base in FORBIDDEN_NAMES or ext == ".qc" or path.startswith("qc/") or "/qc/" in path:
        out.append("QuakeC / progs file")
    if db.get("qcname", path_hash(base)):
        out.append("name of a quakegeneric QuakeC translation (source/g_*.c, game*.c)")
    e = db.get("path", path_hash(path))
    if e:
        out.append(f"path of a Quake file ({db.files[e[1]]})")
    fh = file_hash(data)
    if fh is not None:
        e = db.get("filehash", fh)
        if e:
            out.append(f"copy of {label_of(e[0])} file {db.files[e[1]]}")
    return out


def ident_findings(db, text):
    toks, _ = lex_c(text)
    seen = {}
    for kind, s, line in toks:
        if kind != "id" or s in seen:
            continue
        if s in CURATED_IDENTS or IDENT_PREFIX_RE.match(s) or \
                (not s.startswith(HL64_PREFIXES) and db.get("ident", name_hash(s))):
            seen[s] = line
    return sorted(seen.items(), key=lambda x: x[1])


def fmt_ref(db, ref):
    fi, line = ref
    return f"{db.files[fi]}:{line}"


def run_check(root, db):
    files = repo_files(root)
    if not files:
        print("provcheck: ERROR no files", file=sys.stderr)
        return 2
    findings = []
    nscan = 0
    for path in files:
        fp = os.path.join(root, path)
        if os.path.islink(fp) or not os.path.isfile(fp):
            continue
        with open(fp, "rb") as f:
            data = f.read()
        for msg in forbidden_findings(db, path, data):
            findings.append(f"FORBIDDEN-FILE {path}: {msg}")
        if b"\0" in data:
            continue
        text = data.decode("utf-8", "replace")
        for msg in licence_findings(path, text):
            findings.append(f"LICENCE {path}: {msg}")
        ext = ext_of(path)
        if ext not in C_EXT and ext not in SCRIPT_EXT:
            continue
        nscan += 1
        for kind, lo, hi, size, fl, ref, snip in similarity(db, text, ext in C_EXT, in_hlsdk_zone(path)):
            unit = "words" if kind == "comment" else "tokens"
            findings.append(f"SIMILAR-{kind.upper()} {path}:{lo}-{hi}: {size} {unit} match "
                            f"{label_of(fl)} ({fmt_ref(db, ref)}){snip}")
        if path.startswith("engine/") and ext in C_EXT:
            for name, line in ident_findings(db, text):
                findings.append(f"IDENT {path}:{line}: Quake/GoldSrc engine identifier {name}")
    for f in findings:
        print(f)
    print(f"provcheck: {len(files)} files, {nscan} scanned, {len(findings)} findings: "
          f"{'FAIL' if findings else 'PASS'}")
    return 1 if findings else 0


def cmd_check(args):
    db = DB(args.db, args.manifest)
    root = args.root or repo_root()
    return run_check(root, db)


def cmd_scan(args):
    """Similarity (and with --idents, identifier) report for arbitrary
    paths, as MIT-zone files; for calibration and audits."""
    db = DB(args.db, args.manifest)
    nfiles = nflag = 0
    for top in args.paths:
        walk = [(os.path.dirname(top), [], [os.path.basename(top)])] if os.path.isfile(top) else os.walk(top)
        for dp, dns, fns in walk:
            dns[:] = sorted(d for d in dns if d != ".git")
            for fn in sorted(fns):
                fp = os.path.join(dp, fn)
                ext = ext_of(fp)
                if ext not in C_EXT and ext not in SCRIPT_EXT:
                    continue
                with open(fp, "rb") as f:
                    data = f.read()
                if b"\0" in data:
                    continue
                text = data.decode("utf-8", "replace")
                nfiles += 1
                hits = similarity(db, text, ext in C_EXT, args.hlsdk)
                for kind, lo, hi, size, fl, ref, snip in hits:
                    print(f"SIMILAR-{kind.upper()} {fp}:{lo}-{hi}: {size} match "
                          f"{label_of(fl)} ({fmt_ref(db, ref)}){snip}")
                idents = ident_findings(db, text) if args.idents and ext in C_EXT else []
                for name, line in idents:
                    print(f"IDENT {fp}:{line}: {name}")
                nflag += bool(hits or idents)
    print(f"provcheck scan: {nfiles} files, {nflag} with findings")
    return 0


# ---------------------------------------------------------------- controls

def extract_function(text, name):
    """Source text of the first definition of C function `name`."""
    m = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text, re.M)
    if not m:
        raise SystemExit(f"provcheck controls: {name} not found")
    depth, i = 0, text.index("{", m.start())
    while True:
        c = text[i]
        depth += c == "{"
        depth -= c == "}"
        i += 1
        if depth == 0:
            return text[m.start():i]


def rename_identifiers(src):
    """Consistent renaming of every non-keyword identifier."""
    mapping = {}

    def sub(m):
        s = m.group()
        if s in C_KEYWORDS:
            return s
        if s not in mapping:
            mapping[s] = f"hl_n{len(mapping)}"
        return mapping[s]
    return re.sub(r"[A-Za-z_]\w*", sub, src)


def cmd_controls(args):
    """Plant known-bad content in scratch copies of the tree; every
    control must make `check` fail with the expected finding, the clean
    copy and the HLSDK control must pass."""
    root = repo_root()
    with open(args.corpora, encoding="utf-8") as f:
        cfg = {c["name"]: c for c in json.load(f)["corpora"]}
    quake = corpus_root(cfg["id-quake"])
    xash = corpus_root(cfg["xash3d-fwgs"])
    hlsdk = corpus_root(cfg["hlsdk"])

    def rd(p):
        with open(p, encoding="latin-1") as f:
            return f.read()

    q_world = rd(os.path.join(quake, "WinQuake/world.c"))
    q_fn = extract_function(q_world, "SV_RecursiveHullCheck")
    x_fn = extract_function(rd(os.path.join(xash, "ref/gl/gl_rmain.c")), "CL_FxBlend")
    q_comment = next(m.group() for m in re.finditer(r"/\*.*?\*/", q_world, re.S)
                     if len(WORD_RE.findall(m.group().lower())) >= 20
                     and "General Public" not in m.group())
    q_comment_py = "".join(f"# {ln.strip(' /*')}\n" for ln in q_comment.split("\n"))
    hl_pm = rd(os.path.join(hlsdk, "pm_shared/pm_shared.c"))
    qg = corpus_root(cfg["quakegeneric-audit"])
    w64 = rd(os.path.join(qg, "n64/port/w64_world.c")).split("\n")
    w64_flagged = "\n".join(w64[879:996])     # audit: QUAKE 880-996
    pool = rd(os.path.join(qg, "n64/port/q64_pool.c"))
    pool = re.sub(r"\A\s*/\*.*?\*/", "", pool, count=1, flags=re.S)  # GPL header off
    mit = "/* SPDX-License-Identifier: MIT */\n/* provcheck control */\n"
    valve = ("/***\n*\n*\tCopyright (c) 1996-2002, Valve LLC. All rights reserved.\n*\n****/\n")
    controls = [
        ("clean tree", {}, None),
        ("Quake function in engine/", {"engine/ctl/ctl.c": mit + q_fn}, "SIMILAR-EXACT"),
        ("Quake function in plat/ (identifiers renamed)", {"plat/ctl/ctl.c": mit + rename_identifiers(q_fn)}, "SIMILAR-NORM"),
        ("Quake function in game/ (HLSDK zone)", {"game/ctl/ctl.c": valve + q_fn}, "SIMILAR-EXACT"),
        ("Xash function in engine/", {"engine/ctl/ctl.c": mit + x_fn}, "SIMILAR-EXACT"),
        ("Xash function in tools/", {"tools/ctl/ctl.c": mit + x_fn}, "SIMILAR"),
        ("Quake comment in include/", {"include/hl64/ctl.h": mit + q_comment + "\nint ctl;\n"}, "SIMILAR-COMMENT"),
        ("Quake comment in a Python tool", {"tools/ctl/ctl.py": "# SPDX-License-Identifier: MIT\n" + q_comment_py}, "SIMILAR-COMMENT"),
        ("audit-flagged QUAKE range of w64_world.c kept in an import (D9)", {"engine/world/wld_ctl.c": mit + w64_flagged}, "SIMILAR"),
        ("CLEAN import q64_pool.c with MIT header (allowed)", {"engine/mem/pool.c": mit + pool}, None),
        ("engine identifier edict_t", {"engine/ctl/ctl.c": mit + "typedef struct edict_s edict_t;\n"}, "IDENT"),
        ("engine identifier Sys_Error", {"engine/ctl/ctl.c": mit + "void Sys_Error(const char *e);\n"}, "IDENT"),
        ("missing licence line", {"engine/ctl/ctl.c": "int ctl_value;\n"}, "LICENCE"),
        ("GPL header", {"plat/ctl/ctl.c": "/* SPDX-License-Identifier: MIT */\n/* GNU General Public License */\nint x;\n"}, "LICENCE"),
        ("MIT file in game/", {"game/ctl/ctl.c": mit + "int ctl_value;\n"}, "LICENCE"),
        ("game data file", {"test/fixtures/c1a0.bsp": "x"}, "FORBIDDEN-FILE"),
        ("QuakeC translation name", {"game/hl/g_combat.c": valve + "int ctl;\n"}, "FORBIDDEN-FILE"),
        ("whole Quake file", {"engine/world/world.c": q_world}, "FORBIDDEN-FILE"),
        ("HLSDK pm_shared in game/pm (allowed)", {"game/pm/pm_shared.c": hl_pm}, None),
        ("hl64-style sys_error in engine/ (allowed)", {"engine/ctl/ctl.c": mit + "void sys_error(const char *e);\nint snd_initialized;\n"}, None),
    ]
    scratch = os.path.join(os.environ.get("TMPDIR", "/tmp"), "provcheck-controls")
    db = DB(args.db, args.manifest)
    if args.worktree:
        base = {rel: os.path.join(root, rel) for rel in repo_files(root)}
    else:
        tar = subprocess.run(["git", "-C", root, "archive", "--format=tar", "HEAD"],
                             capture_output=True, check=True).stdout
    fails = 0
    for name, plant, expect in controls:
        shutil.rmtree(scratch, ignore_errors=True)
        os.makedirs(scratch)
        if args.worktree:
            for rel, src in base.items():
                if os.path.isfile(src) and not os.path.islink(src):
                    dst = os.path.join(scratch, rel)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(src, dst)
        else:
            with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
                tf.extractall(scratch, filter="data")
        for rel, content in plant.items():
            dst = os.path.join(scratch, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "w", encoding="latin-1") as f:
                f.write(content)
        subprocess.run(["git", "init", "-q", scratch], check=True)
        buf = io.StringIO()
        old = sys.stdout
        sys.stdout = buf
        try:
            rc = run_check(scratch, db)
        finally:
            sys.stdout = old
        out = buf.getvalue()
        planted = [ln for ln in out.split("\n") if any(r in ln for r in plant)]
        if expect is None:
            ok = rc == 0
        else:
            ok = rc == 1 and any(ln.startswith(expect) for ln in planted)
        fails += not ok
        print(f"control {'ok  ' if ok else 'FAIL'} rc={rc} expect={expect or 'PASS'}: {name}")
        for ln in (planted if expect else out.strip().split("\n")[-3:]):
            print(f"    {ln}")
    # a corrupt or missing DB must fail the real command line with exit 2
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    bad = os.path.join(scratch, "bad.db.gz")
    with open(args.db, "rb") as f:
        blob = bytearray(f.read())
    blob[len(blob) // 2] ^= 0xFF
    with open(bad, "wb") as f:
        f.write(blob)
    for name, db_path in (("corrupt DB", bad), ("missing DB", os.path.join(scratch, "none.db.gz"))):
        r = subprocess.run([sys.executable, os.path.abspath(__file__), "--db", db_path,
                            "--manifest", args.manifest, "check"], capture_output=True, text=True)
        ok = r.returncode == 2
        fails += not ok
        print(f"control {'ok  ' if ok else 'FAIL'} rc={r.returncode} expect=ERROR (2): {name}")
        print(f"    {r.stderr.strip()}")
    shutil.rmtree(scratch, ignore_errors=True)
    total = len(controls) + 2
    print(f"provcheck controls: {total - fails}/{total} ok")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("check")
    c.add_argument("--root")
    b = sub.add_parser("build-db")
    b.add_argument("--corpora", default=DEFAULT_CORPORA)
    s = sub.add_parser("scan")
    s.add_argument("--hlsdk", action="store_true", help="treat as HLSDK zone")
    s.add_argument("--idents", action="store_true")
    s.add_argument("paths", nargs="+")
    t = sub.add_parser("controls")
    t.add_argument("--corpora", default=DEFAULT_CORPORA)
    t.add_argument("--worktree", action="store_true",
                   help="base the scratch copies on the working tree instead of HEAD")
    args = ap.parse_args()
    if args.cmd in (None, "check"):
        if args.cmd is None:
            args.root = None
        return cmd_check(args)
    return {"build-db": build_db, "scan": cmd_scan, "controls": cmd_controls}[args.cmd](args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit as e:
        if isinstance(e.code, str):   # setup/DB error: never 0, never "findings"
            print(e.code, file=sys.stderr)
            sys.exit(2)
        raise
