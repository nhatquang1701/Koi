# Koi CPU profile — 2026-10-31 (Mac mini M4, arm64)

Fresh, deeper CPU profile of the Koi chess engine, produced by an external
profiling teammate. Two workloads: (a) `koi-bench` full 64-position suite,
Threads=1, promoted h128 net; (b) UCI `go infinite` from a middlegame FEN for
40 s at Threads=1 and Threads=4. CPU samples taken with the macOS `sample`
tool (1 ms interval, 25 s per capture, call-tree with symbols).

**Status: complete.** Lichess bot was paused via the documented
placeholder→autostart mechanism for the profiling window (18:21–18:50) and then
restarted (`~/koi-bot/koi-bot.sh start`, "bot started (launchd)"), verified
running (pid 53213; it resumed playing games normally). No engine orphans left;
no source/test/docs/config edits; no commits.

## Provenance

| Item | Value |
| --- | --- |
| Host | `ssh mac-mini`: Apple M4, 10 cores, macOS 27.0.1, load avg 1.5–2.7 during runs (idle dev apps only) |
| Profiled binary | `~/koi-head/build/release/koi-engine` SHA-256 `d757b4a18315521acf057a218ec6f6bc16786365207962200aec588ad6670048`, `info string build 1e09504e6569`, `id name Koi 1.1.5`, AppleClang Release `-O3 -DNDEBUG` |
| Profiled bench | `koi-bench` SHA-256 `1ac5640cb35298feb4bbd629d464e700f6fd74cd1700e48923ee445d8e930062` |
| Network | promoted `koi-v7-h128-official-20261025` at `~/koi-bot/engines/koi.nnue`, SHA-256 `3e75e6d84d7b4ac0b0d46574aeb714032ea4ffff1a639c9d541e9663545829da` (2,360,459 B; same promoted net as Windows, `KOI_NNUE_PATH` points at it) |
| Mac tree state | `~/koi-head` HEAD `1e09504` (= release tag 1.1.5) + uncommitted changes to `docs/*`, `src/koi/syzygy_tablebase.*`, `src/koi/uci_controller.cpp`; the clone does not contain `d6a4a1e`/`dd4e686` |
| Hot-path equivalence to current HEAD | `git diff --stat 1e09504..HEAD -- src/koi` = only `search_types.hpp` (+comment only), `syzygy_tablebase.{cpp,hpp}`, `uci_controller.{cpp,hpp}`. `nnue.cpp/hpp`, `search_context.*`, `search_ordering.*`, `transposition_table.*`, `position.cpp`, `game_state.cpp`, `evaluation_features.cpp`, `attack_tables.*`, `static_exchange.*` are **byte-identical** between the profiled build and current HEAD → the profile applies to current code (`dd4e686`/`7456aa7`/`17dd912` lineage). |
| Analyzer | `analyze_sample.py` in this directory (parses `sample` call graph, computes self/inclusive-once, qsearch subtree, ancestor probes) |

**Post-profile repo state (19:25):** while this report was being written, another
session committed `d49ce0f` (perf: reuse NNUE refresh sums buffer — candidate #2
below) and HEAD moved to `8201422`. The profiled Mac binary does **not** include
that change, so the NNUE refresh numbers here are the pre-fix baseline; the
hot-path equivalence note above holds for commits up to `17dd912` and is
superseded by `d49ce0f` only for the refresh-buffer lines. The profiler made no
source/test/docs edits and no commits.

## Method (exact commands)

Bot-safety (documented approach, no bot config touched):

```bash
# on Mac, while idle between games:
nohup bash -c 'while :; do sleep 30; done' koi-bench-placeholder &   # autostart sees heavy work
# autostart.log shows: "heavy work detected and no game in progress; pausing the bot" + "bot stopped (launchd)"
# ... profiling ...
kill <placeholder-pid>; ~/koi-bot/koi-bot.sh start    # verified: bot started (launchd), pid 53213
```

Workloads (scripts `phaseA-mac.sh`, `phaseB-mac.sh` in this directory):

```bash
export KOI_NNUE_PATH=~/koi-bot/engines/koi.nnue
# (a) bench, Threads=1, full 64-position suite at fixed depth 5, cold hash per position:
./koi-bench --nnue "$KOI_NNUE_PATH" --threads 1 --depth 5 --repeat 1 \
    --profile-json bench-t1-profile.json > logs/bench-sample.log 2>&1 &
sample -mayDie <koi-bench-pid> 25 -file samples/bench-t1.sample.txt

# (b) UCI infinite, middlegame FEN r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3,
#     Threads=1 and Threads=4, Hash 512, go infinite, 25 s capture starting ~6 s in:
{ printf 'uci\nisready\nsetoption name Threads value %s\nsetoption name Hash value 512\nisready\nposition fen <FEN>\ngo infinite\n' N; sleep 40; printf 'stop\nquit\n'; } | ./koi-engine &
sample -mayDie <koi-engine-pid> 25 -file samples/uci-tN-infinite.sample.txt
```

