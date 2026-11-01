# Koi reliability program — final status / handoff

Date: 2026-11-01 (session round 90). Goal active until the user says "Stop".
No version bump, tag, or release has been made for this work.

## What was hardened (source changes)

| Area | Change |
|---|---|
| Protocol input | 1 MiB bounded UCI lines; raw CRLF/NUL/binary-safe reads |
| POSIX stdio | `SIGPIPE` ignored, so a GUI crash closing stdout cannot kill the engine |
| Syzygy path lists | Empty components dropped before Fathom (ASan heap-buffer-overflow fix) |
| Syzygy validation | Single dirs and every path-list component are magic-validated; `missing/empty/usable/corrupt/unreadable` classification with truthful diagnostics |
| Syzygy truncation | Canonical-size manifest for the standard 290-file 3-4-5 man set; truncated/oversized files rejected before Fathom (fixes an 80-byte `KQvK.rtbw` hang) |
| Syzygy zero payload | Canonically sized but zero-filled WDL/DTZ bodies rejected before Fathom (reproduced canonical-size hang) |
| Book detection | Detection announcement requires the file to be readable; unreadable books are silent |
| Book content | Linked books, malformed Polyglot moves, huge/same-key/many-key books, illegal entries all fail closed to search |
| Book state | `OwnBook` auto/false/true and `ucinewgame` semantics pinned |
| NNUE `EvalFile` | Non-regular paths (FIFO/named pipe, directory, device) rejected by type before opening, preventing blocking opens |
| Debug log | Existing non-regular `DebugFile` ignored by type; cannot stall the UCI loop |
| Analysis output | `currmove … currmovenumber` emitted on interim lines (R26) |
| NNUE perf | Reused refresh sums buffer + skipped v4-unused threat copies (+1.07 % NPS on M4, bit-identical depth lines) |

## Verification assets

- Deterministic UCI corpus (`koi_protocol_fuzz_process`, 33+ sections, label `reliability`).
- Resource soak (`koi_protocol_soak_process`) with Threads/Hash and SyzygyPath/BookFile churn.
- Option-churn and broken-pipe/slow-consumer process tests.
- Five dependency-free randomized fuzzers (FEN, move lists, setoption, go/state, raw commands), registered in CTest/CI.
- Boot fail-closed NNUE matrix (missing/directory/corrupt/unreadable → exit 4, no protocol output).
- Permission/unreadable matrices for book, Syzygy and network (ACL/chmod).
- Linux-only OS memory-limit probe (`ulimit -v`; skips when the failure path is not observable).
- Unit additions: Syzygy status/manifest/payload classification, controller hash allocation failure, link/asset cases.
- Fixtures: `tests/data/reliability/{valid-fens,valid-move-lists}.txt` with provenance README and hashes.

## Evidence highlights

- 1000 ultra-fast Koi-vs-Stockfish games on Mac arm64: 0 crashes/timeouts/illegal moves/protocol violations (997 checkmate, 3 fivefold, 90.2 s).
- 200,000-round asset-churn soaks: two passes (200,001 bestmoves; 2,034.5 s and 2,188.7 s; RSS 28–83 MB).
- 1,000,000-round asset-churn soak: in progress (>630k rounds at last check, bounded RSS).
- Full ASan reliability label: **8/8, 919.1 s wall, zero sanitizer reports** (latest cases included).
- Local non-heavy suite: 71 tests, 68/71 (only 3 known local Python measurement failures); reliability label 8/8 in 140 s.
- Real Syzygy set: 290/290 tables pass the zero-payload guard (no false rejects).
- CI: multiple fully green matrices, including `a7fb7c9`, `4a0705b` (Windows release/debug/sanitizer/shadow/package/flake, Linux gcc/clang/**TSan**/modules-off/tarball/flake, macOS, docs).

## Residual risks (honest)

1. **OS-level allocation failure** end-to-end is only partially proven: the controller path is covered by an injected-policy test; the Linux `ulimit` probe skips when the failure is not observable.
2. **Crafted Syzygy files** of exactly canonical size with **non-zero garbage** headers can still reach Fathom and hang a probe (third-party trust boundary); unknown 6-man names are not size-validated.
3. **Directory junctions/UNC paths** are not exercised (file hardlinks/symlinks and permission denial are covered).
4. **Overnight (8 h+) soak** not yet done; the 1M-round (~3 h) run is the current longest.
5. **Platform signals** (SIGTERM/SIGHUP, console-close) are not part of the UCI contract.
6. **Local Python measurement tests** fail in this environment for unrelated test-data/dependency reasons; CI is the authority.

## Rerun

```powershell
ctest --test-dir build/full-release-gpu -C Release -L reliability --output-on-failure
ctest --test-dir build/full-release-gpu -C Release -LE heavy -j 4 --output-on-failure
# ASan (add the VS ASan runtime dir to PATH):
ctest --test-dir build/asan-reliability -C Debug -L reliability -LE heavy --output-on-failure
# long soak:
pwsh -File tests/integration/uci/protocol_soak_test.ps1 -EnginePath <exe> -Rounds 1000000
```

## Release state

- Release tag `1.1.5` remains at `1e09504`; all reliability work is post-release
  and unpushed into any release. A version bump/release requires the user's
  explicit "version bump".
- CCRL readiness package/submission remain ready; see
  `artifacts/plans/ccrl-eligibility-2026-11-01/`.
