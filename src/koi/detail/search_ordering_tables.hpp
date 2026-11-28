#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "koi/game_state.hpp"

namespace koi::detail {

inline constexpr std::size_t kSearchContinuationPlies = 6;

// Canonical table index for a move.  Pure and total: the result depends only
// on the move, and no-move or out-of-range squares both map to 0.  Defined
// here so the per-node precomputation and the ordering tables share one
// definition and can never drift apart.
[[nodiscard]] constexpr std::uint64_t ordering_mix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] constexpr std::size_t table_move_index(const Move move) noexcept {
    if (move.is_no_move() || move.from().index() >= Square::kInvalid ||
        move.to().index() >= Square::kInvalid) {
        return 0;
    }
    return static_cast<std::size_t>(move.from().index()) * 64U + move.to().index();
}

// The recursive search supplies the short move suffix that is available at a
// node.  Keeping this as a value object makes all ordering tables worker-local
// and avoids exposing SearchStack internals to the ordering implementation.
//
// Deliberately without default member initializers: this is a
// write-before-read contract type.  SearchContext::history_context() writes
// every field any reader can observe, and quiescence starts the storage
// without stores (NoInitArray), so a context that can never be read costs no
// initialization.  The unscaled continuation tail still holds Move's own
// sentinel because Move's non-trivial default constructor runs on the
// default-initialized array.
struct SearchHistoryContext {
    std::array<Move, kSearchContinuationPlies> continuation_moves;
    // `table_move_index(continuation_moves[d]) * 131U`, hoisted once per node
    // by SearchContext::history_context() so scoring stops recomputing it per
    // candidate.  table_move_index() is pure and total (no-move and
    // out-of-range map to 0), so each stored value equals the recomputed
    // expression for every input; the read paths only add `move_idx * 17U`
    // and mask, so every continuation slot stays bit-identical.  Zero marks an
    // unsealed context (a real move never has index 0); those recompute the
    // component, which preserves the exact previous behavior for hand-built
    // contexts.
    std::array<std::uint32_t, kSearchContinuationPlies> continuation_previous_scaled;
    std::size_t count;
    int ply;
    // The search supplies a pawn-only structural key so equal pawn
    // structures share experience across otherwise different piece layouts;
    // the move dimensions remain explicit in the table index.
    std::uint64_t pawn_key;
};

struct QuietAndContinuationScores {
    int quiet = 0;
    int continuation = 0;
};

// SearchOrderingTables owns the adaptive state for one worker's move ordering.
// It intentionally has no knowledge of search-policy decisions or recursion.
class SearchOrderingTables {
public:
    SearchOrderingTables();
    ~SearchOrderingTables();
    SearchOrderingTables(const SearchOrderingTables&) = delete;
    SearchOrderingTables& operator=(const SearchOrderingTables&) = delete;

    void clear() noexcept;

    [[nodiscard]] bool is_killer(const Move move, const int ply) const noexcept {
        return killer_rank(move, ply) != 0;
    }
    [[nodiscard]] int killer_rank(const Move move, const int ply) const noexcept {
        const auto& killers = killers_[static_cast<std::size_t>(normalized_ply(ply))];
        if (killers[0] == move) {
            return 2;
        }
        if (killers[1] == move) {
            return 1;
        }
        return 0;
    }
    [[nodiscard]] std::pair<Move, Move> killers_at_ply(int ply) const noexcept {
        const auto& killers = killers_[static_cast<std::size_t>(normalized_ply(ply))];
        return {killers[0], killers[1]};
    }
    [[nodiscard]] Move proven_counter_move(Color side, Move previous_move) const noexcept;
    [[nodiscard]] int quiet_history_score(
        Color side, Move move, std::optional<Move> previous_move = std::nullopt) const noexcept;
    [[nodiscard]] int quiet_history_score(
        Color side, const MoveMetadata& metadata,
        const SearchHistoryContext& context) const noexcept;
    [[nodiscard]] int continuation_history_score(
        const MoveMetadata& metadata, const SearchHistoryContext& context) const noexcept;
    [[nodiscard]] QuietAndContinuationScores quiet_and_continuation_scores(
        Color side, const MoveMetadata& metadata,
        const SearchHistoryContext& context) const noexcept;
    [[nodiscard]] int capture_history_score(const MoveMetadata& metadata) const noexcept {
        if (!metadata.is_capture()) {
            return 0;
        }
        return capture_history_[capture_index(metadata)];
    }

