# Quiescence delta-pruning margin 100 -> 200: neutral (2026-10-30)

## Motivation
The quiescence delta-pruning cushion (`best + captured_piece_value + margin <
alpha`) still used 100, a value inherited from the retired classical evaluator's
narrower score range. The pinned h128 has a wider NNUE score range, so the
reference-like 200 was expected to prune more hopeless captures and buy search
depth.

## Candidate
- `SearchPolicy::quiescence_capture` delta cushion 100 -> 200.
- Commit `366158b`, Mac arm64 candidate SHA-256
  `9746b34cb293b0bb66872981d68cd05ac8fd94c28164def3f3ecdc756166659f`.
- Baseline: current HEAD before the change, Mac binary SHA-256
  `4e19ce2b448533578ac9d15b914650adcc40b18f4b441494f3aa2c1a100445cc`.
- Windows search/strength/soak suites green before the gate.

## Gate (Mac arm64, quiet window)
- 300-opening duplicate-free corpus, 1 game/opening/colour = **600 distinct
  games**, 200 ms, Threads 1, Hash 64, promoted h128 pinned both sides.
- White: 300 games, 105 W / 86 D / 109 L, -4.6 Elo, LLR -1.01.
- Black: 300 games, 110 W / 91 D / 99 L, +12.7 Elo, LLR +0.20.
- **Combined: 215 W / 177 D / 208 L = 50.58 %, +4.1 Elo, LLR -0.819 →
  inconclusive** (below the +20 H1, evidence weakly against it).
- No duplicate games. Evidence:
  `~/koi-run/mac-gate/reports/delta-gate/delta-gate-20261030-013917`.
- Run duration ~2 h 05 m (longer games than the LMP gate); the bot was paused
  between games, its autostart unloaded, and it was restarted afterwards.

## Decision
Neutral, not accepted; reverted in `7178536`. Do not retry this isolated margin
change; the current cushion is fine for the pinned NNUE. A future delta change
should be paired with a materially different qsearch design and its own SPRT.
