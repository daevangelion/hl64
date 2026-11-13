# SPDX-License-Identifier: MIT
"""hl_classes: the class -> studio-model table of the content cook, plus the
map-set queries built on it.

Imported from the quakegeneric tree (tools/hl_classes.py, provenance-audit.md
entry CLEAN, "HLSDK-informed data table"). Which models a cart needs is
derived, never listed per map: the staged maps' entity lumps name the
classes, CLASS_MODEL names the studio model the game precaches for each
class, and the closure adds what those classes spawn at run time
(monstermaker `monstertype`, grunt weapon drops). Only classes the game
spawns count (spawnable_classes), so a class the game does not implement
never drags its model onto the cart. The
game's own precache list (weapons, effects) becomes `game/defs/res.def`
with game/ (M3); ALWAYS_MODELS stands in for it until then.

Users: tools/cook/stage.py (models to stage), tools/cook/hl_campaign.py and
tools/w64c/w64seam.py (entity lumps).

  python3 -m tools.cook.hl_classes --maps-dir DATA/maps [--maps GLOB ...]
"""
from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set

ROOT = Path(__file__).resolve().parents[2]
ENTCOOK_H = ROOT / "tools" / "w64c" / "hl_entcook.h"

# BSP header: version + lump 0 (entities) offset, length
BSP_VERSIONS = (29, 30)
MAX_ENTS_TEXT = 4 << 20

# Primary studio model each class precaches (GoldSrc class Precache).
# Keys as the game spells them; lookups are case-insensitive except
# model_for_class_exact.
CLASS_MODEL: Dict[str, str] = {
    # monsters
    "monster_scientist": "scientist",
    "monster_scientist_dead": "scientist",
    "monster_barney": "barney",
    "monster_headcrab": "headcrab",
    "monster_houndeye": "houndeye",
    "monster_zombie": "zombie",
    "monster_gman": "gman",
    "monster_gargantua": "garg",
    "monster_barnacle": "barnacle",
    "monster_cockroach": "roach",
    "monster_human_grunt": "hgrunt",
    "monster_hgrunt_dead": "hgrunt",
    "monster_grunt_repel": "hgrunt",
    "monster_alien_slave": "islave",
    "monster_miniturret": "miniturret",
    "monster_tripmine": "v_tripmine",
    # items
    "item_suit": "w_suit",
    "item_healthkit": "w_medkit",
    "item_battery": "w_battery",
    "item_longjump": "w_longjump",
    "item_sodacan": "can",
    "ammo_9mmclip": "w_9mmclip",
    "ammo_glockclip": "w_9mmclip",
    "ammo_9mmAR": "w_9mmarclip",
    "ammo_mp5clip": "w_9mmarclip",
    "ammo_9mmbox": "w_chainammo",
    "ammo_buckshot": "w_shotbox",
    "ammo_ARgrenades": "w_argrenade",
    "ammo_mp5grenades": "w_argrenade",
    # weapon world models (viewmodels are in ALWAYS_MODELS)
    "weapon_crowbar": "w_crowbar",
    "weapon_9mmhandgun": "w_9mmhandgun",
    "weapon_glock": "w_9mmhandgun",
    "weapon_357": "p_357",
    "weapon_shotgun": "w_shotgun",
    "weapon_9mmar": "w_9mmar",
    "weapon_mp5": "w_9mmar",
    "weapon_handgrenade": "w_grenade",
}

# Classes whose studio model is the entity's own `model` key.
MODEL_KEY_CLASSES = {"monster_generic": ("holo", "player")}

# Gib shooters precache their debris model, but the cart ships no gib
# models (memory policy: the shooters run without them). Listed so sizing
# reports know about them.
OFFCART_CLASSES = {"env_shooter": "shootmodel", "gibshooter": "models/hgibs.mdl"}

# Classes a class spawns at run time (closure is taken).
CLASS_SPAWNS: Dict[str, Iterable[str]] = {
    # a grunt drops its weapon; a repelling grunt becomes a grunt
    "monster_human_grunt": ("weapon_shotgun", "weapon_9mmAR", "ammo_ARgrenades"),
    "monster_grunt_repel": ("monster_human_grunt",),
    # env_beverage vends item_sodacan
    "env_beverage": ("item_sodacan",),
}

