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
| FEN fuzzer | `artifacts/.../fen_fuzz.py` | 2000 release + 500 ASan cases: 0 failures; also registered in CTest as `koi_fen_fuzz_process` |
| Move-list fuzzer | `artifacts/.../move_fuzz.py` | registered as `koi_move_fuzz_process` |
| setoption fuzzer | `artifacts/.../option_fuzz.py` | registered as `koi_option_fuzz_process` |
| go/state fuzzer | `artifacts/.../go_fuzz.py` | registered as `koi_go_fuzz_process` |
| CCRL game harness | `artifacts/plans/ccrl-eligibility-2026-11-01/crash_1000.py` | 1000 games vs SF19, Mac: 0 crashes/illegal/protocol |
| Permission matrix | fuzz sections 9o/9p/9q | unreadable book/Syzygy/EvalFile (Windows ACL, Unix chmod) |
| Boot fail-closed matrix | fuzz section 31 | missing/directory/corrupt/unreadable `KOI_NNUE_PATH`: exit 4, no protocol output |
| Raw-command fuzzer | `command_fuzz.py` | random/unknown command lines; registered as `koi_command_fuzz_process` |
| OS memory-limit test | `protocol_memory_limit_test.ps1` | Linux-only `ulimit -v` forced Hash allocation failure |
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
- With a workspace `TEMP` override (removing the sandbox artifact), the local
  release suite is 63/66 (only 3 Python measurement tests fail in their own
  asserts) and the ASan unit label is 24/24 with zero sanitizer reports.
- The standard 3-4-5 man Syzygy size gate caught a reproduced 80-byte `KQvK.rtbw`
  search hang and the macOS fixture regression it exposed (fixtures now canonical).
- Boot fail-closed matrix: missing/directory/corrupt/unreadable `KOI_NNUE_PATH`
  exits 4 with `no usable NNUE network` and no protocol output (process-spawned).
- Malformed Polyglot move encodings (from == to, invalid promotion bits) are
  never used; legal fallback.
- Randomized fuzzers are dependency-free (committed fixtures), registered in
  CTest/CI, and run on Windows/Linux/macOS; Windows CI also runs the protocol
  corpus under AddressSanitizer.
- A fresh 200,000-round asset-churn soak was started on the fully hardened
  engine (`soak-200k-latest.log`); the earlier 200k run already passed.
- Linked book assets: hardlink (Windows) and symlink (Unix) resolve for
  detection/use; a broken Unix symlink falls back silently with a legal search.
- Crafted Polyglot entry with an illegal move for the keyed position is never
  returned as `book move`; the engine falls back to a legal search answer.
- Book state machine: linked book files (hardlink/symlink, broken symlink),
  16 MiB all-same-key book, 65,536-distinct-key book, malformed move encodings,
  `OwnBook` auto/false/true toggling, and book use across `ucinewgame` are pinned.
- Syzygy fail-closed hardening: canonical-size manifest for the standard
  3-4-5 man set, zero-filled body rejection (WDL and DTZ), mixed good/corrupt
  directories rejected as a whole, and the real 290-file set passes the guard
  (no false rejects). ASan covers the zero-body and mixed-directory cases.
- CCRL package built (engines + NNUE + licenses, no book/tests/Stockfish);
  provenance audit in `artifacts/plans/ccrl-eligibility-2026-11-01/`.

## 4. Residual risks / not yet covered

1. **OS-level allocation-failure injection** remains untested: the controller
   allocation-failure path is now covered by an injected policy (R60), but no
   process-wide memory-limit/job-object run exists. Large real allocations were
   deliberately not requested to avoid destabilising the host.
2. **Crafted same-size Syzygy payloads**: the standard 3-4-5 man set is
   canonical-size-validated (R65) and zero-filled bodies are rejected before
   Fathom maps them (R93/R95). A crafted file of exactly the expected size with
   **non-zero garbage** header bytes can still reach Fathom and hang a probe
   (third-party trust boundary); unknown 6-man names are not size-validated.
   Auto-discovery remains silent for rejected directories.
3. **Directory junctions/UNC paths** remain untested; file hardlinks/symlinks
   (R62), read-permission denial for book/Syzygy/EvalFile (R57/R58/R59) and the
   transient-lock variant (R41) are covered.
4. **Multi-hour soak**: a 200,000-round asset-churn soak completed on
   2026-11-01 (200,001 bestmoves, 29,922 scored, 2,034.5 s ≈ 34 min, RSS 29-83 MB,
   exit 0), and a second 200k run on the post-asset-hardening engine also
   passed (200,001 bestmoves, 29,933 scored, 2,188.7 s, RSS 28-83 MB). An
   overnight (8 h+) run remains undone.
5. **Platform signals** (SIGTERM/SIGHUP on POSIX, console-close on Windows) are
   not part of the UCI contract; engine defaults apply.
6. **CI backlog**: many pushes queued Windows/Linux CTests; latest green matrix
   may lag HEAD by a few commits. macOS runs are occasionally cancelled by the
   workflow concurrency policy.

## 4b. Program scale (as of round 87)

- **R1–R101** tracked risks: fixed or verified; every fix has a regression.
- Local non-heavy suite: **71 tests**, **68/71** pass (3 known local Python
  measurement failures).
- Reliability label: **8 tests**, ~140 s release / **919 s ASan**, zero reports.
- Randomized condition-free fuzzers: **five** (FEN, move lists, setoption,
  go/state, raw commands), registered in CTest/CI.
- Long runs: two 200k asset-churn soaks; **1,000,000-round soak in progress**.
- CI: Windows full/debug/sanitizer/shadow/package/flake, Linux
  gcc/clang/tsan/modules-off/tarball/flake, macOS, docs; latest fully green
  heads tracked in the goal reports.

## 5. How to rerun

```powershell
ctest --test-dir build/full-release-gpu -C Release -L reliability --output-on-failure
ctest --test-dir build/full-release-gpu -C Release -R koi_protocol_ --repeat until-fail:3
# ASan (MSVC): add the VS ASan runtime dir to PATH, then:
ctest --test-dir build/asan-reliability -C Debug -R koi_protocol_fuzz_process --output-on-failure
# long soak:
pwsh -File tests/integration/uci/protocol_soak_test.ps1 -EnginePath <exe> -Rounds 30000
# fuzzers (fixture-driven, no third-party runtime dependency):
python artifacts/plans/reliability-2026-10-31/fen_fuzz.py --engine <exe> --cases 2000 --out out.json
```

## 6. Current tree / CI

- HEAD at the time of writing: reported in the commit that adds this file.
- Release tag remains `1.1.5` at `1e09504`; post-release reliability work is
  **not** released and no version bump has been requested.
- Worktree is kept clean with signed local commits; pushes are made as work
  completes. `edbbde1` was the latest head with **all four CI matrices green**
  (Windows, Linux, macOS, docs); later heads are being checked as they finish.
- The expanded reliability label (8 CTest entries) passed locally under
  AddressSanitizer: 8/8, 715.8 s wall, zero sanitizer reports.
- A 1,000,000-round asset-churn soak (≈3 h) is running against the hardened
  engine (`soak-1m-assetchurn.log`); two 200k runs already passed.
- The Windows Debug helper now scopes a 120 s UCI wait to the 16 MiB book case
  and reserves four processors for the fuzz entries, so slow Debug runs do not
  time out or disturb match tests.