    struct QuietHistoryBatch {
        static constexpr std::uint16_t kInvalidOffset = 0xFFFFU;
        static constexpr std::size_t kContSize = 16U * 1024U;
        static constexpr std::size_t kContMask = kContSize - 1U;
        static constexpr std::size_t kPawnMask = 32U * 1024U - 1U;
        static constexpr std::int64_t kHistoryMax = 16'384;

        const std::int16_t* history_row = nullptr;
        const std::int16_t* piece_to_row = nullptr;
        const std::int16_t* low_ply_row = nullptr;
        const std::int16_t* pawn_row = nullptr;
        const std::int16_t* cont0 = nullptr;
        const std::int16_t* multi_cont = nullptr;
        std::uint64_t pawn_key = 0;
        std::array<std::uint16_t, 6> cont_prev{};
        bool all_cont = false;

#if defined(__clang__) || defined(__GNUC__)
#define KOI_TABLES_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define KOI_TABLES_ALWAYS_INLINE __forceinline
#else
#define KOI_TABLES_ALWAYS_INLINE inline
#endif

        template <bool AllCont, bool SkipLowPly, bool Unchecked>
        [[nodiscard]] KOI_TABLES_ALWAYS_INLINE int score_impl(
            const MoveMetadata& metadata) const noexcept {
            if constexpr (!Unchecked) {
                if (metadata.move.is_no_move()) {
                    return 0;
                }
            }
            const std::uint32_t raw = metadata.move.raw();
            const std::size_t from_sq = Unchecked ?
                static_cast<std::size_t>(raw & 0x3FU) :
                static_cast<std::size_t>(metadata.move.from().index());
            const std::size_t to_raw = Unchecked ?
                static_cast<std::size_t>((raw >> 6U) & 0x3FU) :
                static_cast<std::size_t>(metadata.move.to().index());
            const std::size_t move_idx =
                (Unchecked || (from_sq < Square::kInvalid && to_raw < Square::kInvalid)) ?
                (from_sq * 64U + to_raw) : 0U;
            const std::size_t to_sq = (Unchecked || to_raw < Square::kInvalid) ? to_raw : 0U;
            const int piece_i = static_cast<int>(metadata.moving_piece);
            const std::size_t piece_idx =
                (Unchecked || (piece_i >= 0 && piece_i < static_cast<int>(kPieceTypeCount))) ?
                static_cast<std::size_t>(piece_i) : 0U;
            std::int32_t total = history_row[move_idx];
            total += piece_to_row[piece_idx * 64U + to_sq];
            if constexpr (!SkipLowPly) {
                if (low_ply_row != nullptr) {
                    total += low_ply_row[move_idx];
                }
            }
            const std::size_t scaled_move = move_idx * 17U;
            if (AllCont || cont_prev[0] != kInvalidOffset) {
                total += cont0[(static_cast<std::size_t>(cont_prev[0]) + scaled_move) & kContMask];
            }
            if (AllCont || cont_prev[1] != kInvalidOffset) {
                total += static_cast<std::int32_t>(
                    multi_cont[kContSize + ((static_cast<std::size_t>(cont_prev[1]) + scaled_move) & kContMask)]) / 2;
            }
            if (AllCont || cont_prev[2] != kInvalidOffset) {
                total += static_cast<std::int32_t>(
                    multi_cont[2U * kContSize + ((static_cast<std::size_t>(cont_prev[2]) + scaled_move) & kContMask)]) / 3;
            }
            if (AllCont || cont_prev[3] != kInvalidOffset) {
                total += static_cast<std::int32_t>(
                    multi_cont[3U * kContSize + ((static_cast<std::size_t>(cont_prev[3]) + scaled_move) & kContMask)]) / 4;
            }
            if (AllCont || cont_prev[4] != kInvalidOffset) {
                total += static_cast<std::int32_t>(
                    multi_cont[4U * kContSize + ((static_cast<std::size_t>(cont_prev[4]) + scaled_move) & kContMask)]) / 5;
            }
            if (AllCont || cont_prev[5] != kInvalidOffset) {
                total += static_cast<std::int32_t>(
                    multi_cont[5U * kContSize + ((static_cast<std::size_t>(cont_prev[5]) + scaled_move) & kContMask)]) / 6;
            }
            const std::uint64_t dims =
                (static_cast<std::uint64_t>(piece_idx) << 8U) | to_sq;
            const std::size_t pawn_slot =
                static_cast<std::size_t>(ordering_mix64(pawn_key ^ dims) & kPawnMask);
            total += static_cast<std::int32_t>(pawn_row[pawn_slot]) / 2;
            return static_cast<int>(
                std::clamp<std::int32_t>(total, -static_cast<std::int32_t>(kHistoryMax),
                                         static_cast<std::int32_t>(kHistoryMax)));
        }

