#include "koi/detail/search_ordering.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "koi/detail/no_init_array.hpp"
#include "koi/detail/static_exchange.hpp"
#include "koi/piece_values.hpp"

namespace koi::detail {
namespace {

constexpr int kCapturePriority = 500'000;
constexpr int kPromotionPriority = 400'000;
constexpr int kCheckingMovePriority = 350'000;
constexpr int kKillerPriority = 300'000;
constexpr int kCounterMovePriority = kKillerPriority - 1;
constexpr int kSeeOrderingWeight = 12;
// Quiet history is a fixed-point score clamped to +/-16,384, the same scale as
// the LMR bands. This threshold leaves initially neutral quiets in the good
// stage, while allowing repeatedly failing quiets to move behind bad captures.
constexpr int kGoodQuietPriorityThreshold = -4'096;
constexpr int kBadCapturePenalty = 210'000;
constexpr int kCaptureHistoryWeight = 2;

int piece_value(const PieceType type) noexcept {
    // Shared material table. A king can appear here as the *attacker* of a
    // capture delta, and Koi's ordering has always treated a king attacker as
    // weightless, so the king maps to 0 instead of kKingMaterialValue.
    // Kings can never be captured, so the victim side is unaffected.
    return type == PieceType::king ? 0 : piece_material_value(type);
}

int promotion_value(const Promotion promotion) noexcept {
    switch (promotion) {
    case Promotion::queen:
        return 900;
    case Promotion::rook:
        return 500;
    case Promotion::bishop:
        return 330;
    case Promotion::knight:
        return 320;
    case Promotion::none:
        return 0;
    }
    return 0;
}

// Ascending 64-bit packed candidate record:
// - bits [63..32]: inverted sign-flipped priority (priority descending)
// - bits [31..9]:  inj. move_tie_break_key (max 20,804 < 2^15, fits in 23 bits)
// - bits [8..0]:   source_index in `moves_` (< 512)
static_assert(((64U * 64U + 64U) * 5U + 4U) < (1ULL << 23U),
              "tie_break key must fit in bits [31..9]");

[[nodiscard]] constexpr std::uint64_t pack_candidate(
    const int priority, const std::uint32_t tie_break, const std::size_t index) noexcept {
    const std::uint32_t inverted = ~(static_cast<std::uint32_t>(priority) ^ 0x80000000U);
    return (static_cast<std::uint64_t>(inverted) << 32U) |
        (static_cast<std::uint64_t>(tie_break) << 9U) |
        static_cast<std::uint64_t>(index);
}

[[nodiscard]] constexpr std::size_t candidate_source_index(
    const std::uint64_t candidate) noexcept {
    return static_cast<std::size_t>(candidate & 0x1FFU);
}

[[nodiscard]] constexpr int candidate_priority(const std::uint64_t candidate) noexcept {
    return static_cast<int>(static_cast<std::int32_t>(
        ~static_cast<std::uint32_t>(candidate >> 32U) ^ 0x80000000U));
}

[[nodiscard]] int capture_priority_no_tt(
    const SearchOrderingTables& tables, const MoveMetadata& metadata) noexcept {
    const int victim_value = piece_value(metadata.captured_piece);
    const int attacker_value = piece_value(metadata.moving_piece);
    const int see = metadata.see_computed ? std::clamp(
        static_cast<int>(metadata.see_score),
        -piece_value(PieceType::queen), piece_value(PieceType::queen)) : 0;
    const int capture_history = tables.capture_history_score(metadata);
    const int bad_capture_penalty = metadata.see_computed && see < 0 && !metadata.gives_check ?
        kBadCapturePenalty : 0;
    return kCapturePriority - bad_capture_penalty + (victim_value * 16) - attacker_value +
        promotion_value(metadata.move.promotion()) + see * kSeeOrderingWeight +
        capture_history * kCaptureHistoryWeight;
}

#if defined(__clang__) || defined(__GNUC__)
#define KOI_ORD_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define KOI_ORD_ALWAYS_INLINE __forceinline
#else
#define KOI_ORD_ALWAYS_INLINE inline
#endif

struct TableQuietScorer {
    const SearchOrderingTables& tables;
    Color side;
    const SearchHistoryContext& history_context;

    [[nodiscard]] KOI_ORD_ALWAYS_INLINE int score(const MoveMetadata& metadata) const noexcept {
        return tables.quiet_history_score(side, metadata, history_context);
    }
};

template <bool AllCont, bool SkipLowPly>
struct LegalBatchQuietScorer {
    const SearchOrderingTables::QuietHistoryBatch& batch;

