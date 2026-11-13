# Campaign sizing (retail Half-Life): cooked bytes per map and chapter, and the cart split

**Status:** measured on 2026-11-13 by the CampaignSize agent. Input is the retail Half-Life data (Steam app 70, `/scratch/hl1/valve`): the 96 campaign maps `c0a0`–`c5a1`, the 7 Hazard Course maps `t0a0*` (liblist `trainmap`), and the 3 Uplink maps `hldemo1-3` as a separate optional set. All content was cooked into scratch (`/scratch/hl64/tmp/CampaignSize`) with the old tree's tools from `~/src/ports/quakegeneric` at `beb6a3f` plus that tree's uncommitted edits as of today. Nothing was written into either tree, and no Valve or cooked bytes are committed. This document replaces the extrapolation in the old tree's `n64/docs/campaign-sizing.md` §6, which was based on Uplink and Hazard Course data only.

KiB / MiB are 1024-based. A cart is 64 MiB = 67,108,864 B. Labels:

| label | meaning |
|---|---|
| **cooked** | size of a file written by the old tree's tools (`w64c.py`, `bake_studio`, `hl_sprite_cook`, `convert_hl_sounds.sh`, `bake_hlhud.py`, `install_decals`) |
| **LZ4** | LZ4 HC (`lz4.block`, high compression), measured per chunk (`.w64`) or per file (models), with no shared dictionary |
| **cook-est** | computed from cooked counts with a stated per-item cost |
| **[EST]** / **[INFERENCE]** | estimated or reasoned, not measured |

## 1. Campaign graph and chapters

`tools/hl_campaign.py` runs over the 106 maps with liblist `startmap c0a0` and `trainmap t0a0`. It builds three components: the campaign (96 maps, one connected graph from `c0a0` to `c5a1`), Uplink (3), and the Hazard Course (7). No links were dropped.

The chapter names are the retail chapter titles in `titles.txt` (`C0A0TITLE` … `C5TITLE`). Each chapter is a map-name family (`cXaY*`). Map placements confirm the boundaries: worldspawn `chaptertitle` opens `C0A1TITLE` in `c1a0`, `C1A1TITLE` in `c1a0c`, `C2A4TITLE2` in `c2a4d`, `C4A1ATITLE` in `c4a1a`, and so on. `C4A1B/C/E/FTITLE` are placed in `c4a1b`, `c4a1c`, `c4a1e` and `c4a1f`, but their text in `titles.txt` is empty, so those maps belong to Interloper. Within a chapter, maps are listed in breadth-first order from `c0a0` over the changelevel graph.

| # | chapter | maps | entry (changelevel → map @ landmark) |
|---|---|---|---|
| 1 | Black Mesa Inbound | c0a0 c0a0a c0a0b c0a0c c0a0d c0a0e | start map |
| 2 | Anomalous Materials | c1a0 c1a0d c1a0a c1a0b c1a0e | c0a0e → c1a0 @ c0a0toc1a0 |
| 3 | Unforeseen Consequences | c1a0c c1a1 c1a1a c1a1f c1a1b c1a1c c1a1d | c1a0b → c1a0c @ c1a0btoc; c1a0e → c1a0c @ c1a0etoc |
| 4 | Office Complex | c1a2 c1a2a c1a2d c1a2b c1a2c | c1a1c → c1a2 @ a1a2 |
| 5 | "We've Got Hostiles" | c1a3 c1a3c c1a3d c1a3a c1a3b | c1a2b → c1a3 @ c1a2_to_c1a3 |
| 6 | Blast Pit | c1a4 c1a4k c1a4b c1a4d c1a4f c1a4i c1a4e c1a4g c1a4j | c1a3 → c1a4 @ c1a3toc1a4 |
| 7 | Power Up | c2a1 c2a1b c2a1a | c1a4j → c2a1 @ c1a4-c2a1 |
| 8 | On A Rail | c2a2 c2a2a c2a2b1 c2a2b2 c2a2c c2a2d c2a2e c2a2f c2a2g c2a2h | c2a1 → c2a2 @ c2a1-c2a2 |
| 9 | Apprehension | c2a3 c2a3a c2a3b c2a3c c2a3d c2a3e | c2a2g → c2a3 @ c2a2/3 |
| 10 | Residue Processing | c2a4 c2a4a c2a4b c2a4c | c2a3e → c2a4 @ c2a3d4 |
| 11 | Questionable Ethics | c2a4d c2a4e c2a4f c2a4g | c2a4c → c2a4d @ lm2 |
| 12 | Surface Tension | c2a5 c2a5w c2a5x c2a5a c2a5b c2a5c c2a5d c2a5e c2a5f c2a5g | c2a4g → c2a5 @ c2a4gc2a5 |
| 13 | "Forget About Freeman!" | c3a1 c3a1a c3a1b | c2a5g → c3a1 @ c2a5g/c3a1 |
| 14 | Lambda Core | c3a2e c3a2 c3a2a c3a2b c3a2c c3a2d c3a2f | c3a1b → c3a2e @ c3a1c3a2 |
| 15 | Xen | c4a1 | c3a2d → c4a1 @ c3a2_c4a1 |
| 16 | Gonarch's Lair | c4a2 c4a2a c4a2b | c4a1 → c4a2 @ c4a2 |
| 17 | Interloper | c4a1a c4a1b c4a1c c4a1d c4a1e c4a1f | c4a2b → c4a1a @ c4a1a |
| 18 | Nihilanth | c4a3 | c4a1f → c4a3 @ c4a3 |
| 19 | Endgame | c5a1 | c4a3 → c5a1 @ c4a3toc5a1 |

Every chapter boundary has exactly one forward changelevel, except Anomalous Materials → Unforeseen Consequences, which has two (both into `c1a0c`). Every boundary also has a changelevel back into the previous chapter (§6.3). The graph cook warns about 17 landmark placement mismatches inside chapters (c1a1c/d, c1a2b/c, c1a3a/b, c1a4d/e, c2a5w/x, c3a2b/c/f). These are pairs whose landmark math is not a single rigid offset. They do not affect sizing, but they matter for hl64's continuous-world seams.

## 2. What was cooked and how bytes are attributed

Per map, with the old tree's tools: `q64mapc` (intermediate) → `world_tex_bake` (`.wci4`) → `hlecook` (HLE1) → `w64c.py --hle --materials valve/sound/materials.txt --no-budget --json` gives one `.w64` v2 package. Its chunks are REND, MOVR, TEXP, COLL (streamed collision, `W64_HF_COLL`), ENTS and VISR. ZONE/DRYS/STRM are not emitted because hl64 drops the zone system (`w64-format.md`). `--no-budget` makes pool overruns warnings, and §7 lists them.

