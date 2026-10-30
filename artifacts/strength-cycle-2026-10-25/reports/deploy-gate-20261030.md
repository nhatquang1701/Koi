# Deployment delta: old bot binary vs current HEAD (2026-10-30)

## Purpose
The Lichess bot was running a 2026-10-25 binary (45 time forfeits in 210 games).
Round 36 redeployed current HEAD. This gate quantifies the playing-strength
difference between the two binaries on the same quiet Mac arm64 host, so the
rollout is evidence-backed rather than assumed.

## Binaries
- Baseline (previously deployed): `koi-engine.pre-deploy-20261030`, SHA-256
  `2cece4fecf6611a0b50c2a4b5d0e47106e8214e6bf12d4d985c2fc8fab17ad9f`
  (handshake `id name Koi`, no version/build line - pre 1.1.4 identity).
- Candidate (current HEAD `9d7ee6f`): SHA-256
  `02b3dd39d49fc739a7377f0740a98e9bd2fd4f6485bfae9a51a4edcf37cf9faa`
  (`id name Koi 1.1.4`, `info string build 9d7ee6f6b596`).
- Both staged in sibling-free directories with the promoted h128 net beside the
  binary and the pinned `opts.json` options; Mac arm64 release build.

## Gate
- `sprt_compare`, 200 ms fixed movetime, 150-opening
  `openings-release-strength-160.txt` corpus, 1 game/opening/colour =
  **300 distinct games**, Threads 1, Hash 64, promoted net pinned both sides.
- White: 45 W / 57 D / 48 L, -6.9 Elo, LLR -0.674.
- Black: 70 W / 34 D / 46 L, **+56.1 Elo**, LLR +1.504.
- **Combined: 115 W / 91 D / 94 L = 53.5 %, +24.6 Elo, LLR 0.83 ->
  inconclusive (positive trend, no regression).**
- Evidence: `~/koi-run/mac-gate/reports/deploy-gate/deploy-gate-20261030-183959`.

## Interpretation
- On Mac arm64 the fixed-time delta reflects the fixed-time iteration guard and
  the full-movetime budget (~190 ms instead of ~162 ms per move); the CPUID
  NPS fix does not apply to Apple Silicon, so +25 Elo is the expected scale.
- On x86-64 the same redeploy also carries the CPUID-cache fix, measured at
  +281.7 Elo in its own gate. Users on x86-64 therefore received a far larger
  upgrade than this arm64 number suggests.
- In real games the redeploy additionally removes the old binary's clock-unsafe
  behaviour: the old corpus had 45 Koi time forfeits in 210 games (21 %), while
  the current engine had **0 forfeits in 24 1+0 self-play games** with 24/24
  clean shutdowns (`reports/clock-flagtest-1plus0.md`).
- Caveat: 300 games is not decisive at H1=+20 (LLR 0.83); the result is
  positive and excludes a large regression, which is what this rollout check
  needed. No threshold shopping follows.

## Decision
Deployment stands. Bot restored after the gate; no game was interrupted.
