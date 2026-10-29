# Strength cycle handoff (2026-10-30)

## State
- Branch `koi-engine`, HEAD `82cda72` (signed), clean worktree, 26 signed local
  commits unpushed (version-bump policy: no push/tag/release yet).
- Engine: NNUE mandatory (HCE removed 2026-10-28), promoted h128
  `koi-v7-h128-official-20261025` pinned for all measurements.
- Accepted upgrades this cycle: CPUID-cache fix (+281.7), fixed-time iteration
  guard (+56.8 Windows / +48.0 1+0 confirmation), full-movetime budget
  (+31 to +42, accepted with duplicate caveat).
- Mac arm64 absolute ladder (2026-10-30): **~1665 Elo, 95 % CI 1605-1726**
  (SF19 anchors 1600/1800, 200 ms, Threads 4, Hash 512, promoted net).
- Cumulative vs program start `9fab3d0` (hash-verified rebuilt baseline):
  Windows x86-64 +394.5 Elo (Llr 105.8, duplicate-inflated); Mac arm64
  duplicate-free +28.0 Elo (LLR 1.122, positive but under boundary).

## Non-accepted / closed levers (all with reports)
- Late-move pruning stage: -17.4 Elo, LLR -3.857 reject (`lmp-negative.md`).
- Quiescence delta margin 100->200: +4.1 Elo, LLR -0.819 neutral
  (`delta-margin-neutral.md`).
- Root ordering by previous-iteration scores: -4.1 Elo, LLR -1.887 neutral
  (`root-order-neutral.md`).
- Earlier closed: quiet SEE, NMP base 3, RFP margins, aspiration 25, LMR
  surface, capturing promotions, search-margin batch, NNUE width (h160/h256),
  teacher depth (d10), data scaling (41.25M), extra epochs, symmetry augment,
  NEON SIMD, AVX2 king-bucket refresh, continuation-history symmetry.
- `docs/ENGINE_SPEC.md` confirms the remaining SF-style search surface is
  already implemented (multicut, negative singular extensions, ProbCut with
  qsearch verification, correction history, IIR, TT aging/generations).

## Remaining possible directions
1. **Clock/time-management (1+0)**: the only large untested strength lever for
   real games. Step 7 explicitly keeps it out of scope for the 200 ms cycle
   unless the user re-enables it; it must be gated with `sprt_compare
   -TimeControl 1+0`, not 200 ms movetime.
2. **Net-side new data/training**: requires Mac-exclusive hours (the Lichess bot
   currently occupies the quiet machine); prior data/teacher/width cycles were
   neutral or negative, so expected value is low without a new data source.
3. **Fixed-time micro tuning**: three consecutive neutral/rejected candidates
   show the current baseline is at a local plateau; further isolated margins
   have low expected value.
4. **Release prep / version bump**: 26 commits (including the HCE removal and
   the movetime/guard upgrades) are local only; CI is unverified until pushed.
   Nothing has been pushed/tagged/released without explicit approval.

## Gate procedure (Mac, quiet windows)
- The Lichess bot may be playing; stop it only **between games** (user-approved
  procedure), **bootout the autostart agent before stopping** (otherwise it
  relaunches the bot mid-gate), run the gate from a single-shot wrapper with an
  EXIT trap that restores the bot, then verify `pgrep -f "lichess-bot\.py"` is
  empty before starting engines.
- Standard fixed-time gate: 300-opening `openings-release-strength-300.txt`,
  1 game/opening/colour (600 distinct games), 200 ms, Threads 1, Hash 64,
  `opts.json` pinning the promoted net; wall time ~100-120 min.
- Docs: `python tools/docs/prepare_pages.py && mkdocs build --strict` is green
  at this revision.

## Evidence
- `artifacts/strength-cycle-2026-10-25/reports/` (all gate reports),
  `artifacts/plans/strength-cycle-2026-10-25/` (plan/PIN/PROGRESS/HANDOFF),
  Mac gate artifacts under `~/koi-run/mac-gate/reports/`.