Before cooking, retail maps were preprocessed in scratch. 76 of the 96 campaign maps reference some of their textures in `halflife.wad` / `liquids.wad` / `xeno.wad` instead of embedding them, and `q64mapc` only reads textures embedded in the BSP. Uplink, the Hazard Course and 20 early campaign maps embed all of theirs. A scratch preprocessor (`embed_wads.py`) embedded 6,844 WAD textures into BSP copies; none were missing. The cooks then report 0 missing textures.

**Referenced assets.** The old `tools/hl_classes.py` class table only covers the Uplink/Hazard classes: 39 retail monster/weapon/item classes are absent from it (§7). Referenced assets were therefore derived from the HLSDK game source (`/scratch/hl64/corpora/hlsdk/dlls`), which hl64's `game/` uses.

- `LINK_ENTITY_TO_CLASS` gives classname → class.
- Each class's string literals in its own methods and its bases' methods give its models, sprites, sounds and sentence names/groups. `UTIL_PrecacheOther` / `Create` / `GiveNamedItem` / `DropItem` calls give the classes it spawns (closure).
- Map entity keys add `monstermaker monstertype`, `world_items type`, `func_breakable spawnobject`, every key value naming a `.mdl`/`.spr`/`.wav`, and `!SENTENCE`, `scripted_sentence` and `speaker` references.
- Every referenced model adds its `T.mdl` / `NN.mdl` companions and its studio-event sounds and sentences (`hl_sound_refs.studio_event_refs`).
- Sentences expand to word samples, with VOX's default `vox/` directory added (`hl_sound_refs` omits it).
- The **global set** is what GoldSrc precaches on every map: `W_Precache` (every weapon, ammo type and item), `ClientPrecache`, `CWorld`, `CBasePlayer` and the base weapon class, with closure. It holds 79 model files, 13 sprites and 587 sounds.

The class-literal rule over-approximates key-indexed choices: every door, button and breakable-material sound of a class counts. Those sounds are small and mostly shared.

**Cart cost per asset.**

- Model = `.mdl` (+ `T.mdl`, `NN.mdl` seqgroups, as the old tree ships them) + `.stb2` from `bake_studio`. The 213 referenced model files are 145 base models; each base model was baked, 136 succeeded and 9 failed (§7). **Lean** = STB2 + MDL minus its embedded skins, and `T.mdl` dropped when the STB2 carries the skins.
- Sprite = `.sc4` (`hl_sprite_cook`); the raw `.spr` the old tree also ships is not counted.
- Sound = `.wav64` from `tools/convert_hl_sounds.sh` with the shipping class rules (`tools/hl_sound_classes.txt`): VADPCM at the source rate (cap 22050 Hz), Huffman for speech. 1,676 WAVs (44.8 MiB) converted with 0 failures.

**Per map** below, models/sprites/sounds are the bytes of what that map references beyond the global set. These overlap between maps. Chapter and cart columns use the union.

## 3. Per chapter and per map

### 3.1 Per chapter (MiB, cooked)

The asset columns are the union of the chapter's non-global references. **Chapter raw** = `.w64` + models (mdl + stb2) + sprites + sounds, as cooked (scenario S0 in §6). Global assets are in §4.1. H (Hazard Course) and U (Uplink) are the optional sets.

| # | chapter | maps | .w64 | .w64 LZ4 | models (mdl+stb2) | models lean | sprites (sc4) | sounds (wav64) | **chapter raw** |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | Black Mesa Inbound | 6 | 4.94 | 2.79 | 2.63 | 1.87 | 0.01 | 7.30 | **14.89** |
| 2 | Anomalous Materials | 5 | 3.63 | 2.38 | 2.70 | 1.90 | 0.16 | 6.86 | **13.35** |
| 3 | Unforeseen Consequences | 7 | 5.82 | 3.86 | 3.64 | 2.58 | 0.09 | 5.94 | **15.49** |
| 4 | Office Complex | 5 | 4.52 | 3.02 | 3.70 | 2.57 | 0.09 | 5.58 | **13.90** |
| 5 | "We've Got Hostiles" | 5 | 2.84 | 1.81 | 3.03 | 2.09 | 0.14 | 5.36 | **11.38** |
| 6 | Blast Pit | 9 | 7.22 | 4.52 | 3.13 | 2.33 | 0.12 | 6.37 | **16.84** |
| 7 | Power Up | 3 | 2.95 | 1.95 | 2.94 | 2.02 | 0.18 | 3.74 | **9.81** |
| 8 | On A Rail | 10 | 9.72 | 6.43 | 3.76 | 2.67 | 0.19 | 4.09 | **17.76** |
| 9 | Apprehension | 6 | 4.37 | 2.84 | 3.81 | 2.64 | 0.15 | 6.07 | **14.40** |
| 10 | Residue Processing | 4 | 4.22 | 2.65 | 2.26 | 1.63 | 0.15 | 3.09 | **9.72** |
| 11 | Questionable Ethics | 4 | 3.35 | 2.17 | 2.70 | 1.95 | 0.08 | 5.59 | **11.72** |
| 12 | Surface Tension | 10 | 8.54 | 5.11 | 3.91 | 2.90 | 0.50 | 7.05 | **20.00** |
| 13 | "Forget About Freeman!" | 3 | 2.58 | 1.73 | 2.25 | 1.44 | 0.23 | 3.08 | **8.15** |
| 14 | Lambda Core | 7 | 4.55 | 2.99 | 4.10 | 2.84 | 0.40 | 6.64 | **15.69** |
| 15 | Xen | 1 | 0.87 | 0.47 | 0.95 | 0.62 | 0.22 | 1.70 | **3.75** |
| 16 | Gonarch's Lair | 3 | 1.06 | 0.59 | 0.88 | 0.53 | 0.07 | 1.44 | **3.45** |
| 17 | Interloper | 6 | 4.59 | 2.63 | 2.57 | 1.83 | 0.18 | 3.21 | **10.55** |
| 18 | Nihilanth | 1 | 2.13 | 0.65 | 2.54 | 1.97 | 0.31 | 4.76 | **9.74** |
| 19 | Endgame | 1 | 1.63 | 0.92 | 0.87 | 0.55 | 0.20 | 1.48 | **4.18** |
| H | Hazard Course | 7 | 5.76 | 3.82 | 1.74 | 1.31 | 0.06 | 6.51 | **14.07** |
| U | Uplink | 3 | 2.72 | 1.78 | 3.23 | 2.26 | 0.18 | 6.35 | **12.48** |

### 3.2 Per map (KiB, cooked)

`.w64` chunk bytes, LZ4 of the whole package (per chunk), and the cook's worst visleaf render / texture set and worst collision grid cell. **Bold** marks a value over the hl64 pools (render 240, texture 128, collision 208 KiB). The last three columns are what the map references beyond the global set; these overlap between maps. Chapter `H` = Hazard Course, `U` = Uplink.

