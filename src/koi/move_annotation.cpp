#include "koi/move_annotation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

#include "koi/game_state.hpp"
#include "koi/opening_book.hpp"
#include "koi/piece_values.hpp"
#include "koi/search_service.hpp"
#include "koi/syzygy_tablebase.hpp"

namespace koi {
namespace {

constexpr int kMateEvalCp = 100'000;
constexpr double kWinProbScale = 0.00368208;
constexpr int kDefaultAnnotationDepth = 4;

struct AnnotationMetadata {
    MoveAnnotation value;
    std::string_view name;
    std::string_view glyph;
    int nag;
};

constexpr std::array<AnnotationMetadata, 11> kAnnotationTable{{
    {MoveAnnotation::brilliant, "brilliant", "!!", 3},
    {MoveAnnotation::great, "great", "!", 1},
    {MoveAnnotation::best, "best", "", 0},
    {MoveAnnotation::excellent, "excellent", "", 0},
    {MoveAnnotation::good, "good", "", 0},
    {MoveAnnotation::book, "book", "", 0},
    {MoveAnnotation::forced, "forced", "", 7},
    {MoveAnnotation::inaccuracy, "inaccuracy", "?!", 6},
    {MoveAnnotation::mistake, "mistake", "?", 2},
    {MoveAnnotation::miss, "miss", "?", 2},
    {MoveAnnotation::blunder, "blunder", "??", 4},
}};

[[nodiscard]] const AnnotationMetadata& lookup_metadata(
    const MoveAnnotation annotation) noexcept {
    for (const auto& entry : kAnnotationTable) {
        if (entry.value == annotation) {
            return entry;
        }
    }
    return kAnnotationTable[4]; // good
}

[[nodiscard]] int effective_cp(const MoveScoreEval& eval) noexcept {
    if (eval.mate.has_value()) {
        const int clamped_distance = std::min(std::abs(*eval.mate), 1'000);
        return *eval.mate > 0 ? (kMateEvalCp - clamped_distance) : (-kMateEvalCp + clamped_distance);
    }
    return std::clamp(eval.score_cp, -kMateEvalCp + 1'001, kMateEvalCp - 1'001);
}

[[nodiscard]] std::uint32_t compute_root_ply(const GameState& state) noexcept {
    const std::uint32_t fullmoves =
        state.fullmove_number() > 0 ? static_cast<std::uint32_t>(state.fullmove_number() - 1) : 0U;
    return fullmoves * 2U + (state.side_to_move() == Color::black ? 1U : 0U);
}

[[nodiscard]] bool equals_ascii_ignore_case(std::string_view lhs,
                                            std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(lhs[index])) !=
            std::tolower(static_cast<unsigned char>(rhs[index]))) {
            return false;
        }
    }
    return true;
}

struct MoveExposure {
    int net_exposure = 0;
    int origin_square_see = 0;
};

[[nodiscard]] std::optional<MoveExposure> compute_move_exposure(
    const GameState& root, const MoveMetadata& meta,
    const Square tracked_origin = Square{}) noexcept {
    GameState after = root;
    if (!after.make_move(meta.move)) {
        return std::nullopt;
    }
    const int captured_val = meta.is_capture() ? piece_material_value(meta.captured_piece) : 0;
    MoveMetadataList replies;
    (void)after.legal_tactical_moves_with_metadata(replies, false, true);

    int max_reply_see = 0;
    int origin_see = 0;
    for (const MoveMetadata& reply : replies) {
        if (!reply.is_capture() || !reply.see_computed) {
            continue;
        }
        const int see = static_cast<int>(reply.see_score);
        if (see > max_reply_see) {
            max_reply_see = see;
        }
        if (tracked_origin.index() != Square::kInvalid && reply.move.to() == tracked_origin &&
            see > origin_see) {
            origin_see = see;
        }
    }
    return MoveExposure{max_reply_see - captured_val, origin_see};
}

