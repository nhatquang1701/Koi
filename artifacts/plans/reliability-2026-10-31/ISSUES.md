# Reliability issue triage

Status: `new` -> `reproduced` -> `fixed` (commit + regression test) -> `verified`.

| # | Area | Symptom / risk | Repro | Status | Evidence |
| --- | --- | --- | --- | --- | --- |
| R1 | controller input | unbounded `std::getline` line length: a hostile GUI can force an arbitrarily large allocation before any validation | 2 MiB line + `isready`/`uci`/`quit`; 1 MiB+1 line followed by a normal search | **fixed** — `read_bounded_protocol_line`, 1 MiB cap, protocol-valid rejection; test `bounded protocol line`; `koi_engine_process` green | `uci_controller.cpp`, `uci_controller_tests.cpp` |
| R2 | contract harness | fuzz/contract coverage exists only ad hoc; no deterministic corpus or fast CI label pins exactly-one-legal-bestmove, isready-during-search, stdout purity, lifecycle storms | `ctest -R koi_protocol_fuzz_process` (7 s, every CI leg) | **fixed** — `protocol_fuzz_test.ps1`, seeded corpus, `reliability` label, CMake + tests/README/ENGINE_SPEC wiring | `tests/integration/uci/protocol_fuzz_test.ps1` |
| R3 | options | extreme `Hash`/`Threads`/`MultiPV` values: clamping contract, reported value, allocation-failure path and apply latency are not pinned by tests | scripted fuzz accepted values; no assertions | new | `uci_controller.cpp` option paths |
| R4 | lifecycle | EOF/`quit` during search: exit code observed clean, but exactly-once legal `bestmove` and silence-after-stop are not asserted adversarially (randomized race coverage missing) | fuzz case `eof-during-search` | new | `BASELINE.md` |
| R5 | sanitizers | no fresh ASan/TSan/UBSan run recorded for the 1.1.5 code | n/a | new | CI legs exist; local evidence pending |
| R6 | assets | repeated `setoption EvalFile/BookFile/SyzygyPath` churn and malformed files are covered piecemeal; a single fail-closed matrix is missing | n/a | new | asset loader tests |
