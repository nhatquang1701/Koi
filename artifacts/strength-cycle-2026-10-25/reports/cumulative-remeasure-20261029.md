# Cumulative program gain re-measured (2026-10-29)

## Why re-measured
The earlier +306.3 Elo figure used a local Windows baseline file
(`koi-engine-avx2-baseline-9fab3d0.exe`) whose SHA-256 (`5b984173...`) did **not**
match a fresh build of commit `9fab3d0` (`c58b17e0...`). The file had been
overwritten by a different build, so that figure had a provenance defect and is
superseded.

## Provenance
- Baseline rebuilt from a clean `9fab3d0` worktree with the documented MSVC
  release configuration: `koi-engine-avx2-baseline-9fab3d0-rebuilt.exe`,
  SHA-256 `c58b17e0a4c654993df6ad351b0658707dca17fb60d6e8e1e177a5c04ea80bd9`.
- Candidate: current HEAD `5d0ceedb` with the iteration-guard, HCE-removal and
  movetime-budget accepted work; Windows build SHA-256 prefix `7a401f6d93f3`
  (the committed fatal-error fix `9601c58` tree), Mac arm64 build
  `4e19ce2b448533578ac9d15b914650adcc40b18f4b441494f3aa2c1a100445cc`.
- Promoted h128 pinned as boot/eval network on **both** sides in every gate.

## Windows gate (52 openings, 4 games/opening/colour)
- 416 played: 328 W / 68 D / 4 L = 90.5 %, **+394.5 Elo, LLR 105.8 → accept**
  (white +358.8, black +430.3).
- 94 of 416 games were exact repeats (306 distinct), so this LLR is inflated;
  the point estimate is nevertheless overwhelming.
- Evidence: `reports/cumulative-remeasure/cumulative-remeasure-20261029-025630`.

## Mac gate (duplicate-free, 150 openings, 1 game/opening/colour)
- 300 distinct games: 123 W / 78 D / 99 L = 54.0 %, **+28.0 Elo, LLR 1.122 →
  inconclusive** (white 0.0 Elo, LLR -0.36; black +56.1 Elo, LLR 1.48).
- Both engines verified running the promoted network (`EvalFile` recorded in the
  batch JSON for Koi and Opponent).
- Evidence: `~/koi-run/mac-gate/reports/cumulative-150/` (Mac).

## Interpretation
The program gain is **platform-specific**. On x86-64 the dominant accepted fix
was CPUID caching in the NNUE hot path (+281.7 Elo over the pre-fix build); on
Apple Silicon that x86 CPUID/XGETBV overhead does not exist, so the program-start
to HEAD gain there is much smaller (+28 Elo, consistent with the iteration guard
plus movetime budget). Two headline numbers therefore replace the old +306.3:
- **Windows x86-64 fixed-time (200 ms): +394.5 Elo (accept, duplicate-inflated;
  duplicate-free equivalent unavailable at this magnitude).**
- **macOS arm64 fixed-time (200 ms): +28.0 Elo, LLR 1.12 (positive but below the
  accept boundary at 300 games).**

## Notes / follow-up
- The Windows gate must not be run on the Windows host again (noisy machine
  policy): future gates run on the Mac. The Windows result is retained as
  historical evidence with verified binary hashes.
- The 300-line corpus is valid (every line replays legally); a full gate on it is
  600 games, i.e. roughly 100 minutes at 200 ms. An earlier attempt was killed
  after ~60 minutes and misdiagnosed as a harness hang; there is no tooling bug.
  The reported Mac gate used the 150-opening corpus (300 games, ~50 minutes).