[[nodiscard]] bool is_sound_sacrifice_candidate(const GameState& root, Move played_move) noexcept {
    if (root.in_check() || played_move.promotion() != Promotion::none) {
        return false;
    }
    const auto metadata = root.describe_move(played_move);
    if (!metadata.has_value()) {
        return false;
    }

    GameState after = root;
    if (!after.make_move(played_move)) {
        return false;
    }

    MoveMetadataList replies;
    (void)after.legal_tactical_moves_with_metadata(replies, false, true);

    const int captured_val =
        metadata->is_capture() ? piece_material_value(metadata->captured_piece) : 0;
    const int moving_val = piece_material_value(metadata->moving_piece);

    bool sacrifices_material = false;
    for (const MoveMetadata& reply : replies) {
        if (!reply.is_capture() || !reply.see_computed) {
            continue;
        }
        const int reply_see = static_cast<int>(reply.see_score);
        if (reply.move.to() == played_move.to()) {
            if (moving_val - captured_val >= 170 && reply_see - captured_val >= 120) {
                sacrifices_material = true;
                break;
            }
        } else {
            const Piece victim = after.piece_at(reply.move.to());
            if (piece_material_value(victim.type) >= kKnightMaterialValue &&
                reply_see - captured_val >= 170) {
                sacrifices_material = true;
                break;
            }
        }
    }
    if (!sacrifices_material) {
        return false;
    }

    // Verify the sacrifice is voluntary rather than a forced capture from a
    // fork, skewer, or trapped-piece (desperado) situation.
    MoveMetadataList legal_moves;
    root.legal_moves_with_metadata(legal_moves, false, true);

    bool has_safe_alternative = false;
    bool another_piece_keeps_from_safe = false;
    bool moving_piece_can_escape = false;
    const Square origin = played_move.from();

    for (const MoveMetadata& alt : legal_moves) {
        if (alt.move == played_move) {
            continue;
        }
        const auto exposure = compute_move_exposure(root, alt, origin);
        if (!exposure.has_value()) {
            continue;
        }
        if (exposure->net_exposure <= 50) {
            has_safe_alternative = true;
        }
        if (metadata->is_capture()) {
            if (alt.move.from() == origin) {
                if (exposure->net_exposure <= 80) {
                    moving_piece_can_escape = true;
                }
            } else if (!alt.is_capture() || exposure->origin_square_see <= 50) {
                if (exposure->net_exposure <= 50 && exposure->origin_square_see <= 50) {
                    another_piece_keeps_from_safe = true;
                }
            }
        }
        if (has_safe_alternative &&
            (!metadata->is_capture() || another_piece_keeps_from_safe || moving_piece_can_escape)) {
            break;
        }
    }

    if (!has_safe_alternative) {
        return false;
    }
    if (metadata->is_capture() && !another_piece_keeps_from_safe && !moving_piece_can_escape) {
        return false;
    }
    return true;
}

[[nodiscard]] std::expected<SearchLimits, std::string> build_base_limits(
    const MoveAnnotationOptions& options) {
    if (options.depth.has_value() && *options.depth <= 0) {
        return std::unexpected("annotation depth must be positive");
    }
    if (options.movetime.has_value() && options.movetime->count() < 0) {
        return std::unexpected("annotation movetime must be non-negative");
    }
    if (options.nodes.has_value() && *options.nodes == 0) {
        return std::unexpected("annotation nodes must be positive");
    }

    SearchLimits limits;
    limits.depth = options.depth;
    limits.movetime = options.movetime;
    limits.nodes = options.nodes;
    if (!limits.depth.has_value() && !limits.movetime.has_value() && !limits.nodes.has_value()) {
        limits.depth = kDefaultAnnotationDepth;
    }
    return limits;
}

