#include "koi/search_service.hpp"
#include "koi/transposition_table.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "koi/detail/search_runner.hpp"
#include "koi/detail/search_session.hpp"

namespace koi {

namespace detail {

namespace {

constexpr std::size_t kMaximumMultiPv = 16;

} // namespace

std::size_t normalized_threads(std::size_t threads) noexcept {
    return std::clamp(threads, std::size_t{1}, maximum_search_threads());
}

std::uint8_t normalized_speed(std::uint8_t speed_percent) noexcept {
    return static_cast<std::uint8_t>(std::clamp<std::uint32_t>(speed_percent, 1, 100));
}

std::size_t normalized_multi_pv(std::size_t multi_pv) noexcept {
    return std::clamp(multi_pv, std::size_t{1}, kMaximumMultiPv);
}

std::uint32_t normalized_move_overhead(std::uint32_t move_overhead_ms) noexcept {
    return std::min<std::uint32_t>(move_overhead_ms, 5'000);
}

std::uint32_t normalized_slow_mover(std::uint32_t slow_mover_percent) noexcept {
    return std::clamp<std::uint32_t>(slow_mover_percent, 10, 1'000);
}

std::uint32_t normalized_elo(std::uint32_t elo) noexcept {
    return std::clamp<std::uint32_t>(elo, 1'320, 3'190);
}

} // namespace detail

struct SearchService::Impl {
    explicit Impl(std::shared_ptr<const Evaluator> evaluator, HashMemoryPolicy hash_memory_policy,
                  std::size_t initial_hash_mb)
        : evaluator(std::move(evaluator)),
          table(std::make_shared<TranspositionTable>(initial_hash_mb, std::move(hash_memory_policy))),
          evaluator_mutex(std::make_shared<std::mutex>()) {}

    // Guards the evaluator pointer against concurrent start()/set_evaluator()
    // calls.  evaluator_mutex is unrelated: it serializes evaluation work on
    // evaluators that cannot run concurrently, never configuration.
    mutable std::mutex config_mutex;
    std::shared_ptr<const Evaluator> evaluator;
    std::shared_ptr<TranspositionTable> table;
    std::shared_ptr<std::mutex> evaluator_mutex;
};

SearchHandle::SearchHandle(std::shared_ptr<detail::SearchSession> state) noexcept
    : state_(std::move(state)) {}

SearchHandle::SearchHandle(SearchHandle&&) noexcept = default;

SearchHandle& SearchHandle::operator=(SearchHandle&& other) noexcept {
    if (this != &other) {
        stop();
        wait();
        state_ = std::move(other.state_);
    }
    return *this;
}

SearchHandle::~SearchHandle() {
    stop();
    wait();
}

void SearchHandle::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

void SearchHandle::wait() {
    if (state_) {
        state_->wait();
    }
}

bool SearchHandle::running() const noexcept {
    return state_ && state_->running();
}

void SearchHandle::stop_after(std::chrono::milliseconds delay) {
    if (state_ != nullptr) {
        state_->request_stop_after(delay);
    }
}

void SearchHandle::request_ponderhit(SearchLimits limits) {
    if (state_) {
        state_->request_ponderhit(std::move(limits));
    }
}

SearchService::SearchService(std::shared_ptr<const Evaluator> evaluator,
                             HashMemoryPolicy hash_memory_policy,
                             std::size_t initial_hash_mb) {
    if (!evaluator) {
        throw std::invalid_argument("SearchService requires an evaluator");
    }
    impl_ = std::make_shared<Impl>(std::move(evaluator), std::move(hash_memory_policy),
                                   initial_hash_mb);
}

SearchHandle SearchService::start(GameState root, SearchLimits limits, SearchEventSink sink,
                                  SearchOptions options) {
    // The default SearchOptions hash value means "use the service configuration".
    // A non-default value explicitly reconfigures the shared service table.
    if (options.hash_mb != 0) {
        (void)impl_->table->set_size_mb(options.hash_mb);
    }

    options.threads = detail::normalized_threads(options.threads);
    options.speed_percent = detail::normalized_speed(options.speed_percent);
    options.multi_pv = detail::normalized_multi_pv(options.multi_pv);
    options.move_overhead_ms = detail::normalized_move_overhead(options.move_overhead_ms);
    options.slow_mover_percent = detail::normalized_slow_mover(options.slow_mover_percent);
    options.elo = detail::normalized_elo(options.elo);
    if (options.limit_strength && options.strength_profile_hook) {
        options.strength_profile_hook(options);
    }

    std::shared_ptr<const Evaluator> evaluator;
    {
        std::lock_guard lock(impl_->config_mutex);
        evaluator = impl_->evaluator;
    }

    auto session = std::make_shared<detail::SearchSession>(
        std::move(root), std::move(limits), options);
    detail::SearchRunner runner(session, std::move(evaluator), impl_->table,
                                impl_->evaluator_mutex, std::move(sink));
    session->launch([runner = std::move(runner)]() mutable { runner.run(); });
    return SearchHandle(std::move(session));
}

void SearchService::set_evaluator(std::shared_ptr<const Evaluator> evaluator) {
    if (!evaluator) {
        throw std::invalid_argument("SearchService requires a non-null evaluator");
    }
    std::lock_guard lock(impl_->config_mutex);
    impl_->evaluator = std::move(evaluator);
}

HashResizeResult SearchService::set_hash_size_mb(std::size_t megabytes) {
    return impl_->table->set_size_mb(megabytes);
}

std::size_t SearchService::hash_size_mb() const noexcept {
    return impl_->table->size_mb();
}

std::size_t SearchService::hashfull_permill() const noexcept {
    return impl_->table->hashfull_permill();
}

void SearchService::clear_hash() noexcept {
    impl_->table->clear();
}

} // namespace koi
