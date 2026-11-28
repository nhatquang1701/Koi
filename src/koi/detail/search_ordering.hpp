#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "koi/detail/search_ordering_tables.hpp"
#include "koi/detail/static_exchange.hpp"
#include "koi/game_state.hpp"

namespace koi::detail {

inline constexpr int kTtMovePriority = 1'000'000;

[[nodiscard]] std::uint32_t move_tie_break_key(Move move) noexcept;

class SearchMovePicker;

// SearchMoveOrdering is deliberately private to the search implementation.
// It does not change the rules or UCI interfaces.
class SearchMoveOrdering {
public:
    void clear() noexcept;
    void order(const GameState& state, std::vector<Move>& moves,
               std::optional<Move> tt_move, int ply,
               std::optional<Move> previous_move = std::nullopt) const;
    void order(const GameState& state, MoveMetadataList& moves,
               std::optional<Move> tt_move, int ply,
               std::optional<Move> previous_move = std::nullopt) const;
    void order(const GameState& state, MoveMetadataList& moves,
               std::optional<Move> tt_move,
               const SearchHistoryContext& history_context) const;
    void order(const GameState& state, std::vector<MoveMetadata>& moves,
               std::optional<Move> tt_move, int ply,
               std::optional<Move> previous_move = std::nullopt) const;
    void record_quiet_cutoff(Color side, Move move, int ply, int depth,
                             std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_fail(Color side, Move move, int depth,
                           std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_cutoff(Color side, const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& history_context) noexcept;
    void record_quiet_best(Color side, const MoveMetadata& metadata, int depth,
                           const SearchHistoryContext& history_context) noexcept;
    void record_quiet_fail(Color side, const MoveMetadata& metadata, int depth,
                           const SearchHistoryContext& history_context) noexcept;
    void record_capture_cutoff(const MoveMetadata& metadata, int depth,
                               const SearchHistoryContext& history_context) noexcept;
    void record_capture_best(const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& history_context) noexcept;
    void record_capture_fail(const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& history_context) noexcept;
    void record_parent_fail_low(Color side, Move move, PieceType piece, int parent_ply,
                                const SearchHistoryContext& history_context, int depth) noexcept;
    void record_parent_refuted(Color side, Move move, PieceType piece, int parent_ply,
                               const SearchHistoryContext& history_context, int depth) noexcept;
    [[nodiscard]] bool is_killer(Move move, int ply) const noexcept;
    [[nodiscard]] int quiet_history_score(Color side, Move move,
                                          std::optional<Move> previous_move = std::nullopt) const noexcept;
    [[nodiscard]] int quiet_history_score(Color side, const MoveMetadata& metadata,
                                           const SearchHistoryContext& history_context) const noexcept;
    [[nodiscard]] int continuation_history_score(
        const MoveMetadata& metadata, const SearchHistoryContext& history_context) const noexcept;
    [[nodiscard]] QuietAndContinuationScores quiet_and_continuation_scores(
        Color side, const MoveMetadata& metadata,
        const SearchHistoryContext& history_context) const noexcept;
    [[nodiscard]] int capture_history_score(const MoveMetadata& metadata) const noexcept;
    [[nodiscard]] bool is_proven_counter_move(
        Color side, Move previous_move, Move move) const noexcept;
#if defined(__clang__) || defined(__GNUC__)
#define KOI_ORD_CORR_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define KOI_ORD_CORR_INLINE __forceinline
#else
#define KOI_ORD_CORR_INLINE inline
#endif

    [[nodiscard]] KOI_ORD_CORR_INLINE int correction_value(
        std::uint64_t pawn_key, std::uint64_t material_key,
        std::uint64_t king_key) const noexcept {
        return tables_.correction_value(pawn_key, material_key, king_key);
    }
    [[nodiscard]] KOI_ORD_CORR_INLINE int correction_value(
        std::uint64_t pawn_key, std::uint64_t material_key,
        std::uint64_t king_key, std::uint64_t minor_key,
        std::uint64_t major_key, std::uint64_t continuation_key = 0,
        bool has_continuation = false) const noexcept {
        return tables_.correction_value(pawn_key, material_key, king_key, minor_key,
                                        major_key, continuation_key, has_continuation);
    }

#undef KOI_ORD_CORR_INLINE
    void update_correction(std::uint64_t pawn_key, std::uint64_t material_key,
                           std::uint64_t king_key, int bonus) noexcept;
    void update_correction(std::uint64_t pawn_key, std::uint64_t material_key,
                           std::uint64_t king_key, std::uint64_t minor_key,
                           std::uint64_t major_key, int bonus,
                           std::uint64_t continuation_key = 0,
                           bool has_continuation = false) noexcept;

private:
    friend class SearchMovePicker;

    [[nodiscard]] int priority(const GameState& state, const MoveMetadata& metadata,
                               std::optional<Move> tt_move, int ply,
                               std::optional<Move> previous_move) const;
    [[nodiscard]] int priority(const GameState& state, const MoveMetadata& metadata,
                               std::optional<Move> tt_move,
                               const SearchHistoryContext& history_context) const;

    template <typename PriorityFn>
    void order_with_priority(const GameState& state, std::span<MoveMetadata> moves,
                             PriorityFn&& priority_fn) const;

    struct ScoredMove {
        MoveMetadata metadata{};
        int priority = 0;
        std::uint32_t tie_break = 0;
    };

    SearchOrderingTables tables_;
};

// SearchMovePicker mirrors Stockfish's staged MovePicker while retaining
// Koi's fixed metadata container and deterministic tie-breaking. It owns only
// bounded stack storage; all history state remains in the parent ordering
// object and all move legality remains authoritative in GameState metadata.
class SearchMovePicker {
public:
    enum class Mode : std::uint8_t { main, quiescence, evasion };

    SearchMovePicker(const SearchMoveOrdering& ordering, const GameState& state,
                     const MoveMetadataList& moves, std::optional<Move> tt_move,
                     const SearchHistoryContext* history_context, Mode mode,
                     Move excluded_move = Move::no_move(),
                     MoveMetadataList* mutable_moves = nullptr) noexcept;

    // Emits the next candidate, or nullptr when no stage remains.  The
    // returned record lives in the picker's emission scratch and is valid
    // only until the following next() call overwrites it: callers bind it per
    // iteration and finish with it before advancing the picker.  A picker
    // instance is driven from one place at a time, and recursive nodes use
    // their own picker objects, so live records never share scratch.
    [[nodiscard]] const MoveMetadata* next();

    // Called by a cut node after enough forcing moves have been searched. The
    // current candidate is preserved, while later quiet stages are skipped in
    // the same direction as Stockfish's skip_quiet_moves().
    void skip_quiet_moves() noexcept { skip_quiets_ = true; }

private:
    enum class Stage : std::uint8_t {
        tt,
        good_captures,
        good_quiets,
        bad_captures,
        bad_quiets,
        quiet_checks,
        evasions,
        done,
    };

    // Deliberately without default member initializers: `staged_` is written
    // for every candidate before any entry is read, and a picker is built once
    // per node, so zeroing 256 records here would be pure per-node overhead.
    // Bits [63..32] carry the inverted signed priority, bits [31..9] carry the
    // tie-break key, and bits [8..0] carry the source index into `moves_`.
    using Candidate = std::uint64_t;
    static_assert(kMaximumLegalMoves <= 512U,
                  "Candidate source_index packing requires <= 512 legal moves");

    static constexpr std::size_t kStageRankCount =
        static_cast<std::size_t>(Stage::done) + 1U;

#if defined(__clang__) || defined(__GNUC__)
#define KOI_PICKER_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define KOI_PICKER_NOINLINE __declspec(noinline)
#else
#define KOI_PICKER_NOINLINE
#endif

    [[nodiscard]] bool is_excluded(std::size_t index) const noexcept;
    [[nodiscard]] bool is_good_capture(const MoveMetadata& metadata) const noexcept;
    [[nodiscard]] Stage first_stage() const noexcept;
    [[nodiscard]] MoveMetadata materialize(std::size_t index);
    KOI_PICKER_NOINLINE void prepare_quiescence_candidates();
    KOI_PICKER_NOINLINE void prepare_evasion_candidates();
    KOI_PICKER_NOINLINE void prepare_main_captures();
    KOI_PICKER_NOINLINE void prepare_main_quiets();
    template <bool AllCont, bool SkipLowPly>
    void prepare_main_quiets_impl(Color side, const SearchOrderingTables::QuietHistoryBatch& quiet_batch);
    KOI_PICKER_NOINLINE void score_quiescence_quiet_checks();
    [[nodiscard]] std::size_t pick_next_in_stage(std::size_t rank) noexcept;
#undef KOI_PICKER_NOINLINE
    void advance_stage() noexcept;
    [[nodiscard]] const MoveMetadata* emit_tt();
    [[nodiscard]] int resolved_capture_priority(std::size_t source_index,
                                                std::uint64_t candidate) const;

    // Compact bitset whose words are deliberately not zeroed by construction.
    // std::bitset's default constructor always clears its storage, which
    // would charge every quiescence/evasion picker construction for bits only
    // a main-mode picker can read (see materialize); the picker constructor
    // calls reset() for main mode instead.  No accessor below is reached
    // before that reset has run.
    class LazyBitset {
    public:
        void reset() noexcept { words_.fill(0); }

        [[nodiscard]] bool test(const std::size_t index) const noexcept {
            return ((words_[index / kWordBits] >> (index % kWordBits)) & 1U) != 0U;
        }

        void set(const std::size_t index) noexcept {
            words_[index / kWordBits] |= std::uint64_t{1} << (index % kWordBits);
        }

    private:
        static constexpr std::size_t kWordBits = 64U;
        static constexpr std::size_t kWordCount =
            (kMaximumLegalMoves + kWordBits - 1U) / kWordBits;
        // Deliberately uninitialized: the main-mode constructor reset() starts
        // these words before any test()/set() touches them.
        std::array<std::uint64_t, kWordCount> words_;
    };

    const SearchMoveOrdering& ordering_;
    const GameState& state_;
    const MoveMetadataList& moves_;
    // Borrowed, not owned: the constructor pointer must refer to a context the
    // caller keeps alive through the picker loop, so the picker never carries
    // a copy of the 96-byte record.
    const SearchHistoryContext* history_context_;
    std::optional<Move> tt_move_;
    Move excluded_move_;
    Mode mode_ = Mode::main;
    Stage stage_ = Stage::done;
    // The bitsets below must start zeroed: they are the guards that decide
    // whether a lazy SEE/check score has been computed yet.  The score arrays
    // are always written under those guards before being read, so they stay
    // uninitialized to avoid clearing several kilobytes at every node.
    std::bitset<kMaximumLegalMoves> emitted_{};
    std::bitset<kMaximumLegalMoves> see_computed_{};
    // The capture-check guard is read only by main-mode pickers (materialize's
    // capture-check probe), so only a main-mode picker zeroes it: quiescence
    // and evasion constructions leave these words unconstructed instead of
    // clearing two bitsets they can never observe.
    LazyBitset capture_check_computed_;
    LazyBitset capture_checks_;
    std::array<std::int16_t, kMaximumLegalMoves> see_scores_;
    // Exchange baseline for this picker, built lazily on the first capture
    // whose SEE is not already available and reused for every later capture.
    // It cannot go stale: `state_` is only mutated by the search's
    // transactional make/unmake, which completes between next() calls
    // (position.cpp restores the pre-move snapshot on unmake and rolls a
    // rejected make back), so the baseline still describes the position the
    // picker was built for.
    std::optional<ExchangeContext> see_context_;
    std::array<Candidate, kMaximumLegalMoves> staged_;
    // Candidates are grouped into disjoint stage ranges and selected
    // incrementally (or sorted lazily past a small prefix), so stages and late
    // moves skipped by cutoffs never pay for full-list scoring or introsort.
    std::array<std::uint16_t, kStageRankCount> stage_begin_{};
    std::array<std::uint16_t, kStageRankCount> stage_end_{};
    std::array<std::uint16_t, kStageRankCount> stage_cursor_{};
    std::bitset<kStageRankCount> stage_sorted_{};
    // The good-capture pass is already ordered by the same history/MVV score
    // used by the bad-capture pass. Keep only the staged slot of each deferred
    // capture instead of copying the complete Candidate record a second time.
    std::array<std::uint16_t, kMaximumLegalMoves> deferred_bad_capture_indices_;
    // Emission scratch for next(): every return path writes it before the
    // pointer is handed out, and the following emitted record overwrites it.
    // Deliberately uninitialized: MoveMetadata has no meaningful empty value
    // here and the record is always assigned before it can be read.
    MoveMetadata emitted_metadata_;
    MoveMetadataList* mutable_moves_ = nullptr;
    std::size_t main_quiet_start_ = 0;
    std::size_t main_captures_end_ = 0;
    std::size_t deferred_bad_capture_count_ = 0;
    std::size_t deferred_bad_capture_index_ = 0;
    bool captures_prepared_ = false;
    bool quiets_prepared_ = false;
    bool quiet_checks_scored_ = false;
    bool tt_emitted_ = false;
    bool skip_quiets_ = false;
    bool defer_quiets_ = false;
};

} // namespace koi::detail