    [[nodiscard]] KOI_ORD_ALWAYS_INLINE int score(const MoveMetadata& metadata) const noexcept {
        return batch.score_legal<AllCont, SkipLowPly>(metadata);
    }
};

template <typename Scorer>
[[nodiscard]] KOI_ORD_ALWAYS_INLINE int quiet_priority_core(
    const MoveMetadata& metadata,
    const Move killer0,
    const Move killer1,
    const Move proven_counter,
    const Scorer& scorer) noexcept {
    if (!metadata.gives_check) {
        const Move move = metadata.move;
        if (move == killer0) {
            return kKillerPriority + 1;
        }
        if (move == killer1) {
            return kKillerPriority;
        }
        if (move == proven_counter) {
            return kCounterMovePriority;
        }
    }
    const int history = scorer.score(metadata);
    return metadata.gives_check ?
        (kCheckingMovePriority + std::clamp(history / 32, -8'192, 8'192)) :
        history;
}

[[nodiscard]] int quiet_priority_preloaded(
    const SearchOrderingTables& tables,
    const Color side,
    const MoveMetadata& metadata,
    const SearchHistoryContext& history_context,
    const Move killer0,
    const Move killer1,
    const Move proven_counter) noexcept {
    return quiet_priority_core(
        metadata, killer0, killer1, proven_counter,
        TableQuietScorer{tables, side, history_context});
}

[[nodiscard]] int priority_with_side(
    const SearchOrderingTables& tables,
    const Color side,
    const MoveMetadata& metadata,
    const std::optional<Move> tt_move,
    const SearchHistoryContext& history_context) noexcept {
    const Move move = metadata.move;
    if (tt_move.has_value() && move == *tt_move) {
        return kTtMovePriority;
    }
    if (metadata.is_capture()) {
        return capture_priority_no_tt(tables, metadata);
    }
    if (move.promotion() != Promotion::none) {
        return kPromotionPriority + promotion_value(move.promotion());
    }
    const auto [killer0, killer1] = tables.killers_at_ply(history_context.ply);
    const Move previous = history_context.count > 0 ? history_context.continuation_moves[0] :
                                                        Move::no_move();
    const Move proven_counter = tables.proven_counter_move(side, previous);
    return quiet_priority_preloaded(
        tables, side, metadata, history_context, killer0, killer1, proven_counter);
}

inline constexpr std::array<std::uint8_t, 256> kSquareTransposeTieKey = []() constexpr {
    std::array<std::uint8_t, 256> keys{};
    for (std::size_t idx = 0; idx < 256U; ++idx) {
        keys[idx] = idx < 64U
            ? static_cast<std::uint8_t>((idx & 7U) * 8U + (idx >> 3U))
            : static_cast<std::uint8_t>(64U);
    }
    return keys;
}();

inline constexpr std::array<std::uint8_t, 8> kPromotionTieKey{
    0U, // none
    2U, // knight
    1U, // bishop
    4U, // rook
    3U, // queen
    0U,
    0U,
    0U,
};

[[nodiscard]] inline std::uint32_t generated_move_tie_break_key(const Move move) noexcept {
    const std::uint32_t raw = move.raw();
    const std::uint32_t from_key = kSquareTransposeTieKey[raw & 0x3FU];
    const std::uint32_t to_key = kSquareTransposeTieKey[(raw >> 6U) & 0x3FU];
    const std::uint32_t promo_key = kPromotionTieKey[(raw >> 12U) & 0x7U];
    return (from_key * 64U + to_key) * 5U + promo_key;
}

[[nodiscard]] inline std::uint32_t generated_quiet_tie_break_key(const Move move) noexcept {
    const std::uint32_t raw = move.raw();
    const std::uint32_t from_key = kSquareTransposeTieKey[raw & 0x3FU];
    const std::uint32_t to_key = kSquareTransposeTieKey[(raw >> 6U) & 0x3FU];
    return (from_key * 64U + to_key) * 5U;
}

} // namespace

std::uint32_t move_tie_break_key(const Move move) noexcept {
    if (move.is_no_move()) {
        return (64U * 64U + 64U) * 5U;
    }
    return generated_move_tie_break_key(move);
}

void SearchMoveOrdering::clear() noexcept {
    tables_.clear();
}

void SearchMoveOrdering::update_correction(const std::uint64_t pawn_key,
                                           const std::uint64_t material_key,
                                           const std::uint64_t king_key,
                                           const int bonus) noexcept {
    tables_.update_correction(pawn_key, material_key, king_key, bonus);
}

void SearchMoveOrdering::update_correction(const std::uint64_t pawn_key,
                                           const std::uint64_t material_key,
                                           const std::uint64_t king_key,
                                           const std::uint64_t minor_key,
                                           const std::uint64_t major_key,
                                           const int bonus,
                                           const std::uint64_t continuation_key,
                                           const bool has_continuation) noexcept {
    tables_.update_correction(pawn_key, material_key, king_key, minor_key,
                              major_key, bonus, continuation_key, has_continuation);
}

bool SearchMoveOrdering::is_killer(const Move move, const int ply) const noexcept {
    return tables_.is_killer(move, ply);
}

void SearchMoveOrdering::order(const GameState& state, std::vector<Move>& moves,
                               const std::optional<Move> tt_move, const int ply,
                               const std::optional<Move> previous_move) const {
    std::vector<MoveMetadata> metadata;
    metadata.reserve(moves.size());
    for (const Move move : moves) {
        if (const auto described = state.describe_move(move); described.has_value()) {
            metadata.push_back(*described);
        }
    }
    order(state, metadata, tt_move, ply, previous_move);
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        moves[index] = metadata[index].move;
    }
}

