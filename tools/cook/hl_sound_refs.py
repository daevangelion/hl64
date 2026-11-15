# SPDX-License-Identifier: MIT
"""hl_sound_refs: which Half-Life sounds, sentences and titles the staged
maps need.

Imported from the quakegeneric tree (tools/hl_sound_refs.py and the
ESSENTIAL list of tools/extract_hl_sounds.sh, provenance-audit.md entries
CLEAN), retargeted to read the player's data through tools/cook/data.py
(loose retail files or pak archives).

References come from:
  * the maps (BSP30 entity lump of every staged map): every key value that
    names a .wav, "!SENTENCE" values, scripted_sentence "sentence", speaker
    "message" (sentence group), env_message / player_loadsaved "message"
    (titles.txt name),
    env_shooter "shootmodel" / gibshooter (models/hgibs.mdl);
  * studio model events of the staged models: 1004/1008/5004 (sample) and
    1005/1009 (sentence) options;
  * the game: ESSENTIAL (the monster, weapon and item sounds the game plays
    by name) until game/defs/res.def lists them, plus the SOUND / SENTENCE /
    GROUP lines of res.def when it exists (passed in by the stage).

Selected sentences are written as a pruned sound/sentences.txt and their
words (restab's parser: dir/word.wav, `vox/` by default, ","/"." ->
_comma/_period) are added to the sample set.
Selected titles are written as a pruned titles.txt (settings inlined).
"""
from __future__ import annotations

import re
import struct
from typing import Dict, Iterable, List, Optional, Set, Tuple

from tools.cook.data import Data
from tools.cook.hl_classes import map_entities
from tools.restab import restab

SAMPLE_RE = re.compile(r"^[A-Za-z0-9_\-!'/]+\.wav$", re.I)

