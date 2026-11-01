# Koi architecture audit — 1.0.0 baseline (2026-11-01)

Scope: inspect the current architecture of Koi and make conservative structural
improvements that will remain valid until 2.0.0. This is **not** a 2.0.0
redesign: no protocol, search-behavior, option-default or version changes.

## Method

Read-only survey of the source tree, headers, CMake targets, test layout and the
existing architecture doc, followed by small verified refactors. Measurements:

- `koi_core` before: 30 engine/system translation units, including the dead
  `move_chooser.cpp`, benchmark fixtures (`strength_suite.cpp`) and
  `uci_controller.cpp`.
- Largest translation units: `detail/search_runner.cpp` (5,307 lines),
  `nnue.cpp` (2,438), `uci_controller.cpp` (2,339),
  `detail/search_context.cpp` (2,324), `game_state.cpp` (2,191),
  `position.cpp` (2,103).
- Layout: `src/koi` (engine API + implementations), `src/koi/detail` (private
  implementation), `src/koi/gpu` (optional CUDA), `src/koi/modules` (optional
  C++26 named modules), `tests/{unit,integration,python,support,data}`,
  `tools/{build,measurement,stability,nnue,book,engine,test}`.
- The public header `search_service.hpp` pulled in the complete
  `transposition_table.hpp` (379 lines: entry layout, striped locks, hazard
  slots) just to use the hash policy/result value types.

## Findings and dispositions

| # | Finding | Disposition |
|---|---|---|
| F1 | `SearchService`'s public header leaked TT storage internals. | **Fixed**: new `src/koi/hash_types.hpp` owns `HashMemoryPolicy`, `HashResizeResult`, `HashMemorySnapshot`, enums/providers; `transposition_table.hpp` and `search_service.hpp` include it independently. `search_service.cpp` includes the TT header explicitly. Public API unchanged. |
| F2 | `move_chooser.*` had no production caller (only its own tests). | **Removed** from the core target, sources and tests. |
| F3 | Benchmark/strength fixtures (`strength_suite.cpp`, 226 lines of test positions) lived in the engine library. | **Separated**: new `koi_strength_suite` static library; `koi-bench` and `koi_strength_tests` link it explicitly; `koi_core` no longer contains test data. |
| F4 | Randomized reliability fuzzers lived under `artifacts/plans/...`, outside the test tree, yet were registered as CTest tests. | **Moved** to `tests/python/reliability/`; CMake points there; docs/evidence paths updated. |
| F5 | `uci_controller.cpp` (2,339 lines) mixes protocol parsing, the option state machine, asset lifecycle, debug logging and clock bookkeeping. | Accepted for 1.0.0 (heavily tested, single protocol writer); extracting option catalog/state is a 2.0.0 candidate, not worth the regression risk now. |
| F6 | `detail/search_runner.cpp` (5,307 lines) and `detail/search_context.*` are large search units. | Accepted; search hot paths benefit from locality and are protected by search/invariants tests. Splitting is a 2.0.0 project. |
| F7 | `koi_core` still contains the UCI controller, so the static library is the whole engine rather than a rules/search/eval library. | Documented as a 1.0.0 package boundary; a `koi_uci`/`koi_search`/`koi_rules` target split is deferred to 2.0.0 to avoid variant/target churn. |
| F8 | Optional C++26 modules (`src/koi/modules/*.ixx`) exist but no implementation imports them and macOS builds disable them. | Kept optional and documented as non-primary; not expanded in 1.0.0. |
| F9 | `docs/ARCHITECTURE.md` was accurate for the main runtime graph but had stale HCE-wording and no explicit dependency/ownership/error rules. | **Refreshed** with the 1.0.0 architecture contract and stability rules. |

## Implemented refactors (verified locally)

1. `refactor(core): move hash memory policy types to a small header` —
   compile-firewall for `SearchService` consumers.
2. `test(arch): move reliability fuzzers into the test tree` — test layout now
   matches the documented tree; CTest paths updated.
3. `refactor(arch): drop dead chooser and move strength fixtures out of core` —
   removes dead production code and test data from `koi_core`.
