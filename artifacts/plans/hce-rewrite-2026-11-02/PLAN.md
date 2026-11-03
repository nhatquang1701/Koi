# HCE rewrite plan (2026-11-02)

Status: active. Round 1: v1 evaluator module + unit suite. Round 2: GPU NNUE removed completely; `KOI_EVALUATOR=hce` boot override, process search regression, and runtime `EvalMode` (`nnue`/`hce`) selection landed. Round 3: `koi-bench --hce`, per-term HCE unit tests, and HCE selection/bench documentation. Round 4: passed-pawn king proximity/blocking/rook support, weighted king attackers, pawn storms, pawn threats, outposts and mobility-area safety; bench evidence in BENCH.md; first Mac gates in GATES.md. Round 5 reverted the combined v2 term set after a negative 50 ms gate. Round 6: focused passed-pawn king-proximity/blocking candidate (v3) screened +72.5, confirmed +30.6 and large-gate +25.6 Elo, all inconclusive-positive; retained as current baseline. Round 7: backward-pawn term screened +11.3/+5.5 Elo and was neutral, so it was reverted; Round 8: HCE now derives its own board/attack state from the native position instead of the shared PositionFeatures container; unit suite green. Behaviour screen vs v3 was neutral (+16.4 Elo, LLR 0.106, 64 games); kept as an architecture refactor, not a strength claim. Round 9: endgame corpus invariants added (7/7). Round 10: removed duplicate pawn-attack computation in the native board builder (NPS 750k->762k on the Windows depth-5 bench). Round 11: rook-behind-passer candidate screened neutral and reverted. Round 12: explicit tapered king PST tables added; neutral screen (+11 Elo, LLR 0.02) retained as tuning infrastructure. Round 13: internal term-scale tuning seam; passed 125% and mobility 125% screens were neutral/negative, defaults kept. Round 14: king-safety 125% neutral-positive and pawn-structure 75% negative; defaults kept. Round 15: king-safety 175% screened +33.2 but the 126-game confirm reversed to -24.9; defaults kept, confirming that 64-game screens are only triage. Round 16: explicit tables for all piece PSTs; screen +33.0 but confirm neutral (-2.8 Elo), retained as tuning infrastructure only. Round 17: evaluation-scale knob added; scale 125% negative, scale 80% screened/confirmed positive but the 300-game gate reversed to -11.6 Elo; default 100 kept. Round 18: ASan validation of the HCE (unit 24/24 and koi_hce_process) clean, and the Windows sanitizer CI fuzz flake was cleared by rerun. Round 19: defended pawn-threat term screened +16.5 but the 126-game confirm was -11.2; reverted. Round 20: endgame mop-up screened -16.5 and was reverted without confirmation. Round 21: Mac profile shows HCE leaf evaluation is not the time bottleneck (search/quiescence dominates; ~67 qnodes/node), so effort shifts to evaluation quality/search synergy rather than evaluator micro-optimization. Round 22: threat/piece-term tuning scales added; threat 150% screened +21.7 but the 126-game confirm was -5.5; default kept. Round 23: piece-term 150% screen negative (-10.9); default kept. Round 24: `koi-eval-features --hce` added for HCE static-evaluation corpus dumps; tool verified on sample FENs. Round 25: HCE vs promoted-NNUE corpus comparison: Pearson 0.966, HCE/NNUE slope 1.208, intercept -45.5 cp, MAE 140 cp, sign agreement 93.4% on 91 tactical/endgame FENs (EVAL_CORPUS.md). Round 26: weighted king-attack model screened -32.8 and was reverted; the raw king-zone count remains the better default. Round 27: a modest 110% evaluation scale also screened negative (-33.5); all 80/110/125/150 scale variants are non-positive, closing that line. Round 28: NNUE-teacher PST correction fitted offline (corpus MAE 163.3->148.2) but the game screen was -44.8 Elo; reverted. Static-corpus agreement is not a playing-strength proxy. Round 29: tempo 200% screened +41.1 but confirmed -2.8; default kept, closing the single-group tuning matrix. Round 30: dedicated `koi_hce_bench_process` regression test added (depth-3 HCE suite, >=60/64 match threshold, evaluator=hce profile); passes in ~4 s locally. Round 31: found and fixed a silent regression where later table work had restored the shared PositionFeatures input path; the native board/attack builder is restored and all HCE tests pass. Round 32: native-restore behaviour screen was neutral-positive (+21.8 Elo, LLR 0.211); retained as architecture, no strength claim. Round 33: safe mobility (excluding enemy-pawn-controlled squares) screened -38.2; reverted. Goal: completely rewrite Koi's hand-crafted evaluator (HCE) to be
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

Round 36: endgame-component scale knob (`KOI_HCE_EG_SCALE`) added; 120% screened neutral (+5.4 Elo, LLR -0.084); default kept.

Round 37: fixed `EvalMode` state sync on successful `EvalFile` load (hce_active_ was left stale) and added a controller regression test; EvalFile now reports `info string evaluator nnue` as well as the enabled path.

Round 39: pawn-islands term screened -27.3 Elo and was reverted.

Round 40: enemy pawn-storm term screened +16.3 but confirmed -19.3; reverted.

Round 42: minor-attacks-major threat term screened +32.7 but confirmed -22.2; reverted.

Round 44: passed-rank scale 120% screened +27.2 and confirmed +13.8 but was neutral (+3.5) in a 300-game gate; default kept.

Round 45: added `koi_hce_eval_features_process` covering the HCE static-eval CSV dump and `--hce/--nnue` conflict rejection.

Round 46: check-aware tempo screened exactly neutral and was reverted.

Round 49: per-piece mobility knobs added; knight mobility 150% screened +43.8 but confirmed +2.8 (neutral); defaults kept.

Round 50: rook mobility 150% screened neutral (+5.7 Elo); default kept.

Round 51: bishop mobility 150% screened neutral (-5.5 Elo); per-piece mobility matrix closed, defaults kept.

Round 55: legacy-tuned PST values screened +43.8 and confirmed +27.7 but reversed in a 300-game gate (-11.6); reverted.

Round 56: legacy king PST candidate screened neutral (+5.4); reverted.