# Sounds the game plays by name (monster idle/alert/attack/death/pain,
# crowbar/glock/shotgun, suit/health), relative to sound/.
ESSENTIAL = [
    # scientist — combat/startle only (no sentence VO)
    "scientist/sci_die1.wav",
    "scientist/sci_die2.wav",
    "scientist/sci_die3.wav",
    "scientist/sci_die4.wav",
    "scientist/sci_pain1.wav",
    "scientist/sci_pain2.wav",
    "scientist/sci_pain3.wav",
    "scientist/sci_fear1.wav",
    "scientist/sci_fear2.wav",
    "scientist/startle1.wav",
    "scientist/startle2.wav",
    "scientist/scream1.wav",
    "scientist/hello.wav",
    "scientist/cough.wav",
    # barney — ba_* core + short greeting/alert
    "barney/ba_die1.wav",
    "barney/ba_die2.wav",
    "barney/ba_die3.wav",
    "barney/ba_pain1.wav",
    "barney/ba_pain2.wav",
    "barney/ba_pain3.wav",
    "barney/ba_attack1.wav",
    "barney/ba_attack2.wav",
    "barney/ba_gotone.wav",
    "barney/howdy.wav",
    "barney/hearsomething.wav",
    "barney/heybuddy.wav",
    # headcrab — skip huge idle3/idle5 (~48 KB each)
    "headcrab/hc_idle1.wav",
    "headcrab/hc_idle2.wav",
    "headcrab/hc_idle4.wav",
    "headcrab/hc_alert1.wav",
    "headcrab/hc_alert2.wav",
    "headcrab/hc_attack1.wav",
    "headcrab/hc_attack2.wav",
    "headcrab/hc_headbite.wav",
    "headcrab/hc_die1.wav",
    "headcrab/hc_die2.wav",
    "headcrab/hc_pain1.wav",
    "headcrab/hc_pain2.wav",
    # houndeye
    "houndeye/he_idle1.wav",
    "houndeye/he_idle2.wav",
    "houndeye/he_idle3.wav",
    "houndeye/he_alert1.wav",
    "houndeye/he_alert2.wav",
    "houndeye/he_attack1.wav",
    "houndeye/he_blast1.wav",
    "houndeye/he_die1.wav",
    "houndeye/he_die2.wav",
    "houndeye/he_pain1.wav",
    "houndeye/he_pain2.wav",
    "houndeye/he_hunt1.wav",
    # zombie — full pack is tiny
    "zombie/claw_miss1.wav",
    "zombie/claw_miss2.wav",
    "zombie/claw_strike1.wav",
    "zombie/claw_strike2.wav",
    "zombie/claw_strike3.wav",
    "zombie/zo_alert10.wav",
    "zombie/zo_alert20.wav",
    "zombie/zo_alert30.wav",
    "zombie/zo_attack1.wav",
    "zombie/zo_attack2.wav",
    "zombie/zo_idle1.wav",
    "zombie/zo_idle2.wav",
    "zombie/zo_idle3.wav",
    "zombie/zo_idle4.wav",
    "zombie/zo_pain1.wav",
    "zombie/zo_pain2.wav",
    # garg — one of each category (+ stomp/step/breathe)
    "garg/gar_idle1.wav",
    "garg/gar_idle3.wav",
    "garg/gar_alert1.wav",
    "garg/gar_attack1.wav",
    "garg/gar_stomp1.wav",
    "garg/gar_die1.wav",
    "garg/gar_pain1.wav",
    "garg/gar_pain2.wav",
    "garg/gar_step1.wav",
    "garg/gar_breathe1.wav",
    # barnacle — full pack is small
    "barnacle/bcl_alert2.wav",
    "barnacle/bcl_bite3.wav",
    "barnacle/bcl_chew1.wav",
    "barnacle/bcl_chew2.wav",
    "barnacle/bcl_chew3.wav",
    "barnacle/bcl_die1.wav",
    "barnacle/bcl_die3.wav",
    "barnacle/bcl_tongue1.wav",
    # roach — full
    "roach/rch_die.wav",
    "roach/rch_smash.wav",
    "roach/rch_walk.wav",
    # weapons — crowbar / glock / shotgun (Uplink has no 357_shot*.wav)
    "weapons/cbar_hit1.wav",
    "weapons/cbar_hit2.wav",
    "weapons/cbar_hitbod1.wav",
    "weapons/cbar_miss1.wav",
    "weapons/pl_gun1.wav",
    "weapons/pl_gun2.wav",
    "weapons/pl_gun3.wav",
    "weapons/sbarrel1.wav",
    "weapons/dbarrel1.wav",
    "weapons/scock1.wav",
    "weapons/reload1.wav",
    "weapons/reload2.wav",
    "weapons/reload3.wav",
    "weapons/hks1.wav",
    "weapons/hks2.wav",
    "weapons/hks3.wav",
    "weapons/dryfire1.wav",
    "weapons/ric1.wav",
    "weapons/ric2.wav",
    "weapons/bullet_hit1.wav",
    "weapons/sshell1.wav",
    # items — suit / health / pickups
    "items/smallmedkit1.wav",
    "items/smallmedkit2.wav",
    "items/suitcharge1.wav",
    "items/suitchargeok1.wav",
    "items/suitchargeno1.wav",
    "items/gunpickup1.wav",
    "items/ammopickup1.wav",
]


# --------------------------------------------------------------- sentences

def parse_sentences(text: str) -> Dict[str, Tuple[str, str]]:
    """UPPER name -> (name as written, body); file order kept."""
    out: Dict[str, Tuple[str, str]] = {}
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("//"):
            continue
        parts = s.split(None, 1)
        if len(parts) < 2:
            continue
        out.setdefault(parts[0].upper(), (parts[0], parts[1]))
    return out


def group_of(name: str) -> str:
    return name.rstrip("0123456789")


def vox_words(name: str, body: str) -> List[str]:
    """sentences.txt body -> sample paths, by restab's sentence parser
    (tools/restab/restab.py parse_sentence_line: words in the current
    directory, `vox` until a word names another, "," and "." the
    _comma / _period samples), so the content holds every word restab
    tables."""
    try:
        words = restab.parse_sentence_line(name, body, f"sentences.txt {name}")
    except restab.CookError as e:
        raise ValueError(str(e)) from None
    return [w + ".wav" for w, _mods in words]


# --------------------------------------------------------------- titles

