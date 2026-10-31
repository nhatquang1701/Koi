# Koi idea catalog — 2026-10-31

Purpose: durable local backlog of strength, feature, reliability and measurement
ideas. Inputs:

1. The Gemini review pasted by the user on 2026-10-31 (claims about NPS,
   accumulator design, net architecture, training data, SPSA, expected Elo).
2. `artifacts/plans/elo-research-2026-10-31/REPORT.md` (48 ideas, URL-cited,
   read-only teammate research).
3. The reliability program evidence (`artifacts/plans/reliability-2026-10-31/`)
   and this session's fresh measurements.
4. New brainstorming in this document (everything marked `new`).

Status legend: `new` = not started; `planned` = in the active shortlist;
`covered` = already implemented/tested (kept for context); `rejected` = tried
and reverted (do not retry without a materially different design); `pending` =
waiting on the profiler/measurement result.

Every strength item still has to pass the AGENTS.md §8 gate: paired self-play
SPRT at 200 ms (H0=0, H1=+20 Elo, target 200 pairs, cap 500) with the promoted
net pinned on both sides, then a 1+0 confirmation. One change per gate; failed
gates are reverted and reported, never threshold-shopped.

---

## Part 1 — Gemini review triage

| # | Gemini claim | Reality in this tree | Verdict / action |
|---|---|---|---|
| G01 | "At 1729 Elo with an SF-class search suite it should be 2800-3200 Elo; it is depth-starved." | 1729 is Koi's **SF `UCI_Elo`-anchor ladder at 200 ms on Mac**, not a CCRL/FIDE rating. The same source measured 1665 earlier; the scale is machine-specific and anchored at SF limited-strength levels, so "2800-3200" is not a like-for-like target. The depth-starvation concern is testable and partly real (fresh Windows startpos: depth 7 at 5 s, depth 4 at 1 s). | Reject the Elo-scale claim; **adopt** the diagnostic program: M09 fixed-depth vs fixed-time, M10 depth/NPS telemetry, P01 profiler (running). Do not promise +300-400 Elo. |
| G02 | "~80k NPS; accumulator leaks force it." | Post-CPUID fix measured ~524k NPS on Windows (2026-10-26 report) and ~1.2-1.7M on M4 per prior notes. Fresh noisy-Windows startpos 5 s: 244k (T1) / 317k (T4). Mac profiling pending; 80k is not current reality. | Reject premise; **adopt** fresh NPS/depth measurement (P01) and NPS gate M07. |
| G03 | "Accumulator slots are keyed by position key with a fallback refresh; move the accumulator to SearchStack." | The engine already keeps **per-slot incremental accumulators** (132 slots, delta updates, parent-key validation, full refresh only on skipped/mismatched notifications; refresh on king-bucket change was measured ~7.4% of makes). That is the standard design the reviewer is asking for, not a leak; `incremental_fallback_count_` is a safety counter, not the hot path. | Reject the "leak" claim; **adopt** "quantify refresh frequency/cost and reduce it": finny tables (E08/N37), accumulator instrumentation (M10), switch-stack locality (E09). |
| G04 | "Delete CUDA GPU NNUE." | GPU path is optional, `GpuNnue=off` by default, already scheduled for removal in docs. | Agree as hygiene; **not** a strength lever. E12. |
| G05 | "Revert threat pairs to pure HalfKA." | Already pure HalfKA-9216 input (`halfka-king-bucket-v1`); no threat-pair net ships. The only 36,864-input threat experiment lost its match. | No action; already true. Keep N19 gated. |
| G06 | "9216 inputs -> 512 hidden is the gold standard." | Equal-time sweeps on this engine chose **h128**; h160 was -49.5 Elo, h256 -40.6, and wider nets do not repay inference cost at 200 ms. Wider at equal nodes is not wider at equal time. | Reject 512 hidden; **adopt** small head-architecture expansion (N13-N15) only with equal-time gates. |
| G07 | "Retrain on 10-30M depth 10-12 labels from SF self-play." | The promoted net already used a **29.9M-row depth-8** corpus; a full depth-10 relabel was decisively negative (-35.4 Elo, SPRT reject), deeper teachers likewise. | Reject deeper labels; **adopt** untested label-quality axes: quiescence-filtered labels (N06), dedupe/balance (N07-N10), distillation (N03/N04), WDL loss (N01/N02). |
| G08 | "Quiescence filtering of training positions is mandatory." | The generator labels positions by a fixed-depth search of the current engine; it does not filter to quiet positions before writing rows. Untested for Koi. | **Adopt as a cheap experiment** (N06): filter/avoid rows with pending capture or in-check, or sample them separately. |
| G09 | "Verify NNUE de-scaling vs search constants; wild ±1500 cp breaks RFP/razoring/ProbCut." | `TARGET_SCALE=100`; search margins were hand-recalibrated to the NNUE range in `search_constants.hpp`. Plausible residual scale error. | **Adopt** the cheap eval-scale calibration probe (A3/G09): output multiplier grid 0.9-1.2 or output-shift, several quick SPRTs. |
| G10 | "SPSA-tune LMR, aspiration and pruning constants for +80-150 Elo." | SPSA tooling exists and is unused (`tools/measurement/spsa_search.py`). Constant retunes in isolation have mostly been rejected, but a batch retune on a stable baseline is untested. | **Adopt** as a later batch program (M/planned): tune 15-25 constants at 10+0.1, then one gate per batch. Do not expect +150. |
| G11 | "Each step takes 1-2 days and gives +300-600 Elo." | The repo's NNUE campaign already spent weeks and produced +134 Elo (net promotion) and a series of decisive negatives; search/TM changes typically measured +0-30 or negative. | Reject effort/Elo framing; **adopt** the ordering idea: fix measurement + diagnostics first, then NNUE quality, then small search/TM, then perf. |
| G12 | "Run fixed-depth vs fixed-time matches; if it wins at depth but loses on clock, it is NPS." | This is a good diagnostic that the repo has not run systematically at the pinned stack (node-limited matches exist for nets only). | **Adopt** M09/M10 and add a diagnostic report before the next strength campaign. |