template <typename PriorityFn>
void SearchMoveOrdering::order_with_priority(const GameState& state,
                                               std::span<MoveMetadata> moves,
                                               PriorityFn&& priority_fn) const {
    // One exchange baseline serves every capture in this pass, mirroring
    // GameState::finalize_metadata; it is built at most once and only when a
    // capture still needs SEE.  Ordering never mutates the position, so the
    // baseline stays valid for the whole loop.
    std::optional<ExchangeContext> exchange_context;
    NoInitArray<ScoredMove, kMaximumLegalMoves> scored_moves;
    for (std::size_t index = 0; index < moves.size(); ++index) {
        MoveMetadata& metadata = moves[index];
        if (!metadata.see_computed && metadata.is_capture()) {
            if (!exchange_context.has_value()) {
                exchange_context = exchange_context_from(state);
            }
            metadata.see_score = static_cast<std::int16_t>(std::clamp(
                static_exchange_gain(*exchange_context, metadata),
                static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                static_cast<int>(std::numeric_limits<std::int16_t>::max())));
            metadata.see_computed = true;
        }
        const int score = priority_fn(metadata);
        MoveMetadata scored_metadata = metadata;
        scored_metadata.ordering_score = score;
        scored_moves[index] = ScoredMove{
            scored_metadata, score, move_tie_break_key(metadata.move)};
    }

    std::sort(scored_moves.data(), scored_moves.data() + moves.size(),
              [](const ScoredMove& lhs, const ScoredMove& rhs) {
                  return lhs.priority != rhs.priority ? lhs.priority > rhs.priority :
                      lhs.tie_break < rhs.tie_break;
              });

    for (std::size_t index = 0; index < moves.size(); ++index) {
        moves[index] = scored_moves[index].metadata;
    }
}

void SearchMoveOrdering::order(const GameState& state, MoveMetadataList& moves,
                               const std::optional<Move> tt_move, const int ply,
                               const std::optional<Move> previous_move) const {
    order_with_priority(state, moves, [&](const MoveMetadata& metadata) {
        return priority(state, metadata, tt_move, ply, previous_move);
    });
}

void SearchMoveOrdering::order(const GameState& state, MoveMetadataList& moves,
                               const std::optional<Move> tt_move,
                               const SearchHistoryContext& history_context) const {
    order_with_priority(state, moves, [&](const MoveMetadata& metadata) {
        return priority(state, metadata, tt_move, history_context);
    });
}

int SearchMoveOrdering::priority(const GameState& state, const MoveMetadata& metadata,
                                 const std::optional<Move> tt_move, const int ply,
                                 const std::optional<Move> previous_move) const {
    const Move move = metadata.move;
    if (tt_move.has_value() && move == *tt_move) {
        return kTtMovePriority;
    }

    if (metadata.is_capture()) {
        const int victim_value = piece_value(metadata.captured_piece);
        const int attacker_value = piece_value(metadata.moving_piece);
        // Staged search scores captures before SEE is needed, just like
        // Stockfish 19's capture-history picker.  Once a candidate has been
        // materialized, the exact exchange result is a useful tie-breaker.
        const int see = metadata.see_computed ? std::clamp(
            static_cast<int>(metadata.see_score),
            -piece_value(PieceType::queen), piece_value(PieceType::queen)) : 0;
        const int capture_history = tables_.capture_history_score(metadata);
        const int bad_capture_penalty = metadata.see_computed && see < 0 && !metadata.gives_check ?
            kBadCapturePenalty : 0;
        return kCapturePriority - bad_capture_penalty + (victim_value * 16) - attacker_value +
            promotion_value(move.promotion()) + see * kSeeOrderingWeight +
            capture_history * kCaptureHistoryWeight;
    }
    if (move.promotion() != Promotion::none) {
        return kPromotionPriority + promotion_value(move.promotion());
    }
    if (metadata.gives_check) {
        const int check_history = quiet_history_score(
            state.side_to_move(), move, previous_move);
        return kCheckingMovePriority + std::clamp(check_history / 32, -8'192, 8'192);
    }

    const int killer_rank = tables_.killer_rank(move, ply);
    if (killer_rank == 2) {
        return kKillerPriority + 1;
    }
    if (killer_rank == 1) {
        return kKillerPriority;
    }
    if (previous_move.has_value() &&
        tables_.is_proven_counter_move(state.side_to_move(), *previous_move, move)) {
        return kCounterMovePriority;
    }
    return tables_.quiet_history_score(state.side_to_move(), move, previous_move);
}

