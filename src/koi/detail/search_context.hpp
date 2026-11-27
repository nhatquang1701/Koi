#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "koi/detail/search_constants.hpp"
#include "koi/detail/search_context_support.hpp"
#include "koi/detail/evaluation_context.hpp"
#include "koi/detail/search_ordering.hpp"
#include "koi/detail/search_policy.hpp"
#include "koi/detail/search_budget.hpp"
#include "koi/detail/search_stack.hpp"
#include "koi/detail/search_table_access.hpp"
#include "koi/evaluator.hpp"
#include "koi/search_types.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace koi::detail {

[[nodiscard]] constexpr bool is_claimable_draw_status(
    const DrawStatus status) noexcept {
    return status == DrawStatus::claimable_threefold ||
        status == DrawStatus::claimable_fifty_move;
}

[[nodiscard]] constexpr bool is_forced_draw_status(
    const DrawStatus status) noexcept {
    return status == DrawStatus::dead_position ||
        status == DrawStatus::automatic_fivefold ||
        status == DrawStatus::automatic_seventy_five_move;
}

// Interior tablebase probing configuration shared by every worker in a search.
// The table pointer and hook are read-only during the search; each context
// takes its own copy so workers never share mutable probe state.
struct TablebaseSearchBinding {
    const SyzygyTablebase* table = nullptr;
    int interior_depth = 0;
    bool fifty_move_rule = true;
    SearchOptions::TablebaseProbeHook probe_hook;
};

struct SearchContext {
    // History maluses are deferred until a node has selected its final best
    // move.  They do not need SEE, provenance, or ordering data; retaining a
    // compact record keeps the recursive stack bounded when a node has many
    // legal moves while preserving the exact metadata dimensions used by the
    // ordering tables.
    // Deliberately without default member initializers: `failed_moves` is a
    // per-node stack array whose entries are only read below failed_move_count,
    // so clearing it would cost several kilobytes at every node.
    struct DeferredHistoryMove {
        Move move;
        PieceType moving_piece;
        PieceType captured_piece;
        MoveKind kind;
        Color history_side;
        bool authoritative;

        [[nodiscard]] static DeferredHistoryMove from(
            const MoveMetadata& metadata, const Color side = Color::white,
            const bool child_authoritative = true) noexcept {
            return DeferredHistoryMove{
                metadata.move, metadata.moving_piece, metadata.captured_piece,
                metadata.kind, side, child_authoritative};
        }

        [[nodiscard]] MoveMetadata metadata() const noexcept {
            MoveMetadata result{};
            result.move = move;
            result.moving_piece = moving_piece;
            result.captured_piece = captured_piece;
            result.kind = kind;
            return result;
        }
    };

    struct RootMoveScore {
        Move move = Move::no_move();
        int score = -kInfinity;
        bool exact = false;
        // A selective child can still be a safe upper bound for the
        // root move when it failed low against the incumbent. Keep that
        // direction separate from `exact`: the serial root may ignore such a
        // move while an unresolved selective challenger must block authority.
        bool selective_bound = false;
        bool safe_upper_bound = false;
        bool selective_upper_bound = false;
        std::uint64_t visited_nodes = 0;
    };

    struct RootVerification {
        int score = -kInfinity;
        PrincipalVariation pv;
        bool authoritative = false;
        // Coverage and a legal root PV make the returned score usable as a
        // move-selection estimate. It is not a directional score proof.
        bool line_complete = false;

        [[nodiscard]] bool proves_losing_mate() const noexcept {
            return authoritative && score <= -kMateThreshold;
        }
    };

    struct EvaluationCacheEntry {
        std::uint64_t key = 0;
        int score = 0;
        bool valid = false;
    };

    struct QSearchCacheEntry {
        std::uint64_t key = 0;
        int score = 0;
        bool valid = false;
        bool lower_bound = false;
    };

    struct PathFlagGuard {
        bool* output = nullptr;
        const bool* value = nullptr;

        ~PathFlagGuard() noexcept {
            if (output != nullptr && value != nullptr) {
                *output = *value;
            }
        }
    };