**Bottom line:** the review's *diagnostic instincts* (depth vs quality, NPS,
eval scale, SPSA) are reasonable; its *specific technical premises* (80k NPS,
broken accumulator refresh, 512 hidden, depth-10 labels, +300-600 per step) are
contradicted by the repo's measurements and code. The single biggest honest
lever remains NNUE training quality (loss/result blending, distillation,
finetune schedule, output scaling, small head), followed by measurement noise
reduction; not a rewrite of the accumulator stack.

---

## Part 2 — Anchors measured/known before starting

| Fact | Value / source |
|---|---|
| Strength | ~1729 Elo, 95% CI 1659-1795, Mac arm64, 200 ms, Threads 4, Hash 512, SF19 `UCI_Elo` anchors; honest ~1700 ± 35 |
| Net | v4, HalfKA-king-bucket 9216 inputs, 128 hidden, 8 output buckets, ~2.36 MB, 29.9M-row depth-8 corpus |
| Search | SF19-class port: PVS, aspiration 50+min(32,2d), NMP R=7+d/3+excess/256, RFP 80+60d, razor, futility, ProbCut, SEE, LMR bands, 6-ply conthist, 3-key corrhist, TT 24 B + 64 striped locks |
| NPS | ~1.2-1.7M on M4 (prior notes); ~524k Windows post-CPUID (2026-10-26); fresh noisy-Windows startpos 244k T1 / 317k T4 at 5 s |
| Depth | Fresh noisy-Windows startpos: depth 7 at `movetime 5000` (T1), depth 7/T4, depth 4 at `wtime 1000` (bestmove in 0.23 s) |
| Accumulator | 132 per-slot incremental accumulators, delta updates, parent-key validated; full refresh only on skipped/mismatched slots or king-bucket changes (~7.4% of makes; refresh ~14.1k cycles scalar path) |
| Known perf negatives | AVX2 sparse refresh +10.6% NPS neutral; NEON delta neutral; lazy-SEE ordering broke tests; CPUID per-node bug cost ~4x NPS |
| CI | Green at 6089144 / 9b633ac; latest head 81e95a4 pushed |
| Open profiler | `profiler-analyst` teammate running a fresh CPU profile (Mac, `sample`/xctrace) with accumulator/NPS/depth asks |

---

## Part 3 — Idea catalog

Effort: XS (<1 h), S (hours), M (days), L (weeks). Risk: low/med/high.
Impact: Elo estimate, NPS estimate, or quality/feature effect.

### A. NNUE quality and training (N)

