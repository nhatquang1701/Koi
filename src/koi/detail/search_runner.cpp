#include "koi/detail/search_runner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "koi/detail/root_coordinator.hpp"
#include "koi/detail/search_context.hpp"
#include "koi/detail/search_stats_support.hpp"
#include "koi/detail/search_worker_pools.hpp"
#include "koi/detail/search_ordering.hpp"
#include "koi/detail/root_fallback.hpp"
#include "koi/detail/search_session.hpp"
#include "koi/detail/search_stack.hpp"
#include "koi/detail/search_table_access.hpp"
#include "koi/detail/thread_stack.hpp"
#include "koi/evaluator.hpp"
#include "koi/evaluation_features.hpp"
#include "koi/game_state.hpp"
#include "koi/syzygy_tablebase.hpp"
#include "koi/gpu/nnue_gpu_evaluator.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace koi::detail {

namespace {

constexpr int kRootSelectiveDepth = 3;
constexpr int kRootSelectiveMargin = 20;
constexpr int kRootSelectiveImprovement = 15;
constexpr std::size_t kMaximumRootCheckConfirmationCandidates = 2;
// A depth-two root commonly has enough work for PVS scouts to establish a
// useful ordering, but not enough horizon to make every scout fail-low an
// exact score.  Recheck a small, score-banded set at the same depth so root
// ranking is based on comparable full-window results rather than a mixture of
// exact scores and upper bounds.
constexpr int kRootShallowConfirmationDepth = 2;
constexpr int kRootShallowConfirmationMargin = 32;
constexpr int kRootShallowForcingMargin = 200;
constexpr std::size_t kMaximumRootShallowConfirmationCandidates = 4;
constexpr int kRootKingSafetySelectiveDepth = 5;
constexpr int kRootKingSafetyTieMargin = 50;
constexpr std::size_t kMaximumRootKingSafetyCandidates = 2;
// If a checked-root search is interrupted before any iteration completes,
// static material can make a king capture look much better than a safer
// escape.  Treat king-zone exposure as a crisis tie-break, but only inside
// this emergency path and only when the raw scores are reasonably close.
// The emergency static fallback is deliberately limited to the middle of the
// short-search band. At very low requested movetime the fixed overhead/safety
// ceiling leaves too little budget for both fallback scoring and a root pass;
// at the serial-path cutoff, scoring every root move can likewise starve the
// first authoritative iteration.

} // namespace

namespace {

std::optional<int> mate_from_score(int score) noexcept {
    if (score >= kMateThreshold) {
        return (kMateScore - score + 1) / 2;
    }
    if (score <= -kMateThreshold) {
        return -((kMateScore + score + 1) / 2);
    }
    return std::nullopt;
}


struct RootScheduleRecord {
    Move move = Move::no_move();
    std::size_t stable_index = 0;
    int previous_score = -kInfinity;
    bool has_previous_score = false;
    bool previous_exact = false;
};

// Keeps the process-wide "GPU batching is worth it" counter true for exactly
// the lifetime of a multi-threaded search.  A boolean flag would be wrong when
// two searches with different thread counts overlap: the second search would
// flip the evaluator path of the first one mid-flight.
class ThreadedGpuSearchScope {
public:
    explicit ThreadedGpuSearchScope(const bool threaded) noexcept : threaded_(threaded) {
        if (threaded_) {
            gpu::begin_gpu_nnue_threaded_search();
        }
    }

    ~ThreadedGpuSearchScope() {
        if (threaded_) {
            gpu::end_gpu_nnue_threaded_search();
        }
    }

    ThreadedGpuSearchScope(const ThreadedGpuSearchScope&) = delete;
    ThreadedGpuSearchScope& operator=(const ThreadedGpuSearchScope&) = delete;

private:
    bool threaded_;
};

} // namespace

SearchRunner::SearchRunner(std::shared_ptr<SearchSession> session,
                           std::shared_ptr<const Evaluator> evaluator,
                           std::shared_ptr<TranspositionTable> table,
                           std::shared_ptr<std::mutex> evaluator_mutex,
                           SearchEventSink sink)
    : session_(std::move(session)),
      evaluator_(std::move(evaluator)),
      table_(std::move(table)),
      evaluator_mutex_(std::move(evaluator_mutex)),
      sink_(std::move(sink)) {}

void SearchRunner::run() {
    try {
        run_impl();
    } catch (...) {
        // A failure in the setup prefix (root/limits/identity copies allocate
        // before run_impl's internal try) must still deliver exactly one
        // completion; otherwise the GUI waits forever for a bestmove.
        SearchResult failed;
        // A setup-prefix failure still has to answer the request that created
        // this session.  The completion gate suppresses completions whose
        // identity does not match the live root, so a default identity here
        // would silently swallow the only bestmove the GUI will ever get.
        failed.identity = session_->identity();
        failed.failed = true;
        failed.failure_reason = SearchFailureReason::setup_failed;
        failed.completed = true;
        try {
            (void)session_->publish_completion(sink_, failed);
        } catch (...) {
        }
    }
}