Supplementary probes (same scripts): startpos/middlegame `go movetime 200/1000/5000`,
`go depth 8`, `go wtime 1000 btime 1000`, Threads 1 and 4 with wall-clock timing.

## TL;DR headline numbers

- **NNUE make/accumulator notification is the top cost: 50.9–52.7 % inclusive,
  43.1–45.0 % self** of search CPU in every workload (`on_make_move`, which
  inlines `apply_move_deltas`/`apply_feature_delta` on arm64 — there is no AVX2
  kernel in this build, so deltas are the scalar int64-clamp loop).
- **Move generation + legality + metadata: ~27 %** of search CPU
  (`legal_moves_with_metadata` 13.1 % incl, `candidate_is_legal` 9.2 %,
  `native_move_gives_check` 4.3 %, `generate_pseudo` 5.0 %, attack tables 7.8 %).
- **Move ordering + SEE: ~8 %** (picker `next` 8.9 % incl, `prepare_candidates`
  6.1 %, `static_exchange*` 3.6 %); **TT 4.5–4.8 %** at T1 and **6.2–7.0 %** at T4;
  **repetition/draw bookkeeping is 0.1 %** (refutes the concern that it is costly).
- **Quiescence dominates the tree**: 79.0–88.4 % of search samples and
  **98.53 % of visited nodes** in the bench suite. Depth-5 over 64 positions =
  4.98 M main nodes + **333.9 M qnodes** (67 qnodes per main node).
- **NPS / depth (T1, startpos)**: 0.90–0.94 M NPS; `movetime 200` reaches depth 5
  (~36 k nodes); `movetime 1000` reports depth 7 but the depth-6 iteration was
  the last completed (415 ms), and `movetime 5000` still has the **same depth-7
  iteration in progress at 4.9 s** (4.42 M nodes) — 5 s buys no completed ply
  over 1 s. Middlegame FEN: depth 6 completes at 4.2 s; depth 7 still in
  progress after **40 s / 29.3 M nodes**.
- **SMP**: 4 threads give ~2.0–2.2× NPS at completed iterations
  (T1 0.94 M → T4 1.85 M NPS at 1 s, 1.98×; fixed depth 8: 0.93 M → 2.03 M,
  2.18×), but need 1.9–2.2× the nodes for the same depth and do **not** reach a deeper ply in the
  same fixed time in these tests. The "1.2× at 5 s" reading is an artifact:
  helper node counters are only published when a helper iteration completes
  (`search_runner.cpp:2001-2011,2061`), so long in-flight iterations are omitted
  from `info nps` at long TC. All 4 helper threads are 99.3–99.7 % inside the
  search stack (no idle/spin helpers).
- **Solved/deepened accumulator question**: the *full-rebuild fallback* path is
  effectively invisible in steady search (NnueWorker::evaluate has ~0
  extract/encode children) → the review's "fallback refresh leak" is **not**
  supported. The visible accumulator extra cost is the **king-bucket refresh**
  (~3.6–6.7 % of search CPU incl. its children, of which 0.7–2.3 % is
  malloc/free/memset from a per-call `std::vector` allocation) plus the inlined
  scalar accumulation hidden in `on_make_move` self time. Exact call rate cannot
  be measured because `incremental_make_count_`/`incremental_fallback_count_`
  have no callers and no debug/bench path exposes them; no sudo → no dtrace.

## Table 1 — top 15 by self samples, UCI middlegame `go infinite`, Threads=1

25 s capture, 18,981 search-thread samples (100 %); main thread idle excluded.
`self` = samples whose leaf is in the function; `incl` = samples whose stack
contains the function (once).

