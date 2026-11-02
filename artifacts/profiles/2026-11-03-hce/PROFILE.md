# HCE hot-path profile (Mac M4)

Date: 2026-11-03. Binary: `bench-v10` (HCE default, `--hce --depth 5`),
AppleClang 21 Release, arm64. Tool: macOS `sample <pid> 8 1`.

## Finding

The 8-second sample contains 5,256 samples. Nearly all are under
`SearchRunner::run_impl` -> `SearchContext::negamax` -> `SearchContext::quiescence`,
with the remaining time in move generation/legality and attack-table helpers
(`bishop_attacks`, `rook_attacks`, `square_attacked`). `ClassicalEvaluator::evaluate`
does not appear as a material share of the stack.

The depth-5 bench shows ~67 qnodes per node (286.6M qnodes for 4.28M nodes), so
quiescence/search work dominates the measured time, not HCE leaf evaluation.

## Implication

Micro-optimising `make_eval_board`/term loops will not materially change HCE
playing strength at fixed time; future work should target:
- evaluation quality (weights/terms that change move choices), and
- search/eval synergy (HCE score interaction with pruning), with Mac gates.

The profiling artifact is local; this note records the finding.
