# Clean-room incidents

Exposures of clean-side agents to flagged material, and their resolution.

| date | agent | what happened | resolution |
|---|---|---|---|
| 2026-11-13 | CookImport / W64cSplit | A relative-path edit overwrote lines 1–58 of the old tree's `tools/w64c/w64c.py` (an import source, not a forbidden file). No flagged lines were displayed. | Restored from the redacted copy at once; byte-compared equal to a scratch copy of the old tree. |
| 2026-11-13 | CookImport / SeamCheck | A rejected edit displayed the audit-flagged `w64seam.py` lines 77–83 (a 7-line angle basis). | The agent kept its own version, written beforehand from HLSDK `AngleVectors`; results are numerically identical. |
| 2026-11-13 | M0Core | While reading the clean ranges of `r64_vid.c`, a context line showed flagged lines 162–164. | Not used; the retarget took only ranges 59–150 and 214–251. |
| 2026-11-13 | M1Render | Reads of `r64_sector.c` displayed 3 context lines of each flagged range (431–433, 634–636). | Not used; texture animation and dlight falloff written from world-surfaces.md and dlights.md. |