| # | symbol (short) | source | self % | incl % | what it does in Koi |
| --- | --- | --- | --- | --- | --- |
| 1 | `NnueEvaluatorWorker::on_make_move` | `src/koi/nnue.cpp:2162` (inlines `apply_move_deltas` 2048, `prepare_child_slot` 2135 not fully inlined, `refresh_slot_perspective` 1957) | **44.96** | **52.27** | Per-move NNUE accumulator update: copy parent slot forward (2×128 int32), apply halfka feature deltas for both perspectives (scalar int64 clamp path on arm64), king-bucket refresh on crossing |
| 2 | `TranspositionTable::probe_exclusive` | `src/koi/transposition_table.cpp:835` (via `detail/search_table_access.hpp:147`) | 4.51 | 4.51 | Serial-driver TT probe through the pinned session storage (unlocked/exclusive path), 24-byte entries |
| 3 | `detail::bishop_attacks` | `src/koi/detail/attack_tables.cpp:249` | 4.22 | 4.22 | Magic/table bishop attack generation (feature extraction, movegen, legality, SEE) |
| 4 | `NativePosition::candidate_is_legal` | `src/koi/position.cpp:1061` | 4.11 | 9.18 | Per-pseudo-legal-move legality filter (pins/checkers/ep) |
| 5 | `detail::rook_attacks` | `src/koi/detail/attack_tables.cpp:259` | 3.62 | 3.62 | Magic/table rook attack generation |
| 6 | `_platform_memmove` | libsystem | 3.21 | 3.21 | Accumulator/slot copies (`std::copy_n` in `prepare_child_slot`, `copy_slot_forward`, feature-state copies) |
| 7 | `NnueEvaluatorWorker::evaluate` | `src/koi/nnue.cpp:2228` (forward pass `NnueWorker::evaluate` 1820) | 3.11 | 3.13 | NNUE forward pass (accumulator → h128 → buckets) per eval, no feature rebuild on the hot path |
| 8 | `NativePosition::generate_pseudo<false, AddAllMoves>` | `src/koi/position.cpp:1112/1164` | 3.04 | 4.99 | Pseudo-legal move generation (all moves/quiet stage) |
| 9 | `native_move_gives_check` | `src/koi/game_state.cpp:1119` | 3.02 | 4.26 | Post-make check detection for metadata/pruning |
| 10 | `SearchContext::quiescence` (self) | `src/koi/detail/search_context.cpp:77` | 1.92 | **78.96** | Quiescence search recursion (all captures/evasions); children dominate the tree |
| 11 | `SearchMovePicker::prepare_candidates` | `src/koi/search_ordering.cpp:505` | 1.63 | 6.07 | Build + score the staged move list for the node |
| 12 | `generate_pseudo(...)::lambda` | `src/koi/position.cpp` (inside `generate_pseudo`) | 1.60 | 1.60 | Per-piece move emission lambda (inlined capture/quiet push) |
| 13 | `refresh_king_masks` | `src/koi/position.cpp:526` | 1.41 | 1.99 | Recompute king attack/zone masks after make/unmake |
| 14 | `GameState::legal_tactical_moves_with_metadata` | `src/koi/game_state.cpp:1653` | 1.31 | 7.99 | Capture/promotion/check move list for quiescence |
| 15 | `SearchContext::negamax` (self) | `src/koi/detail/search_context.cpp:646` | 1.05 | 100.00 | Main alpha-beta recursion (self work: node bookkeeping, pruning decisions) |

Also in the top-25: `legal_moves_with_metadata` self 0.90 % / **incl 13.05 %**
(`game_state.cpp:1604`), `SearchMovePicker::next` self 0.86 % / **incl 8.86 %**
(`search_ordering.cpp:738`), `static_exchange_gain_from_features` incl 3.57 %
(`detail/static_exchange.hpp:255`), `fill_move_metadata_from_facts` incl 2.68 %
(`game_state.cpp:1285`), `halfka_king_bucket_indices` self 1.4 % (`evaluation_features.cpp:143`).

## Table 2 — top 12 by self, `koi-bench` 64 positions, depth 5, Threads=1

25 s capture across the bench run (18,939 search-thread samples; bench spawns a
fresh search thread per position, so samples are summed over sequential threads).

| # | symbol (short) | self % | incl % |
| --- | --- | --- | --- |
| 1 | `on_make_move` | 39.31 | 52.66 |
| 2 | `rook_attacks` | 6.04 | 6.04 |
| 3 | `_platform_memmove` | 5.61 | 5.61 |
| 4 | `bishop_attacks` | 4.86 | 4.86 |
| 5 | `candidate_is_legal` | 4.46 | 13.92 |
| 6 | `quiescence` (self) | 3.62 | **88.38** |
| 7 | `native_move_gives_check` | 2.58 | 3.75 |
| 8 | `TranspositionTable::probe_exclusive` | 2.21 | 2.21 |
| 9 | `generate_pseudo` lambda | 2.09 | 2.09 |
| 10 | `halfka_king_bucket_indices` (refresh encode) | 1.92 | 2.12 |
| 11 | `NativePosition::apply_unchecked` | 1.87 | 1.98 |
| 12 | `native_position_features` (refresh extract) | 1.55 | 2.24 |

