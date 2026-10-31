# NEON delta kernel — negative result (2026-10-31)

Profiler candidate #1 was an arm64 NEON kernel for
`NnueWorker::apply_feature_delta` (NNUE accumulator row update, inside
`on_make_move`, ~50% of search CPU; the arm64 build otherwise uses the scalar
int64-clamp loop).

## Attempt 1: int64 widening + saturating narrow
- 4 int32 lanes: widen int16 weight to int32, widen current+delta to int64,
  add/subtract, `vqmovn_s64` back to int32.
- Bit-identical to scalar: yes (completed iteration lines identical on 4 FENs
  at depth 7, including nodes/scores/PVs).
- **NPS: 797,489 vs baseline 865,178 (−7.82%)**, measured Mac M4, startpos
  `go nodes 2,000,000` T1, 5 alternating trials, medians.

## Attempt 2: int32 saturating add
- Observation: a row weight is at most ±32767 and the accumulator is int32, so
  the exact int64 add-then-clamp equals a signed saturating int32 add.
- Implementation: `vmovl_s16` / `vnegq_s32` / `vqaddq_s32` / `vst1q_s32`,
  4 lanes, scalar tail. Also bit-identical on the same 4-FEN depth-7 check.
- **NPS: 807,167 vs baseline 864,826 (−6.67%)**, same methodology.

## Why
AppleClang already auto-vectorizes the scalar int64 loop very well on M4, and
the manual 4-lane `int16→int32` widening + saturating add costs more per lane
than the compiler's schedule (load/convert/store overhead, 4-lane granularity
vs wider auto-vectorized blocks). The refresh-buffer fix was present in both
attempts, so it does not explain the loss.

## Decision
Do **not** add a NEON delta kernel. Reverted; only the refresh-buffer reuse
(`d49ce0f`) stays. If this is revisited, the only defensible angle is a wider
auto-vectorizable formulation with measured bit-identical output — not a
narrower manual kernel. Recorded here so the experiment is not repeated.

Baseline Mac binary: `d757b4a1…` (1.1.5 build). Candidate builds:
`f1955dc6…` (attempt 1), `333812cd…` (attempt 2). Both compared with all
completed depth lines identical, so this is purely an NPS regression.
