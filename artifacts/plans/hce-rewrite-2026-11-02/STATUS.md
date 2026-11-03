# HCE handoff status

Status: active rewrite (rounds 1-35 so far). Release remains `1.1.5`; no version
bump, tag, or packaging work.

## What exists

- `src/koi/classical_evaluator.{hpp,cpp}`: clean, stateless, thread-safe HCE.
  Tapered mg/eg evaluation with material/PST, mobility, pawn structure,
  king safety, threats, bishop pair/rook/outpost terms, space/development/tempo.
- Native inputs: HCE builds its own bitboards/attack/mobility maps from
  `GameState::copy_board_to` and the native attack tables; it does not use the
  shared NNUE `PositionFeatures` container.
- Explicit mg/eg PST tables for every piece (tuning-friendly; no hidden formula
  coupling).
- Internal tuning seam (`KOI_HCE_*_SCALE`, default 100; `KOI_HCE_EVAL_SCALE`)
  for one-binary parameter screens. Not a public UCI surface.
- Integration: `EvalMode=nnue|hce` UCI option; `KOI_EVALUATOR=hce` boot override
  without a network; `EvalFile` remains NNUE-only.
- Tools: `koi-bench --hce`; `koi-eval-features --hce` (static eval corpus dumps);
  HCE benchmark regression test (`koi_hce_bench_process`); HCE process test.
- Tests: `classical_evaluator_tests` (term directions, pawn-structure/tempo,
  endgame corpus invariants, determinism, bounds, concurrency, seeded random-game stress), 9/9. ASan-clean.
  Controller evaluator-mode transitions (68/68), `koi_hce_process`,
  `koi_hce_bench_process` (>=60/64 depth-3 tactical matches),
  `koi_hce_eval_features_process` (HCE CSV + mode conflicts).
- Evidence: `BENCH.md` (depth-5 bench), `GATES.md` (Mac screen/confirm/300-game
  gates), `EVAL_CORPUS.md` (HCE-vs-NNUE static comparison), `PROFILE` note
  (search/quiescence dominates, eval is not the time bottleneck).

## Accepted strength evidence

- v3 passed-pawn king-proximity/blocking: consistently positive in screens
  (+72.5/64, +30.6/126, +25.6/300 Elo) but never crossed the SPRT boundary;
  retained as the current baseline, no Elo claim.
- No other candidate survived confirmation. Reverted: combined v2 term set,
  backward pawns, rook-behind-passer, pawn threats, mop-up, weighted
  king-attack model, safe mobility, NNUE-fitted PST correction.
- Single-group tuning matrix (passed/mobility/king safety/pawns/tempo/threats/
  piece terms/eval scale) closed; no variant confirmed positive.

## How to use

- HCE engine: `KOI_EVALUATOR=hce ./koi-engine` or `setoption name EvalMode value hce`.
- Bench: `koi-bench --hce --depth 5`.
- Static eval corpus: `koi-eval-features --hce --input fens.txt`.
- Tests: `ctest -R 'classical_evaluator_tests|koi_hce_process|koi_hce_bench_process'`.

## Next

- Larger structural evaluation ideas rather than single-weight screens.
- Longer equal-time confirmation (>=300 games) for any screen-positive change.
- Search/eval synergy investigation (HCE score interaction with pruning).
EOF

## Recent state (rounds 31-47)

- Native board builder restored after a silent regression (round 31); behaviour
  screen was neutral-positive and the architecture is retained.
- Added `EvalMode`/`EvalFile` state-sync fix and transition regression tests.
- ASan fuzz per-case timeout hardened to 45 s for loaded CI runners.
- Additional unit/tool tests and the consolidated evidence were added.
- Further single-term candidates (safe mobility, pawn islands, pawn storm,
  minor threats, check-aware tempo, passed-rank scale) screened and reverted:
  none survived confirmation/large gates.
