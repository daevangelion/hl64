# SummerCart64 campaign streaming (design)

Status: design (2026-11-16), nothing implemented. Direction approved by the
user: the whole cooked campaign plus music is one file on the SummerCart64
(SC64) SD card. The cartridge's 64 MiB SDRAM (the PI ROM window) acts as an
L2 cache that holds the previous, current and next chapter. The engine fills
the next chapter in the background with the SC64 `SD_READ` command. Within a
chapter, streaming stays as built (architecture.md 5): `engine/io` reads
pieces from the ROM window by PI DMA. Saves go to the SD card. Builds for
carts without an SD card stay as they are today.

KiB / MiB are 1024-based. Sizes are the CampaignSize cook as measured
(`campaign-sizing-retail.md`, scenario S0: raw, shipping audio, no LZ4, no
lean models). The compression levers there only shrink these numbers.

## 0. Sources and provenance

- SC64 repository at commit `a1e7996`, read-only clone at
  `/scratch/ref/SummerCart64` (GPL-3.0). We implement against its
  documented N64 interface (`docs/`). We read `fw/` (FPGA RTL) and
  `sw/controller/` (MCU firmware) for behaviour and timing facts only. No
  code from them is copied, paraphrased or translated (PROVENANCE.md,
  "Allowed sources"). Citations below give paths in that repository as
  `path:line`.
- libdragon (our fork `~/src/libdragon-hl64`, Unlicense): `fat_mount` /
  FatFs (ChaN licence) and libcart (`src/libcart`, devwizard, MIT-style
  permission notice) for finding the pack on the SD card at boot.
- meeq's ares fork, branch `sc64` (ISC), is the emulator for functional
  tests (section 12).

## 1. SC64 facts