| ID | Idea | Mechanism / why | Impact | Effort | Risk | Status |
|---|---|---|---|---|---|---|
| N01 | WDL/result-blended loss | Train with game result + eval mix instead of only cp SmoothL1; better calibrated leaf eval | +15-45 (unverified) | M | med | planned |
| N02 | Low-LR final finetune | One or two extra epochs at 1/10 LR (SF commit 2d54ef335a) | +5-20 | S | low | planned |
| N03 | Distill h128 from a stronger teacher | Fit student to h256/threat/K32H3 search or eval; inference cost unchanged | +25-80 (unverified) | M | med | planned |
| N04 | Distill from existing accepted h256 net | Cheapest distillation source already in `/artifacts/networks` | +10-40 | M | med | new |
| N05 | Mix depth-8 and selected deeper labels | Deep labels only for high-disagreement rows, avoid full relabel cost | +0-15 | M | med | new |
| N06 | Quiescence-filtered labels (Gemini) | Skip/sample-separate rows with pending captures or in-check; cleaner leaf labels | +5-25 (unknown) | S-M | med | new |
| N07 | Near-duplicate position dedupe | Hash-feature/dedup near-identical FENs to reduce overfitting | +0-10 | M | low | new |
| N08 | Per-game position caps + phase balance | Avoid opening/middlegame/endgame imbalance in rows | +0-10 | S | low | new |
| N09 | Endgame oversampling | More KRK/KP/KQ/KR endings in labels | +0-10 | S | low | new |
| N10 | Stratified batch sampling | Shuffle by phase/material bucket | +0-5 | S | low | new |
| N11 | Output-scale multiplier grid | Multiply NNUE output by 0.9/0.95/1.05/1.1/1.2; quick gates | 0-30 | XS | low | planned |
| N12 | Output-shift search in trainer | Grid-search output shift before quantization | 0-15 | S | low | new |
| N13 | Output buckets 8->16/32 | Finer material buckets; small inference cost | +0-15 | M | med | new |
| N14 | L2 16/32 + dual activation | Alexandria/SF head pattern | +5-20 | M | med | new |
| N15 | quantmoid4 output | SF's quantized sigmoid output | +0-10 | M | med | new |
| N16 | int16 accumulators | Halve accumulator memory bandwidth | +5-10% NPS | M | med | new |
| N17 | Finny tables | Avoid full king-bucket refresh on 7.4% of makes | +3-8% NPS | L | med | new |
| N18 | PP_3Wide pawn-pair features | Stormphrax reported large gains with pawn structure | 0-40 (high variance) | L | high | new |
| N19 | Full-threat features | SF ships them; Koi's 36,864-input attempt lost | 0-40 (high variance) | L | high | new, gated |
| N20 | K32H3 king buckets | More king granularity | 0-25 | L | high | new |
| N21 | Piece-count output buckets vs material signature | Simpler/faster bucket selection | 0-5 | M | med | new |
| N22 | Symmetry augmentation with WDL loss | Previous cp-loss attempt was -12.2 | 0-10 | M | med | new (retry) |
| N23 | Factorized/other arch with fixed quantization grid | Prior v5 small nets were unusable | 0-10 | L | high | rejected (retry only with grid) |
| N24 | Time-odds validation for nets | Separate eval quality from eval cost | quality | S | low | planned |
| N25 | SF/Lichess database bootstrap data | Broaden style/phase coverage | 0-20 | L | high | new |
| N26 | Deeper labels only on disagreement subset | Current depth-10 relabel whole-corpus was -35 | 0-10 | M | med | new |
| N27 | Hard-position mining from bot losses | Target recurring blunders | +0-15 | M | med | new |
| N28 | STS/tactics suite as cheap pre-screen | Catch eval regressions before SPRT | speed | S | low | new |
| N29 | `eval` command / NNUE breakdown | Debug eval scale systematically | quality | S | low | planned (F07) |
| N30 | Float-vs-quantized consistency test | Catch quantization/output-shift bugs | quality | S | low | new |
| N31 | AVX-512/VNNI int8 head kernels | x86-only inference speed | +2-8% NPS | M | low | new |
| N32 | NEON int16 kernels | Mac inference speed (prior NEON attempts neutral) | 0-6% NPS | M | med | new |
| N33 | Prefetch weights/accumulator | Reduce cache misses | +1-3% NPS | S | low | new |
| N34 | Sparse feature extraction optimisation | Feature listing cost | +1-4% NPS | M | med | new |
| N35 | Quantization-aware training | Less quantization loss at same time | 0-10 | M | med | new |
| N36 | Training data from current net with temperature/noise | Self-play distribution refresh | 0-15 | L | high | new |
| N37 | Accelerator: run training on Mac GPU/MLX? | Faster campaign iteration | speed | L | high | new |
| N38 | Net card/lineage metadata improvements | Reproducibility | quality | XS | low | new |

### B. Search, pruning and move ordering (S)