| map | ch | .w64 | REND | MOVR | TEXP | COLL | ENTS | VISR | dir | LZ4 | worst leaf render / tex, cell coll | models | sprites | sounds (n) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|
| c0a0 | 1 | 873 | 451 | 46 | 106 | 105 | 8 | 55 | 102 | 439 | **319** / 78 / 57 | 1841 | 2 | 4426 (446) |
| c0a0a | 1 | 733 | 332 | 55 | 98 | 127 | 7 | 46 | 69 | 442 | 186 / 75 / 78 | 667 | 3 | 1380 (152) |
| c0a0b | 1 | 1065 | 551 | 59 | 94 | 206 | 9 | 60 | 87 | 586 | **331** / 53 / 100 | 1308 | 6 | 4844 (434) |
| c0a0c | 1 | 1118 | 460 | 98 | 101 | 272 | 24 | 50 | 114 | 668 | 231 / 64 / 106 | 1648 | 3 | 4091 (365) |
| c0a0d | 1 | 860 | 419 | 42 | 122 | 138 | 7 | 44 | 90 | 475 | **258** / 81 / 65 | 2327 | 1 | 2679 (283) |
| c0a0e | 1 | 413 | 171 | 40 | 92 | 58 | 8 | 12 | 32 | 246 | 164 / 68 / 37 | 0 | 0 | 1091 (110) |
| c1a0 | 2 | 718 | 296 | 46 | 196 | 90 | 22 | 28 | 41 | 482 | 152 / 118 / 65 | 1606 | 2 | 4484 (451) |
| c1a0d | 2 | 793 | 325 | 48 | 228 | 104 | 20 | 25 | 43 | 537 | 126 / 116 / 61 | 1358 | 3 | 4198 (438) |
| c1a0a | 2 | 680 | 245 | 82 | 143 | 125 | 13 | 33 | 39 | 462 | 137 / 76 / 79 | 1174 | 1 | 4052 (412) |
| c1a0b | 2 | 784 | 305 | 55 | 164 | 149 | 20 | 48 | 43 | 529 | 158 / 120 / 114 | 910 | 17 | 3749 (361) |
| c1a0e | 2 | 738 | 223 | 45 | 137 | 145 | 43 | 27 | 118 | 430 | 167 / 123 / 94 | 2054 | 148 | 5131 (446) |
| c1a0c | 3 | 889 | 331 | 55 | 169 | 172 | 25 | 78 | 59 | 598 | 193 / **140** / 140 | 1517 | 86 | 4699 (476) |
| c1a1 | 3 | 827 | 304 | 85 | 152 | 175 | 18 | 45 | 48 | 569 | 158 / 72 / 113 | 2103 | 84 | 4704 (508) |
| c1a1a | 3 | 762 | 311 | 49 | 240 | 89 | 11 | 24 | 38 | 525 | 130 / 120 / 59 | 1420 | 3 | 1107 (153) |
| c1a1f | 3 | 1124 | 462 | 95 | 235 | 158 | 27 | 56 | 91 | 743 | **250** / **151** / 96 | 1913 | 5 | 4546 (474) |
| c1a1b | 3 | 1031 | 386 | 126 | 144 | 230 | 25 | 61 | 59 | 687 | 220 / 85 / 145 | 2153 | 86 | 3930 (415) |
| c1a1c | 3 | 1039 | 433 | 112 | 160 | 170 | 24 | 60 | 81 | 656 | 190 / 91 / 98 | 1700 | 89 | 1789 (213) |
| c1a1d | 3 | 289 | 175 | 3 | 35 | 48 | 3 | 10 | 16 | 176 | 128 / 31 / 33 | 0 | 0 | 439 (73) |
| c1a2 | 4 | 1219 | 516 | 154 | 157 | 195 | 36 | 74 | 87 | 813 | **289** / 100 / 105 | 2243 | 6 | 3753 (379) |
| c1a2a | 4 | 1215 | 587 | 96 | 143 | 186 | 27 | 94 | 82 | 802 | 195 / 89 / 118 | 2651 | 4 | 4641 (499) |
| c1a2d | 4 | 367 | 181 | 15 | 67 | 52 | 5 | 29 | 18 | 248 | 157 / 59 / 47 | 1233 | 0 | 1830 (245) |
| c1a2b | 4 | 1221 | 570 | 122 | 141 | 206 | 32 | 73 | 77 | 816 | 189 / 88 / 142 | 3027 | 89 | 4742 (508) |
| c1a2c | 4 | 611 | 273 | 72 | 95 | 93 | 16 | 24 | 39 | 416 | 210 / 78 / 72 | 2878 | 3 | 3853 (415) |
| c1a3 | 5 | 923 | 385 | 88 | 193 | 126 | 35 | 43 | 54 | 625 | 112 / 90 / 75 | 2429 | 86 | 4891 (504) |
| c1a3c | 5 | 275 | 93 | 26 | 50 | 39 | 9 | 5 | 52 | 154 | 108 / 50 / 35 | 457 | 64 | 1019 (142) |
| c1a3d | 5 | 429 | 156 | 45 | 104 | 67 | 14 | 19 | 24 | 292 | 106 / 82 / 51 | 1846 | 2 | 3465 (360) |
| c1a3a | 5 | 869 | 384 | 109 | 129 | 121 | 27 | 44 | 55 | 560 | 167 / 87 / 66 | 1362 | 2 | 3598 (369) |
| c1a3b | 5 | 414 | 198 | 16 | 51 | 50 | 12 | 23 | 65 | 221 | 196 / 51 / 41 | 425 | 66 | 787 (114) |
| c1a4 | 6 | 863 | 370 | 94 | 108 | 150 | 18 | 30 | 93 | 525 | 171 / 79 / 74 | 1334 | 7 | 1299 (185) |
| c1a4k | 6 | 545 | 348 | 7 | 68 | 70 | 7 | 16 | 30 | 271 | **273** / 59 / 50 | 658 | 56 | 980 (131) |
| c1a4b | 6 | 974 | 479 | 49 | 121 | 163 | 18 | 63 | 82 | 571 | **331** / 88 / 91 | 2145 | 7 | 1836 (221) |
| c1a4d | 6 | 1016 | 490 | 74 | 114 | 217 | 10 | 57 | 54 | 678 | 152 / 79 / 111 | 1158 | 3 | 1101 (152) |
| c1a4f | 6 | 1102 | 526 | 78 | 122 | 208 | 26 | 65 | 78 | 685 | **262** / 91 / 104 | 1863 | 13 | 4091 (399) |
| c1a4i | 6 | 977 | 443 | 36 | 136 | 166 | 20 | 91 | 85 | 632 | **289** / 107 / 126 | 2179 | 53 | 4951 (481) |
| c1a4e | 6 | 490 | 190 | 32 | 73 | 141 | 7 | 18 | 28 | 341 | 134 / 54 / 78 | 669 | 0 | 909 (136) |
| c1a4g | 6 | 1124 | 546 | 56 | 60 | 300 | 12 | 65 | 85 | 714 | 230 / 45 / 169 | 1400 | 0 | 942 (127) |
| c1a4j | 6 | 306 | 115 | 31 | 45 | 81 | 4 | 12 | 18 | 213 | 124 / 45 / 65 | 444 | 0 | 700 (115) |
| c2a1 | 7 | 1074 | 435 | 104 | 134 | 231 | 31 | 65 | 74 | 711 | 158 / 96 / 114 | 1770 | 28 | 2959 (331) |
| c2a1b | 7 | 565 | 228 | 70 | 81 | 115 | 13 | 27 | 31 | 382 | 131 / 72 / 79 | 1242 | 16 | 1246 (173) |
| c2a1a | 7 | 1378 | 591 | 91 | 172 | 260 | 27 | 135 | 102 | 902 | **242** / 116 / 194 | 1093 | 171 | 1632 (205) |
| c2a2 | 8 | 483 | 240 | 22 | 79 | 81 | 6 | 19 | 36 | 318 | 99 / 53 / 49 | 594 | 5 | 2200 (234) |
| c2a2a | 8 | 1234 | 614 | 45 | 120 | 259 | 12 | 93 | 89 | 810 | 195 / 85 / 94 | 2172 | 8 | 1629 (219) |
| c2a2b1 | 8 | 1137 | 504 | 85 | 181 | 191 | 15 | 67 | 93 | 764 | 178 / 92 / 105 | 1047 | 7 | 1438 (187) |
| c2a2b2 | 8 | 830 | 375 | 47 | 146 | 131 | 11 | 60 | 59 | 562 | 158 / 74 / 79 | 1515 | 87 | 1515 (203) |
| c2a2c | 8 | 1084 | 496 | 56 | 152 | 163 | 9 | 60 | 148 | 683 | 200 / 85 / 98 | 983 | 3 | 600 (74) |
| c2a2d | 8 | 1170 | 507 | 64 | 163 | 193 | 17 | 106 | 119 | 759 | 234 / 99 / 106 | 2288 | 89 | 2731 (337) |
| c2a2e | 8 | 1300 | 622 | 81 | 166 | 254 | 16 | 73 | 88 | 871 | 139 / 88 / 88 | 821 | 10 | 1354 (176) |
| c2a2f | 8 | 1028 | 481 | 68 | 126 | 194 | 14 | 71 | 73 | 685 | 187 / 75 / 106 | 262 | 3 | 1148 (145) |
| c2a2g | 8 | 882 | 356 | 47 | 182 | 167 | 10 | 56 | 65 | 600 | 160 / 111 / 84 | 738 | 4 | 1104 (148) |
| c2a2h | 8 | 802 | 298 | 60 | 165 | 161 | 14 | 56 | 49 | 531 | 185 / 117 / 102 | 991 | 105 | 1256 (156) |
| c2a3 | 9 | 855 | 413 | 41 | 95 | 198 | 17 | 33 | 58 | 586 | 149 / 60 / 120 | 630 | 99 | 1200 (162) |
| c2a3a | 9 | 726 | 362 | 51 | 110 | 124 | 13 | 22 | 43 | 487 | 122 / 77 / 83 | 1415 | 0 | 3515 (351) |
| c2a3b | 9 | 882 | 477 | 38 | 122 | 125 | 34 | 29 | 57 | 539 | 217 / 73 / 76 | 1974 | 85 | 1662 (218) |
| c2a3c | 9 | 789 | 333 | 79 | 123 | 133 | 22 | 56 | 44 | 531 | 144 / 82 / 96 | 1468 | 4 | 3576 (357) |
| c2a3d | 9 | 790 | 366 | 59 | 99 | 124 | 33 | 52 | 56 | 523 | 208 / 66 / 69 | 1383 | 90 | 2447 (287) |
| c2a3e | 9 | 431 | 135 | 114 | 69 | 64 | 7 | 4 | 38 | 245 | 121 / 53 / 40 | 297 | 0 | 594 (63) |
| c2a4 | 10 | 552 | 219 | 28 | 47 | 170 | 8 | 44 | 36 | 344 | 167 / 45 / 101 | 1069 | 108 | 1079 (142) |
| c2a4a | 10 | 1261 | 558 | 89 | 73 | 267 | 21 | 152 | 102 | 789 | **249** / 55 / 161 | 937 | 10 | 2330 (278) |
| c2a4b | 10 | 1067 | 410 | 171 | 118 | 204 | 17 | 77 | 69 | 689 | 198 / 77 / 127 | 1243 | 33 | 1319 (170) |
| c2a4c | 10 | 1437 | 696 | 98 | 108 | 252 | 18 | 160 | 104 | 889 | 206 / 73 / 137 | 778 | 31 | 1416 (154) |
| c2a4d | 11 | 576 | 243 | 28 | 141 | 94 | 15 | 27 | 27 | 391 | 92 / 84 / 65 | 1241 | 16 | 2436 (292) |
| c2a4e | 11 | 1434 | 670 | 102 | 142 | 269 | 32 | 118 | 101 | 916 | 189 / 97 / 135 | 1826 | 14 | 3793 (369) |
| c2a4f | 11 | 1058 | 475 | 36 | 157 | 179 | 21 | 115 | 75 | 675 | 215 / 122 / 132 | 947 | 69 | 1091 (158) |
| c2a4g | 11 | 358 | 121 | 29 | 79 | 82 | 5 | 19 | 22 | 236 | 138 / 78 / 66 | 1129 | 2 | 819 (124) |
| c2a5 | 12 | 1272 | 624 | 75 | 85 | 259 | 27 | 97 | 104 | 624 | **411** / 70 / 99 | 1918 | 129 | 1318 (173) |
| c2a5w | 12 | 771 | 333 | 42 | 60 | 162 | 11 | 82 | 82 | 451 | **360** / 60 / 84 | 769 | 66 | 765 (120) |
| c2a5x | 12 | 803 | 316 | 48 | 29 | 177 | 24 | 106 | 103 | 477 | **352** / 27 / 107 | 807 | 91 | 1273 (147) |
| c2a5a | 12 | 577 | 248 | 35 | 40 | 146 | 7 | 38 | 63 | 326 | 226 / 40 / 65 | 655 | 64 | 1157 (137) |
| c2a5b | 12 | 733 | 273 | 47 | 113 | 173 | 14 | 59 | 55 | 475 | 178 / 96 / 96 | 335 | 127 | 708 (109) |
| c2a5c | 12 | 562 | 197 | 40 | 109 | 98 | 23 | 35 | 61 | 344 | 228 / 109 / 55 | 2066 | 36 | 4931 (500) |
| c2a5d | 12 | 507 | 192 | 51 | 101 | 85 | 18 | 29 | 32 | 353 | 137 / 83 / 70 | 988 | 71 | 1926 (240) |
| c2a5e | 12 | 1447 | 565 | 164 | 196 | 242 | 42 | 95 | 143 | 858 | **372** / 125 / 122 | 1420 | 334 | 2826 (302) |
| c2a5f | 12 | 1398 | 602 | 167 | 152 | 279 | 50 | 71 | 78 | 902 | 157 / 85 / 136 | 1524 | 139 | 2724 (304) |
| c2a5g | 12 | 675 | 257 | 78 | 144 | 120 | 18 | 12 | 46 | 427 | 191 / 94 / 64 | 1022 | 64 | 1425 (151) |
| c3a1 | 13 | 1019 | 420 | 91 | 170 | 195 | 32 | 48 | 62 | 688 | 201 / 104 / 93 | 1697 | 97 | 2499 (306) |
| c3a1a | 13 | 889 | 425 | 39 | 141 | 149 | 15 | 68 | 52 | 591 | 164 / 76 / 95 | 973 | 128 | 1458 (179) |
| c3a1b | 13 | 733 | 326 | 35 | 150 | 109 | 24 | 46 | 43 | 493 | 182 / 102 / 72 | 1350 | 101 | 1080 (157) |
| c3a2e | 14 | 653 | 283 | 74 | 156 | 67 | 19 | 19 | 34 | 429 | 147 / 114 / 50 | 2031 | 5 | 1421 (184) |
| c3a2 | 14 | 407 | 128 | 54 | 141 | 38 | 20 | 3 | 22 | 280 | 102 / 102 / 28 | 1888 | 89 | 4125 (403) |
| c3a2a | 14 | 1085 | 568 | 75 | 147 | 143 | 40 | 41 | 72 | 710 | 141 / 71 / 80 | 2340 | 94 | 4988 (523) |
| c3a2b | 14 | 531 | 192 | 44 | 110 | 106 | 22 | 26 | 31 | 352 | 183 / 85 / 97 | 520 | 187 | 1187 (161) |
| c3a2c | 14 | 995 | 471 | 61 | 148 | 169 | 36 | 43 | 66 | 648 | 160 / 81 / 102 | 2399 | 195 | 4625 (495) |
| c3a2d | 14 | 645 | 239 | 69 | 139 | 96 | 45 | 24 | 33 | 418 | 170 / 109 / 69 | 2195 | 248 | 5216 (515) |
| c3a2f | 14 | 347 | 110 | 34 | 110 | 60 | 7 | 11 | 15 | 231 | 130 / 108 / 56 | 926 | 101 | 1179 (148) |
| c4a1 | 15 | 892 | 363 | 49 | 30 | 191 | 34 | 141 | 85 | 485 | **284** / 29 / 135 | 973 | 228 | 1746 (208) |
| c4a2 | 16 | 496 | 279 | 8 | 17 | 110 | 14 | 30 | 39 | 236 | **252** / 17 / 65 | 701 | 11 | 1084 (139) |
| c4a2a | 16 | 405 | 196 | 14 | 21 | 113 | 12 | 19 | 31 | 235 | 124 / 19 / 70 | 513 | 6 | 1029 (139) |
| c4a2b | 16 | 189 | 69 | 9 | 25 | 58 | 12 | 6 | 10 | 134 | 78 / 25 / 49 | 708 | 66 | 871 (118) |
| c4a1a | 17 | 815 | 317 | 61 | 38 | 242 | 40 | 50 | 68 | 519 | 209 / 34 / 152 | 1702 | 110 | 1944 (226) |
| c4a1b | 17 | 1409 | 642 | 28 | 31 | 377 | 27 | 173 | 130 | 768 | **295** / 22 / 149 | 1923 | 86 | 2452 (256) |
| c4a1c | 17 | 461 | 203 | 23 | 55 | 106 | 17 | 34 | 24 | 288 | 164 / 49 / 73 | 659 | 48 | 1188 (145) |
| c4a1d | 17 | 817 | 365 | 41 | 46 | 190 | 28 | 87 | 60 | 493 | 219 / 40 / 142 | 1127 | 149 | 1357 (186) |
| c4a1e | 17 | 970 | 509 | 21 | 30 | 210 | 29 | 119 | 52 | 484 | **332** / 26 / 157 | 1127 | 121 | 993 (114) |
| c4a1f | 17 | 225 | 67 | 17 | 20 | 60 | 10 | 21 | 32 | 141 | 83 / 19 / 49 | 0 | 6 | 522 (36) |
| c4a3 | 18 | 2180 | 1594 | 25 | 46 | 345 | 45 | 15 | 110 | 664 | **1066** / 23 / 201 | 2599 | 322 | 4873 (442) |
| c5a1 | 19 | 1665 | 697 | 84 | 52 | 480 | 32 | 115 | 204 | 943 | 227 / 17 / **208** | 890 | 201 | 1519 (96) |
| t0a0 | H | 1051 | 564 | 23 | 97 | 182 | 21 | 78 | 86 | 692 | 192 / 76 / 88 | 1242 | 42 | 3719 (363) |
| t0a0a | H | 1053 | 550 | 44 | 117 | 165 | 18 | 83 | 76 | 676 | 176 / 76 / 84 | 444 | 42 | 1310 (162) |
| t0a0b | H | 1304 | 715 | 55 | 138 | 196 | 30 | 85 | 85 | 855 | 219 / 83 / 85 | 947 | 2 | 2269 (258) |
| t0a0b1 | H | 810 | 438 | 30 | 117 | 115 | 14 | 42 | 54 | 545 | 157 / 72 / 69 | 891 | 2 | 1855 (247) |
| t0a0b2 | H | 579 | 303 | 25 | 89 | 81 | 18 | 30 | 32 | 386 | 180 / 68 / 54 | 500 | 2 | 922 (130) |
| t0a0c | H | 674 | 347 | 10 | 111 | 103 | 11 | 49 | 42 | 455 | 140 / 72 / 78 | 444 | 66 | 771 (129) |
| t0a0d | H | 425 | 150 | 46 | 132 | 52 | 11 | 11 | 22 | 298 | 158 / 110 / 39 | 1707 | 0 | 4460 (469) |
| hldemo1 | U | 765 | 320 | 87 | 123 | 130 | 47 | 17 | 41 | 508 | 130 / 105 / 67 | 2869 | 35 | 5687 (556) |
| hldemo2 | U | 1151 | 513 | 102 | 161 | 209 | 43 | 58 | 66 | 743 | 207 / 85 / 106 | 2281 | 175 | 4912 (506) |
| hldemo3 | U | 874 | 341 | 94 | 108 | 202 | 30 | 48 | 52 | 569 | 211 / 86 / 144 | 2068 | 31 | 4996 (508) |

