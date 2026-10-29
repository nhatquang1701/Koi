# Late-move pruning candidate rejected (2026-10-29)

## Candidate
A dedicated late-move-pruning stage was added to the scout-node move loop:
after capture pruning, a quiet move is skipped when `move_number >= 4 +
depth^2/(improving ? 2 : 1)` at `depth <= 5`, excluding PV nodes, excluded
searches, claimable draws, checks, captures, promotions, tactical positions, TT
moves, killer moves and positions below the mate threshold. New
`late_move_prunes` statistic; `search/strength/soak` suites green on Windows.

- Candidate commit `bc0d521`, Mac arm64 binary SHA-256
  `31c6e4d592fd5f10e9d09f39a58e0dd663ed6d10b41ecf676188522bf8e58a67`.
- Baseline: HEAD before the candidate, Mac binary SHA-256
  `4e19ce2b448533578ac9d15b914650adcc40b18f4b441494f3aa2c1a100445cc`.

## Gate (Mac arm64, quiet window)
- 300-opening duplicate-free corpus, 1 game per opening per colour,
  **600 distinct games**, 200 ms movetime, Threads 1, Hash 64, promoted h128
  (`koi-v7-h128-official-20261025`) pinned on both sides, own book off, no Syzygy.
- White: 300 games, 93 W / 86 D / 121 L, **-32.5 Elo, LLR -2.98 → reject**.
- Black: 300 games, 103 W / 92 D / 105 L, -2.3 Elo, LLR -0.88 → inconclusive.
- **Combined: 196 W / 178 D / 226 L = 47.5 %, -17.4 Elo, LLR -3.857 → reject.**
- No duplicate games (600 distinct of 600 played).
- Evidence: `~/koi-run/mac-gate/reports/lmp-gate/lmp-gate-20261029-222444`.

## Decision
Rejected and reverted (revert commit `ce5cd7e`). Koi's existing child futility
and static quiet-futility stages already cover most of this surface; dropping
late moves by count alone discards defensive resources that the reduced search
still needs. Do not resurrect this formulation without a materially different
design (e.g. depth >= 2 only with a much larger limit, restricted to
non-improving positions, or combined with a SEE/history guard), and only behind
a fresh SPRT.

## Measurement note
The gate ran with the Lichess bot paused **between games** (user-approved
2026-10-29); the bot autostart agent was unloaded for the window and the bot was
restarted by the watcher once the gate finished. No game was interrupted.
