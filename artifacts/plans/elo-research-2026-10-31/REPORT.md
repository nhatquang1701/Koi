# Koi strength & feature research report (2026-10-31)

Source: read-only engine inspection + Playwright web research by teammate
`elo-researcher` (no repo mutations). Every external claim carries a URL;
"unverified" marks extrapolations from other engines, not Koi measurements.
This report is planning input, not evidence of strength. Gates still follow
AGENTS.md §8 (paired SPRT at 200 ms, 1+0 confirmation, pinned stack).

## 0. Basis verified in the tree

- Search is essentially a faithful Stockfish-19 port: PVS + aspiration
  `50+min(32,2d)`, NMP `R=7+d/3+excess/256` (verify at d>=16), RFP `80+60d`,
  razor d1 `120*d^2`, child/quiet/capture futility, ProbCut `241-64*imp`,
  capture SEE prune `177d`, IIR/IID at d>=6, singular +1..+3, LMR step bands,
  6-ply conthist, capture/pawn/piece-to history, 3-key corrhist
  (pawn/material/king), TT 24 B + 64 striped locks, 132-slot NNUE accumulator,
  64K eval cache, bespoke root-safety battery.
- Files: `src/koi/detail/search_context.cpp:646`, `search_policy.hpp:72-217`,
  `search_constants.hpp`, `search_runner.cpp:42`, `time_manager.cpp:124/377`,
  `transposition_table.hpp:76`, `nnue.cpp:437-2268`, `uci_controller.cpp:218`.
- Missing vs Stockfish master (Oct 2026): optimism eval bias, dynamic
  aspiration delta + fail-high depth recovery, ply/time-advantage TM +
  score-stability scaling, SMP search diversification, minor/major/continuation
  corrhist, int16 accumulators, finny tables, staged movegen, 16 B lockless TT,
  `UCI_LimitStrength/UCI_Elo`, fitted WDL model, `currmovenumber`, multi-layer
  dual-activation NNUE head, richer features (K32H3/FullThreats/PP_3Wide),
  WDL/result loss + low-LR finetune + factorization in training.
- h128/8 buckets, SmoothL1 on cp labels, `TARGET_SCALE=100`; Mac M4 ~1.2-1.7M
  NPS; `make_observed` ~42% wall; picker `next()` 26.5%; TT probe 5.2%.
- Prior repo evidence: NPS hedges repeatedly neutral (AVX2 refresh +10.6% NPS
  neutral; NEON neutral); margin retunes negative. Rank NNUE quality first,
  cheap search/TM second, big refactors last.

## A. Expected-Elo items (ordered by value-for-effort)