| ID | Idea | Mechanism / why | Impact | Effort | Risk | Status |
|---|---|---|---|---|---|---|
| S01 | Dynamic aspiration + fail-high recovery | Delta from previous score; reduce depth on repeated fail-high | +5-15 | M | med | planned |
| S02 | Optimism eval bias | Add bounded root-score optimism to static eval (SF) | +2-8 | S | low | planned |
| S03 | Corrhist expansion + gravity | Minor/major/continuation keys, exponential decay, up-to-date static eval | +5-15 | M | low | planned |
| S04 | Pawn history in pruning | Add pawn history to negative-history pruning sum | +2-8 | S | low | new |
| S05 | Up-to-date static eval for corrhist update | SF/Viridithas pattern | +1-5 | S | low | new |
| S06 | LMR loose-alpha-window refinement | Reduce less when alpha far below score | +3-10 | M | med | new (prior full surface retune negative) |
| S07 | LMR reduction per alpha-raise | Incremental reduction when alpha raises | +2-6 | M | med | new |
| S08 | Mate-finding futility depth cutoff / seekMate | Better mate discovery near horizon | +2-6 | S | low | new |
| S09 | IIR/IID tuning | Reduce/raise internal iteration thresholds | 0-5 | S | low | new |
| S10 | Singular extension refinements | Double/triple extension conditions | 0-8 | M | med | new |
| S11 | ProbCut margin/conditions | Revisit with tuned NNUE scale | 0-8 | S | med | new |
| S12 | TT 16-byte lockless entries | Halve TT bandwidth, better SMP | +2-8% NPS | L | high | new |
| S13 | TT aging/replacement policy | Avoid stale deep entries | +1-4 | M | med | new |
| S14 | TT move/score validation harden | Avoid rare bad entries | quality | S | low | new |
| S15 | Staged move generation | Avoid eager full-list SEE/history annotation (26.5% picker cost) | +5-15% NPS | L | high | new |
| S16 | Capture history refinements | Include capture type/victim/attacker | +1-4 | S | low | new |
| S17 | Quiet history gravity/decay | SF-style history aging | +1-5 | S | low | new |
| S18 | Killers per ply / more slots | More tactical ordering info | 0-3 | S | low | new |
| S19 | Countermove table breadth | All piece types | 0-3 | S | low | new |
| S20 | Continuation history 6 plies | Already 6; tune weights | 0-4 | S | low | new |
| S21 | Follow-up capture history | Extra ordering key | 0-3 | S | low | new |
| S22 | SEE pruning threshold tuning | Capture/quiet SEE gates | 0-6 | S | med | new |
| S23 | Late move pruning revisit with history gate | Prior LMP -17.4 Elo as implemented | 0-8 | M | med | new (different design only) |
| S24 | Null-move verification | Verify deep null-move cutoffs at high depth | +1-5 | M | med | new |
| S25 | Null-move reduction base 4/5/6 | Base 3 rejected; 4-6 untested | 0-6 | S | med | new |
| S26 | Qsearch delta margin tuning | 100->200 neutral; try position-aware | 0-4 | S | med | new |
| S27 | Qsearch SEE prune refinement | Avoid losing captures | 0-5 | M | med | new |
| S28 | Incremental repetition detection | Avoid scanning history per node | +1-3% NPS | M | med | new |
| S29 | Draw-status check ordering | Cheap checks first | +0-2% NPS | S | low | new |
| S30 | Root safety shell simplification | Koi-only root battery, unclear value | unknown | M | med | planned (A14) |
| S31 | Root ordering revisit | Prior previous-iteration ordering -4.1 | 0-5 | S | med | new (different formulation only) |
| S32 | MultiPV search efficiency | Shared work across PVs | analysis speed | M | med | new |
| S33 | SMP thread selection | Viridithas pattern | +5-20 at T4 | S-M | low | new |
| S34 | SMP helper depth offsets | Better helper diversity | +0-10 at T4 | S | low | new |
| S35 | Odd-thread NMP jitter | SF 61c6871785 | +0-5 at T4 | XS | low | new |
| S36 | Node-limited vs time-limited tuning split | Different optimums | quality | S | low | new |
| S37 | Search-stack locality/pressure | Keep hot per-ply data together | +1-3% NPS | M | med | new |
| S38 | Prefetch TT/corrhist/continuation entries | Reduce memory stalls | +1-3% NPS | S | low | new |
| S39 | Batched NNUE evaluation for T>1 | Amortize inference across threads | +2-5% T4 | M | med | new |
| S40 | Eval-cache sizing/tuning | 64K entries currently | 0-3% NPS | S | low | new |
| S41 | TT probe/probe-store micro-opt | 5.2% wall | +0-2% NPS | S | low | new |
| S42 | PVS re-search reduction | Fail-high/hard re-search policy | +0-3 | S | med | new |
| S43 | Aspiration reset/fail-low policy | Avoid repeated narrow windows | +0-3 | S | med | new |
| S44 | Move-count pruning tuning | Late-move counts per depth | 0-4 | S | med | new |
| S45 | Internal iterative reduction threshold | SF-style | 0-3 | S | low | new |
| S46 | SEE implementation micro-opt | Shared exchange chain already exists | +0-1% NPS | XS | low | new |
| S47 | History table memory layout | Cache locality of history arrays | +0-2% NPS | S | low | new |
| S48 | MoveMetadata compaction | Reduce per-move memory traffic | +1-3% NPS | M | med | new |

### C. Time management and game play (T)