        template <bool AllCont, bool SkipLowPly>
        [[nodiscard]] KOI_TABLES_ALWAYS_INLINE int score_legal(
            const MoveMetadata& metadata) const noexcept {
            return score_impl<AllCont, SkipLowPly, true>(metadata);
        }

        [[nodiscard]] KOI_TABLES_ALWAYS_INLINE int score(
            const MoveMetadata& metadata) const noexcept {
            return score_impl<false, false, false>(metadata);
        }
#undef KOI_TABLES_ALWAYS_INLINE
    };

    [[nodiscard]] QuietHistoryBatch prepare_quiet_history_batch(
        Color side, const SearchHistoryContext& context) const noexcept;
    [[nodiscard]] bool is_proven_counter_move(
        const Color side, const Move previous_move, const Move move) const noexcept {
        if (previous_move.is_no_move() || move.is_no_move()) {
            return false;
        }
        const std::size_t side_index = side == Color::white ? 0U : 1U;
        const std::size_t previous_index = table_move_index(previous_move);
        return counter_moves_[side_index][previous_index] == move &&
            counter_confidence_[side_index][previous_index] >= kCounterMoveMinimumConfidence;
    }

#if defined(__clang__) || defined(__GNUC__)
#define KOI_TABLES_CORR_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define KOI_TABLES_CORR_INLINE __forceinline
#else
#define KOI_TABLES_CORR_INLINE inline
#endif

    // Bounded static-evaluation correction learned from searched nodes. The
    // correction only shifts the static eval used by pruning and ordering;
    // TT scores always keep the unadjusted search result.
    [[nodiscard]] KOI_TABLES_CORR_INLINE int correction_value(
        std::uint64_t pawn_key, std::uint64_t material_key,
        std::uint64_t king_key) const noexcept {
        const LargeTables& tables = *large_;
        const std::int32_t total =
            static_cast<std::int32_t>(
                tables.pawn_correction[correction_slot(pawn_key, kPawnCorrectionHistorySize)]) +
            tables.material_correction[correction_slot(material_key, kMaterialCorrectionHistorySize)] +
            tables.king_correction[correction_slot(king_key, kKingCorrectionHistorySize)];
        return static_cast<int>(std::clamp<std::int32_t>(
            total, -kCorrectionTotalLimit, kCorrectionTotalLimit));
    }