int SearchMoveOrdering::priority(const GameState& state, const MoveMetadata& metadata,
                                 const std::optional<Move> tt_move,
                                 const SearchHistoryContext& history_context) const {
    return priority_with_side(
        tables_, state.side_to_move(), metadata, tt_move, history_context);
}

void SearchMoveOrdering::order(const GameState& state, std::vector<MoveMetadata>& moves,
                               const std::optional<Move> tt_move, const int ply,
                               const std::optional<Move> previous_move) const {
    order_with_priority(state, moves, [&](const MoveMetadata& metadata) {
        return priority(state, metadata, tt_move, ply, previous_move);
    });
}

int SearchMoveOrdering::quiet_history_score(
    const Color side, const Move move, const std::optional<Move> previous_move) const noexcept {
    return tables_.quiet_history_score(side, move, previous_move);
}

int SearchMoveOrdering::quiet_history_score(
    const Color side, const MoveMetadata& metadata,
    const SearchHistoryContext& history_context) const noexcept {
    return tables_.quiet_history_score(side, metadata, history_context);
}

int SearchMoveOrdering::continuation_history_score(
    const MoveMetadata& metadata, const SearchHistoryContext& history_context) const noexcept {
    return tables_.continuation_history_score(metadata, history_context);
}

QuietAndContinuationScores SearchMoveOrdering::quiet_and_continuation_scores(
    const Color side, const MoveMetadata& metadata,
    const SearchHistoryContext& history_context) const noexcept {
    return tables_.quiet_and_continuation_scores(side, metadata, history_context);
}

int SearchMoveOrdering::capture_history_score(
    const MoveMetadata& metadata) const noexcept {
    return tables_.capture_history_score(metadata);
}

bool SearchMoveOrdering::is_proven_counter_move(
    const Color side, const Move previous_move, const Move move) const noexcept {
    return tables_.is_proven_counter_move(side, previous_move, move);
}

void SearchMoveOrdering::record_quiet_cutoff(
    const Color side, const Move move, const int ply, const int depth,
    const std::optional<Move> previous_move) noexcept {
    tables_.record_quiet_cutoff(side, move, ply, depth, previous_move);
}

void SearchMoveOrdering::record_quiet_fail(
    const Color side, const Move move, const int depth,
    const std::optional<Move> previous_move) noexcept {
    tables_.record_quiet_fail(side, move, depth, previous_move);
}

void SearchMoveOrdering::record_quiet_cutoff(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_quiet_cutoff(side, metadata, depth, history_context);
}

void SearchMoveOrdering::record_quiet_best(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_quiet_best(side, metadata, depth, history_context);
}

void SearchMoveOrdering::record_quiet_fail(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_quiet_fail(side, metadata, depth, history_context);
}

void SearchMoveOrdering::record_capture_cutoff(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_capture_cutoff(metadata, depth, history_context);
}

void SearchMoveOrdering::record_capture_best(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_capture_best(metadata, depth, history_context);
}

void SearchMoveOrdering::record_capture_fail(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_capture_fail(metadata, depth, history_context);
}

void SearchMoveOrdering::record_parent_fail_low(
    const Color side, const Move move, const PieceType piece, const int parent_ply,
    const SearchHistoryContext& history_context, const int depth) noexcept {
    tables_.record_parent_fail_low(side, move, piece, parent_ply, history_context, depth);
}

void SearchMoveOrdering::record_parent_refuted(
    const Color side, const Move move, const PieceType piece, const int parent_ply,
    const SearchHistoryContext& history_context, const int depth) noexcept {
    tables_.record_parent_refuted(side, move, piece, parent_ply, history_context, depth);
}

SearchMovePicker::SearchMovePicker(
    const SearchMoveOrdering& ordering, const GameState& state,
    const MoveMetadataList& moves, std::optional<Move> tt_move,
    const SearchHistoryContext* history_context, const Mode mode,
    const Move excluded_move, MoveMetadataList* const mutable_moves) noexcept
    : ordering_(ordering), state_(state), moves_(moves),
      history_context_(history_context), tt_move_(tt_move),
      excluded_move_(excluded_move), mode_(mode), stage_(first_stage()),
      mutable_moves_(mutable_moves), defer_quiets_(mutable_moves != nullptr) {
    // Only a main-mode picker reads the capture-check guard (materialize's
    // capture-check probe); quiescence and evasion pickers never reach that
    // probe, so only main mode pays for its zeroed start.
    if (mode_ == Mode::main) {
        capture_check_computed_.reset();
        capture_checks_.reset();
    }
}

bool SearchMovePicker::is_excluded(const std::size_t index) const noexcept {
    return index >= moves_.size() || emitted_[index] ||
        moves_[index].move == excluded_move_;
}

