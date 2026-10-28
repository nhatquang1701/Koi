#pragma once

// Deterministic, deliberately simple material + centralisation evaluator used
// only by tests. It exists because the hand-crafted classical evaluator (HCE)
// was removed from the engine on 2026-10-28; a clean HCE rewrite is planned
// (docs/HCE_REMOVAL_PLAN.md). Production code must use NNUE.

#include "koi/evaluator.hpp"
#include "koi/game_state.hpp"
#include "koi/piece_values.hpp"

namespace koi::test {

class MaterialEvaluator final : public Evaluator {
public:
    [[nodiscard]] int evaluate(const GameState& state, const Color perspective) const override {
        int score = 0;  // white-positive
        for (std::uint8_t index = 0; index < Square::kInvalid; ++index) {
            const Piece piece = state.piece_at(Square::from_index(index));
            if (piece.empty()) {
                continue;
            }
            const int file = static_cast<int>(index % 8U);
            const int rank = static_cast<int>(index / 8U);
            const int centre = 7 - (std::abs(file - 3) + std::abs(file - 4) +
                                    std::abs(rank - 3) + std::abs(rank - 4));
            int value = piece_material_value(piece.type);
            switch (piece.type) {
            case PieceType::pawn:
                value += centre * 2;
                break;
            case PieceType::knight:
            case PieceType::bishop:
                value += centre * 3;
                break;
            default:
                break;
            }
            score += piece.color == Color::white ? value : -value;
        }
        return perspective == Color::white ? score : -score;
    }

    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }
};

} // namespace koi::test
