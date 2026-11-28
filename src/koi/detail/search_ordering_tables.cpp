#include "koi/detail/search_ordering_tables.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace koi::detail {
namespace {

// All history tables share Stockfish's fixed-point scale so the LMR bands in
// search_policy (|history| > 512, and the >= 128 high-history exclusion) and the
// ordering tiers stay meaningful.  The main table once clamped at the
// counter-move priority (~300k), which made gravity negligible: scores rapidly
// saturated in the thousands, most good quiet moves were excluded from LMR, and
// quiet ordering was distorted.  Keep every table on the same 16,384 scale.
constexpr int kHistoryTableMaximum = 16'384;
constexpr int kMaximumHistoryScore = kHistoryTableMaximum;
constexpr int kCaptureHistoryMaximum = 16'384;
constexpr int kCounterMoveMinimumConfidence = 7 * 7;

// These priors match Stockfish 19's initial history bias and seed capture,
// piece-to, low-ply, continuation, and pawn histories on worker construction
// so unvisited moves carry that calibrated baseline (unlike `clear()`, which
// resets tables to zero for diagnostics). Score helpers return the sums
// directly without subtracting the priors.
constexpr int kInitialCaptureHistory = -742;
constexpr int kInitialPieceToHistory = -586;
constexpr int kInitialContinuationHistory = -586;
constexpr int kInitialPawnHistory = -1338;
constexpr int kInitialLowPlyHistory = 102;

int color_index(const Color color) noexcept {
    return color == Color::white ? 0 : 1;
}

int clamp_history(const std::int64_t value, const int maximum) noexcept {
    return static_cast<int>(std::clamp<std::int64_t>(value, -maximum, maximum));
}


std::uint64_t mix64(const std::uint64_t value) noexcept {
    return ordering_mix64(value);
}

template <typename Entry>
void update_gravity_entry(Entry& score, const int delta, const int maximum) noexcept {
    const std::int64_t wide_score = score;
    const std::int64_t wide_delta = delta;
    const std::int64_t wide_abs_delta = wide_delta < 0 ? -wide_delta : wide_delta;
    const std::int64_t updated = wide_score + wide_delta -
        (wide_score * wide_abs_delta) / std::max(1, maximum);
    score = static_cast<Entry>(clamp_history(updated, maximum));
}

} // namespace

SearchOrderingTables::SearchOrderingTables()
    : large_(std::make_unique<LargeTables>()) {
    // `clear()` intentionally retains its historical zeroed-reset contract
    // for diagnostics.  A newly created worker, however, should begin with
    // the calibrated Stockfish-style priors rather than indeterminate heap
    // contents.
    capture_history_.fill(kInitialCaptureHistory);
    piece_to_history_.fill(kInitialPieceToHistory);
    low_ply_history_.fill(kInitialLowPlyHistory);
    continuation_history_.fill(kInitialContinuationHistory);
    large_->multi_ply_continuation_history.fill(kInitialContinuationHistory);
    large_->pawn_history.fill(kInitialPawnHistory);
    // Correction tables start neutral: the first visit to a key sees the raw
    // evaluator score until the search itself supplies evidence.
    large_->pawn_correction.fill(0);
    large_->material_correction.fill(0);
    large_->king_correction.fill(0);
    large_->minor_correction.fill(0);
    large_->major_correction.fill(0);
    large_->continuation_correction.fill(0);
}

SearchOrderingTables::~SearchOrderingTables() = default;

int SearchOrderingTables::piece_index(const PieceType type) noexcept {
    const int index = static_cast<int>(type);
    return index >= 0 && index < static_cast<int>(kPieceTypeCount) ? index : 0;
}

std::size_t SearchOrderingTables::move_index(const Move move) noexcept {
    return table_move_index(move);
}

std::size_t SearchOrderingTables::capture_index(const MoveMetadata& metadata) noexcept {
    const std::size_t piece = static_cast<std::size_t>(piece_index(metadata.moving_piece));
    const std::size_t to = metadata.move.to().index() < Square::kInvalid ?
        static_cast<std::size_t>(metadata.move.to().index()) : 0U;
    const std::size_t captured = static_cast<std::size_t>(piece_index(metadata.captured_piece));
    return (piece * 64U + to) * kCapturedPieceCount + captured;
}