## Table 3 — top 10, UCI middlegame `go infinite`, Threads=4

4 search threads + 1 idle main thread; 67,660 search samples (16,915/thread).

| # | symbol (short) | self % | incl % |
| --- | --- | --- | --- |
| 1 | `on_make_move` | 43.15 | 50.85 |
| 2 | `TranspositionTable::probe_impl<true>` (locked striped path) | 5.94 | 7.00 |
| 3 | `bishop_attacks` | 4.24 | 4.24 |
| 4 | `candidate_is_legal` | 4.20 | 9.13 |
| 5 | `rook_attacks` | 3.43 | 3.43 |
| 6 | `_platform_memmove` | 3.19 | 3.19 |
| 7 | `generate_pseudo` | 3.16 | 5.03 |
| 8 | `native_move_gives_check` | 3.02 | 4.14 |
| 9 | `quiescence` (self) | 1.85 | 79.18 |
| 10 | `NnueEvaluatorWorker::evaluate` | 1.80 | 1.81 |

Per-thread idle/spin share: 0.3 %, 0.3 %, 0.4 %, 0.7 % (wait symbols only);
helpers are otherwise inside `negamax`/`quiescence` for the whole capture. So 4
threads are genuinely computing, but each does ~1/3 of the single-thread node
rate (aggregate NPS only ~2.1×, nodes for the same depth 2.2–2.3×). The three
helpers are `LazySmpPool` helpers whose depth is
`max(own, published + index%3)` (`search_runner.cpp:2051-2056`) — i.e. all
threads search essentially the same depth class, with no work splitting, and
share the TT via the locked path (probe cost rises 4.5 %→7.0 % incl).

## Bucket summary (% of search samples)

| bucket | bench T1 | UCI T1 | UCI T4 |
| --- | --- | --- | --- |
| NNUE accumulator/inference (on_make, prepare/copy, refresh, evaluate, alloc) | **51.9** | **54.0** | **51.4** |
| Move generation + metadata + legality (+attack tables) | 31.4 | 27.2 | 26.7 |
| Move ordering + SEE | 6.5 | 8.3 | 8.2 |
| TranspositionTable (probe/store) | 2.4 | 4.8 | 6.2 |
| Draw/repetition bookkeeping | 0.2 | 0.1 | 0.1 |
| Search recursion self (`negamax`+`quiescence` bookkeeping) | 4.5 | 3.1 | 3.1 |
| unattributed/other | 2.7 | 2.1 | 3.2 |

Coverage of classified rows: 99.5 % / 99.5 % / 98.8 %.

## Quiescence subtree breakdown

| metric | bench T1 | UCI T1 | UCI T4 |
| --- | --- | --- | --- |
| qsearch share of search samples | 88.4 % | 79.0 % | 79.2 % |
| qsearch bucket: NNUE (make/eval/copy/refresh/alloc) | 45.0 % | 42.1 % | 39.7 % |
| qsearch bucket: movegen+metadata+legality | 28.8 % | 23.3 % | 22.8 % |
| qsearch bucket: ordering+SEE | 4.8 % | 4.5 % | 4.8 % |
| qsearch bucket: TT probe/store | 2.3 % | 4.7 % | 6.1 % |
| qsearch self (recursion bookkeeping) | 3.6 % | 1.9 % | 1.9 % |

Most expensive individual qsearch leaves (UCI T1, % of search):
`on_make_move` 35.2, `probe_exclusive` 4.4, `candidate_is_legal` 3.9,
`bishop_attacks` 3.9, `rook_attacks` 3.3, `evaluate` 3.0, `generate_pseudo` 2.7,
`native_move_gives_check` 2.5, `memmove` 1.9, qsearch self 1.9.

Ancestor-attributed inclusive (UCI T1): the top-level qsearch nodes account for
78.96 % of search samples; inside them the biggest costs directly under
`quiescence` are: `on_make_move` 40.4 %, `legal_moves_with_metadata` 10.8 %,
`legal_tactical_moves_with_metadata` 9.4 %, `candidate_is_legal` 8.8 %,
`generate_pseudo` 6.3 %, `static_exchange` 5.6 %, picker `next` 5.2 %,
TT probe 4.4 %, `prepare_candidates` 4.3 %.

