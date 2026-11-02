#include <atomic>
#include <cmath>
#include <cstddef>
#include <thread>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/detail/search_constants.hpp"
#include "koi/game_state.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::Color;
using koi::ClassicalEvaluator;
using koi::GameState;
using koi::test::require;

[[nodiscard]] GameState state_from_fen(const char* fen) {
    auto state = GameState::from_fen(fen);
    require(state.has_value(), "test FEN must be valid");
    return *state;
}

void test_startpos_is_symmetric_and_bounded() {
    const GameState state = GameState::startpos();
    const ClassicalEvaluator evaluator;
    const int white = evaluator.evaluate(state, Color::white);
    const int black = evaluator.evaluate(state, Color::black);
    require(white == -black, "perspective scores must be exact negations");
    require(std::abs(white) <= 60, "startpos evaluation must be near zero");
    require(evaluator.supports_concurrent_evaluation(),
            "the HCE is stateless and must advertise concurrent evaluation");
}

void test_material_advantage_has_the_expected_sign() {
    const ClassicalEvaluator evaluator;
    const GameState white_queen_up = state_from_fen("4k3/8/8/8/8/8/8/QQ2K3 w - - 0 1");
    require(evaluator.evaluate(white_queen_up, Color::white) > 800,
            "two extra queens must be a large white advantage");
    require(evaluator.evaluate(white_queen_up, Color::black) < -800,
            "the same advantage must be negative for Black");

    const GameState black_rook_up = state_from_fen("4k3/r7/8/8/8/8/8/4K3 w - - 0 1");
    require(evaluator.evaluate(black_rook_up, Color::white) < -400,
            "a missing rook must be a large white disadvantage");
}

void test_color_swapped_positions_are_negations() {
    const ClassicalEvaluator evaluator;
    const GameState white_pawn = state_from_fen("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1");
    const GameState black_pawn = state_from_fen("4k3/4p3/8/8/8/8/8/4K3 b - - 0 1");
    // With side-to-move tempo, a colour-swapped position is equal from the
    // mover's perspective (and the opposite perspectives are its negation).
    require(evaluator.evaluate(white_pawn, Color::white) ==
                evaluator.evaluate(black_pawn, Color::black),
            "colour-swapped positions must be equal for the side to move");
    require(evaluator.evaluate(white_pawn, Color::black) ==
                evaluator.evaluate(black_pawn, Color::white),
            "the opposite perspectives must remain negations");
}

void test_scores_are_bounded_and_deterministic() {
    const ClassicalEvaluator evaluator;
    const GameState crowded = state_from_fen(
        "r1bq1rk1/ppp2ppp/2np1n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w - - 0 1");
    const int first = evaluator.evaluate(crowded, Color::white);
    require(std::abs(first) < koi::detail::kMateThreshold,
            "HCE scores must stay outside the search's mate range");
    for (int repetition = 0; repetition < 8; ++repetition) {
        require(evaluator.evaluate(crowded, Color::white) == first,
                "the HCE must be deterministic");
    }
}

void test_concurrent_evaluation_is_safe_and_stable() {
    const ClassicalEvaluator evaluator;
    const GameState state = state_from_fen(
        "r1bq1rk1/ppp2ppp/2np1n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w - - 0 1");
    const int expected = evaluator.evaluate(state, Color::white);
    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            for (int repetition = 0; repetition < 100; ++repetition) {
                if (evaluator.evaluate(state, Color::white) != expected) {
                    ++mismatches;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(mismatches == 0, "concurrent HCE evaluations must be identical");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"startpos symmetric and bounded", test_startpos_is_symmetric_and_bounded},
        {"material advantage sign", test_material_advantage_has_the_expected_sign},
        {"colour-swapped negation", test_color_swapped_positions_are_negations},
        {"bounded and deterministic", test_scores_are_bounded_and_deterministic},
        {"concurrent evaluation", test_concurrent_evaluation_is_safe_and_stable},
    };
    return koi::test::run_tests(tests, argc, argv);
}