def parse_titles(text: str) -> Dict[str, Tuple[str, Dict[str, str], str]]:
    """UPPER name -> (name, settings in effect, body text)."""
    out = {}
    settings: Dict[str, str] = {}
    lines = text.splitlines()
    i = 0
    name: Optional[str] = None
    while i < len(lines):
        s = lines[i].strip()
        i += 1
        if not s or s.startswith("//"):
            continue
        if s.startswith("$"):
            key = s.split(None, 1)[0]
            settings[key] = s
            continue
        if s.startswith("{") and name:
            body = []
            while i < len(lines) and not lines[i].strip().startswith("}"):
                body.append(lines[i].rstrip("\r"))
                i += 1
            i += 1
            out.setdefault(name.upper(), (name, dict(settings), "\n".join(body)))
            name = None
            continue
        name = s
    return out


# --------------------------------------------------------------- studio events

def studio_event_refs(data: bytes) -> Tuple[Set[str], Set[str]]:
    """Samples (events 1004/1008/5004) and sentences (1005/1009) a studio
    model's animation events name. Out-of-range tables stop the walk."""
    samples: Set[str] = set()
    sentences: Set[str] = set()
    if len(data) < 172 or data[:4] != b"IDST":
        return samples, sentences
    numseq, seqindex = struct.unpack_from("<ii", data, 164)
    if numseq < 0 or numseq > 2048 or seqindex < 0:
        return samples, sentences
    for s in range(numseq):
        base = seqindex + s * 176
        if base + 56 > len(data):
            break
        numevents, eventindex = struct.unpack_from("<ii", data, base + 48)
        if numevents < 0 or numevents > 4096 or eventindex < 0:
            continue
        for e in range(numevents):
            eb = eventindex + e * 76
            if eb + 76 > len(data):
                break
            event = struct.unpack_from("<i", data, eb + 4)[0]
            opt = data[eb + 12:eb + 76].split(b"\0", 1)[0].decode("latin-1").strip()
            opt = opt.lstrip("*")  # GoldSrc "*name": stream, don't cache
            if not opt:
                continue
            if event in (1004, 1008, 5004) and opt.lower().endswith(".wav"):
                samples.add(opt[6:] if opt.lower().startswith("sound/") else opt)
            elif event in (1005, 1009):
                sentences.add(opt)
    return samples, sentences


# --------------------------------------------------------------- collect

class Refs:
    def __init__(self) -> None:
        self.samples: Set[str] = set()          # as requested at runtime
        self.why: Dict[str, str] = {}           # lower-case sample -> first referrer
        self.sentence_names: Set[str] = set()   # UPPER exact names
        self.sentence_groups: Set[str] = set()  # UPPER group names
        self.titles: Set[str] = set()           # UPPER
        self.models: Set[str] = set()           # models/x.mdl lower-case
        # "sentence X" / "sentence group X" the player's sentences.txt lacks
        # -> first referrer (studio events; map keys: tools/cook/stage.py)
        self.missing_sentences: Dict[str, str] = {}

    def add_sample(self, s: str, who: str) -> None:
        self.samples.add(s)
        self.why.setdefault(sample_key(s), who)


def sample_key(s: str) -> str:
    """A sample name as the stage compares it (tools/cook/stage.py lower_rel)."""
    return s.replace("\\", "/").lstrip("/").lower()


def add_sentence_ref(refs: Refs, value: str, table: Dict[str, Tuple[str, str]],
                     groups: Set[str], who: str = "") -> None:
    v = value.strip()
    if v.startswith("!"):
        if v[1:].upper() in table:
            refs.sentence_names.add(v[1:].upper())
        elif who and len(v) > 1:
            refs.missing_sentences.setdefault(f"sentence {v[1:].upper()}", who)
    elif v.upper() in groups:
        refs.sentence_groups.add(v.upper())
    elif who and v:
        refs.missing_sentences.setdefault(f"sentence group {v.upper()}", who)


