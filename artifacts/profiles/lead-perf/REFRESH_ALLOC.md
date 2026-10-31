# NNUE refresh-buffer allocation fix (2026-10-31)

## Finding
Profiler interim (Mac `sample`, 1.1.5 hot path): `NnueWorker::refresh_slot_perspective`
(`src/koi/nnue.cpp:1957`) allocated `std::vector<std::int32_t> sums(hidden_units_, 0)`
on **every** full refresh, i.e. inside the hot make/unmake path on every
king-bucket crossing (~7.4 % of makes per the 2026-10-27 x86 profile).

## Fix
- `NnueWorker` now owns `refresh_sums_` (`src/koi/nnue.hpp`), sized once per
  worker and zero-filled with `std::fill` per refresh (`src/koi/nnue.cpp`).
- No other logic changed: same scalar `accumulate_hidden_sparse_scalar`, same
  copy to slot values, same bucket/piece caching.

## Behavior verification (MSVC Release, promoted h128 net, Threads=1)
Baseline binary (pre-fix) SHA-256:
`b0f972e6c2f4e3d36e65ac5ac64bab7947228e79431c0435f0f64f901cf74ac3`
Patched binary SHA-256:
`88a33a583fb7aad53e869d50da54007c18403e861749934baa9c04040cdee363`

Compared completed iteration lines (no `currmove`, `nps`/`time` stripped) for
depth 7 on four FENs (startpos, kiwipete, Italian, en-passant-ish endgame):
**identical depth/seldepth/multipv/score/nodes/hashfull/PV and identical
bestmove** in all four cases. Interim `currmove` publication lines differ only
in timing-dependent live node counts (expected). Incremental NNUE tests
(`NNUE v4 incremental walk/king bucket/recovery`, `worker isolation`) pass.

## NPS
Windows noisy host, `go nodes 1500000` startpos, 4 alternating trials:
baseline median **377,355 NPS**, patched **381,501 NPS** (~+1.1 %, within
noise; the refresh path is a small fraction of wall time). Windows is not a
gate; a quiet-Mac NPS/bench comparison and the LTC/200 ms SPRT decision are
pending (profiler still owns the Mac; bot must not be disturbed).

## Status
Committed as a behavior-preserving hot-path hygiene fix. Do **not** claim an
Elo gain; it needs the normal NPS gate plus a strength gate if it is to be
described as a strength change. Two failing local unit tests
(`NNUE network required`, `NNUE v4 corruption`) are the known `%TEMP%`
sandbox `create_directories: Access is denied` environment quirk, not this
change.

## Follow-up: quiet-Mac NPS measurements and v4 threat-copy skip

Mac M4, startpos `go nodes 2,000,000` T1, 5 alternating trials, medians, bot
paused between games (documented placeholder + autostart; restored after):

| Candidate | Median NPS | vs 1.1.5 baseline |
|---|---|---|
| 1.1.5 baseline (`d757b4a1…`) | 863,055 | — |
| refresh-buffer only | 865,179 | **+0.25 %** (noise) |
| refresh + skip v4 threat-slot copies | 888,718 | **+1.07 %** |

The v4 threat-slot skip is behavior-identical: profiler showed v4 halfka
networks never use the threat buffers, yet `copy_slot_forward` and the scratch
path copied 2 x 128 x uint16 plus counts per make. The buffers are now allocated
and copied only when `uses_halfka_threat_features(...)`. Verified on Windows:
completed iteration lines identical on four FENs at depth 7 vs the pre-change
binary, all 15 incremental NNUE tests pass.

Neither candidate is an Elo claim; combined ~+1% NPS. NEON delta kernel was
rejected separately (`NEON_DELTA_NEGATIVE.md`).