bool SearchMovePicker::is_good_capture(const MoveMetadata& metadata) const noexcept {
    // A quiet promotion is still a forcing material transformation.  It has
    // no SEE score to certify it as a capture, so never defer it with poisoned
    // captures merely because it does not give check.
    if (metadata.move.promotion() != Promotion::none) {
        return true;
    }
    if (!metadata.is_capture()) {
        return false;
    }
    // Stockfish 19 scales the SEE threshold with capture history and victim
    // value (its `-cur->value / 18` gate). A fixed threshold over-searches
    // poorly ordered pawn captures while delaying high-value tactical
    // recaptures, so retain that calibrated relationship in centipawn units.
    const int capture_score = ordering_.capture_history_score(metadata) +
        7 * piece_value(metadata.captured_piece);
    const int threshold = std::clamp(-(capture_score / 18), -1024, 1024);
    return metadata.gives_check || metadata.see_score >= threshold;
}

SearchMovePicker::Stage SearchMovePicker::first_stage() const noexcept {
    if (tt_move_.has_value()) {
        return Stage::tt;
    }
    switch (mode_) {
    case Mode::main:
    case Mode::quiescence:
        return Stage::good_captures;
    case Mode::evasion:
        return Stage::evasions;
    }
    return Stage::done;
}

MoveMetadata SearchMovePicker::materialize(const std::size_t index) {
    MoveMetadata metadata = moves_[index];
    if (metadata.is_capture()) {
        if (!see_computed_[index]) {
            if (metadata.see_computed) {
                see_scores_[index] = metadata.see_score;
            } else {
                if (!see_context_.has_value()) {
                    see_context_ = exchange_context_from(state_);
                }
                see_scores_[index] = static_cast<std::int16_t>(std::clamp(
                    static_exchange_gain(*see_context_, metadata),
                    static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<int>(std::numeric_limits<std::int16_t>::max())));
            }
            see_computed_.set(index);
        }
        metadata.see_score = see_scores_[index];
        metadata.see_computed = true;
    }
    const bool promotion = metadata.move.promotion() != Promotion::none;
    if (mode_ == Mode::main && (metadata.is_capture() || promotion) && !metadata.gives_check) {
        if (!capture_check_computed_.test(index)) {
            if (state_.move_gives_check(metadata.move)) {
                capture_checks_.set(index);
            }
            capture_check_computed_.set(index);
        }
        metadata.gives_check = capture_checks_.test(index);
    }
    return metadata;
}

void SearchMovePicker::prepare_quiescence_candidates() {
    captures_prepared_ = true;
    quiet_checks_scored_ = false;
    std::size_t capture_count = 0;
    std::size_t check_count = 0;
    for (std::size_t index = 0; index < moves_.size(); ++index) {
        if (emitted_[index]) {
            continue;
        }
        const MoveMetadata& source = moves_[index];
        if (source.move == excluded_move_) {
            continue;
        }
        const bool tactical = source.is_capture() ||
            source.move.promotion() != Promotion::none;
        if (!tactical) {
            if (source.gives_check) {
                // Defer 6-ply continuation history lookups for quiet checks
                // until Stage::quiet_checks is reached (captures or stand-pat
                // usually cut off first).
                staged_[kMaximumLegalMoves - 1 - check_count] =
                    static_cast<std::uint64_t>(index);
                ++check_count;
            }
            continue;
        }
        int priority = 0;
        if (source.is_capture()) {
            MoveMetadata materialized = source;
            if (!see_computed_[index]) {
                if (materialized.see_computed) {
                    see_scores_[index] = materialized.see_score;
                } else {
                    if (!see_context_.has_value()) {
                        see_context_ = exchange_context_from(state_);
                    }
                    see_scores_[index] = static_cast<std::int16_t>(std::clamp(
                        static_exchange_gain(*see_context_, materialized),
                        static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                        static_cast<int>(std::numeric_limits<std::int16_t>::max())));
                }
                see_computed_.set(index);
            }
            materialized.see_score = see_scores_[index];
            materialized.see_computed = true;
            priority = capture_priority_no_tt(ordering_.tables_, materialized);
        } else {
            priority = kPromotionPriority + promotion_value(source.move.promotion());
        }
        staged_[capture_count++] = pack_candidate(
            priority, generated_move_tie_break_key(source.move), index);
    }
    const std::size_t cap_rank = static_cast<std::size_t>(Stage::good_captures);
    const std::size_t chk_rank = static_cast<std::size_t>(Stage::quiet_checks);
    stage_begin_[cap_rank] = 0;
    stage_end_[cap_rank] = static_cast<std::uint16_t>(capture_count);
    stage_cursor_[cap_rank] = 0;
    stage_begin_[chk_rank] = static_cast<std::uint16_t>(kMaximumLegalMoves - check_count);
    stage_end_[chk_rank] = static_cast<std::uint16_t>(kMaximumLegalMoves);
    stage_cursor_[chk_rank] = stage_begin_[chk_rank];
    stage_sorted_.reset();
}