std::size_t SearchOrderingTables::piece_to_index(const PieceType piece,
                                                 const Square to) noexcept {
    const std::size_t square = to.index() < Square::kInvalid ?
        static_cast<std::size_t>(to.index()) : 0U;
    return static_cast<std::size_t>(piece_index(piece)) * 64U + square;
}

std::size_t SearchOrderingTables::continuation_index(const Move previous_move,
                                                     const Move move) noexcept {
    // Keep the original hash shape.  A few diagnostics intentionally exercise
    // its collisions, while the exact counter table below remains collision-free.
    constexpr std::size_t mask = kContinuationHistorySize - 1;
    return (move_index(previous_move) * 131U + move_index(move) * 17U) & mask;
}

std::size_t SearchOrderingTables::pawn_index(const SearchHistoryContext& context,
                                             const PieceType piece,
                                             const Square to) noexcept {
    const std::uint64_t dimensions =
        (static_cast<std::uint64_t>(piece_index(piece)) << 8U) |
        static_cast<std::uint64_t>(to.index() < Square::kInvalid ? to.index() : 0U);
    return static_cast<std::size_t>(mix64(context.pawn_key ^ dimensions) &
                                    (kPawnHistorySize - 1));
}

int SearchOrderingTables::normalized_ply(const int ply) noexcept {
    return std::clamp(ply, 0, kMaximumKillerPly - 1);
}

void SearchOrderingTables::update_history(int& score, const int delta,
                                           const int maximum) noexcept {
    update_gravity_entry(score, delta, maximum);
}

void SearchOrderingTables::update_history(std::int16_t& score, const int delta,
                                           const int maximum) noexcept {
    update_gravity_entry(score, delta, maximum);
}

void SearchOrderingTables::clear() noexcept {
    killers_ = {};
    history_ = {};
    counter_moves_ = {};
    counter_confidence_ = {};
    capture_history_ = {};
    piece_to_history_ = {};
    low_ply_history_ = {};
    continuation_history_ = {};
    if (large_ != nullptr) {
        large_->multi_ply_continuation_history.fill(0);
        large_->pawn_history.fill(0);
        large_->pawn_correction.fill(0);
        large_->material_correction.fill(0);
        large_->king_correction.fill(0);
        large_->minor_correction.fill(0);
        large_->major_correction.fill(0);
        large_->continuation_correction.fill(0);
    }
}

int SearchOrderingTables::killer_rank(const Move move, const int ply) const noexcept {
    const auto& killers = killers_[static_cast<std::size_t>(normalized_ply(ply))];
    if (killers[0] == move) {
        return 2;
    }
    if (killers[1] == move) {
        return 1;
    }
    return 0;
}

bool SearchOrderingTables::is_killer(const Move move, const int ply) const noexcept {
    return killer_rank(move, ply) != 0;
}

int SearchOrderingTables::quiet_history_score(
    const Color side, const Move move, const std::optional<Move> previous_move) const noexcept {
    if (move.is_no_move()) {
        return 0;
    }
    int score = history_[static_cast<std::size_t>(color_index(side))][move_index(move)];
    if (previous_move.has_value() && !previous_move->is_no_move()) {
        score += continuation_history_[continuation_index(*previous_move, move)];
    }
    return std::clamp(score, -kMaximumHistoryScore, kMaximumHistoryScore);
}