    [[nodiscard]] KOI_TABLES_CORR_INLINE int correction_value(
        std::uint64_t pawn_key, std::uint64_t material_key,
        std::uint64_t king_key, std::uint64_t minor_key,
        std::uint64_t major_key, std::uint64_t continuation_key = 0,
        bool has_continuation = false) const noexcept {
        // Keep the proven 8-unit structural average exact whenever the
        // continuation pair is neutral or unavailable, and layer a gentle 1/8
        // continuation residual on top within [-192, +192].
        const LargeTables& tables = *large_;
        const std::int32_t pawn =
            tables.pawn_correction[correction_slot(pawn_key, kPawnCorrectionHistorySize)];
        const std::int32_t material =
            tables.material_correction[correction_slot(material_key, kMaterialCorrectionHistorySize)];
        const std::int32_t king =
            tables.king_correction[correction_slot(king_key, kKingCorrectionHistorySize)];
        const std::int32_t minor =
            tables.minor_correction[correction_slot(minor_key, kMinorCorrectionHistorySize)];
        const std::int32_t major =
            tables.major_correction[correction_slot(major_key, kMajorCorrectionHistorySize)];
        const std::int32_t continuation = has_continuation ?
            static_cast<std::int32_t>(tables.continuation_correction[correction_slot(
                continuation_key, kContinuationCorrectionHistorySize)]) :
            0;
        const std::int32_t total =
            (2 * pawn + 2 * minor + 2 * major + material + king + continuation) / 8;
        return static_cast<int>(std::clamp<std::int32_t>(
            total, -kCorrectionTotalLimit, kCorrectionTotalLimit));
    }

#undef KOI_TABLES_CORR_INLINE
    void update_correction(std::uint64_t pawn_key, std::uint64_t material_key,
                           std::uint64_t king_key, int bonus) noexcept;
    void update_correction(std::uint64_t pawn_key, std::uint64_t material_key,
                           std::uint64_t king_key, std::uint64_t minor_key,
                           std::uint64_t major_key, int bonus,
                           std::uint64_t continuation_key = 0,
                           bool has_continuation = false) noexcept;