void SearchMovePicker::prepare_evasion_candidates() {
    captures_prepared_ = true;
    const Color side = state_.side_to_move();
    const auto [killer0, killer1] = ordering_.tables_.killers_at_ply(history_context_->ply);
    const Move previous = history_context_->count > 0 ?
        history_context_->continuation_moves[0] : Move::no_move();
    const Move proven_counter = ordering_.tables_.proven_counter_move(side, previous);
    const auto quiet_batch = ordering_.tables_.prepare_quiet_history_batch(side, *history_context_);
    std::size_t evasion_count = 0;
    for (std::size_t index = 0; index < moves_.size(); ++index) {
        if (emitted_[index]) {
            continue;
        }
        const MoveMetadata& metadata = moves_[index];
        if (metadata.move == excluded_move_) {
            continue;
        }
        const int priority = metadata.is_capture() ?
            capture_priority_no_tt(ordering_.tables_, metadata) :
            (metadata.move.promotion() != Promotion::none ?
                kPromotionPriority + promotion_value(metadata.move.promotion()) :
                quiet_priority_core(metadata, killer0, killer1, proven_counter, quiet_batch));
        staged_[evasion_count++] = pack_candidate(
            priority, generated_move_tie_break_key(metadata.move), index);
    }
    const std::size_t rank = static_cast<std::size_t>(Stage::evasions);
    stage_begin_[rank] = 0;
    stage_end_[rank] = static_cast<std::uint16_t>(evasion_count);
    stage_cursor_[rank] = 0;
    stage_sorted_.reset();
}

void SearchMovePicker::prepare_main_captures() {
    captures_prepared_ = true;
    main_captures_end_ = 0;
    deferred_bad_capture_count_ = 0;
    deferred_bad_capture_index_ = 0;
    for (std::size_t index = 0; index < moves_.size(); ++index) {
        if (emitted_[index]) {
            continue;
        }
        const MoveMetadata& metadata = moves_[index];
        if (metadata.move == excluded_move_) {
            continue;
        }
        if (metadata.move.promotion() == Promotion::none && !metadata.is_capture()) {
            continue;
        }
        const int priority = metadata.is_capture() ?
            capture_priority_no_tt(ordering_.tables_, metadata) :
            kPromotionPriority + promotion_value(metadata.move.promotion());
        staged_[main_captures_end_++] = pack_candidate(
            priority, generated_move_tie_break_key(metadata.move), index);
    }
    const std::size_t cap_rank = static_cast<std::size_t>(Stage::good_captures);
    stage_begin_[cap_rank] = 0;
    stage_end_[cap_rank] = static_cast<std::uint16_t>(main_captures_end_);
    stage_cursor_[cap_rank] = 0;
}

template <bool AllCont, bool SkipLowPly>
void SearchMovePicker::prepare_main_quiets_impl(
    const Color side, const SearchOrderingTables::QuietHistoryBatch& quiet_batch) {
    const auto [killer0, killer1] = ordering_.tables_.killers_at_ply(history_context_->ply);
    const Move previous = history_context_->count > 0 ?
        history_context_->continuation_moves[0] : Move::no_move();
    const Move proven_counter = ordering_.tables_.proven_counter_move(side, previous);
    const LegalBatchQuietScorer<AllCont, SkipLowPly> scorer{quiet_batch};
    std::size_t good_cursor = main_captures_end_;
    std::size_t bad_cursor = kMaximumLegalMoves;
    for (std::size_t index = main_quiet_start_; index < moves_.size(); ++index) {
        if (emitted_[index]) {
            continue;
        }
        const MoveMetadata& metadata = moves_[index];
        if (metadata.move == excluded_move_) {
            continue;
        }
        if ((metadata.move.raw() & 0x7000U) != 0U || metadata.is_capture()) {
            continue;
        }
        const int priority = quiet_priority_core(
            metadata, killer0, killer1, proven_counter, scorer);
        const Candidate candidate = pack_candidate(
            priority, generated_quiet_tie_break_key(metadata.move), index);
        if (priority <= kGoodQuietPriorityThreshold) {
            staged_[--bad_cursor] = candidate;
        } else {
            staged_[good_cursor++] = candidate;
        }
    }
    const std::size_t good_rank = static_cast<std::size_t>(Stage::good_quiets);
    const std::size_t bad_rank = static_cast<std::size_t>(Stage::bad_quiets);
    stage_begin_[good_rank] = static_cast<std::uint16_t>(main_captures_end_);
    stage_end_[good_rank] = static_cast<std::uint16_t>(good_cursor);
    stage_cursor_[good_rank] = stage_begin_[good_rank];
    stage_begin_[bad_rank] = static_cast<std::uint16_t>(bad_cursor);
    stage_end_[bad_rank] = static_cast<std::uint16_t>(kMaximumLegalMoves);
    stage_cursor_[bad_rank] = stage_begin_[bad_rank];
}

void SearchMovePicker::prepare_main_quiets() {
    quiets_prepared_ = true;
    const Color side = state_.side_to_move();
    const auto quiet_batch = ordering_.tables_.prepare_quiet_history_batch(side, *history_context_);
    if (quiet_batch.all_cont && quiet_batch.low_ply_row == nullptr) {
        prepare_main_quiets_impl<true, true>(side, quiet_batch);
    } else {
        prepare_main_quiets_impl<false, false>(side, quiet_batch);
    }
}

