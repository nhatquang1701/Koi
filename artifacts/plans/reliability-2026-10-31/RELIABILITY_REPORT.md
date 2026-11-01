# Koi reliability program — consolidated report (2026-10-31 .. 2026-11-01)

Status: **active** (until the user says "Stop"). No version bump, tag, or release
has been made for this work. Engine source changes in this program:
1 MiB bounded protocol lines; Fathom Syzygy path-list sanitization + per-component
magic validation (ASan heap-buffer-overflow fix); SIGPIPE ignore on POSIX; NNUE
refresh/threat-copy micro-optimizations; `currmovenumber` emission; empty-dir and
unreadable Syzygy classification; truthful unreadable-book detection; book/
analysis/eligibility behavior pinned. Everything else is test/evidence/tooling.

## 1. Contracts hardened

- **Single protocol writer**: `UciController` is the only stdout writer; all
  diagnostics are `info string`/stderr.
- **Exactly-once legal bestmove**: completion gate/generation suppression;
  verified across go/stop/ponderhit/quit/EOF/position changes and option races.
- **Lifecycle**: `isready` during search, stop storms, immediate ponder stop,
  duplicate `stop`/`ponderhit`, idle `ponderhit`, quit-during-ponder,
  ucinewgame-during-ponder, stop-before-go, EOF at every phase.
- **Strict options**: exhaustive invalid-value matrix over all 28 advertised
  options; malformed syntax; quoted/Unicode/control-byte/NUL values; huge
  `searchmoves`; contradictory `go` limits; weird clock/increment values.
- **Fail-closed assets**: book size/multiple/16 MiB caps; transient lock (no
  negative cache); corrupt/truncated/removed/hot-added book; analysis/MultiPV/
  infinite/ponder/searchmoves book eligibility; **unreadable** book (ACL/chmod)
  is not announced as detected; corrupt **and unreadable** Syzygy dirs/path-list
  components are classified and reported distinctly; invalid or unreadable
  EvalFile forms (missing, empty, directory, random bytes, permission-denied);
  bad DebugFile paths/directories. Permission-denied regressions exist for all
  three assets (book, Syzygy, network).
- **Raw stdio**: 1 MiB line bound, binary/NUL stdin, CRLF, tab separators,
  closed stdout/stderr (GUI crash), slow-consumer backpressure, PID-level
  process tests.

## 2. Verification assets

| Asset | Location | Notes |
|---|---|---|
| Deterministic UCI corpus | `tests/integration/uci/protocol_fuzz_test.ps1` | 33 sections; label `reliability`; ~25 s |
| Resource soak | `protocol_soak_test.ps1` | Threads/Hash + SyzygyPath/BookFile churn; 60 rounds default; `-Rounds` for long runs |
| Option churn | `protocol_option_churn_test.ps1` | Hash/Threads/MultiPV/EvalFile/BookFile/Syzygy during active search |
| Broken pipe / stdio | `protocol_broken_pipe_test.ps1` | mid-search and pre-handshake disconnects, stderr close, slow consumer |
| FEN fuzzer | `artifacts/.../fen_fuzz.py` | 2000 release + 500 ASan cases: 0 failures |
| Move-list fuzzer | `move_fuzz.py` | 2000 + 500 cases, max 1493 tokens: 0 failures |
| setoption fuzzer | `option_fuzz.py` | 1000 + 300 cases: 0 failures |
| go/state fuzzer | `go_fuzz.py` | 1000 + 300 cases: 0 failures |
| CCRL game harness | `artifacts/plans/ccrl-eligibility-2026-11-01/crash_1000.py` | 1000 games vs SF19, Mac: 0 crashes/illegal/protocol |
| Permission matrix | fuzz sections 9o/9p/9q | unreadable book/Syzygy/EvalFile (Windows ACL, Unix chmod) |
| ASan tree | `build/asan-reliability` | reliability legs + 20/24 unit suites clean (4 local `%TEMP%` quirks) |
| TSan | Linux CI `linux-tsan` job | 64 TT mutexes require `detect_deadlocks=0` |