| # | Idea / mechanism | Evidence | Est. Elo | Effort/risk | Koi hook |
|---|---|---|---|---|---|
| A1 | Distill h128 student from a stronger teacher (accepted h256/threat/K32H3 net searched deeper); inference cost unchanged | Viridithas v20 "+60.84 Elo, distill network from large teacher (#458)" https://github.com/cosmobobak/viridithas/releases/tag/v20.0.0 ; https://github.com/official-stockfish/nnue-pytorch/blob/master/docs/nnue.md | +25..80 (unverified) | M/med | `tools/measurement/train_nnue_koi.py`, `tools/nnue/bullet_train` |
| A2 | WDL/result-blended loss + 1/10-LR finetune instead of pure SmoothL1 on cp; lambda*WDL + (1-lambda)*result, final low-LR epoch | nnue-pytorch "Converting evaluation from CP to WDL space" (above URL); SF commit 2d54ef335a "longer finetune with one-tenth LR" STC+LTC https://github.com/official-stockfish/Stockfish/commit/2d54ef335a ; Alexandria v9.1 "better WDL scheduling" +18.3 LTC | +15..45 (unverified) | M/med | `train_nnue_koi.py` loss/epochs; corpus headers |
| A3 | Eval-scale calibration probe: multiply NNUE output by 0.9-1.2 (or fix output shift) and gate | Viridithas v19.0.1 "+46.12 Elo evaluation scaling" https://github.com/cosmobobak/viridithas/releases/tag/v19.0.1 ; Koi margins hand-recalibrated to NNUE range `search_constants.hpp:33-45,86-90` | 0..30 (cheap) | S/low | `nnue.cpp:2268`, trainer output-shift grid |
| A4 | Dynamic aspiration + fail-high recovery (delta from prev score mean-square; reduce depth on repeated fail-highs) | SF search.cpp:388-455 https://github.com/official-stockfish/Stockfish/blob/master/src/search.cpp#L388-L455 ; Alexandria v9.1 | +5..15 | S-M/med | `search_runner.cpp:42`, ID loop ~3000-3100 |
| A5 | TM modernization for real games: opt/max scale from log time, ply, time advantage, hard cap ~0.81*remaining-overhead; fallingEval*instability*effort*reduction | SF timeman.cpp https://github.com/official-stockfish/Stockfish/blob/master/src/timeman.cpp ; stability block search.cpp:583-635; Alexandria v9.1 | +10..30 clock games, 0 at movetime | M/low | `time_manager.cpp:124/377/429-470` |
| A6 | Corrhist expansion + gravity: minor, major/non-pawn, continuation keys, exponential decay, update with up-to-date static eval | SF history.h:228-247 https://github.com/official-stockfish/Stockfish/blob/master/src/history.h#L228-L247 ; Alexandria v9.0; Viridithas #406 | +5..15 | M/low | `search_ordering_tables.hpp:132-167`, `.cpp:409-421`, `search_context.cpp:2242-2253` |
| A7 | Lazy-SMP diversification: odd-thread NMP jitter, helper depth offsets, better thread selection | SF 61c6871785 SMP STC+LTC https://github.com/official-stockfish/Stockfish/commit/61c6871785 ; Viridithas #415 | +5..20 at Threads=4 | S-M/low | `search_runner.cpp:2037-2060` |
| A8 | Drop fullmove from TT + eval-cache keys (HCE relic; NNUE does not read fullmove) | Verified `search_table_access.hpp:19-46`, `search_context.hpp:467-476` still cite removed classical evaluator | 0..5 (free hit-rate) | S/med | two key-mix sites; update invariant #9/tests |
| A9 | Optimism eval bias `optimism[us]=456*avg/(|avg|+85)` | SF search.cpp:398+1937; commit 7347cc2f4e non-reg STC/LTC https://github.com/official-stockfish/Stockfish/commit/7347cc2f4e | +2..8 | S/low | `search_context.cpp:975-997` |
| A10 | Pawn history in pruning + 6-ply conthist refinements | SF search.cpp:1231-1236; Alexandria v9.1 | +2..8 | S/low | `search_policy.hpp:275-285`, call site ~1517-1580 |
| A11 | LMR window-aware refinement (less reduction in loose alpha windows; reduction per alpha-raise) | SF 5f7348f03f; Viridithas #431 | +3..10 | S-M/med | `search_policy.hpp:112-217`; but repo LMR-surface retune was negative |
| A12 | Mate-finding: dynamic futility depth cutoff + seekMate trigger | SF fa8b6add9d | +2..6 | S/low | `search_context.cpp:646+`, `search_constants.hpp` |
| A13 | NNUE head architecture: output buckets 8->16/32, L2 16/32 + dual activation + quantmoid4; L1=128 cost barely changes | SF nnue_architecture.h https://github.com/official-stockfish/Stockfish/blob/master/src/nnue/nnue_architecture.h ; Alexandria v9.0 +28.8 LTC https://github.com/PGG106/Alexandria/releases/tag/v9.0.0 | +5..30 | M-L/med | `nnue.cpp:698-918`, trainer |
| A14 | Reduce root-heuristic surface: Koi-only battery (selective confirmations, emergency king escape, safe-capture margins) A/B remove/simplify | code `search_runner.cpp:43-89,207-2100`; `review-verification.md:46` | -10..+15 (unknown) | M/med | same file; keep time-safety parts |
| A15 | SPSA batch retune of ~15-25 constants at 10+0.1 then one SPRT per batch | Alexandria v9.1; SF practice; Koi has unused `tools/measurement/spsa_search.py`, `spsa_runner.sh` | +10..25 | M-L/high | `search_constants.hpp`, `search_policy.hpp` |
| A16 | Feature-set expansion: PP_3Wide, K32H3, full threats | SF features; Stormphrax v8 +62.44 https://github.com/Ciekce/Stormphrax/releases/tag/v8.0.0 ; but Koi 36864-input threat net scored 33.75%, h160 -49.5 | 0..40 (high variance) | L/high | `evaluation_features.*`, `nnue.cpp`, trainer |