## 4. Shared assets and per-cart fixed overhead

### 4.1 Fixed overhead (every cart)

| item | KiB | basis |
|---|---:|---|
| code + ROM header/IPL3 | 1034 | the old HL cart's measured ROM overhead (1.01 MiB, `campaign-sizing.md` §5). hl64's RAM code target is ≤ 900 KB [EST] |
| HUD sheets (`320hud1/2/4`, `crosshairs`, `320_pain` → ui8) | 111 | cooked, `bake_hlhud.py` on retail sprites |
| `gsfx/decals.i4` (34 decals) | 23 | cooked, `install_decals` on retail `decals.wad` |
| menu / console art | 680 | proxy: the old image's `gfx/` minus its HUD [INFERENCE: the hl64 menu is not designed yet] |
| `campaign.cwg` + resource table + sentence table | 76 | CWG3 of the whole graph cooked (12.5, before seams); rtb 40 and sentences 24 [EST, architecture.md 4.1] |
| global models (79 files: player, every weapon v_/p_/w_, `W_Precache` ammo/items, gibs, grenade) | 4054 · lean 2287 · lean+LZ4 1586 | cooked |
| global sprites (13 `.sc4`) | 133 | cooked |
| global sounds (587: player, weapons, HEV/fvox, materials, common) | 2971 · 3-bit speech + 11025 Hz 2313 | cooked |
| **total** | **9083 (8.87 MiB)** · lean 7.14 MiB · lean+LZ4 6.46 MiB | |

