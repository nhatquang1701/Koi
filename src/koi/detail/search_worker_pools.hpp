#pragma once

// Worker pools for the search drivers:
//  * LazySmpPool  -- Threads > 1, Lazy SMP helpers that only deposit into the
//                    shared transposition table and report statistics;
//  * RootWorkerPool -- root-parallel workers that evaluate one root move each.
//
// They are private implementation, kept out of search_runner.cpp so the driver
// owns flow/publication and the pools own worker lifetime and error capture.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "koi/detail/search_constants.hpp"
#include "koi/detail/search_context.hpp"
#include "koi/detail/search_stats_support.hpp"
#include "koi/detail/thread_stack.hpp"
#include "koi/search_types.hpp"

namespace koi::detail {

// Lazy SMP helper pool. The main thread keeps running the authoritative
// iterative deepening; each helper searches the same root with a depth offset
// so it can deposit lines into the shared transposition table that the main
// thread has not reached yet. Helper results are never published -- only their
// statistics are merged once the search finishes. The pool only exists for
// Threads > 1 without a node limit, so the single-threaded path stays
// bit-for-bit deterministic.
class LazySmpPool {
public:
    LazySmpPool(std::size_t helper_count, const GameState& root, MoveMetadataList root_moves,
                const Evaluator& evaluator, TranspositionTable& table, TimeManager& time_manager,
                std::atomic_bool& stop_requested, std::mutex* evaluator_mutex,
                bool use_transposition_table,
                SearchOptions::QuietHistorySideHook quiet_history_side_hook,
                TablebaseSearchBinding tablebase_binding = {},
                Color root_color = Color::white, int contempt_cp = 0)
        : root_(root), root_moves_(std::move(root_moves)), evaluator_(evaluator), table_(table),
          time_manager_(time_manager), stop_requested_(stop_requested),
          evaluator_mutex_(evaluator_mutex), use_transposition_table_(use_transposition_table),
          quiet_history_side_hook_(std::move(quiet_history_side_hook)),
          tablebase_binding_(std::move(tablebase_binding)), root_color_(root_color),
          contempt_cp_(contempt_cp),
          helper_stats_(std::max<std::size_t>(1, helper_count)) {
        helpers_.reserve(helper_stats_.size());
        try {
            for (std::size_t index = 0; index < helper_stats_.size(); ++index) {
                helpers_.emplace_back(spawn_worker_thread([this, index] {
                    helper_loop(index);
                }));
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    LazySmpPool(const LazySmpPool&) = delete;
    LazySmpPool& operator=(const LazySmpPool&) = delete;

    ~LazySmpPool() {
        stop();
    }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_relaxed);
        helper_abort_.store(true, std::memory_order_relaxed);
        for (WorkerThread& helper : helpers_) {
            if (helper.joinable()) {
                helper.join();
            }
        }
    }

    [[nodiscard]] SearchStats stats() const noexcept {
        SearchStats total{};
        for (const SearchStats& helper : helper_stats_) {
            accumulate_stats(total, helper);
        }
        return total;
    }

    // Live totals for the `info` stream. The search reports node counts while
    // helpers are still working, so the published NPS must include the traffic
    // helpers already generated; otherwise a threaded search under-reports its
    // own speed by the helper share. Each helper publishes the counters of the
    // iteration it just finished, so the totals are monotonic and never
    // double-count. `seldepth` keeps the maximum rather than a sum.
    [[nodiscard]] std::uint64_t live_nodes() const noexcept {
        return live_nodes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t live_qnodes() const noexcept {
        return live_qnodes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t live_tt_hits() const noexcept {
        return live_tt_hits_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t live_tbhits() const noexcept {
        return live_tbhits_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int live_seldepth() const noexcept {
        return live_seldepth_.load(std::memory_order_relaxed);
    }

    // The main driver publishes the depth it is about to search so helpers can
    // keep pace with it instead of re-walking the shallow iterations the main
    // thread has already finished. Depositing deeper lines into the shared
    // table is the only way a helper can pay for itself.
    void publish_depth(int depth) noexcept {
        main_depth_.store(depth, std::memory_order_relaxed);
    }

    // A helper that throws (bad_alloc under memory pressure, an evaluator
    // failure) must not let the exception cross the thread entry point, which
    // would call std::terminate and take the whole engine down. The failure is
    // recorded here and surfaced to the driver, which stops deepening and
    // publishes the best completed iteration (or the abort-safe fallback).
    [[nodiscard]] bool failed() const noexcept {
        return failed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string failure_message() const {
        std::lock_guard lock(failure_mutex_);
        return failure_message_;
    }

private:
    // A helper's iteration counters are appended to the live totals once the
    // iteration returns; the helper is the only writer of its own stats, so the
    // relaxed read-modify-write cannot double-count.
    void publish_iteration_stats(const SearchStats& iteration) noexcept {
        live_nodes_.fetch_add(iteration.nodes, std::memory_order_relaxed);
        live_qnodes_.fetch_add(iteration.qnodes, std::memory_order_relaxed);
        live_tt_hits_.fetch_add(iteration.tt_hits, std::memory_order_relaxed);
        live_tbhits_.fetch_add(iteration.tbhits, std::memory_order_relaxed);
        int current = live_seldepth_.load(std::memory_order_relaxed);
        while (iteration.seldepth > current &&
               !live_seldepth_.compare_exchange_weak(current, iteration.seldepth,
                                                     std::memory_order_relaxed)) {
        }
    }

    void record_failure(const char* message) noexcept {
        try {
            std::lock_guard lock(failure_mutex_);
            if (failure_message_.empty()) {
                failure_message_ = message;
            }
        } catch (...) {
            // The diagnostic is best-effort; the flags below are what stop the
            // search and keep the process alive.
        }
        failed_.store(true, std::memory_order_relaxed);
        helper_abort_.store(true, std::memory_order_relaxed);
    }

    void helper_loop(std::size_t helper_index) noexcept {
        try {
            helper_body(helper_index);
        } catch (const std::exception& error) {
            record_failure(error.what());
        } catch (...) {
            record_failure("an unknown error");
        }
    }

    void helper_body(std::size_t helper_index) {
        // A helper owns its own position copy, ordering tables and evaluator
        // worker; only the transposition table and the time budget are shared.
        // The published depth is re-read at the top of every iteration, so a
        // helper that starts late does not re-walk the shallow iterations, and
        // a helper already ahead keeps its own progress. The starting class
        // comes from the helper index (never from a race on the published
        // value), so the stagger is deterministic for a fixed thread count.
        GameState state = root_;
        auto context_storage = std::make_unique<SearchContext>(
            evaluator_, table_, time_manager_, stop_requested_, nullptr, evaluator_mutex_,
            &root_moves_, use_transposition_table_, quiet_history_side_hook_, tablebase_binding_,
            root_color_, contempt_cp_);
        SearchContext& context = *context_storage;
        int depth = 1 + static_cast<int>(helper_index % 3);
        while (!stopping_.load(std::memory_order_relaxed) &&
               !stop_requested_.load(std::memory_order_relaxed) &&
               !time_manager_.should_stop(0) && depth <= kMaximumSearchDepth) {
            const int published = std::max(1, main_depth_.load(std::memory_order_relaxed));
            depth = std::max(depth, published + static_cast<int>(helper_index % 3));
            PrincipalVariation pv;
            context.begin_iteration(&helper_abort_);
            context.negamax(state, depth, -kInfinity, kInfinity, 0, pv);
            accumulate_stats(helper_stats_[helper_index], context.stats);
            publish_iteration_stats(context.stats);
            if (stopping_.load(std::memory_order_relaxed) || context.aborted) {
                break;
            }
            ++depth;
        }
    }

    GameState root_;
    MoveMetadataList root_moves_;
    const Evaluator& evaluator_;
    TranspositionTable& table_;
    TimeManager& time_manager_;
    std::atomic_bool& stop_requested_;
    std::mutex* evaluator_mutex_ = nullptr;
    bool use_transposition_table_ = true;
    SearchOptions::QuietHistorySideHook quiet_history_side_hook_;
    TablebaseSearchBinding tablebase_binding_;
    Color root_color_ = Color::white;
    int contempt_cp_ = 0;
    std::vector<WorkerThread> helpers_;
    std::vector<SearchStats> helper_stats_;
    std::atomic<int> main_depth_{1};
    std::atomic_bool stopping_{false};
    std::atomic_bool helper_abort_{false};
    std::atomic_bool failed_{false};
    std::atomic<std::uint64_t> live_nodes_{0};
    std::atomic<std::uint64_t> live_qnodes_{0};
    std::atomic<std::uint64_t> live_tt_hits_{0};
    std::atomic<std::uint64_t> live_tbhits_{0};
    std::atomic<int> live_seldepth_{0};
    mutable std::mutex failure_mutex_;
    std::string failure_message_;
};

class RootWorkerPool {
public:
    RootWorkerPool(std::size_t worker_count, const Evaluator& evaluator, TranspositionTable& table,
                   TimeManager& time_manager, std::atomic_bool& stop_requested,
                   std::atomic<std::uint64_t>* global_nodes, std::mutex* evaluator_mutex,
                   bool use_transposition_table,
                   SearchOptions::QuietHistorySideHook quiet_history_side_hook = {},
                   TablebaseSearchBinding tablebase_binding = {},
                   Color root_color = Color::white, int contempt_cp = 0)
        : worker_count_(std::max<std::size_t>(1, worker_count)), evaluator_(evaluator), table_(table),
          time_manager_(time_manager), stop_requested_(stop_requested), global_nodes_(global_nodes),
          evaluator_mutex_(evaluator_mutex), use_transposition_table_(use_transposition_table),
          quiet_history_side_hook_(std::move(quiet_history_side_hook)),
          tablebase_binding_(std::move(tablebase_binding)), root_color_(root_color),
          contempt_cp_(contempt_cp), worker_stats_(worker_count_),
          live_visited_(worker_count_) {
        workers_.reserve(worker_count_);
        try {
            for (std::size_t index = 0; index < worker_count_; ++index) {
                workers_.emplace_back(spawn_worker_thread([this, index] {
                    worker_loop(index);
                }));
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    RootWorkerPool(const RootWorkerPool&) = delete;
    RootWorkerPool& operator=(const RootWorkerPool&) = delete;

    ~RootWorkerPool() {
        shutdown();
    }

    void run(const GameState& root, const MoveMetadataList& root_moves, int depth,
             bool root_check_extension, const std::vector<std::size_t>& stable_root_indices,
             std::vector<RootLine>& lines, bool use_root_pvs, int root_static_eval,
             int window_alpha, int window_beta, SearchStats& stats, bool& aborted,
             bool& window_failed_low, bool& window_failed_high,
             const std::vector<int>& move_previous_scores,
             const std::vector<bool>& move_has_previous, bool multi_pv) {
        auto job = std::make_shared<Job>();
        job->root = &root;
        job->root_moves = &root_moves;
        job->depth = depth;
        job->root_check_extension = root_check_extension;
        job->stable_root_indices = &stable_root_indices;
        job->lines = &lines;
        job->worker_stats = &worker_stats_;
        job->use_root_pvs = use_root_pvs;
        job->root_static_eval = root_static_eval;
        job->window_alpha = window_alpha;
        job->window_beta = window_beta;
        job->move_previous_scores = move_previous_scores;
        job->move_has_previous = move_has_previous;
        job->multi_pv = multi_pv;
        job->root_alpha.store(window_alpha, std::memory_order_relaxed);
        if (depth == 1) {
            job->root_features = root.position_features();
        }

        {
            std::lock_guard lock(mutex_);
            ++sequence_;
            job->sequence = sequence_;
            finished_workers_ = 0;
            job_ = job;
        }
        work_available_.notify_all();

        {
            std::unique_lock lock(mutex_);
            while (finished_workers_ != worker_count_ &&
                   !failed_.load(std::memory_order_relaxed)) {
                if (!progress_tick) {
                    // No interim tick configured: the original predicate wait.
                    work_finished_.wait(lock, [this] {
                        return finished_workers_ == worker_count_ ||
                            failed_.load(std::memory_order_relaxed);
                    });
                    continue;
                }
                const std::chrono::milliseconds interval =
                    std::max(std::chrono::milliseconds{1}, progress_tick_interval);
                work_finished_.wait_for(lock, interval, [this] {
                    return finished_workers_ == worker_count_ ||
                        failed_.load(std::memory_order_relaxed);
                });
                if (finished_workers_ == worker_count_) {
                    break;
                }
                // A tick publishes interim progress from the runner thread
                // only. It runs outside the job mutex and its failure cannot
                // affect the search or the completion accounting.
                const std::function<void()> tick = progress_tick;
                lock.unlock();
                try {
                    tick();
                } catch (...) {
                }
                lock.lock();
            }
        }

        stats = {};
        for (const SearchStats& worker_stats : worker_stats_) {
            accumulate_stats(stats, worker_stats);
        }
        aborted = failed_.load(std::memory_order_relaxed) ||
            job->aborted.load(std::memory_order_relaxed) ||
            stop_requested_.load(std::memory_order_relaxed);
        window_failed_high = job->window_failed_high.load(std::memory_order_relaxed);
        window_failed_low = false;
        if (!aborted && !window_failed_high && window_alpha > -kInfinity) {
            const bool has_exact_score_above_alpha = std::any_of(
                lines.begin(), lines.end(), [window_alpha](const RootLine& line) {
                    return line.completed && line.exact && line.score > window_alpha;
                });
            // A selective full-window line may still be useful for ranking,
            // but it cannot certify that the aspiration floor was cleared.
            // Only an exact root line may prevent a fail-low retry.
            window_failed_low = !has_exact_score_above_alpha;
        }
    }

    // Runner-thread interim-progress hook. While the pool runs, `run` wakes
    // every `progress_tick_interval` and calls the tick outside the job mutex
    // so the runner can publish refreshed info lines. An empty tick keeps the
    // plain predicate wait. Both members are set by the runner before run().
    std::function<void()> progress_tick;
    std::chrono::milliseconds progress_tick_interval{250};

    // Sum of the per-worker visited-node high-water marks. Monotonic for the
    // pool lifetime and read by the runner thread only.
    [[nodiscard]] std::uint64_t live_visited() const noexcept {
        std::uint64_t total = 0;
        for (const std::atomic<std::uint64_t>& slot : live_visited_) {
            total += slot.load(std::memory_order_relaxed);
        }
        return total;
    }

    // The freshest published root-line PV for the rank-one interim in-flight
    // line. Read under `mutex_`; persists across per-attempt line resets.
    [[nodiscard]] PrincipalVariation leader_pv_copy() {
        std::lock_guard lock(mutex_);
        return has_leader_pv_ ? leader_pv_ : PrincipalVariation{};
    }

private:
    struct Job {
        const GameState* root = nullptr;
        const MoveMetadataList* root_moves = nullptr;
        int depth = 0;
        bool root_check_extension = false;
        const std::vector<std::size_t>* stable_root_indices = nullptr;
        std::vector<RootLine>* lines = nullptr;
        std::vector<SearchStats>* worker_stats = nullptr;
        std::optional<PositionFeatures> root_features;
        int root_static_eval = 0;
        std::atomic<std::size_t> next_move = 0;
        std::atomic<int> root_alpha = -kInfinity;
        int window_alpha = -kInfinity;
        int window_beta = kInfinity;
        std::atomic_bool window_failed_high = false;
        std::atomic_bool aborted = false;
        bool use_root_pvs = false;
        std::uint64_t sequence = 0;
        // Per-move aspiration hints for the MultiPV path: the previous
        // completed score of each parallel root move, used to narrow the
        // search window around it.  Empty when no per-move hints exist.
        std::vector<int> move_previous_scores;
        std::vector<bool> move_has_previous;
        bool multi_pv = false;
    };

    void worker_loop(std::size_t worker_index) noexcept {
        // Mirror LazySmpPool: a throwing root worker (bad_alloc under memory
        // pressure, condition-variable/system_error) must not cross the thread
        // entry point, which would call std::terminate and take the engine down.
        try {
            worker_body(worker_index);
        } catch (...) {
            failed_.store(true, std::memory_order_relaxed);
            std::lock_guard lock(mutex_);
            if (job_) {
                job_->aborted.store(true, std::memory_order_relaxed);
            }
            stopping_ = true;
            work_available_.notify_all();
            work_finished_.notify_all();
        }
    }

    void worker_body(std::size_t worker_index) {
        // Root jobs are authoritative. The striped table keeps concurrent probe/store activity
        // safe without requiring a serial confirmation search.
        auto context_storage = std::make_unique<SearchContext>(
            evaluator_, table_, time_manager_, stop_requested_, global_nodes_,
            evaluator_mutex_, nullptr, use_transposition_table_, quiet_history_side_hook_,
            tablebase_binding_, root_color_, contempt_cp_);
        SearchContext& context = *context_storage;
        std::uint64_t seen_sequence = 0;

        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex_);
                work_available_.wait(lock, [this, seen_sequence] {
                    return stopping_ || (job_ != nullptr && job_->sequence != seen_sequence);
                });
                if (stopping_) {
                    return;
                }
                job = job_;
            }

            seen_sequence = job->sequence;
            // Total visited nodes before this job. The per-job stats reset
            // below, so combining the base with the running job counters
            // yields a monotonic high-water mark for interim progress lines.
            const std::uint64_t job_base = context.stats.nodes + context.stats.qnodes;
            context.begin_iteration(&job->aborted);
            // The serial root search initializes ply zero before descending
            // into its children. Root-parallel workers start directly at ply
            // one, so seed the same parent snapshot from the already computed
            // root evaluation; this keeps improving/opponent-worsening and
            // reduction hindsight identical without an extra evaluator call.
            detail::SearchFrame& root_frame = context.stack.frame(0);
            root_frame.static_eval = job->root_static_eval;
            root_frame.static_eval_valid = true;
            root_frame.in_check = job->root->in_check();
            root_frame.previous_move = Move::no_move();
            root_frame.current_move = Move::no_move();
            root_frame.move_count = 0;
            root_frame.reduction = 0;
            root_frame.cutoff_count = 0;
            root_frame.tt_pv = true;

            const auto search_root = [&context, &job, this](std::size_t move_index) {
                const MoveMetadata& root_move = (*job->root_moves)[move_index];
                try {
                    GameState child = *job->root;
                    if (!child.make_search_move(root_move)) {
                        // Root metadata is generated from the unchanged job
                        // position. A failed application therefore signals a
                        // broken root contract, not a move that may simply be
                        // omitted from an otherwise completed iteration.
                        context.request_abort();
                        job->aborted.store(true, std::memory_order_relaxed);
                        return;
                    }

                    PrincipalVariation child_pv;
                    int child_depth = job->depth - 1 +
                        (job->root_check_extension ? 1 : 0);
                    const int child_check_extensions_remaining =
                        job->root_check_extension ? kMaximumCheckExtensionsPerPath - 1 :
                            kMaximumCheckExtensionsPerPath;
                    if (job->depth == 1 && job->root_features.has_value() &&
                        !root_move.is_capture() && !root_move.gives_check &&
                        root_move.move.promotion() == Promotion::none &&
                        quiet_move_has_direct_forcing_target(*job->root_features, root_move) &&
                        quiet_move_is_forcing(*job->root_features, child.position_features(),
                                              root_move)) {
                        ++context.stats.quiet_forcing_extensions;
                        ++child_depth;
                    }
                    const int shared_alpha = job->root_alpha.load(std::memory_order_relaxed);
                    // MultiPV per-move aspiration: narrow the window around
                    // this move's previous completed score when one exists.
                    // The full job window remains the outer bound, so a failed
                    // aspiration re-searches exactly as the runner would.
                    int move_alpha = job->window_alpha;
                    int move_beta = job->window_beta;
                    if (job->multi_pv &&
                        move_index < job->move_has_previous.size() &&
                        job->move_has_previous[move_index]) {
                        const int aspiration_span =
                            kAspirationWindow + std::min(32, job->depth * 2);
                        move_alpha = std::max(job->window_alpha,
                                              job->move_previous_scores[move_index] - aspiration_span);
                        move_beta = std::min(job->window_beta,
                                             job->move_previous_scores[move_index] + aspiration_span);
                    }
                    const bool scout = job->use_root_pvs && move_index != 0 &&
                        shared_alpha > job->window_alpha;
                    bool exact_score = false;
                    bool completed_score = false;
                    bool child_selective_bound = false;
                    bool child_lower_bound = false;
                    int score_alpha = move_alpha;
                    int score = 0;
                    if (scout) {
                        ++context.stats.root_pvs_searches;
                        score_alpha = shared_alpha;
                        bool scout_selective_bound = false;
                        score = -context.negamax(child, child_depth,
                                                 -shared_alpha - 1, -shared_alpha, 1, child_pv,
                                                 root_move.move, true,
                                                 child_check_extensions_remaining,
                                                 Move::no_move(), nullptr,
                                                 &scout_selective_bound, nullptr,
                                                 &child_lower_bound);
                        child_selective_bound = scout_selective_bound;
                        if (!context.aborted && score >= shared_alpha) {
                            ++context.stats.root_pvs_researches;
                            child_pv = {};
                            // A scout that returns exactly the current root
                            // alpha may be an upper-bound hit rather than an
                            // exact tie.  Re-search equality from the full
                            // aspiration floor so deterministic stable-index
                            // ranking sees every genuinely equal line.
                            const int research_alpha = score > shared_alpha ?
                                shared_alpha : job->window_alpha;
                            score_alpha = research_alpha;
                            bool research_selective_bound = false;
                            score = -context.negamax(child, child_depth,
                                                     -move_beta, -research_alpha, 1,
                                                     child_pv,
                                                     root_move.move, true,
                                                     child_check_extensions_remaining,
                                                     Move::no_move(), nullptr,
                                                     &research_selective_bound, nullptr,
                                                     &child_lower_bound);
                            child_selective_bound = research_selective_bound;
                            completed_score = !context.aborted &&
                                score > move_alpha && score < move_beta;
                            exact_score = score > research_alpha &&
                                score < move_beta && !child_selective_bound;
                        }
                    } else {
                        score = -context.negamax(child, child_depth,
                                                 -move_beta, -move_alpha, 1, child_pv,
                                                 root_move.move, true,
                                                 child_check_extensions_remaining,
                                                 Move::no_move(), nullptr,
                                                 &child_selective_bound, nullptr,
                                                 &child_lower_bound);
                        // A narrowed per-move window that fails must widen in
                        // the worker: the runner's aspiration retry does not
                        // rerun MultiPV iterations, so a bound-limited score
                        // would otherwise never become authoritative.
                        if (!context.aborted && !child_selective_bound &&
                            (move_alpha > job->window_alpha || move_beta < job->window_beta) &&
                            (score <= move_alpha || score >= move_beta)) {
                            ++context.stats.root_pvs_researches;
                            child_pv = {};
                            bool widen_selective_bound = false;
                            score = -context.negamax(child, child_depth,
                                                     -job->window_beta, -job->window_alpha, 1,
                                                     child_pv,
                                                     root_move.move, true,
                                                     child_check_extensions_remaining,
                                                     Move::no_move(), nullptr,
                                                     &widen_selective_bound, nullptr,
                                                     &child_lower_bound);
                            child_selective_bound = widen_selective_bound;
                            score_alpha = job->window_alpha;
                            move_alpha = job->window_alpha;
                            move_beta = job->window_beta;
                        }
                        completed_score = !context.aborted &&
                            score > move_alpha && score < move_beta;
                        exact_score = score > move_alpha &&
                            score < move_beta && !child_selective_bound;
                    }
                    // A selective child can return a useful score at the
                    // aspiration ceiling without proving that the root move
                    // truly failed high.  Only an authoritative child may
                    // widen the aspiration window; otherwise a selective
                    // estimate can repeatedly drive the root away from the
                    // previous completed score.
                    if (score >= job->window_beta && !child_selective_bound) {
                        job->window_failed_high.store(true, std::memory_order_relaxed);
                    } else if (move_beta < job->window_beta && !child_selective_bound &&
                               score >= move_beta) {
                        // A per-move aspiration fail-high must widen too: the
                        // runner's window retry re-searches the whole root.
                        job->window_failed_high.store(true, std::memory_order_relaxed);
                    }
                    if (context.aborted) {
                        job->aborted.store(true, std::memory_order_relaxed);
                        return;
                    }
                    if (!child.native_shadow_consistent()) {
                        context.request_abort();
                        job->aborted.store(true, std::memory_order_relaxed);
                        return;
                    }

                    int observed_alpha = job->root_alpha.load(std::memory_order_relaxed);
                    while (exact_score && score > observed_alpha &&
                           !job->root_alpha.compare_exchange_weak(
                               observed_alpha, score, std::memory_order_relaxed,
                               std::memory_order_relaxed)) {
                    }

                    { std::lock_guard line_lock(mutex_); RootLine& line = (*job->lines)[move_index];
                    line.searched = true;
                    line.score = score;
                    line.stable_index = (*job->stable_root_indices)[move_index];
                    line.pv.prepend(root_move.move, child_pv);
                    if (line.pv.length > 0) {
                        leader_pv_ = line.pv;
                        has_leader_pv_ = true;
                    }
                    // A scout fail-low is an upper bound for this root move,
                    // not a completed comparable score. A full-window
                    // selective result, however, is still useful when a
                    // full-window fallback must rank otherwise incomplete
                    // root lines. Keep that estimate out of exact aspiration
                    // authority and shared-alpha publication.
                    line.completed = completed_score;
                    line.exact = exact_score;
                    line.selective_bound = child_selective_bound;
                    // A non-selective fail-low is an upper bound for this root
                    // move at the alpha used by its final search. A selective
                    // fail-low is safe only when the child explicitly proved
                    // a lower bound before negation; unresolved/upper-bound
                    // selective paths must block root authority.
                    line.safe_upper_bound = !exact_score && score <= score_alpha &&
                        (!child_selective_bound || child_lower_bound);
                    line.selective_upper_bound = child_selective_bound && child_lower_bound; }
                } catch (...) {
                    context.request_abort();
                    job->aborted.store(true, std::memory_order_relaxed);
                }
            };

            while (!job->aborted.load(std::memory_order_relaxed) &&
                   !stop_requested_.load(std::memory_order_relaxed)) {
                const std::size_t move_index = job->next_move.fetch_add(1, std::memory_order_relaxed);
                if (move_index >= job->root_moves->size()) {
                    break;
                }
                search_root(move_index);
                // Refresh this worker's high-water mark so the runner's
                // interim tick can sum live traffic without touching the
                // worker-local context.
                live_visited_[worker_index].store(
                    job_base + context.stats.nodes + context.stats.qnodes,
                    std::memory_order_relaxed);
            }

            (*job->worker_stats)[worker_index] = context.stats;
            {
                std::lock_guard lock(mutex_);
                ++finished_workers_;
            }
            work_finished_.notify_one();
        }
    }

    void shutdown() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_available_.notify_all();
        for (WorkerThread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    const std::size_t worker_count_;
    const Evaluator& evaluator_;
    TranspositionTable& table_;
    TimeManager& time_manager_;
    std::atomic_bool& stop_requested_;
    std::atomic<std::uint64_t>* global_nodes_;
    std::mutex* evaluator_mutex_;
    bool use_transposition_table_;
    SearchOptions::QuietHistorySideHook quiet_history_side_hook_;
    TablebaseSearchBinding tablebase_binding_;
    Color root_color_ = Color::white;
    int contempt_cp_ = 0;
    std::vector<WorkerThread> workers_;
    std::vector<SearchStats> worker_stats_;
    // Per-worker visited-node high-water marks, cumulative across jobs. Each
    // worker is the only writer of its own slot; the runner thread sums them
    // for interim `info` lines while the job is still running.
    std::vector<std::atomic<std::uint64_t>> live_visited_;
    std::mutex mutex_;
    // The freshest published root-line PV, guarded by `mutex_`. Interim
    // reports use it for the rank-one in-flight line; it persists across
    // the per-attempt line resets so even a slow instrumented build has a
    // live PV to report.
    PrincipalVariation leader_pv_{};
    bool has_leader_pv_ = false;
    std::condition_variable work_available_;
    std::condition_variable work_finished_;
    std::shared_ptr<Job> job_;
    std::atomic_bool failed_{false};
    std::size_t finished_workers_ = 0;
    std::uint64_t sequence_ = 0;
    bool stopping_ = false;
};

inline void safely_report_info(const SearchEventSink& sink, const SearchInfo& info) {
    if (!sink.on_info) {
        return;
    }
    try {
        sink.on_info(info);
    } catch (...) {
    }
}

// Snapshot of the most recently published completed iteration. Interim
// progress lines clone this state so a repeated depth keeps the completed
// score, PV, and bound while only the live counters are refreshed. The state
// is touched by the runner thread only; every sink call stays there.
struct InterimReportState {
    std::vector<SearchInfo> lines;
    // Elapsed-time origin for refreshed `time`/`nps` values.
    std::chrono::steady_clock::time_point started{};
    std::chrono::steady_clock::time_point last_publish{};
};

// Publishes a completed iteration (one line for single-PV, one per MultiPV
// rank) through the sink and remembers it for interim refresh.
inline void publish_iteration_lines(const SearchEventSink& sink, InterimReportState& state,
                             std::vector<SearchInfo> lines) {
    for (const SearchInfo& info : lines) {
        safely_report_info(sink, info);
    }
    state.lines = std::move(lines);
    state.last_publish = std::chrono::steady_clock::now();
}

// Republishes the stored iteration snapshot with refreshed node counters when
// a long iteration has run past the configured cadence. The score, bound, and
// tbhits stay those of the completed iteration while the reported depth and
// the principal variation track the in-flight iteration; `nodes` is clamped
// against the live total so the GUI readout never goes backwards.
inline void maybe_publish_interim(const SearchEventSink& sink, InterimReportState& state,
                           const std::uint64_t live_nodes, const int live_depth,
                           const PrincipalVariation* live_pv,
                           const std::chrono::milliseconds interval) {
    if (interval.count() <= 0 || state.lines.empty() || state.lines.front().depth < 1) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - state.last_publish < interval) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.started);
    bool first_line = true;
    for (const SearchInfo& stored : state.lines) {
        SearchInfo info = stored;
        info.depth = std::max(stored.depth, live_depth);
        info.seldepth = std::max(stored.seldepth, info.depth);
        const bool use_live_pv = first_line && live_pv != nullptr && live_pv->length > 0;
        first_line = false;
        if (use_live_pv) {
            info.pv = live_pv->to_vector();
            // Report the root move of the in-flight principal variation as the
            // current move so interval lines show live progress instead of
            // repeating only the completed iteration.  The PV can still be
            // empty at the very first interim tick, so guard the access.
            if (!info.pv.empty()) {
                info.currmove = info.pv.front();
                info.currmovenumber = 1;
            }
        } else if (info.currmove.is_no_move() && !info.pv.empty()) {
            // The in-flight PV is not published yet on this path; still report
            // the root move of the line being shown so GUIs get live progress.
            info.currmove = info.pv.front();
            info.currmovenumber = 1;
        }
        info.nodes = std::max(stored.nodes, live_nodes);
        info.nps = elapsed.count() > 0 ? info.nodes * 1000 /
            static_cast<std::uint64_t>(elapsed.count()) : info.nodes;
        info.elapsed = elapsed;
        safely_report_info(sink, info);
    }
    state.last_publish = now;
}

} // namespace koi::detail