# GoldSrc world_items `type` -> item class (HLSDK items.cpp CWorldItem).
WORLD_ITEMS_TYPE = {"42": "item_antidote", "43": "item_security",
                    "44": "item_battery", "45": "item_suit"}

# Precached for every map: the six viewmodels, thrown / M203 grenades.
VIEW_MODELS = ("v_crowbar", "v_9mmhandgun", "v_shotgun", "v_357", "v_9mmar",
               "v_grenade")
ALWAYS_MODELS = VIEW_MODELS + ("w_grenade", "grenade")

# Studio bake layout (M3): viewmodels bone-local, these props pre-skinned
# model-space rest poses, every other monster model a bone-local world NPC.
STATIC_BAKE = ("w_suit", "v_tripmine")


class BspError(ValueError):
    pass


def entity_text(bsp: Path) -> str:
    """The BSP29/30 entity lump (lump 0) text; BspError on a bad header."""
    try:
        with open(bsp, "rb") as f:
            head = f.read(12)
            f.seek(0, 2)
            size = f.tell()
            if len(head) < 12:
                raise BspError(f"{bsp}: shorter than a BSP header")
            version, ofs, length = struct.unpack("<iII", head)
            if version not in BSP_VERSIONS:
                raise BspError(f"{bsp}: BSP version {version}, want 29 or 30")
            if ofs > size or length > size - ofs or length > MAX_ENTS_TEXT:
                raise BspError(f"{bsp}: entity lump {ofs}+{length} outside the "
                               f"{size}-byte file or over {MAX_ENTS_TEXT} bytes")
            f.seek(ofs)
            data = f.read(length)
    except OSError as e:
        raise BspError(f"{bsp}: {e.strerror}") from None
    return data.split(b"\0", 1)[0].decode("latin-1")


def map_entities(bsp: Path) -> List[Dict[str, str]]:
    """Entity lump as a list of key/value dicts (later keys win)."""
    return [dict(re.findall(r'"([^"]*)"\s*"([^"]*)"', blk))
            for blk in re.findall(r'\{((?:"[^"]*"|[^"{}])*)\}', entity_text(Path(bsp)))]


def discover_maps(maps_dir: Path, only: Optional[Iterable[str]] = None) -> List[str]:
    """Map names (BSP stems) in maps_dir, sorted; `only` filters by fnmatch."""
    import fnmatch
    names = sorted(p.stem for p in Path(maps_dir).glob("*.bsp"))
    if only:
        pats = list(only)
        names = [n for n in names if any(fnmatch.fnmatch(n, p) for p in pats)]
    return names


def spawnable_classes(entcook_h: Path = ENTCOOK_H) -> Set[str]:
    """Lower-case classnames of placed entities the game spawns: what the
    entity cook keeps (`hle_classes[]`)."""
    text = Path(entcook_h).read_text("latin-1")
    start = text.find("hle_classes[]")
    end = text.find("{NULL", start)
    if start < 0 or end < 0:
        raise ValueError(f"{entcook_h}: no hle_classes[] table")
    out = {m.lower() for m in re.findall(r'\{"([A-Za-z0-9_]+)"\s*,', text[start:end])}
    if not out:
        raise ValueError(f"{entcook_h}: empty hle_classes[] table")
    return out


def offcart_models(ents: List[Dict[str, str]]) -> Set[str]:
    """Model bases the map's gib shooters reference (not shipped)."""
    out: Set[str] = set()
    for e in ents:
        v = OFFCART_CLASSES.get(e.get("classname", "").lower())
        if v:
            m = (v if v.startswith("models/") else e.get(v, "")).lower()
            if m.startswith("models/") and m.endswith(".mdl"):
                out.add(m[len("models/"):-4])
    return out


def _lc(table: Dict[str, str]) -> Dict[str, str]:
    return {k.lower(): v for k, v in table.items()}


def model_for_class_exact(cn: str) -> Optional[str]:
    """models/<base>.mdl for an exact-case classname."""
    base = CLASS_MODEL.get(cn)
    return f"models/{base}.mdl" if base else None


