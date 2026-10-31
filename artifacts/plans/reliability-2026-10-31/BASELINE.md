# Reliability baseline — 2026-10-31

Binary: Koi 1.1.5 release build `build/full-release-gpu/koi-engine.exe`
(`info string build 1e09504e6569`), promoted h128 exposed via `KOI_NNUE_PATH`.

## Adversarial UCI fuzz pass (first run)

- 140 cases: 20 scripted edge cases + 120 randomized command sequences.
- Inputs included: empty/whitespace commands, unknown/duplicated commands,
  malformed `go` (negative/abc/missing/huge depth, movetime 0/-5/abc,
  nodes 999999999999999999999), malformed `position fen` and illegal moves,
  `setoption` with missing names/values and extremes (Hash -1/999999999,
  Threads 0/9999, MultiPV 0/999, Skill -5), bad EvalFile path, 100 KB lines,
  5,000-move position line, NUL/binary junk, double quit, `quit` then `stop`,
  EOF during `go infinite`.
- Results: **0 crashes, 0 hangs, 0 unhandled exceptions**; every process exited
  with the documented codes (0 clean/EOF, 3 unsupported CPU variant where
  applicable, 4 no evaluator without a net). No stderr noise observed on normal
  exits.
- Caveat: exit-code-only oracle. It did not yet assert exactly-one-legal
  `bestmove`, `isready` responsiveness during search, or stdout purity — those
  become W2 contract assertions in the harness.

## Static audit notes (initial)

- `UciController::run` wraps every command in try/catch and degrades to
  `info string command failed: ...` while continuing to serve (good).
- Numeric option parses use `std::from_chars` with end-pointer checks (safe).
- `position`/`go` paths validate FEN/move/option shapes and report
  `info string` errors instead of throwing.
- Candidate hardening: `std::getline` has no line-length cap, so a malicious
  GUI could force a multi-GB allocation. A bounded line reader (e.g. 1 MiB) with
  a protocol-valid diagnostic is cheap insurance.
- Candidate hardening: extreme `Hash`/`Threads` values are clamped somewhere;
  the exact contract (which value is reported back, allocation failure path,
  time to apply) deserves a pinned test.

## Existing assets to build on

- `tests/integration/uci/uci_process_test.ps1`, `time_safety_test.ps1`,
  `perft_responsiveness_test.ps1`, `en_croissant_uci_test.ps1`,
  `tests/integration/runtime/hash_memory_stability_test.ps1`,
  `tools/stability/hash_memory_soak.ps1`, `tests/unit/runtime/koi_soak_tests.cpp`.
- CI already runs Windows Debug+ASan, Linux TSan, shadow diff and the full
  process/match suites.