std::int64_t SearchOrderingTables::continuation_history_raw(
    const std::size_t move_idx, const SearchHistoryContext& context) const noexcept {
    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    if (move_count == 0) {
        return 0;
    }
    const std::size_t scaled_move = move_idx * 17U;
    constexpr std::size_t kMask = kContinuationHistorySize - 1U;
    const auto slot_index = [&](const std::size_t distance) noexcept -> std::size_t {
        const std::uint32_t cached = context.continuation_previous_scaled[distance];
        if (cached != 0U) {
            return (static_cast<std::size_t>(cached) + scaled_move) & kMask;
        }
        const Move previous = context.continuation_moves[distance];
        return previous.is_no_move() ? kContinuationHistorySize
                                     : ((table_move_index(previous) * 131U + scaled_move) & kMask);
    };
    std::int64_t cont_score = 0;
    if (const std::size_t pair0 = slot_index(0); pair0 < kContinuationHistorySize) {
        cont_score += continuation_history_[pair0];
    }
    auto add_ply = [&](const std::size_t distance, const int divisor) noexcept {
        if (const std::size_t pair = slot_index(distance); pair < kContinuationHistorySize) {
            cont_score += large_->multi_ply_continuation_history[
                distance * kContinuationHistorySize + pair] / divisor;
        }
    };
    if (move_count <= 1) {
        return cont_score;
    }
    add_ply(1, 2);
    if (move_count <= 2) {
        return cont_score;
    }
    add_ply(2, 3);
    if (move_count <= 3) {
        return cont_score;
    }
    add_ply(3, 4);
    if (move_count <= 4) {
        return cont_score;
    }
    add_ply(4, 5);
    if (move_count <= 5) {
        return cont_score;
    }
    add_ply(5, 6);
    return cont_score;
}

QuietAndContinuationScores SearchOrderingTables::quiet_and_continuation_scores(
    const Color side, const MoveMetadata& metadata,
    const SearchHistoryContext& context) const noexcept {
    if (metadata.move.is_no_move()) {
        return {};
    }

    const std::size_t move_idx = move_index(metadata.move);
    const std::int64_t cont_score = continuation_history_raw(move_idx, context);

    const int continuation = clamp_history(cont_score, kMaximumHistoryScore);
    if (metadata.is_capture()) {
        return QuietAndContinuationScores{0, continuation};
    }

    const std::size_t side_index = static_cast<std::size_t>(color_index(side));
    std::int64_t quiet_score = history_[side_index][move_idx];
    quiet_score += piece_to_history_[piece_to_index(metadata.moving_piece, metadata.move.to())];
    if (context.ply < static_cast<int>(kLowPlyHistoryPlies)) {
        quiet_score += low_ply_history_[static_cast<std::size_t>(context.ply) * kMoveTableSize +
                                        move_idx];
    }
    quiet_score += cont_score;
    quiet_score += large_->pawn_history[pawn_index(context, metadata.moving_piece, metadata.move.to())] / 2;
    return QuietAndContinuationScores{
        clamp_history(quiet_score, kMaximumHistoryScore), continuation};
}

int SearchOrderingTables::quiet_history_score(
    const Color side, const MoveMetadata& metadata,
    const SearchHistoryContext& context) const noexcept {
    if (metadata.move.is_no_move()) {
        return 0;
    }
    const std::size_t move_idx = move_index(metadata.move);
    const std::size_t side_index = static_cast<std::size_t>(color_index(side));
    std::int64_t score = history_[side_index][move_idx];
    score += piece_to_history_[piece_to_index(metadata.moving_piece, metadata.move.to())];
    if (context.ply < static_cast<int>(kLowPlyHistoryPlies)) {
        score += low_ply_history_[static_cast<std::size_t>(context.ply) * kMoveTableSize +
                                  move_idx];
    }
    score += continuation_history_raw(move_idx, context);
    score += large_->pawn_history[pawn_index(context, metadata.moving_piece, metadata.move.to())] / 2;
    return clamp_history(score, kMaximumHistoryScore);
}

int SearchOrderingTables::continuation_history_score(
    const MoveMetadata& metadata, const SearchHistoryContext& context) const noexcept {
    if (metadata.move.is_no_move()) {
        return 0;
    }
    return clamp_history(continuation_history_raw(move_index(metadata.move), context),
                         kMaximumHistoryScore);
}

int SearchOrderingTables::capture_history_score(const MoveMetadata& metadata) const noexcept {
    if (!metadata.is_capture()) {
        return 0;
    }
    return capture_history_[capture_index(metadata)];
}