    void record_quiet_cutoff(Color side, Move move, int ply, int depth,
                             std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_cutoff(Color side, const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& context) noexcept;
    void record_quiet_best(Color side, const MoveMetadata& metadata, int depth,
                           const SearchHistoryContext& context) noexcept;
    void record_quiet_fail(Color side, Move move, int depth,
                           std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_fail(Color side, const MoveMetadata& metadata, int depth,
                           const SearchHistoryContext& context) noexcept;
    void record_capture_cutoff(const MoveMetadata& metadata, int depth,
                               const SearchHistoryContext& context) noexcept;
    void record_capture_best(const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& context) noexcept;
    void record_capture_fail(const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& context) noexcept;

    // Parent-move feedback: the quiet move that led to a node is rewarded when
    // the node fails low (the move caused the failure) and penalized when the
    // node finds a refutation. Updates land in the same tables that
    // quiet_history_score() consults when that move is ordered again. Both
    // updates are deliberately weaker than direct cutoff/failure evidence.
    void record_parent_fail_low(Color side, Move move, PieceType piece, int parent_ply,
                                const SearchHistoryContext& context, int depth) noexcept;
    void record_parent_refuted(Color side, Move move, PieceType piece, int parent_ply,
                               const SearchHistoryContext& context, int depth) noexcept;

private:
    static constexpr int kMaximumPly = 64;
    // Killers are per-ply state; the search stack is 128 frames deep and a deep
    // path can exceed 64 plies through extensions.  Size the killer table for
    // the real stack instead of folding plies >= 63 onto one slot.
    static constexpr int kMaximumKillerPly = 128;
    static constexpr std::size_t kMoveTableSize = 64 * 64;
    static constexpr std::size_t kPieceTypeCount = 7;
    static constexpr std::size_t kCapturedPieceCount = 7;
    static constexpr std::size_t kCaptureHistorySize =
        kPieceTypeCount * 64 * kCapturedPieceCount;
    static constexpr std::size_t kPieceToHistorySize = kPieceTypeCount * 64;
    static constexpr std::size_t kLowPlyHistoryPlies = 5;
    static constexpr std::size_t kPawnHistorySize = 32 * 1024;
    static constexpr std::size_t kContinuationHistorySize = 16 * 1024;
    static constexpr std::size_t kPawnCorrectionHistorySize = 16 * 1024;
    static constexpr std::size_t kMaterialCorrectionHistorySize = 8 * 1024;
    static constexpr std::size_t kKingCorrectionHistorySize = 8 * 1024;
    static constexpr std::size_t kMinorCorrectionHistorySize = 16 * 1024;
    static constexpr std::size_t kMajorCorrectionHistorySize = 16 * 1024;
    static constexpr std::size_t kContinuationCorrectionHistorySize = 16 * 1024;
    // Each correction entry saturates at the entry limit; the combined static
    // evaluation shift is clamped to the total limit so correction remains a
    // bounded residual adjustment rather than replacing the evaluator.
    static constexpr int kCorrectionEntryLimit = 192;
    static constexpr int kCorrectionTotalLimit = 192;
    static constexpr int kCounterMoveMinimumConfidence = 7 * 7;

    [[nodiscard]] static constexpr int piece_index(const PieceType type) noexcept {
        const int index = static_cast<int>(type);
        return index >= 0 && index < static_cast<int>(kPieceTypeCount) ? index : 0;
    }
    [[nodiscard]] static std::size_t move_index(Move move) noexcept;
    [[nodiscard]] static constexpr std::size_t capture_index(
        const MoveMetadata& metadata) noexcept {
        const std::size_t piece = static_cast<std::size_t>(piece_index(metadata.moving_piece));
        const std::size_t to = metadata.move.to().index() < Square::kInvalid ?
            static_cast<std::size_t>(metadata.move.to().index()) : 0U;
        const std::size_t captured =
            static_cast<std::size_t>(piece_index(metadata.captured_piece));
        return (piece * 64U + to) * kCapturedPieceCount + captured;
    }
    [[nodiscard]] static std::size_t piece_to_index(PieceType piece, Square to) noexcept;
    [[nodiscard]] static std::size_t continuation_index(Move previous_move,
                                                        Move move) noexcept;
    [[nodiscard]] static std::size_t pawn_index(const SearchHistoryContext& context,
                                                PieceType piece, Square to) noexcept;
    [[nodiscard]] static constexpr std::size_t correction_slot(
        const std::uint64_t key, const std::size_t size) noexcept {
        return static_cast<std::size_t>(ordering_mix64(key) & (size - 1U));
    }
    [[nodiscard]] static constexpr int normalized_ply(const int ply) noexcept {
        return std::clamp(ply, 0, kMaximumKillerPly - 1);
    }
    [[nodiscard]] std::int64_t continuation_history_raw(
        std::size_t move_idx, const SearchHistoryContext& context) const noexcept;
    [[nodiscard]] std::int64_t quiet_history_raw(
        Color side, const MoveMetadata& metadata,
        const SearchHistoryContext& context, std::size_t move_idx,
        std::int64_t cont_score) const noexcept;
    static void update_history(int& score, int delta, int maximum) noexcept;
    static void update_history(std::int16_t& score, int delta, int maximum) noexcept;
    void update_parent_histories(Color side, Move move, PieceType piece, int parent_ply,
                                 const SearchHistoryContext& context, int bonus,
                                 bool continuation_only) noexcept;
    void record_scaled_multi_ply_continuation(
        std::size_t scaled_move, const SearchHistoryContext& context,
        int base_bonus) noexcept;

    struct LargeTables {
        // Leave the arrays uninitialized on allocation; SearchOrderingTables()
        // immediately fills history priors and zeroes the contiguous
        // correction-history tail without double-zeroing.
        LargeTables() noexcept {}
        std::array<std::int16_t, kSearchContinuationPlies * kContinuationHistorySize>
            multi_ply_continuation_history;
        std::array<std::int16_t, kPawnHistorySize> pawn_history;
        std::array<std::int16_t, kPawnCorrectionHistorySize> pawn_correction;
        std::array<std::int16_t, kMaterialCorrectionHistorySize> material_correction;
        std::array<std::int16_t, kKingCorrectionHistorySize> king_correction;
        std::array<std::int16_t, kMinorCorrectionHistorySize> minor_correction;
        std::array<std::int16_t, kMajorCorrectionHistorySize> major_correction;
        std::array<std::int16_t, kContinuationCorrectionHistorySize> continuation_correction;
    };

    std::array<std::array<Move, 2>, kMaximumKillerPly> killers_{};
    std::array<std::array<std::int16_t, kMoveTableSize>, 2> history_{};
    std::array<std::array<Move, kMoveTableSize>, 2> counter_moves_{};
    std::array<std::array<std::int16_t, kMoveTableSize>, 2> counter_confidence_{};
    std::array<std::int16_t, kCaptureHistorySize> capture_history_{};
    std::array<std::int16_t, kPieceToHistorySize> piece_to_history_{};
    std::array<std::int16_t, kLowPlyHistoryPlies * kMoveTableSize> low_ply_history_{};
    std::array<std::int16_t, kContinuationHistorySize> continuation_history_{};
    std::unique_ptr<LargeTables> large_;
};

} // namespace koi::detail
