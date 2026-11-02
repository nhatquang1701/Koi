#pragma once

// Clean hand-crafted evaluator (HCE).
//
// Optional, selectable evaluator: NNUE remains the production/default path.
// This implementation is stateless and thread-safe; every evaluation derives
// from the position's feature snapshot and the native attack tables, with no
// hidden global state.  The score is in centipawns from the requested
// perspective, compatible with the search's score range.

#include "koi/evaluator.hpp"

namespace koi {

class ClassicalEvaluator final : public Evaluator {
public:
    [[nodiscard]] int evaluate(const GameState& state, Color perspective) const override;

    // Pure function of (position, perspective): no shared mutable evaluator
    // state, so search workers may evaluate concurrently.
    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }
};

} // namespace koi