def collect(data: Data, maps: Iterable[str], models: Dict[str, bytes],
            samples: Iterable[str] = (), sentences: Iterable[str] = (),
            groups_in: Iterable[str] = ()
            ) -> Tuple[Refs, Dict[str, Tuple[str, str]],
                       Dict[str, Tuple[str, Dict[str, str], str]]]:
    """Refs of the staged maps (data/maps/<m>.bsp), the staged studio models
    ({name: bytes}) and the game (samples, sentence names, group names)."""
    sent_table = parse_sentences(data.need("sound/sentences.txt").decode("latin-1"))
    groups = {group_of(n) for n in sent_table}
    titles_raw = data.read("titles.txt")
    titles = parse_titles(titles_raw.decode("latin-1")) if titles_raw is not None else {}
    refs = Refs()

    for m in maps:
        for lump, e in enumerate(map_entities(data.root / "maps" / f"{m}.bsp")):
            cn = e.get("classname", "")
            for k, v in e.items():
                if SAMPLE_RE.match(v):
                    refs.add_sample(v[6:] if v.lower().startswith("sound/") else v,
                                    entity_ref(m, lump, e, k))
                elif v.startswith("!"):
                    add_sentence_ref(refs, v, sent_table, groups)
            if cn == "scripted_sentence":
                add_sentence_ref(refs, e.get("sentence", ""), sent_table, groups)
            elif cn == "speaker":
                add_sentence_ref(refs, e.get("message", ""), sent_table, groups)
            elif cn in ("env_message", "player_loadsaved") and \
                    e.get("message", "").upper() in titles:
                refs.titles.add(e["message"].upper())
            elif cn == "worldspawn" and e.get("chaptertitle", "").upper() in titles:
                refs.titles.add(e["chaptertitle"].upper())
            elif cn == "env_shooter" and e.get("shootmodel"):
                refs.models.add(e["shootmodel"].lower())
            elif cn == "gibshooter":
                refs.models.add("models/hgibs.mdl")
            # talk monsters (HLSDK CTalkMonster): a group name or !NAME
            for k in ("UseSentence", "UnUseSentence"):
                if e.get(k):
                    add_sentence_ref(refs, e[k], sent_table, groups)

    for s in ESSENTIAL:
        refs.add_sample(s, "game (hl_sound_refs.ESSENTIAL)")
    for s in samples:
        refs.add_sample(s, "game/defs/res.def")
    for n in sentences:
        add_sentence_ref(refs, "!" + n, sent_table, groups)
    for g in groups_in:
        add_sentence_ref(refs, g, sent_table, groups)

    for name in sorted(models):
        s, sn = studio_event_refs(models[name])
        for x in sorted(s):
            refs.add_sample(x, f"{name} studio event")
        for n in sorted(sn):
            add_sentence_ref(refs, n, sent_table, groups, f"{name} studio event")

    return refs, sent_table, titles


def selected_sentences(refs: Refs, table: Dict[str, Tuple[str, str]]) -> List[str]:
    return [n for n in table
            if n in refs.sentence_names or group_of(n) in refs.sentence_groups]


def sentences_txt(refs: Refs, table: Dict[str, Tuple[str, str]]) -> str:
    lines = ["// Pruned by tools/cook/hl_sound_refs.py: sentences referenced by the",
             "// staged maps, the game and studio model events."]
    for n in selected_sentences(refs, table):
        name, body = table[n]
        lines.append(f"{name} {body}")
    return "\n".join(lines) + "\n"


def titles_txt(refs: Refs, titles: Dict[str, Tuple[str, Dict[str, str], str]]) -> str:
    out = ["// Pruned by tools/cook/hl_sound_refs.py: titles used by env_message / "
           "player_loadsaved."]
    for key in sorted(refs.titles):
        name, settings, body = titles[key]
        out.extend(settings[k] for k in sorted(settings))
        out.append(name)
        out.append("{")
        out.append(body)
        out.append("}")
        out.append("")
    return "\n".join(out) + "\n"


def entity_ref(m: str, lump: int, e: Dict[str, str], key: str) -> str:
    """Who names a value: `maps/<m>.bsp entity <lump> (<classname> <targetname>) key <k>`."""
    tn = e.get("targetname", "")
    return (f"maps/{m}.bsp entity {lump} ({e.get('classname', '?')}"
            f"{' ' + tn if tn else ''}) key {key}")


def sentence_samples(refs: Refs, table: Dict[str, Tuple[str, str]]) -> Set[str]:
    out: Set[str] = set()
    for n in selected_sentences(refs, table):
        name, body = table[n]
        for w in vox_words(name, body):
            out.add(w)
            refs.why.setdefault(sample_key(w), f"sound/sentences.txt {name}")
    return out