def entity_classes(e: Dict[str, str]) -> List[str]:
    """Classes an entity puts in the game: itself plus what it names
    (monstermaker monstertype, world_items type), lower-case."""
    cn = e.get("classname", "").lower()
    out = [cn]
    if cn == "monstermaker" and e.get("monstertype"):
        out.append(e["monstertype"].lower())
    elif cn == "world_items" and e.get("type") in WORLD_ITEMS_TYPE:
        out.append(WORLD_ITEMS_TYPE[e["type"]])
    return out


def class_closure(classes: Iterable[str]) -> Set[str]:
    spawns = {k.lower(): [c.lower() for c in v] for k, v in CLASS_SPAWNS.items()}
    seen: Set[str] = set()
    todo = [c.lower() for c in classes]
    while todo:
        c = todo.pop()
        if c in seen:
            continue
        seen.add(c)
        todo.extend(spawns.get(c, ()))
    return seen


def map_models(ents: List[Dict[str, str]], spawnable: Set[str]) -> Dict[str, Set[str]]:
    """{model base: {classes needing it}} for one map's entities."""
    cm = _lc(CLASS_MODEL)
    classes: Set[str] = set()
    out: Dict[str, Set[str]] = {}
    for e in ents:
        cls = entity_classes(e)
        if cls[0] not in spawnable:
            continue                   # the entity cook drops it
        # a kept entity's named class (monstermaker monstertype, world_items
        # type) is created by the game: it counts when the game implements it
        classes.update([cls[0]] + [c for c in cls[1:] if c in spawnable or c in cm])
        if cls[0] in MODEL_KEY_CLASSES:
            m = e.get("model", "").replace("\\", "/").lower()
            if m.startswith("models/") and m.endswith(".mdl"):
                out.setdefault(m[len("models/"):-4], set()).add(cls[0])
    # what those classes spawn at run time (CLASS_SPAWNS) the game creates
    for c in class_closure(classes):
        if c in cm:
            out.setdefault(cm[c], set()).add(c)
    for b in ALWAYS_MODELS:
        out.setdefault(b, set()).add("(always)")
    return out


def models_for_maps(maps_dir: Path, maps: Iterable[str],
                    entcook_h: Path = ENTCOOK_H) -> Dict[str, Dict[str, Set[str]]]:
    """{map: {model base: classes}} for every map."""
    spawnable = spawnable_classes(entcook_h)
    return {m: map_models(map_entities(Path(maps_dir) / f"{m}.bsp"), spawnable)
            for m in maps}


def bake_sets(models: Iterable[str],
              per_map: Optional[Dict[str, Dict[str, Set[str]]]] = None):
    """(viewmodels, world models, static world models) to studio-bake.
    World = models of monster classes (NPC bone-local) + STATIC_BAKE props
    in the wanted set; order follows CLASS_MODEL for stable output."""
    want = set(models)
    npc: Set[str] = set()
    if per_map:
        for mm in per_map.values():
            for base, cls in mm.items():
                if any(c.startswith("monster_") for c in cls):
                    npc.add(base)
    world_order = [v for v in dict.fromkeys(CLASS_MODEL.values())]
    extra = sorted(npc - set(world_order))
    world = [b for b in world_order + extra
             if b in want and (b in npc or b in STATIC_BAKE)]
    view = [b for b in VIEW_MODELS if b in want]
    static = tuple(b for b in world if b in STATIC_BAKE)
    # NPCs first, statics last
    world = [b for b in world if b not in STATIC_BAKE] + list(static)
    return view, world, static


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--maps-dir", type=Path, required=True)
    ap.add_argument("--maps", nargs="*", default=None,
                    help="map names / globs (default: every *.bsp)")
    args = ap.parse_args()
    maps = discover_maps(args.maps_dir, args.maps)
    try:
        per_map = models_for_maps(args.maps_dir, maps)
    except ValueError as e:
        print(f"hl_classes: error: {e}")
        return 1
    allm = set().union(*per_map.values()) if per_map else set()
    view, world, _ = bake_sets(allm, per_map)
    for m in maps:
        print(f"{m}: " + " ".join(sorted(per_map[m])))
    print(f"union ({len(allm)}): " + " ".join(sorted(allm)))
    print("bake view: " + " ".join(view))
    print("bake world: " + " ".join(world))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