SearchOrderingTables::QuietHistoryBatch SearchOrderingTables::prepare_quiet_history_batch(
    const Color side, const SearchHistoryContext& context) const noexcept {
    static_assert(QuietHistoryBatch::kContSize == kContinuationHistorySize);
    static_assert(QuietHistoryBatch::kPawnMask == kPawnHistorySize - 1U);
    static_assert(QuietHistoryBatch::kHistoryMax == kMaximumHistoryScore);
    QuietHistoryBatch batch;
    batch.history_row = history_[static_cast<std::size_t>(color_index(side))].data();
    batch.piece_to_row = piece_to_history_.data();
    batch.low_ply_row = (context.ply >= 0 && context.ply < static_cast<int>(kLowPlyHistoryPlies)) ?
        low_ply_history_.data() + static_cast<std::size_t>(context.ply) * kMoveTableSize :
        nullptr;
    batch.pawn_row = large_->pawn_history.data();
    batch.cont0 = continuation_history_.data();
    batch.multi_cont = large_->multi_ply_continuation_history.data();
    batch.pawn_key = context.pawn_key;
    batch.cont_prev.fill(QuietHistoryBatch::kInvalidOffset);
    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    std::uint8_t cont_mask = 0U;
    for (std::size_t distance = 0; distance < move_count; ++distance) {
        const std::uint32_t cached = context.continuation_previous_scaled[distance];
        if (cached != 0U) {
            batch.cont_prev[distance] =
                static_cast<std::uint16_t>(cached & (kContinuationHistorySize - 1U));
            cont_mask |= static_cast<std::uint8_t>(1U << distance);
        } else if (!context.continuation_moves[distance].is_no_move()) {
            batch.cont_prev[distance] = static_cast<std::uint16_t>(
                (table_move_index(context.continuation_moves[distance]) * 131U) &
                (kContinuationHistorySize - 1U));
            cont_mask |= static_cast<std::uint8_t>(1U << distance);
        }
    }
    batch.all_cont = cont_mask == 0x3FU;
    return batch;
}

Move SearchOrderingTables::proven_counter_move(
    const Color side, const Move previous_move) const noexcept {
    if (previous_move.is_no_move()) {
        return Move::no_move();
    }
    const std::size_t side_index = static_cast<std::size_t>(color_index(side));
    const std::size_t previous_index = move_index(previous_move);
    if (counter_confidence_[side_index][previous_index] < kCounterMoveMinimumConfidence) {
        return Move::no_move();
    }
    return counter_moves_[side_index][previous_index];
}

bool SearchOrderingTables::is_proven_counter_move(
    const Color side, const Move previous_move, const Move move) const noexcept {
    if (previous_move.is_no_move() || move.is_no_move()) {
        return false;
    }
    const std::size_t side_index = static_cast<std::size_t>(color_index(side));
    const std::size_t previous_index = move_index(previous_move);
    return counter_moves_[side_index][previous_index] == move &&
        counter_confidence_[side_index][previous_index] >= kCounterMoveMinimumConfidence;
}

void SearchOrderingTables::update_correction(const std::uint64_t pawn_key,
                                             const std::uint64_t material_key,
                                             const std::uint64_t king_key,
                                             const int bonus) noexcept {
    if (bonus == 0) {
        return;
    }
    update_history(large_->pawn_correction[correction_slot(pawn_key, kPawnCorrectionHistorySize)],
                   bonus, kCorrectionEntryLimit);
    update_history(large_->material_correction[correction_slot(material_key, kMaterialCorrectionHistorySize)],
                   bonus, kCorrectionEntryLimit);
    update_history(large_->king_correction[correction_slot(king_key, kKingCorrectionHistorySize)],
                   bonus, kCorrectionEntryLimit);
}