GoldSrc precaches every weapon on every map (`W_Precache`), and inventory crosses carts, so every cart carries every weapon's assets.

### 4.2 Sharing between chapters (non-global assets, cooked, MiB)

| asset | used by 1 chapter | used by ≥ 2 chapters |
|---|---:|---:|
| models (mdl + stb2) | 2.14 | 6.28 |
| sprites (sc4) | 0.27 | 0.83 |
| sounds (wav64) | 10.06 | 8.70 |

Most-repeated: `bullsquid.mdl` (11 chapters, 352 KiB), `scientist.mdl` (13, 200), `islave.mdl` (13, 173), `barney.mdl` (14, 163), `gmant.mdl` (9, 141), `scientistt.mdl` (13, 124), `headcrab.mdl` (16, 86), `holo.mdl` (5, 269), `agrunt.mdl` (6, 186). Each cart duplicates every shared asset its chapters need. That duplication, fixed overhead included, costs 18.1 MiB for the recommended 2-cart split (§6.2: carts total 97.1 against 79.0 MiB on one hypothetical cart) and 41.7 MiB for the 3-cart split as cooked (166.3 against 124.5).

## 5. Against `campaign-sizing.md` and the levers

### 5.1 Estimate vs measurement

| item | `campaign-sizing.md` §6 (extrapolated to ~95 maps from Uplink + Hazard) | retail, measured here |
|---|---|---|
| maps, v2 raw | 79–129 MiB (COLL 1×–3.2× cook-est) | **79.5 MiB** for 96 campaign maps; 85.3 with the Hazard Course |
| per map | 854–1385 KiB | mean **848**, median 828, p90 1272, max 2180 KiB (`c4a3`) |
| COLL | the largest open lever (1×–3.2× of baseline) | cooked: **0.78–1.15×** of the 8 B node + 16 B plane baseline (mean 0.95); 15.8 MiB, 18 % of `.w64` |
| LZ4 on maps | 0.59 | **0.625** per chunk (53.3 / 85.3 MiB) |
| sounds | 38.3 MiB (per-map share grown linearly) | **23.7 MiB** (campaign + Hazard + global): retail maps reuse far more than the per-map extrapolation assumed |
| models | 8–15 MiB raw | **12.4 MiB** mdl + stb2; 8.1 lean |
| fixed | 2.6 MiB | **8.9 MiB**, of which 7.0 are the global (every-map) models/sprites/sounds the estimate did not count |
| best single cart | 80–85 MiB (row E) | **69.1 MiB** (S6, §6.1): still over 64 |
| verdict | two carts | two carts with per-chunk LZ4 (13 MiB headroom), or three carts with no lever at all |