| # | fact | citation |
|---|---|---|
| F1 | SD bus is 4-bit. The clock is 400 kHz while the card initialises, then 25 MHz. It switches to 50 MHz when the card accepts the high-speed switch (CMD6); a card that doesn't stays at 25 MHz. FPGA clock 100 MHz; SD clock divider modes /256, /4, /2. | `fw/rtl/sd/sd_top.sv:11`; `sw/controller/src/sd.c:359,393,411,416-431`; `fw/rtl/sd/sd_clk.sv:25-30`; `fw/rtl/vendor/lcmxo2/generated/pll_lattice_generated.v:80-82` |
| F2 | A 512 B block is 1024 nibble clocks plus CRC16 and an end bit (block ends at count 1041). The FPGA checks each block's CRC; a mismatch fails the command. Peak rate is stated as ~23.8 MiB/s. Arithmetic: 50 MHz × 4 bit = 25 MB/s raw. | `fw/rtl/sd/sd_dat.sv:273-290`, `:284`; `README.md:23` |
| F3 | `SD_READ` ('s', DATA0 = PI address, DATA1 = sector count) reads from the sector set by `SD_SECTOR_SET` ('I'). The firmware splits it into multi-block reads of at most 256 sectors: CMD18, then wait for the data, then CMD12 (R1b). Each 256-sector chunk has a 5000 ms timeout. After the command the sector pointer advances by the count, so back-to-back slices of one extent need no new `SD_SECTOR_SET`. | `docs/02_n64_commands.md:22-23`; `sw/controller/src/sd.c:38,41,565-600`; `sw/controller/src/cfg.c:705-723`, `:721` |
| F4 | Sector count must be below `0x800000` (4 GiB per command). The destination is checked as one region. The ROM window `0x1000_0000–0x13FF_FFFF` maps to SDRAM `0x0000_0000`, with no `ROM_WRITE_ENABLE` check. The 8 KiB data buffer (`0x1FFE_0000`) and the save window (`0x0800_0000`) are also valid targets. | `sw/controller/src/cfg.c:706-710,187-241` (ROM `:205-210`) |
| F5 | **`SD_READ` into the ROM window does not need `ROM_WRITE_ENABLE`.** That option gates only N64 PI writes (`write_port`). The ares fork behaves the same. | `fw/rtl/n64/n64_pi.sv:241-246` (`:243`); `docs/04_config_options.md:58-65`; ares fork `sc64.cpp:343-347,1215-1245` (section 12) |
| F6 | One command at a time. The MCU main loop runs `cfg_process` → `sd_read_sectors` synchronously. While an `SD_READ` runs, no other command is served: `SD_WRITE`, `CONFIG_*`, and also FlashRAM emulation, save writeback and the rest of the loop. `CMD_BUSY` stays set until it finishes. There is no N64 command to abort a running `SD_READ`. | `sw/controller/src/app.c:38-50`; `sw/controller/src/cfg.c:549-556,705-723`; `docs/01_memory_map.md:111,125` |
| F7 | The command interface is SCR / DATA0 / DATA1 at `0x1FFF_0000`. Registers are locked after power-on or NMI; unlock by writing `0x5F554E4C` then `0x4F434B5F` to KEY. IDENTIFIER reads `SCv2`. On error `CMD_ERROR` is set and DATA0 = type << 24 \| code. | `docs/01_memory_map.md:95-160,217-239`; `sw/controller/src/cfg.c:165-177` |
| F8 | **Completion IRQ:** set `CMD_IRQ_REQUEST` (SCR bit 8) when writing the command id. The cart raises the CART interrupt when the command finishes. Check `CMD_IRQ_PENDING` (bit 27) and clear with IRQ register bit 30 (`CMD_CLEAR`). Interrupts are cleared and disabled on reset, NMI and register lock. libdragon exposes `register_CART_handler` / `set_CART_interrupt`. | `docs/01_memory_map.md:109-123,164-184,229-239`; libdragon `include/interrupt.h:147-163` |
| F9 | SD commands need the card lock. `SD_CARD_OP` init ('i', operation init) takes `SD_LOCK_N64`; `SD_READ` / `SD_WRITE` / `SD_SECTOR_SET` without it fail with `SD_ERROR_LOCKED`. The lock is released while the N64 is held in reset, unless a save writeback is pending. Init on an already initialised card returns at once. | `sw/controller/src/cfg.c:134-137,646-656,696-703,712`; `sw/controller/src/sd.c:348-350,632-645` |
| F10 | SDRAM is 16-bit wide, 100 MHz, CAS 2, burst length 1, one open row (2 KiB) at a time with precharge-all on a row change. Refresh runs every 7.8125 µs. Every requester moves 2 bytes per SDRAM request; the SD DMA writes 2 bytes per request. | `fw/rtl/memory/memory_sdram.sv:14,18-29,78,84,105,157-160,245-257`; `fw/rtl/memory/memory_dma.sv:137-171` |
| F11 | **Arbitration:** N64 PI requests win over the MCU config bus, USB DMA and SD DMA. SD/USB/config SDRAM requests are not granted at all while `pi_sdram_active` is set. That flag is set at the PI address latch (ALE_H) of any ROM-window access and cleared when the PI leaves the data phase. A request already granted finishes first, since one request is one 2-byte access. | `fw/rtl/memory/memory_arbiter.sv:39-42,56-99`; `fw/rtl/n64/n64_pi.sv:161-165,241-246` |
| F12 | **Flow control:** SD data lands in an 8 KiB RX FIFO. When SDRAM is busy (PI active) and the FIFO holds more than 512 B at a block end, the FPGA stops the SD clock, and restarts it when the FIFO drains to ≤ 512 B. A PI burst therefore only pauses the fill; no data is lost and nothing times out short of the 5 s chunk timeout. | `fw/rtl/sd/sd_dat.sv:46-58,257-260,288-291` |
| F13 | The PI read path prefetches through a FIFO from ALE_L. The N64 PI has no wait states: if the FIFO is empty at a read strobe, the word is late, and only debug flags record it (`pi_debug_fifo_flags`). Those flags are visible to the MCU (`REG_DEBUG_0`), not to the N64. | `fw/rtl/n64/n64_pi.sv:318-348,439-470`; `fw/rtl/mcu/mcu_top.sv:653-660` |
| F14 | Save types SRAM / FlashRAM live in the **last 128 KiB of SDRAM** (`0x03FE_0000`), shared with the ROM window. ROM shadow (option 2, default off) maps that last 128 KiB of the ROM window to flash instead. 64DD IPL mode takes SDRAM `0x03BC_0000–0x03FD_FFFF`. libcart sizes the usable cart at 64 MiB, 64 MiB − 128 KiB (save type ≥ SRAM) or less (64DD). | `docs/01_memory_map.md:45-66`; `docs/04_config_options.md:69-78,125-139`; `fw/rtl/n64/n64_pi.sv:151-156`; libdragon `src/libcart/cart.c:1629-1660` |
| F15 | Save writeback (enabled by the SC64 menu for the game's save type) copies the whole save area to the menu's save file on SD 1000 ms after the last save-count change. Writing `SAVE_TYPE` from the N64 disables writeback. | `sw/controller/src/writeback.c:22,78-92,163-190`; `docs/04_config_options.md:139` |
| F16 | `SD_WRITE` ('S') works like `SD_READ`: chunks of ≤ 256 sectors, CMD25 + CMD12, source in SDRAM / flash / 8 KiB buffer. | `sw/controller/src/sd.c:533-563`; `sw/controller/src/cfg.c:725-743` |

### 1.1 Derived limits [INFERENCE from F10-F12; confirm on hardware, section 13]

- **Added latency to a game PI DMA while a fill runs: at most one SDRAM
  access already granted to the SD DMA.** That is ≈ 7 clocks on a row
  change (precharge + activate + write), plus ≈ 6 if a refresh is due:
  ≤ ~150 ns. It is paid during the PI address phase (the arbiter blocks
  the SD DMA from ALE_H, data prefetch starts at ALE_L). After that the
  whole burst runs with the SD DMA shut out. **PI reads are never blocked
  for the length of an `SD_READ`.** It is the other way round: the fill
  pauses during every PI burst.
- So blk1ms (no frame blocks > 1 ms on cart I/O) does **not** limit the
  fill slice size. The fill costs the CPU a few PI register accesses per
  slice (section 6.2), not waits.
- Fill throughput ≈ SD rate × (1 − PI duty). At the 64 KiB/frame I/O
  budget (≈ 12 ms of PI per 33 ms frame at 5.25 MB/s) the duty is ≤ 36 %,
  so the fill keeps ≥ 64 % of the SD rate. The SD rate is ≤ 23.8 MiB/s at
  50 MHz and about half that at 25 MHz, less the per-256-sector CMD18 /
  CMD12 turnaround and the card's access time, both card-dependent. This
  design budgets **10 MiB/s sustained** until measured.
- What does limit the slice size is F6: a running `SD_READ` delays every
  other SC64 command (a save's `SD_WRITE`, a music refill, aborting a fill
  when the player turns back) by up to one slice.

## 2. Content split: common region + chapter packs + music

From the CampaignSize data (`/scratch/hl64/tmp/CampaignSize/work`:
`chapters.json`, `assets.json`, `asset_sizes.json`, `sound_sizes.json`,
`report.json`):

| part | contents | MiB (S0 raw) |
|---|---|---:|
| fixed | code + header, HUD, decals, menu, tables, global models / sprites / sounds (every map) | 8.87 |
| shared | non-global assets used by ≥ 2 chapters: models 6.28, sprites 0.83, sounds 8.70 | 15.81 |
| **common** | fixed + shared, resident for the whole session | **24.68** |
| chapter "own" pack | the chapter's `.w64` maps + assets only that chapter uses | 0.89 (Xen) … **10.19 (On A Rail)** |
| music | 25 CD tracks, stereo 4-bit VADPCM 22050 Hz | ≈ 56 |

Chapter own packs (MiB): 1 8.47, 2 4.98, 3 6.06, 4 4.85, 5 3.16, 6 7.60,
7 3.08, 8 10.19, 9 4.87, 10 4.28, 11 3.82, 12 9.23, 13 2.76, 14 5.62, 15 0.89,
16 1.78, 17 4.87, 18 3.25, 19 2.24; Hazard Course 7.91, Uplink 3.48. Sum:
92.0 for the campaign, 103.4 with Hazard + Uplink.

How the options compare (largest consecutive window of the 19 campaign
chapters):

| window | chapter-complete packs, naive sum | chapter-complete, deduplicated within the window | own packs (common resident) |
|---|---:|---:|---:|
| 1 chapter | 20.00 (12) | 20.00 (12) | 10.19 (8) |
| 2 | 32.16 (8+9) | 26.13 (8+9) | 15.05 (8+9) |
| 3 | **44.41 (6+7+8)** | 32.89 (6+7+8) | 20.86 (6+7+8) |

Main's brief quoted 43.84 for the largest triple, which is 12+13+14. The
larger one is 6+7+8.

**Choice: own packs, with the shared assets in the resident common
region.** With equal fixed slots, chapter-complete packs need 3 × 20.00 +
8.87 = 68.9 MiB and do not fit. A within-window deduplication would need a
per-file allocator and reference counts across slots. With the shared
assets resident, every chapter pack fits one fixed-size slot, 3 slots
cover the window, and a chapter change refills exactly one slot. The cost
is 15.81 MiB of shared assets resident in SDRAM, where the cart-split plan
duplicated them per cart.

**Music is not part of the chapter slots.** 56 MiB cannot be resident, and
a track plays for minutes at ≈ 24.8 KB/s
(22050 / 16 × 9 B × 2 channels). Tracks are stored once in the pack and
streamed from SD through a small ring (section 4). "Per-chapter music" is
each chapter directory's list of the track ids its maps trigger
(`trigger_cdaudio` / `target_cdaudio`). The list drives the install check
and the warm-up of the chapter's first track (section 7).

## 3. Pack file format (`sd:/hl64/hl64.pak`, big-endian, z64 byte order)

The file is written in N64 byte order and the fill runs with byte swap off,
so the SDRAM bytes match the file bytes.

```
offset 0      header, 4096 B
              magic 'H64P', version u16, flags u16
              build_id u32      CRC-32 of the ROM's resource table + campaign.cwg;
                                must equal the running ROM's (rejects mixed installs)
              slot_bytes u32    chapter slot size the cook chose (multiple of 64 KiB)
              n_packs u16, n_tracks u16
              pack[n]  { id u8 (1..19, 'H', 'U'), pad, sd_off u32 (4 KiB aligned),
                         bytes u32 (<= slot_bytes), crc32 u32 }
              track[n] { cd u8, pad, sd_off u32 (4 KiB aligned), bytes u32 }
              header crc32 u32 (last word)
then          chapter packs, each 4 KiB aligned
              pack header { magic 'H64C', id, n_files u16, n_tracks u8,
                            track ids[], file[n] { file_index u16, off u32, bytes u32 } }
              file data, offsets relative to the pack start, aligned as the DFS
              packer aligns ROM files today (PI DMA rules unchanged)
then          music tracks: cooked media/cdNN.wav64 as today
```

- `file_index` is the index of the engine's file table (architecture.md
  5.1). The resource table keeps referring to files by index. The cook
  marks each file `COMMON` (in the ROM's DFS) or `CHAPTER n`.
- 4 KiB alignment keeps every pack sector-aligned for `SD_READ` and
  cluster-friendly on FAT.
- Per-pack CRC-32 is checked by the PC installer, not by the N64. On the
  N64, data integrity comes from the per-block SD CRC16 (F2), which fails
  the command. Reading 10 MiB back through the PI to checksum it would cost
  ≈ 2 s of PI time.

## 4. SDRAM layout (PI `0x1000_0000` + offset)

| region | offset | bytes (S0) | filled by |
|---|---|---:|---|
| common: ROM image (IPL3, code, DFS with fixed + shared files) | 0 | 24.68 MiB | the SC64 menu loading the `.z64` (as any ROM) |
| chapter slot A | 64 KiB-aligned after common | `slot_bytes` = 10.25 MiB | `SD_READ` |
| chapter slot B | | 10.25 MiB | `SD_READ` |
| chapter slot C | | 10.25 MiB | `SD_READ` |
| music ring | | 1 MiB (≈ 42 s of a track) | `SD_READ`, 4 × 256 KiB quarters |
| free | | ≈ 7.45 MiB headroom | — |
| reserved: save area / ROM shadow | `0x03FE_0000` | 128 KiB | never touched (F14) |

The total is 56.55 MiB of 64. `slot_bytes` is the largest own pack rounded
up to 64 KiB. The cook fails if common + 3 × `slot_bytes` + ring + 128 KiB
exceeds 64 MiB. At boot the engine refuses SD mode if `DD_MODE` ≠ 0 (F14:
the 64DD IPL would overlap slot C and the ring).

`ROM_WRITE_ENABLE` stays **off** (F5). No N64 PI write ever lands in SDRAM,
so a stray write cannot corrupt the cache. The save path stages through the
8 KiB buffer (section 9).

## 5. Finding the pack on the SD card (boot)

1. Detect SC64: KEY reset + unlock sequence, IDENTIFIER == `SCv2` (F7);
   `CONFIG_GET DD_MODE` == 0; `SD_CARD_OP` init (takes the lock, F9).
2. Mount FAT with libdragon's public `fat_mount("sd:/", &disk, 0)`
   (`include/fat.h:244`) over a `fat_disk_t` whose `disk_read` is libcart's
   `cart_card_rd_dram` (`include/libcart/cart.h`; blocking, through the
   8 KiB buffer; boot only). Do not use `debug_init_sdfs`: it is compiled
   out under `NDEBUG` (`include/debug.h:148,235-240`).
3. `open("sd:/hl64/hl64.pak")`. Read the 4 KiB header and check magic,
   header CRC and `build_id`.
4. Build the extent list (file offset → LBA runs).
   `ioctl(fd, IOFAT_GET_CLUSTER_SIZE)` gives sectors per cluster. For each
   cluster k: `lseek(fd, k × cluster_bytes + 1, SEEK_SET)`, then
   `ioctl(fd, IOFAT_GET_SECTOR)` gives the cluster's first LBA. Merge
   consecutive LBAs into runs.
   - libdragon's FatFs is built with `FF_FS_TINY 1`
     (`src/fatfs/ffconf.h:230`). A mid-sector seek then only sets `sect`
     and reads no data (`src/fatfs/ff.c:4700-4711`).
   - Forward seeks follow the chain from the current cluster
     (`ff.c:4649-4653`), so the walk reads only FAT sectors: 128 entries
     per FAT32 sector, ≈ 50 sector reads for a 200 MiB file with 32 KiB
     clusters. exFAT files flagged contiguous need no FAT reads.
   - `FF_USE_FASTSEEK` and `FF_USE_EXPAND` are off (`ffconf.h:40,44`),
     hence the seek walk. `__fat_lseek` takes an `int` offset (`src/fat.c:267`),
     which is fine below 2 GiB.
5. Keep at most 64 runs in RDRAM (8 B each). A file on a fresh card is one
   run. More than 64 runs → message "re-copy hl64.pak to a freshly
   formatted card". Same for `sd:/hl64/save.bin` (section 9).
6. `fat_unmount`. No FatFs at run time: fills and saves address LBAs
   directly.

## 6. The fill engine (`engine/io`, new `plat/n64/sc64.c`)

### 6.1 Slots

Each slot has `{pack id, state EMPTY | FILLING | READY, bytes done}`. The
engine's file lookup maps a `CHAPTER n` file to `slot_base(n) + off`. A
request for a file whose slot is not READY is "not resident":

- prefetch / near requests are dropped (counted);
- open / pin requests should not happen: the seam gate (section 7) keeps
  them from happening;
- a sync read (spawn) is made only after `fill-then-play` (section 8).

A slot is never READY while it is being written, so a game PI read never
races a fill.

### 6.2 Commands and PI ownership

SC64 register accesses are PI accesses. They follow `plat/n64/flash.c`'s
rule and are made only while the io chain is idle between pieces:

- from the PI-completion handler before it launches the next piece, or
- from `io_pump` when no piece is in flight.

A slice is 2-5 register writes: `SD_SECTOR_SET` at an extent start (DATA0,
SCR), then `SD_READ` (DATA0, DATA1, SCR). Completion is one SCR read per
frame at `io_pump` (`CMD_BUSY` clear, `CMD_ERROR` checked). The CART IRQ
(F8) is used only on loading screens, to chain slices back to back; its
handler only sets a flag, and the next idle point issues the next slice.

### 6.3 Slice size and priorities

- Slices never cross an extent end or a slot end.
- **Chapter fill slice: 1024 sectors (512 KiB).** Per F3 a larger command
  saves nothing on the SD side, since the firmware splits at 256 sectors
  anyway. 512 KiB occupies the MCU ≈ 21 ms at 23.8 MiB/s and ≈ 50 ms at
  10 MiB/s, and that is the longest a save write or music refill waits
  (F6). One slice per frame caps the fill at 15 MiB/s at 30 fps, above the
  budgeted SD rate. The largest pack (10.19 MiB) then fills in ≈ 1.0 s at
  10 MiB/s, or ≈ 2.7 s at 3.75 MiB/s (a slow 25 MHz card under full PI duty).
- Priority when the previous command completes: **music quarter >
  save write > fill of the slot the player is heading toward > other
  fill.** A chapter change re-targets the fill after the in-flight slice
  (no abort command exists, F6).
- Tunables (`SC64_FILL_SECTORS`, `SC64_MUSIC_SECTORS = 512`) are fixed
  after the hardware measurement (section 13).

### 6.4 Music ring

`snd_hw`'s music stream today reads `[rom, rom + size)` linearly. On SC64
it reads through the ring instead:

- track offset `o` lives at `ring + (o mod 1 MiB)`;
- a quarter is refilled from SD when the reader enters the quarter before
  it (3 quarters ≈ 31 s ahead);
- a loop restarts from track offset 0 with the same rule.

A track starts when its first quarter is READY: wait for the in-flight
slice (≤ ~50 ms) + 256 KiB (≈ 11-25 ms). Reader past the filled
watermark → silence and an `mus_underrun` counter. That should be
impossible at ≥ 3 MiB/s against 24.8 KB/s.

## 7. Chapter transitions and backtracking

The campaign is linear. Every boundary has one forward changelevel (two
into `c1a0c`) and one back changelevel to the previous chapter
(campaign-sizing-retail.md §1, §6.3), so the resident set is
**{cur − 1, cur, cur + 1}**.

- **Crossing into an adjacent chapter.** The new set differs from the old
  one by exactly one chapter. That chapter's slot is the one being
  refilled: going forward the slot of cur − 2 receives cur + 2, going
  back the slot of cur + 2 receives cur − 2. The chapter the player
  enters is already READY.
- **Seam gate.** A seam whose B map is in a chapter that is not READY does
  not open (architecture.md 5.4 step 1 waits). If the player reaches the
  switch box anyway (the fill lags, or the player turned back and forth
  faster than a refill), the switch holds with a "Loading" overlay until
  the slot is READY: a GoldSrc-style pause, counted (`seam_wait_ms`). At
  ≥ 3 MiB/s the worst case is the 10.19 MiB pack, ≈ 3.4 s, and only when
  the player crosses two boundaries within one fill. Single-map chapters
  (Xen, Nihilanth, Endgame) put both boundaries in one map; their
  neighbours' packs are small (Gonarch's Lair 1.78, Interloper 4.87,
  Lambda Core 5.62 MiB).