4. `docs(architecture): codify the 1.0.0 architecture contract` — this document
   plus the refreshed `docs/ARCHITECTURE.md`.

Verification after the refactors (Windows Release):

- engine, `koi-bench`, `koi_core_tests`, `koi_strength_tests`,
  `uci_controller_tests` build;
- `uci_controller_tests --filter=hash`: 3/3;
- `koi_core_tests`: 26/26;
- `koi_strength_tests --filter=strength`: 6/6;
- `koi_command_fuzz_process`: pass (after the test-tree move);
- full CI is run on the pushed heads; local `%TEMP%` sandbox limitations are
  unchanged and documented.

## Implemented during the program (continued)

5. Hash-memory policy/result value types split into `src/koi/hash_types.hpp`, so
   `SearchService` no longer pulls the transposition-table definition.
6. Dead `move_chooser.*` removed; benchmark fixtures moved into the separate
   `koi_strength_suite` library.
7. Reliability fuzzers relocated from `artifacts/` to `tests/python/reliability/`.
8. `detail::DebugLog` extracted behind a forward declaration; Syzygy
   forward-declared in the controller header (compile firewall).
9. UCI option catalog extracted to `detail/uci_option_table.hpp`; architecture
   guard enforces the split.
10. Bounded root-fallback policy extracted from `search_runner.cpp` into
    `detail/root_fallback.{hpp,cpp}`; the driver shrank by ~1,700 lines.
11. Option normalization moved into `SearchService` (runner no longer owns
    configuration policy).
12. Synthetic NNUE fixture factories moved to `tests/support/nnue_synthetic.cpp`.
13. Module contract drift guards added (runtime assertions in
    `koi_module_tests`); `SyzygyTablebase` gained an explicit `SyzygyLoadStatus`
    so path conflicts are reported accurately.
14. Architecture-boundary CTest (`koi_architecture_boundaries`) extended to
    enforce all of the above.

## Architecture rules codified for 1.0.0

1. **Dependency direction**: GUI/CLI → `UciController` (protocol + option + asset
   owner) → `SearchService` (session/cancellation boundary) → search runner /
   context → rules (`Position`/`GameState`), evaluator, TT, ordering, SEE.
   `detail/` is private implementation with no ABI promise.
2. **Protocol purity**: `UciController` is the only UCI writer; core search does
   no I/O and knows nothing about UCI option names or protocol syntax.
3. **Public value surface**: `search_types.hpp`, `hash_types.hpp`,
   `evaluator.hpp`, `game_state.hpp` and `search_service.hpp` are the embedding
   boundary; public headers must not expose `detail/` or third-party storage
   layouts.
4. **Lifetime/ownership**: the controller owns the current position and asset
   adapters; a `SearchHandle` owns the session; TT lifetime is guarded by hazard
   leases; Fathom is process-global and owned by the `SyzygyTablebase` adapter.
5. **Fail-closed boundaries**: assets (NNUE, book, Syzygy, debug log) validate
   and fail closed at the boundary; no failure may cross a thread boundary or
   terminate the UCI session.
6. **Tests mirror the tree**: component unit tests in `tests/unit`, protocol/
   process behaviour in `tests/integration`, randomized fixtures/tools in
   `tests/python/reliability`, shared helpers in `tests/support`. Fuzzers are
   CTest tests, not artifacts.
7. **Stability**: UCI surface/order, option defaults, file layout,
   `SearchService` API and `koi-*` CLI behaviour are frozen for 1.0.0; changes
   after this baseline must be internal refactors or bug fixes.

## Deferred to 2.0.0 (documented, not started)

- Target split: `koi_rules` / `koi_eval` / `koi_search` / `koi_uci` instead of
  one `koi_core`.
- Optional extraction of the UCI option catalog/state machine from the
  controller.
- Primary-module build (or removal) of `src/koi/modules`.
- Any evaluator/search architecture redesign; GPU NNUE remains experimental and
  scheduled for removal.

Non-blocking TT resizing was **implemented** during the program:
`try_set_size_mb` publishes new storage immediately and keeps a pinned retired
storage in a deferred list for later reclamation; the blocking API and session
pin contracts are unchanged.
- No version bump/tag/release is part of this baseline.