### 5.2 Levers (campaign + Hazard Course, all carts summed as one)

| lever | before → after | saving | basis |
|---|---|---:|---|
| per-chunk LZ4 on `.w64` | 85.28 → 53.34 MiB | **31.9 MiB** | measured |
| per-file LZ4 on models | 12.43 → 8.86 MiB (lean: 8.12 → 5.97) | 3.6 (2.2) | measured |
| texture sharing: TEXP pages deduplicated by content | 11.32 → 2.60 MiB | **8.7 MiB** on one cart; 1.5–4.1 MiB per cart on a 2- or 3-cart split | measured (SHA-1 of every TEXP chunk) |
| 12 B vertices | vertex bytes 31.13 MiB (70 % of REND + MOVR) | 7.8 MiB raw, ≈ 4.9 after LZ4 | cook-est: ¼ of the vertex bytes |
| model dedup / lean models: drop the MDL's embedded skins and `T.mdl` where the STB2 carries the skins | 12.43 → 8.12 MiB | 4.3 MiB (skins in MDLs: 4.87 MiB) | measured |
| sound rate cap 11025 Hz (all classes) | 23.69 → 21.47 MiB | 2.2 MiB | measured (reconverted) |
| 3-bit VADPCM speech (`vadpcm,bits=3,huffman=true`) | 23.69 → 20.94 MiB | 2.8 MiB | measured; quality per AudioDiet (PESQ-WB 2.13 vs 2.29) |
| 3-bit speech + 11025 Hz cap | 23.69 → 18.73 MiB | 5.0 MiB | measured |
| light probes in REND | 6.72 MiB | not applied | measured size; a lever only if probes are thinned |
| sprites: ship `.sc4` only | `.spr` + `.sc4` 4.53 → 1.23 MiB | 3.3 MiB | measured |

Speech dominates sound: voice 12.88 MiB, fx 6.96, ambience 3.85.

## 6. Cart split plan (64 MiB carts)

Each cart holds the fixed overhead, its chapters' `.w64`, and the union of every asset those maps need, with shared assets duplicated per cart. Cart 1 also carries the Hazard Course (5.76 MiB of maps raw). Uplink is not on any campaign cart: on its own it needs 2.72 MiB of maps plus 3.23 models + 0.18 sprites + 6.35 sounds beyond the global set, in a separate ROM. Scenarios add levers cumulatively:

| scenario | levers | 1 cart, campaign + Hazard (MiB) |
|---|---|---:|
| S0 | as cooked (shipping audio, raw MDL + STB2, sc4 sprites, raw `.w64`) | 124.51 |
| S1 | + lean models | 120.20 |
| S2 | + TEXP dedup within a cart | 111.48 |
| S3 | + per-chunk LZ4 (maps, models) | 78.95 |
| S4 | + 12 B vertices (cook-est) | 74.08 |
| S5 | + 3-bit speech | 71.34 |
| S6 | + 11025 Hz cap | 69.13 |
| N1 | S2 + 12 B vertices, no LZ4 | 103.69 |
| N2 | N1 + 3-bit speech + 11025 Hz, no LZ4 | 98.74 |

**One cart is out of reach**: the best measured row (S6) is 69.1 MiB, 5.1 over.

### 6.1 Best split per scenario (every split at chapter boundaries searched; the largest cart is minimised)