Bench suite profile-json (`bench-t1-profile.json`): visited nodes 338.9 M of
which qnodes 333.9 M (**98.53 %**), main nodes 4.98 M, TT hits 107.75 M
(31.8 % of visited). Pruning counters per Mnodes visited: qchecks 311.3,
pvs_searches 109.5, delta_prunes 52.8, see_prunes 6.1, check_extensions 0.8,
lmr_reductions 0.2, pvs_researches 0.02. Note `qchecks` ≈ 105.5 M nodes (31 % of
qnodes) — a large fraction of the quiescence tree is check-related.
(`position_feature_extractions`, eval-cache counters are 0 in this JSON: they
are compiled-out work counters in the stock build.)

## NNUE accumulator deep dive (answers the review hypothesis)

Promoted net is v4 halfka-king-bucket (9216 inputs, h128); the make path is:
`prepare_child_slot` (copy parent accumulator → child slot, validated by parent
position key) → `apply_move_deltas` (2 perspectives × add/remove feature deltas,
plus `refresh_slot_perspective` for each perspective whose own king crosses a
bucket boundary). Failure to find a valid parent slot increments
`incremental_fallback_count_`; the next eval then does a full feature rebuild
(`nnue.cpp:1820-1882`).

- **on_make_move total**: 52.27 % inclusive (UCI T1), 52.66 % (bench), 50.85 %
  (T4). Self (inlined delta math + refresh accumulation) 39.3–45.0 %.
- **copy/slot cost**: `prepare_child_slot` 3.6–5.9 % incl, `_platform_memmove`
  2.8–4.8 % of search (per make: 2 perspectives × 128 int32 accumulator copy =
  1 KiB, plus threat-list buffers that v4 nets never use).
- **king-bucket refresh, visible part**: UCI T1 3.6 %, bench 6.7 %, T4 4.0 %
  of search; components under `on_make_move` (bench numbers): halfka index
  encode 2.10 %, extract/features 2.22 %, `memset` 0.77 %, malloc/zone free
  ≈1.6 %, refresh slot copy ≈0.1 %. Of that, **heap alloc/free + memset is
  0.7–2.3 % of search CPU on its own** — `refresh_slot_perspective` allocates
  `std::vector<std::int32_t> sums(hidden_units_)` **on every call**
  (`nnue.cpp:1961`), zeroes it, then copies into the slot.
- **fallback ratio**: cannot be measured directly with shipped binaries (the
  counters have no callers/exposure and there is no sudo for dtrace), but the
  *cost signature* says the fallback rebuild is not happening in steady search:
  the full-rebuild branch's children (`native_position_features`,
  `halfka_king_bucket_indices`, encode) appear under `on_make_move` (refresh)
  but **not** under `NnueWorker::evaluate` (its 359 top occurrences have only 3
  child samples, all `ensure_incremental_storage`). Conclusion: the
  "key-lookup + fallback refresh leak" premise is **refuted**; the real
  accumulator cost is the bucket-crossing refresh plus the per-make delta math,
  not the fallback.
- Review's "move accumulators onto SearchStack": the code already has 132
  per-slot accumulators keyed by ply + parent key and a scratch slot
  (`nnue.cpp:2135-2160`). Moving them to SearchStack would not remove the delta
  math or the refresh; it is not the missing 50 %.

## Depth / NPS / node efficiency

Mac M4, startpos, promoted h128, Hash 512, single process at a time.

| run | reported depth | last completed depth | nodes | wall/time used | NPS |
| --- | --- | --- | --- | --- | --- |
| T1 movetime 200 ms | 5 | 5 (35 ms) | 36,460 | ~full 200 ms budget | ~1.04 M |
| T1 movetime 1000 ms | 7 | 6 (415 ms) | 856,377 @915 ms | 931 ms | 936 k |
| T1 movetime 5000 ms | 7 | 6 (416 ms) | 4,417,983 @4.92 s | 4.92 s | 898 k |
| T1 go depth 8 | 8 | 8 (9.83 s) | 9,143,351 | 9.83 s | 930 k |
| T4 movetime 200 ms | 5 | 5 (42 ms) | 130,856 | ~full 200 ms | ~3.1 M* |
| T4 movetime 1000 ms | 7 | 6 (395 ms) | 1,658,415 @895 ms | 895 ms | 1.85 M |
| T4 movetime 5000 ms | 7 | 6 (386 ms) | 5,161,811 @4.89 s | 4.89 s | 1.06 M* |
| T4 go depth 8 | 8 | 8 (9.86 s) | 20,008,746 | 9.86 s | 2.03 M |
| T1 middlegame movetime 5000 | 6 | 5 (335 ms) | 3,736,027 @4.84 s | 4.84 s | 772 k |
| T1 middlegame go depth 8 (capped) | 7 | 6 (4.22 s) | 9,497,198 @11.7 s | >11.7 s | 810 k |
| T1 middlegame go infinite 40 s | 7 | 6 | 29,339,207 @39.86 s | 40 s | 736 k |
| T4 middlegame go infinite 40 s | 7 | 6 | 35,024,924 @39.72 s | 40 s | 882 k* |
| T1 1+1 clock (`wtime 1000 btime 1000`) | 4 | 4 (40 ms) | 36,205 | engine user 0.12 s total | ~0.1 s used |
| T4 1+1 clock | 4 | 4 (39 ms) | 36,205 | engine user 0.12 s total | identical to T1 |