## 3. Evidence highlights

- Deterministic corpus: release ~25 s; ASan up to ~190 s after additions; no
  sanitizer reports.
- Long soaks: 10,000 rounds (119 s), 30,000 rounds (350.8 s with asset churn),
  100,000 rounds (1,329 s) — all bounded RSS (30–82 MB), one bestmove per search,
  clean exit.
- Fuzzers: 4,500 release cases + 1,600 ASan cases across FEN/move/option/go;
  zero crashes, timeouts, unexpected stderr or malformed output.
- 1000-game ultra-fast Koi-vs-Stockfish run on Mac arm64: 0 crashes/timeouts/
  illegal moves/protocol violations (997 checkmate, 3 fivefold, 90.2 s).
- Read-permission matrix: unreadable book is not announced; unreadable Syzygy
  table reports `unreadable tables` (not corrupt) and disables probing; unreadable
  EvalFile is rejected once with the previous evaluator preserved — each followed
  by a legal search. All pass release + ASan fuzz legs.
- Controller-level Hash allocation failure: policy failing every allocation
  yields `reason allocation_failed`, `readyok`, one legal bestmove, exit 0
  (release + ASan filtered runs).
- Linked book assets: hardlink (Windows) and symlink (Unix) resolve for
  detection/use; a broken Unix symlink falls back silently with a legal search.
- Crafted Polyglot entry with an illegal move for the keyed position is never
  returned as `book move`; the engine falls back to a legal search answer.
- CCRL package built (engines + NNUE + licenses, no book/tests/Stockfish);
  provenance audit in `artifacts/plans/ccrl-eligibility-2026-11-01/`.

## 4. Residual risks / not yet covered

1. **OS-level allocation-failure injection** remains untested: the controller
   allocation-failure path is now covered by an injected policy (R60), but no
   process-wide memory-limit/job-object run exists. Large real allocations were
   deliberately not requested to avoid destabilising the host.
2. **Symlinked assets** were not exercised; read-permission denial for book,
   Syzygy and EvalFile is now covered (R57/R58/R59), and the transient-lock
   variant is covered (R41).
3. **Multi-hour soak**: a 200,000-round asset-churn soak completed on
   2026-11-01 (200,001 bestmoves, 29,922 scored, 2,034.5 s ≈ 34 min, RSS 29-83 MB,
   exit 0), after the earlier 100k runs. An overnight (8 h+) run remains undone.
4. **Platform signals** (SIGTERM/SIGHUP on POSIX, console-close on Windows) are
   not part of the UCI contract; engine defaults apply.
5. **CI backlog**: many pushes queued Windows/Linux CTests; latest green matrix
   may lag HEAD by a few commits. macOS runs are occasionally cancelled by the
   workflow concurrency policy.

## 5. How to rerun

```powershell
ctest --test-dir build/full-release-gpu -C Release -L reliability --output-on-failure
ctest --test-dir build/full-release-gpu -C Release -R koi_protocol_ --repeat until-fail:3
# ASan (MSVC): add the VS ASan runtime dir to PATH, then:
ctest --test-dir build/asan-reliability -C Debug -R koi_protocol_fuzz_process --output-on-failure
# long soak:
pwsh -File tests/integration/uci/protocol_soak_test.ps1 -EnginePath <exe> -Rounds 30000
# fuzzers (python-chess required):
python artifacts/plans/reliability-2026-10-31/fen_fuzz.py --engine <exe> --cases 2000 --out out.json
```

## 6. Current tree / CI

- HEAD at the time of writing: reported in the commit that adds this file.
- Release tag remains `1.1.5` at `1e09504`; post-release reliability work is
  **not** released and no version bump has been requested.
- Worktree is kept clean with signed local commits; pushes are made as work
  completes. Latest fully verified green CI head is reported in the goal report.