void SearchOrderingTables::update_correction(const std::uint64_t pawn_key,
                                             const std::uint64_t material_key,
                                             const std::uint64_t king_key,
                                             const std::uint64_t minor_key,
                                             const std::uint64_t major_key,
                                             const int bonus,
                                             const std::uint64_t continuation_key,
                                             const bool has_continuation) noexcept {
    if (bonus == 0) {
        return;
    }
    update_correction(pawn_key, material_key, king_key, bonus);
    update_history(large_->minor_correction[correction_slot(minor_key, kMinorCorrectionHistorySize)],
                   bonus, kCorrectionEntryLimit);
    update_history(large_->major_correction[correction_slot(major_key, kMajorCorrectionHistorySize)],
                   bonus, kCorrectionEntryLimit);
    if (has_continuation) {
        update_history(
            large_->continuation_correction[correction_slot(
                continuation_key, kContinuationCorrectionHistorySize)],
            bonus, kCorrectionEntryLimit);
    }
}

void SearchOrderingTables::record_quiet_cutoff(
    const Color side, const Move move, const int ply, const int depth,
    const std::optional<Move> previous_move) noexcept {
    if (move.is_no_move() || move.promotion() != Promotion::none) {
        return;
    }

    auto& killers = killers_[static_cast<std::size_t>(normalized_ply(ply))];
    if (killers[0] != move) {
        killers[1] = killers[0];
        killers[0] = move;
    }

    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int bonus = depth_bonus * depth_bonus;
    std::int16_t& history = history_[static_cast<std::size_t>(color_index(side))][move_index(move)];
    update_history(history, bonus, kMaximumHistoryScore);
    if (previous_move.has_value() && !previous_move->is_no_move()) {
        const std::size_t side_index = static_cast<std::size_t>(color_index(side));
        const std::size_t previous_index = move_index(*previous_move);
        Move& counter_move = counter_moves_[side_index][previous_index];
        std::int16_t& counter_confidence = counter_confidence_[side_index][previous_index];
        if (counter_move != move) {
            counter_move = move;
            counter_confidence = 0;
        }
        update_history(counter_confidence, bonus, kMaximumHistoryScore);
        update_history(continuation_history_[continuation_index(*previous_move, move)],
                       bonus * 2, kHistoryTableMaximum);
    }
}

void SearchOrderingTables::record_quiet_cutoff(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& context) noexcept {
    if (metadata.move.is_no_move() || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none) {
        return;
    }

    record_quiet_cutoff(side, metadata.move, context.ply, depth,
                        context.count > 0 ? std::optional<Move>{context.continuation_moves[0]} :
                                            std::nullopt);
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int bonus = depth_bonus * depth_bonus;
    update_history(piece_to_history_[piece_to_index(metadata.moving_piece, metadata.move.to())],
                   bonus, kHistoryTableMaximum);
    if (context.ply < static_cast<int>(kLowPlyHistoryPlies)) {
        update_history(low_ply_history_[static_cast<std::size_t>(context.ply) * kMoveTableSize +
                                        move_index(metadata.move)], bonus,
                       kHistoryTableMaximum);
    }

    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    for (std::size_t distance = 1; distance < move_count; ++distance) {
        const Move previous = context.continuation_moves[distance];
        if (previous.is_no_move()) {
            continue;
        }
        update_history(large_->multi_ply_continuation_history[
                           distance * kContinuationHistorySize +
                           continuation_index(previous, metadata.move)],
                       bonus, kHistoryTableMaximum);
    }
    update_history(large_->pawn_history[pawn_index(context, metadata.moving_piece, metadata.move.to())],
                   bonus, kHistoryTableMaximum);
}

void SearchOrderingTables::record_scaled_multi_ply_continuation(
    const Move move, const SearchHistoryContext& context,
    const int base_bonus) noexcept {
    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    auto update_at = [&](const std::size_t distance, const int divisor) noexcept {
        const Move previous = context.continuation_moves[distance];
        if (!previous.is_no_move()) {
            update_history(large_->multi_ply_continuation_history[
                               distance * kContinuationHistorySize +
                               continuation_index(previous, move)],
                           std::max(1, base_bonus / divisor), kHistoryTableMaximum);
        }
    };
    // Constant divisors keep compiler strength-reduction bit-identical to a loop.
    if (move_count <= 1) {
        return;
    }
    update_at(1, 2);
    if (move_count <= 2) {
        return;
    }
    update_at(2, 3);
    if (move_count <= 3) {
        return;
    }
    update_at(3, 4);
    if (move_count <= 4) {
        return;
    }
    update_at(4, 5);
    if (move_count <= 5) {
        return;
    }
    update_at(5, 6);
}