[[nodiscard]] SearchOptions build_search_options(const MoveAnnotationOptions& options,
                                                 const std::size_t multi_pv) {
    SearchOptions search_options;
    search_options.threads = std::max<std::size_t>(1, options.threads);
    search_options.multi_pv = std::max<std::size_t>(1, multi_pv);
    search_options.analyse_mode = options.analyse_mode;
    search_options.contempt_cp = options.contempt_cp;
    search_options.random_seed = options.random_seed;
    search_options.syzygy = options.syzygy;
    search_options.syzygy_interior_depth = options.syzygy_interior_depth;
    search_options.interim_report_interval = std::chrono::milliseconds{0};
    return search_options;
}

struct CapturedMultiPv {
    std::mutex mutex;
    std::optional<SearchInfo> primary;
    std::optional<SearchInfo> secondary;
    std::vector<SearchInfo> by_root_move;
    SearchResult result;
    bool has_result = false;
};

[[nodiscard]] SearchResult run_single_line_search(SearchService& service, GameState root,
                                                  SearchLimits limits,
                                                  const SearchOptions& options) {
    CapturedMultiPv captured;
    SearchEventSink sink;
    sink.on_info = [&captured](const SearchInfo& info) {
        if (!info.currmove.is_no_move()) {
            return;
        }
        std::lock_guard lock(captured.mutex);
        if (info.multipv == 1 && (!captured.primary || info.depth >= captured.primary->depth)) {
            captured.primary = info;
        }
    };
    sink.on_complete = [&captured](const SearchResult& result) {
        std::lock_guard lock(captured.mutex);
        captured.result = result;
        captured.has_result = true;
    };
    SearchHandle handle = service.start(std::move(root), std::move(limits), std::move(sink), options);
    handle.wait();
    if (!captured.has_result && captured.primary.has_value()) {
        captured.result.best_move =
            captured.primary->pv.empty() ? std::nullopt : std::optional<Move>{captured.primary->pv.front()};
        captured.result.pv = captured.primary->pv;
        captured.result.score_cp = captured.primary->score_cp;
        captured.result.mate = captured.primary->mate;
        captured.result.completed_depth = captured.primary->depth;
    }
    return captured.result;
}

void accumulate_player_summary(PlayerAnnotationSummary& summary,
                               const MoveAnnotationResult& move_result,
                               double& accuracy_sum, double& cpl_sum) noexcept {
    ++summary.move_count;
    accuracy_sum += move_result.accuracy_percent;
    cpl_sum += static_cast<double>(move_result.centipawn_loss);
    switch (move_result.annotation) {
    case MoveAnnotation::brilliant: ++summary.brilliant; break;
    case MoveAnnotation::great: ++summary.great; break;
    case MoveAnnotation::best: ++summary.best; break;
    case MoveAnnotation::excellent: ++summary.excellent; break;
    case MoveAnnotation::good: ++summary.good; break;
    case MoveAnnotation::book: ++summary.book; break;
    case MoveAnnotation::forced: ++summary.forced; break;
    case MoveAnnotation::inaccuracy: ++summary.inaccuracy; break;
    case MoveAnnotation::mistake: ++summary.mistake; break;
    case MoveAnnotation::miss: ++summary.miss; break;
    case MoveAnnotation::blunder: ++summary.blunder; break;
    }
}

void finalize_player_summary(PlayerAnnotationSummary& summary,
                             const double accuracy_sum, const double cpl_sum) noexcept {
    if (summary.move_count == 0) {
        summary.average_accuracy = 100.0;
        summary.average_cpl = 0.0;
        return;
    }
    const double count = static_cast<double>(summary.move_count);
    summary.average_accuracy = accuracy_sum / count;
    summary.average_cpl = cpl_sum / count;
}

} // namespace

std::string_view move_annotation_name(const MoveAnnotation annotation) noexcept {
    return lookup_metadata(annotation).name;
}

std::string_view move_annotation_glyph(const MoveAnnotation annotation) noexcept {
    return lookup_metadata(annotation).glyph;
}

int move_annotation_nag(const MoveAnnotation annotation) noexcept {
    return lookup_metadata(annotation).nag;
}