void SearchMovePicker::score_quiescence_quiet_checks() {
    quiet_checks_scored_ = true;
    const std::size_t rank = static_cast<std::size_t>(Stage::quiet_checks);
    const auto quiet_batch = ordering_.tables_.prepare_quiet_history_batch(
        state_.side_to_move(), *history_context_);
    for (std::size_t s = stage_begin_[rank]; s < stage_end_[rank]; ++s) {
        const std::size_t src = candidate_source_index(staged_[s]);
        const MoveMetadata& check_meta = moves_[src];
        const int priority = quiet_priority_core(
            check_meta, Move::no_move(), Move::no_move(), Move::no_move(), quiet_batch);
        staged_[s] = pack_candidate(priority, generated_move_tie_break_key(check_meta.move), src);
    }
}

std::size_t SearchMovePicker::pick_next_in_stage(const std::size_t rank) noexcept {
    // Pick incrementally for a short prefix so cutoffs on the first few moves
    // avoid sorting the full stage, then sort any longer tail once. The packed
    // 64-bit key is a strict total order, so emitted order matches full sort.
    constexpr std::size_t kIncrementalPickPrefix = 6U;
    const std::size_t begin = stage_begin_[rank];
    const std::size_t end = stage_end_[rank];
    const std::size_t cursor = stage_cursor_[rank];
    if (!stage_sorted_[rank]) {
        const std::size_t remaining = end - cursor;
        if (remaining <= 1U) {
            stage_sorted_.set(rank);
        } else if (cursor - begin < kIncrementalPickPrefix) {
            std::size_t best_slot = cursor;
            Candidate best_key = staged_[cursor];
            for (std::size_t probe = cursor + 1U; probe < end; ++probe) {
                if (staged_[probe] < best_key) {
                    best_key = staged_[probe];
                    best_slot = probe;
                }
            }
            if (best_slot != cursor) {
                std::swap(staged_[cursor], staged_[best_slot]);
            }
        } else {
            stage_sorted_.set(rank);
            if (remaining <= 48U) {
                for (std::size_t i = cursor + 1U; i < end; ++i) {
                    const Candidate value = staged_[i];
                    std::size_t j = i;
                    while (j > cursor && value < staged_[j - 1U]) {
                        staged_[j] = staged_[j - 1U];
                        --j;
                    }
                    staged_[j] = value;
                }
            } else {
                std::sort(staged_.begin() + cursor, staged_.begin() + end,
                          [](const Candidate lhs, const Candidate rhs) {
                              return lhs < rhs;
                          });
            }
        }
    }
    stage_cursor_[rank] = static_cast<std::uint16_t>(cursor + 1U);
    return cursor;
}

void SearchMovePicker::advance_stage() noexcept {
    switch (stage_) {
    case Stage::tt:
        stage_ = mode_ == Mode::evasion ? Stage::evasions : Stage::good_captures;
        break;
    case Stage::good_captures:
        stage_ = mode_ == Mode::quiescence ? Stage::quiet_checks : Stage::good_quiets;
        break;
    case Stage::good_quiets:
        stage_ = Stage::bad_captures;
        break;
    case Stage::bad_captures:
        stage_ = mode_ == Mode::main ? Stage::bad_quiets : Stage::done;
        break;
    case Stage::bad_quiets:
    case Stage::quiet_checks:
        stage_ = Stage::done;
        break;
    case Stage::evasions:
    case Stage::done:
        stage_ = Stage::done;
        break;
    }
}

const MoveMetadata* SearchMovePicker::emit_tt() {
    if (tt_emitted_ || !tt_move_.has_value()) {
        return nullptr;
    }
    tt_emitted_ = true;
    for (std::size_t index = 0; index < moves_.size(); ++index) {
        if (moves_[index].move == *tt_move_ && !is_excluded(index)) {
            emitted_.set(index);
            emitted_metadata_ = mode_ == Mode::evasion ? moves_[index] :
                materialize(index);
            // The emitted move equals tt_move_, which priority() answers with
            // the TT constant before any other branch; materialize never
            // changes the move.
            emitted_metadata_.ordering_score = kTtMovePriority;
            return &emitted_metadata_;
        }
    }
    return nullptr;
}

int SearchMovePicker::resolved_capture_priority(
    const std::size_t source_index, const std::uint64_t candidate) const {
    if (!moves_[source_index].see_computed) {
        // Deferring SEE during staging leaves `see == 0` and no bad-capture
        // penalty; apply only that delta on emission so the score matches
        // eager materialization bit for bit.
        const int see = std::clamp(
            static_cast<int>(emitted_metadata_.see_score),
            -piece_value(PieceType::queen), piece_value(PieceType::queen));
        const int bad_capture_penalty =
            see < 0 && !emitted_metadata_.gives_check ? kBadCapturePenalty : 0;
        return candidate_priority(candidate) + see * kSeeOrderingWeight -
            bad_capture_penalty;
    }
    return ordering_.priority(state_, emitted_metadata_, std::nullopt, *history_context_);
}

