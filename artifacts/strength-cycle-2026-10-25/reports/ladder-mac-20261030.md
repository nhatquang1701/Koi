# Absolute strength ladder, Mac arm64 (2026-10-30)

## Purpose
Context for the accepted upgrades: a fresh absolute estimate of the current
HEAD on the quiet measurement machine, using the pinned Stockfish-19 anchor
methodology.

## Configuration
- Engine: HEAD `7d2ceda` tree; Mac arm64 binary SHA-256
  `4e19ce2b448533578ac9d15b914650adcc40b18f4b441494f3aa2c1a100445cc`
  (the engine source is identical to the pre-LMP tree; the LMP candidate was
  reverted before this measurement).
- Promoted h128 `koi-v7-h128-official-20261025` pinned via `EvalFile`
  (NNUE-enabled confirmation recorded in every batch).
- Stockfish 19 universal, `UCI_LimitStrength` anchors **1600 / 1800**,
  32-opening curated corpus (`openings-curated-32.txt`), paired colours,
  128 games per anchor, movetime **200 ms**, Threads 4, Hash 512, no book,
  no Syzygy. `elo_estimate.py` pinned configuration honoured.
- Quiet window: the Lichess bot was paused **between games** (user-approved
  procedure), the bot autostart agent was unloaded for the window and the bot
  was restarted afterwards; no game was interrupted.

## Result
- **Elo estimate 1665**, 95 % CI **1605-1726**, half-width 60.5,
  reliability **moderate**, low_confidence false.
- Anchor scores: 56.9 % vs SF19 `UCI_Elo=1600`, 33.8 % vs `UCI_Elo=1800`
  (raw: 128 games vs 1600, score 0.453 for the lower-batch round as recorded
  in the batch artifacts; the Jeffreys-smoothed pair feeds the logistic fit).
- Evidence: `~/koi-run/mac-gate/reports/ladder-20261030.json` (Mac).
- Baseline comparison: the previous ladder reported ~1624 Elo (Windows,
  2026-10-27, before the iteration-guard and movetime upgrades). The two numbers
  are **machine-specific** and not directly comparable; this Mac estimate is the
  reference for future Mac ladders.

## Harness fix required
`elo_estimate.py` pins `--hash 512`, but `uci_match.ps1` was rejecting it: when
the engine's `Speed` option was removed on 2026-10-27, its
`[ValidateRange(1, 100)]` attribute was left orphaned above the `Hash`
parameter, silently capping Hash at 100 and breaking the estimator. Fixed in
`7d2ceda` (orphan attribute removed); Hash 512 now binds correctly.

## Follow-up
- Use the same quiet-window procedure for future Mac ladders; keep the bot's
  autostart unloaded only for the measurement window.
- The 2600 milestone remains far off; next strength work should target
  structural search/NNUE gains, not small margin tuning.
