# 1+0 clock robustness probe on current HEAD (2026-10-30)

## Question
The old deployed bot binary produced 45 Koi time forfeits in 210 games (21 %).
Is the current engine's time management itself unsafe, or was it only the old
binary?

## Method
Self-play A/B `sprt_compare -TimeControl 1+0`, current Mac arm64 engine
(`koi-head-cand`) on both sides, 12 unique openings x 2 colours = 24 games,
`-Games 1`, Threads 1, Hash 64, promoted h128 pinned, bot stopped for the
window (and restarted afterwards). Evidence:
`~/koi-run/mac-gate/reports/flagtest-small/flagtest-small-20261030-181329`
(match JSON `koi-uci-match-v2`, fields `termination` and `process_status`).

## Result
- 24 games, 8 W / 8 D / 8 L (score 0.50, SPRT inconclusive as expected for
  identical binaries).
- **Terminations: 16 checkmate, 8 rule draw, 0 time forfeit.**
- process_status: 24/24 `clean shutdown` on both sides.
- Game length: min 66, avg 139, max 242 plies (long games, no flagging).

## Conclusion
The current engine's clock allocation is robust at 1+0 with no flags in this
sample; the observed Koi forfeits were the old pre-guard/pre-overhead-fix
binary, not the current time manager. The bot redeploy (reports/
bot-engine-redeploy-20261030.md) therefore addresses both the speed and the
flagging problem. Time-allocation redesign is not justified by this evidence;
future aggression experiments should be gated with `-TimeControl 1+0` and
count terminations, not just Elo.

## Notes
- A full 300-game 1+0 SPRT would take many hours (each game ~1-2 min); use
  12-24 game probes for flag robustness and reserve full SPRT for allocation
  changes with a clear hypothesis.
- Bot deployment verification: no new Lichess games were received between
  17:14 and 18:35 (bot awaiting challenges), so live forfeit-rate evidence is
  still pending.
