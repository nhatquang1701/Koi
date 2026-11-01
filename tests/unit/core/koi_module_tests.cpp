import koi;

#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "koi/search_types.hpp"
#include "koi_test_support.hpp"

namespace {

using koi::test::require;

// The optional module layer mirrors native value contracts.  Keep the
// SearchOptions defaults in lock-step at compile time so a native default
// change cannot silently drift away from the module contract.
static_assert(koi::module_api::SearchOptionsContract{}.hash_mb == koi::SearchOptions{}.hash_mb,
              "module SearchOptionsContract.hash_mb default drifted");
static_assert(koi::module_api::SearchOptionsContract{}.threads == koi::SearchOptions{}.threads,
              "module SearchOptionsContract.threads default drifted");
static_assert(koi::module_api::SearchOptionsContract{}.speed_percent ==
                  koi::SearchOptions{}.speed_percent,
              "module SearchOptionsContract.speed_percent default drifted");
static_assert(koi::module_api::SearchOptionsContract{}.multi_pv == koi::SearchOptions{}.multi_pv,
              "module SearchOptionsContract.multi_pv default drifted");
static_assert(koi::module_api::SearchOptionsContract{}.analyse_mode ==
                  koi::SearchOptions{}.analyse_mode,
              "module SearchOptionsContract.analyse_mode default drifted");
static_assert(koi::module_api::SearchOptionsContract{}.show_wdl == koi::SearchOptions{}.show_wdl,
              "module SearchOptionsContract.show_wdl default drifted");
static_assert(koi::module_api::SearchOptionsContract{}.move_overhead_ms ==
                  koi::SearchOptions{}.move_overhead_ms,
              "module SearchOptionsContract.move_overhead_ms default drifted");
static_assert(koi::module_api::SearchOptionsContract{}.slow_mover_percent ==
                  koi::SearchOptions{}.slow_mover_percent,
              "module SearchOptionsContract.slow_mover_percent default drifted");


void test_module_contracts() {
    require(koi::module_api::architecture_version == 1,
            "module architecture version must be stable");
    koi::module_api::PositionContract position{};
    position.zobrist_key = 0x1234;
    position.piece_count = 32;
    require(position.zobrist_key == 0x1234 && position.piece_count == 32,
            "position contract must be value-oriented");

    koi::module_api::SearchSessionContract session{};
    session.thread_count = 1;
    require(!session.cancelled && session.thread_count == 1,
            "search session contract must expose cancellation and thread state");
    koi::module_api::SearchLimitsContract limits{};
    limits.depth = 12;
    limits.node_budget = 50'000;
    require(limits.depth == 12 && limits.node_budget == 50'000,
            "search limits contract must carry deterministic limits");
    koi::module_api::SearchWorkerContextContract worker{};
    worker.worker_id = 2;
    worker.nodes = 128;
    require(worker.worker_id == 2 && worker.nodes == 128,
            "worker context contract must expose local counters");
    koi::module_api::RootCoordinatorContract coordinator{};
    coordinator.root_move_count = 20;
    coordinator.deterministic_tiebreak = true;
    require(coordinator.root_move_count == 20 && coordinator.deterministic_tiebreak,
            "root coordinator contract must expose stable ordering");
    koi::module_api::SearchResultContract result{};
    result.score_cp = 34;
    result.completed_depth = 8;
    require(result.score_cp == 34 && result.completed_depth == 8,
            "search result contract must expose completed iteration data");
    require(koi::module_api::aggregate_boundary_version == 1,
            "aggregate module boundary version must be stable");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"module contracts", test_module_contracts},
    };
    return koi::test::run_tests(tests, argc, argv);
}