std::optional<MoveAnnotation> parse_move_annotation(const std::string_view name) noexcept {
    for (const auto& entry : kAnnotationTable) {
        if (equals_ascii_ignore_case(entry.name, name)) {
            return entry.value;
        }
    }
    return std::nullopt;
}

double win_probability_from_cp(const int score_cp) noexcept {
    const double clamped = std::clamp(static_cast<double>(score_cp), -10'000.0, 10'000.0);
    return 1.0 / (1.0 + std::exp(-kWinProbScale * clamped));
}

double win_probability_from_eval(const MoveScoreEval& eval) noexcept {
    if (eval.mate.has_value()) {
        return *eval.mate > 0 ? 1.0 : 0.0;
    }
    return win_probability_from_cp(eval.score_cp);
}

int compute_centipawn_loss(const MoveScoreEval& best_eval,
                           const MoveScoreEval& played_eval) noexcept {
    const std::int64_t diff =
        static_cast<std::int64_t>(effective_cp(best_eval)) -
        static_cast<std::int64_t>(effective_cp(played_eval));
    return diff > 0 ? static_cast<int>(diff) : 0;
}

double move_accuracy_percent(const double win_prob_before,
                             const double win_prob_after) noexcept {
    const double clamped_before = std::clamp(win_prob_before, 0.0, 1.0);
    const double clamped_after = std::clamp(win_prob_after, 0.0, 1.0);
    const double wp_drop = std::max(0.0, clamped_before - clamped_after);
    if (wp_drop <= 1e-12) {
        return 100.0;
    }
    const double delta_percent = wp_drop * 100.0;
    const double raw = 103.1668 * std::exp(-0.04354 * delta_percent) - 3.1669;
    return std::clamp(raw, 0.0, 100.0);
}

MoveAnnotation classify_move_annotation(const MoveClassificationInput& input) noexcept {
    if (input.legal_move_count == 1) {
        return MoveAnnotation::forced;
    }

    const int cpl = compute_centipawn_loss(input.best_eval, input.played_eval);
    const double best_wp = win_probability_from_eval(input.best_eval);
    const double played_wp = win_probability_from_eval(input.played_eval);
    const double delta_wp = std::max(0.0, best_wp - played_wp);

    const bool has_winning_mate = input.best_eval.mate.has_value() && *input.best_eval.mate > 0;
    const bool played_winning_mate =
        input.played_eval.mate.has_value() && *input.played_eval.mate > 0;
    const bool played_allows_mate =
        input.played_eval.mate.has_value() && *input.played_eval.mate <= 0;

    if (input.is_book_move && !played_allows_mate && delta_wp < 0.10 &&
        (!has_winning_mate || played_winning_mate)) {
        return MoveAnnotation::book;
    }

    // 1. Tactical/severe error bands checked from worst to mildest.
    if (has_winning_mate && played_allows_mate) {
        return MoveAnnotation::blunder;
    }
    if (has_winning_mate && !played_winning_mate) {
        return played_wp < 0.60 ? MoveAnnotation::blunder : MoveAnnotation::miss;
    }
    if (!played_allows_mate && best_wp >= 0.75 && played_wp >= 0.47 &&
        delta_wp >= 0.12 && delta_wp < 0.40) {
        return MoveAnnotation::miss;
    }
    if (played_allows_mate ||
        (delta_wp >= 0.20 && played_wp < 0.45) ||
        delta_wp >= 0.30 ||
        (!has_winning_mate && best_wp < 0.75 && cpl >= 300)) {
        return MoveAnnotation::blunder;
    }
    if (delta_wp >= 0.10 || (cpl >= 100 && delta_wp >= 0.06)) {
        return MoveAnnotation::mistake;
    }
    if (delta_wp >= 0.05 || (cpl >= 50 && delta_wp >= 0.03)) {
        return MoveAnnotation::inaccuracy;
    }

    // 2. Strong/top move classifications.
    const bool exact_best_move =
        !input.best_move.is_no_move() && input.played_move == input.best_move;
    const bool equal_mate_best =
        !has_winning_mate ||
        (played_winning_mate && *input.played_eval.mate <= *input.best_eval.mate);
    const bool is_top_choice =
        exact_best_move || (cpl == 0 && delta_wp <= 0.005 && equal_mate_best);

    const bool has_second_best = input.second_best_eval.has_value();
    const double second_wp =
        has_second_best ? win_probability_from_eval(*input.second_best_eval) : 0.0;
    const double second_gap_wp =
        has_second_best ? std::max(0.0, best_wp - second_wp) : 0.0;
    const bool second_is_winning_mate =
        has_second_best && input.second_best_eval->mate.has_value() &&
        *input.second_best_eval->mate > 0;

    const bool forced_defensive_capture =
        input.is_capture_move && has_second_best && second_wp < 0.25 &&
        !played_winning_mate && !input.gives_check && best_wp < 0.80;
    const bool overwhelming_position =
        (has_second_best && second_wp > 0.80 && !played_winning_mate) ||
        (best_wp > 0.90 && (!played_winning_mate || second_is_winning_mate));

    if (input.is_sacrifice &&
        !input.in_check &&
        !forced_defensive_capture &&
        !overwhelming_position &&
        (is_top_choice || (cpl <= 10 && delta_wp <= 0.015)) &&
        played_wp >= 0.45 &&
        (!has_second_best || second_gap_wp >= 0.025)) {
        return MoveAnnotation::brilliant;
    }

    const bool unique_winning_mate =
        played_winning_mate && !second_is_winning_mate && second_wp <= 0.92;
    if (is_top_choice &&
        has_second_best &&
        ((second_gap_wp >= 0.10 && played_wp >= 0.25 && second_wp <= 0.85) ||
         unique_winning_mate)) {
        return MoveAnnotation::great;
    }

    if (is_top_choice) {
        return MoveAnnotation::best;
    }
    if (delta_wp <= 0.02 &&
        (cpl <= 25 || (best_wp >= 0.85 && delta_wp <= 0.015 && cpl <= 60))) {
        return MoveAnnotation::excellent;
    }
    return MoveAnnotation::good;
}