\* T4 long-TC NPS is under-reported; see SMP section. At completed iterations the
fair T4/T1 ratios are 2.06× (1 s movetime) and 2.18× (fixed depth 8).

Per-depth node growth, startpos T1 (`go depth 8`):

| depth | nodes | time |
| --- | --- | --- |
| 4 | 13,220 | 10 ms |
| 5 | 40,322 | 37 ms |
| 6 | 368,612 | 373 ms |
| 7 | **5,603,069** | **6,079 ms** |
| 8 | 9,143,351 | 9,832 ms |

Middlegame FEN (`go depth 8`): depth 5 354,610 nodes/369 ms → depth 6
3,470,937/4.22 s → depth 7 9,497,198 at 11.7 s (not completed; infinite run
reaches 29.3 M nodes in depth 7 after 40 s).

**Node efficiency is the real limiter, not raw NPS**: 0.9–1.0 M NPS is decent
for a 1.7 k-Elo engine, but reaching depth 7 from startpos needs 5.6 M nodes
(≈15× the depth-6 cost), and reaching depth 6 on the middlegame needs 3.5 M.
`go depth 8` on the middlegame FEN does not complete in 40 s. At the 200 ms
gate budget the engine completes depth 5 with ~36 k nodes. Note the
non-monotonic per-ply cost: **depth 7 costs 15× depth 6 but depth 8 only 1.6×
depth 7** — a strong hint of a depth-7-specific search-effort anomaly (aspiration
fail-high/low loops, TT-cutoff instability at odd depth, qsearch parity or
check-extension chains). This is worth a targeted investigation: recovering even
one ply at 1–5 s could be a large Elo gain.

## SMP helper threads

- Threads=4 run: 4 search threads, each sampled 16,915 times (one sample per ms
  per live thread); wait/blocked self-samples 44/48/74/112 (0.3–0.7 %) — helpers
  are **not** idle or spinning, they are searching 99.3–99.7 % of the capture.
- 3 helpers come from `LazySmpPool` (`search_runner.cpp:1895-2094`); they each
  own position copy/ordering/evaluator worker, share only TT + time budget, and
  start at `depth = 1 + index%3`, then `depth = max(depth, published + index%3)`.
  So all 4 threads search the same root at almost the same depth with no work
  partitioning; the only saving is the shared TT.
- Node accounting: helpers publish their iteration counters only when a whole
  iteration returns (`publish_iteration_stats`, lines 2001-2011, called at 2061).
  At 5 s / infinite TC a helper is inside a very long iteration, so `info nps`
  omits most helper work — that is why the T4 `info nps` at 5 s/infinite reads
  only 1.06–1.18× T1. Honest scaling is measured at completed iterations:
  1.98× (1 s movetime: 1.85 M vs 0.94 M NPS) and 2.18× (fixed depth 8:
  2.03 M vs 0.93 M), with 1.9–2.2× the nodes for the same
  depth — i.e. 4 threads ≈ 2× throughput but also ≈ 2× duplicate work, so
  **no deeper ply in the same fixed time** at 200 ms–5 s in these tests.

## Top 10 optimization candidates

Estimated NPS gain = naive upper bound from removing a share of CPU; effort
S/M/L. "Behavioral" = changes search/eval results → needs equal-time SPRT
(pinned NNUE, 200 ms screen then 1+0 confirmation); "safe perf" = bit-identical
results verifiable with fixed-depth parity. Cross-check cites AGENTS.md §9.

