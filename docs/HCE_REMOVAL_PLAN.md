# HCE (classical evaluator) removal + clean re-add plan

User direction (2026-10-28): remove the hand-crafted evaluator (HCE) because the
current implementation is riddled with odd code, and plan a clean rewrite later.
NNUE `koi-v7-h128-official-20261025` is the production evaluator.

## Inventory (current tree)
58 files mention the classical evaluator. The core surfaces:

- Implementation/parameters: `src/koi/classical_evaluator.*`,
  `evaluation_parameters*.hpp`, `evaluation_piece_squares.hpp`,
  `piece_values.hpp` (verify ownership before deleting), classical term code in
  `evaluation_features.*`.
- Evaluator selection: `make_evaluator()` in `nnue.cpp` (classical fallback when
  no/`bad` net), `main.cpp` boot resolution, `uci_controller.*` EvalFile fallback
  and identity diagnostics, GPU evaluator fallback.
- Search coupling: `SearchOptions::evaluator_mode`, eval-cache key comments,
  classical-specific pruning comments in `search_constants.hpp` /
  `search_context.hpp` / `search_policy.hpp` / `search_table_access.hpp`.
- Tools: `koi-bench --nnue`-less mode, `koi-eval-features`, `tuning` scripts,
  `tools/nnue/ab_match.ps1` (NNUE vs classical), `elo_estimate.py` mode flags.
- Tests: `classical_evaluator_tests`, `evaluation_boundary_tests`,
  `nnue_boundary_tests`, `uci_controller_tests`, `koi_search_tests`,
  `koi_strength_tests`, GPU/soak/service tests, several PowerShell/Python
  integration tests that assume a no-net engine works.
- Docs: README, ENGINE_SPEC §5, USER_GUIDE, ARCHITECTURE, NNUE_PIPELINE,
  tests/README, bot docs.

## Behavior decision required
With HCE gone the engine cannot evaluate without a network. Options:
A. **NNUE required** (recommended): missing/rejected net => boot continues with no
   evaluator and `go`/search refuses (exit 4 only when nothing can run), searches
   emit `info string no evaluator`; release packages bundle the gated net; tests
   and scripts pass `EvalFile` (or place the fixture net beside the engine).
B. Keep a minimal temporary material/PST fallback shim until the clean HCE lands.
C. Build-time `KOI_ENABLE_HCE=OFF` default: code stays in the tree unreferenced.

## Staged plan
1. Freeze: tag/record current HCE sources in a local archive (no git history of the
   removal needed beyond the commit).
2. Delete HCE implementation + parameter tables; remove `evaluator_mode` and
   classical branches from selection/UCI/GPU/tools.
3. Rewire boot: NNUE required (option A), truthful diagnostics; update handshake
   tests if any option disappears.
4. Convert tests that used classical as a neutral baseline: pin the promoted net
   via `EvalFile` (or the `koi-nnue-fixture` net for cheap unit tests). Delete
   classical-only test suites.
5. Update docs (README, ENGINE_SPEC, USER_GUIDE, ARCHITECTURE, NNUE_PIPELINE,
   tests/README, bot docs) and the packaging INSTALL text.
6. Run: unit evolution/search/runtime sets, process/integration, Python
   measurement/stability, docs build; commit staged commits.
7. Re-add plan (later, separate cycle): clean HCE module with its own value type,
   explicit score scale conversion to search, no hidden coupling into NNUE
   feature code, equal-time gate vs promoted NNUE only for feature-development
   purposes (not for promotion).

## Risks
- Large test-suite rework (process tests launch engines without a net).
- Score-scale assumptions in pruning comments/tables were calibrated for the old
  classical range; after removal those comments should be rewritten for NNUE.
- Tooling that offers evaluator A/B (`ab_match`, `koi-bench` mode flags) must be
  retargeted or deleted.
