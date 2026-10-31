# CCRL eligibility plan — Koi (2026-11-01)

Goal: make Koi ready for a CCRL submission/testing request. CCRL's current
request (TalkChess "How to get your engine tested for CCRL", Graham Banks) asks
for: engine name + version, estimated strength, author name and nationality,
whether it is open source, a homepage if any, credit where due if closed source,
and whether it was coded by the author or AI assisted; then exe files
(AVX2 preferred) and optionally default Hash 256 MB / 1 thread.

## Requirement → status

| Requirement | Koi status | Evidence |
|---|---|---|
| UCI protocol | Implemented; only `UciController` writes the protocol; exactly-once legal `bestmove`; `isready` during search; `stop`/`quit`/`ponderhit`/EOF covered | reliability fuzz/soak suite, 100k-round soak, 1000-game game harness |
| Internal opening book must be disable-able | `OwnBook=false` disables the book, including an already auto-detected one; no learning features | process test `book hot-add + OwnBook=false`; 1000-game harness ran with `OwnBook=false` |
| No forced proprietary book | Auto-detection looks only for `book.bin` beside the executable; release packages place the book at `book/koi.bin`, so it is not auto-used | `src/koi/opening_book.cpp` default `book.bin`; packaging script places `book/koi.bin` |
| Learning features off | No book learning, no persistent/position learning implemented | code audit |
| 64-bit executable, AVX2 | Windows/Linux x86-64 selector + AVX2 + AVX-512 builds; macOS arm64 build | CMake targets/archives |
| Threads 1 / ponder off | `Threads` default 1; `Ponder` default false; CCRL profile test sets both explicitly | handshake + fuzz `ccrl-profile` |
| Hash 128/256 MB | Tester can set it; CCRL profile uses 256 MB (default is 512 MB) | `ccrl-profile` test; 1000-game harness used 256 MB |
| Neutral openings, paired colours | Test harness uses neutral curated openings and alternates Koi's colour | `crash_1000.py` |
| 1000 ultra-fast games without a crash | **PASS: 1000/1000 games, 0 crashes, 0 timeouts, 0 illegal moves, 0 protocol violations** (Mac arm64, Koi vs Stockfish 19, 5 ms/move, 6 workers, 90.2 s) | `EVIDENCE.md`, `mac/1000-games.json/jsonl` |
| No copied code | Audit: Koi's engine sources are its own implementation (MIT); third-party libraries are vendored dependencies with licenses; the Stockfish 19 binary is a GPL-3.0 test opponent only — not linked into the engine, not in release packages; engine binary contains no "stockfish" string | `PROVENANCE.md` |
| Submission metadata | Prepared template; author name/nationality must be filled by the author | `SUBMISSION.md` |

## Remaining before an actual submission

1. Fill author/nationality in `SUBMISSION.md`.
2. Decide whether to submit the released 1.1.5 binaries or a newer build that
   includes the post-release reliability fixes (SIGPIPE, Syzygy path-list ASan
   fix, book hot-swap, etc.). A new version requires an explicit "version bump"
   from the user; this repository does not bump versions autonomously.
3. Produce the exe package (engine binaries + NNUE net + licenses; **no book**,
   no test suite, no Stockfish) and hash it.
4. Send the TalkChess PM/email to the CCRL tester with the template details and
   a download link.

## CCRL test settings used here

```
setoption name Hash value 256
setoption name Threads value 1
setoption name OwnBook value false
setoption name Ponder value false
setoption name MultiPV value 1
setoption name SyzygyPath value        (empty = no tablebases)
```

Engine: 64-bit AVX2 (`koi-engine-avx2.exe`) or the selector `koi-engine.exe`;
net auto-discovered from `nnue/koi.nnue` beside the binary.