## B. Features / UX (mostly no strength)

| # | Item | Note / evidence | Effort | Strength |
|---|---|---|---|---|
| B1 | `UCI_LimitStrength` + `UCI_Elo` + Skill Level | SF search.cpp:326 https://github.com/official-stockfish/Stockfish/blob/master/src/search.cpp#L326 | S-M | none full strength |
| B2 | Fitted WDL model replacing linear heuristic (`draw=300-|cp|/4`) | SF uci.cpp:543-606; https://github.com/official-stockfish/WDL_model | S | none (analysis) |
| B3 | Emit `currmovenumber` (populated but not written) | standard UCI; implemented in this round | XS | none |
| B4 | MultiPV 16->256/500 | SF supports 500 | S | none |
| B5 | Built-in `bench` + `info string compiler` | SF convention | S | none |
| B6 | `eval` command / NNUE breakdown | debugging/analysis | S | none |
| B7 | Syzygy interior probing default for <=5-man + qsearch probes | needs TB-only suite, not the 200 ms no-TB SPRT | S | endgame only |
| B8 | Ponder time `optimumTime/4` equivalent | SF timeman tail | XS | small clock games |
| B9 | `StrengthMode` advertised but neutral; implement via B1 or stop advertising | AGENTS trap row | XS | none |
| B10 | Analysis: WDL for estimate lines via B2 model | UX | XS | none |

## C. Performance engineering (require LTC/Threads=4 evidence; NPS->Elo conversion has been weak)

| # | Item | Evidence | Speed | Effort/risk | Hook |
|---|---|---|---|---|---|
| C1 | int16 NNUE accumulators | nnue-pytorch guide; SF nnue_accumulator.h | +5..10% | M-L/med | `nnue.cpp:437-560` |
| C2 | Finny tables for king-bucket refresh | SF nnue_accumulator.h:52-67; Alexandria v9.1 | +3..8% | L/med | `nnue.cpp:1757-1985` |
| C3 | Staged move generation (TT, captures, killers, quiets, bad) | SF movepick stages; 17a0e6648c | +5..15% | L/high | ordering is 26.5% of wall |
| C4 | 16 B lockless TT + compressed keys | SF ff3a6c508e | +2..8%, better SMP | L/high | TT/hazard contracts |
| C5 | PGO+LTO for the Mac gate binary (release stays generic) | Berserk README | +3..10% | S/low | CMake, mac-worker docs |
| C6 | x86 AVX-512/VNNI kernels | SF commits 453f220763 etc. | +2..8% x86 | M/low | `nnue.cpp` kernels |
| C7 | arm64 NEON int16 + SDOT | prior NEON negatives | 0..6% | M/med | `nnue.cpp` |
| C8 | Remove `Position` pimpl indirection | repo review "dedicated campaign" | +2..6% | M-L/med | `position.*` |
| C9 | Prefetch continuation/corrhist entries | SF 4150d22b86 | +1..3% | S/low | `search_context.cpp` |
| C10 | Movegen/metadata cost + eval cache sizing | profile 9% wall | +2..5% | S-M/med | `search_context.cpp:773-779` |

## D. Measurement/experiment process (biggest practical leverage on quiet Mac)

