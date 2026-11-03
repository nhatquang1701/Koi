#include <atomic>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
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

std::map<std::string, std::string> read_endgame_fixtures() {
    std::map<std::string, std::string> fixtures;
    const std::filesystem::path path =
        std::filesystem::path(KOI_TEST_DATA_DIR) / "endgames" / "endgame-positions.txt";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "the endgame FEN corpus must exist");
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t separator = line.find('|');
        require(separator != std::string::npos,
                "every endgame fixture must be name|FEN");
        fixtures[line.substr(0, separator)] = line.substr(separator + 1);
    }
    require(!fixtures.empty(), "the endgame FEN corpus must not be empty");
    return fixtures;
}

void test_random_game_evaluations_are_bounded_and_symmetric() {
    const ClassicalEvaluator evaluator;
    std::mt19937 rng(20261103U);
    for (int game = 0; game < 24; ++game) {
        GameState state = GameState::startpos();
        for (int ply = 0; ply < 120; ++ply) {
            const int white = evaluator.evaluate(state, Color::white);
            const int black = evaluator.evaluate(state, Color::black);
            require(white == -black,
                    "random-game evaluations must negate across perspectives");
            require(std::abs(white) < koi::detail::kMateThreshold,
                    "random-game evaluations must stay outside the mate range");
            const std::vector<koi::Move> moves = state.legal_moves();
            if (moves.empty()) {
                break;
            }
            std::uniform_int_distribution<std::size_t> pick(0, moves.size() - 1);
            require(state.make_move(moves[pick(rng)]),
                    "random legal move must apply");
        }
    }
}

void test_pawn_structure_directions_and_tempo() {
    const ClassicalEvaluator evaluator;

    // Same two pawns: doubled on one file vs connected on adjacent files.
    const int doubled = evaluator.evaluate(
        state_from_fen("4k3/8/8/8/8/3P4/3P4/4K3 w - - 0 1"), Color::white);
    const int connected = evaluator.evaluate(
        state_from_fen("4k3/8/8/8/8/4P3/3P4/4K3 w - - 0 1"), Color::white);
    require(connected > doubled,
            "connected pawns must outscore doubled pawns");

    // Same two pawns: isolated on opposite wings vs connected.
    const int isolated = evaluator.evaluate(
        state_from_fen("4k3/8/8/8/8/P6P/8/4K3 w - - 0 1"), Color::white);
    const int connected_wing = evaluator.evaluate(
        state_from_fen("4k3/8/8/8/8/PP6/8/4K3 w - - 0 1"), Color::white);
    require(connected_wing > isolated,
            "connected wing pawns must outscore isolated wing pawns");

    // Tempo: the side to move gets a small positive bonus in a symmetric
    // position, so the side-to-move score is positive.
    const int startpos_white = evaluator.evaluate(GameState::startpos(), Color::white);
    require(startpos_white > 0,
            "the side to move must receive a positive tempo bonus");
}

void test_endgame_fixture_invariants() {
    const ClassicalEvaluator evaluator;
    const std::map<std::string, std::string> fixtures = read_endgame_fixtures();
    for (const auto& [name, fen] : fixtures) {
        const GameState state = state_from_fen(fen.c_str());
        const int white = evaluator.evaluate(state, Color::white);
        const int black = evaluator.evaluate(state, Color::black);
        require(white == -black, "endgame perspective scores must negate: " + name);
        require(std::abs(white) < koi::detail::kMateThreshold,
                "endgame scores must stay outside the mate range: " + name);
        require(evaluator.evaluate(state, Color::white) == white,
                "endgame evaluation must be deterministic: " + name);
    }

    const GameState dead = state_from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    require(evaluator.evaluate(dead, Color::white) == 0,
            "bare kings must evaluate as a dead draw");
}

void test_pawn_king_and_piece_terms_are_directional() {
    const ClassicalEvaluator evaluator;
    const int blocked = evaluator.evaluate(
        state_from_fen("4k3/4p3/4P3/8/8/8/8/4K3 w - - 0 1"), Color::white);
    const int free_pawn = evaluator.evaluate(
        state_from_fen("4k3/8/4P3/8/8/8/8/4K3 w - - 0 1"), Color::white);
    require(free_pawn > blocked,
            "a free advanced pawn must outscore a blocked one");

    const int advanced = evaluator.evaluate(
        state_from_fen("4k3/4P3/8/8/8/8/8/4K3 w - - 0 1"), Color::white);
    const int initial = evaluator.evaluate(
        state_from_fen("4k3/8/4P3/8/8/8/8/4K3 w - - 0 1"), Color::white);
    require(advanced > initial, "a more advanced passed pawn must score higher");

    const int bishop_pair = evaluator.evaluate(
        state_from_fen("4k3/8/8/8/8/8/8/2BBK3 w - - 0 1"), Color::white);
    const int bishop_knight = evaluator.evaluate(
        state_from_fen("4k3/8/8/8/8/8/8/2BNK3 w - - 0 1"), Color::white);
    require(bishop_pair > bishop_knight, "the bishop pair must be rewarded");

    const int blocked_rook = evaluator.evaluate(
        state_from_fen("4k3/8/8/8/8/8/P7/R3K3 w - - 0 1"), Color::white);
    const int semi_open_rook = evaluator.evaluate(
        state_from_fen("4k3/8/8/8/8/8/1P6/R3K3 w - - 0 1"), Color::white);
    require(semi_open_rook > blocked_rook,
            "a rook beside an own pawn must outscore a blocked rook");

    // Middlegame material keeps the tapered king table in its mg regime; in a
    // bare pawn ending a central king is legitimately better.
    const int exposed_king = evaluator.evaluate(
        state_from_fen("rnbqkbnr/pppppppp/8/8/4K3/8/PPPPPPPP/RNBQ1BNR w kq - 0 1"),
        Color::white);
    const int sheltered_king = evaluator.evaluate(
        state_from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w kq - 0 1"),
        Color::white);
    require(sheltered_king > exposed_king,
            "a sheltered castled king must outscore an exposed king (sheltered=" +
                std::to_string(sheltered_king) + " exposed=" +
                std::to_string(exposed_king) + ")");
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
        {"random game evaluations", test_random_game_evaluations_are_bounded_and_symmetric},
        {"pawn structure and tempo directions",
         test_pawn_structure_directions_and_tempo},
        {"endgame fixture invariants", test_endgame_fixture_invariants},
        {"pawn/king/piece directions", test_pawn_king_and_piece_terms_are_directional},
        {"concurrent evaluation", test_concurrent_evaluation_is_safe_and_stable},
    };
    return koi::test::run_tests(tests, argc, argv);
}