| scenario | 2 carts: chapters | sizes (MiB) | fits? headroom on the larger | 3 carts: chapters | sizes (MiB) | headroom on the largest |
|---|---|---|---|---|---|---|
| S0 | 1–8 \| 9–19 | 78.36 / 67.91 | **no** | 1–5 \| 6–11 \| 12–19 | 55.66 / 56.32 / 54.27 | 7.68 (12 %) |
| S1 | 1–7 \| 8–19 | 64.55 / 74.52 | no | same | 52.27 / 52.84 / 50.62 | 11.16 |
| S2 | 1–8 \| 9–19 | 69.38 / 61.66 | no | same | 49.13 / 50.07 / 49.10 | 13.93 |
| S3 | 1–7 \| 8–19 | 46.07 / 50.99 | **yes, 13.01 (20 %)** | same | 38.42 / 37.73 / 36.66 | 25.58 |
| S4 | 1–7 \| 8–19 | 43.76 / 48.43 | yes, 15.57 | same | 36.72 / 35.69 / 35.49 | 27.28 |
| S5 | 1–7 \| 8–19 | 41.72 / 46.60 | yes, 17.40 | same | 34.73 / 34.27 / 33.91 | 29.27 |
| S6 | 1–8 \| 9–19 | 44.85 / 40.58 | yes, 19.15 | same | 32.88 / 33.72 / 33.17 | 30.28 |
| N1 | 1–8 \| 9–19 | 64.80 / 58.46 | no (0.80 over) | same | 46.49 / 46.92 / 47.10 | 16.90 |
| N2 | 1–8 \| 9–19 | 60.73 / 55.89 | yes, 3.27 (5 %) | same | 42.64 / 44.95 / 44.78 | 19.05 |

### 6.2 Recommendation

- **2 carts, chapters 1–7 + Hazard Course | 8–19, with per-chunk LZ4, lean models and per-cart TEXP dedup (S3).** Cart 1 (Black Mesa Inbound … Power Up) is 46.07 MiB: fixed 6.46, maps 20.85, models 3.24, sprites 0.50, sounds 15.02; 17.9 MiB (28 %) free. Cart 2 (On A Rail … Endgame) is 50.99 MiB: fixed 6.46, maps 26.02, models 4.03, sprites 1.08, sounds 13.40; 13.0 MiB (20 %) free. The headroom covers what is not measured: hl64 code and menu, seam tables, and the sector splits needed by the 22 maps that exceed the pools (§7.3).
- **Which lever makes 2 carts possible: per-chunk LZ4.** It takes the larger cart from 69.38 (S2) to 50.99 MiB. Without it, the 2-cart split needs every other lever at once (N2: 12 B vertices, 3-bit speech, 11025 Hz, lean models, TEXP dedup) and still leaves only 3.27 MiB (5 %). Of those, 12 B vertices are cook-est and their precision is unproven. Not recommended.
- **Fallback: 3 carts, chapters 1–5 + Hazard | 6–11 | 12–19.** It fits as cooked today with no lever (55.66 / 56.32 / 54.27 MiB, ≥ 7.68 MiB free each) and reaches 49–50 MiB per cart with lean models and TEXP dedup alone.
- The alternative 2-cart cut 1–8 | 9–19 is 51.88 / 45.07 MiB under S3. It is slightly worse balanced, and its boundary map carries up to 3 placed weapons (§6.3).

### 6.3 Cross-cart boundaries and the Controller Pak carry

At a boundary the leaving cart writes its level state (the dehydrate store of the maps visited on that cart) to its own FlashRAM, as any within-cart save does (D4). It then writes the carry note to the Controller Pak and asks for the next cart. The carry is GoldSrc's changelevel set (HLSDK `CChangeLevel::ChangeList` + `InTransitionVolume`):

- the changelevel: from map, to map, landmark name and the landmark origin in the from map. Positions travel landmark-relative.
- the player block: origin − landmark, angles, velocity, punch, health/armour, suit/HEV state, long jump, weapons with clips and the ammo slots, active weapon.
- the global state table entries still referenced on the far side (env_global states, `globalname` entities).
- the moveable entities in the landmark's PVS: classes that keep `FCAP_ACROSS_TRANSITION`, inside a `trigger_transition` named after the landmark when the map has one. `globalname` entities in that PVS carry their state through the global table.
- skill, game time, next cart / chapter id and a campaign CRC, so a later cart rejects a note from a different build.

The table below is a static view of the compiled maps; live play adds followers and dropped items. "Brush" entities are moveable-cap brush entities of the from-map (triggers, ladders). Whether GoldSrc restores another BSP's brush entity is engine-side behaviour that HLSDK does not show, so it is to be checked against the oracle.

| boundary (plan) | changelevel @ landmark (origin in from-map) | point entities in PVS ∩ volume | brush | global entities in PVS | global names used on both sides | back trigger |
|---|---|---|---:|---|---:|---|
| 7 \| 8 (2 carts, recommended) | c2a1 → c2a2 @ `c2a1-c2a2` (2016 −1664 112) | none | 6 trigger_hurt | none | 1 | c2a2 → c2a1, touch (spawnflags 0, no targetname) |
| 8 \| 9 (2 carts, alternative) | c2a2g → c2a3 @ `c2a2/3` (1732 −1720 −1416) | weapon_shotgun, 2 × weapon_tripmine (if not picked up) | 1 | none | 1 | c2a3 → c2a2g, touch |
| 5 \| 6 (3 carts) | c1a3 → c1a4 @ `c1a3toc1a4` (1066 1072 90) | none | 0 | 8 func_breakable `c1a4_breakable_{1–7,12}` | 8 | c1a4 → c1a3, touch |
| 11 \| 12 (3 carts) | c2a4g → c2a5 @ `c2a4gc2a5` (−1280 −2296 280) | none (11 `infodecal` are removed at spawn) | 0 | none | 4 (`c2a4g_crate1–4`) | c2a5 → c2a4g, touch |

Pak budget: a Controller Pak has 123 user pages of 256 B (31,488 B). Header and ids take 64 B. The player block is ≈ 1 KB [EST]. The global entries are 8 B each, and all 178 campaign global names (16 of them used in two chapters) fit in 1.4 KB. Each carried entity is ≈ 228 B of components + ≤ 1 KB private block (architecture.md 4.1) ≈ 1.25 KB. So a note holds about 22 carried entities [EST]. The plan boundaries need at most 3 statically, but in-chapter boundaries such as `c1a0e → c1a0c` (26 point entities in the PVS) show why the carried-entity count needs a cap.

Every boundary's back changelevel into the previous chapter is a live touch trigger, except `c3a2e → c3a1b` and `c4a3 → c4a1f`, which are use-only. A reverse crossing must therefore be supported: the same carry note in the other direction, with the earlier cart restoring its boundary map from its own FlashRAM. Duplicating the boundary map on both carts is the alternative: `c2a1` costs 1074 KiB raw / 711 LZ4 plus assets, and its own exits lead further back, so it only moves the problem.

