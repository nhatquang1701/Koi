# Architecture verification snapshot (1.0.0 baseline)

Branch: `koi-engine`
Verified heads: code `325c7c1`, certification commit `85ebba7`, and the final
hygiene head `fc5311b`; all are fully green on Windows/Linux/docs (watched to
completion). Earlier CI head
`0ac7758` and the intermediate refactors were validated locally and under ASan.
Date: 2026-11-01

## CI (GitHub Actions)

| Workflow | Head | Result |
|---|---|---|
| Windows CTest | `0ac7758` | **success** (release-full, debug-smoke, sanitizer, shadow-diff, package, flake) |
| Linux CTest | `0ac7758` | **success** (gcc, clang, modules-off, tarball, flake, TSan) |
| Deploy documentation site | `0ac7758` | **success** |
| macOS CTest | `0ac7758`, later heads | cancelled by the runner ~1 s after creation (infrastructure; both jobs), including a manual `workflow_dispatch` run on `32a8554`. Local macOS-equivalent configurations are documented; earlier macOS legs were green. |

Run IDs: Windows `41326510478`, Linux `41326510541`.

## Local verification (Windows Release tree `build/full-release-gpu`)

- Full non-heavy suite: all C++/process/packaging tests pass. Only the known
  local Python environment/timing failures remain (`elo_estimate_python`,
  `measurement_phase_python`, `measurement_forensics_python`,
  `uci_benchmark_python`).
- **Complete local CTest matrix, heavy labels included** (358.9 s): all
  C++/process/packaging/reliability/search/tools/uci/unit tests pass; only the
  known local Python environment/timing failures remain
  (`elo_estimate_python`, `measurement_phase_python`,
  `measurement_forensics_python`, `koi_trainer_python` timeout).
- `koi_search_tests`: 4/4 shards (140.5 s).
- `uci_controller_tests`: 66/66.
- `syzygy_tablebase_tests`: 22/22.
- `search_service_tests`: 15/15.
- Module build (`KOI_BUILD_MODULES=ON`): `koi_module_tests` 1/1.
- `koi_architecture_boundaries`: pass (public-header seams, protocol purity,
  core-source purity, test-tree placement, artifacts-free build, documented
  contract).
- Strict documentation build: `mkdocs build --strict` pass.

### macOS local validation (GitHub macOS runner unavailable)

- Fresh clone of `koi-engine` built on the Mac host (Apple clang 21, arm64
  Darwin 27, `-DKOI_BUILD_MODULES=OFF -DKOI_STATIC_RUNTIME=OFF
  -DKOI_ENABLE_GPU_NNUE=OFF`, Ninja Release).
- `koi_engine`, `koi_core_tests`, `search_service_tests`,
  `nnue_boundary_tests`, `uci_controller_tests` and `koi_nnue_fixture` built.
- CTest: `koi_core_tests`, `nnue_boundary_tests`, `uci_controller_tests`,
  `search_service_tests` — **4/4 pass** (2.35 s).
- This compensates for the GitHub macOS jobs, which are cancelled by the runner
  infrastructure including on manual `workflow_dispatch`.

## Architecture changes landed on this branch

- Hash policy/result values separated from the TT definition (`hash_types.hpp`).
- Dead `move_chooser` removed; benchmark fixtures split into
  `koi_strength_suite`.
- Reliability fuzzers moved to `tests/python/reliability/`.
- `detail::DebugLog` and Syzygy compiled behind controller-header forward
  declarations.
- UCI option catalog extracted to `detail/uci_option_table.hpp`.
- Bounded root-fallback policy extracted to `detail/root_fallback.{hpp,cpp}`.
- Worker pools and stats merge extracted to
  `detail/search_worker_pools.hpp` / `detail/search_stats_support.hpp`
  (`search_runner.cpp`: 5,307 → 2,725 lines).
- Option normalization lives in `SearchService` (the runner no longer owns
  configuration policy).
- Synthetic NNUE fixtures moved to `tests/support/nnue_synthetic.cpp`.
- Stateless FEN validation moved to `detail/fen_validation.*`.
- `SyzygyTablebase` has an explicit `SyzygyLoadStatus` (path conflicts are
  reported distinctly instead of as "no usable tables").
- Failed search completions emit a protocol diagnostic before their fallback
  `bestmove`; setup-prefix failures carry the request identity so their
  completion is not suppressed as stale.
- `koi_architecture_boundaries` CTest enforces the above rules.

## Sanitizer validation after the refactors

- ASan tree rebuilt from the refactored source.
- ASan unit label: **24/24 pass** (50.6 s).
- ASan reliability label: **8/8 pass** on the final tree (`325c7c1`),
  **804.9 s wall**, zero AddressSanitizer/SEGV reports (an earlier 790.9 s run
  covered the same refactors).
- ASan unit label on the final tree: **24/24 pass** (71.6 s).
- This covers the root-fallback extraction, worker pools/stats support,
  header-seam changes, typed failure paths, protocol diagnostics, move-only
  `SearchService`, and the test-support/API seam moves.

## Review follow-ups completed

The independent re-audit at `8b9cc2b` confirmed all boundaries and found only
hygiene issues; all were fixed: inline `/` direct includes for header helpers,
internal linkage restored for moved helper TUs, shared option bounds
centralized, the start-failure typed diagnostic, the `noexcept` allocation
window in the fallback scanner, and all test-only declarations removed from
shipped headers (synthetic NNUE factories, test controller factory).

## Deliberately deferred (2.0.0 ownership work)

Deferred/non-blocking TT-resize semantics. The current `set_size_mb` waits for
retired-storage readers, which is correct and only reachable by an embedder that
violates the one-live-search contract or resizes mid-search. A safe fix needs a
queued-resize design tied to session teardown and is out of scope for the
1.0.0-stability pass.

## Release state

No version bump, tag, or release was made. `1.1.5` remains at `1e09504`.