std::expected<MoveAnnotationResult, std::string> analyze_and_annotate_move(
    SearchService& service, const GameState& root, const Move played_move,
    const MoveAnnotationOptions& options) {
    if (played_move.is_no_move() || !root.is_legal(played_move)) {
        return std::unexpected("move is illegal in the current position");
    }

    const auto limits_expected = build_base_limits(options);
    if (!limits_expected) {
        return std::unexpected(limits_expected.error());
    }
    const SearchLimits base_limits = *limits_expected;

    const std::vector<Move> legal = root.legal_moves();
    const std::size_t legal_count = legal.size();

    CapturedMultiPv root_capture;
    SearchEventSink sink;
    sink.on_info = [&root_capture](const SearchInfo& info) {
        if (!info.currmove.is_no_move()) {
            return;
        }
        std::lock_guard lock(root_capture.mutex);
        if (info.multipv == 1 &&
            (!root_capture.primary || info.depth >= root_capture.primary->depth)) {
            root_capture.primary = info;
        } else if (info.multipv == 2 &&
                   (!root_capture.secondary || info.depth >= root_capture.secondary->depth)) {
            root_capture.secondary = info;
        }
        if (!info.pv.empty()) {
            const Move root_move = info.pv.front();
            auto existing = std::find_if(
                root_capture.by_root_move.begin(), root_capture.by_root_move.end(),
                [root_move](const SearchInfo& entry) {
                    return !entry.pv.empty() && entry.pv.front() == root_move;
                });
            if (existing == root_capture.by_root_move.end()) {
                root_capture.by_root_move.push_back(info);
            } else if (info.depth >= existing->depth) {
                *existing = info;
            }
        }
    };
    sink.on_complete = [&root_capture](const SearchResult& result) {
        std::lock_guard lock(root_capture.mutex);
        root_capture.result = result;
        root_capture.has_result = true;
    };

    const std::size_t requested_multipv = std::min<std::size_t>(2, legal_count);
    SearchHandle handle = service.start(
        root, base_limits, std::move(sink), build_search_options(options, requested_multipv));
    handle.wait();

    if (root_capture.result.failed && !root_capture.result.best_move.has_value()) {
        return std::unexpected("search failed while annotating position");
    }

    const Move best_move =
        root_capture.result.best_move.value_or(
            root_capture.primary && !root_capture.primary->pv.empty()
                ? root_capture.primary->pv.front()
                : played_move);
    const MoveScoreEval best_eval{root_capture.result.score_cp, root_capture.result.mate};
    const int completed_depth =
        root_capture.result.completed_depth > 0
            ? root_capture.result.completed_depth
            : (root_capture.primary ? root_capture.primary->depth : 0);

    std::optional<Move> second_best_move;
    std::optional<MoveScoreEval> second_best_eval;
    std::vector<Move> second_pv;
    if (legal_count >= 2 && root_capture.secondary.has_value() &&
        !root_capture.secondary->pv.empty() &&
        root_capture.secondary->depth >= completed_depth &&
        root_capture.secondary->pv.front() != best_move) {
        second_best_move = root_capture.secondary->pv.front();
        second_best_eval = MoveScoreEval{
            root_capture.secondary->score_cp,
            root_capture.secondary->mate,
        };
        second_pv = root_capture.secondary->pv;
    } else if (legal_count >= 2) {
        const SearchInfo* runner_up = nullptr;
        int runner_up_cp = std::numeric_limits<int>::min();
        for (const SearchInfo& candidate : root_capture.by_root_move) {
            if (candidate.pv.empty() || candidate.pv.front() == best_move ||
                candidate.depth + 1 < completed_depth) {
                continue;
            }
            const int candidate_cp =
                effective_cp(MoveScoreEval{candidate.score_cp, candidate.mate});
            if (runner_up == nullptr ||
                candidate.depth > runner_up->depth ||
                (candidate.depth == runner_up->depth && candidate_cp > runner_up_cp)) {
                runner_up = &candidate;
                runner_up_cp = candidate_cp;
            }
        }
        if (runner_up != nullptr) {
            second_best_move = runner_up->pv.front();
            second_best_eval = MoveScoreEval{runner_up->score_cp, runner_up->mate};
            second_pv = runner_up->pv;
        }
    }

    MoveScoreEval played_eval = best_eval;
    std::vector<Move> played_pv = root_capture.result.pv;
    if (played_move == best_move) {
        played_eval = best_eval;
        played_pv = root_capture.result.pv;
    } else if (second_best_move.has_value() && *second_best_move == played_move &&
               second_best_eval.has_value()) {
        played_eval = *second_best_eval;
        played_pv = second_pv;
    } else {
        SearchLimits restricted_limits = base_limits;
        restricted_limits.search_moves_specified = true;
        restricted_limits.search_moves = {played_move};
        const SearchResult played_result = run_single_line_search(
            service, root, std::move(restricted_limits), build_search_options(options, 1));
        if (played_result.failed && !played_result.best_move.has_value()) {
            return std::unexpected("search failed while evaluating played move");
        }
        played_eval = MoveScoreEval{played_result.score_cp, played_result.mate};
        played_pv = played_result.pv;
    }

    const std::uint32_t root_ply = compute_root_ply(root);
    const auto metadata = root.describe_move(played_move);
    const bool in_check = root.in_check();
    const bool is_capture_move = metadata.has_value() && metadata->is_capture();
    const bool gives_check = metadata.has_value() && metadata->gives_check;
    const bool is_book_move =
        options.is_book_move ||
        (options.opening_book != nullptr && options.book_enabled &&
         options.opening_book->contains_move(
             root, played_move, root_ply, true, options.book_depth, options.book_safety,
             options.book_safety_depth));
    const bool is_sacrifice =
        legal_count > 1 && !is_book_move && !in_check &&
        is_sound_sacrifice_candidate(root, played_move);

    MoveClassificationInput input;
    input.played_move = played_move;
    input.best_move = best_move;
    input.best_eval = best_eval;
    input.played_eval = played_eval;
    input.second_best_eval = second_best_eval;
    input.legal_move_count = legal_count;
    input.is_book_move = is_book_move;
    input.is_sacrifice = is_sacrifice;
    input.is_capture_move = is_capture_move;
    input.in_check = in_check;
    input.gives_check = gives_check;

    MoveAnnotationResult output;
    output.ply = root_ply;
    output.side_to_move = root.side_to_move();
    output.played_move = played_move;
    output.best_move = best_move;
    output.best_eval = best_eval;
    output.played_eval = played_eval;
    output.second_best_eval = second_best_eval;
    output.centipawn_loss = compute_centipawn_loss(best_eval, played_eval);
    output.win_prob_before = win_probability_from_eval(best_eval);
    output.win_prob_after = win_probability_from_eval(played_eval);
    output.accuracy_percent =
        move_accuracy_percent(output.win_prob_before, output.win_prob_after);
    output.annotation = classify_move_annotation(input);
    if (output.annotation == MoveAnnotation::book ||
        output.annotation == MoveAnnotation::forced) {
        output.accuracy_percent = 100.0;
        if (output.annotation == MoveAnnotation::forced) {
            output.centipawn_loss = 0;
        }
    }
    output.depth = completed_depth;
    output.best_pv = root_capture.result.pv;
    output.played_pv = std::move(played_pv);
    output.is_sacrifice = is_sacrifice;
    output.is_capture_move = is_capture_move;
    output.in_check = in_check;
    output.gives_check = gives_check;
    return output;
}