    // Direct-mapped static-evaluation cache.  Iterative deepening re-evaluates
    // the same subtree repeatedly, so collisions between distinct positions
    // cost real evaluator work; 16K entries keep the working set of a search
    // comfortably inside the table on this hardware.
    static constexpr std::size_t kEvaluationCacheSize = 16 * 1024;
    // 16K entries (256 KiB) keeps the direct-mapped quotient-cache collision
    // rate low across iterative deepening; the entries are exact-key checked,
    // so the size only trades memory for hit rate.
    static constexpr std::size_t kQSearchCacheSize = 16 * 1024;
    // Upper bound on how many `interrupted()`/`count_node()` clock polls may
    // skip the wall-clock read. Every call still inspects the atomic stop
    // flags, so an external stop is immediate; only a timer deadline can be
    // observed a few hundred microseconds late.
    static constexpr int kClockPollInterval = 256;

    static constexpr std::size_t stack_capacity() noexcept {
        return SearchStack::kCapacity;
    }

    static constexpr bool has_evaluation_context() noexcept {
        return true;
    }

    EvaluationContext evaluation;
    SearchTableAccess table_access;
    SearchBudget budget;
    TimeManager& time_manager;
    std::atomic_bool& stop_requested;
    const MoveMetadataList* root_moves = nullptr;
    // Root perspective the draw bias is resolved against.
    Color root_color = Color::white;
    int contempt_cp = 0;
    std::atomic_bool* iteration_aborted = nullptr;
    SearchOptions::QuietHistorySideHook quiet_history_side_hook;
    TablebaseSearchBinding tablebase;
    SearchMoveOrdering ordering;
    SearchStats stats;
    bool aborted = false;
    bool allow_root_forcing_extension = false;
    // While a null-move verification search runs, null pruning stays disabled
    // at plies below this floor (Stockfish's nmpMinPly).  Keeping most of the
    // verification subtree null-free stops the reduced probe from confirming
    // itself through the same heuristic it is meant to validate.
    int null_verification_min_ply = 0;
    int clock_poll_countdown = 0;
    int quiescence_check_depth_limit = kMaximumQuiescenceCheckDepth;
    SearchStack stack;
    struct FrameScratch;
    struct FrameScratchDeleter {
        void operator()(FrameScratch* ptr) const noexcept;
    };
    std::unique_ptr<FrameScratch, FrameScratchDeleter> frame_scratch;
    std::unique_ptr<EvaluationCacheEntry[]> evaluation_cache;
    std::array<QSearchCacheEntry, kQSearchCacheSize> qsearch_cache{};
    std::array<RootMoveScore, kMaximumLegalMoves> root_move_scores{};
    std::size_t root_move_score_count = 0;
    // A previous completed root move remains a useful ordering hint even when
    // the shared TT entry was displaced or hash use is disabled. The service
    // sets this only between completed iterations; the first iteration keeps
    // the normal generated-move ordering.
    std::optional<Move> root_move_hint;
    // Optional progress hook for interim UCI publication, invoked on the
    // runner thread from the clock-poll slow path. Only the runner's main
    // search context ever sets it; helper and parallel-worker contexts leave
    // it empty so every protocol line stays runner-thread-only. The hook is
    // observation-only: it may never change search control flow, and any
    // exception it throws is swallowed.
    std::function<void()> progress_probe;

    // Populated only when an opt-in developer profile sink is installed.
    // Regular engine searches never allocate per-root-move profile rows.
    bool profile_enabled = false;
    int profile_depth = 0;
    std::uint32_t profile_root_pass = 0;
    std::uint64_t profile_iteration_start_nodes = 0;
    std::uint64_t profile_iteration_start_qnodes = 0;
    std::array<std::uint64_t, 17> profile_qnodes_by_depth{};
    std::vector<SearchRootMoveProfile> profile_root_moves;
    Move profile_current_root_move = Move::no_move();
    int profile_current_root_move_number = 0;
    std::uint64_t profile_current_root_move_nodes = 0;
    std::uint64_t profile_current_root_move_qnodes = 0;
    std::uint64_t profile_current_root_move_tt_hits = 0;
    std::chrono::steady_clock::time_point profile_current_root_move_started{};

    void begin_profile_iteration(const int depth) noexcept {
        if (!profile_enabled) {
            return;
        }
        profile_depth = depth;
        profile_root_pass = 0;
        profile_iteration_start_nodes = stats.nodes;
        profile_iteration_start_qnodes = stats.qnodes;
        profile_qnodes_by_depth.fill(0);
        profile_root_moves.clear();
        profile_current_root_move = Move::no_move();
        profile_current_root_move_number = 0;
    }

    void begin_profile_root_pass() noexcept {
        if (profile_enabled) {
            ++profile_root_pass;
            profile_current_root_move = Move::no_move();
            profile_current_root_move_number = 0;
        }
    }