| ID | Idea | Mechanism / why | Impact | Effort | Risk | Status |
|---|---|---|---|---|---|---|
| T01 | Ply/time-advantage TM | SF timeman: opt/max scale from log time, ply, time advantage | +10-30 clock games | M | low | planned |
| T02 | Score-stability scaling | Scale time by best-move instability/falling eval | +5-15 clock | M | low | planned |
| T03 | Falling-eval extension | Spend more when eval drops | +0-8 clock | S | low | new |
| T04 | Node-effort scaling | Use nodes spent so far | +0-5 | S | low | new |
| T05 | Hard-cap formula | Ensure never flags with overhead | safety | S | low | new (rediscover SF 0.8097) |
| T06 | Clock-overhead learning improvement | Already learns GUI/network overhead; refine | safety | S | low | new |
| T07 | Increment handling at low time | 1+0/2+1 behavior | +5-15 clock | S | med | new |
| T08 | Ponder time allocation | `optimumTime/4` equivalent | +0-5 clock | XS | low | new |
| T09 | `movestogo` allocation tuning | Sudden-death vs movestogo budgets | +0-5 | S | med | new |
| T10 | Fast-TC depth guard | Ensure minimum depth at fast TC | safety | S | med | new |
| T11 | Time-forfeit regression suite | Simulated clock games checking flags | safety | M | low | planned |
| T12 | Time-management simulation tests | Unit-test budgets across TCs | safety | S | low | new |
| T13 | Time distribution by game phase | More time in critical middlegame | +0-8 | M | med | new |
| T14 | Low-time search quality | Avoid too many threads/low depth scramble | +0-5 | S | med | new |
| T15 | Bot time-control analysis | Mine bot games for time losses | safety | S | low | planned |
| T16 | 1+0 ladder/confirmation automation | Reproducible TC suite | quality | S | low | new |
| T17 | Move overhead option semantics review | Ensure full movetime rule preserved | quality | XS | low | covered |
| T18 | Adjudication-free time safety checks | Avoid relying on opponent flags | quality | S | low | new |

### D. UCI / product features (F)

| ID | Idea | Mechanism / why | Impact | Effort | Risk | Status |
|---|---|---|---|---|---|---|
| F01 | `UCI_LimitStrength` + `UCI_Elo` + Skill Level | SF-style root filtering; internal calibration + bot levels | feature | M | low | new |
| F02 | Fitted WDL model | Replace linear draw heuristic with fitted win/draw/loss curves | analysis quality | S | low | new |
| F03 | WDL for estimate/bound lines | Analysis GUIs get calibrated WDL everywhere | feature | XS | low | new |
| F04 | `currmovenumber` emission | Paired with currmove | feature | XS | low | covered |
| F05 | Raise MultiPV maximum to 256/500 | Deep analysis use | feature | S | low | new |
| F06 | Built-in `bench` command | Reproduce NPS from any GUI/binary | feature | S | low | new |
| F07 | `eval` command / NNUE breakdown | Debug and analysis | feature | S | low | new |
| F08 | Syzygy interior probing default for <=5-man | Better endgame play | endgame | S | med | new |
| F09 | DTZ/50-move-aware TB output | Correct TB play | quality | M | med | new |
| F10 | In-engine `perft` command | Testing convenience | dev UX | XS | low | new |
| F11 | `info string compiler` / build details | Reproducibility | feature | XS | low | new |
| F12 | Ponder option time behavior | Verify/document | feature | XS | low | new |
| F13 | `StrengthMode` honesty | Advertised but neutral; implement or remove | feature | XS | low | new |
| F14 | Better option rejection diagnostics | One `info string` per rejected option | UX | S | low | new |
| F15 | `d` / `flip` / `show` debug commands | GUI/debug convenience | UX | XS | low | new |
| F16 | Debug file logging improvements | Rotate/limit/robust writes | reliability | S | low | new |
| F17 | Case-insensitive option names robustness | UCI tolerance | reliability | XS | low | new |
| F18 | Unknown-option warnings | Currently silent ignore | UX | XS | low | new |
| F19 | Option boundary documentation | Ranges/behaviour in USER_GUIDE | docs | XS | low | new |
| F20 | MultiPV ordering guarantee doc/test | Ensure deterministic ordering | quality | XS | low | new |
| F21 | WDL consistency test suite | Cross-check writer fields | quality | S | low | new |
| F22 | Hashfull accuracy/reset behavior | Verify semantics | quality | S | low | new |
| F23 | Book selection weight/variety improvements | Better opening diversity for bot | games | M | low | new |
| F24 | Book safety probe tuning | Avoid dubious book moves | games | S | low | new |
| F25 | Lichess-DB book builder improvements | More/better CC0 book | games | M | low | new |
| F26 | Syzygy auto-discovery recursive/more names | User convenience | feature | S | low | new |
| F27 | TB path validation diagnostics | Clearer messages | UX | S | low | new |
| F28 | CLI `--help`/`--version` | Engine UX | XS | low | new |
| F29 | Log redaction | Privacy | XS | low | new |
| F30 | Remove GPU NNUE path | Hygiene/maintenance | quality | M | low | planned |
| F31 | MultiPV analysis-mode book bypass doc | Already covered | docs | XS | low | covered |
| F32 | Ponder regression process tests | Already covered | quality | XS | low | covered |