| # | candidate | evidence from this profile | effort | est. NPS gain | changes behavior? | cross-check §9 / new angle |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | **NEON (arm64) kernel for `apply_feature_delta`** (and optionally the v4 forward pass) — mirror the x86 AVX2 `apply_delta_avx2` semantics on Apple Silicon | `on_make_move` self 43–45 %; arm64 build compiles out AVX2 so deltas use the scalar int64-clamp loop; per make ~2 perspectives × 2–6 features × 128 lanes | M | **+8–18 %** | safe (bit-identical achievable; verify fixed-depth parity) | New angle. §9 refresh-AVX2 gave +10.6 % NPS but was gate-neutral at 200 ms → treat as NPS-only; measure 1+0, bundle with other NPS wins or accept neutral |
| 2 | **Remove heap allocation + memset in `refresh_slot_perspective`** — reuse a preallocated scratch buffer (or write deltas into the slot in place after zeroing it) | refresh visible 3.6–6.7 % of search; its malloc/free/memset children 0.7–2.3 %; `nnue.cpp:1961` allocates `std::vector<int32_t>` per refresh | S | +1–2.5 % | safe | **Landed while this report was being written** as `d49ce0f` "perf(nnue): reuse refresh sums buffer in the hot path" + verification `artifacts/profiles/lead-perf/REFRESH_ALLOC.md` (Windows noisy +1.1 % NPS, within noise; behavior-preserving). Quiet-Mac NPS comparison still pending; the Mac binary profiled here predates the fix. Profile-based expectation from the 0.7–2.3 % child share is ≈+1–2 % NPS, not an Elo claim |
| 3 | **Skip v4-unused threat buffers in slot copies** — `copy_slot_forward` copies `slot_threats_`/counts for both perspectives even when `uses_halfka_threat_features()` is false (promoted net is v4) | `_platform_memmove` 2.8–4.8 %; copy path 3.6–5.9 % incl | S | +0.5–1.5 % | safe | New; §9 mentions threat features only for v5 nets |
| 4 | **Quiescence width reduction** — target the 98.5 %-of-nodes qsearch: qchecks are 31 % of qnodes; delta prunes only 5.3 % of qnodes; SEE prunes 0.6 %. Try (a) cap/disable checks-in-qsearch expansion, (b) more aggressive qsearch delta/futility (tested once at margin 200), (c) TT probe in qsearch before generating, (d) SEE-gate quiet check evasions | qsearch 79–88 % of search samples; bench 333.9 M qnodes for depth 5; TT hits 31.8 % of visited | M | +10–30 % (or +0.5–1 ply at same time) | **behavioral** → SPRT | §9: delta margin 100→200 neutral; lazy-SEE qsearch +4.2 % NPS but reverted (broke 8 search tests + ordering differential). New angle: attack the *check* share (qchecks 105 M) and evasion generation, not SEE ranking |
| 5 | **Fuse legality into generation** — compute pin/checker masks once per node and emit only legal moves; avoid `candidate_is_legal` per pseudo move and recomputing `native_move_gives_check` in metadata | candidate_is_legal 9.2 % incl, native_move_gives_check 4.3 %, fill metadata 2.7 %, generate 5.0 %, apply_generated 4.8 % | M–H | +5–10 % | safe if exact (ordering unchanged) — verify fixed-depth parity + ordering differential tests | New; §9 has no legality-fusion row |
| 6 | **Move ordering + SEE cost** — picker next 8.9 % incl + prepare_candidates 6.1 % + SEE 3.6 %; eager SEE resolution is load-bearing | ordering+SEE 8.2–8.3 % | M | +2–5 % | **behavioral** (ordering changes) | §9: lazy-SEE ranking reverted; quiet-SEE pruning negative. Only pursue with a new equivalent-quality design + SPRT |
| 7 | **TT probe cost** — 4.5 % (T1 exclusive path) → 7.0 % incl (T4 locked path); 24-byte entries; add prefetch and consider splitting data/aux or relaxed load path for helpers | TT bucket 4.8 % T1 / 6.2 % T4; probe_impl<true> self 5.94 % at T4 | M | +1–4 % | safe (layout change must preserve exactness) T4 gains most | New; §9 has no TT-probe optimization; TT correctness contracts apply |
| 8 | **SMP: honest helper node counters + diversification + gate Threads=4** — publish in-flight iteration nodes (or sample counters periodically); give helpers distinct depth offsets/ordering jitter instead of `index%3`; evaluate whether Threads=4 is worth shipping for the 200 ms bot config | helpers 99.3–99.7 % busy but 2.2× nodes for same depth; T4 200 ms = same depth 5 as T1 with 3.6× nodes | M | T4 throughput +0–15 %; no T1 change | (counter fix: none; diversification: behavioral → SPRT Threads=4) | New angle on the catalog's "SMP diversification (gate Threads=4)" idea; §9 has no SMP profiling row |
| 9 | **Investigate the depth-7 iteration blow-up** — 15× depth-6 cost at startpos (5.60 M nodes/5.7 s) vs 1.6× for depth 8; middlegame depth 7 > 40 s. Check aspiration fail-high/low loops, TT cutoff instability at odd depths, qsearch parity, check/singular extension chains; add per-depth node/time telemetry to koi-bench | per-depth table above | M | potentially +1 ply at 1–5 s (large Elo; not an NPS claim) | behavioral → SPRT | New; §9 has aspiration-25 neutral and cheap search-margin rejects, but no per-depth effort anomaly |
| 10 | **NEON-vectorize the NNUE forward pass** (`finish_inference_v4_scalar`, currently the only path on arm64) | `NnueEvaluatorWorker::evaluate` self 3.1 % T1 / 1.8 % T4; refresh accumulation inlined in on_make self | S–M | +1–3 % | safe (bit-identical int arithmetic) | New; complements #1; §9 width sweeps fixed h128 so net size is not a lever |

