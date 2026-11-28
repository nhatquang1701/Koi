#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "koi/time_manager.hpp"

#include "koi_test_support.hpp"

namespace {

using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;
using koi::ClockLimit;
using koi::RootTimingContext;
using koi::SearchIterationObservation;
using koi::SearchLimits;
using koi::TimeManagementStats;
using koi::TimeManager;

constexpr std::uint32_t kMoveOverheadMs = 30;

using koi::test::require;

SearchLimits clock_limits(milliseconds remaining, milliseconds increment) {
    SearchLimits limits;
    limits.white_clock = ClockLimit{remaining, increment};
    limits.black_clock = ClockLimit{remaining, increment};
    return limits;
}

// A warm TT yields a low initial hardness (letting the emergency reduction
// apply); the default context is cold and therefore always a "hard" position.
RootTimingContext warm_context() {
    RootTimingContext context;
    context.table_available = true;
    context.tt_hit = true;
    context.tt_exact = true;
    context.tt_has_best_move = true;
    context.tt_depth = 10;
    context.tt_generation_age = 0;
    return context;
}

milliseconds expected_hard_safety(milliseconds remaining) {
    const milliseconds overhead{kMoveOverheadMs};
    const auto doubled_overhead = overhead + overhead + milliseconds{15};
    const auto fraction = std::min(milliseconds{50}, milliseconds{remaining.count() / 20});
    return std::max(doubled_overhead, fraction);
}

milliseconds expected_fraction_ceiling(milliseconds usable) {
    return std::max(milliseconds{5}, milliseconds{usable.count() / 4});
}

milliseconds nonnegative_difference(milliseconds value, milliseconds amount) {
    return value > amount ? value - amount : milliseconds{0};
}

TimeManager make_clock_manager(milliseconds remaining, milliseconds increment,
                               std::optional<std::uint32_t> moves_to_go, bool warm) {
    SearchLimits limits = clock_limits(remaining, increment);
    limits.moves_to_go = moves_to_go;
    return TimeManager(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                       warm ? warm_context() : RootTimingContext{});
}

// The headline guarantee: whatever the clock, the increment, the horizon or the
// detected position hardness, the enforced deadline leaves the safety margin and
// never spends more than a quarter of the usable clock on a single move.
void test_clock_budget_never_breaks_the_safety_invariants() {
    const milliseconds overage{kMoveOverheadMs};
    (void)overage;

    const milliseconds remainings[] = {milliseconds{600},  milliseconds{1500}, milliseconds{2500},
                                       milliseconds{6000}, milliseconds{15000}, milliseconds{60000},
                                       milliseconds{600000}};
    const milliseconds increments[] = {milliseconds{0}, milliseconds{1000}, milliseconds{5000},
                                       milliseconds{35000}};
    const std::optional<std::uint32_t> horizons[] = {std::nullopt, 20, 40};
    const bool hardness[] = {false, true};

    for (const milliseconds remaining : remainings) {
        for (const milliseconds increment : increments) {
            for (const std::optional<std::uint32_t> moves_to_go : horizons) {
                for (const bool warm : hardness) {
                    TimeManager manager =
                        make_clock_manager(remaining, increment, moves_to_go, warm);
                    const auto budget = manager.time_budget();
                    require(budget.has_value(), "a clock search must always allocate a budget");

                    const TimeManagementStats stats = manager.diagnostics();
                    const auto safety = expected_hard_safety(remaining);
                    const auto ceiling = std::max(milliseconds{1}, nonnegative_difference(stats.usable, safety));

                    require(stats.usable <= remaining, "usable time must not exceed the clock");
                    require(*budget <= ceiling,
                            "the enforced deadline must leave the safety margin on the clock");
                    require(*budget <= expected_fraction_ceiling(stats.usable),
                            "a single move must not exceed a quarter of the usable clock");
                    require(*budget < remaining, "the deadline must be smaller than the clock");
                    require(*budget >= milliseconds{1}, "the deadline must never be zero");
                    require(stats.hard_budget == *budget,
                            "diagnostics must report the enforced deadline");
                    require(stats.soft_budget <= stats.hard_budget,
                            "the soft budget must not exceed the hard budget");
                    require(stats.reserve <= remaining, "the reserve must not exceed the clock");
                    if (stats.usable > milliseconds{0}) {
                        require(*budget <= stats.usable,
                                "the deadline must fit inside the usable clock");
                    }
                    if (remaining >= milliseconds{2000}) {
                        require(stats.reserve.count() >= 2 * kMoveOverheadMs,
                                "the reserve must cover at least two move overheads");
                    }
                }
            }
        }
    }
}

// Regression guard for the 6+1 endgame that lost on time: with 1.5 s left and a
// 1 s increment the old arithmetic allocated ~1.45 s in a single move.
void test_fast_control_move_leaves_most_of_the_clock() {
    TimeManager manager = make_clock_manager(milliseconds{1500}, milliseconds{1000},
                                             std::nullopt, false);
    const auto budget = manager.time_budget();
    require(budget.has_value(), "a clock search must allocate a budget");
    require(*budget <= milliseconds{400},
            "a 6+1 move must not consume the whole remaining clock");
    require(*budget * 3 <= milliseconds{1500},
            "at least two thirds of the clock must survive a single move");
}

// Regression guard for the 5+0 blitz game that lost on time: with no increment
// and no `movestogo` the old horizon (20) allocated ~15 s per move and let an
// unstable position spend up to 45 s, so the clock was gone by move 22.
void test_sudden_death_paces_the_whole_clock() {
    const milliseconds control{300000};
    TimeManager manager = make_clock_manager(control, milliseconds{0}, std::nullopt, false);
    const TimeManagementStats stats = manager.diagnostics();
    require(manager.time_budget().has_value(), "a sudden-death clock must allocate a budget");

    // 5+0: aiming to keep playing for 40+ moves means single-digit seconds.
    require(stats.soft_budget <= milliseconds{8000},
            "a 5+0 soft budget must stay under eight seconds");
    require(stats.hard_budget <= milliseconds{16000},
            "a 5+0 hard budget must stay under sixteen seconds");
    require(stats.horizon >= 40,
            "sudden death must plan over a longer horizon than twenty moves");
    require(stats.hard_budget * 20 <= control,
            "twenty hard-budget moves must still fit inside the whole clock");

    // 1+0 must be proportionally tighter, not just scaled by the same budget.
    TimeManager bullet = make_clock_manager(milliseconds{60000}, milliseconds{0},
                                            std::nullopt, false);
    require(bullet.diagnostics().hard_budget <= milliseconds{4000},
            "a 1+0 hard budget must stay under four seconds");
    require(bullet.time_budget().value() < milliseconds{60000},
            "a 1+0 budget must leave time on the clock");

    // An increment refunds part of every move, so the pacing may stay shorter.
    TimeManager incremental = make_clock_manager(milliseconds{300000}, milliseconds{2000},
                                                 std::nullopt, false);
    require(incremental.diagnostics().horizon <= 30,
            "an increment control must not use the sudden-death horizon");
    require(incremental.diagnostics().soft_budget >= milliseconds{2000},
            "an increment control must still allow real think time");
}

void test_increment_hard_budget_stays_near_two_soft_budgets() {
    // 10+2: an old 3-base-unit hard allocation allowed ~74 s in one move when
    // the soft budget was ~25 s. Online clocks cannot absorb two such moves.
    TimeManager manager = make_clock_manager(milliseconds{600000}, milliseconds{2000},
                                             std::nullopt, true);
    const TimeManagementStats stats = manager.diagnostics();
    require(stats.hard_budget <= stats.soft_budget * 2,
            "an increment hard budget must stay within two soft budgets");
    require(stats.hard_budget <= milliseconds{60000},
            "a 10+2 hard budget must stay well under a minute");
}
void test_low_clock_engages_emergency_pacing() {
    TimeManager emergency = make_clock_manager(milliseconds{2000}, milliseconds{100},
                                               std::nullopt, true);
    require(emergency.diagnostics().emergency_pacing,
            "a clock of two seconds must engage emergency pacing");

    TimeManager normal = make_clock_manager(milliseconds{6000}, milliseconds{100},
                                            std::nullopt, true);
    require(!normal.diagnostics().emergency_pacing,
            "a healthy clock must not engage emergency pacing");
}

void test_movetime_keeps_its_own_budget() {
    SearchLimits limits;
    limits.movetime = milliseconds{1000};
    TimeManager manager(limits, koi::Color::white, 100, kMoveOverheadMs, 100);
    const auto budget = manager.time_budget();
    require(budget.has_value(), "a movetime search must allocate a budget");
    require(*budget <= milliseconds{1000} - milliseconds{kMoveOverheadMs},
            "a movetime budget must pay the move overhead");
    require(*budget <= milliseconds{920},
            "a one second movetime budget must reserve teardown and GUI pipe margin");
    require(*budget >= milliseconds{800},
            "a one second movetime budget must stay close to one second");
    require(manager.diagnostics().hard_budget == *budget,
            "movetime diagnostics must report the enforced deadline");

    SearchLimits short_limits;
    short_limits.movetime = milliseconds{100};
    const auto backdated_start = std::chrono::steady_clock::now() - milliseconds{120};
    TimeManager backdated(short_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                          koi::RootTimingContext{}, {}, false, backdated_start);
    require(backdated.should_stop(0),
            "pre-search latency reflected in search_started must count toward movetime");
}

void test_deadline_is_fixed_at_construction_and_cannot_be_extended() {
    auto current = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits limits = clock_limits(milliseconds{60000}, milliseconds{0});
    TimeManager manager(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                        RootTimingContext{}, [current]() { return *current; });

    const auto budget = manager.time_budget();
    require(budget.has_value(), "a clock search must allocate a budget");
    const auto allocated = *budget;

    require(!manager.should_stop(0), "the search must not stop before the deadline");
    *current += allocated - milliseconds{1};
    require(!manager.should_stop(0), "the search must not stop before the allocated time");
    *current += milliseconds{1};
    require(manager.should_stop(0), "the search must stop once the allocated time is spent");
    require(manager.diagnostics().hard_deadline_reached,
            "the deadline flag must be visible in diagnostics");

    // Hard evidence drives diagnostics and iteration pacing only; it must never
    // buy additional wall-clock time beyond the frozen deadline.
    auto second_clock = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager second(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                       RootTimingContext{}, [second_clock]() { return *second_clock; });
    const auto second_budget = second.time_budget();
    require(second_budget.has_value(), "a clock search must allocate a budget");

    SearchIterationObservation observation;
    observation.depth = 12;
    observation.score_cp = 400;
    observation.best_move_changed = true;
    observation.pv_changed = true;
    observation.aspiration_researched = true;
    second.observe_iteration(observation);

    require(second.time_budget() == second_budget,
            "an iteration observation must not move the enforced deadline");
    require(second.diagnostics().hard_budget == *second_budget,
            "diagnostics must keep reporting the enforced deadline");
    require(second.diagnostics().observed_hardness >= 100,
            "hard evidence must still be recorded for diagnostics");
    require(second.diagnostics().extended_for_hard_position,
            "hard evidence must be recorded as an extended hard position");

    *second_clock += *second_budget - milliseconds{20};
    require(second.should_start_next_iteration(milliseconds{1}),
            "an iteration that still fits must be allowed to start");
    require(!second.should_start_next_iteration(milliseconds{50}),
            "an iteration that cannot finish must not start");
    *second_clock += milliseconds{20};
    require(second.should_stop_after_iteration(),
            "the search must stop after the iteration that reaches the deadline");
}

// Worst case simulation: every move spends the entire enforced deadline and only
// the increment comes back. The safety margin must hold on every single move and
// an incremental control must retain a healthy clock. A control without an
// increment necessarily drains towards the reserve over a long game - that is
// physics, not a flag - but the engine must still never allocate more than it
// has and must never reach zero.
void test_synthetic_games_never_reach_the_flag() {
    struct Control {
        const char* name;
        std::int64_t remaining_ms;
        std::int64_t increment_ms;
    };
    const Control controls[] = {
        {"6+1", 6000, 1000},  {"15+0", 15000, 0}, {"3+2", 3000, 2000},
        {"1+0", 1000, 0},     {"60+0", 60000, 0}, {"2.5+1", 2500, 1000},
    };

    for (const Control& control : controls) {
        milliseconds remaining{control.remaining_ms};
        const milliseconds increment{control.increment_ms};
        for (int move = 0; move < 80; ++move) {
            TimeManager manager = make_clock_manager(remaining, increment, std::nullopt, false);
            const auto budget = manager.time_budget();
            require(budget.has_value(), "a simulated move must allocate a budget");

            const TimeManagementStats stats = manager.diagnostics();
            const auto safety = expected_hard_safety(remaining);
            const auto ceiling =
                std::max(milliseconds{1}, nonnegative_difference(stats.usable, safety));

            require(*budget <= ceiling,
                    "a simulated move must leave the safety margin on the clock");
            require(*budget >= milliseconds{1}, "a simulated move must allocate some time");
            if (remaining > milliseconds{2}) {
                require(*budget < remaining,
                        "a simulated move must never spend the whole clock");
            }
            remaining = remaining - *budget + increment;
        }
        require(remaining > milliseconds{0}, "the simulated clock must never reach zero");
        if (increment >= milliseconds{500}) {
            require(remaining >= milliseconds{100},
                    "an incremental control must retain a playable clock");
        }
    }
}

void test_dynamic_move_overhead_tracks_the_time_control() {
    const auto overhead_for = [](milliseconds remaining, bool dynamic) {
        SearchLimits limits = clock_limits(remaining, milliseconds{0});
        TimeManager manager(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                            RootTimingContext{}, {}, dynamic);
        return manager.diagnostics().overhead;
    };
    require(overhead_for(milliseconds{1000}, true) == milliseconds{20},
            "dynamic overhead on a 1+0 clock must be 2% of the remaining clock");
    require(overhead_for(milliseconds{300000}, true) == milliseconds{100},
            "dynamic overhead must cap at 100 ms on a long clock");
    require(overhead_for(milliseconds{100}, true) == milliseconds{10},
            "dynamic overhead must floor at 10 ms");
    require(overhead_for(milliseconds{1000}, false) == milliseconds{kMoveOverheadMs},
            "a fixed overhead must keep the configured Move Overhead value");
}
void test_soft_budget_stops_a_stable_quiet_position() {
    auto fake_now = std::make_shared<Clock::time_point>(Clock::now());
    const SearchLimits limits = clock_limits(milliseconds{300'000}, milliseconds{0});
    TimeManager manager(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                        warm_context(), [fake_now] { return *fake_now; });
    require(!manager.should_stop_after_iteration(),
            "a fresh quiet search must not stop before the soft budget");
    const auto soft = manager.diagnostics().soft_budget;
    require(soft > milliseconds{0}, "a 5+0 clock must produce a positive soft budget");
    for (int iteration = 0; iteration < 3; ++iteration) {
        SearchIterationObservation observation;
        observation.depth = 6 + iteration;
        observation.score_cp = 20;
        manager.observe_iteration(observation);
    }
    *fake_now += soft + milliseconds{1};
    require(manager.should_stop_after_iteration(),
            "a stable quiet position must stop once the soft budget is reached");
}

void test_dynamic_optimum_budget_scales_with_effort_and_falling_eval() {
    const SearchLimits limits = clock_limits(milliseconds{300'000}, milliseconds{0});
    TimeManager calm(limits, koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    TimeManager contested(limits, koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    TimeManager falling(limits, koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    TimeManager rising(limits, koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());

    for (int depth = 5; depth <= 8; ++depth) {
        SearchIterationObservation calm_obs;
        calm_obs.depth = depth;
        calm_obs.score_cp = 25;
        calm_obs.nodes = 10000;
        calm_obs.best_move_nodes = 9200;
        calm.observe_iteration(calm_obs);

        SearchIterationObservation contested_obs;
        contested_obs.depth = depth;
        contested_obs.score_cp = 25;
        contested_obs.best_move_changed = (depth == 8);
        contested_obs.nodes = 10000;
        contested_obs.best_move_nodes = 2200;
        contested.observe_iteration(contested_obs);

        SearchIterationObservation falling_obs;
        falling_obs.depth = depth;
        falling_obs.score_cp = depth == 8 ? -90 : 30;
        falling_obs.nodes = 10000;
        falling_obs.best_move_nodes = 5000;
        falling.observe_iteration(falling_obs);

        SearchIterationObservation rising_obs;
        rising_obs.depth = depth;
        rising_obs.score_cp = depth == 8 ? 150 : 30;
        rising_obs.nodes = 10000;
        rising_obs.best_move_nodes = 5000;
        rising.observe_iteration(rising_obs);
    }

    require(calm.diagnostics().optimum_budget < calm.diagnostics().soft_budget,
            "high best-move node concentration and stability must reduce the optimum budget");
    require(contested.diagnostics().optimum_budget > calm.diagnostics().optimum_budget,
            "low best-move node effort and a late move change must allocate more time than a calm move");
    require(falling.diagnostics().optimum_budget > rising.diagnostics().optimum_budget,
            "a falling evaluation must allocate more defensive time than an equal score gain");
}

void test_single_legal_move_paces_fast_in_clock_mode() {
    auto fake_now = std::make_shared<Clock::time_point>(Clock::now());
    const SearchLimits limits = clock_limits(milliseconds{60'000}, milliseconds{0});
    RootTimingContext single_reply = warm_context();
    single_reply.legal_move_count = 1;
    TimeManager manager(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                        single_reply, [fake_now] { return *fake_now; });
    const TimeManagementStats stats = manager.diagnostics();
    require(stats.optimum_budget <= stats.soft_budget / 4,
            "a forced single-reply position must scale down the optimum budget");
    SearchIterationObservation obs;
    obs.depth = 4;
    obs.score_cp = 0;
    manager.observe_iteration(obs);
    *fake_now += stats.optimum_budget + milliseconds{1};
    require(manager.should_stop_after_iteration(),
            "a forced single-reply position must stop once the reduced optimum budget is reached");
}

void test_should_start_next_iteration_avoids_overflow_and_reconfigure_races() {
    auto fake_now = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits extreme_limits;
    extreme_limits.movetime = milliseconds::max();
    TimeManager extreme(extreme_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                        RootTimingContext{}, [fake_now] { return *fake_now; });
    *fake_now += milliseconds{10};
    require(extreme.should_start_next_iteration(milliseconds{50}),
            "near-max movetime budget with small elapsed time must allow next iteration");
    require(!extreme.should_start_next_iteration(milliseconds::max()),
            "extreme estimated next iteration duration must saturate without signed overflow");

    SearchLimits initial_movetime;
    initial_movetime.movetime = milliseconds{60'000};
    TimeManager concurrent(initial_movetime, koi::Color::white, 100, kMoveOverheadMs, 100);
    std::atomic_bool stop_readers{false};
    std::atomic_bool saw_missing_budget{false};
    std::atomic_bool saw_unexpected_stop{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 2; ++i) {
        readers.emplace_back([&] {
            while (!stop_readers.load(std::memory_order_acquire)) {
                if (!concurrent.time_budget().has_value()) {
                    saw_missing_budget.store(true, std::memory_order_relaxed);
                }
                if (concurrent.should_stop(1)) {
                    saw_unexpected_stop.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int step = 0; step < 500; ++step) {
        SearchLimits next;
        next.movetime = milliseconds{40'000 + (step % 2) * 10'000};
        concurrent.reconfigure(next, koi::Color::white, 100, kMoveOverheadMs, 100,
                               RootTimingContext{});
    }
    stop_readers.store(true, std::memory_order_release);
    for (std::thread& reader : readers) {
        reader.join();
    }
    require(!saw_missing_budget.load() && !saw_unexpected_stop.load(),
            "concurrent reconfigure between bounded limits must never expose a transient -1 budget");

    SearchLimits ponder_limits;
    ponder_limits.ponder = true;
    ponder_limits.white_clock = ClockLimit{milliseconds{60'000}, milliseconds{0}};
    ponder_limits.black_clock = ClockLimit{milliseconds{60'000}, milliseconds{0}};
    TimeManager ponder_mgr(ponder_limits, koi::Color::white, 100, kMoveOverheadMs, 100);
    stop_readers.store(false, std::memory_order_relaxed);
    saw_unexpected_stop.store(false, std::memory_order_relaxed);
    readers.clear();
    for (int i = 0; i < 2; ++i) {
        readers.emplace_back([&] {
            while (!stop_readers.load(std::memory_order_acquire)) {
                if (ponder_mgr.should_stop(1)) {
                    saw_unexpected_stop.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int step = 0; step < 300; ++step) {
        SearchLimits converted = ponder_limits;
        converted.ponder = (step % 2) != 0;
        ponder_mgr.reconfigure(converted, koi::Color::white, 100, kMoveOverheadMs, 100,
                               RootTimingContext{});
    }
    stop_readers.store(true, std::memory_order_release);
    for (std::thread& reader : readers) {
        reader.join();
    }
    require(!saw_unexpected_stop.load(),
            "ponderhit reconfigure must publish valid start timestamps and budgets before clearing unbounded_");
}

void test_dynamic_overhead_respects_floors_and_reserve_unbounded() {
    SearchLimits limits = clock_limits(milliseconds{10'000}, milliseconds{1'000});

    // Dynamic Overhead must never undercut learned online GUI/network latency.
    TimeManager with_observed(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                              RootTimingContext{}, {}, true, {}, 250);
    const TimeManagementStats observed_stats = with_observed.diagnostics();
    require(observed_stats.overhead == milliseconds{250},
            "dynamic overhead must preserve observed GUI/network latency");
    require(observed_stats.reserve == milliseconds{500},
            "reserve must cover two observed overheads");

    // Explicit Move Overhead above the 30 ms default also forms a floor under dynamic overhead.
    TimeManager with_explicit_floor(limits, koi::Color::white, 100, 120, 100,
                                    RootTimingContext{}, {}, true, {}, 0);
    require(with_explicit_floor.diagnostics().overhead == milliseconds{120},
            "dynamic overhead must preserve explicit Move Overhead above default");

    // Reserve must not clip 2 * overhead at 750 ms on high-latency links.
    SearchLimits long_clock = clock_limits(milliseconds{60'000}, milliseconds{0});
    TimeManager high_latency(long_clock, koi::Color::white, 100, 30, 100,
                             RootTimingContext{}, {}, true, {}, 900);
    const TimeManagementStats high_stats = high_latency.diagnostics();
    require(high_stats.overhead == milliseconds{900},
            "high observed latency up to 1500 ms must be preserved");
    require(high_stats.reserve == milliseconds{1'800},
            "reserve must protect two full overheads above the 750 ms proportional cap");
}

void test_tmbpusok_opening_and_winning_conversion_pacing() {
    // Opening move 8 on 180+1 must pace conservatively instead of burning 10-17 s.
    RootTimingContext opening_ctx;
    opening_ctx.legal_move_count = 32;
    opening_ctx.game_phase = 16;
    opening_ctx.fullmove_number = 8;
    TimeManager opening(clock_limits(milliseconds{170'000}, milliseconds{1'000}),
                        koi::Color::white, 100, 100, 100, opening_ctx, {}, true, {}, 0);
    const TimeManagementStats opening_stats = opening.diagnostics();
    require(opening_stats.horizon == 36,
            "incremental opening must plan over an extended 36-move horizon");
    require(opening_stats.soft_budget <= milliseconds{6'500} &&
                opening_stats.hard_budget <= milliseconds{12'500},
            "180+1 opening move must avoid burning double-digit seconds");

    // Winning +7.00 endgame on 10s+1s must convert quickly so clock grows instead of flagging.
    RootTimingContext endgame_ctx;
    endgame_ctx.table_available = true;
    endgame_ctx.tt_hit = true;
    endgame_ctx.tt_exact = true;
    endgame_ctx.tt_has_best_move = true;
    endgame_ctx.tt_depth = 12;
    endgame_ctx.legal_move_count = 14;
    endgame_ctx.game_phase = 8;
    endgame_ctx.fullmove_number = 48;
    TimeManager winning(clock_limits(milliseconds{10'000}, milliseconds{1'000}),
                        koi::Color::white, 100, 100, 100, endgame_ctx, {}, true, {}, 100);
    for (int depth = 5; depth <= 9; ++depth) {
        SearchIterationObservation obs;
        obs.depth = depth;
        obs.score_cp = 700;
        obs.nodes = 10'000;
        obs.best_move_nodes = 8'500;
        winning.observe_iteration(obs);
    }
    require(winning.diagnostics().optimum_budget < milliseconds{800},
            "stable winning conversion on 10s+1s must spend less than net increment");
}

void test_forced_mate_lookahead_and_instability_signals() {
    auto now = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});

    // Forced mate confirmed twice at depth >= 4 exits rapidly.
    TimeManager mate_mgr(limits, koi::Color::white, 100, 30, 100,
                         RootTimingContext{}, [now] { return *now; });
    for (int depth = 4; depth <= 5; ++depth) {
        SearchIterationObservation mate_obs;
        mate_obs.depth = depth;
        mate_obs.score_cp = 99'500;
        mate_obs.best_move_changed = false;
        mate_mgr.observe_iteration(mate_obs);
    }
    const TimeManagementStats mate_stats = mate_mgr.diagnostics();
    require(mate_stats.optimum_budget <= mate_stats.soft_budget / 6 ||
                mate_stats.optimum_budget == milliseconds{20},
            "confirmed forced mate must collapse optimum_budget");
    *now += mate_stats.optimum_budget + milliseconds{1};
    require(mate_mgr.should_stop_after_iteration(),
            "confirmed forced mate must stop promptly once collapsed optimum_budget is reached");

    // Cumulative best-move changes and inter-move score drop expand optimum_budget.
    RootTimingContext calm_ctx;
    calm_ctx.table_available = true;
    calm_ctx.tt_hit = true;
    calm_ctx.tt_exact = true;
    calm_ctx.tt_has_best_move = true;
    calm_ctx.tt_depth = 10;
    calm_ctx.previous_search_score = 100;
    RootTimingContext drop_ctx = calm_ctx;
    drop_ctx.previous_search_score = 320;
    TimeManager calm(limits, koi::Color::white, 100, 30, 100, calm_ctx);
    TimeManager dropped(limits, koi::Color::white, 100, 30, 100, drop_ctx);
    for (int depth = 4; depth <= 6; ++depth) {
        SearchIterationObservation c_obs{depth, 100, false, false, false, 10'000, 6'000, milliseconds{50}};
        SearchIterationObservation d_obs{depth, 100, depth == 5, false, false, 10'000, 6'000, milliseconds{50}};
        calm.observe_iteration(c_obs);
        dropped.observe_iteration(d_obs);
    }
    require(dropped.diagnostics().optimum_budget > calm.diagnostics().optimum_budget,
            "cumulative best-move changes and inter-move score drops must increase optimum_budget");

    // Pre-iteration lookahead declines starting a doomed next iteration.
    auto look_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager look(limits, koi::Color::white, 100, 30, 100, calm_ctx,
                     [look_now] { return *look_now; });
    const auto soft = look.diagnostics().soft_budget;
    for (int depth = 4; depth <= 6; ++depth) {
        SearchIterationObservation obs;
        obs.depth = depth;
        obs.score_cp = 100;
        obs.nodes = 10'000;
        obs.best_move_nodes = 5'000;
        obs.iteration_elapsed = soft;
        look.observe_iteration(obs);
    }
    *look_now += (look.diagnostics().optimum_budget * 6) / 10;
    require(!look.should_start_next_iteration(milliseconds{1}),
            "pre-iteration lookahead must decline doomed iterations that would overrun optimum_budget");

    // Backdated reconfigure preserves elapsed ponderhit time.
    TimeManager ponder_reconf(limits, koi::Color::white, 100, 30, 100);
    const auto backdated = Clock::now() - milliseconds{50'000};
    ponder_reconf.reconfigure(limits, koi::Color::white, 100, 30, 100,
                              RootTimingContext{}, false, backdated, 0);
    require(ponder_reconf.should_stop(0),
            "reconfigure must measure elapsed time from the ponderhit arrival timestamp");
}

void test_ponderhit_preserves_ponder_observations_and_mate_collapse() {
    auto fake_now = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits ponder_limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    ponder_limits.ponder = true;

    TimeManager mate_ponder(ponder_limits, koi::Color::white, 100, 30, 100,
                            RootTimingContext{}, [fake_now] { return *fake_now; });
    for (int depth = 4; depth <= 5; ++depth) {
        SearchIterationObservation obs;
        obs.depth = depth;
        obs.score_cp = 99'500;
        obs.best_move_changed = false;
        mate_ponder.observe_iteration(obs);
    }
    SearchLimits converted = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    mate_ponder.reconfigure(converted, koi::Color::white, 100, 30, 100,
                            RootTimingContext{}, false, {}, 0);
    const TimeManagementStats mate_stats = mate_ponder.diagnostics();
    require(mate_stats.optimum_budget <= mate_stats.soft_budget / 6 ||
                mate_stats.optimum_budget == milliseconds{20},
            "ponderhit must retain confirmed forced-mate observations for immediate collapse");
    *fake_now += mate_stats.optimum_budget + milliseconds{1};
    require(mate_ponder.should_stop_after_iteration(),
            "ponderhit with confirmed forced mate must stop promptly at optimum_budget");

    TimeManager calm_ponder(ponder_limits, koi::Color::white, 100, 30, 100,
                            warm_context());
    for (int depth = 5; depth <= 8; ++depth) {
        SearchIterationObservation obs;
        obs.depth = depth;
        obs.score_cp = 30;
        obs.nodes = 10'000;
        obs.best_move_nodes = 9'000;
        calm_ponder.observe_iteration(obs);
    }
    calm_ponder.reconfigure(converted, koi::Color::white, 100, 30, 100,
                            warm_context(), false, {}, 0);
    TimeManager fresh_clock(converted, koi::Color::white, 100, 30, 100, warm_context());
    require(calm_ponder.diagnostics().optimum_budget < fresh_clock.diagnostics().optimum_budget,
            "ponderhit must carry forward calm ponder effort/stability to trim optimum_budget");

    // Non-ponder reconfigure resets prior search observations cleanly.
    fresh_clock.reconfigure(converted, koi::Color::white, 100, 30, 100,
                            warm_context(), false, {}, 0);
    require(fresh_clock.diagnostics().optimum_budget == fresh_clock.diagnostics().soft_budget,
            "non-ponder reconfigure must reset search observations cleanly");
}

void test_continuous_horizon_and_opponent_clock_asymmetry() {
    const auto horizon_at = [](int phase, std::uint16_t fullmove, milliseconds inc) {
        RootTimingContext ctx = warm_context();
        ctx.game_phase = phase;
        ctx.fullmove_number = fullmove;
        return TimeManager(clock_limits(milliseconds{180'000}, inc),
                           koi::Color::white, 100, 30, 100, ctx)
            .diagnostics()
            .horizon;
    };

    const std::uint32_t h8 = horizon_at(16, 8, milliseconds{1'000});
    const std::uint32_t h11 = horizon_at(16, 11, milliseconds{1'000});
    const std::uint32_t h14 = horizon_at(16, 14, milliseconds{1'000});
    require(h8 == 36U && h8 > h11 && h11 > h14 && h14 == 28U,
            "incremental horizon must interpolate smoothly from move 8 to middlegame");

    const std::uint32_t queenless_mid = horizon_at(6, 18, milliseconds{1'000});
    const std::uint32_t rich_mid = horizon_at(20, 18, milliseconds{1'000});
    require(queenless_mid > rich_mid,
            "low-phase middlegames must plan over a slightly calmer horizon than rich ones");

    // Opponent clock much larger -> more conservative horizon and smaller soft budget.
    RootTimingContext asym_ctx = warm_context();
    asym_ctx.game_phase = 16;
    asym_ctx.fullmove_number = 20;
    SearchLimits behind_sym = clock_limits(milliseconds{15'000}, milliseconds{1'000});
    SearchLimits behind_asym = behind_sym;
    behind_asym.black_clock = ClockLimit{milliseconds{60'000}, milliseconds{1'000}};
    TimeManager tm_behind_sym(behind_sym, koi::Color::white, 100, 30, 100, asym_ctx);
    TimeManager tm_behind_asym(behind_asym, koi::Color::white, 100, 30, 100, asym_ctx);
    require(tm_behind_asym.diagnostics().horizon > tm_behind_sym.diagnostics().horizon &&
                tm_behind_asym.diagnostics().soft_budget < tm_behind_sym.diagnostics().soft_budget,
            "trailing far behind on clock must expand horizon and conserve soft budget");

    // Opponent clock much smaller -> slightly faster horizon and larger soft budget.
    SearchLimits ahead_sym = clock_limits(milliseconds{90'000}, milliseconds{1'000});
    SearchLimits ahead_asym = ahead_sym;
    ahead_asym.black_clock = ClockLimit{milliseconds{20'000}, milliseconds{1'000}};
    TimeManager tm_ahead_sym(ahead_sym, koi::Color::white, 100, 30, 100, asym_ctx);
    TimeManager tm_ahead_asym(ahead_asym, koi::Color::white, 100, 30, 100, asym_ctx);
    require(tm_ahead_asym.diagnostics().horizon < tm_ahead_sym.diagnostics().horizon &&
                tm_ahead_asym.diagnostics().soft_budget > tm_ahead_sym.diagnostics().soft_budget &&
                tm_ahead_asym.time_budget().value() <= expected_fraction_ceiling(tm_ahead_asym.diagnostics().usable),
            "holding a large clock advantage may modestly compress horizon within safety bounds");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"clock budget safety invariants", test_clock_budget_never_breaks_the_safety_invariants},
        {"fast control keeps the clock", test_fast_control_move_leaves_most_of_the_clock},
        {"sudden death pacing", test_sudden_death_paces_the_whole_clock},
        {"emergency pacing", test_low_clock_engages_emergency_pacing},
        {"increment hard budget", test_increment_hard_budget_stays_near_two_soft_budgets},
        {"movetime budget", test_movetime_keeps_its_own_budget},
        {"frozen deadline", test_deadline_is_fixed_at_construction_and_cannot_be_extended},
        {"synthetic games never flag", test_synthetic_games_never_reach_the_flag},
        {"soft budget stops stable quiet positions", test_soft_budget_stops_a_stable_quiet_position},
        {"dynamic move overhead", test_dynamic_move_overhead_tracks_the_time_control},
        {"dynamic optimum budget scaling", test_dynamic_optimum_budget_scales_with_effort_and_falling_eval},
        {"single legal move fast pacing", test_single_legal_move_paces_fast_in_clock_mode},
        {"overflow-safe lookahead and reconfigure ordering",
         test_should_start_next_iteration_avoids_overflow_and_reconfigure_races},
        {"dynamic overhead floors and reserve bounds",
         test_dynamic_overhead_respects_floors_and_reserve_unbounded},
        {"opening and winning endgame pacing",
         test_tmbpusok_opening_and_winning_conversion_pacing},
        {"forced mate, lookahead, and instability signals",
         test_forced_mate_lookahead_and_instability_signals},
        {"ponderhit observation continuity",
         test_ponderhit_preserves_ponder_observations_and_mate_collapse},
        {"continuous horizon and opponent clock asymmetry",
         test_continuous_horizon_and_opponent_clock_asymmetry},
    };
    return koi::test::run_tests(tests, argc, argv);
}