    void begin_profile_root_move(const Move move, const int move_number) noexcept {
        if (!profile_enabled) {
            return;
        }
        profile_current_root_move = move;
        profile_current_root_move_number = move_number;
        profile_current_root_move_nodes = stats.nodes;
        profile_current_root_move_qnodes = stats.qnodes;
        profile_current_root_move_tt_hits = stats.tt_hits;
        profile_current_root_move_started = std::chrono::steady_clock::now();
    }

    void finish_profile_root_move(const bool completed) noexcept {
        if (!profile_enabled || profile_current_root_move.is_no_move()) {
            return;
        }
        SearchRootMoveProfile row;
        row.pass = profile_root_pass;
        row.move_number = profile_current_root_move_number;
        row.move = profile_current_root_move;
        row.nodes = stats.nodes - profile_current_root_move_nodes;
        row.qnodes = stats.qnodes - profile_current_root_move_qnodes;
        row.tt_hits = stats.tt_hits - profile_current_root_move_tt_hits;
        row.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - profile_current_root_move_started);
        row.completed = completed;
        try {
            profile_root_moves.push_back(row);
        } catch (...) {
            // Diagnostic allocation failures must not change search behavior.
        }
        profile_current_root_move = Move::no_move();
        profile_current_root_move_number = 0;
    }

    SearchContext(const Evaluator& evaluator, TranspositionTable& table, TimeManager& time_manager,
                  std::atomic_bool& stop_requested,
                  std::atomic<std::uint64_t>* global_nodes = nullptr,
                  std::mutex* evaluator_mutex = nullptr,
                  const MoveMetadataList* root_moves = nullptr,
                  bool use_transposition_table = true,
                  SearchOptions::QuietHistorySideHook quiet_history_side_hook = {},
                  TablebaseSearchBinding tablebase_binding = {},
                  Color root_color = Color::white, int contempt_cp = 0)
        : evaluation(evaluator, evaluator_mutex), table_access(table, use_transposition_table),
          budget(time_manager, global_nodes), time_manager(time_manager),
          stop_requested(stop_requested), root_moves(root_moves),
          root_color(root_color), contempt_cp(contempt_cp),
          quiet_history_side_hook(std::move(quiet_history_side_hook)),
          tablebase(std::move(tablebase_binding)),
          frame_scratch(create_frame_scratch()),
          evaluation_cache(std::make_unique<EvaluationCacheEntry[]>(kEvaluationCacheSize)) {
        if (const auto budget = time_manager.time_budget(); budget.has_value() &&
            *budget <= kShortTimedSerialThreshold) {
            // A short timed search must finish its first root iteration before
            // spending time on the deepest quiet-checking layer. Fixed-depth
            // searches remain on the full horizon.
            quiescence_check_depth_limit = std::max(1, kMaximumQuiescenceCheckDepth - 1);
        }
    }

    [[nodiscard]] static std::unique_ptr<FrameScratch, FrameScratchDeleter> create_frame_scratch();
    ~SearchContext();

    [[nodiscard]] Color history_side(Color candidate, const bool after_unmake) const noexcept {
        if (!quiet_history_side_hook) [[likely]] {
            return candidate;
        }
        return history_side_slow(candidate, after_unmake);
    }

    [[nodiscard]] Color history_side_slow(Color candidate, bool after_unmake) const noexcept;

    // Interior Syzygy WDL probe.  Returns a decisive side-to-move score below
    // the mate threshold, or nullopt when no cutoff applies.  A configured
    // probe hook fully overrides real tablebase probing.  Never throws.
    [[nodiscard]] std::optional<int> probe_tablebase(const GameState& state, int depth);

    void begin_iteration(std::atomic_bool* shared_abort) noexcept {
        stats = {};
        aborted = false;
        iteration_aborted = shared_abort;
        root_move_score_count = 0;
        stack.reset();
    }

    [[nodiscard]] bool root_result_complete(const PrincipalVariation& pv) const noexcept {
        if (aborted || root_moves == nullptr || pv.length == 0 ||
            root_move_score_count != root_moves->size()) {
            return false;
        }
        // Every root score is recorded only after its child has returned. A
        // complete score set plus a PV whose first move belongs to that set
        // establishes coverage; callers may then apply the provenance rule
        // appropriate to publication versus strict root authority.
        return std::any_of(
            root_moves->begin(), root_moves->end(), [&pv](const MoveMetadata& metadata) {
                return metadata.move == pv.moves[0];
            });
    }

    [[nodiscard]] std::optional<RootMoveScore> best_completed_root_move(
        const GameState& root, const bool avoid_check_exposure) const noexcept {
        if (root_move_score_count == 0) {
            return std::nullopt;
        }
        const auto metadata_for = [this](const Move move) -> const MoveMetadata* {
            if (root_moves == nullptr) {
                return nullptr;
            }
            const auto match = std::find_if(root_moves->begin(), root_moves->end(),
                                            [move](const MoveMetadata& metadata) {
                                                return metadata.move == move;
                                            });
            return match == root_moves->end() ? nullptr : &*match;
        };
        const auto is_losing_capture = [&metadata_for, &root](const Move move) {
            const MoveMetadata* metadata = metadata_for(move);
            return metadata != nullptr && metadata->is_capture() && metadata->see_computed &&
                metadata->see_score < 0 && !metadata->gives_check &&
                !root.move_gives_check(move);
        };
        const auto is_forcing = [&metadata_for](const Move move) {
            const MoveMetadata* metadata = metadata_for(move);
            return metadata != nullptr &&
                (metadata->is_capture() || metadata->gives_check ||
                 metadata->move.promotion() != Promotion::none);
        };
        std::optional<RootMoveScore> best;
        for (std::size_t index = 0; index < root_move_score_count; ++index) {
            const RootMoveScore candidate = root_move_scores[index];
            if (!candidate.exact) {
                continue;
            }
            const MoveMetadata* candidate_metadata = metadata_for(candidate.move);
            if (avoid_check_exposure && candidate_metadata != nullptr &&
                root_move_exposes_immediate_check(root, *candidate_metadata)) {
                continue;
            }
            if (is_losing_capture(candidate.move)) {
                continue;
            }
            if (!best.has_value()) {
                best = candidate;
                continue;
            }
            const RootMoveScore current_best = *best;
            const bool candidate_forcing = is_forcing(candidate.move);
            const bool best_forcing = is_forcing(current_best.move);
            if (best_forcing && !candidate_forcing &&
                candidate.score <= current_best.score + kIncompleteRootForcingMargin) {
                continue;
            }
            if (candidate.score > current_best.score ||
                (candidate.score >= current_best.score - kIncompleteRootForcingMargin &&
                 candidate_forcing && !best_forcing)) {
                best = candidate;
            }
        }
        return best;
    }

    [[nodiscard]] RootVerification selectively_research_root_move(
        GameState& root, const MoveMetadata& metadata, int depth) {
        MoveMetadataList single_move;
        (void)single_move.push_back(metadata);
        const MoveMetadataList* saved_root_moves = root_moves;
        const bool saved_use_transposition_table = table_access.enabled();
        root_moves = &single_move;
        // The first shallow root pass stores exact child entries at the same
        // depth that this verification is meant to extend.  Reusing those
        // entries would make the "deeper" comparison identical to the
        // shallow score and hide the tactical consequence being tested.
        table_access.set_enabled(false);
        RootVerification verification;
        bool selective_bound = false;
        try {
            verification.score = negamax(root, depth, -kInfinity, kInfinity, 0,
                                         verification.pv, std::nullopt, true,
                                         kMaximumCheckExtensionsPerPath, Move::no_move(),
                                         nullptr, &selective_bound);
            verification.line_complete = root_result_complete(verification.pv);
            verification.authoritative = verification.line_complete && !selective_bound;
        } catch (...) {
            root_moves = saved_root_moves;
            table_access.set_enabled(saved_use_transposition_table);
            throw;
        }
        root_moves = saved_root_moves;
        table_access.set_enabled(saved_use_transposition_table);
        return verification;
    }

    void request_abort() noexcept {
        aborted = true;
        if (iteration_aborted != nullptr) {
            iteration_aborted->store(true, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::uint64_t visited_nodes() const noexcept {
        return budget.visited(stats.nodes + stats.qnodes);
    }

    // Samples the external stop flags once and records the abort. count_node()
    // and interrupted() share it so a node pays one flag load pair; the clock
    // poll below keeps its own cadence.
    [[nodiscard]] bool stop_signal_pending() noexcept {
        if (stop_requested.load(std::memory_order_relaxed) ||
            (iteration_aborted != nullptr && iteration_aborted->load(std::memory_order_relaxed))) {
            aborted = true;
            return true;
        }
        return false;
    }

    // Wall-clock polling separated from the flag check so count_node() can
    // sample the stop flags once per node instead of twice. The countdown
    // decrement and the kClockPollInterval reset are unchanged. The progress
    // probe runs after the stop handling so it can never delay a deadline,
    // and a failing probe must not alter the search result.
    [[nodiscard]] bool poll_clock() noexcept {
        if (clock_poll_countdown > 0) {
            --clock_poll_countdown;
            return false;
        }
        clock_poll_countdown = kClockPollInterval;
        return poll_clock_slow();
    }

    [[nodiscard]] bool poll_clock_slow() noexcept;
    void reinforce_tt_lower_quiet(const GameState& state, Move best_move, int depth, int ply,
                                  std::optional<Move> previous_move, Color side_to_move,
                                  bool moves_generated, const MoveMetadataList& moves,
                                  bool& history_ready, SearchHistoryContext& history_out,
                                  std::uint64_t& node_pawn_key) noexcept;
    void compute_root_authoritative(std::size_t legal_move_count, Move best_move, int best_score,
                                    bool best_metadata_valid, bool best_score_authoritative,
                                    bool best_score_safe_lower_bound, bool claimable_draw,
                                    bool& root_authoritative_path) noexcept;

    [[nodiscard]] bool interrupted() noexcept {
        return stop_signal_pending() || poll_clock();
    }

    [[nodiscard]] bool count_node(bool quiescence) noexcept {
        if (stop_signal_pending()) {
            return false;
        }
        if (!budget.reserve(stats.nodes + stats.qnodes)) {
            request_abort();
            return false;
        }
        if (quiescence) {
            ++stats.qnodes;
        } else {
            ++stats.nodes;
        }
        return !poll_clock();
    }

    [[nodiscard]] int terminal_score(const GameState& state, std::size_t move_count,
                                     int ply) const noexcept {
        if (move_count != 0) {
            return 0;
        }
        return state.in_check() ? -kMateScore + ply : draw_value(state);
    }

    // Draw score from the side to move's perspective, biased against the root
    // side by `contempt_cp`.  Zero contempt keeps the historical neutral draw.
    [[nodiscard]] int draw_value(const GameState& state) const noexcept {
        return state.side_to_move() == root_color ? -contempt_cp : contempt_cp;
    }

    [[nodiscard]] int evaluate(const GameState& state, Color perspective) {
        const std::uint64_t key = state.position_key() ^
            (perspective == Color::black ? 0xD6E8FEB86659FD93ULL : 0ULL);
        EvaluationCacheEntry& entry =
            evaluation_cache[static_cast<std::size_t>(key) & (kEvaluationCacheSize - 1)];
        if (entry.valid && entry.key == key) {
            ++stats.evaluation_cache_hits;
            return entry.score;
        }

        ++stats.evaluation_cache_misses;
        const int score = evaluation.evaluate(state, perspective);
        entry = EvaluationCacheEntry{key, score, true};
        return score;
    }

    // Bounded static-eval correction history.  The keys are cheap structural
    // fingerprints (pawn structure, material counts, side-to-move king square)
    // and the learned correction is applied to the raw evaluator score only:
    // transposition entries and every search score keep their own semantics.
    struct CorrectionKeys {
        std::uint64_t pawn = 0;
        std::uint64_t material = 0;
        std::uint64_t king = 0;
        std::uint64_t minor = 0;
        std::uint64_t major = 0;
        std::uint64_t continuation = 0;
        bool has_continuation = false;
    };

#if defined(__clang__) || defined(__GNUC__)
#define KOI_CTX_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define KOI_CTX_ALWAYS_INLINE __forceinline
#else
#define KOI_CTX_ALWAYS_INLINE inline
#endif

    KOI_CTX_ALWAYS_INLINE static void init_correction_keys(
        const GameState& state, std::uint64_t pawn_key,
        CorrectionKeys& keys, SearchFrame* frame = nullptr,
        Move two_ply_ago_move = Move::no_move()) noexcept {
        // Static evaluations and residuals are side-to-move-relative, so every
        // correction key must isolate White-to-move and Black-to-move entries.
        const std::uint64_t stm_salt = state.side_to_move() == Color::black ?
            0x9E3779B97F4A7C15ULL : 0ULL;
        keys.pawn = pawn_key ^ stm_salt;
        const auto& bitboards = state.piece_bitboards();
        // Material signature: FNV-1a over the ten piece counts.  Since quiet
        // non-promoting moves leave piece counts unchanged, cache it across
        // quiet subtrees via SearchFrame rather than running 10 popcounts +
        // dependent FNV multiplies at every interior node.
        std::uint64_t signature = 0;
        if (frame != nullptr && frame->material_signature_valid) {
            signature = frame->material_signature;
        } else {
            signature = 0xCBF29CE484222325ULL;
            constexpr std::array<PieceType, 5> kTypes{PieceType::pawn, PieceType::knight,
                                                      PieceType::bishop, PieceType::rook,
                                                      PieceType::queen};
            for (const PieceType type : kTypes) {
                const std::size_t type_index = static_cast<std::size_t>(type);
                for (std::size_t color_index = 0; color_index < 2U; ++color_index) {
                    const std::uint64_t count = static_cast<std::uint64_t>(
                        std::popcount(bitboards[color_index][type_index]));
                    signature = (signature ^ count) * 0x100000001B3ULL;
                }
            }
            if (frame != nullptr) {
                frame->material_signature = signature;
                frame->material_signature_valid = true;
            }
        }
        keys.material = signature ^ stm_salt;
        constexpr std::size_t kKnightIdx = static_cast<std::size_t>(PieceType::knight);
        constexpr std::size_t kBishopIdx = static_cast<std::size_t>(PieceType::bishop);
        constexpr std::size_t kRookIdx = static_cast<std::size_t>(PieceType::rook);
        constexpr std::size_t kQueenIdx = static_cast<std::size_t>(PieceType::queen);
        constexpr std::size_t kKingIdx = static_cast<std::size_t>(PieceType::king);
        const std::uint64_t wk_bb = bitboards[0][kKingIdx];
        const std::uint64_t bk_bb = bitboards[1][kKingIdx];
        const std::uint64_t wk_sq = wk_bb == 0 ?
            64ULL : static_cast<std::uint64_t>(std::countr_zero(wk_bb));
        const std::uint64_t bk_sq = bk_bb == 0 ?
            64ULL : static_cast<std::uint64_t>(std::countr_zero(bk_bb));
        keys.king = (wk_sq | (bk_sq << 7U)) ^ stm_salt;
        keys.minor = (bitboards[0][kKnightIdx] * 0xBF58476D1CE4E5B9ULL) ^
            (bitboards[1][kKnightIdx] * 0x94D049BB133111EBULL) ^
            (bitboards[0][kBishopIdx] * 0xD6E8FEB86659FD93ULL) ^
            (bitboards[1][kBishopIdx] * 0xA0761D6478BD642FULL) ^ keys.king;
        keys.major = (bitboards[0][kRookIdx] * 0xE7037ED1A0B428DBULL) ^
            (bitboards[1][kRookIdx] * 0x8EBC6AF09C88C6E3ULL) ^
            (bitboards[0][kQueenIdx] * 0x589965CC75374CC3ULL) ^
            (bitboards[1][kQueenIdx] * 0x1D8E4E27C47D124FULL) ^ keys.king;
        if (frame != nullptr && !frame->previous_move.is_no_move() &&
            !two_ply_ago_move.is_no_move()) {
            const std::uint64_t m1_idx = static_cast<std::uint64_t>(
                table_move_index(frame->previous_move));
            const std::uint64_t m2_idx = static_cast<std::uint64_t>(
                table_move_index(two_ply_ago_move));
            keys.continuation = ((m2_idx << 12U) | m1_idx) ^ stm_salt;
            keys.has_continuation = true;
        } else {
            keys.continuation = 0;
            keys.has_continuation = false;
        }
    }

    [[nodiscard]] static CorrectionKeys correction_keys_for(const GameState& state) noexcept {
        CorrectionKeys keys;
        init_correction_keys(state, state.pawn_key(), keys, nullptr);
        return keys;
    }

    [[nodiscard]] static CorrectionKeys correction_keys_for(
        const GameState& state, std::uint64_t pawn_key,
        SearchFrame* frame = nullptr) noexcept {
        CorrectionKeys keys;
        init_correction_keys(state, pawn_key, keys, frame);
        return keys;
    }

    // Make/unmake wrappers that report the move to the evaluator worker, if it
    // has one.  The notifications are advisory: the worker validates the
    // position key before trusting any cached accumulator, so a missing call
    // here can only cost a refresh, never a wrong score.
    [[nodiscard]] bool make_observed(GameState& state, const MoveMetadata& metadata,
                                     const int ply) {
        // The parent key is observed only by the notification below, which is
        // a no-op without a private worker; a serial NNUE search never
        // has one, so skip the key computation there.
        const std::uint64_t parent_key =
            evaluation.has_private_worker() ? state.position_key() : 0;
        if (!state.make_search_move(metadata)) {
            return false;
        }
        if (ply >= 0 && static_cast<std::size_t>(ply + 1) < SearchStack::kCapacity) {
            const SearchFrame& parent = stack.frame(static_cast<std::size_t>(ply));
            SearchFrame& child = stack.frame(static_cast<std::size_t>(ply + 1));
            if (!metadata.is_capture() && metadata.move.promotion() == Promotion::none) {
                child.material_signature = parent.material_signature;
                child.material_signature_valid = parent.material_signature_valid;
            } else {
                child.material_signature_valid = false;
            }
        }
        evaluation.notify_make_move(state, metadata, ply, parent_key);
        return true;
    }

    void unmake_observed(GameState& state, const int child_ply) {
        evaluation.notify_unmake_move(child_ply);
        state.unmake_move();
    }

    [[nodiscard]] bool make_null_observed(GameState& state, const int ply) {
        const std::uint64_t parent_key = state.position_key();
        if (!state.make_null_move()) {
            return false;
        }
        if (ply >= 0 && static_cast<std::size_t>(ply + 1) < SearchStack::kCapacity) {
            const SearchFrame& parent = stack.frame(static_cast<std::size_t>(ply));
            SearchFrame& child = stack.frame(static_cast<std::size_t>(ply + 1));
            child.material_signature = parent.material_signature;
            child.material_signature_valid = parent.material_signature_valid;
        }
        evaluation.notify_make_null_move(state, ply, parent_key);
        return true;
    }

    void unmake_null_observed(GameState& state, const int child_ply) {
        evaluation.notify_unmake_null_move(child_ply);
        state.unmake_null_move();
    }

    [[nodiscard]] static std::uint64_t qsearch_move_key(const Move move) noexcept {
        if (move.is_no_move()) {
            return 0;
        }
        return 1ULL + static_cast<std::uint64_t>(move.from().index()) +
            ((1ULL + static_cast<std::uint64_t>(move.to().index())) << 7U) +
            ((1ULL + static_cast<std::uint64_t>(move.promotion())) << 14U);
    }

    [[nodiscard]] static std::uint64_t qsearch_cache_key(
        const GameState& state, const int ply, const int qdepth,
        const std::optional<Move> previous_move, const int check_depth_limit) noexcept {
        // The regular TT key intentionally excludes qsearch-only context. A
        // private cache can include it without changing the public TT format;
        // mix every dimension before direct-mapped indexing so common move
        // pairs do not cluster in the small worker-local table.
        std::uint64_t key = state.position_key();
        key ^= (qsearch_move_key(previous_move.value_or(Move::no_move())) +
                0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
        key ^= (static_cast<std::uint64_t>(std::max(0, ply)) + 1ULL) *
            0x94D049BB133111EBULL;
        key ^= (static_cast<std::uint64_t>(std::max(0, qdepth)) + 1ULL) *
            0xD6E8FEB86659FD93ULL;
        key ^= (static_cast<std::uint64_t>(std::max(0, check_depth_limit)) + 1ULL) *
            0xA24BAED4963EE407ULL;
        key ^= (static_cast<std::uint64_t>(state.halfmove_clock()) + 1ULL) *
            0x369DEA0F31A53F85ULL;
        key ^= key >> 30U;
        key *= 0xBF58476D1CE4E5B9ULL;
        key ^= key >> 27U;
        key *= 0x94D049BB133111EBULL;
        return key ^ (key >> 31U);
    }

    [[nodiscard]] QSearchCacheEntry* qsearch_cache_entry(
        const std::uint64_t key) noexcept {
        return &qsearch_cache[static_cast<std::size_t>(key) & (kQSearchCacheSize - 1)];
    }

    [[nodiscard]] const QSearchCacheEntry* qsearch_cache_entry(
        const std::uint64_t key) const noexcept {
        return &qsearch_cache[static_cast<std::size_t>(key) & (kQSearchCacheSize - 1)];
    }

    void store_qsearch_cache(const std::uint64_t key, const int score,
                             const bool lower_bound) noexcept {
        QSearchCacheEntry* entry = qsearch_cache_entry(key);
        // An exact result is stronger than a later lower-bound cutoff from a
        // different window. Do not downgrade it within this search context.
        if (entry->valid && entry->key == key && !entry->lower_bound && lower_bound) {
            return;
        }
        *entry = QSearchCacheEntry{key, score, true, lower_bound};
    }

    [[nodiscard]] bool qsearch_cache_allowed(
        const GameState& state, const bool repetition_sensitive) const noexcept {
        // Position keys do not encode the set of reversible ancestor keys.
        // A non-repeated position with a non-zero halfmove clock can therefore
        // still have a different repetition-sensitive descendant when reached
        // through another transposition.  Only a halfmove-zero boundary is
        // history-independent: the preceding pawn move/capture cannot be
        // undone by a future reversible qsearch continuation.
        return !repetition_sensitive && state.halfmove_clock() == 0;
    }

    void record_ply(int ply) noexcept {
        stats.seldepth = std::max(stats.seldepth, ply);
    }

    KOI_CTX_ALWAYS_INLINE void init_history_context(
        SearchHistoryContext& context,
        const int ply, const std::optional<Move> previous_move,
        const std::uint64_t pawn_key) const noexcept {
        context.count = 0;
        context.ply = ply;
        context.pawn_key = pawn_key;

        Move candidate = previous_move.value_or(Move::no_move());
        if (!previous_move.has_value() && ply > 0) {
            candidate = stack.frame(static_cast<std::size_t>(ply)).previous_move;
        }
        std::size_t distance = 0;
        for (; distance < kSearchContinuationPlies && !candidate.is_no_move(); ++distance) {
            context.continuation_moves[distance] = candidate;
            context.continuation_previous_scaled[distance] = static_cast<std::uint32_t>(
                table_move_index(candidate) * 131U);
            const int parent_ply = ply - static_cast<int>(distance) - 1;
            if (parent_ply < 0) {
                ++distance;
                break;
            }
            candidate = stack.frame(static_cast<std::size_t>(parent_ply)).previous_move;
        }
        context.count = distance;
        for (; distance < kSearchContinuationPlies; ++distance) {
            context.continuation_moves[distance] = Move::no_move();
            context.continuation_previous_scaled[distance] = 0U;
        }
    }

    [[nodiscard]] SearchHistoryContext history_context(
        const int ply, const std::optional<Move> previous_move,
        const std::uint64_t pawn_key) const noexcept {
        SearchHistoryContext context;
        init_history_context(context, ply, previous_move, pawn_key);
        return context;
    }

#undef KOI_CTX_ALWAYS_INLINE

    int quiescence(GameState& state, int alpha, int beta, int ply, int qdepth = 0,
                   std::optional<Move> previous_move = std::nullopt,
                   bool* repetition_sensitive_path = nullptr,
                   bool* selective_bound_path = nullptr,
                   bool* lower_bound_path = nullptr);

    int negamax(GameState& state, int depth, int alpha, int beta, int ply,
                PrincipalVariation& pv, std::optional<Move> previous_move = std::nullopt,
                bool allow_null_pruning = true,
                int check_extensions_remaining = kMaximumCheckExtensionsPerPath,
                Move excluded_move = Move::no_move(),
                bool* repetition_sensitive_path = nullptr,
                bool* selective_bound_path = nullptr,
                bool* root_authoritative_path = nullptr,
                bool* lower_bound_path = nullptr,
                int singular_extensions_remaining = kMaximumSingularExtensionsPerPath);

    int run_null_verification(
        GameState& state, int null_depth, int beta, int ply, SearchFrame& frame,
        PrincipalVariation& verification_pv, std::optional<Move> previous_move,
        int child_check_extensions_remaining, bool& path_repetition_sensitive);

    bool run_probcut(
        GameState& state, int ply, int beta, int probcut_beta, int probcut_depth,
        int static_eval, int raw_static_eval, std::uint64_t transposition_key,
        const MoveMetadataList* existing_moves, Move excluded_move,
        std::optional<Move> tt_move, const SearchHistoryContext& history,
        PrincipalVariation& probcut_pv, MoveMetadataList& probcut_moves,
        int child_check_extensions_remaining, bool& path_repetition_sensitive,
        bool& path_selective_bound, bool& path_lower_bound, int& cutoff_score);

    std::optional<Move> run_true_iid(
        GameState& state, int probe_depth, int alpha, int ply, SearchFrame& frame,
        PrincipalVariation& probe_pv, std::optional<Move> previous_move,
        int check_extensions_remaining, std::uint64_t transposition_key,
        bool& path_selective_bound);

    int run_singular_search(
        GameState& state, int singular_depth, int singular_beta, int ply,
        SearchFrame& frame, PrincipalVariation& singular_pv,
        std::optional<Move> previous_move, int child_check_extensions_remaining,
        Move excluded_move, bool& excluded_repetition_sensitive,
        bool& excluded_selective_bound);
};

} // namespace koi::detail
