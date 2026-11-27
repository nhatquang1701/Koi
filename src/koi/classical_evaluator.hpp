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

struct ClassicalScore {
    int mg = 0;
    int eg = 0;
};

struct ClassicalEvalTrace {
    ClassicalScore material{};
    ClassicalScore pst{};
    ClassicalScore mobility{};
    ClassicalScore pawns{};
    ClassicalScore king_safety{};
    ClassicalScore threats{};
    ClassicalScore piece_terms{};
    ClassicalScore space_dev{};
    ClassicalScore tempo{};
    ClassicalScore total{};
    int phase = 0;
    int total_white_cp = 0;
    bool insufficient_material = false;
    bool opposite_bishop_draw = false;
};

class ClassicalEvaluator final : public Evaluator {
public:
    [[nodiscard]] int evaluate(const GameState& state, Color perspective) const override;
    [[nodiscard]] ClassicalEvalTrace trace(const GameState& state) const;

    // Pure function of (position, perspective): no shared mutable evaluator
    // state, so search workers may evaluate concurrently.
    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }
};

} // namespace koi