| # | Change | Note |
|---|---|---|
| D1 | UHO unbalanced openings for final gates (official SF books repo https://github.com/official-stockfish/books) | fewer games for same variance |
| D2 | Pair-level/pentanomial SPRT instead of summing white+black LLRs (`sprt_compare.ps1:41-51` currently trinomial per colour) | ~20-30% fewer games; Fishtest reference |
| D3 | Migrate SPRT to fastchess https://github.com/Disservin/fastchess | high concurrency, robustness, compliance checker |
| D4 | Gate SMP changes at Threads=4 (current gates Threads=1) | A7 invisible otherwise |
| D5 | 200 ms for screening only; LTC (1+0 or 60+0.6) for decisions | 200 ms noise flipped results before (d10big) |
| D6 | Split node-limited vs time-limited gates | eval quality vs speed |
| D7 | Mandatory NPS gate before strength gate for NNUE/perf changes | `speed_gate.ps1` exists |
| D8 | Time-odds net gates (candidate at 0.9x time) | separate quality from speed |
| D9 | A/A calibration + quiet-Mac protocol (caffeinate -s, bot paused, orphan sweep, therm log) | false-positive floor |
| D10 | SPSA/OpenBench-style batch tuning | post-search plateau |
| D11 | 300-opening duplicate-free corpus for final confirmations | exists from LMP gate |
| D12 | Frozen baseline + ladder JSON per accepted change; report cumulative since 1.1.5 | program habit |

## E. Dead ends / do not retry (repo evidence + external lessons)

- Wider nets at current eval cost: h160 -49.5, h256 -40.6, h96 self-play-only;
  self-play is not a promotion gate.
- Threat-pair 36864-input net 33.75% vs official; v5 small nets quantization
  unusable; `--symmetry-augment` -12.2; extra epochs e20/e40 neutral; 37% data
  scaling neutral; deeper teachers d10/d13 -35..-55.
- NMP base 7->3 -12.2; RFP base 60/100; aspiration 50->25 neutral; quiet-move
  SEE pruning -32.7; capturing-promotion classification -32.8; LMR full-surface
  retune -2.7; root ordering by previous-iteration scores -4.1; LMP as
  implemented -17.4; qsearch delta 200 neutral; razor depth 2; child futility
  80; TT-ProbCut 340; continuation-symmetry malus -35.5.
- Lazy SEE ranking in qsearch picker breaks root-forcing/tactical contract;
  only retry after redesigning qsearch pruning to read its own SEE.
- NEON delta kernels neutral, AVX2 refresh +10.6% NPS neutral: do not spend
  again without LTC/Threads=4 evidence.
- Never call CPU-feature probes per node (CPUID bug ~4x NPS / 281 Elo).
- Do not port SF constants/net sizes blindly: Koi already carries SF19 constants
  with a different NNUE scale; h128 is the fixed-time optimum; SF L1=1024/threat
  nets are not affordable here.
- Do not trust validation MAE or 64-game screens as strength evidence; do not
  gate on Windows; do not change more than one variable per gate.

## Top 10 recommended order (cumulative estimate)

1. NNUE training v2: A2 WDL/result loss + low-LR finetune, then A1 distillation
   (keep h128) -- +25..70
2. A3 eval-scale calibration probe -- +0..20
3. A8 drop fullmove keys + A10 pawn history + A9 optimism (separate SPRTs) -- +5..15
4. A4 dynamic aspiration + fail-high recovery -- +5..15
5. A6 corrhist expansion + gravity -- +5..15
6. A7 SMP diversification, gated at Threads=4 -- +5..20
7. A5 TM modernization, gated at 1+0 + ladder -- +10..30 real games
8. A13 NNUE head (layer stacks/multi-layer/quantmoid) -- +5..30
9. Perf bundle C5 PGO + C1 int16 + C2 finny (LTC-gated; C3/C4 later) -- +3..10
10. A15 SPSA batch retune once plateau; A16 feature expansion only if net cost
    stays <=10% -- +10..25

Honest cumulative outlook: +50-150 Elo over a few months if the NNUE training
campaign lands; +15-40 from search/TM/perf if it fails. Largest uncertainty is
NNUE training quality; the untested axes are loss/result blending,
distillation, finetune schedule, output scaling, and small-head architecture.
Fix the measurement stack (D1-D5) in parallel so +5-10 Elo deltas are decidable.