std::expected<GameAnnotationResult, std::string> annotate_game(
    SearchService& service, const GameState& initial_position, const std::span<const Move> moves,
    const MoveAnnotationOptions& options) {
    const auto limits_expected = build_base_limits(options);
    if (!limits_expected) {
        return std::unexpected(limits_expected.error());
    }

    GameAnnotationResult game_result;
    game_result.moves.reserve(moves.size());
    GameState cursor = initial_position;

    double white_accuracy_sum = 0.0;
    double white_cpl_sum = 0.0;
    double black_accuracy_sum = 0.0;
    double black_cpl_sum = 0.0;

    MoveAnnotationOptions ply_options = options;
    ply_options.is_book_move = false;
    int consecutive_non_book = 0;

    for (std::size_t index = 0; index < moves.size(); ++index) {
        const Move move = moves[index];
        if (move.is_no_move() || !cursor.is_legal(move)) {
            return std::unexpected(
                "illegal move at ply " + std::to_string(index) + ": " + move.uci());
        }
        auto annotated = analyze_and_annotate_move(service, cursor, move, ply_options);
        if (!annotated) {
            return std::unexpected(annotated.error());
        }
        if (ply_options.book_enabled) {
            if (annotated->annotation == MoveAnnotation::book) {
                consecutive_non_book = 0;
            } else {
                ++consecutive_non_book;
                if (consecutive_non_book >= 2) {
                    ply_options.book_enabled = false;
                }
            }
        }
        if (annotated->side_to_move == Color::white) {
            accumulate_player_summary(
                game_result.white, *annotated, white_accuracy_sum, white_cpl_sum);
        } else {
            accumulate_player_summary(
                game_result.black, *annotated, black_accuracy_sum, black_cpl_sum);
        }
        game_result.moves.push_back(std::move(*annotated));
        if (!cursor.make_move(move)) {
            return std::unexpected(
                "failed to advance game state at ply " + std::to_string(index));
        }
    }

    finalize_player_summary(game_result.white, white_accuracy_sum, white_cpl_sum);
    finalize_player_summary(game_result.black, black_accuracy_sum, black_cpl_sum);
    return game_result;
}

} // namespace koi
