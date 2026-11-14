#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "koi/move.hpp"
#include "koi/search_types.hpp"

namespace koi {

class GameState;
class OpeningBook;
class SearchService;
class SyzygyTablebase;

// Standard move-quality classifications ordered from best/special finds down to
// tactical errors.
enum class MoveAnnotation : std::uint8_t {
    brilliant,
    great,
    best,
    excellent,
    good,
    book,
    forced,
    inaccuracy,
    mistake,
    miss,
    blunder,
};

// Side-to-move normalized evaluation score for a root move.
struct MoveScoreEval {
    int score_cp = 0;
    std::optional<int> mate;
};

// Inputs to deterministic move-quality classification.
struct MoveClassificationInput {
    Move played_move = Move::no_move();
    Move best_move = Move::no_move();
    MoveScoreEval best_eval{};
    MoveScoreEval played_eval{};
    std::optional<MoveScoreEval> second_best_eval;
    std::size_t legal_move_count = 0;
    bool is_book_move = false;
    bool is_sacrifice = false;
    bool is_capture_move = false;
    bool in_check = false;
    bool gives_check = false;
};

// Controls engine-backed single-move and full-game annotation.
struct MoveAnnotationOptions {
    std::optional<int> depth;
    std::optional<std::chrono::milliseconds> movetime;
    std::optional<std::uint64_t> nodes;
    std::size_t threads = 1;
    bool analyse_mode = true;
    int contempt_cp = 0;
    std::uint32_t random_seed = 0;
    std::shared_ptr<const SyzygyTablebase> syzygy;
    std::uint8_t syzygy_interior_depth = 0;
    bool is_book_move = false;
    const OpeningBook* opening_book = nullptr;
    bool book_enabled = false;
    std::uint8_t book_depth = 16;
    bool book_safety = true;
    std::uint8_t book_safety_depth = 2;
};

struct MoveAnnotationResult {
    // 0-based ply index matching UCI's root_ply convention (0 = White's move 1).
    std::uint32_t ply = 0;
    Color side_to_move = Color::white;
    Move played_move = Move::no_move();
    Move best_move = Move::no_move();
    MoveAnnotation annotation = MoveAnnotation::good;
    MoveScoreEval best_eval{};
    MoveScoreEval played_eval{};
    std::optional<MoveScoreEval> second_best_eval;
    int centipawn_loss = 0;
    double win_prob_before = 0.5;
    double win_prob_after = 0.5;
    double accuracy_percent = 100.0;
    int depth = 0;
    std::vector<Move> best_pv;
    std::vector<Move> played_pv;
    bool is_sacrifice = false;
    bool is_capture_move = false;
    bool in_check = false;
    bool gives_check = false;
};

struct PlayerAnnotationSummary {
    std::size_t move_count = 0;
    double average_accuracy = 100.0;
    double average_cpl = 0.0;
    std::size_t brilliant = 0;
    std::size_t great = 0;
    std::size_t best = 0;
    std::size_t excellent = 0;
    std::size_t good = 0;
    std::size_t book = 0;
    std::size_t forced = 0;
    std::size_t inaccuracy = 0;
    std::size_t mistake = 0;
    std::size_t miss = 0;
    std::size_t blunder = 0;
};

struct GameAnnotationResult {
    std::vector<MoveAnnotationResult> moves;
    PlayerAnnotationSummary white;
    PlayerAnnotationSummary black;
};

[[nodiscard]] std::string_view move_annotation_name(MoveAnnotation annotation) noexcept;
[[nodiscard]] std::string_view move_annotation_glyph(MoveAnnotation annotation) noexcept;
// PGN Numeric Annotation Glyph (0 when no standard NAG applies).
[[nodiscard]] int move_annotation_nag(MoveAnnotation annotation) noexcept;
[[nodiscard]] std::optional<MoveAnnotation> parse_move_annotation(
    std::string_view name) noexcept;

// Converts a side-to-move evaluation into expected win probability in [0.0, 1.0].
[[nodiscard]] double win_probability_from_eval(const MoveScoreEval& eval) noexcept;
[[nodiscard]] double win_probability_from_cp(int score_cp) noexcept;

// Non-negative centipawn loss between best and played evaluations (same root side).
[[nodiscard]] int compute_centipawn_loss(const MoveScoreEval& best_eval,
                                         const MoveScoreEval& played_eval) noexcept;

// Move accuracy percentage in [0.0, 100.0] from win-probability drop.
[[nodiscard]] double move_accuracy_percent(double win_prob_before,
                                           double win_prob_after) noexcept;

[[nodiscard]] MoveAnnotation classify_move_annotation(
    const MoveClassificationInput& input) noexcept;

// Searches `root` (MultiPV=2, plus a targeted search if needed) to classify `played_move`.
[[nodiscard]] std::expected<MoveAnnotationResult, std::string> analyze_and_annotate_move(
    SearchService& service, const GameState& root, Move played_move,
    const MoveAnnotationOptions& options = {});

// Replays `moves` from `initial_position`, annotating each ply in order.
[[nodiscard]] std::expected<GameAnnotationResult, std::string> annotate_game(
    SearchService& service, const GameState& initial_position, std::span<const Move> moves,
    const MoveAnnotationOptions& options = {});

} // namespace koi
