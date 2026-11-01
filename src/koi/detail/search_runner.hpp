#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "koi/search_types.hpp"

namespace koi {

class Evaluator;
class TranspositionTable;

namespace detail {

class SearchSession;

// Owns the per-search flow that used to live inside the SearchService::start
// worker lambda: timing setup, tablebase probing, the serial and root-parallel
// iterative-deepening drivers, bounded fallbacks, and result publication.
// The runner holds exactly the dependencies the lambda captured; the session
// and table are shared so the worker keeps both alive for the search.
class SearchRunner {
public:
    SearchRunner(std::shared_ptr<SearchSession> session,
                 std::shared_ptr<const Evaluator> evaluator,
                 std::shared_ptr<TranspositionTable> table,
                 std::shared_ptr<std::mutex> evaluator_mutex,
                 SearchEventSink sink);

    void run();

private:
    void run_impl();
    std::shared_ptr<SearchSession> session_;
    std::shared_ptr<const Evaluator> evaluator_;
    std::shared_ptr<TranspositionTable> table_;
    std::shared_ptr<std::mutex> evaluator_mutex_;
    SearchEventSink sink_;
};

} // namespace detail

} // namespace koi