void SearchOrderingTables::record_quiet_best(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& context) noexcept {
    // A best move that did not cut off still deserves a small positive update;
    // this is the quiet analogue of Stockfish's quiet-best history.
    if (metadata.move.is_no_move() || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none) {
        return;
    }
    const int bonus = std::max(1, std::clamp(depth, 1, kMaximumPly));
    std::int16_t& history = history_[static_cast<std::size_t>(color_index(side))][move_index(metadata.move)];
    update_history(history, bonus, kMaximumHistoryScore);
    update_history(piece_to_history_[piece_to_index(metadata.moving_piece, metadata.move.to())],
                   bonus, kHistoryTableMaximum);
    if (context.ply < static_cast<int>(kLowPlyHistoryPlies)) {
        update_history(low_ply_history_[static_cast<std::size_t>(context.ply) * kMoveTableSize +
                                        move_index(metadata.move)],
                       std::max(1, bonus * 3 / 4), kHistoryTableMaximum);
    }
    if (context.count > 0 && !context.continuation_moves[0].is_no_move()) {
        update_history(continuation_history_[continuation_index(
                           context.continuation_moves[0], metadata.move)],
                       std::max(1, bonus * 3 / 4), kHistoryTableMaximum);
    }
    record_scaled_multi_ply_continuation(metadata.move, context, bonus);
    update_history(large_->pawn_history[pawn_index(context, metadata.moving_piece, metadata.move.to())],
                   bonus, kHistoryTableMaximum);
}

void SearchOrderingTables::record_quiet_fail(
    const Color side, const Move move, const int depth,
    const std::optional<Move> previous_move) noexcept {
    if (move.is_no_move() || move.promotion() != Promotion::none) {
        return;
    }
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int malus = -(depth_bonus * depth_bonus);
    update_history(history_[static_cast<std::size_t>(color_index(side))][move_index(move)],
                   malus, kMaximumHistoryScore);
    if (previous_move.has_value() && !previous_move->is_no_move()) {
        const std::size_t side_index = static_cast<std::size_t>(color_index(side));
        const std::size_t previous_index = move_index(*previous_move);
        if (counter_moves_[side_index][previous_index] == move) {
            counter_confidence_[side_index][previous_index] = 0;
        }
        update_history(continuation_history_[continuation_index(*previous_move, move)],
                       malus, kHistoryTableMaximum);
    }
}

void SearchOrderingTables::record_quiet_fail(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& context) noexcept {
    if (metadata.move.is_no_move() || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none) {
        return;
    }
    record_quiet_fail(side, metadata.move, depth,
                      context.count > 0 ? std::optional<Move>{context.continuation_moves[0]} :
                                          std::nullopt);
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int malus = -(depth_bonus * depth_bonus);
    update_history(piece_to_history_[piece_to_index(metadata.moving_piece, metadata.move.to())],
                   malus, kHistoryTableMaximum);
    if (context.ply < static_cast<int>(kLowPlyHistoryPlies)) {
        update_history(low_ply_history_[static_cast<std::size_t>(context.ply) * kMoveTableSize +
                                        move_index(metadata.move)], malus,
                       kHistoryTableMaximum);
    }

    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    for (std::size_t distance = 1; distance < move_count; ++distance) {
        const Move previous = context.continuation_moves[distance];
        if (previous.is_no_move()) {
            continue;
        }
        update_history(large_->multi_ply_continuation_history[
                           distance * kContinuationHistorySize +
                           continuation_index(previous, metadata.move)],
                       malus, kHistoryTableMaximum);
    }
    update_history(large_->pawn_history[pawn_index(context, metadata.moving_piece, metadata.move.to())],
                   malus, kHistoryTableMaximum);
}

