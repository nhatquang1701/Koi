# Root-move ordering by previous-iteration scores: neutral (2026-10-30)

## Motivation
Root move scores are already collected for selective research, but the single-PV
iterative-deepening loop only moved the previous best move to the front; every
other root move was re-ordered from generic history/SEE/TT priorities each
iteration. Classic engine design orders all root moves by the previous
iteration's scores, so the likely-best lines are searched first at every depth.

## Candidate
- In the runner's ordering block: after the existing best-move hint ordering,
  stable-sort `legal_moves` by the previous completed iteration's
  `RootMoveScore` values; unscored moves keep their relative order at the tail.
- Snapshot taken only from a non-aborted iteration, before selective root
  verification can overwrite the context scratch buffer.
- Commit `3fb2f20`, Mac arm64 candidate SHA-256
  `a8c7375bd08ca80e5b69e1a13c3c95213b679e208178ce19e0e986b82a767fd6`.
- Baseline: current HEAD before the change, `koi-head-cand`
  (`4e19ce2b448533578ac9d15b914650adcc40b18f4b441494f3aa2c1a100445cc`).
- Windows search/strength/soak/time-manager suites green before the gate.

## Gate (Mac arm64, quiet window)
- 300-opening duplicate-free corpus, 1 game/opening/colour = **600 distinct
  games**, 200 ms, Threads 1, Hash 64, promoted h128 pinned both sides.
- White: 300 games, 98 W / 80 D / 122 L, -27.9 Elo, LLR -2.57.
- Black: 300 games, 114 W / 89 D / 97 L, +19.7 Elo, LLR +0.69.
- **Combined: 212 W / 169 D / 219 L = 49.42 %, -4.1 Elo, LLR -1.887 →
  inconclusive** (point estimate negative; evidence against a +20 gain).
- No duplicate games. Evidence:
  `~/koi-run/mac-gate/reports/rootord-gate/rootord-gate-20261030-041037`.
- The bot was stopped between games for the whole window (autostart unloaded)
  and restarted by the wrapper afterwards; no game was interrupted.

## Decision
Neutral/negative, not accepted; reverted in `a1415c3`. The generic root
priority ordering (history/SEE/TT plus the previous-best hint) is already good
at this baseline; do not retry this pure re-ordering without a materially
different root scheme (e.g. per-move aspiration combined with a different
selection policy) and a fresh SPRT.

## Process note
A first window attempt was aborted because a killed watcher's EXIT trap
re-enabled the bot autostart mid-gate; the clean rerun ran with the bot stopped
and its autostart unloaded until completion. Watch for trap/restore ordering in
future gate wrappers.