void SearchRunner::run_impl() {
    const auto& session = session_;
    const auto& evaluator = evaluator_;
    const auto& table = table_;
    const auto& evaluator_mutex = evaluator_mutex_;
    const auto& sink = sink_;

    const auto started = std::chrono::steady_clock::now();
    // Interim progress publication state, owned by the runner thread. Normal
    // publications deposit the completed iteration here; long iterations
    // refresh it through the context probe (serial/LazySMP) or the root-pool
    // tick (root-parallel). Results never read this state.
    InterimReportState interim_state;
    interim_state.started = started;
    GameState root = session->root();
    // Search never reads the vendored compatibility board. Detaching it keeps
    // interior make/unmake from copying a second board on every node; native
    // legality and keys remain the sole authority. Public GameState consumers
    // (UCI book probing, diagnostics) keep mirror tracking enabled.
    root.detach_mirror();
    // Mutable copy: a UCI ponderhit converts the running search in place and
    // re-arms timing at the top of the next iteration.
    SearchLimits limits = session->limits();
    const SearchOptions& options = session->options();
    const ThreadedGpuSearchScope gpu_scope(options.threads > 1);
    SearchResult result;
    result.identity = session->identity();
    // The orchestrator uses the same table seam as the recursive search layer;
    // nothing below reaches TranspositionTable directly.
    SearchTableAccess table_access(*table);
    try {
        RootTimingContext timing_context;
        timing_context.table_available = table_access.size_mb() != 0;
        if (const auto entry = table_access.probe(search_transposition_key(root), 0);
            entry.has_value()) {
            timing_context.tt_hit = true;
            timing_context.tt_exact = entry->bound == TranspositionBound::exact;
            timing_context.tt_has_best_move = !entry->best_move.is_no_move();
            timing_context.tt_depth = entry->depth;
            timing_context.tt_generation_age = entry->generation_age;
        }

        std::mutex* evaluator_mutex_ptr = evaluator->supports_concurrent_evaluation() ?
            nullptr : evaluator_mutex.get();

        MoveMetadataList all_legal_moves;
        root.legal_moves_with_metadata(
            all_legal_moves, true, true, CheckFlagMode::quiet_moves_only);
        timing_context.legal_move_count = static_cast<std::uint32_t>(all_legal_moves.size());
        timing_context.forcing_move_count = static_cast<std::uint32_t>(std::count_if(
            all_legal_moves.begin(), all_legal_moves.end(), [](const MoveMetadata& metadata) {
                return metadata.is_capture() || metadata.gives_check ||
                    metadata.move.promotion() != Promotion::none;
            }));
        timing_context.in_check = root.in_check();

        MoveMetadataList legal_moves = all_legal_moves;
        if (limits.search_moves_specified) {
            MoveMetadataList filtered_moves;
            for (const MoveMetadata& metadata : legal_moves) {
                if (std::find(limits.search_moves.begin(), limits.search_moves.end(), metadata.move) !=
                    limits.search_moves.end()) {
                    (void)filtered_moves.push_back(metadata);
                }
            }
            legal_moves = filtered_moves;
        }
        table_access.new_generation();
        TimeManager time_manager(limits, root.side_to_move(), options.speed_percent,
                                 options.move_overhead_ms, options.slow_mover_percent,
                                 timing_context, {}, options.dynamic_move_overhead);
        const FallbackControl fallback_control{&time_manager, &session->stop_requested()};
        result.timing = time_manager.diagnostics();
        result.best_move = legal_moves.empty() ? std::nullopt :
            std::optional<Move>{legal_moves.front().move};
        if (result.best_move.has_value()) {
            result.pv = {*result.best_move};
        }
        const bool root_is_claimable_draw = root.is_claimable_draw();
        const bool root_is_forced_draw = root.is_forced_draw();
        int root_static_eval = 0;
        bool root_static_eval_ready = false;
        const auto initialize_root_static_eval = [&] {
            if (root_static_eval_ready) {
                return;
            }
            if (evaluator_mutex_ptr != nullptr) {
                std::lock_guard lock(*evaluator_mutex_ptr);
                result.score_cp = evaluator->evaluate(root, root.side_to_move());
            } else {
                result.score_cp = evaluator->evaluate(root, root.side_to_move());
            }
            root_static_eval = result.score_cp;
            if (root_is_claimable_draw || root_is_forced_draw) {
                result.score_cp = std::max(0, result.score_cp);
                if (root_is_forced_draw) {
                    result.score_cp = 0;
                }
                result.mate.reset();
            }
            root_static_eval_ready = true;
        };
        initialize_root_static_eval();

        // Interior tablebase probing is opt-in, off by default, and mirrors
        // the root probe restrictions except for ponder: a pondering search is
        // converted in place on ponderhit, so the binding must already be in
        // force when the conversion happens.  The diagnostic hook may stand in
        // for a real tablebase so the plumbing can be verified without assets.
        TablebaseSearchBinding tablebase_binding;
        if (options.syzygy_interior_depth > 0 &&
            (options.syzygy != nullptr || options.tablebase_probe_hook) &&
            options.multi_pv == 1 && !options.analyse_mode &&
            !limits.search_moves_specified && !root_is_claimable_draw && !root_is_forced_draw) {
            tablebase_binding.table = options.syzygy ? options.syzygy.get() : nullptr;
            tablebase_binding.interior_depth = options.syzygy_interior_depth;
            tablebase_binding.fifty_move_rule =
                options.syzygy == nullptr || options.syzygy->fifty_move_rule();
            tablebase_binding.probe_hook = options.tablebase_probe_hook;
        }
        const auto normalize_root_fallback_score =
            [root_is_claimable_draw, root_is_forced_draw,
             contempt = options.contempt_cp](const int score) {
                if (root_is_forced_draw) {
                    return -contempt;
                }
                return root_is_claimable_draw ? std::max(-contempt, score) : score;
            };
        std::optional<SyzygyRootResult> tablebase_result;
        // SyzygyProbeDepth is a minimum search depth.  A declared depth limit is
        // compared against it directly; a time or node search has no declared
        // limit, so it is treated as unbounded and may probe (the search will
        // pass any requested probe depth it is given time for).
        const int probe_gate_depth = limits.depth.has_value() ? *limits.depth :
            std::numeric_limits<int>::max();
        // A ponder search cannot take the root fast path because it must not
        // emit a bestmove before stop/ponderhit.  After an in-place conversion
        // the search continues from its warmed state without a fresh root
        // probe; interior probing, when enabled, is already active.
        if (options.syzygy && options.multi_pv == 1 && !options.analyse_mode &&
            !limits.infinite && !limits.ponder && !limits.search_moves_specified &&
            !root_is_claimable_draw && !root_is_forced_draw &&
            options.syzygy->allows_depth(probe_gate_depth)) {
            std::vector<Move> allowed_moves;
            allowed_moves.reserve(legal_moves.size());
            for (const MoveMetadata& metadata : legal_moves) {
                allowed_moves.push_back(metadata.move);
            }
            tablebase_result = options.syzygy->probe_root(root, allowed_moves);
        }

        const std::optional<std::chrono::milliseconds> effective_time_budget =
            time_manager.time_budget();
        const bool requested_short_timed_fallback = limits.movetime.has_value() &&
            *limits.movetime >= kShortTimedFallbackMinimum &&
            *limits.movetime <= kShortTimedFallbackThreshold;
        const bool ultra_short_nonchecked_fallback = limits.movetime.has_value() &&
            *limits.movetime < kShortTimedFallbackMinimum &&
            !timing_context.in_check && timing_context.forcing_move_count != 0;
        const bool ultra_short_checked_fallback = limits.movetime.has_value() &&
            *limits.movetime < kShortTimedFallbackMinimum && timing_context.in_check;
        const bool clock_short_timed_fallback = !limits.movetime.has_value() &&
            effective_time_budget.has_value() &&
            *effective_time_budget <= kShortTimedFallbackThreshold;
        bool ultra_short_nonchecked_forcing_root = false;
        if (limits.movetime.has_value() &&
            *limits.movetime < kShortTimedFallbackMinimum && !timing_context.in_check) {
            const PositionFeatures root_features = root.position_features();
            ultra_short_nonchecked_forcing_root = std::any_of(
                all_legal_moves.begin(), all_legal_moves.end(),
                [&root_features](const MoveMetadata& metadata) {
                    return quiet_move_has_direct_forcing_target(root_features, metadata);
                });
        }
        const bool short_search_fallback = requested_short_timed_fallback ||
            ultra_short_nonchecked_fallback || ultra_short_nonchecked_forcing_root ||
            ultra_short_checked_fallback || clock_short_timed_fallback;
        const bool short_tactical_fallback = short_search_fallback &&
            (timing_context.in_check || timing_context.forcing_move_count != 0 ||
             ultra_short_nonchecked_forcing_root);
        const bool short_tactical_budget =
            (timing_context.in_check || timing_context.forcing_move_count != 0 ||
             ultra_short_nonchecked_forcing_root) &&
            ((limits.movetime.has_value() &&
              *limits.movetime <= kShortTimedSerialThreshold) ||
             (!limits.movetime.has_value() && effective_time_budget.has_value() &&
              *effective_time_budget <= kShortTimedSerialThreshold));
        // A checked root has no time for the full emergency evasion/reply
        // scan when the hard budget is below 150 ms.  Keep the fallback
        // to one static post-evasion evaluation in that band so the
        // authoritative first root iteration still gets a chance to
        // complete. Normal timed and fixed-depth paths retain the deeper
        // checking fallback.
        const bool shallow_checked_fallback = root.in_check() &&
            ((limits.movetime.has_value() &&
              *limits.movetime < kShortTimedFallbackMinimum) ||
             (!limits.movetime.has_value() && effective_time_budget.has_value() &&
              *effective_time_budget < kShortTimedFallbackMinimum));
        const bool allow_deep_checked_fallback = !shallow_checked_fallback;
        const bool defer_expensive_short_fallback = clock_short_timed_fallback;
        const bool ultra_short_nonchecking_parallel = limits.movetime.has_value() &&
            *limits.movetime < kShortTimedFallbackMinimum && !timing_context.in_check;

        // Lazy SMP replaces the root-splitting pool for the common Threads > 1
        // configuration. Node-limited, MultiPV, forced-draw and short-tactical
        // searches keep the root-parallel path so their accounting stays exact.
        const bool use_lazy_smp = options.threads > 1 && options.multi_pv == 1 &&
            !time_manager.node_limit().has_value() && !short_tactical_budget &&
            !root_is_claimable_draw && !root_is_forced_draw && legal_moves.size() >= 2;
        std::unique_ptr<LazySmpPool> lazy_pool;
        if (use_lazy_smp) {
            lazy_pool = std::make_unique<LazySmpPool>(
                options.threads - 1, root, legal_moves, *evaluator, *table, time_manager,
                session->stop_requested(), evaluator_mutex_ptr, true,
                options.quiet_history_side_hook, tablebase_binding, root.side_to_move(),
                options.contempt_cp);
        }

        if (legal_moves.empty()) {
            result.score_cp = root.in_check() ? -kMateScore : 0;
            result.mate = mate_from_score(result.score_cp);
        } else if (tablebase_result.has_value()) {
            result.best_move = tablebase_result->moves.front();
            result.pv = {*result.best_move};
            result.score_cp = tablebase_result->score.score_cp;
            result.mate = tablebase_result->score.mate;
            result.completed_depth = 1;
            result.stats.tbhits = 1;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            SearchInfo info{1, result.score_cp, result.mate, 0, 0, elapsed,
                            {result.best_move.value()}, 0, 0, 0, 1, 1};
            info.exact_wdl = syzygy_wdl_permill(tablebase_result->wdl);
            safely_report_info(sink, info);
        } else if ((root_is_claimable_draw && options.multi_pv == 1) ||
                   (root_is_forced_draw && options.multi_pv == 1) || use_lazy_smp ||
                   (options.threads == 1 && options.multi_pv == 1) || legal_moves.size() < 2 ||
                   (time_manager.node_limit().has_value() && options.multi_pv == 1) ||
                   (short_tactical_budget && options.multi_pv == 1 &&
                    !ultra_short_nonchecking_parallel)) {
            auto context_storage = std::make_unique<SearchContext>(
                *evaluator, *table, time_manager, session->stop_requested(),
                nullptr, evaluator_mutex_ptr, &legal_moves, true,
                options.quiet_history_side_hook, tablebase_binding, root.side_to_move(),
                options.contempt_cp);
            SearchContext& context = *context_storage;
            // The loop below records the depth and principal variation being
            // searched so interim publications can report the in-flight
            // iteration instead of only the last completed one.
            int in_flight_depth = 0;
            PrincipalVariation pv;
            if (options.interim_report_interval.count() > 0) {
                // Interim progress publisher for long iterations. Only this
                // runner-thread context gets the probe; LazySMP helpers and
                // root-parallel workers never publish protocol output.
                context.progress_probe = [&context, &sink, &interim_state, &lazy_pool,
                                          &in_flight_depth, &pv,
                                          interval = options.interim_report_interval] {
                    std::uint64_t live = context.stats.nodes + context.stats.qnodes;
                    if (lazy_pool != nullptr) {
                        live += lazy_pool->live_nodes() + lazy_pool->live_qnodes();
                    }
                    maybe_publish_interim(sink, interim_state, live, in_flight_depth, &pv, interval);
                };
            }
            if (lazy_pool == nullptr) {
                // Exclusive TT access contract: one live search per
                // TranspositionTable; the runner thread is the only TT
                // toucher while an exclusive session is active (UCI
                // stop+join, koi-bench sequential).  The LazySMP main thread
                // shares the table with its helpers, so it stays locked.
                context.table_access.set_exclusive(true);
            }
            context.allow_root_forcing_extension = limits.depth.has_value() &&
                *limits.depth == 1;
            const bool ultra_short_nonchecked_extension = limits.movetime.has_value() &&
                *limits.movetime < kShortTimedFallbackMinimum &&
                !root.in_check();
            if (requested_short_timed_fallback || ultra_short_nonchecked_extension) {
                context.allow_root_forcing_extension = true;
            }
            context.ordering.order(root, legal_moves, std::nullopt, 0);
            result.best_move = legal_moves.front().move;
            bool used_short_fallback = false;
            // Do not spend an ultra-short non-checking movetime in the
            // emergency evaluator before the first authoritative root
            // iteration. The ordered legal move remains the safety value
            // if the clock expires, while a completed iteration remains
            // authoritative when it fits.
            const bool defer_nonchecking_short_fallback =
                ultra_short_nonchecked_fallback || ultra_short_nonchecked_forcing_root;
            if (short_tactical_fallback && !defer_nonchecking_short_fallback &&
                !ultra_short_checked_fallback && !defer_expensive_short_fallback) {
                if (const auto fallback = short_search_fallback_move(
                        root, legal_moves, *evaluator, evaluator_mutex_ptr,
                        &context.stats, allow_deep_checked_fallback, &fallback_control);
                    fallback.has_value()) {
                    result.best_move = fallback->move;
                    result.pv = {fallback->move};
                    const int fallback_score = normalize_root_fallback_score(fallback->score);
                    result.score_cp = fallback_score;
                    result.mate = mate_from_score(fallback_score);
                    used_short_fallback = true;
                }
            } else if (short_search_fallback && !defer_nonchecking_short_fallback &&
                       !ultra_short_checked_fallback) {
                if (const auto fallback = first_safe_short_search_move(root, legal_moves);
                    fallback.has_value()) {
                    result.best_move = *fallback;
                    result.pv = {*fallback};
                    used_short_fallback = true;
                }
            }

            int maximum_depth =
                std::min(kMaximumSearchDepth, std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
            bool unbounded = limits.infinite || limits.ponder;
            if (limits.ponder && limits.depth.has_value()) {
                // While pondering, a depth limit only says how deep the GUI
                // wanted the search before it started pondering; the engine must
                // keep deepening instead of re-searching the same final depth
                // until stop/ponderhit.  A ponderhit conversion below reinstates
                // the converted depth/time budget.
                maximum_depth = kMaximumSearchDepth;
            }
            std::optional<int> previous_score;
            std::chrono::milliseconds previous_iteration_elapsed{1};
            std::chrono::milliseconds previous_report_elapsed{0};
            for (int depth = 1;; depth = depth < maximum_depth ? depth + 1 : maximum_depth) {
                in_flight_depth = depth;
                pv.clear();
                if (auto conversion = session->take_ponderhit_limits(); conversion.has_value()) {
                    limits = std::move(*conversion);
                    time_manager.reconfigure(limits, root.side_to_move(), options.speed_percent,
                                             options.move_overhead_ms, options.slow_mover_percent,
                                             timing_context, options.dynamic_move_overhead);
                    maximum_depth = std::min(
                        kMaximumSearchDepth,
                        std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
                    unbounded = limits.infinite || limits.ponder;
                }
                if (!unbounded && depth > maximum_depth) {
                    break;
                }
                // Let the deadline trim the last iteration instead of a 2x
                // cost estimate: a partially searched deeper root can already
                // contain a better move, which is adopted on interruption below.
                if (!unbounded && !time_manager.should_start_next_iteration(
                        std::chrono::milliseconds{1})) {
                    break;
                }
                if (context.interrupted()) {
                    break;
                }
                if (lazy_pool) {
                    lazy_pool->publish_depth(depth);
                }
                const std::optional<Move> previous_best_move = result.best_move;
                const std::vector<Move> previous_pv = result.pv;
                const bool had_completed_iteration = result.completed_depth > 0;
                context.root_move_hint = had_completed_iteration ? result.best_move : std::nullopt;
                int alpha = -kInfinity;
                int beta = kInfinity;
                bool aspiration_researched = false;
                int aspiration_window = kAspirationWindow + std::min(32, depth * 2);
                if (previous_score.has_value()) {
                    alpha = std::max(-kInfinity, *previous_score - aspiration_window);
                    beta = std::min(kInfinity, *previous_score + aspiration_window);
                    if (result.best_move.has_value()) {
                        // Retain the previous principal move explicitly
                        // even when the TT entry was displaced by a
                        // shallow verification or a hash resize.
                        context.ordering.order(root, legal_moves,
                                               result.best_move, 0);
                    }
                }

                bool root_authoritative = false;
                int score = context.negamax(
                    root, depth, alpha, beta, 0, pv, std::nullopt, true,
                    kMaximumCheckExtensionsPerPath, Move::no_move(), nullptr,
                    nullptr, &root_authoritative);
                if (!context.aborted && previous_score.has_value()) {
                    // Widen the failed side progressively. This preserves
                    // the useful narrow window when the iteration is
                    // stable, while avoiding repeated full-width searches
                    // after a tactical score jump.
                    for (int retry = 0; retry < 3 &&
                         (score <= alpha || score >= beta) && !context.aborted; ++retry) {
                        ++context.stats.aspiration_researches;
                        aspiration_researched = true;
                        aspiration_window = std::min(kInfinity / 4,
                                                    aspiration_window * 2 + 16);
                        if (score <= alpha) {
                            alpha = std::max(-kInfinity, alpha - aspiration_window);
                        }
                        if (score >= beta) {
                            beta = std::min(kInfinity, beta + aspiration_window);
                        }
                        pv = {};
                        score = context.negamax(
                            root, depth, alpha, beta, 0, pv, std::nullopt, true,
                            kMaximumCheckExtensionsPerPath, Move::no_move(), nullptr,
                            nullptr, &root_authoritative);
                    }
                    if (!context.aborted && (score <= alpha || score >= beta)) {
                        ++context.stats.aspiration_researches;
                        aspiration_researched = true;
                        pv = {};
                        score = context.negamax(
                            root, depth, -kInfinity, kInfinity, 0, pv, std::nullopt, true,
                            kMaximumCheckExtensionsPerPath, Move::no_move(), nullptr,
                            nullptr, &root_authoritative);
                    }
                }
                const bool helper_failed = lazy_pool != nullptr && lazy_pool->failed();
                if (context.aborted || helper_failed) {
                    if (helper_failed) {
                        // A helper could not finish its iteration. Helpers only
                        // feed the shared table, so the main result stays valid;
                        // stop deepening, mark the diagnostic flag, and publish
                        // the best completed iteration through the abort-safe
                        // path below instead of letting the failure kill the
                        // process or leave the search running.
                        result.failed = true;
                        result.failure_reason = SearchFailureReason::helper_failed;
                    }
                    if (result.completed_depth == 0 && !used_short_fallback &&
                        defer_nonchecking_short_fallback) {
                        if (const auto fallback = short_search_fallback_move(
                                root, legal_moves, *evaluator, evaluator_mutex_ptr,
                                &context.stats, allow_deep_checked_fallback, &fallback_control,
                                true, true);
                            fallback.has_value()) {
                            result.best_move = fallback->move;
                            result.pv = {fallback->move};
                            const int fallback_score =
                                normalize_root_fallback_score(fallback->score);
                            result.score_cp = fallback_score;
                            result.mate = mate_from_score(fallback_score);
                            used_short_fallback = true;
                        }
                    }
                    if (result.completed_depth == 0 && !used_short_fallback &&
                        ultra_short_checked_fallback) {
                        if (const auto fallback = short_search_fallback_move(
                                root, legal_moves, *evaluator, evaluator_mutex_ptr,
                                &context.stats, false, &fallback_control, true);
                            fallback.has_value()) {
                            result.best_move = fallback->move;
                            result.pv = {fallback->move};
                            const int fallback_score =
                                normalize_root_fallback_score(fallback->score);
                            result.score_cp = fallback_score;
                            result.mate = mate_from_score(fallback_score);
                            used_short_fallback = true;
                        }
                    }
                    if (result.completed_depth == 0 && !used_short_fallback) {
                        // An interrupted first iteration is never an
                        // authoritative tactical result.  Reject a
                        // partial root move that exposes an immediate
                        // checking reply or broad checking horizon even
                        // when the root move list did not contain a
                        // capture or direct check.
                        const auto partial = context.best_completed_root_move(root, true);
                        const auto partial_metadata = partial.has_value() ?
                            std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&partial](const MoveMetadata& metadata) {
                                    return metadata.move == partial->move;
                                }) : legal_moves.end();
                        if (partial.has_value() && root.is_legal(partial->move) &&
                            partial_metadata != legal_moves.end() &&
                            !root_move_is_broad_quiet_check(root, *partial_metadata)) {
                            result.best_move = partial->move;
                            result.pv = {partial->move};
                            result.score_cp = partial->score;
                        } else if (const auto fallback = first_safe_short_search_move(
                                       root, legal_moves);
                                   fallback.has_value()) {
                            result.best_move = *fallback;
                            result.pv = {*fallback};
                        }
                    }
                    break;
                }

                // Publication requires a complete root-score coverage plus a
                // valid PV. Strict provenance is tracked separately through
                // `root_authoritative` (the negamax root flag) and only gates
                // aspiration seeding and proof-sensitive metadata; ordinary
                // selective qsearch paths must not suppress UCI depth
                // progress or replace a completed iteration with an
                // unsearched ordered fallback.
                const bool root_result_publishable = context.root_result_complete(pv);
                if (!root_result_publishable) {
                    // The recursive search did not return a complete root
                    // result (missing score coverage or PV). Preserve the
                    // last completed result and use this work only to pace
                    // the next attempt.
                    const auto elapsed = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started);
                    previous_iteration_elapsed = std::max(
                        std::chrono::milliseconds{1}, elapsed - previous_report_elapsed);
                    if ((!unbounded && depth == maximum_depth) || root_is_forced_draw ||
                        time_manager.should_stop_after_iteration()) {
                        break;
                    }
                    continue;
                }

                result.completed_depth = depth;
                result.score_cp = score;
                result.mate = mate_from_score(score);
                if (pv.length > 0) {
                    result.best_move = pv.moves[0];
                    result.pv = pv.to_vector();
                }
                // The bounded short-search correction below is allowed to
                // improve the move handed to the caller, but its static
                // score is not an iterative-deepening proof.  Retain the
                // ordinary full-width result for aspiration and timing
                // observations so a heuristic safety choice cannot make
                // the next iteration look artificially unstable.
                const int searched_iteration_score = score;
                const bool searched_best_move_changed = had_completed_iteration &&
                    previous_best_move != result.best_move;
                const bool searched_pv_changed = had_completed_iteration &&
                    previous_pv != result.pv;
                bool bounded_fallback_result = false;

                // The correction must also cover depth-two blind iterations in
                // the timed band: a cheaper fallback scan can flip
                // should_start_next_iteration's estimated-next-iteration guard
                // and let a blind depth two publish the poisoned capture
                // (batch 30: base 106+106 = 212 >= 209 stops, cheaper
                // 82+82 = 164 < 209 continues to depth two).
                if ((short_tactical_fallback || defer_nonchecking_short_fallback) &&
                    !defer_expensive_short_fallback &&
                    result.completed_depth <= 2 &&
                    (short_capture_needs_fallback(legal_moves, result.best_move) ||
                     short_quiet_needs_fallback(root, legal_moves, result.best_move) ||
                     short_quiet_exposes_immediate_check(
                         root, legal_moves, result.best_move) ||
                     short_check_needs_fallback(legal_moves, result.best_move) ||
                     short_checked_root_king_evasion_needs_fallback(
                         root, legal_moves, result.best_move))) {
                    // The iteration is complete, so preserve a bounded
                    // shallow safety correction even if the hard clock
                    // has just fired. Do not re-enter the recursive
                    // emergency scan after the deadline.
                    auto fallback = short_search_fallback_move(
                        root, legal_moves, *evaluator, evaluator_mutex_ptr,
                        &context.stats, allow_deep_checked_fallback,
                        &fallback_control, true, true);
                    std::optional<Move> fallback_move;
                    int fallback_score = score;
                    if (fallback.has_value()) {
                        fallback_move = fallback->move;
                        fallback_score = normalize_root_fallback_score(fallback->score);
                    }
                    const auto current_metadata = result.best_move.has_value() ?
                        std::find_if(
                            legal_moves.begin(), legal_moves.end(),
                            [&result](const MoveMetadata& metadata) {
                                return metadata.move == *result.best_move;
                            }) : legal_moves.end();
                    if (current_metadata != legal_moves.end() &&
                        !current_metadata->is_capture() && !current_metadata->gives_check &&
                        current_metadata->move.promotion() == Promotion::none &&
                        (!fallback_move.has_value() ||
                         *fallback_move == current_metadata->move)) {
                        if (const auto checking_capture = scored_safe_checking_capture(
                                root, legal_moves, *evaluator, evaluator_mutex_ptr,
                                allow_deep_checked_fallback, &fallback_control);
                            checking_capture.has_value()) {
                            fallback_move = checking_capture->move;
                            fallback_score = normalize_root_fallback_score(checking_capture->score);
                        } else {
                            fallback_move.reset();
                        }
                    }
                    if (fallback_move.has_value() &&
                        short_fallback_should_replace_completed_root(
                            root, legal_moves, result.best_move, *fallback_move)) {
                        result.best_move = *fallback_move;
                        result.pv = {*fallback_move};
                        result.score_cp = fallback_score;
                        result.mate = mate_from_score(fallback_score);
                        score = fallback_score;
                        bounded_fallback_result = true;
                        // Keep the protocol PV in lockstep with the
                        // corrected authoritative root move. The normal
                        // iteration PV still contains the pre-correction
                        // move, and publishing it here would make the
                        // following bestmove disagree with the final
                        // info line.
                        pv = {};
                        pv.moves[0] = *fallback_move;
                        pv.length = 1;
                    }
                }

                // Selective root checks below may run another negamax call,
                // which refreshes the context's root-score scratch buffer.
                // Preserve the completed full-width scores before any such
                // check so later safety decisions remain based on this
                // iteration's complete root ordering.
                std::vector<SearchContext::RootMoveScore> root_score_snapshot;
                if (limits.depth.has_value() &&
                    (depth == kRootShallowConfirmationDepth ||
                     depth == kRootSelectiveDepth) &&
                    context.root_move_score_count != 0) {
                    root_score_snapshot.assign(
                        context.root_move_scores.begin(),
                        context.root_move_scores.begin() + context.root_move_score_count);
                }
                // Some bounded root safety passes compare candidates at a
                // deeper horizon to expose a shallow tactical trap.  The
                // selected move still belongs to the requested iteration,
                // so refresh its line at the nominal depth before
                // publishing the score/PV.  This keeps fixed-depth UCI
                // results and timing observations tied to one horizon.
                const auto nominal_root_verification =
                    [&context, &root, &legal_moves](const Move move, const int nominal_depth,
                                                   const bool allow_estimated_line = false)
                    -> std::optional<SearchContext::RootVerification> {
                    const auto metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [move](const MoveMetadata& candidate) {
                            return candidate.move == move;
                        });
                    if (metadata == legal_moves.end()) {
                        return std::nullopt;
                    }
                    ++context.stats.root_selective_candidates;
                    const SearchContext::RootVerification verification =
                        context.selectively_research_root_move(root, *metadata, nominal_depth);
                    if (context.aborted ||
                        (!verification.authoritative &&
                         !(allow_estimated_line && verification.line_complete))) {
                        return std::nullopt;
                    }
                    return verification;
                };

                if (limits.depth.has_value() &&
                    *limits.depth == kRootShallowConfirmationDepth &&
                    depth == kRootShallowConfirmationDepth &&
                    !limits.search_moves_specified && !context.aborted &&
                    !root_score_snapshot.empty() && result.best_move.has_value() &&
                    !mate_from_score(score).has_value()) {
                    struct ShallowRootCandidate {
                        MoveMetadata metadata{};
                        int score = -kInfinity;
                        bool forcing = false;
                    };

                    const auto metadata_for = [&legal_moves](const Move move) {
                        return std::find_if(
                            legal_moves.begin(), legal_moves.end(),
                            [move](const MoveMetadata& metadata) {
                                return metadata.move == move;
                            });
                    };
                    const auto incumbent_metadata = metadata_for(*result.best_move);
                    const bool incumbent_forcing = incumbent_metadata != legal_moves.end() &&
                        (incumbent_metadata->is_capture() || incumbent_metadata->gives_check ||
                         incumbent_metadata->move.promotion() != Promotion::none);
                    const auto is_same_target_capture =
                        [&incumbent_metadata, &legal_moves](
                            const auto metadata) {
                        return incumbent_metadata != legal_moves.end() &&
                            metadata != legal_moves.end() &&
                            incumbent_metadata->is_capture() && metadata->is_capture() &&
                            metadata->move.to() == incumbent_metadata->move.to();
                    };
                    const bool has_mixed_forcing_alternative = incumbent_metadata != legal_moves.end() &&
                        std::any_of(
                            root_score_snapshot.begin(), root_score_snapshot.end(),
                            [&metadata_for, &legal_moves, &incumbent_forcing,
                             &is_same_target_capture, &result](
                                const SearchContext::RootMoveScore& root_score) {
                                if (root_score.move == *result.best_move) {
                                    return false;
                                }
                                const auto metadata = metadata_for(root_score.move);
                                if (metadata == legal_moves.end()) {
                                    return false;
                                }
                                const bool forcing = metadata->is_capture() || metadata->gives_check ||
                                    metadata->move.promotion() != Promotion::none;
                                return (root_score.exact && forcing != incumbent_forcing) ||
                                    is_same_target_capture(metadata);
                            });
                    if (incumbent_metadata != legal_moves.end() &&
                        has_mixed_forcing_alternative) {
                        std::vector<ShallowRootCandidate> alternatives;
                        alternatives.reserve(root_score_snapshot.size());
                        for (const SearchContext::RootMoveScore& root_score :
                             root_score_snapshot) {
                            if (root_score.move == *result.best_move) {
                                continue;
                            }
                            const auto metadata = metadata_for(root_score.move);
                            if (metadata == legal_moves.end()) {
                                continue;
                            }
                            const bool same_target_capture =
                                is_same_target_capture(metadata);
                            if (!root_score.exact && !same_target_capture) {
                                // A scout bound may nominate a defensive
                                // recapture cluster, but no other
                                // inexact root line may enter the
                                // authoritative confirmation set.
                                continue;
                            }
                            const bool forcing = metadata->is_capture() || metadata->gives_check ||
                                metadata->move.promotion() != Promotion::none;
                            const int score_margin = forcing ? kRootShallowForcingMargin :
                                kRootShallowConfirmationMargin;
                            if (root_score.score < score - score_margin) {
                                continue;
                            }
                            alternatives.push_back(
                                ShallowRootCandidate{*metadata, root_score.score, forcing});
                        }
                        std::stable_sort(
                            alternatives.begin(), alternatives.end(),
                            [](const ShallowRootCandidate& left,
                               const ShallowRootCandidate& right) {
                                if (left.forcing != right.forcing) {
                                    return left.forcing;
                                }
                                if (left.score != right.score) {
                                    return left.score > right.score;
                                }
                                return detail::move_tie_break_key(left.metadata.move) <
                                    detail::move_tie_break_key(right.metadata.move);
                            });

                        std::array<ShallowRootCandidate,
                                   kMaximumRootShallowConfirmationCandidates> candidates{};
                        std::size_t candidate_count = 0;
                        candidates[candidate_count++] =
                            ShallowRootCandidate{*incumbent_metadata, score,
                                                  incumbent_metadata->is_capture() ||
                                                      incumbent_metadata->gives_check ||
                                                      incumbent_metadata->move.promotion() !=
                                                          Promotion::none};
                        for (const ShallowRootCandidate& candidate : alternatives) {
                            if (candidate_count == candidates.size()) {
                                break;
                            }
                            candidates[candidate_count++] = candidate;
                        }

                        int confirmed_score = -kInfinity;
                        std::optional<Move> confirmed_move;
                        PrincipalVariation confirmed_pv;
                        for (std::size_t index = 0; index < candidate_count; ++index) {
                            ++context.stats.root_selective_candidates;
                            const SearchContext::RootVerification verification =
                                context.selectively_research_root_move(
                                    root, candidates[index].metadata,
                                    kRootShallowConfirmationDepth);
                            if (context.aborted) {
                                break;
                            }
                            if (!verification.authoritative) {
                                continue;
                            }
                            if (!confirmed_move.has_value() ||
                                verification.score > confirmed_score) {
                                confirmed_score = verification.score;
                                confirmed_move = candidates[index].metadata.move;
                                confirmed_pv = verification.pv;
                            }
                        }
                        if (!context.aborted && confirmed_move.has_value()) {
                            if (confirmed_move != result.best_move ||
                                confirmed_score != score) {
                                ++context.stats.root_selective_researches;
                            }
                            score = confirmed_score;
                            result.score_cp = confirmed_score;
                            result.mate = mate_from_score(confirmed_score);
                            result.best_move = confirmed_move;
                            pv = confirmed_pv;
                            result.pv = confirmed_pv.length > 0 ? confirmed_pv.to_vector() :
                                std::vector<Move>{*confirmed_move};
                            bounded_fallback_result = false;
                        }
                    }
                }

                // A forcing capture can look marginally best at the first
                // full-width iteration while a quiet escape survives one
                // more ply.  Re-search a small capture-versus-quiet band
                // only for explicit depth-one diagnostics; timed searches
                // and deeper iterations retain their normal budget.
                const bool shallow_root_research =
                    (limits.depth.has_value() ||
                     (limits.movetime.has_value() &&
                      *limits.movetime <= kShortTimedSerialThreshold)) &&
                    !limits.search_moves_specified;
                const bool short_timed_root_research =
                    limits.movetime.has_value() &&
                    *limits.movetime >= kShortTimedFallbackMinimum &&
                    *limits.movetime <= kShortTimedSerialThreshold;
                const int shallow_root_margin = short_timed_root_research ? 50 : kRootSelectiveMargin;
                if (shallow_root_research &&
                    depth == 1 && !context.aborted && result.best_move.has_value() &&
                    !mate_from_score(score).has_value()) {
                    const auto best_metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [&result](const MoveMetadata& metadata) {
                            return metadata.move == *result.best_move;
                        });
                    if (best_metadata != legal_moves.end() && best_metadata->is_capture()) {
                        std::array<MoveMetadata, 3> candidates{};
                        std::size_t candidate_count = 0;
                        struct QuietCandidate {
                            int score = -kInfinity;
                            MoveMetadata metadata{};
                        };
                        std::array<QuietCandidate, 2> quiet_candidates{};
                        std::size_t quiet_count = 0;
                        for (std::size_t index = 0; index < context.root_move_score_count; ++index) {
                            const SearchContext::RootMoveScore& root_score =
                                context.root_move_scores[index];
                            // This band only nominates a candidate for a
                            // deeper full-window comparison. A scout
                            // score is useful discovery evidence here; it
                            // never becomes authoritative by itself.
                            if (!short_timed_root_research &&
                                root_score.score < score - shallow_root_margin) {
                                continue;
                            }
                            const auto metadata = std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&root_score](const MoveMetadata& move_metadata) {
                                    return move_metadata.move == root_score.move;
                                });
                            if (metadata == legal_moves.end() || metadata->move == *result.best_move ||
                                metadata->is_capture() || metadata->gives_check ||
                                metadata->move.promotion() != Promotion::none) {
                                continue;
                            }
                            QuietCandidate quiet{root_score.score, *metadata};
                            if (quiet_count < quiet_candidates.size()) {
                                quiet_candidates[quiet_count++] = quiet;
                            } else if (root_score.score > quiet_candidates.back().score) {
                                quiet_candidates.back() = quiet;
                            } else {
                                continue;
                            }
                            for (std::size_t quiet_index = quiet_count; quiet_index > 1; --quiet_index) {
                                if (quiet_candidates[quiet_index - 2].score >=
                                    quiet_candidates[quiet_index - 1].score) {
                                    break;
                                }
                                std::swap(quiet_candidates[quiet_index - 2],
                                          quiet_candidates[quiet_index - 1]);
                            }
                        }

                        candidates[candidate_count++] = *best_metadata;
                        for (std::size_t quiet_index = 0;
                             quiet_index < quiet_count && candidate_count < candidates.size();
                             ++quiet_index) {
                            candidates[candidate_count++] = quiet_candidates[quiet_index].metadata;
                        }
                        const bool has_quiet_candidate = quiet_count != 0;
                        if (has_quiet_candidate) {
                            int extended_score = -kInfinity;
                            std::optional<Move> extended_move;
                            PrincipalVariation extended_pv;
                            for (std::size_t index = 0; index < candidate_count; ++index) {
                                ++context.stats.root_selective_candidates;
                                const SearchContext::RootVerification verification =
                                    context.selectively_research_root_move(root, candidates[index],
                                                                           depth + 1);
                                if (context.aborted) {
                                    break;
                                }
                                if (!verification.authoritative) {
                                    continue;
                                }
                                if (!extended_move.has_value() || verification.score > extended_score) {
                                    extended_score = verification.score;
                                    extended_move = candidates[index].move;
                                    extended_pv = verification.pv;
                                }
                            }
                            // Compare the candidates at the same deeper horizon.  A
                            // shallow capture can carry a horizon bonus, so comparing a
                            // deeper quiet score against the old shallow score would keep
                            // poisoned captures.  A later candidate may hit the hard
                            // deadline; in that case the best completed candidate remains
                            // authoritative.
                            bool nominal_score_authoritative = false;
                            if (extended_move.has_value() && !context.aborted) {
                                if (const auto nominal = nominal_root_verification(
                                        *extended_move, depth); nominal.has_value()) {
                                    extended_score = nominal->score;
                                    extended_pv = nominal->pv;
                                    nominal_score_authoritative = true;
                                }
                            }
                            if (extended_move.has_value() && nominal_score_authoritative &&
                                !context.aborted) {
                                if (extended_move != result.best_move || extended_score != score) {
                                    ++context.stats.root_selective_researches;
                                }
                                score = extended_score;
                                result.score_cp = score;
                                result.mate = mate_from_score(score);
                                 result.best_move = extended_move;
                                 pv = extended_pv;
                                 result.pv = extended_pv.to_vector();
                                 bounded_fallback_result = false;
                             }
                        }
                    } else if (best_metadata != legal_moves.end() &&
                               !best_metadata->is_capture() && !best_metadata->gives_check &&
                               best_metadata->move.promotion() == Promotion::none) {
                        // The inverse horizon trap is a quiet move that is only a few
                        // centipawns ahead of a check, capture, or promotion.  At an
                        // incomplete depth-one root the quiet score is not authoritative;
                        // compare the quiet move and a small forcing band at depth two.
                        // A quiet move can itself be forcing (for example, a fork or
                        // direct attack) even though it has no check flag. Include those
                        // moves in the same confirmation band so root ordering does not
                        // discard the tactical signal already used by the recursive search.
                        const PositionFeatures root_features = root.position_features();
                        const auto is_root_forcing_candidate = [&root, &root_features](
                            const MoveMetadata& metadata) {
                            if (metadata.is_capture() || metadata.gives_check ||
                                metadata.move.promotion() != Promotion::none) {
                                return true;
                            }
                            if (!quiet_move_has_direct_forcing_target(root_features, metadata) ||
                                !root.make_search_move(metadata)) {
                                return false;
                            }
                            const bool forcing = quiet_move_is_forcing(
                                root_features, root.position_features(), metadata);
                            (void)root.unmake_move();
                            return forcing;
                        };
                        std::array<MoveMetadata, 4> candidates{};
                        std::size_t candidate_count = 0;
                        bool has_quiet_forcing_candidate = false;
                        candidates[candidate_count++] = *best_metadata;
                        const auto append_candidates = [&] (const bool quiet_only) {
                            for (std::size_t index = 0;
                                 index < context.root_move_score_count &&
                                 candidate_count < candidates.size(); ++index) {
                                const SearchContext::RootMoveScore& root_score =
                                    context.root_move_scores[index];
                                // Scout scores may nominate a candidate;
                                // the bounded re-search below supplies the
                                // only score used for replacement.
                                if (root_score.move == *result.best_move ||
                                    root_score.score < score - shallow_root_margin) {
                                    continue;
                                }
                                const auto metadata = std::find_if(
                                    legal_moves.begin(), legal_moves.end(),
                                    [&root_score](const MoveMetadata& move_metadata) {
                                        return move_metadata.move == root_score.move;
                                    });
                                if (metadata == legal_moves.end()) {
                                    continue;
                                }
                                const bool conventional_forcing = metadata->is_capture() ||
                                    metadata->gives_check ||
                                    metadata->move.promotion() != Promotion::none;
                                if (quiet_only) {
                                    if (conventional_forcing ||
                                        !is_root_forcing_candidate(*metadata)) {
                                        continue;
                                    }
                                    has_quiet_forcing_candidate = true;
                                } else if (!conventional_forcing) {
                                    continue;
                                }
                                candidates[candidate_count++] = *metadata;
                            }
                        };
                        // Keep checks/captures/promotions ahead of the more
                        // expensive quiet-forcing confirmation candidates.
                        append_candidates(false);
                        append_candidates(true);

                        if (candidate_count > 1) {
                            int extended_score = -kInfinity;
                            std::optional<Move> extended_move;
                            bool extended_forcing = false;
                            bool estimated_candidate_used = false;
                            PrincipalVariation extended_pv;
                            const int confirmation_depth =
                                has_quiet_forcing_candidate && limits.depth.has_value() ?
                                    depth + 2 : depth + 1;
                            for (std::size_t index = 0; index < candidate_count; ++index) {
                                ++context.stats.root_selective_candidates;
                                const SearchContext::RootVerification verification =
                                    context.selectively_research_root_move(root, candidates[index],
                                                                           confirmation_depth);
                                if (context.aborted) {
                                    break;
                                }
                                // Fixed-depth-one move selection may use a
                                // complete deeper line as a tactical estimate.
                                // Its score never proves a bound or enters TT.
                                const bool usable_tactical_probe =
                                    verification.authoritative ||
                                    (limits.depth.has_value() && depth == 1 &&
                                     verification.line_complete);
                                if (!usable_tactical_probe) {
                                    continue;
                                }
                                estimated_candidate_used = estimated_candidate_used ||
                                    !verification.authoritative;
                                const bool candidate_forcing = candidates[index].is_capture() ||
                                    candidates[index].gives_check ||
                                    candidates[index].move.promotion() != Promotion::none;
                                if (!extended_move.has_value() || verification.score > extended_score ||
                                    (verification.score == extended_score && candidate_forcing &&
                                     !extended_forcing)) {
                                    extended_score = verification.score;
                                    extended_move = candidates[index].move;
                                    extended_forcing = candidate_forcing;
                                    extended_pv = verification.pv;
                                }
                            }
                            bool nominal_line_complete = false;
                            if (extended_move.has_value() && !context.aborted) {
                                if (const auto nominal = nominal_root_verification(
                                        *extended_move, depth,
                                        limits.depth.has_value() && depth == 1);
                                    nominal.has_value()) {
                                    extended_score = nominal->score;
                                    extended_pv = nominal->pv;
                                    nominal_line_complete = true;
                                    root_authoritative = root_authoritative &&
                                        !estimated_candidate_used && nominal->authoritative;
                                }
                            }
                            if (extended_move.has_value() && nominal_line_complete &&
                                !context.aborted) {
                                if (extended_move != result.best_move || extended_score != score) {
                                    ++context.stats.root_selective_researches;
                                }
                                score = extended_score;
                                result.score_cp = score;
                                result.mate = mate_from_score(score);
                                result.best_move = extended_move;
                                pv = extended_pv;
                                result.pv = extended_pv.to_vector();
                            }
                        }
                    }
                }

                // A shallow fixed-depth iteration can leave two quiet root moves
                // inside the same narrow score band even though one move's first
                // forcing consequence is one ply deeper. Re-search only the first
                // ordered alternative in that band, and only at the diagnostic
                // depth where this horizon is meaningful. Timed searches and
                // deeper fixed-depth searches retain the normal PVS path.
                if (limits.depth.has_value() && !limits.search_moves_specified &&
                    depth == kRootSelectiveDepth &&
                    !context.aborted && result.best_move.has_value() &&
                    !mate_from_score(score).has_value()) {
                    const auto best_metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [&result](const MoveMetadata& metadata) {
                            return metadata.move == *result.best_move;
                        });
                    if (best_metadata != legal_moves.end() &&
                        !best_metadata->is_capture() && !best_metadata->gives_check &&
                        best_metadata->move.promotion() == Promotion::none) {
                        std::array<MoveMetadata, 3> candidates{};
                        std::size_t candidate_count = 0;
                        std::vector<MoveMetadata> forcing_candidates;
                        std::vector<MoveMetadata> quiet_candidates;
                        const PositionFeatures root_features = root.position_features();
                        const auto is_forcing_quiet = [&root, &root_features](
                            const MoveMetadata& metadata) {
                            if (!root.make_search_move(metadata)) {
                                return false;
                            }
                            const bool forcing = quiet_move_is_forcing(
                                root_features, root.position_features(), metadata);
                            (void)root.unmake_move();
                            return forcing;
                        };
                        for (std::size_t index = 0; index < context.root_move_score_count; ++index) {
                            const SearchContext::RootMoveScore& root_score =
                                context.root_move_scores[index];
                            // A scout bound is discovery-only. Every
                            // candidate selected from this band is
                            // re-searched before it can change the root.
                            if (root_score.move == *result.best_move ||
                                root_score.score < score - kRootSelectiveMargin) {
                                continue;
                            }
                            const auto metadata = std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&root_score](const MoveMetadata& move_metadata) {
                                    return move_metadata.move == root_score.move;
                                });
                            if (metadata != legal_moves.end() && !metadata->is_capture() &&
                                !metadata->gives_check &&
                                metadata->move.promotion() == Promotion::none) {
                                (is_forcing_quiet(*metadata) ? forcing_candidates :
                                    quiet_candidates).push_back(*metadata);
                            }
                        }

                        const auto append_candidates = [&candidates, &candidate_count](
                            const std::vector<MoveMetadata>& source) {
                            for (const MoveMetadata& metadata : source) {
                                if (candidate_count == candidates.size()) {
                                    break;
                                }
                                candidates[candidate_count++] = metadata;
                            }
                        };
                        append_candidates(forcing_candidates);
                        append_candidates(quiet_candidates);

                        const int shallow_score = score;
                        for (std::size_t index = 0; index < candidate_count; ++index) {
                            const MoveMetadata& candidate = candidates[index];
                            ++context.stats.root_selective_candidates;
                            const SearchContext::RootVerification verification =
                                context.selectively_research_root_move(root, candidate,
                                                                       depth + 1);
                            if (context.aborted) {
                                break;
                            }
                            if (!verification.authoritative) {
                                continue;
                            }
                            if (verification.score > shallow_score + kRootSelectiveImprovement) {
                                const auto nominal = nominal_root_verification(
                                    candidate.move, depth);
                                if (!nominal.has_value()) {
                                    continue;
                                }
                                ++context.stats.root_selective_researches;
                                score = nominal->score;
                                result.score_cp = score;
                                result.mate = mate_from_score(score);
                                result.best_move = candidate.move;
                                pv = nominal->pv;
                                result.pv = pv.to_vector();
                                bounded_fallback_result = false;
                                break;
                            }
                        }
                    }
                }

                // A shallow root can also leave two quiet checking moves
                // with nearly identical scores.  The incumbent may be an
                // exact first-move result while the alternative is only a
                // PVS upper bound, so ordinary root ranking is not a
                // comparable check-versus-check decision.  Re-search the
                // incumbent and at most two near-tied quiet checks at one
                // common full-window horizon; this is restricted to the
                // explicit depth-three diagnostic path and does not alter
                // timed or deeper searches.
                if (limits.depth.has_value() && !limits.search_moves_specified &&
                    depth == kRootSelectiveDepth &&
                    !context.aborted && result.best_move.has_value() &&
                    !root_score_snapshot.empty() &&
                    !mate_from_score(score).has_value()) {
                    const auto best_metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [&result](const MoveMetadata& metadata) {
                            return metadata.move == *result.best_move;
                        });
                    if (best_metadata != legal_moves.end() &&
                        !best_metadata->is_capture() && best_metadata->gives_check &&
                        best_metadata->move.promotion() == Promotion::none) {
                        std::vector<std::pair<int, MoveMetadata>> alternatives;
                        for (const SearchContext::RootMoveScore& root_score :
                             root_score_snapshot) {
                            if (root_score.move == *result.best_move ||
                                root_score.score < score - kRootSelectiveMargin) {
                                continue;
                            }
                            const auto metadata = std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&root_score](const MoveMetadata& move_metadata) {
                                    return move_metadata.move == root_score.move;
                                });
                            if (metadata == legal_moves.end() || metadata->is_capture() ||
                                !metadata->gives_check ||
                                metadata->move.promotion() != Promotion::none) {
                                continue;
                            }
                            alternatives.emplace_back(root_score.score, *metadata);
                        }
                        std::stable_sort(
                            alternatives.begin(), alternatives.end(),
                            [](const auto& left, const auto& right) {
                                if (left.first != right.first) {
                                    return left.first > right.first;
                                }
                                return detail::move_tie_break_key(left.second.move) <
                                    detail::move_tie_break_key(right.second.move);
                            });
                        if (alternatives.size() > kMaximumRootCheckConfirmationCandidates) {
                            alternatives.resize(kMaximumRootCheckConfirmationCandidates);
                        }

                        if (!alternatives.empty()) {
                            int confirmed_score = -kInfinity;
                            std::optional<Move> confirmed_move;
                            PrincipalVariation confirmed_pv;
                            ++context.stats.root_selective_candidates;
                                const SearchContext::RootVerification incumbent_verification =
                                    context.selectively_research_root_move(
                                        root, *best_metadata, depth + 1);
                            if (!context.aborted && incumbent_verification.authoritative) {
                                confirmed_score = incumbent_verification.score;
                                confirmed_move = best_metadata->move;
                                confirmed_pv = incumbent_verification.pv;
                            }
                            for (const auto& [unused_shallow_score, candidate] : alternatives) {
                                (void)unused_shallow_score;
                                if (context.aborted) {
                                    break;
                                }
                                ++context.stats.root_selective_candidates;
                                const SearchContext::RootVerification verification =
                                    context.selectively_research_root_move(
                                        root, candidate, depth + 1);
                                if (context.aborted) {
                                    break;
                                }
                                if (!verification.authoritative) {
                                    continue;
                                }
                                if (!confirmed_move.has_value() ||
                                    verification.score > confirmed_score) {
                                    confirmed_score = verification.score;
                                    confirmed_move = candidate.move;
                                    confirmed_pv = verification.pv;
                                }
                            }
                            if (!context.aborted && confirmed_move.has_value()) {
                                // The deeper confirmation chooses the move,
                                // but the public result still represents
                                // the requested fixed depth.  Refresh the
                                // selected line at that nominal depth so a
                                // deeper horizon score is never published
                                // as the depth-three evaluation.
                                const auto confirmed_metadata = std::find_if(
                                    legal_moves.begin(), legal_moves.end(),
                                    [&confirmed_move](const MoveMetadata& metadata) {
                                        return metadata.move == *confirmed_move;
                                    });
                                bool nominal_score_authoritative = false;
                                if (confirmed_metadata != legal_moves.end()) {
                                    ++context.stats.root_selective_candidates;
                                    const SearchContext::RootVerification nominal_verification =
                                        context.selectively_research_root_move(
                                            root, *confirmed_metadata, depth);
                                    if (!context.aborted && nominal_verification.authoritative) {
                                        confirmed_score = nominal_verification.score;
                                        confirmed_pv = nominal_verification.pv;
                                        nominal_score_authoritative = true;
                                    }
                                }
                                if (!nominal_score_authoritative) {
                                    confirmed_move.reset();
                                }
                            }
                            if (!context.aborted && confirmed_move.has_value()) {
                                if (confirmed_move != result.best_move ||
                                    confirmed_score != score) {
                                    ++context.stats.root_selective_researches;
                                }
                                score = confirmed_score;
                                result.score_cp = score;
                                result.mate = mate_from_score(score);
                                result.best_move = confirmed_move;
                                pv = confirmed_pv;
                                result.pv = confirmed_pv.length > 0 ? confirmed_pv.to_vector() :
                                    std::vector<Move>{*confirmed_move};
                                bounded_fallback_result = false;
                            }
                        }
                    }
                }

                // A quiet king move that removes active king-zone pressure
                // deserves a bounded confirmation when a shallow fixed-depth
                // root prefers a non-forcing move.  Compare the current root
                // move and at most two ordered king escapes at depth five;
                // this is intentionally limited to an explicit depth-three
                // diagnostic search so normal clock searches retain their
                // ordinary iterative-deepening budget.
                if (limits.depth.has_value() && *limits.depth == kRootSelectiveDepth &&
                    depth == kRootSelectiveDepth && !limits.search_moves_specified &&
                    !context.aborted && result.best_move.has_value() &&
                    !root_score_snapshot.empty() && !root.in_check() &&
                    !mate_from_score(score).has_value()) {
                    const auto best_metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [&result](const MoveMetadata& metadata) {
                            return metadata.move == *result.best_move;
                        });
                    const PositionFeatures root_features = root.position_features();
                    const std::size_t own_side = root.side_to_move() == Color::white ? 0U : 1U;
                    if (best_metadata != legal_moves.end() &&
                        !best_metadata->is_capture() && !best_metadata->gives_check &&
                        best_metadata->move.promotion() == Promotion::none &&
                        root_features.game_phase >= 8 && root_features.fullmove_number > 10 &&
                        root_features.king_zone_attacks[own_side] != 0) {
                        std::vector<std::pair<int, MoveMetadata>> defensive_candidates;
                        for (const SearchContext::RootMoveScore& root_score : root_score_snapshot) {
                            // Use the shallow value only to prioritize a
                            // bounded candidate list; depth-five
                            // confirmation remains authoritative.
                            if (root_score.move == *result.best_move) {
                                continue;
                            }
                            const auto metadata = std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&root_score](const MoveMetadata& move_metadata) {
                                    return move_metadata.move == root_score.move;
                                });
                            if (metadata == legal_moves.end() ||
                                metadata->moving_piece != PieceType::king ||
                                metadata->is_capture() || metadata->gives_check ||
                                metadata->move.promotion() != Promotion::none) {
                                continue;
                            }
                            if (!root.make_search_move(*metadata)) {
                                continue;
                            }
                            const PositionFeatures after_features = root.position_features();
                            (void)root.unmake_move();
                            if (after_features.king_zone_attacks[own_side] >=
                                root_features.king_zone_attacks[own_side]) {
                                continue;
                            }
                            defensive_candidates.emplace_back(root_score.score, *metadata);
                        }
                        std::stable_sort(
                            defensive_candidates.begin(), defensive_candidates.end(),
                            [](const auto& left, const auto& right) {
                                return left.first > right.first;
                            });
                        if (defensive_candidates.size() > kMaximumRootKingSafetyCandidates) {
                            defensive_candidates.resize(kMaximumRootKingSafetyCandidates);
                        }

                        if (!defensive_candidates.empty()) {
                            ++context.stats.king_safety_extensions;
                            ++context.stats.root_selective_candidates;
                            const SearchContext::RootVerification best_verification =
                                context.selectively_research_root_move(
                                    root, *best_metadata, kRootKingSafetySelectiveDepth);
                            if (!context.aborted &&
                                (best_verification.authoritative ||
                                 best_verification.line_complete)) {
                                int selected_score = best_verification.score;
                                Move selected_move = best_metadata->move;
                                PrincipalVariation selected_pv = best_verification.pv;
                                bool selected_from_estimate = false;
                                if (best_verification.authoritative) {
                                    for (const auto& [unused_shallow_score, candidate] :
                                         defensive_candidates) {
                                        (void)unused_shallow_score;
                                        if (context.aborted) {
                                            break;
                                        }
                                        ++context.stats.root_selective_candidates;
                                        const SearchContext::RootVerification verification =
                                            context.selectively_research_root_move(
                                                root, candidate, kRootKingSafetySelectiveDepth);
                                        if (context.aborted) {
                                            break;
                                        }
                                        if (!verification.authoritative) {
                                            continue;
                                        }
                                        if (verification.score > selected_score ||
                                            verification.score >= best_verification.score -
                                                kRootKingSafetyTieMargin) {
                                            selected_score = verification.score;
                                            selected_move = candidate.move;
                                            selected_pv = verification.pv;
                                        }
                                    }
                                } else {
                                    // With no proven incumbent score, do not
                                    // let its estimate veto every safe escape.
                                    // Choose only from completed lines and
                                    // exclude an escape already proven lost.
                                    bool found_escape_estimate = false;
                                    int escape_estimate_score = -kInfinity;
                                    for (const auto& [unused_shallow_score, candidate] :
                                         defensive_candidates) {
                                        (void)unused_shallow_score;
                                        if (context.aborted) {
                                            break;
                                        }
                                        ++context.stats.root_selective_candidates;
                                        const SearchContext::RootVerification verification =
                                            context.selectively_research_root_move(
                                                root, candidate, kRootKingSafetySelectiveDepth);
                                        if (context.aborted || !verification.line_complete ||
                                            verification.proves_losing_mate()) {
                                            continue;
                                        }
                                        if (!found_escape_estimate ||
                                            verification.score > escape_estimate_score) {
                                            found_escape_estimate = true;
                                            escape_estimate_score = verification.score;
                                            selected_score = verification.score;
                                            selected_move = candidate.move;
                                            selected_pv = verification.pv;
                                        }
                                    }
                                    selected_from_estimate = found_escape_estimate;
                                }
                                bool nominal_score_usable = false;
                                if (!context.aborted &&
                                    (best_verification.authoritative || selected_from_estimate)) {
                                    if (const auto nominal = nominal_root_verification(
                                            selected_move, depth, selected_from_estimate);
                                        nominal.has_value()) {
                                        selected_score = nominal->score;
                                        selected_pv = nominal->pv;
                                        nominal_score_usable = nominal->authoritative ||
                                            (selected_from_estimate && nominal->line_complete);
                                        if (selected_from_estimate || !nominal->authoritative) {
                                            root_authoritative = false;
                                        }
                                    }
                                }
                                if (nominal_score_usable && !context.aborted) {
                                    if (selected_move != result.best_move ||
                                        selected_score != result.score_cp) {
                                        ++context.stats.root_selective_researches;
                                    }
                                    result.score_cp = selected_score;
                                    result.mate = mate_from_score(selected_score);
                                     result.best_move = selected_move;
                                     pv = selected_pv;
                                     result.pv = pv.length > 0 ? pv.to_vector() :
                                         std::vector<Move>{selected_move};
                                     score = selected_score;
                                     bounded_fallback_result = false;
                                 }
                            }
                        }
                    }
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                // Helpers run in parallel with the main thread, so the `info`
                // stream reports their live node traffic too; otherwise a
                // threaded search looks slower than it is.
                const std::uint64_t helper_nodes =
                    lazy_pool != nullptr ? lazy_pool->live_nodes() : 0;
                const std::uint64_t helper_qnodes =
                    lazy_pool != nullptr ? lazy_pool->live_qnodes() : 0;
                const std::uint64_t visited = context.stats.nodes + context.stats.qnodes +
                    helper_nodes + helper_qnodes;
                SearchInfo info{depth, score, result.mate, visited,
                                elapsed.count() > 0 ? visited * 1000 /
                                    static_cast<std::uint64_t>(elapsed.count()) : visited,
                                elapsed, pv.to_vector()};
                info.seldepth = lazy_pool != nullptr ?
                    std::max(context.stats.seldepth, lazy_pool->live_seldepth()) :
                    context.stats.seldepth;
                info.qnodes = context.stats.qnodes + helper_qnodes;
                info.tt_hits = context.stats.tt_hits +
                    (lazy_pool != nullptr ? lazy_pool->live_tt_hits() : 0);
                info.tbhits = context.stats.tbhits +
                    (lazy_pool != nullptr ? lazy_pool->live_tbhits() : 0);
                if (!root_authoritative) {
                    // A selective root pass has a usable PV and score estimate,
                    // but its unresolved children give no proven direction.
                    // Republish the most recent proven score so a GUI never
                    // receives an unproven score, while the PV stays this
                    // iteration's own.
                    if (!interim_state.lines.empty() &&
                        interim_state.lines.front().bound != SearchInfo::Bound::estimate) {
                        const SearchInfo& proven = interim_state.lines.front();
                        info.score_cp = proven.score_cp;
                        info.mate = proven.mate;
                        info.bound = proven.bound;
                    } else {
                        info.bound = SearchInfo::Bound::estimate;
                    }
                }
                publish_iteration_lines(sink, interim_state, std::vector<SearchInfo>{info});
                const int iteration_observation_score = bounded_fallback_result ?
                    searched_iteration_score : score;
                time_manager.observe_iteration(SearchIterationObservation{
                    depth,
                    iteration_observation_score,
                    bounded_fallback_result ? searched_best_move_changed :
                        (had_completed_iteration && previous_best_move != result.best_move),
                    bounded_fallback_result ? searched_pv_changed :
                        (had_completed_iteration && previous_pv != result.pv),
                    aspiration_researched,
                    visited});
                result.timing = time_manager.diagnostics();
                // Keep a bounded safety score visible in the result, but
                // anchor the next aspiration window to the full-width
                // search score when that correction is still active.
                if (root_authoritative) {
                    previous_score = iteration_observation_score;
                } else {
                    // A complete selective iteration is useful to the UCI
                    // caller, but its score is not a safe aspiration
                    // center. Force the next iteration to establish a
                    // fresh full-window baseline.
                    previous_score.reset();
                }
                previous_iteration_elapsed = elapsed - previous_report_elapsed;
                previous_report_elapsed = elapsed;

                if ((!unbounded && depth == maximum_depth) ||
                    time_manager.should_stop_after_iteration()) {
                    break;
                }
            }
            result.stats = context.stats;
        } else {
            std::atomic<std::uint64_t> global_nodes = 0;
            std::atomic<std::uint64_t>* global_nodes_ptr = time_manager.node_limit().has_value() ?
                &global_nodes : nullptr;
            const std::size_t worker_count = std::min(options.threads, legal_moves.size());
            const bool multi_pv = options.multi_pv > 1;
            RootWorkerPool pool(worker_count, *evaluator, *table, time_manager,
                                session->stop_requested(), global_nodes_ptr, evaluator_mutex_ptr, true,
                                options.quiet_history_side_hook, tablebase_binding,
                                root.side_to_move(), options.contempt_cp);
            SearchStats total_stats;
            auto root_context_storage = std::make_unique<SearchContext>(
                *evaluator, *table, time_manager, session->stop_requested(),
                global_nodes_ptr, evaluator_mutex_ptr, nullptr, true,
                options.quiet_history_side_hook, tablebase_binding, root.side_to_move(),
                options.contempt_cp);
            SearchContext& root_context = *root_context_storage;
            // The loop below records the depth being searched so interim
            // publications can report the in-flight iteration.
            int in_flight_depth = 0;
            std::vector<RootLine> lines;
            if (options.interim_report_interval.count() > 0) {
                // The pool's run() wait loop wakes on this cadence and calls
                // the tick on the runner thread; live worker traffic comes
                // from the per-worker high-water marks plus the runner's own
                // root work.
                pool.progress_tick_interval = options.interim_report_interval;
                pool.progress_tick = [&root_context, &pool, &sink, &interim_state,
                                      &in_flight_depth,
                                      interval = options.interim_report_interval] {
                    const std::uint64_t live = root_context.stats.nodes +
                        root_context.stats.qnodes + pool.live_visited();
                    const PrincipalVariation leader = pool.leader_pv_copy();
                    const PrincipalVariation* live_pv = leader.length > 0 ? &leader : nullptr;
                    maybe_publish_interim(sink, interim_state, live, in_flight_depth, live_pv, interval);
                };
            }

            MoveMetadataList parallel_moves = legal_moves;
            detail::SearchMoveOrdering root_ordering;
            root_ordering.order(root, parallel_moves, std::nullopt, 0);
            std::vector<RootScheduleRecord> root_schedule;
            root_schedule.reserve(parallel_moves.size());
            for (std::size_t index = 0; index < parallel_moves.size(); ++index) {
                root_schedule.push_back(
                    RootScheduleRecord{
                        parallel_moves[index].move, index, -kInfinity, false, false});
            }
            const auto schedule_record_for = [&root_schedule](const Move move)
                -> RootScheduleRecord* {
                const auto record = std::find_if(
                    root_schedule.begin(), root_schedule.end(),
                    [move](const RootScheduleRecord& candidate) {
                        return candidate.move == move;
                    });
                return record == root_schedule.end() ? nullptr : &*record;
            };
            bool used_short_fallback = false;
            const bool defer_nonchecking_short_fallback =
                ultra_short_nonchecked_fallback || ultra_short_nonchecked_forcing_root;
            if (short_tactical_fallback && !defer_nonchecking_short_fallback &&
                !defer_expensive_short_fallback) {
                if (const auto fallback = short_search_fallback_move(
                        root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                        &root_context.stats, allow_deep_checked_fallback, &fallback_control);
                    fallback.has_value()) {
                    result.best_move = fallback->move;
                    result.pv = {fallback->move};
                    const int fallback_score = normalize_root_fallback_score(fallback->score);
                    result.score_cp = fallback_score;
                    result.mate = mate_from_score(fallback_score);
                    used_short_fallback = true;
                }
            } else if (short_search_fallback && !defer_nonchecking_short_fallback) {
                if (const auto fallback = first_safe_short_search_move(root, parallel_moves);
                    fallback.has_value()) {
                    result.best_move = *fallback;
                    result.pv = {*fallback};
                    used_short_fallback = true;
                }
            }
            int maximum_depth =
                std::min(kMaximumSearchDepth, std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
            bool unbounded = limits.infinite || limits.ponder;
            if (limits.ponder && limits.depth.has_value()) {
                // Mirror of the serial driver: a ponder depth limit is a target,
                // not an iteration cap, so the search keeps deepening until
                // stop/ponderhit instead of repeating the final depth.
                maximum_depth = kMaximumSearchDepth;
            }
            std::optional<int> previous_score;
            std::chrono::milliseconds previous_iteration_elapsed{1};
            std::chrono::milliseconds previous_report_elapsed{0};
            for (int depth = 1;; depth = depth < maximum_depth ? depth + 1 : maximum_depth) {
                in_flight_depth = depth;
                if (auto conversion = session->take_ponderhit_limits(); conversion.has_value()) {
                    limits = std::move(*conversion);
                    time_manager.reconfigure(limits, root.side_to_move(), options.speed_percent,
                                             options.move_overhead_ms, options.slow_mover_percent,
                                             timing_context, options.dynamic_move_overhead);
                    maximum_depth = std::min(
                        kMaximumSearchDepth,
                        std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
                    unbounded = limits.infinite || limits.ponder;
                }
                if (!unbounded && depth > maximum_depth) {
                    break;
                }
                if (!unbounded && !time_manager.should_start_next_iteration(previous_iteration_elapsed * 2)) {
                    break;
                }
                if (session->stop_requested().load(std::memory_order_relaxed) ||
                    time_manager.should_stop(root_context.visited_nodes())) {
                    break;
                }

                root_context.begin_iteration(nullptr);
                if (!root_context.count_node(false)) {
                    accumulate_stats(total_stats, root_context.stats);
                    break;
                }
                const std::optional<std::chrono::milliseconds> iteration_time_budget =
                    time_manager.time_budget();
                const bool short_timed_root = iteration_time_budget.has_value() &&
                    *iteration_time_budget < kShortTimedFallbackMinimum;
                const bool root_check_extension = root.in_check() &&
                    depth < kMaximumSearchDepth && !short_timed_root;
                if (root_check_extension) {
                    ++root_context.stats.check_extensions;
                }

                std::optional<Move> tt_move;
                if (const auto entry = table_access.probe(search_transposition_key(root), 0);
                    entry.has_value()) {
                    ++root_context.stats.tt_hits;
                    if (!entry->best_move.is_no_move()) {
                        tt_move = entry->best_move;
                    }
                }
                // The previous completed root PV is the authoritative
                // iterative-deepening hint.  A TT move can come from a
                // displaced/partial worker and must not displace the
                // completed result at the root, matching Stockfish 19's
                // root PV precedence.
                if (result.completed_depth > 0 && result.best_move.has_value()) {
                    tt_move = result.best_move;
                }
                root_ordering.order(root, parallel_moves, tt_move, 0);
                // Keep the abort-safe fallback in step with the ordering: an
                // interrupted first iteration must answer with the best-ordered
                // move, never with the generation-order head.
                if (!parallel_moves.empty()) {
                    result.best_move = parallel_moves.front().move;
                    if (result.completed_depth == 0) {
                        result.pv = {*result.best_move};
                    }
                }
                if (depth > 1) {
                    std::vector<std::size_t> schedule_indices;
                    schedule_indices.reserve(parallel_moves.size());
                    for (std::size_t index = 0; index < parallel_moves.size(); ++index) {
                        schedule_indices.push_back(index);
                    }
                    std::stable_sort(
                        schedule_indices.begin(), schedule_indices.end(),
                        [&parallel_moves, &tt_move, &schedule_record_for](
                            const std::size_t left, const std::size_t right) {
                        const Move left_move = parallel_moves[left].move;
                        const Move right_move = parallel_moves[right].move;
                        const bool left_tt = tt_move.has_value() && left_move == *tt_move;
                        const bool right_tt = tt_move.has_value() && right_move == *tt_move;
                        if (left_tt != right_tt) {
                            return left_tt;
                        }

                        const RootScheduleRecord* left_record =
                            schedule_record_for(left_move);
                        const RootScheduleRecord* right_record =
                            schedule_record_for(right_move);
                        const bool left_scored = left_record != nullptr &&
                            left_record->has_previous_score;
                        const bool right_scored = right_record != nullptr &&
                            right_record->has_previous_score;
                        if (left_scored != right_scored) {
                            return left_scored;
                        }
                        const bool left_exact = left_scored && left_record->previous_exact;
                        const bool right_exact = right_scored && right_record->previous_exact;
                        if (left_exact != right_exact) {
                            return left_exact;
                        }
                        if (left_scored && right_scored &&
                            left_record->previous_score != right_record->previous_score) {
                            return left_record->previous_score > right_record->previous_score;
                        }
                        const std::size_t left_stable = left_record != nullptr ?
                            left_record->stable_index : left;
                        const std::size_t right_stable = right_record != nullptr ?
                            right_record->stable_index : right;
                        return left_stable < right_stable;
                    });

                    MoveMetadataList scheduled_moves;
                    for (const std::size_t index : schedule_indices) {
                        (void)scheduled_moves.push_back(parallel_moves[index]);
                    }
                    parallel_moves = scheduled_moves;
                }
                std::vector<std::size_t> stable_root_indices;
                stable_root_indices.reserve(parallel_moves.size());
                for (std::size_t index = 0; index < parallel_moves.size(); ++index) {
                    const RootScheduleRecord* record =
                        schedule_record_for(parallel_moves[index].move);
                    stable_root_indices.push_back(
                        record != nullptr ? record->stable_index : index);
                }

                lines.assign(parallel_moves.size(), RootLine{});

                SearchStats iteration_stats;
                bool aborted = false;
                bool aspiration_researched = false;
                int window_alpha = -kInfinity;
                int window_beta = kInfinity;
                int aspiration_window = kAspirationWindow + std::min(32, depth * 2);
                const bool use_aspiration = previous_score.has_value() && !multi_pv;
                if (use_aspiration) {
                    window_alpha = std::max(-kInfinity,
                                            *previous_score - aspiration_window);
                    window_beta = std::min(kInfinity,
                                           *previous_score + aspiration_window);
                }
                // Keep the two-worker fixed-depth path for non-checked
                // roots on shared root PVS so it benefits from the live
                // alpha bound.  At four workers,
                // fixed-depth benchmark parity is more important than the
                // racy order in which a moving root alpha is observed;
                // timed searches retain shared root PVS at every worker
                // count.
                // A checked root has only a small evasion set, and a
                // shared null-window alpha is both unnecessary and
                // nondeterministic there: concurrent equal evasions can
                // leave the stable-order line as an incomplete scout.
                // Search checked roots with full windows so every legal
                // evasion receives an authoritative score before the
                // stable root ranking is applied.
                const bool use_root_pvs = !multi_pv && !root.in_check() &&
                    (!limits.depth.has_value() || worker_count < 4);
                int aspiration_retry_count = 0;
                for (;;) {
                    SearchStats attempt_stats;
                    bool attempt_aborted = false;
                    bool window_failed_low = false;
                    bool window_failed_high = false;
                    // Every aspiration attempt must rank only lines
                    // searched under its own window.  If a later attempt
                    // is interrupted, retaining `completed` flags from a
                    // prior failed window would make stale bounds look
                    // like current root results.
                    std::fill(lines.begin(), lines.end(), RootLine{});
                    std::vector<int> move_previous_scores(parallel_moves.size(), 0);
                    std::vector<bool> move_has_previous(parallel_moves.size(), false);
                    for (std::size_t index = 0; index < parallel_moves.size(); ++index) {
                        const RootScheduleRecord* record =
                            schedule_record_for(parallel_moves[index].move);
                        if (record != nullptr && record->has_previous_score) {
                            move_previous_scores[index] = record->previous_score;
                            move_has_previous[index] = true;
                        }
                    }
                    pool.run(root, parallel_moves, depth, root_check_extension,
                             stable_root_indices, lines, use_root_pvs, root_static_eval,
                             window_alpha, window_beta, attempt_stats, attempt_aborted,
                             window_failed_low, window_failed_high,
                             move_previous_scores, move_has_previous,
                             options.multi_pv > 1);
                    accumulate_stats(iteration_stats, attempt_stats);
                    if (attempt_aborted) {
                        aborted = true;
                        break;
                    }
                    if (!use_aspiration || (!window_failed_low && !window_failed_high)) {
                        break;
                    }

                    aspiration_researched = true;
                    ++iteration_stats.aspiration_researches;
                    ++aspiration_retry_count;
                    if (aspiration_retry_count > 3) {
                        // A pathological score swing gets one final
                        // full-window search so no inexact aspiration line
                        // can become the completed root result.
                        window_alpha = -kInfinity;
                        window_beta = kInfinity;
                        continue;
                    }
                    aspiration_window = std::min(
                        kInfinity / 4, aspiration_window * 2 + 16);
                    if (window_failed_low) {
                        window_alpha = std::max(-kInfinity,
                                                window_alpha - aspiration_window);
                    }
                    if (window_failed_high) {
                        window_beta = std::min(kInfinity,
                                               window_beta + aspiration_window);
                    }
                }
                if (aborted) {
                    if (result.completed_depth == 0 && !used_short_fallback &&
                        defer_nonchecking_short_fallback) {
                        if (const auto fallback = short_search_fallback_move(
                                root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                                &root_context.stats, allow_deep_checked_fallback, &fallback_control,
                                true, true);
                            fallback.has_value()) {
                            result.best_move = fallback->move;
                            result.pv = {fallback->move};
                            const int fallback_score =
                                normalize_root_fallback_score(fallback->score);
                            result.score_cp = fallback_score;
                            result.mate = mate_from_score(fallback_score);
                            used_short_fallback = true;
                        }
                    }
                    accumulate_stats(iteration_stats, root_context.stats);
                    accumulate_stats(total_stats, iteration_stats);
                    const std::vector<std::size_t> partial_ranked_indices =
                        RootCoordinator::rank(lines);
                    const auto safe_partial = used_short_fallback ?
                        partial_ranked_indices.end() : std::find_if(
                        partial_ranked_indices.begin(), partial_ranked_indices.end(),
                        [&lines, &parallel_moves, &root](const std::size_t index) {
                            return index < parallel_moves.size() &&
                                lines[index].exact &&
                                !root_move_exposes_immediate_check(root, parallel_moves[index]) &&
                                !root_move_is_broad_quiet_check(root, parallel_moves[index]);
                        });
                    if (result.completed_depth == 0 &&
                        safe_partial != partial_ranked_indices.end()) {
                        const RootLine& partial_best = lines[*safe_partial];
                        result.score_cp = partial_best.score;
                        result.mate = mate_from_score(partial_best.score);
                        if (partial_best.pv.length > 0) {
                            result.best_move = partial_best.pv.moves[0];
                            result.pv = partial_best.pv.to_vector();
                        }
                    } else if (result.completed_depth == 0 && !used_short_fallback) {
                        // An interrupted first iteration is never an
                        // authoritative tactical result, and the root lines
                        // did not produce a safe exact one either. Fall back
                        // to the same last-resort safety scan the serial
                        // driver uses instead of answering with the
                        // pre-search ordered move, which may expose an
                        // immediate check.
                        if (const auto fallback = first_safe_short_search_move(
                                root, parallel_moves);
                            fallback.has_value()) {
                            result.best_move = *fallback;
                            result.pv = {*fallback};
                        }
                    }
                    break;
                }

                // Use one deterministic ranking path for both single-PV
                // and MultiPV roots. The explicit stable index matters
                // when concurrent workers finish equal-scoring lines in
                // different orders.
                std::vector<std::size_t> ranked_indices =
                    RootCoordinator::rank(lines);
                if (ranked_indices.empty()) {
                    if (result.completed_depth == 0 && !used_short_fallback &&
                        defer_nonchecking_short_fallback) {
                        if (const auto fallback = short_search_fallback_move(
                                root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                                &root_context.stats, allow_deep_checked_fallback, &fallback_control);
                            fallback.has_value()) {
                            result.best_move = fallback->move;
                            result.pv = {fallback->move};
                            const int fallback_score =
                                normalize_root_fallback_score(fallback->score);
                            result.score_cp = fallback_score;
                            result.mate = mate_from_score(fallback_score);
                            used_short_fallback = true;
                        }
                    }
                    accumulate_stats(iteration_stats, root_context.stats);
                    accumulate_stats(total_stats, iteration_stats);
                    break;
                }

                // Some fixed-depth threaded confirmations deliberately
                // look one or two plies deeper to choose a candidate. A
                // RootLine must not publish that selective score under
                // the shallower completed depth, so retain the ordinary
                // root lines until the selected move is refreshed at the
                // requested horizon.
                std::optional<std::vector<RootLine>> pre_selective_lines;
                bool needs_nominal_root_refresh = false;
                const auto snapshot_before_selective_update = [&]() {
                    if (!pre_selective_lines.has_value()) {
                        pre_selective_lines = lines;
                    }
                };

                // Root-parallel searches otherwise stop at the shallow result when a
                // short timed search finishes depth two. Re-search a small, stable set of
                // quiet moves whose immediate consequence is forcing so that the root gets
                // the same bounded tactical lookahead as the serial path.
                const bool diagnostic_fixed_depth_selective =
                    limits.depth.has_value() && depth == kRootSelectiveDepth;
                const bool short_timed_selective =
                     limits.movetime.has_value() &&
                     ((*limits.movetime >= kShortTimedFallbackMinimum &&
                       *limits.movetime <= kShortTimedSerialThreshold) ||
                      (*limits.movetime < kShortTimedFallbackMinimum &&
                       !root.in_check())) &&
                     depth + 1 == kRootSelectiveDepth;
                if (!multi_pv && !limits.search_moves_specified && !root_context.aborted &&
                    !time_manager.should_stop(root_context.visited_nodes()) &&
                    (diagnostic_fixed_depth_selective || short_timed_selective)) {
                    const std::size_t best_candidate_index = ranked_indices.front();
                    const RootLine& shallow_best = lines[best_candidate_index];
                    const MoveMetadata& best_metadata = parallel_moves[best_candidate_index];
                    if (!best_metadata.is_capture() && !best_metadata.gives_check &&
                        best_metadata.move.promotion() == Promotion::none &&
                        !mate_from_score(shallow_best.score).has_value()) {
                        const PositionFeatures root_features = root.position_features();
                        std::vector<std::pair<std::size_t, MoveMetadata>> candidates;
                        candidates.reserve(3);
                        for (const std::size_t candidate_index : ranked_indices) {
                            if (candidate_index == best_candidate_index ||
                                lines[candidate_index].score <
                                    shallow_best.score - kRootSelectiveMargin ||
                                !lines[candidate_index].completed) {
                                continue;
                            }
                            const MoveMetadata& metadata = parallel_moves[candidate_index];
                            if (metadata.is_capture() || metadata.gives_check ||
                                metadata.move.promotion() != Promotion::none) {
                                continue;
                            }
                            if (!root.make_search_move(metadata)) {
                                continue;
                            }
                            const bool forcing = quiet_move_is_forcing(
                                root_features, root.position_features(), metadata);
                            (void)root.unmake_move();
                            if (!forcing) {
                                continue;
                            }
                            candidates.emplace_back(candidate_index, metadata);
                            if (candidates.size() == 3) {
                                break;
                            }
                        }

                        for (const auto& [candidate_index, metadata] : candidates) {
                            if (time_manager.should_stop(root_context.visited_nodes())) {
                                break;
                            }
                            ++root_context.stats.root_selective_candidates;
                            const SearchContext::RootVerification verification =
                                root_context.selectively_research_root_move(root, metadata,
                                                                            depth + 1);
                            if (root_context.aborted) {
                                break;
                            }
                            if (!verification.authoritative) {
                                continue;
                            }
                            if (verification.score > shallow_best.score +
                                    kRootSelectiveImprovement) {
                                ++root_context.stats.root_selective_researches;
                                snapshot_before_selective_update();
                                lines[candidate_index].score = verification.score;
                                lines[candidate_index].pv = verification.pv;
                                lines[candidate_index].completed = true;
                                lines[candidate_index].exact = true;
                                lines[candidate_index].selective_bound = false;
                                lines[candidate_index].safe_upper_bound = false;
                                lines[candidate_index].selective_upper_bound = false;
                                needs_nominal_root_refresh = true;
                                ranked_indices = RootCoordinator::rank(lines);
                                break;
                            }
                        }
                    }
                }

                // At fixed depth one, a direct check/capture/promotion can be
                // hidden behind a shallow quiet root score.  Re-search only a
                // small near-tied forcing band so root-parallel fixed-depth
                // behavior retains the serial tactical preference without
                // changing the normal timed or deeper search budget.
                const bool threaded_depth_one_forcing_selective =
                    limits.depth.has_value() && depth == 1;
                if (!multi_pv && !limits.search_moves_specified &&
                    threaded_depth_one_forcing_selective && !root_context.aborted &&
                    !time_manager.should_stop(root_context.visited_nodes()) &&
                    !ranked_indices.empty()) {
                    const std::size_t best_candidate_index = ranked_indices.front();
                    const RootLine& shallow_best = lines[best_candidate_index];
                    const MoveMetadata& best_metadata = parallel_moves[best_candidate_index];
                    if (!best_metadata.is_capture() && !best_metadata.gives_check &&
                        best_metadata.move.promotion() == Promotion::none &&
                        !mate_from_score(shallow_best.score).has_value()) {
                        std::vector<std::pair<std::size_t, MoveMetadata>> candidates;
                        candidates.reserve(3);
                        for (const std::size_t candidate_index : ranked_indices) {
                            if (candidate_index == best_candidate_index ||
                                lines[candidate_index].score <
                                    shallow_best.score - kRootSelectiveMargin ||
                                !lines[candidate_index].completed) {
                                continue;
                            }
                            const MoveMetadata& metadata = parallel_moves[candidate_index];
                            if (!metadata.is_capture() && !metadata.gives_check &&
                                metadata.move.promotion() == Promotion::none) {
                                continue;
                            }
                            candidates.emplace_back(candidate_index, metadata);
                            if (candidates.size() == 3) {
                                break;
                            }
                        }

                        if (!candidates.empty()) {
                            // Compare the incumbent at the same horizon as
                            // every forcing alternative.  Comparing a
                            // depth-one quiet score directly with a depth-two
                            // forcing score lets a merely near-tied check
                            // replace a genuinely stronger quiet move.
                            ++root_context.stats.root_selective_candidates;
                            const SearchContext::RootVerification incumbent_verification =
                                root_context.selectively_research_root_move(
                                    root, best_metadata, depth + 1);
                            if (!root_context.aborted && incumbent_verification.authoritative) {
                                const int shallow_best_score = shallow_best.score;
                                int confirmed_score = incumbent_verification.score;
                                std::size_t confirmed_index = best_candidate_index;
                                snapshot_before_selective_update();
                                lines[best_candidate_index].score = incumbent_verification.score;
                                lines[best_candidate_index].pv = incumbent_verification.pv;
                                lines[best_candidate_index].completed = true;
                                lines[best_candidate_index].exact = true;
                                lines[best_candidate_index].selective_bound = false;
                                lines[best_candidate_index].safe_upper_bound = false;
                                lines[best_candidate_index].selective_upper_bound = false;
                                needs_nominal_root_refresh = true;

                                for (const auto& [candidate_index, metadata] : candidates) {
                                    ++root_context.stats.root_selective_candidates;
                                    const SearchContext::RootVerification verification =
                                        root_context.selectively_research_root_move(
                                            root, metadata, depth + 1);
                                    if (root_context.aborted) {
                                        break;
                                    }
                                    if (!verification.authoritative) {
                                        continue;
                                    }
                                    lines[candidate_index].score = verification.score;
                                    lines[candidate_index].pv = verification.pv;
                                    lines[candidate_index].completed = true;
                                    lines[candidate_index].exact = true;
                                    lines[candidate_index].selective_bound = false;
                                    lines[candidate_index].safe_upper_bound = false;
                                    lines[candidate_index].selective_upper_bound = false;
                                    if (verification.score > confirmed_score) {
                                        confirmed_score = verification.score;
                                        confirmed_index = candidate_index;
                                    }
                                }
                                if (!root_context.aborted) {
                                    if (confirmed_index != best_candidate_index ||
                                        confirmed_score != shallow_best_score) {
                                        ++root_context.stats.root_selective_researches;
                                    }
                                    ranked_indices = RootCoordinator::rank(lines);
                                }
                            }
                        }
                    }
                }

                if (needs_nominal_root_refresh) {
                    bool nominal_refresh_completed = false;
                    if (!root_context.aborted &&
                        !time_manager.should_stop(root_context.visited_nodes()) &&
                        !ranked_indices.empty()) {
                        const std::size_t selected_index = ranked_indices.front();
                        if (selected_index < parallel_moves.size()) {
                            ++root_context.stats.root_selective_candidates;
                            const SearchContext::RootVerification verification =
                                root_context.selectively_research_root_move(
                                    root, parallel_moves[selected_index], depth);
                            if (!root_context.aborted && verification.authoritative) {
                                lines[selected_index].score = verification.score;
                                lines[selected_index].pv = verification.pv;
                                lines[selected_index].completed = true;
                                lines[selected_index].exact = true;
                                lines[selected_index].selective_bound = false;
                                lines[selected_index].safe_upper_bound = false;
                                lines[selected_index].selective_upper_bound = false;
                                nominal_refresh_completed = true;
                                if (pre_selective_lines.has_value()) {
                                    // The selective pass may have updated
                                    // several candidates at a deeper
                                    // horizon. Keep only the selected
                                    // candidate's nominal-depth refresh;
                                    // restoring the other lines prevents
                                    // mixed-horizon scores from entering
                                    // ranking, MultiPV publication, or the
                                    // next iteration's schedule.
                                    const RootLine nominal_line = lines[selected_index];
                                    lines = *pre_selective_lines;
                                    lines[selected_index] = nominal_line;
                                    ranked_indices = RootCoordinator::rank(lines);
                                }
                            }
                        }
                    }
                    if (!nominal_refresh_completed && pre_selective_lines.has_value()) {
                        // A selective confirmation that cannot finish its
                        // nominal refresh is discovery-only. Restore the
                        // fully searched root lines before result and info
                        // publication, including the interrupted case.
                        lines = *pre_selective_lines;
                        ranked_indices = RootCoordinator::rank(lines);
                    }
                }

                const std::optional<Move> previous_best_move = result.best_move;
                const std::vector<Move> previous_pv = result.pv;
                const bool had_completed_iteration = result.completed_depth > 0;
                const bool complete_root_coverage = std::all_of(
                    lines.begin(), lines.end(), [](const RootLine& line) {
                        return line.searched;
                    });
                const bool has_exact_incumbent = !ranked_indices.empty() &&
                    lines[ranked_indices.front()].exact;
                const int exact_incumbent_score = has_exact_incumbent ?
                    lines[ranked_indices.front()].score : -kInfinity;
                const bool root_lines_have_safe_provenance = complete_root_coverage &&
                    std::all_of(lines.begin(), lines.end(), [has_exact_incumbent,
                                                             exact_incumbent_score](
                                                                const RootLine& line) {
                        return line.exact || line.safe_upper_bound ||
                            (has_exact_incumbent && line.selective_bound &&
                             line.selective_upper_bound &&
                             line.score <= exact_incumbent_score);
                    });
                const bool authoritative_root_iteration =
                    root_lines_have_safe_provenance && has_exact_incumbent;
                // A complete root pass is publishable even when normal
                // qsearch/selective provenance prevents strict authority.
                // Publication mirrors the serial path: any complete root
                // pass with a valid ranked PV advances the published
                // iteration. Strict provenance is reserved for
                // authoritative_root_iteration, which alone gates
                // aspiration seeding and proof-sensitive metadata.
                const bool publishable_root_iteration = complete_root_coverage &&
                    !ranked_indices.empty() &&
                    lines[ranked_indices.front()].completed &&
                    lines[ranked_indices.front()].pv.length > 0;

                // Remember completed lines for the next root schedule.
                // Selective estimates are useful ordering hints when the
                // iteration also contains an authoritative line.  If an
                // entire attempted iteration is selective, do not replace
                // the previous iteration's queue evidence unless there is
                // no completed iteration yet and the estimate is serving
                // as the initial fallback.
                for (std::size_t index = 0; index < lines.size(); ++index) {
                    if (!lines[index].completed) {
                        continue;
                    }
                    if (!authoritative_root_iteration && had_completed_iteration) {
                        continue;
                    }
                    if (RootScheduleRecord* record =
                            schedule_record_for(parallel_moves[index].move);
                        record != nullptr) {
                        record->previous_score = lines[index].score;
                        record->has_previous_score = true;
                        record->previous_exact = lines[index].exact;
                    }
                }

                const std::size_t best_index = ranked_indices.front();
                const RootLine& best_line = lines[best_index];

                if (publishable_root_iteration) {
                    result.completed_depth = depth;
                    result.score_cp = best_line.score;
                    result.mate = mate_from_score(best_line.score);
                    result.best_move = best_line.pv.length > 0 ?
                        std::optional<Move>{best_line.pv.moves[0]} : result.best_move;
                    if (best_line.pv.length > 0) {
                        result.pv = best_line.pv.to_vector();
                    } else if (result.best_move.has_value()) {
                        result.pv = {*result.best_move};
                    }
                } else if (!had_completed_iteration && best_line.completed) {
                    // Keep a fully returned selective line as a depth-zero
                    // fallback when no authoritative iteration exists yet.
                    // It remains deliberately outside completed-depth and
                    // aspiration authority.
                    result.score_cp = best_line.score;
                    result.mate = mate_from_score(best_line.score);
                    if (best_line.pv.length > 0) {
                        result.best_move = best_line.pv.moves[0];
                        result.pv = best_line.pv.to_vector();
                    }
                }
                const int searched_iteration_score = result.score_cp;
                const bool searched_best_move_changed = had_completed_iteration &&
                    previous_best_move != result.best_move;
                const bool searched_pv_changed = had_completed_iteration &&
                    previous_pv != result.pv;
                bool bounded_fallback_result = false;

                // The correction must also cover depth-two blind iterations in
                // the timed band: a cheaper fallback scan can flip
                // should_start_next_iteration's estimated-next-iteration guard
                // and let a blind depth two publish the poisoned capture
                // (batch 30: base 106+106 = 212 >= 209 stops, cheaper
                // 82+82 = 164 < 209 continues to depth two).
                if (authoritative_root_iteration && !multi_pv &&
                    (short_tactical_fallback || defer_nonchecking_short_fallback) &&
                    !defer_expensive_short_fallback &&
                    result.completed_depth <= 2 &&
                    (short_capture_needs_fallback(parallel_moves, result.best_move) ||
                     short_quiet_needs_fallback(root, parallel_moves, result.best_move) ||
                     short_quiet_exposes_immediate_check(
                         root, parallel_moves, result.best_move) ||
                     short_check_needs_fallback(parallel_moves, result.best_move) ||
                     short_checked_root_king_evasion_needs_fallback(
                         root, parallel_moves, result.best_move))) {
                    // Keep the post-iteration safety correction bounded;
                    // recursive fallback work is never started after a
                    // short explicit deadline.
                    auto fallback = short_search_fallback_move(
                        root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                        &root_context.stats, allow_deep_checked_fallback,
                        &fallback_control, true, true);
                    std::optional<Move> fallback_move;
                    int fallback_score = best_line.score;
                    if (fallback.has_value()) {
                        fallback_move = fallback->move;
                        fallback_score = normalize_root_fallback_score(fallback->score);
                    }
                    const auto current_metadata = result.best_move.has_value() ?
                        std::find_if(
                            parallel_moves.begin(), parallel_moves.end(),
                            [&result](const MoveMetadata& metadata) {
                                return metadata.move == *result.best_move;
                            }) : parallel_moves.end();
                    if (current_metadata != parallel_moves.end() &&
                        !current_metadata->is_capture() && !current_metadata->gives_check &&
                        current_metadata->move.promotion() == Promotion::none &&
                        (!fallback_move.has_value() ||
                         *fallback_move == current_metadata->move)) {
                        if (const auto checking_capture = scored_safe_checking_capture(
                                root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                                allow_deep_checked_fallback, &fallback_control);
                            checking_capture.has_value()) {
                            fallback_move = checking_capture->move;
                            fallback_score = normalize_root_fallback_score(checking_capture->score);
                        } else {
                            fallback_move.reset();
                        }
                    }
                    const bool should_replace = fallback_move.has_value() &&
                        short_fallback_should_replace_completed_root(
                            root, parallel_moves, result.best_move, *fallback_move);
                    if (should_replace) {
                        const std::optional<Move> superseded_move = result.best_move;
                        result.best_move = *fallback_move;
                        result.pv = {*fallback_move};
                        result.score_cp = fallback_score;
                        result.mate = mate_from_score(fallback_score);
                        // The info loop below reads the ranked root line,
                        // not result.pv. Replace that line as well so a
                        // bounded safety correction cannot publish a PV
                        // for the superseded move before bestmove.
                        if (!ranked_indices.empty()) {
                            RootLine& corrected_line = lines[ranked_indices.front()];
                            corrected_line.score = fallback_score;
                            corrected_line.exact = false;
                            corrected_line.selective_bound = false;
                            corrected_line.safe_upper_bound = false;
                            corrected_line.selective_upper_bound = false;
                            corrected_line.pv = {};
                            corrected_line.pv.moves[0] = *fallback_move;
                            corrected_line.pv.length = 1;
                        }
                        if (RootScheduleRecord* record =
                                schedule_record_for(*fallback_move);
                            record != nullptr) {
                            // Keep the next iteration's queue ordering in
                            // sync with the corrected authoritative root
                            // result. Otherwise the old line score would
                            // still make the superseded move look like
                            // the strongest continuation to workers.
                            record->previous_score = fallback_score;
                            record->has_previous_score = true;
                            record->previous_exact = false;
                        }
                        if (superseded_move.has_value() &&
                            *superseded_move != *fallback_move) {
                            if (RootScheduleRecord* record =
                                    schedule_record_for(*superseded_move);
                                record != nullptr) {
                                // The old line was superseded by a
                                // bounded safety decision. Do not let its
                                // stale high score outrank the corrected
                                // move when the next iteration is queued.
                                record->previous_score = -kInfinity;
                                record->has_previous_score = false;
                                record->previous_exact = false;
                            }
                        }
                        bounded_fallback_result = true;
                    }
                }

                accumulate_stats(iteration_stats, root_context.stats);
                accumulate_stats(total_stats, iteration_stats);
                if (root_context.aborted) {
                    break;
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                if (!publishable_root_iteration) {
                    // The attempted pass still consumed real search time,
                    // so use its duration to pace the next attempt while
                    // withholding publication and aspiration state.
                    previous_iteration_elapsed = std::max(
                        std::chrono::milliseconds{1}, elapsed - previous_report_elapsed);
                    if ((!unbounded && depth == maximum_depth) ||
                        time_manager.should_stop_after_iteration()) {
                        break;
                    }
                    continue;
                }

                const std::uint64_t helper_nodes =
                    lazy_pool != nullptr ? lazy_pool->live_nodes() : 0;
                const std::uint64_t helper_qnodes =
                    lazy_pool != nullptr ? lazy_pool->live_qnodes() : 0;
                const std::uint64_t visited = total_stats.nodes + total_stats.qnodes +
                    helper_nodes + helper_qnodes;
                const std::uint64_t nps = elapsed.count() > 0 ? visited * 1000 /
                    static_cast<std::uint64_t>(elapsed.count()) : visited;
                const std::size_t line_count = multi_pv ?
                    std::min(options.multi_pv, ranked_indices.size()) : std::size_t{1};
                // Proven-score substitution must be keyed by the root move,
                // not by publication rank: ranks reorder every iteration, so a
                // rank index can pair an old score with a different move's PV.
                const auto proven_info_for_move =
                    [&interim_state](const std::vector<Move>& current_pv) -> const SearchInfo* {
                        if (current_pv.empty()) {
                            return nullptr;
                        }
                        for (const SearchInfo& previous : interim_state.lines) {
                            if (!previous.pv.empty() &&
                                previous.pv.front() == current_pv.front() &&
                                previous.bound != SearchInfo::Bound::estimate) {
                                return &previous;
                            }
                        }
                        return nullptr;
                    };
                std::vector<SearchInfo> iteration_lines;
                iteration_lines.reserve(line_count);
                for (std::size_t rank = 0; rank < line_count; ++rank) {
                    const RootLine& line = lines[ranked_indices[rank]];
                    SearchInfo info{depth, line.score, mate_from_score(line.score), visited,
                                    nps, elapsed, line.pv.to_vector()};
                    info.seldepth = lazy_pool != nullptr ?
                        std::max(total_stats.seldepth, lazy_pool->live_seldepth()) :
                        total_stats.seldepth;
                    info.qnodes = total_stats.qnodes + helper_qnodes;
                    info.tt_hits = total_stats.tt_hits +
                        (lazy_pool != nullptr ? lazy_pool->live_tt_hits() : 0);
                    info.tbhits = total_stats.tbhits +
                        (lazy_pool != nullptr ? lazy_pool->live_tbhits() : 0);
                    info.multipv = static_cast<int>(rank + 1);
                    if (!authoritative_root_iteration) {
                        // Same rule as the serial path: a selective root pass
                        // has no proven score direction. Republish the most
                        // recent proven score for this rank so a GUI never
                        // receives an unproven score, while the PV stays this
                        // iteration's own.
                        if (const SearchInfo* proven = proven_info_for_move(info.pv);
                            proven != nullptr) {
                            info.score_cp = proven->score_cp;
                            info.mate = proven->mate;
                            info.bound = proven->bound;
                        } else {
                            info.bound = SearchInfo::Bound::estimate;
                        }
                    }
                    iteration_lines.push_back(info);
                }
                publish_iteration_lines(sink, interim_state, std::move(iteration_lines));
                const auto iteration_elapsed = elapsed - previous_report_elapsed;
                const int iteration_observation_score = bounded_fallback_result ?
                    searched_iteration_score : result.score_cp;
                time_manager.observe_iteration(SearchIterationObservation{
                    depth,
                    // Selective root confirmation is a full-window result
                    // and remains part of pacing. A bounded fallback is a
                    // caller-facing safety choice, so use the ordinary
                    // root line for hardness and aspiration state.
                    iteration_observation_score,
                    bounded_fallback_result ? searched_best_move_changed :
                        (had_completed_iteration && previous_best_move != result.best_move),
                    bounded_fallback_result ? searched_pv_changed :
                        (had_completed_iteration && previous_pv != result.pv),
                    aspiration_researched,
                    visited});
                result.timing = time_manager.diagnostics();
                if (authoritative_root_iteration) {
                    previous_score = iteration_observation_score;
                } else {
                    // A complete selective root pass is a valid published
                    // iteration, but its score is not a safe aspiration
                    // center. Force a fresh full-window baseline next.
                    previous_score.reset();
                }
                previous_iteration_elapsed = iteration_elapsed;
                previous_report_elapsed = elapsed;
                if ((!unbounded && depth == maximum_depth) ||
                    time_manager.should_stop_after_iteration()) {
                    break;
                }
            }
            result.stats = total_stats;
        }

        if (limits.ponder) {
            // A pondering search must never publish a bestmove on its own: UCI
            // requires one answer only after `stop` or `ponderhit`. A terminal
            // ponder root has nothing to deepen, and a node-limited ponder has
            // already spent its budget, so both wait here; a time/infinite
            // ponder only reaches this point after stop/ponderhit anyway. The
            // conversion (if any) is consumed so the caller can see the
            // converted limits.
            session->wait_until_stopped_or_ponderhit();
            (void)session->take_ponderhit_limits();
        }
        if (result.completed_depth == 0 && result.best_move.has_value()) {
            // Retain the emergency line for non-UCI search consumers that
            // use the final info callback to keep the published PV in sync
            // with bestmove. The UCI controller filters this depth-zero
            // diagnostic because UCI info depths are positive iterations.
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            const std::uint64_t visited = result.stats.nodes + result.stats.qnodes;
            const std::uint64_t nps = elapsed.count() > 0 ? visited * 1000 /
                static_cast<std::uint64_t>(elapsed.count()) : visited;
            result.pv = {*result.best_move};
            SearchInfo info{0, result.score_cp, mate_from_score(result.score_cp), visited,
                            nps, elapsed, result.pv};
            info.seldepth = result.stats.seldepth;
            info.qnodes = result.stats.qnodes;
            info.tt_hits = result.stats.tt_hits;
            info.tbhits = result.stats.tbhits;
            // The emergency line is a safety choice without a proven score
            // direction.
            info.bound = SearchInfo::Bound::estimate;
            safely_report_info(sink, info);
        }
        if (lazy_pool != nullptr) {
            // Helpers never publish a result; their only contribution is the
            // transposition-table traffic they generated and the node counts
            // merged here.
            lazy_pool->stop();
            accumulate_stats(result.stats, lazy_pool->stats());
            if (lazy_pool->failed()) {
                // The main iteration may have finished before the flag was
                // observed; keep the diagnostic honest either way.
                result.failed = true;
                result.failure_reason = SearchFailureReason::helper_failed;
            }
        }
        result.timing = time_manager.diagnostics();
    } catch (...) {
        result.failed = true;
        result.failure_reason = SearchFailureReason::internal_error;
        try {
                    if (!result.best_move.has_value()) {
                        const std::vector<Move> legal = root.legal_moves();
                for (const Move& move : legal) {
                    if (!limits.search_moves_specified ||
                        std::find(limits.search_moves.begin(), limits.search_moves.end(), move) !=
                            limits.search_moves.end()) {
                            result.best_move = move;
                            result.pv = {move};
                            break;
                    }
                }
            }
        } catch (...) {
        }
        if (limits.ponder) {
            // A failed ponder search must keep the protocol contract: UCI allows
            // exactly one bestmove, and only after stop/ponderhit. Without this
            // gate the catch path publishes an unsolicited bestmove and a later
            // ponderhit answers a second time.
            try {
                session->wait_until_stopped_or_ponderhit();
                (void)session->take_ponderhit_limits();
            } catch (...) {
            }
        }
    }

    result.stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    result.mate = mate_from_score(result.score_cp);
    result.completed = true;
    result.cancelled = session->stop_requested().load(std::memory_order_relaxed);
    (void)session->publish_completion(sink, result);
}

} // namespace koi::detail