void SearchOrderingTables::record_capture_cutoff(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& context) noexcept {
    if (!metadata.is_capture()) {
        return;
    }
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    update_history(capture_history_[capture_index(metadata)], depth_bonus * depth_bonus,
                   kCaptureHistoryMaximum);
    // Captures also provide useful continuation information, but keep this
    // update weaker than the quiet history updates so tactical noise cannot
    // permanently dominate a quiet move ordering table.
    if (context.count > 0 && !context.continuation_moves[0].is_no_move()) {
        update_history(continuation_history_[continuation_index(
                           context.continuation_moves[0], metadata.move)],
                       depth_bonus, kHistoryTableMaximum);
    }
    record_scaled_multi_ply_continuation(metadata.move, context, depth_bonus);
}

void SearchOrderingTables::record_capture_best(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& /*context*/) noexcept {
    if (!metadata.is_capture()) {
        return;
    }
    update_history(capture_history_[capture_index(metadata)],
                   std::max(1, std::clamp(depth, 1, kMaximumPly)),
                   kCaptureHistoryMaximum);
}

void SearchOrderingTables::record_capture_fail(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& /*context*/) noexcept {
    if (!metadata.is_capture()) {
        return;
    }
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    update_history(capture_history_[capture_index(metadata)], -(depth_bonus * depth_bonus),
                   kCaptureHistoryMaximum);
}

void SearchOrderingTables::update_parent_histories(
    const Color side, const Move move, const PieceType piece, const int parent_ply,
    const SearchHistoryContext& context, const int bonus, const bool continuation_only) noexcept {
    if (move.is_no_move() || move.promotion() != Promotion::none || bonus == 0) {
        return;
    }
    if (!continuation_only) {
        update_history(history_[static_cast<std::size_t>(color_index(side))][move_index(move)],
                       bonus, kMaximumHistoryScore);
        update_history(piece_to_history_[piece_to_index(piece, move.to())], bonus,
                       kHistoryTableMaximum);
        if (parent_ply >= 0 && parent_ply < static_cast<int>(kLowPlyHistoryPlies)) {
            update_history(low_ply_history_[static_cast<std::size_t>(parent_ply) * kMoveTableSize +
                                            move_index(move)],
                           bonus, kHistoryTableMaximum);
        }
    }

    // The move being updated is the immediate predecessor of the node that
    // supplied `context`, so its own continuation pairs are one ply older than
    // the current node's context. Mirror the weights used by
    // record_quiet_cutoff(): the distance-zero pair is the compact table above,
    // and the multi-ply table starts at the second predecessor.
    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    if (move_count > 1 && !context.continuation_moves[1].is_no_move()) {
        update_history(continuation_history_[continuation_index(
                           context.continuation_moves[1], move)],
                       bonus * 2, kHistoryTableMaximum);
    }
    for (std::size_t distance = 1; distance + 1 < move_count; ++distance) {
        const Move previous = context.continuation_moves[distance + 1];
        if (previous.is_no_move()) {
            continue;
        }
        update_history(large_->multi_ply_continuation_history[
                           distance * kContinuationHistorySize +
                           continuation_index(previous, move)],
                       bonus, kHistoryTableMaximum);
    }
}

void SearchOrderingTables::record_parent_fail_low(
    const Color side, const Move move, const PieceType piece, const int parent_ply,
    const SearchHistoryContext& context, const int depth) noexcept {
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    // Indirect evidence, so half the weight of a direct cutoff at this depth.
    update_parent_histories(side, move, piece, parent_ply, context,
                            std::max(1, depth_bonus * depth_bonus / 2), false);
}

void SearchOrderingTables::record_parent_refuted(
    const Color side, const Move move, const PieceType piece, const int parent_ply,
    const SearchHistoryContext& context, const int depth) noexcept {
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    // Stockfish penalizes only the continuation histories of a refuted early
    // quiet move; the move's own failure malus is recorded at its parent node.
    update_parent_histories(side, move, piece, parent_ply, context,
                            -std::max(1, depth_bonus * depth_bonus / 2), true);
}

} // namespace koi::detail