const MoveMetadata* SearchMovePicker::next() {
    for (;;) {
        if (stage_ == Stage::done) {
            return nullptr;
        }
        if (stage_ == Stage::tt) {
            if (const MoveMetadata* tt = emit_tt(); tt != nullptr) {
                advance_stage();
                return tt;
            }
            advance_stage();
            continue;
        }
        if (mode_ == Mode::quiescence) {
            if (!captures_prepared_) {
                prepare_quiescence_candidates();
            }
            const std::size_t rank = static_cast<std::size_t>(stage_);
            if (stage_cursor_[rank] < stage_end_[rank]) {
                if (stage_ == Stage::quiet_checks && !quiet_checks_scored_) {
                    score_quiescence_quiet_checks();
                }
                const std::size_t slot = pick_next_in_stage(rank);
                const Candidate candidate = staged_[slot];
                const std::size_t source_index = candidate_source_index(candidate);
                emitted_metadata_ = moves_[source_index];
                if (emitted_metadata_.is_capture()) {
                    emitted_metadata_.see_score = see_scores_[source_index];
                    emitted_metadata_.see_computed = true;
                }
                emitted_metadata_.ordering_score = candidate_priority(candidate);
                return &emitted_metadata_;
            }
            advance_stage();
            continue;
        }
        // Quiet checks, killers, and proven counters receive large priority
        // bonuses inside the good-quiet stage.  Once a cut node decides to
        // skip quiets, the remaining quiet prefix and tail are both deferred;
        // captures still flow through their own stages.
        if (skip_quiets_ && (stage_ == Stage::good_quiets ||
                             stage_ == Stage::bad_quiets)) {
            advance_stage();
            continue;
        }
        if (mode_ == Mode::evasion) {
            if (!captures_prepared_) {
                prepare_evasion_candidates();
            }
            const std::size_t rank = static_cast<std::size_t>(stage_);
            if (stage_cursor_[rank] < stage_end_[rank]) {
                const Candidate candidate = staged_[pick_next_in_stage(rank)];
                emitted_metadata_ = moves_[candidate_source_index(candidate)];
                emitted_metadata_.ordering_score = candidate_priority(candidate);
                return &emitted_metadata_;
            }
            advance_stage();
            continue;
        }
        if (stage_ == Stage::bad_captures) {
            if (deferred_bad_capture_index_ < deferred_bad_capture_count_) {
                const std::size_t candidate_slot =
                    deferred_bad_capture_indices_[deferred_bad_capture_index_++];
                const Candidate candidate = staged_[candidate_slot];
                const std::size_t source_index = candidate_source_index(candidate);
                emitted_metadata_ = materialize(source_index);
                emitted_metadata_.ordering_score =
                    resolved_capture_priority(source_index, candidate);
                return &emitted_metadata_;
            }
            advance_stage();
            continue;
        }
        if (stage_ == Stage::good_captures) {
            if (!captures_prepared_) {
                prepare_main_captures();
            }
        } else if (!quiets_prepared_) {
            if (defer_quiets_ && mutable_moves_ != nullptr) {
                main_quiet_start_ = mutable_moves_->size();
                state_.append_quiet_search_moves_with_metadata(*mutable_moves_);
                defer_quiets_ = false;
            }
            prepare_main_quiets();
        }
        const std::size_t rank = static_cast<std::size_t>(stage_);
        if (stage_cursor_[rank] < stage_end_[rank]) {
            const std::size_t candidate_slot = pick_next_in_stage(rank);
            const Candidate candidate = staged_[candidate_slot];
            const std::size_t source_index = candidate_source_index(candidate);
            const int priority = candidate_priority(candidate);
            if (stage_ != Stage::good_captures) {
                emitted_metadata_ = moves_[source_index];
                emitted_metadata_.ordering_score = priority;
                return &emitted_metadata_;
            }
            emitted_metadata_ = materialize(source_index);
            if (emitted_metadata_.is_capture() && !is_good_capture(emitted_metadata_)) {
                if (deferred_bad_capture_count_ < deferred_bad_capture_indices_.size()) {
                    // Keep the staged slot rather than the source index so the
                    // candidate's staged priority is still reachable when the
                    // bad-capture stage emits the deferred record.
                    deferred_bad_capture_indices_[deferred_bad_capture_count_++] =
                        static_cast<std::uint16_t>(candidate_slot);
                }
                continue;
            }
            emitted_metadata_.ordering_score = emitted_metadata_.is_capture()
                ? resolved_capture_priority(source_index, candidate)
                : priority;
            return &emitted_metadata_;
        }
        advance_stage();
    }
}

#undef KOI_ORD_ALWAYS_INLINE

} // namespace koi::detail
