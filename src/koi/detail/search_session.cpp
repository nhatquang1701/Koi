#include "koi/detail/search_session.hpp"

#include <stdexcept>
#include <utility>

#include "koi/detail/thread_stack.hpp"

namespace koi::detail {

SearchSession::SearchSession(GameState root, SearchLimits limits, SearchOptions options)
    : root_(std::move(root)), limits_(std::move(limits)), options_(std::move(options)),
      identity_(SearchRequestIdentity::from(root_, options_.generation)) {}

SearchSession::~SearchSession() {
    stop();
    wait();
    std::jthread timer_to_join;
    {
        std::lock_guard lock(stop_timer_mutex_);
        finished_ = true;
        timer_to_join = std::move(stop_timer_);
    }
}

void SearchSession::launch(std::function<void()> work) {
    if (!work || worker_.joinable()) {
        throw std::logic_error("search session can only launch one worker");
    }
    stop_requested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    try {
        worker_ = spawn_worker_thread([this, work = std::move(work)]() mutable {
            try {
                work();
            } catch (...) {
                // SearchService owns result construction and its worker body
                // already converts failures into a failed SearchResult. This
                // guard keeps the lifecycle state correct for other private
                // session users as well.
            }
            mark_finished();
        });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        throw;
    }
}

void SearchSession::stop() noexcept {
    stop_internal(true);
}

void SearchSession::stop_internal(const bool cancel_timer) noexcept {
    // The stop mutex serializes with the wait predicates so the store cannot
    // land between a waiter's predicate check and its block, which would lose
    // the wakeup and hang the joining caller (and with it the UCI loop).
    // request_ponderhit takes the same lock for the same reason.
    try {
        std::lock_guard stop_lock(stop_mutex_);
        stop_requested_.store(true, std::memory_order_relaxed);
        stop_condition_.notify_all();
    } catch (...) {
        // A failed lock is unrecoverable here; publish the flag anyway so the
        // next predicate evaluation observes the stop.
        stop_requested_.store(true, std::memory_order_relaxed);
        stop_condition_.notify_all();
    }
    if (cancel_timer) {
        try {
            std::lock_guard timer_lock(stop_timer_mutex_);
            if (stop_timer_.joinable()) {
                stop_timer_.request_stop();
            }
        } catch (...) {
        }
    }
}

void SearchSession::wait() {
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void SearchSession::wait_until_stopped() {
    std::unique_lock lock(stop_mutex_);
    stop_condition_.wait(lock, [this] {
        return stop_requested_.load(std::memory_order_relaxed);
    });
}

void SearchSession::wait_until_stopped_or_ponderhit() {
    std::unique_lock lock(stop_mutex_);
    stop_condition_.wait(lock, [this] {
        return stop_requested_.load(std::memory_order_relaxed) ||
               ponderhit_requested_.load(std::memory_order_acquire);
    });
}

void SearchSession::request_ponderhit(
    SearchLimits limits, const std::optional<std::uint32_t> observed_overhead_ms) {
    // The stop mutex serializes with the wait predicates so a conversion
    // signalled between a predicate check and its wait is never lost.
    const auto received_at = std::chrono::steady_clock::now();
    std::lock_guard stop_lock(stop_mutex_);
    {
        std::lock_guard ponderhit_lock(ponderhit_mutex_);
        ponderhit_conversion_ = PonderhitConversion{
            std::move(limits), received_at, observed_overhead_ms};
    }
    ponderhit_requested_.store(true, std::memory_order_release);
    stop_condition_.notify_all();
}

void SearchSession::request_stop_after(std::chrono::milliseconds delay) {
    if (delay <= std::chrono::milliseconds::zero()) {
        return;
    }
    std::jthread next_timer([this, delay](std::stop_token token) {
        std::mutex wait_mutex;
        std::condition_variable_any wait_cv;
        auto remaining = delay;
        constexpr std::chrono::milliseconds kMaxSlice{24LL * 60 * 60 * 1000};
        while (remaining > std::chrono::milliseconds::zero()) {
            if (token.stop_requested() || stop_requested_.load(std::memory_order_acquire)) {
                return;
            }
            const auto slice = remaining < kMaxSlice ? remaining : kMaxSlice;
            std::unique_lock<std::mutex> lock(wait_mutex);
            const bool already_stopped = wait_cv.wait_for(lock, token, slice, [this] {
                return stop_requested_.load(std::memory_order_acquire);
            });
            if (already_stopped || token.stop_requested()) {
                return;
            }
            remaining -= slice;
        }
        if (token.stop_requested()) {
            return;
        }
        stop_internal(false);
    });
    std::jthread old_timer;
    {
        std::lock_guard lock(stop_timer_mutex_);
        if (finished_ || stop_requested_.load(std::memory_order_acquire)) {
            next_timer.request_stop();
            return;
        }
        old_timer = std::move(stop_timer_);
        stop_timer_ = std::move(next_timer);
    }
}

std::optional<PonderhitConversion> SearchSession::take_ponderhit() {
    if (!ponderhit_requested_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::lock_guard lock(ponderhit_mutex_);
    ponderhit_requested_.store(false, std::memory_order_release);
    return std::exchange(ponderhit_conversion_, std::nullopt);
}

bool SearchSession::publish_completion(const SearchEventSink& sink,
                                       const SearchResult& result) noexcept {
    if (!completion_once_.try_claim()) {
        return false;
    }
    running_.store(false, std::memory_order_release);
    stop_condition_.notify_all();
    if (sink.on_complete) {
        try {
            sink.on_complete(result);
        } catch (...) {
        }
    }
    return true;
}

void SearchSession::mark_finished() noexcept {
    running_.store(false, std::memory_order_release);
    stop_condition_.notify_all();
    try {
        std::lock_guard lock(stop_timer_mutex_);
        finished_ = true;
        if (stop_timer_.joinable()) {
            stop_timer_.request_stop();
        }
    } catch (...) {
    }
}

} // namespace koi::detail