Bonus (diagnostic, no perf claim): expose `incremental_make_count()` /
`incremental_fallback_count()` and make/qnode counters through
`koi-bench --profile-json` so the next profile can report the exact
refresh/fallback ratio (currently S effort, would close the only open question).

## Caveats

1. **Platform**: Apple M4 arm64, AppleClang `-O3`; the build has no
   `KOI_NNUE_COMPILED_AVX2` path (x86-only), so NNUE delta math is scalar. The
   2026-10-27 x86 rdtsc profile (make_observed 42 % wall, picker next 26.5 %)
   is directionally consistent for make_observed but **not** for ordering share;
   do not transplant x86 percentages to arm64 or vice versa.
2. **`sample` self%** is a CPU-time estimate from 1 ms sampling; inclusive-once
   counts a symbol once per stack. `sample` can attribute inlined callees to the
   nearest symbol (e.g. `refresh_slot_perspective`/`apply_feature_delta` are
   inlined into `on_make_move`), which is why on_make self is so large and the
   refresh cost is only partly separable.
3. **Bench workload**: depth 5 with cold hash per position (bench clears TT
   between positions) — 38 % of the suite's positions are endgame/tactical with
   very high qnode shares; it is a synthetic workload, not a game distribution.
   The middle-game UCI infinite run is the game-like reference.
4. **Depth reporting**: `info depth N` lines include the in-progress iteration;
   "last completed depth" is derived from the last published line of N-1. At
   `movetime` stops the final iteration is partial. The 200 ms run's stop time is
   inferred from the full-movetime policy (~190–200 ms) plus engine user time
   (0.24 s incl. startup); the last published info is at 35 ms.
5. **T4 long-TC NPS is under-reported** because helper counters publish per
   completed iteration (see SMP). Do not quote 1.06–1.18× as SMP scaling.
6. **Windows** was not used for symbols: the MSVC Release binary has no PDBs and
   WPT (`wpr`/`wpa`/`xperf` exist) cannot decode symbols headlessly there without
   instrumented builds; the Lead's Windows NPS (244 k T1) is host/variant
   specific and is not comparable to the Mac numbers.
7. `mach_absolute_time` samples (0.05–0.74 %) under on_make_move are kept in
   "other"; likely sampling/time-manager attribution noise, not NNUE.
8. The profiled binary is the 1.1.5 hot path (see provenance); hot-path files
   are byte-identical to HEAD, but the tree has uncommitted Syzygy/UCI changes
   that do not affect search.

## Artifacts (this directory)

| file | content |
| --- | --- |
| `SUMMARY.md` | this report |
| `analyze_sample.py` | call-tree parser (self/inclusive/qsearch/probes) |
| `phaseA-mac.sh`, `phaseB-mac.sh`, `fens-small.txt` | workload scripts and probe FENs |
| `mac/samples/uci-t1-infinite.sample.trim.txt` | trimmed raw `sample` call tree + full "Sort by top of stack" section, T1 (25 s) |
| `mac/samples/uci-t4-infinite.sample.trim.txt` | same, T4 |
| `mac/samples/bench-t1.sample.trim.txt` | same, koi-bench |
| `mac/samples/*.analysis.json` | parsed tables: threads, self/incl, qsearch subtree, probes, per-thread |
| `mac/bench-t1-profile.json` | `koi-bench --profile-json` output: per-position nodes/qnodes/tt_hits/pruning counters |
| `mac/logs/*.log` | exact UCI/bench logs for every measurement (depth/NPS/wall) |
| `mac/logs/*.time` | `/usr/bin/time -p` wall/user for budget runs |
| `mac/t*.in` | exact UCI input scripts used |

Full untrimmed sample files (2.9/6.8/24 MB) were captured on the Mac, parsed to
the JSON above, then deleted from the Mac temp dir after copying the trimmed
text; they can be regenerated with `phaseB-mac.sh`. The raw full call trees are
not kept in the repo (artifact size policy ≤256 KiB/file).