## 7. Failures

### 7.1 Cook failures

1. **`w64c.py` COLL partition, 13 maps:** `c1a4d c1a4f c1a4g c2a1b c2a2b1 c2a2b2 c2a4 c2a4a c2a4b c2a5 c3a2b c3a2f c4a3`. The error is `w64c: error: hull node N shared by tree groups G and G` (c1a4d: `hull node 1059050 shared by tree groups 6 and 6`). Retail clipnode trees are DAGs: the compiler shares identical clip subtrees inside a model's hulls and between brush models. The small-subtree DFS in `coll_partition` (old tree `tools/w64c/w64c.py` 1074–1083) emits a shared subtree twice. With that fixed, the same maps fail across models (`… groups 6 and 7`).
   - Sized with a scratch copy (`work/w64c_sz.py`): the DFS skips nodes already placed, and a subtree shared across models is stored in each model's chunks.
   - The hl64 `w64c` rewrite needs a decision: shared COLL chunks between models, or duplication.
2. **`q64mapc` and `hlecook` (`source/hl/hl_nodecook.h` 840–842), 2 maps:** `c2a2a` (model 52) and `c3a2c` (models 160–162). The error is `model head node out of range`: hull 1–3 heads equal `numclipnodes` (19517 / 11620) on clip-less brush models.
   - Sized after setting those heads to −1 (empty) in the scratch BSP copy. GoldSrc never traces those hulls [INFERENCE].
   - The hl64 cook should accept head == count as empty.
3. **`bake_studio`, 9 of 145 base models:**
   - `|pos| exceeds Q8 range`: apache (365.9), big_mom (148.4), fungus(large) (208.0), nihilanth (771.9), osprey (360.7), tentacle2 (173.6).
   - `empty after cull`: concrete_gibs, mechgibs, metalgibs.
   - These are counted as raw MDL only (no STB2), so the lean rule does not drop their skins. Large-scale models need a wider position format.
4. **Sounds, sprites, decals, HUD: 0 failures** (1,676 WAVs, 76 sprites, 34 decals, 5 HUD sheets).

### 7.2 Tool coverage gaps found on retail data (no crash, wrong output)

- `q64mapc` ignores WAD-referenced textures: 76 campaign maps, 6,844 textures; fixed in scratch by embedding them.
- `hl_classes.py` has no entries for 39 retail classes:
  - monsters: `monster_alien_controller`, `alien_grunt`, `apache`, `babycrab`, `barney_dead`, `bigmomma`, `bullchicken`, `flyer_flock`, `furniture`, `generic`, `hevsuit_dead`, `human_assassin`, `ichthyosaur`, `leech`, `mortar`, `nihilanth`, `osprey`, `rat`, `satchel`, `sentry`, `sitting_scientist`, `snark`, `tentacle`, `turret`;
  - weapons: crossbow, egon, gauss, hornetgun, rpg, satchel, snark, tripmine;
  - ammo and items: `ammo_357`, `crossbow`, `gaussclip`, `rpgclip`, `9mmarclip`, `item_antidote`, `item_security`.
  
  Sizing used HLSDK precaches instead.
- `hl_sound_refs.vox_words` does not add VOX's default `vox/` directory: 300 sentence words would be unresolved.
- 24 references name sounds absent from the retail data: `holo/holostn1–16.wav`, `holo/holostn10a.wav`, `holo/holointro.wav` (from `holo.mdl` events), `ambience/explode.wav`, `barney/stop3.wav`, `debris/beamstart13.wav`, `headcrab/hc_run2.wav`, `vox/units.wav`, and one parse artefact `ambience/).wav`.
- The HLSDK parse found no link for `ammo_9mmarclip` (placed in a retail map; GoldSrc cannot spawn it), `env_render`, `multi_manager` and `weaponbox`. None of these precache assets.

### 7.3 Pool budget overruns (cook would fail without `--no-budget`; hl64 pools render 240 / texture 128 / collision 208 KiB)

22 maps; values are worst visleaf render / texture and worst grid-cell collision, in KiB:

| map | render | tex | coll | | map | render | tex | coll |
|---|---:|---:|---:|---|---|---:|---:|---:|
| c0a0 | **319** | 78 | 57 | | c2a4a | **249** | 55 | 161 |
| c0a0b | **331** | 53 | 100 | | c2a5 | **411** | 70 | 99 |
| c0a0d | **258** | 81 | 65 | | c2a5w | **360** | 60 | 84 |
| c1a0c | 193 | **140** | 140 | | c2a5x | **352** | 27 | 107 |
| c1a1f | **251** | **151** | 96 | | c2a5e | **372** | 125 | 122 |
| c1a2 | **289** | 100 | 105 | | c4a1 | **284** | 29 | 135 |
| c1a4k | **273** | 59 | 50 | | c4a2 | **252** | 17 | 65 |
| c1a4b | **331** | 88 | 91 | | c4a1b | **295** | 22 | 149 |
| c1a4f | **262** | 91 | 104 | | c4a1e | **332** | 26 | 157 |
| c1a4i | **289** | 107 | 126 | | c4a3 | **1066** | 23 | 201 |
| c2a1a | **242** | 116 | 194 | | c5a1 | 227 | 17 | **208.1** |

The largest render overruns are big open spaces (Surface Tension, Xen, the tram ride in Black Mesa Inbound). In `c2a5`'s worst leaf, 244 of 411 KiB are light probes. `c4a3`'s worst leaf (1066 KiB) is the Nihilanth arena.

## 8. Reproduce

Scratch scripts are in `/scratch/hl64/tmp/CampaignSize/work/` (not committed). Run with `PYTHONPATH=~/src/ports/quakegeneric/tools:~/src/ports/quakegeneric/tools/w64c`.

```bash
S=/scratch/hl64/tmp/CampaignSize
python3 $S/work/embed_wads.py <valve>/maps/<m>.bsp $S/data/maps/<m>.bsp   # WAD textures in
tools/hl_campaign.py --maps-dir $S/data/maps --liblist <valve>/liblist.gam -o campaign.cwg --json campaign.json
$S/work/cook_map.sh <m>          # q64mapc → world_tex_bake → hlecook → w64c.py --no-budget --json
W64C=$S/work/w64c_sz.py $S/work/cook_map.sh <m>     # the 13 COLL-DAG maps
python3 $S/work/chapters.py      # chapters (titles.txt families + chaptertitle placements)
python3 $S/work/hlsdk_res.py $S/work/hlsdk_res.json && python3 $S/work/assets.py   # referenced sets
python3 $S/work/cook_assets.py   # bake_studio, hl_sprite_cook, decals
python3 $S/work/snd.py           # convert_hl_sounds.sh; SOUND_CLASSES=<rules> SND_ROOT= SND_OUT= for levers
python3 $S/work/transitions.py   # boundary carry sets
python3 $S/work/report.py        # tables, levers, split search → work/report.json
```
