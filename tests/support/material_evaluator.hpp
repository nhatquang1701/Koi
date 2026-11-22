#pragma once

// Deterministic, deliberately simple material + centralisation evaluator used
// only by tests to isolate search contracts from NNUE network or ClassicalEvaluator
// calibration changes. Production code uses NNUE by default (or ClassicalEvaluator
// via EvalMode=hce).

#include <array>
#include <bit>
#include <cstdint>

#include "koi/evaluator.hpp"
#include "koi/game_state.hpp"
#include "koi/piece_values.hpp"

namespace koi::test {

namespace detail {

inline constexpr std::array<int, 64> kCentreBonus = []() constexpr {
    auto abs_diff = [](const int a, const int b) constexpr noexcept {
        return a < b ? b - a : a - b;
    };
    std::array<int, 64> bonus{};
    for (std::uint8_t index = 0; index < 64; ++index) {
        const int file = static_cast<int>(index % 8U);
        const int rank = static_cast<int>(index / 8U);
        bonus[index] = 7 - (abs_diff(file, 3) + abs_diff(file, 4) +
                            abs_diff(rank, 3) + abs_diff(rank, 4));
    }
    return bonus;
}();

} // namespace detail

class MaterialEvaluator final : public Evaluator {
public:
    [[nodiscard]] int evaluate(const GameState& state, const Color perspective) const override {
        const auto& bitboards = state.piece_bitboards();
        const auto side_score = [&](const std::size_t color) noexcept {
            int total = 0;
            constexpr std::array<PieceType, 6> kTypes{
                PieceType::pawn, PieceType::knight, PieceType::bishop,
                PieceType::rook, PieceType::queen,  PieceType::king};
            for (const PieceType type : kTypes) {
                const std::uint64_t bb = bitboards[color][static_cast<std::size_t>(type)];
                if (bb != 0) {
                    total += std::popcount(bb) * piece_material_value(type);
                }
            }
            const auto sum_centre = [](std::uint64_t bb) noexcept {
                int sum = 0;
                while (bb != 0) {
                    sum += detail::kCentreBonus[static_cast<std::size_t>(std::countr_zero(bb))];
                    bb &= bb - 1;
                }
                return sum;
            };
            total += sum_centre(bitboards[color][static_cast<std::size_t>(PieceType::pawn)]) * 2;
            const std::uint64_t minors =
                bitboards[color][static_cast<std::size_t>(PieceType::knight)] |
                bitboards[color][static_cast<std::size_t>(PieceType::bishop)];
            total += sum_centre(minors) * 3;
            return total;
        };
        const int score = side_score(0U) - side_score(1U);  // white-positive
        return perspective == Color::white ? score : -score;
    }

    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }
};

} // namespace koi::test
