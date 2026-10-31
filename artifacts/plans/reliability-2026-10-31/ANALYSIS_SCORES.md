# En Croissant analysis: missing UCI scores (fixed 2026-10-31)

## Report

Koi's analysis mode in En Croissant showed a constant 0.00 evaluation, even in
positions where a queen was lost or mate was coming, and the displayed lines
looked unlike normal engine play.

## Root cause

The UCI writer published `score cp/mate` only when `SearchInfo::Bound` was not
`estimate`.  Selective root iterations almost always end up with
`Bound::estimate` in middlegame positions (their root lines carry unproven
selective bounds), and the runner's "republish the last proven score" fallback
has nothing to republish until a fully authoritative iteration exists.  Result:
`info depth ...` lines contained no `score` token at all.  GUIs then leave the
evaluation at their default 0 cp (En Croissant's shakmaty-uci parser and
`BestMoves` default score), which is exactly what the user saw.

Evidence: an En-Croissant-style transcript (`uci`, `setoption MultiPV`,
`isready`, `position`, `go infinite`, `stop`) produced 12 info lines and **0**
with a score before the fix; the same transcript produced **11/11** scored lines
after it.  The behaviour was present in the released 1.1.5 build as well.

## Fix

`UciController::write_search_info` now always emits the score token.  UCI has no
"estimate" marker; omitting the number is worse than publishing the engine's
current best estimate (every other UCI engine publishes a score).  Internal
consumers still see `Bound::estimate` and must not treat it as exact; lower/upper
bound markers are unchanged, and WDL remains suppressed for unproven estimates.
The contract test was renamed and now asserts an unmarked estimate score rather
than score suppression; the resource-churn soak additionally asserts that a
normal search publishes at least one scored `info` line.

Commit: `fix(uci): publish estimate scores so GUI analysis shows evaluation`.

## Verification

- `selective root score provenance` unit test: PASS (estimate published,
  unmarked, no WDL/bound marker).
- `koi_protocol_fuzz_process` and `koi_protocol_soak_process`: PASS.
- Live transcript with `go infinite` on `r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/...`:
  11 info lines, 11 scored (e.g. `info depth 5 ... score cp 70 ...`,
  `info depth 6 ... score cp 58 ...`).
- Docs corrected in ENGINE_SPEC §7 and USER_GUIDE (estimate scores are published
  unmarked; WDL stays proven-only).