- **Fill order** after a crossing: the slot in the direction of travel
  first.
- **Music warm-up.** When a chapter becomes cur + 1, the first quarter of
  its first listed track is read into a spare quarter, if the ring is idle.
- **Hazard Course and Uplink** are separate games from the menu: their
  pack takes slot A, the others are EMPTY.

## 8. Save / load across chapters

- A save records its chapter (the home layer's map → chapter, from the
  cooked graph).
- **Load / New Game / chapter select**, target chapter t:
  1. If t is READY, load as today, then re-target the other two slots to
     {t − 1, t + 1}.
  2. Otherwise **fill-then-play**: a loading screen fills t first with
     back-to-back slices (IRQ-chained, the PI otherwise idle), ≈ 0.45 s
     at 23.8 MiB/s and ≈ 1 s at 10 MiB/s for the largest pack. Then the
     load and its load I/O run as today (architecture.md 5.3 "spawn"), and
     t ± 1 fill in the background during play.
- Nothing in the save format changes: chapter residency is not level
  state.

## 9. Saves on the SD card

- **File.** `sd:/hl64/save.bin`, created by the PC installer at 128 KiB of
  zeros (8 × 16 KiB "sectors", the FlashRAM pool's geometry). It is
  located at boot like the pack (section 5) and never resized: no FAT
  writes at run time.
- **Same pool, new medium.** `flq.c` / `save.c` keep the FlashRAM layout
  and rules of architecture.md 14 (FlashRAM bullet):
  - 1 or 2 whole sectors per save;
  - a new copy goes only to sectors no current copy holds;
  - of 2 sectors the second is written first and the header's sector
    last;
  - settings copies under the same rule.
  `test/save/flashcut.sh`'s worst case carries over unchanged.
  The backend changes only:
  - erase is a no-op;
  - program = PI-DMA up to 8 KiB into the SC64 data buffer
    (`0x1FFE_0000`, F4), then `SD_SECTOR_SET` + `SD_WRITE` of ≤ 16
    sectors from it, repeated. A 16 KiB sector is 2 rounds; the
    largest image (32256 B) is 4;
  - read = `SD_READ` into the buffer + PI DMA out (or one read at boot
    through the extents).
  Each round is one queued SC64 command (priority in 6.3), and the existing
  `sys_flash_poll` style (start, poll next frame) still applies.
- **Atomicity against power loss.**
  - SC64 level: a write touches only the new copy's sectors. A cut mid-copy
    leaves a copy whose header is missing or fails its CRC, so the
    previous copy is used.
  - This is why hl64 does **not** use the SC64 menu's FlashRAM writeback
    (F15). Writeback rewrites the whole 128 KiB `.sav`, including the
    sectors holding the good copies, one second after the last change.
  - SD-card level: whether a cut during a write can damage *other* recently
    written sectors inside the card is down to the card's FTL. This can only
    be checked on hardware (section 13). The header-last + CRC rule covers
    any torn sector, but not FTL collateral damage. The installer places
    `save.bin` in its own file, away from the pack.
- **FlashRAM on SC64** (the ROM header's save type) is left untouched in
  SD mode, so the save-count never changes and writeback never fires
  (F15). If `save.bin` is missing or has too many runs, the build falls
  back to the FlashRAM backend as today (the menu's writeback then
  persists it).

## 10. Fallbacks (no SD, not an SC64)

- **Two build products:**
  - `make rom` stays as today: one game image in the ROM's DFS (Uplink or a
    chapter subset), FlashRAM + Controller Pak saves, no SC64 code path
    taken;
  - `make rom-sc64` builds the common-region ROM, `hl64.pak` and the
    installer layout.
- `rom-sc64` without an SC64, without a card, with `DD_MODE` ≠ 0, without
  a pack, or with a `build_id` mismatch shows a message screen naming the
  missing piece. It has no maps to fall back to. Detection never writes
  anything.
- **Same engine code in both builds.** Only the file table's `CHAPTER n`
  entries (absent in `make rom`) and `plat/n64/sc64.c` (linked, unused)
  differ.

## 11. Cook and install changes

- `tools/` pack step (after `make cook`):
  - chapter membership from the campaign graph (CWG3 / `campaign.json`
    chapters);
  - asset → chapters use sets as in `report.py`'s attribution: global set,
    multi-chapter set → COMMON, single-chapter → that chapter;
  - map `.w64` → its chapter;
  - track ids per chapter from `trigger_cdaudio` / `target_cdaudio`;
  - `slot_bytes`;
  - the layout check (section 4);
  - writes `hl64.pak` and the file table locations (`COMMON` / `CHAPTER n`
    + offset).
- The resource table / io file table gains the location field. Format doc
  update: `docs/formats/rtb-format.md` (or wherever 5.1's table is
  specified) plus a new `docs/formats/pak-format.md` from section 3 when
  implemented.
- **Installer** (`tools/sc64_install.py`): copies `hl64-sc64.z64`, `hl64.pak`
  and a zeroed `save.bin` (only if absent) to `sd:/hl64/`, verifies every
  pack CRC after the copy, and reports each file's fragment count (via the
  host OS's FIEMAP / `fsck` listing where available).
- Never committed: cooked packs, ROMs, SD images (PROVENANCE /
  M3-contract rule).

## 12. Testing

- **Host build** (`plat/host`). The SDRAM window is the host cart image,
  64 MiB as on the cart. The SC64 model is `plat/host/sc64_host.c`:
  - `SD_READ` / `SD_WRITE` against a host file;
  - completion after a modelled time (default 10 MiB/s, slowed by the
    frame's PI duty as in 1.1), with F6's one-command-at-a-time rule;
  - fault injection: CRC error, timeout, card removed, slow card
    (2 MiB/s).

  Gates:
  1. blk1ms = 0 over a route crossing two chapter boundaries forward and
     one back;
  2. `seam_wait_ms` = 0 at 10 MiB/s, and bounded by section 7 at 2 MiB/s;
  3. `mus_underrun` = 0;
  4. save power-cut injection at every `SD_WRITE` sector, extending
     `flashcut.sh`: the newest complete copy always loads;
  5. fill-then-play load of a save from a non-resident chapter.
- **ares** (meeq's fork, branch `sc64`, adopted by SC64Emu). It emulates
  SDRAM, KEY / IDENTIFIER, `SD_SECTOR_SET` / `SD_READ` / `SD_WRITE` and the
  command IRQ, with every command completing immediately and no timing
  model. `SD_READ` into the ROM window works with `ROM_WRITE_ENABLE` off,
  as on hardware (F5). Writeback, 64DD mapping and SC64 SRAM/FlashRAM
  emulation are not modelled (ares fork commit `0a0a789c8e`, `sc64.cpp`).

  Hence:
  - functional tests: boot detection, the extent walk on a FAT32 SD image
    built with mtools, including a deliberately fragmented `hl64.pak`; slot
    state machine; chapter crossing and backtrack routes; SD saves.
  - Timing paths come from the engine: `HL64_TEST` builds hold a command's
    completion back by the modelled time, the same model as the host.
  - The existing ares PI model (fixed 5.25 MB/s, no contention) is
    unchanged, so ares says nothing about F11/F12 interaction.
  - All ares runs on ares-host (`HL64_ARES_HOST=ares-host`).

## 13. What only real hardware can confirm

1. Sustained `SD_READ` rate per card (HS 50 MHz vs 25 MHz) and per slice
   size (256 / 1024 / 4096 sectors). Also the per-command overhead: MCU
   loop latency + CMD18 / CMD12 turnaround + card access time (F3, F6).
2. Fill throughput while the game streams at the full I/O budget: F12's
   clock-stop pacing, the 1 − PI-duty estimate.
3. **No PI corruption during fills**:
   - a test build reads every piece with a cook-side CRC while slices run
     back to back, at the ROM header's PI DOM1 timing (F11 / F13: the ≤
     ~150 ns added before the first strobe);
   - plus, via USB, the MCU's `pi_debug_fifo_flags` after a soak.
4. blk1ms, `seam_wait_ms` and `mus_underrun` on the Uplink + chapter
   routes with real fill timing.
5. Power cut during `SD_WRITE` on a few card brands: collateral damage
   beyond the written sectors (section 9).
6. Card-init behaviour after the SC64 menu boots the ROM (F9: does init
   return at once, is the lock free), and after console reset (NMI).
7. Boot time: menu load of the ≈ 25 MiB `.z64` + extent walk + first
   chapter fill.

## 14. Prior art checked

meeq's N64 projects (survey 2026-11-16, read-only; licences per GitHub):

- wonderswan-n64 (no licence file) pages a guest ROM from `sd:/` into
  RDRAM, through `debug_init_sdfs` + unbuffered `fseek` / `fread`, blocking
  on page faults, 64 KiB pages.
- mac64 (no licence file) reads a ~1 MB writable disk image from `sd:/` the
  same way.
- cd-i-n64 (no licence file) and lego-island-n64 (LGPL-3.0) stream only
  from cart ROM. cd-i states "~5 MB/s PI" for cart reads.
- His libdragon fork has no SD / SC64 branch.

None uses `SD_READ` into SC64 SDRAM, builds a sector map, or reports an SD
throughput figure, so section 13's measurements have no published
substitute. Nothing from these projects is reused.
