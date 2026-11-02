# HCE rewrite plan (2026-11-02)

Status: active. Round 1: v1 evaluator module + unit suite. Round 2: GPU NNUE removed completely; `KOI_EVALUATOR=hce` boot override, process search regression, and runtime `EvalMode` (`nnue`/`hce`) selection landed. Round 3: `koi-bench --hce`, per-term HCE unit tests, and HCE selection/bench documentation. Goal: completely rewrite Koi's hand-crafted evaluator (HCE) to be
very strong and cleanly integrated, with an explicit evidence trail and at least
250 iteration rounds (continuing until the user says stop). No version bump/tag/
release until the user explicitly approves.

## Why a rewrite

The previous HCE was removed on 2026-10-28 because it was described as "riddled
with odd code"; NNUE became the production evaluator. This rewrite starts from
scratch instead of restoring old code:
- explicit, documented evaluation terms and score scale;
- no coupling into NNUE feature containers and no hidden global state;
- optional, selectable evaluator (NNUE stays the production/default path);
- every term has a unit test and a measurable strength gate.

## Integration policy (1.0.0-compatible)

- New files: `src/koi/classical_evaluator.{hpp,cpp}` implementing the existing
  `Evaluator` interface; stateless and thread-safe
  (`supports_concurrent_evaluation() == true`).
- UCI option `EvalMode` (combo `var nnue var hce`, default `nnue`).
  - `hce`: stop/suppress the active search, install the HCE evaluator.
  - `nnue`: load the current `EvalFile` (or boot network); fail closed with a
    diagnostic if no network is available.
  - `EvalFile` remains an NNUE loader; a successful load selects NNUE mode.
- Boot override for development/measurement: `KOI_EVALUATOR=hce` constructs the
  HCE evaluator without requiring a network. Default boot behavior is unchanged
  (NNUE required); release packages remain NNUE-default.
- Measurement tooling: `uci_match.ps1` stays harness-side; matches select HCE
  through `KOI_EVALUATOR=hce` (and/or `-KoiOptions @{EvalMode='hce'}`).
  `koi-bench` gets an explicit HCE mode in a later round.
- Strength gates run on the Mac per standing policy; Windows is only for MSVC
  builds/git/push.

## Score model

- Centipawn scale compatible with search (`kMateThreshold` untouched).
- White-positive internal score, negated for `perspective == black`.
- Tapered midgame/endgame interpolation using `PositionFeatures::game_phase`
  (0..24), with separate mg/eg parameters for material, PST and most terms.
- Clamp to ±`kMateThreshold - 1`; search owns mate/terminal scores.
- Drawish scaling for opposite bishops and low-material endings; true rule
  draws remain search's responsibility.

## Evaluation terms (v1, then iterated)

1. Material + tapered PSTs (pawn, knight, bishop, rook, queen, king).
2. Mobility by piece type (from `PositionFeatures::piece_mobility`), with
   separate mg/eg weights.
3. Pawn structure: doubled, isolated, connected, backward, candidate, passed
   (rank-scaled), pawn-chain/space interactions.
4. King safety: pawn shelter/storm around the king, open/semi-open files near
   the king, attacker count in the king zone, phase-scaled.
5. Threats: pawn attacks on pieces, minor attacks on majors, hanging/overloaded
   pieces using attack tables, king-zone attackers.
6. Piece features: bishop pair, knight outposts, rook open/semi-open files and
   seventh rank, queen development, trapped bishops.
7. Space/center control, development, tempo.
8. Drawish/endgame scaling (opposite bishops, insufficient material, no pawns).

## Iteration schedule (>= 250 rounds)

- Rounds 1–10: module skeleton, unit invariants, UCI/boot integration, docs.
- Rounds 11–40: first complete term set with per-term unit fixtures; ASan pass.
- Rounds 41–80: feature refinement (pawn/king/threads), bench NPS, quick
  self-play sanity.
- Rounds 81–140: parameter tuning (SPSA/coordinate) gated by equal-time
  matches against fixed HCE baselines; revert failed gates.
- Rounds 141–200: Mac strength gates vs promoted NNUE (informational) and
  HCE-vs-HCE regressions; process/soak validation.
- Rounds 201–250: cleanup, performance, documentation, final evidence.
- Continue beyond 250 until the user says stop. Every iteration: signed
  commit, clean worktree, green CI, evidence under
  `artifacts/plans/hce-rewrite-2026-11-02/`.

## Verification

- Unit: term invariants, color/vertical symmetry, material monotonicity,
  score bounds, deterministic output, concurrent evaluation safety.
- Process: engine with `KOI_EVALUATOR=hce` handshake/search/quit; NNUE default
  unchanged; `EvalMode` transitions.
- Sanitizer: ASan unit + process/reliability labels.
- Bench: `koi-bench` HCE mode later; NPS and score sanity.
- Strength: Mac equal-time matches (uci_match/sprt_compare); never use
  Windows for gates.

## Risks

- Score-scale/pruning interactions: search constants were calibrated for NNUE;
  verify no assumption breaks (HCE uses the same cp scale; tuning must watch
  pruning margins via games, not code changes).
- Evaluator identity/diagnostics must stay truthful (no "NNUE active" claim
  when HCE runs); handshake fixture and docs updates are part of the work.
- Performance: HCE evaluates every leaf; keep terms O(64) and avoid allocation
  in the hot path; add incremental caching only with measured benefit.