### E. Reliability / robustness residuals (R)

| ID | Idea | Mechanism / why | Impact | Effort | Risk | Status |
|---|---|---|---|---|---|---|
| R01 | Grammar-aware UCI mutation fuzzer | Mutate valid transcripts (truncate, duplicate, reorder) with replay seeds | crash coverage | M | low | new |
| R02 | libFuzzer/AFL harness for protocol/position parsers | Continuous fuzzing | crash coverage | L | med | new |
| R03 | FEN roundtrip/property tests | Random FEN -> parse -> emit -> parse | correctness | M | low | new |
| R04 | Movegen property tests vs oracle/perft | Random positions, legal move count/keys | correctness | M | low | new |
| R05 | Allocation-failure injection | Huge Hash/Threads/EvalFile under failing allocator | crash coverage | M | med | new |
| R06 | Disk-full / unwritable debug-log handling | Failed log writes must not crash | robustness | S | low | new |
| R07 | Read-only asset directories | Book/net/TB in read-only dirs | robustness | S | low | new |
| R08 | Asset deleted during search | Book/TB file removed while loaded | robustness | S | med | new |
| R09 | TB directory removed after load | Fail-closed probing | robustness | S | med | new |
| R10 | Two engines sharing one book file | Locked-file semantics | robustness | S | low | new |
| R11 | SIGTERM/SIGHUP handling | Clean shutdown | robustness | S | low | new |
| R12 | Windows console close / Ctrl+C | Clean shutdown | robustness | M | low | new |
| R13 | stderr pipe close | SIGPIPE already ignored; test stderr | robustness | XS | low | new |
| R14 | stdin binary garbage fuzz | Non-UTF8 bytes, NULs | robustness | S | low | new |
| R15 | 1 MiB boundary variants | Exactly cap, cap+1, CRLF endings | robustness | XS | low | new |
| R16 | Option values with quotes/backslashes/Unicode paths | Path parser robustness | robustness | S | low | new |
| R17 | Handshake storm (1000x `uci`) | Output/flood behavior | robustness | S | low | new |
| R18 | stop/quit/ponderhit race fuzz | Random interleavings under threads | crash coverage | M | med | new |
| R19 | Book hot-swap during active search | Currently tested idle-ish | robustness | M | med | new |
| R20 | Syzygy auto-load race with option change | Deferred load ordering | robustness | M | med | new |
| R21 | EvalFile swap storm (valid/invalid mix) | Repeated install failures | robustness | M | med | new |
| R22 | Multi-hour memory-leak soak | RSS bounds after 10k+ searches | reliability | L | low | new |
| R23 | ASan fuzz corpus in CI | Sanitized protocol fuzz | crash coverage | M | med | new |
| R24 | TSan option-churn leg | Thread races around options | crash coverage | M | med | new |
| R25 | Deterministic failed-fuzz replay | Persist minimal failing transcript | dev UX | S | low | new |
| R26 | Flake detector for process tests | Repeated runs, timing census | reliability | S | low | new |
| R27 | Symlink asset handling | Symlinked book/net/TB | robustness | S | low | new |
| R28 | Unicode path assets | Non-ASCII paths | robustness | S | low | new |
| R29 | Unhandled-exception diagnostic | Simple crash message/exit code | robustness | S | low | new |
| R30 | Graceful OOM abort | Report instead of silent kill | robustness | M | med | new |
| R31 | Fuzz `position`/`go` state-machine combinations | Cross-product of commands | crash coverage | M | low | new |
| R32 | Fuzz analysis mode / MultiPV / TB combinations | Higher-level modes | crash coverage | M | med | new |
| R33 | Fuzz book/Syzygy asset matrix | Corrupt/truncated/empty/permission | crash coverage | M | low | new |
| R34 | Fuzz time-controls (negative, huge, zero, mixed) | Budget edge cases | crash coverage | S | low | new |
| R35 | Crash-free `quit` during every phase | Load, search, resize, asset load | robustness | M | low | new |

### F. Measurement and experiment process (M)

| ID | Idea | Mechanism / why | Impact | Effort | Risk | Status |
|---|---|---|---|---|---|---|
| M01 | UHO unbalanced openings | More decisive games for same SPRT variance | 20-30% fewer games | S | low | planned |
| M02 | Pentanomial pair-level SPRT | Current script sums per-colour trinomial LLRs | fewer games | S-M | low | planned |
| M03 | Fastchess migration | Robust, parallel, compliance checker | throughput | M | low | new |
| M04 | Gate SMP changes at Threads=4 | Ladder config; Threads=1 gates miss SMP gains | quality | S | low | planned |
| M05 | LTC decisions for small deltas | 200 ms noise floor flipped results before | quality | S | low | planned |
| M06 | Time-odds net gates | Separate quality from speed | quality | S | low | planned |
| M07 | Mandatory NPS gate for perf changes | Avoid neutral NPS experiments | process | S | low | new |
| M08 | A/A calibration SPRT | False-positive floor of the harness | process | S | low | new |
| M09 | Fixed-depth vs fixed-time diagnostic | Gemini's useful idea; depth vs quality | diagnosis | S | low | planned |
| M10 | Depth/NPS/effort telemetry per gate | Store depth, nps, qnode share, time used | diagnosis | M | low | new |
| M11 | Machine-readable gate manifests | Provenance/audit | process | S | low | new |
| M12 | Opening corpus expansion/refresh | Avoid opening overfit | quality | M | low | new |
| M13 | Duplicate-game detection improvements | Prior gate had 68 inflated duplicates | quality | S | low | new |
| M14 | SPRT early-stop calibration tests | Verify LLR implementation | correctness | S | low | new |
| M15 | Per-loss game analysis pipeline | Turn games into positions/ideas | diagnosis | M | low | new |
| M16 | Passive bot-game evaluation | Real-game telemetry without new matches | diagnosis | S | low | new |
| M17 | Ladder anchor suite refresh | More anchors, CIs | quality | M | low | new |
| M18 | Cumulative Elo tracker since 1.1.5 | Program-level reporting | process | XS | low | new |
| M19 | Baseline binary archive per accepted change | Reproducibility | process | XS | low | new |
| M20 | Local Mac gate queue automation | Run gates unattended between bot games | throughput | M | med | new |
| M21 | Parallel independent gates on spare cores | Batch experiments | throughput | M | med | new |
| M22 | Node-count bench suite for search-quality diagnosis | Separate depth vs time | diagnosis | S | low | new |
| M23 | Eval-quality suite (STS/tactics/positional) | Cheap pre-screen before SPRT | quality | M | med | new |
| M24 | Time-control matrix (200ms/1+0/10+0) | Detect TC-specific gains | quality | M | med | new |
| M25 | SPRT on fixed nodes for search changes | Removes time noise | quality | S | low | new |

### G. Engineering, performance and maintenance (E)

| ID | Idea | Mechanism / why | Impact | Effort | Risk | Status |
|---|---|---|---|---|---|---|
| E01 | Remove `Position` pimpl indirection | Per-node pointer deref | +2-6% NPS | M-L | med | new |
| E02 | Staged move generation | See S15 | +5-15% NPS | L | high | new |
| E03 | PGO + LTO for Mac gate binary | Berserk-style; release stays generic | +3-10% NPS | S | low | new |
| E04 | CPU-variant dispatch improvements | Ensure generic/AVX2/AVX512 selection | +0-3% | S | low | new |
| E05 | AVX-512/VNNI NNUE kernels | x86 inference | +2-8% NPS | M | low | new |
| E06 | NEON int16 kernels | Mac inference | 0-6% NPS | M | med | new |
| E07 | 16-byte lockless TT | See S12 | +2-8% NPS | L | high | new |
| E08 | Finny tables | See N17 | +3-8% NPS | L | med | new |
| E09 | Hot-path memory layout pass | SearchStack, history tables, MoveMetadata | +1-5% NPS | M | med | new |
| E10 | Remove hot-path allocations | Verify no per-node allocations | +1-3% NPS | M | med | new |
| E11 | Modules build cleanup | Maintenance | quality | M | low | new |
| E12 | Dead code removal | `move_chooser`, leftover classical remnants, GPU path | maintenance | S | low | planned |
| E13 | Compile-time reduction | Split headers, reduce templates | dev speed | M | low | new |
| E14 | CI build caching | Faster CI | process | S | low | new |
| E15 | Benchmark CI job | NPS regression detection | process | S | low | new |
| E16 | Sanitizer matrix expansion | ASan fuzz, TSan churn | reliability | M | med | new |
| E17 | Static analysis in CI | clang-tidy warnings | quality | M | low | new |
| E18 | Coverage reporting | Untested paths | quality | M | low | new |
| E19 | Binary size reduction | Packaging UX | quality | S | low | new |
| E20 | Structured debug logging | Reproducible diagnostics | quality | S | low | new |
| E21 | Unit-test sharding speedups | CI runtime | process | S | low | new |
| E22 | Process-test parallelization | CI runtime | process | S | low | new |
| E23 | Docs drift checker | Option/tests/docs consistency | quality | M | low | new |
| E24 | Windows ARM64 experiment | Platform reach | feature | L | high | new |
| E25 | Linux ARM64 release target | Platform reach | feature | M | med | new |

---

## Part 4 — Prioritized shortlist

### Tier 0 — finish diagnostics first (this week)
1. **P01/profiler** (running): fresh CPU profile + NPS/depth/accumulator counters.
2. **M09/M10**: fixed-depth vs fixed-time diagnostic and depth/NPS telemetry; save a report.
3. **M02**: pentanomial pair-level SPRT (cheap, immediately reduces future gate cost).
4. **M07**: NPS gate for perf changes.
5. **A3/N11**: eval-scale calibration probe (cheap, may explain margin mismatches).

### Tier 1 — cheap search/eval wins (1-3 weeks, each separately gated)
6. **A8**: drop fullmove from TT/eval keys (hit-rate, needs invariant/test update).
7. **A10/S04**: pawn history in pruning.
8. **A9/S02**: optimism eval bias.
9. **A4/S01**: dynamic aspiration + fail-high recovery.
10. **A6/S03**: corrhist expansion + gravity.
11. **S33-S35**: SMP diversification, gated at Threads=4.
12. **N01/N02**: WDL/result loss + low-LR finetune (the main NNUE experiment).

### Tier 2 — NNUE campaign (4-8 weeks)
13. **N06/N07**: quiescence-filtered labels + dedupe/balance.
14. **N03/N04**: distillation from a stronger teacher into h128.
15. **N13-N15**: head architecture (buckets/L2/quantmoid), equal-time gated.
16. **A5/T01/T02**: TM modernization for real games, LTC confirmation.

### Tier 3 — performance engineering (only with LTC/Threads=4 evidence)
17. **E03**: PGO+LTO Mac gate binary.
18. **N16/N17**: int16 accumulators + finny tables.
19. **E01/E09**: layout/locality, pimpl removal.
20. **E02/E07**: staged movegen, 16 B TT (big refactors; last).

### Tier 4 — features/product (parallel, low risk)
21. **F01**: UCI_LimitStrength/UCI_Elo + Skill (bot levels, internal testing).
22. **F02/F03**: fitted WDL model and WDL for estimate lines.
23. **F06/F07**: `bench`/`eval` commands; **F04** done.
24. **F08/F09**: Syzygy interior + DTZ/50-move awareness.
25. **F23-F25**: book/bot opening variety.

### Tier 5 — reliability residuals (keep program running in background)
26. **R01/R31**: grammar-aware mutation fuzzer + state-machine cross-product.
27. **R05/R06**: allocation/disk failure injection.
28. **R17-R21**: storms and asset churn during active search.
29. **R22**: multi-hour soak + leak bounds.
30. **E15/E16**: bench and sanitizer CI legs.

### Explicit non-goals / do-not-retry
- Wider nets at equal time (h160 -49.5, h256 -40.6); 512 hidden is not free.
- Deeper teacher labels (d10 -35.4 SPRT reject; d13 subset -55).
- Whole-corpus data scaling (+37% neutral) and extra epochs (neutral).
- Threat-pair 36,864-input net (33.75% vs official), v5 small nets without a
  fixed quantization grid.
- NMP base 3, RFP 60/100, aspiration 25, quiet SEE pruning, capturing-promotion
  classification, LMR full-surface retune, root ordering by previous scores, LMP
  as implemented, qsearch delta 200, continuation-symmetry malus.
- Lazy-SEE ranking in qsearch without redesigning qsearch pruning.
- CPU-feature probes in per-node code (cost ~4x NPS).
- Porting SF constants/net sizes blindly; Koi's NNUE scale differs.
- Trusting validation MAE, 64-game screens, or a single 256-game run as strength
  evidence; gating on Windows; multiple variables per gate.

### Gemini-derived experiments worth actually running (cheap)
- **G03** accumulator instrumentation: count incremental makes vs fallbacks and
  refresh cost share (profiler).
- **G06 alternative**: instead of 512 hidden, test h128 with a small L2 head and
  16 output buckets (N13/N14) at equal time.
- **G07 alternative**: quiescence-filtered labels (N06), not deeper labels.
- **G08**: verify output scale/de-scaling with an eval-scale grid (N11).
- **G12**: fixed-depth vs fixed-time diagnostic (M09).

---

## Part 5 — Notes / open questions

- The profiler report (`artifacts/profiles/2026-10-31/`) is pending; it should
  refine E01/E02/E07/E08 priorities and either confirm or refute the
  "accumulator refresh leak" hypothesis.
- Any NNUE experiment needs a frozen baseline binary + net and the M02/M04/M05
  measurement fixes first, otherwise +5-10 Elo deltas are not decidable.
- The reliability program remains active until the user says "Stop"; strength
  work must not bump version/tag/release without explicit approval.
- This catalog is a backlog, not a commitment; each item still needs a gate
  design and can be rejected by evidence.
