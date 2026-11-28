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

    TimeManager long_single(clock_limits(milliseconds{600'000}, milliseconds{2'000}),
                            koi::Color::white, 100, kMoveOverheadMs, 100, single_reply);
    require(long_single.diagnostics().optimum_budget <= milliseconds{150},
            "single-legal-move optimum budget must cap at 150 ms even on long clocks");

    TimeManager long_mate(clock_limits(milliseconds{600'000}, milliseconds{2'000}),
                          koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    for (int depth = 4; depth <= 5; ++depth) {
        SearchIterationObservation mate_obs;
        mate_obs.depth = depth;
        mate_obs.score_cp = 99'500;
        long_mate.observe_iteration(mate_obs);
    }
    require(long_mate.diagnostics().optimum_budget <= milliseconds{250},
            "twice-confirmed forced mate must cap at 250 ms even on long clocks");
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

void test_asymmetric_conversion_and_high_effort_recapture_cutoff() {
    const SearchLimits limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    const auto make_stable_at = [&](const int score_cp) {
        TimeManager mgr(limits, koi::Color::white, 100, 30, 100, warm_context());
        for (int depth = 5; depth <= 8; ++depth) {
            SearchIterationObservation obs;
            obs.depth = depth;
            obs.score_cp = score_cp;
            obs.nodes = 10'000;
            obs.best_move_nodes = 5'000;
            mgr.observe_iteration(obs);
        }
        return mgr.diagnostics().optimum_budget;
    };

    const auto neutral_opt = make_stable_at(0);
    const auto winning_opt = make_stable_at(700);
    const auto losing_defend_opt = make_stable_at(-550);
    const auto hopeless_opt = make_stable_at(-800);
    require(winning_opt < neutral_opt && hopeless_opt < neutral_opt,
            "clearly winning (+500) and hopeless (-700) positions must trim optimum_budget");
    require(losing_defend_opt == neutral_opt,
            "borderline losing (-550) position must keep full defensive search time");

    auto high_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager high_effort(limits, koi::Color::white, 100, 30, 100, warm_context(),
                            [high_now] { return *high_now; });
    auto mod_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager mod_effort(limits, koi::Color::white, 100, 30, 100, warm_context(),
                           [mod_now] { return *mod_now; });
    for (int depth = 4; depth <= 6; ++depth) {
        SearchIterationObservation high_obs;
        high_obs.depth = depth;
        high_obs.score_cp = 40;
        high_obs.nodes = 10'000;
        high_obs.best_move_nodes = 9'200;
        high_effort.observe_iteration(high_obs);

        SearchIterationObservation mod_obs = high_obs;
        mod_obs.best_move_nodes = 5'000;
        mod_effort.observe_iteration(mod_obs);
    }
    *high_now += (high_effort.diagnostics().optimum_budget * 65) / 100;
    *mod_now += (mod_effort.diagnostics().optimum_budget * 65) / 100;
    require(!high_effort.should_start_next_iteration(milliseconds{1}),
            "high best-move node dominance (>=90%) must stop early past 60% of optimum_budget");
    require(mod_effort.should_start_next_iteration(milliseconds{1}),
            "moderate best-move node share must continue past 65% of optimum_budget");
}

void test_adaptive_branching_factor_lookahead() {
    auto steep_now = std::make_shared<Clock::time_point>(Clock::now());
    auto flat_now = std::make_shared<Clock::time_point>(Clock::now());
    const SearchLimits limits = clock_limits(milliseconds{30'000}, milliseconds{0});
    TimeManager steep(limits, koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                      [steep_now] { return *steep_now; });
    TimeManager flat(limits, koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                     [flat_now] { return *flat_now; });

    const auto opt = std::max(milliseconds{100}, steep.diagnostics().optimum_budget);
    const std::array<milliseconds, 3> steep_times{
        std::max(milliseconds{10}, opt / 18),
        std::max(milliseconds{15}, opt / 6),
        std::max(milliseconds{30}, opt / 3),
    };
    const std::array<milliseconds, 3> flat_times{
        std::max(milliseconds{20}, (opt * 22) / 100),
        std::max(milliseconds{25}, (opt * 27) / 100),
        std::max(milliseconds{30}, opt / 3),
    };
    for (int i = 0; i < 3; ++i) {
        SearchIterationObservation s_obs{4 + i, 30, false, false, false, 10'000, 5'000, steep_times[static_cast<std::size_t>(i)]};
        SearchIterationObservation f_obs{4 + i, 30, false, false, false, 10'000, 5'000, flat_times[static_cast<std::size_t>(i)]};
        steep.observe_iteration(s_obs);
        flat.observe_iteration(f_obs);
    }
    const auto check_elapsed = (opt * 65) / 100;
    *steep_now += check_elapsed;
    *flat_now += check_elapsed;
    require(!steep.should_start_next_iteration(milliseconds{1}),
            "steep branching growth (~2.5x) must skip doomed partial iterations");
    require(flat.should_start_next_iteration(milliseconds{1}),
            "flat branching growth (~1.2x) must continue when projection fits inside 1.2x optimum_budget");
}

void test_high_latency_hard_extension_guard_and_symmetric_effort_stop() {
    auto latency_now = std::make_shared<Clock::time_point>(Clock::now());
    const SearchLimits high_lat_limits = clock_limits(milliseconds{5'000}, milliseconds{0});
    TimeManager high_latency(high_lat_limits, koi::Color::white, 100, 30, 100,
                             RootTimingContext{}, [latency_now] { return *latency_now; },
                             true, {}, 1'200);
    for (int depth = 4; depth <= 5; ++depth) {
        SearchIterationObservation unstable_obs{depth, 10, depth == 4, true, false, 10'000, 5'000, milliseconds{20}};
        high_latency.observe_iteration(unstable_obs);
    }
    *latency_now += high_latency.diagnostics().optimum_budget + milliseconds{1};
    require(high_latency.should_stop_after_iteration(),
            "high-latency clock with <= 5 overheads remaining must decline hard-position extensions");

    auto effort_now = std::make_shared<Clock::time_point>(Clock::now());
    auto mod_now = std::make_shared<Clock::time_point>(Clock::now());
    const SearchLimits normal_limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    TimeManager high_effort(normal_limits, koi::Color::white, 100, 30, 100, warm_context(),
                            [effort_now] { return *effort_now; });
    TimeManager mod_effort(normal_limits, koi::Color::white, 100, 30, 100, warm_context(),
                           [mod_now] { return *mod_now; });
    for (int depth = 4; depth <= 6; ++depth) {
        high_effort.observe_iteration({depth, 30, false, false, false, 10'000, 9'200, milliseconds{40}});
        mod_effort.observe_iteration({depth, 30, false, false, false, 10'000, 5'000, milliseconds{40}});
    }
    *effort_now += (high_effort.diagnostics().optimum_budget * 65) / 100;
    *mod_now += (mod_effort.diagnostics().optimum_budget * 65) / 100;
    require(high_effort.should_stop_after_iteration(),
            "calm high-effort search (>= 90%) must stop symmetrically past 60% of optimum_budget");
    require(!mod_effort.should_stop_after_iteration(),
            "moderate-effort calm search must not trigger the 60% high-effort early stop");
}

void test_ponderhit_discount_scramble_cap_and_doomed_hard_ply_guard() {
    SearchLimits ponder_limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    ponder_limits.ponder = true;
    const SearchLimits clock = clock_limits(milliseconds{120'000}, milliseconds{1'000});

    TimeManager pondered(ponder_limits, koi::Color::white, 100, 30, 100, warm_context());
    TimeManager unpondered(clock, koi::Color::white, 100, 30, 100, warm_context());
    for (int depth = 5; depth <= 8; ++depth) {
        const SearchIterationObservation obs{depth, 25, false, false, false, 10'000, 5'000, milliseconds{40}};
        pondered.observe_iteration(obs);
        unpondered.observe_iteration(obs);
    }
    pondered.reconfigure(clock, koi::Color::white, 100, 30, 100, warm_context(), false, {}, 0);
    require(pondered.diagnostics().optimum_budget < unpondered.diagnostics().optimum_budget,
            "calm deep ponderhit must apply the 20% ponder-time-saved optimum discount");

    // Sudden-death scramble (<= 10s+0, no movestogo) caps optimum_budget scaling at 1.35x soft.
    const SearchLimits scramble_limits = clock_limits(milliseconds{8'000}, milliseconds{0});
    const SearchLimits normal_sd_limits = clock_limits(milliseconds{60'000}, milliseconds{0});
    TimeManager scramble(scramble_limits, koi::Color::white, 100, 30, 100, warm_context());
    TimeManager normal_sd(normal_sd_limits, koi::Color::white, 100, 30, 100, warm_context());
    for (int depth = 4; depth <= 6; ++depth) {
        const int score = depth == 4 ? 120 : -80;
        const SearchIterationObservation obs{depth, score, depth == 5, true, true, 10'000, 2'500, milliseconds{20}};
        scramble.observe_iteration(obs);
        normal_sd.observe_iteration(obs);
    }
    const auto scramble_soft = scramble.diagnostics().soft_budget;
    const auto scramble_opt = scramble.diagnostics().optimum_budget;
    const auto scramble_cap = (scramble_soft * 135) / 100 + milliseconds{1};
    require(scramble_opt <= scramble_cap &&
                scramble_opt * 100 <= scramble_soft * 136 &&
                normal_sd.diagnostics().optimum_budget * 100 >
                    normal_sd.diagnostics().soft_budget * 145,
            "sudden-death <=10s+0 scramble must clamp optimum_budget scale to 1.35x soft while 60s+0 keeps full headroom");

    // Doomed hard-ply guard declines starting a ply projected to overrun hard_budget when not crashing (<60 cp drop).
    auto hard_now = std::make_shared<Clock::time_point>(Clock::now());
    auto crash_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager hard_guard(clock, koi::Color::white, 100, 30, 100, warm_context(),
                           [hard_now] { return *hard_now; });
    TimeManager crash_guard(clock, koi::Color::white, 100, 30, 100, warm_context(),
                            [crash_now] { return *crash_now; });
    const auto hard_budget = hard_guard.diagnostics().hard_budget;
    const auto opt_budget = hard_guard.diagnostics().optimum_budget;
    const auto iter_time = std::max(milliseconds{20}, (hard_budget * 2) / 3);
    for (int depth = 4; depth <= 6; ++depth) {
        hard_guard.observe_iteration(
            {depth, 50, depth == 6, true, false, 10'000, 5'000, iter_time});
        crash_guard.observe_iteration(
            {depth, depth < 6 ? 120 : 20, depth == 6, true, false, 10'000, 5'000, iter_time});
    }
    const auto check_at = std::max(opt_budget, hard_guard.diagnostics().optimum_budget);
    const auto safe_point = std::min(hard_budget - milliseconds{5}, check_at + milliseconds{1});
    *hard_now += safe_point;
    *crash_now += safe_point;
    require(safe_point >= hard_guard.diagnostics().optimum_budget &&
                !hard_guard.should_start_next_iteration(milliseconds{1}),
            "doomed hard-ply guard must skip starting a ply projected to overrun hard_budget");
    require(crash_guard.should_start_next_iteration(milliseconds{1}),
            "severe score drop (>=60 cp) must still allow attempting the emergency ply inside hard_budget");
}

void test_repeating_movestogo_end_of_control_and_sudden_death_ramp() {
    // Repeating time-control end (`movestogo == 1..3`) relaxes the 1/4 usable
    // cap to 3/4, 1/2, 1/3 respectively while still protecting hard_safety.
    const auto mtg_manager = [](std::uint32_t mtg) {
        return make_clock_manager(milliseconds{20'000}, milliseconds{0}, mtg, false);
    };
    const TimeManager mtg1 = mtg_manager(1);
    const TimeManager mtg2 = mtg_manager(2);
    const TimeManager mtg4 = mtg_manager(4);
    const auto usable1 = mtg1.diagnostics().usable;
    const auto safety20 = expected_hard_safety(milliseconds{20'000});
    require(mtg1.time_budget().value() > usable1 / 2 &&
                mtg1.time_budget().value() <= (usable1 * 3) / 4 &&
                mtg1.time_budget().value() <= nonnegative_difference(usable1, safety20),
            "movestogo=1 must unlock up to 3/4 usable while keeping hard safety margin");
    require(mtg2.time_budget().value() > mtg4.time_budget().value() &&
                mtg2.time_budget().value() <= usable1 / 2,
            "movestogo=2 must exceed movestogo=4 and stay capped at 1/2 usable");

    // Sudden-death 3..15s ramp stretches horizon monotonically below 15s,
    // while incremental controls leave the base horizon unchanged.
    RootTimingContext mid_ctx = warm_context();
    mid_ctx.game_phase = 16;
    mid_ctx.fullmove_number = 25;
    TimeManager sd_6s(clock_limits(milliseconds{6'000}, milliseconds{0}),
                      koi::Color::white, 100, kMoveOverheadMs, 100, mid_ctx);
    TimeManager sd_20s(clock_limits(milliseconds{20'000}, milliseconds{0}),
                       koi::Color::white, 100, kMoveOverheadMs, 100, mid_ctx);
    require(sd_6s.diagnostics().horizon > sd_20s.diagnostics().horizon,
            "sudden-death 6s+0 must stretch horizon compared to 20s+0");

    TimeManager inc_6s(clock_limits(milliseconds{6'000}, milliseconds{1'000}),
                       koi::Color::white, 100, kMoveOverheadMs, 100, mid_ctx);
    TimeManager inc_20s(clock_limits(milliseconds{20'000}, milliseconds{1'000}),
                        koi::Color::white, 100, kMoveOverheadMs, 100, mid_ctx);
    require(inc_6s.diagnostics().horizon == inc_20s.diagnostics().horizon,
            "incremental controls must not apply the sudden-death pre-emergency horizon ramp");
}

void test_subsecond_sudden_death_bullet_scramble_protection() {
    auto subsec_now = std::make_shared<Clock::time_point>(Clock::now());
    auto emerg_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager subsec(clock_limits(milliseconds{800}, milliseconds{0}),
                       koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                       [subsec_now] { return *subsec_now; });
    TimeManager emerg(clock_limits(milliseconds{2'500}, milliseconds{0}),
                      koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                      [emerg_now] { return *emerg_now; });

    for (int depth = 4; depth <= 6; ++depth) {
        const int score = depth == 4 ? 120 : -80;
        const SearchIterationObservation obs{
            depth, score, depth == 5, true, true, 10'000, 2'500, milliseconds{5}};
        subsec.observe_iteration(obs);
        emerg.observe_iteration(obs);
    }

    require(subsec.diagnostics().optimum_budget <= subsec.diagnostics().soft_budget,
            "sub-second <=1s+0 sudden-death scramble must clamp optimum_budget at 1.00x soft_budget");
    require(emerg.diagnostics().optimum_budget > emerg.diagnostics().soft_budget,
            "2.5s+0 sudden-death must retain scramble headroom above soft_budget");

    *subsec_now += subsec.diagnostics().optimum_budget + milliseconds{1};
    *emerg_now += emerg.diagnostics().optimum_budget + milliseconds{1};
    require(subsec.should_stop_after_iteration() &&
                !subsec.should_start_next_iteration(milliseconds{1}),
            "sub-second <=1s+0 scramble must decline hard extensions once optimum_budget is reached");
    require(!emerg.should_stop_after_iteration() &&
                emerg.should_start_next_iteration(milliseconds{1}),
            "2.5s+0 sudden-death may continue inside hard_budget on unstable iterations");
}

void test_ponderhit_instant_exit_and_repeating_emergency_horizon() {
    auto single_now = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits ponder_limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    ponder_limits.ponder = true;
    const SearchLimits clock = clock_limits(milliseconds{120'000}, milliseconds{1'000});

    RootTimingContext single_ctx = warm_context();
    single_ctx.legal_move_count = 1;
    TimeManager ponder_single(ponder_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                              single_ctx, [single_now] { return *single_now; });
    ponder_single.observe_iteration({4, 10, false, false, false, 10'000, 10'000, milliseconds{5}});
    ponder_single.reconfigure(clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                              single_ctx, false, {}, 0);
    require(!ponder_single.should_start_next_iteration(milliseconds{1}) &&
                ponder_single.should_stop_after_iteration(),
            "ponderhit with verified depth-4 single legal move must exit immediately at 0 ms");

    auto mate_now = std::make_shared<Clock::time_point>(Clock::now());
    auto unpondered_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager ponder_mate(ponder_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                            warm_context(), [mate_now] { return *mate_now; });
    TimeManager unpondered_mate(clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                                warm_context(), [unpondered_now] { return *unpondered_now; });
    for (int depth = 5; depth <= 6; ++depth) {
        const SearchIterationObservation obs{depth, 99'500, false, false, false, 10'000, 9'000, milliseconds{15}};
        ponder_mate.observe_iteration(obs);
        unpondered_mate.observe_iteration(obs);
    }
    ponder_mate.reconfigure(clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                            warm_context(), false, {}, 0);
    require(!ponder_mate.should_start_next_iteration(milliseconds{1}) &&
                ponder_mate.should_stop_after_iteration(),
            "ponderhit with twice-confirmed depth-6 forced mate must exit immediately at 0 ms");
    require(unpondered_mate.should_start_next_iteration(milliseconds{1}),
            "unpondered search with forced mate must not exit at 0 ms before elapsed reaches optimum");

    const TimeManager mtg1 = make_clock_manager(milliseconds{2'500}, milliseconds{0}, 1, true);
    const auto safety2500 = expected_hard_safety(milliseconds{2'500});
    require(mtg1.diagnostics().emergency_pacing &&
                mtg1.diagnostics().soft_budget > milliseconds{1'000} &&
                mtg1.time_budget().value() <= (mtg1.diagnostics().usable * 3) / 4 &&
                mtg1.time_budget().value() <= nonnegative_difference(mtg1.diagnostics().usable, safety2500),
            "emergency repeating movestogo=1 must not collapse to 10-move sudden-death horizon");

    const TimeManager mtg20 = make_clock_manager(milliseconds{2'500}, milliseconds{0}, 20, true);
    require(mtg20.diagnostics().soft_budget <= mtg20.diagnostics().usable / 20,
            "emergency repeating movestogo=20 must honor movestogo instead of halving it to 10");
}

void test_overwhelming_win_noise_immunity_and_emergency_bullet_lookahead() {
    const SearchLimits clock = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    TimeManager won_dip(clock, koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    TimeManager won_blunder(clock, koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    TimeManager equal_dip(clock, koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    for (int depth = 4; depth <= 6; ++depth) {
        won_dip.observe_iteration(
            {depth, depth == 6 ? 800 : 850, false, false, false, 10'000, 5'000, milliseconds{20}});
        won_blunder.observe_iteration(
            {depth, depth == 6 ? 200 : 850, false, false, false, 10'000, 5'000, milliseconds{20}});
        equal_dip.observe_iteration(
            {depth, depth == 6 ? 100 : 150, false, false, false, 10'000, 5'000, milliseconds{20}});
    }
    require(won_dip.diagnostics().optimum_budget < equal_dip.diagnostics().optimum_budget,
            "small eval noise (+850 -> +800) in an overwhelming win must not inflate optimum_budget");
    require(won_blunder.diagnostics().optimum_budget > won_dip.diagnostics().optimum_budget,
            "dropping below +600 cp (+850 -> +200) must still trigger full defensive extension");

    auto bullet_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager bullet(clock_limits(milliseconds{2'500}, milliseconds{0}),
                       koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                       [bullet_now] { return *bullet_now; });
    const std::array<milliseconds, 3> bullet_times{
        milliseconds{6}, milliseconds{12}, milliseconds{18}};
    for (int i = 0; i < 3; ++i) {
        bullet.observe_iteration(
            {4 + i, 20, i == 2, i == 2, false, 10'000, 5'000, bullet_times[static_cast<std::size_t>(i)]});
    }
    const auto hard_budget = bullet.diagnostics().hard_budget;
    const auto check_at = std::max(bullet.diagnostics().optimum_budget, hard_budget - milliseconds{15});
    *bullet_now += std::min(hard_budget - milliseconds{2}, check_at);
    require(bullet.diagnostics().emergency_pacing &&
                !bullet.should_start_next_iteration(milliseconds{1}),
            "emergency <=3s bullet lookahead must catch <20ms doomed hard plies");
}

void test_high_latency_net_increment_credit_and_symmetric_calm_stop() {
    TimeManager laggy(clock_limits(milliseconds{30'000}, milliseconds{1'000}),
                      koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                      {}, true, {}, 800);
    TimeManager clean(clock_limits(milliseconds{30'000}, milliseconds{1'000}),
                      koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                      {}, true, {}, 30);
    require(laggy.diagnostics().soft_budget + milliseconds{400} < clean.diagnostics().soft_budget,
            "high network latency (>100ms) must discount net increment credit even above 15 increments");

    auto calm_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager calm(clock_limits(milliseconds{120'000}, milliseconds{1'000}),
                     koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                     [calm_now] { return *calm_now; });
    for (int depth = 4; depth <= 6; ++depth) {
        calm.observe_iteration(
            {depth, 30, false, false, false, 10'000, 5'000, milliseconds{30}});
    }
    const auto opt = calm.diagnostics().optimum_budget;
    *calm_now += (opt * 65) / 100;
    require(!calm.should_stop_after_iteration() && calm.should_start_next_iteration(milliseconds{1}),
            "calm search at 65% optimum_budget must continue");
    *calm_now += (opt * 11) / 100 + milliseconds{1};
    require(calm.should_stop_after_iteration() && !calm.should_start_next_iteration(milliseconds{1}),
            "calm search past 75% optimum_budget must stop symmetrically after iteration");
}

void test_opponent_flagging_pressure_and_aspiration_calm_symmetry() {
    auto make_asymmetric_limits = [](milliseconds white_rem, milliseconds black_rem, milliseconds inc) {
        SearchLimits limits;
        limits.white_clock = ClockLimit{white_rem, inc};
        limits.black_clock = ClockLimit{black_rem, inc};
        return limits;
    };
    TimeManager sd_equal(make_asymmetric_limits(milliseconds{30'000}, milliseconds{30'000}, milliseconds{0}),
                         koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    TimeManager sd_flagging(make_asymmetric_limits(milliseconds{30'000}, milliseconds{4'000}, milliseconds{0}),
                            koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    require(sd_flagging.diagnostics().horizon > sd_equal.diagnostics().horizon &&
                sd_flagging.diagnostics().soft_budget < sd_equal.diagnostics().soft_budget,
            "sudden-death opponent <=5s clock must stretch horizon and tighten soft budget");

    TimeManager inc_equal(make_asymmetric_limits(milliseconds{30'000}, milliseconds{30'000}, milliseconds{1'000}),
                          koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    TimeManager inc_low_opp(make_asymmetric_limits(milliseconds{30'000}, milliseconds{4'000}, milliseconds{1'000}),
                            koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
    require(inc_low_opp.diagnostics().soft_budget >= inc_equal.diagnostics().soft_budget,
            "incremental clock advantage must retain conversion time against low opponent clock");

    auto asp_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager asp(clock_limits(milliseconds{120'000}, milliseconds{1'000}),
                    koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                    [asp_now] { return *asp_now; });
    asp.observe_iteration({4, 30, false, false, false, 10'000, 5'000, milliseconds{20}});
    asp.observe_iteration({5, 30, false, false, false, 10'000, 5'000, milliseconds{20}});
    asp.observe_iteration({6, 30, false, false, true, 10'000, 5'000, milliseconds{20}});
    const auto opt = asp.diagnostics().optimum_budget;
    *asp_now += (opt * 80) / 100;
    require(!asp.should_stop_after_iteration() &&
                asp.should_start_next_iteration(milliseconds{1}),
            "aspiration re-search at last depth must keep 75% calm stop/start symmetric");
}

void test_aspiration_research_fallback_guard_and_emergency_depth4_stop() {
    auto now_pt = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager asp_hard(clock_limits(milliseconds{120'000}, milliseconds{1'000}),
                         koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                         [now_pt] { return *now_pt; });
    asp_hard.observe_iteration({5, 100, false, false, false, 10'000, 5'000, milliseconds{20}});
    asp_hard.observe_iteration({6, 70, false, false, true, 10'000, 5'000, milliseconds{20}});
    *now_pt += asp_hard.diagnostics().optimum_budget + milliseconds{5};
    require(!asp_hard.should_stop_after_iteration(),
            "aspiration re-search with moderate score drop must keep hard extension");

    auto emg_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg(clock_limits(milliseconds{2'000}, milliseconds{0}),
                    koi::Color::white, 100, kMoveOverheadMs, 100, RootTimingContext{},
                    [emg_now] { return *emg_now; });
    emg.observe_iteration({3, 0, false, false, false, 10'000, 5'000, milliseconds{5}});
    emg.observe_iteration({4, 0, false, false, false, 10'000, 5'000, milliseconds{5}});
    *emg_now += emg.diagnostics().optimum_budget;
    require(emg.diagnostics().emergency_pacing && emg.should_stop_after_iteration(),
            "emergency pacing must allow depth-4 stable fallback stop past optimum_budget");
}

void test_single_move_zero_hardness_and_deep_ponderhit_fast_exit() {
    RootTimingContext single_ctx;
    single_ctx.legal_move_count = 1;
    single_ctx.forcing_move_count = 1;
    single_ctx.in_check = true;
    TimeManager forced_single(clock_limits(milliseconds{60'000}, milliseconds{1'000}),
                              koi::Color::white, 100, kMoveOverheadMs, 100, single_ctx);
    require(forced_single.diagnostics().initial_hardness == 0 &&
                !forced_single.diagnostics().extended_for_hard_position,
            "single-legal-move root must classify as zero hardness even in check");

    auto now_pt = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits ponder_limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    ponder_limits.ponder = true;
    TimeManager ponder(ponder_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                       warm_context(), [now_pt] { return *now_pt; });
    for (int depth = 6; depth <= 10; ++depth) {
        ponder.observe_iteration(
            {depth, 30, false, false, false, 10'000, 9'500, milliseconds{25}});
    }
    SearchLimits live_limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    ponder.reconfigure(live_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                       warm_context(), false, *now_pt, 0);
    const auto opt = ponder.diagnostics().optimum_budget;
    *now_pt += (opt * 15) / 100;
    require(!ponder.should_stop_after_iteration() &&
                ponder.should_start_next_iteration(milliseconds{1}),
            "deep high-effort ponderhit at 15% optimum_budget must continue");
    *now_pt += (opt * 12) / 100 + milliseconds{1};
    require(ponder.should_stop_after_iteration() &&
                !ponder.should_start_next_iteration(milliseconds{1}),
            "deep >=10 ply >=90% effort ponderhit past 25% optimum_budget must exit symmetrically");
}

void test_sudden_death_scramble_calm_depth5_cutoff() {
    auto sd_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager sd(clock_limits(milliseconds{8'000}, milliseconds{0}),
                   koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                   [sd_now] { return *sd_now; });
    sd.observe_iteration({4, 20, false, false, false, 10'000, 5'000, milliseconds{10}});
    sd.observe_iteration({5, 20, false, false, false, 10'000, 5'000, milliseconds{15}});
    const auto sd_opt = sd.diagnostics().optimum_budget;
    *sd_now += (sd_opt * 60) / 100;
    require(!sd.should_stop_after_iteration() &&
                sd.should_start_next_iteration(milliseconds{1}),
            "sudden-death scramble calm depth-5 search at 60% optimum_budget must continue");
    *sd_now += (sd_opt * 12) / 100 + milliseconds{1};
    require(sd.should_stop_after_iteration() &&
                !sd.should_start_next_iteration(milliseconds{1}),
            "sudden-death scramble calm depth-5 search past 70% optimum_budget must stop cleanly");

    auto inc_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager inc(clock_limits(milliseconds{8'000}, milliseconds{1'000}),
                    koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                    [inc_now] { return *inc_now; });
    inc.observe_iteration({4, 20, false, false, false, 10'000, 5'000, milliseconds{10}});
    inc.observe_iteration({5, 20, false, false, false, 10'000, 5'000, milliseconds{15}});
    *inc_now += (inc.diagnostics().optimum_budget * 72) / 100;
    require(!inc.should_stop_after_iteration() &&
                inc.should_start_next_iteration(milliseconds{1}),
            "incremental clock at depth 5 and 72% optimum_budget must not trigger sudden-death scramble cutoff");
}

void test_deep_ponderhit_doomed_over_budget_ply_bank() {
    auto now_over = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits ponder_limits = clock_limits(milliseconds{20'000}, milliseconds{0});
    ponder_limits.ponder = true;
    TimeManager over(ponder_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                     warm_context(), [now_over] { return *now_over; });
    over.observe_iteration({6, 25, false, false, false, 10'000, 5'000, milliseconds{800}});
    over.observe_iteration({7, 25, false, false, false, 10'000, 5'000, milliseconds{1'600}});
    over.observe_iteration({8, 25, false, false, false, 10'000, 5'000, milliseconds{3'200}});
    over.reconfigure(clock_limits(milliseconds{20'000}, milliseconds{0}),
                     koi::Color::white, 100, kMoveOverheadMs, 100,
                     warm_context(), false, *now_over, 0);
    require(over.should_stop_after_iteration() &&
                !over.should_start_next_iteration(milliseconds{1}),
            "calm deep ponderhit whose next ply exceeds 2x hard_budget must bank immediately");

    auto now_fast = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager fast(ponder_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                     warm_context(), [now_fast] { return *now_fast; });
    fast.observe_iteration({6, 25, false, false, false, 10'000, 5'000, milliseconds{25}});
    fast.observe_iteration({7, 25, false, false, false, 10'000, 5'000, milliseconds{50}});
    fast.observe_iteration({8, 25, false, false, false, 10'000, 5'000, milliseconds{100}});
    fast.reconfigure(clock_limits(milliseconds{20'000}, milliseconds{0}),
                     koi::Color::white, 100, kMoveOverheadMs, 100,
                     warm_context(), false, *now_fast, 0);
    require(!fast.should_stop_after_iteration() &&
                fast.should_start_next_iteration(milliseconds{1}),
            "ponderhit whose next ply fits inside 2x hard_budget must keep searching at elapsed=0");

    auto now_drop = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager drop(ponder_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                     warm_context(), [now_drop] { return *now_drop; });
    drop.observe_iteration({6, 60, false, false, false, 10'000, 5'000, milliseconds{800}});
    drop.observe_iteration({7, 60, false, false, false, 10'000, 5'000, milliseconds{1'600}});
    drop.observe_iteration({8, 25, false, false, false, 10'000, 5'000, milliseconds{3'200}});
    drop.reconfigure(clock_limits(milliseconds{20'000}, milliseconds{0}),
                     koi::Color::white, 100, kMoveOverheadMs, 100,
                     warm_context(), false, *now_drop, 0);
    require(!drop.should_stop_after_iteration() &&
                drop.should_start_next_iteration(milliseconds{1}),
            "ponderhit with a >15 cp score drop must not exit early even on expensive next ply");
}

void test_defensive_mate_preserves_budget_and_unified_stop_start_symmetry() {
    auto neg_now = std::make_shared<Clock::time_point>(Clock::now());
    auto pos_now = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits ponder_limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});
    ponder_limits.ponder = true;
    const SearchLimits live_limits = clock_limits(milliseconds{120'000}, milliseconds{1'000});

    TimeManager getting_mated(ponder_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                              warm_context(), [neg_now] { return *neg_now; });
    TimeManager delivering_mate(ponder_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                                warm_context(), [pos_now] { return *pos_now; });
    for (int depth = 5; depth <= 6; ++depth) {
        getting_mated.observe_iteration(
            {depth, -99'500, false, false, false, 10'000, 9'000, milliseconds{15}});
        delivering_mate.observe_iteration(
            {depth, 99'500, false, false, false, 10'000, 9'000, milliseconds{15}});
    }
    getting_mated.reconfigure(live_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                              warm_context(), false, *neg_now, 0);
    delivering_mate.reconfigure(live_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                                warm_context(), false, *pos_now, 0);
    require(getting_mated.diagnostics().optimum_budget > milliseconds{250} &&
                !getting_mated.should_stop_after_iteration() &&
                getting_mated.should_start_next_iteration(milliseconds{1}),
            "getting mated (-M) must preserve defensive budget instead of fast-exiting at 0 ms");
    require(delivering_mate.diagnostics().optimum_budget <= milliseconds{250} &&
                delivering_mate.should_stop_after_iteration() &&
                !delivering_mate.should_start_next_iteration(milliseconds{1}),
            "delivering mate (+M) must retain fast-exit budget cap and instant ponderhit exit");

    auto sym_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager sym(live_limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                    warm_context(), [sym_now] { return *sym_now; });
    sym.observe_iteration({4, 30, true, false, false, 10'000, 5'000, milliseconds{20}});
    sym.observe_iteration({5, 20, false, false, false, 10'000, 5'000, milliseconds{25}});
    *sym_now += sym.diagnostics().optimum_budget;
    require(sym.should_stop_after_iteration() &&
                !sym.should_start_next_iteration(milliseconds{1}),
            "non-collapsing depth-5 fallback must stay symmetric between stop and start checks");
}

void test_dead_drawn_endgame_speedup_banks_clock() {
    const SearchLimits clock = clock_limits(milliseconds{60'000}, milliseconds{1'000});
    RootTimingContext endgame_ctx = warm_context();
    endgame_ctx.game_phase = 4;
    RootTimingContext mid_ctx = warm_context();
    mid_ctx.game_phase = 16;

    TimeManager drawn_endgame(clock, koi::Color::white, 100, kMoveOverheadMs, 100, endgame_ctx);
    TimeManager drawn_middlegame(clock, koi::Color::white, 100, kMoveOverheadMs, 100, mid_ctx);
    TimeManager imbalanced_endgame(clock, koi::Color::white, 100, kMoveOverheadMs, 100, endgame_ctx);
    for (int depth = 6; depth <= 10; ++depth) {
        const SearchIterationObservation drawn_obs{
            depth, 0, false, false, false, 10'000, 5'000, milliseconds{25}};
        drawn_endgame.observe_iteration(drawn_obs);
        drawn_middlegame.observe_iteration(drawn_obs);
        imbalanced_endgame.observe_iteration(
            {depth, 80, false, false, false, 10'000, 5'000, milliseconds{25}});
    }
    require(drawn_endgame.diagnostics().optimum_budget <
                drawn_middlegame.diagnostics().optimum_budget,
            "dead-drawn low-phase endgame must bank clock relative to middlegame");
    require(drawn_endgame.diagnostics().optimum_budget <
                imbalanced_endgame.diagnostics().optimum_budget,
            "dead-drawn endgame speedup must not slow imbalanced (+80 cp) endgames");
}

void test_hard_position_doomed_overrun_ply_guard() {
    const SearchLimits clock = clock_limits(milliseconds{60'000}, milliseconds{1'000});
    auto stable_now = std::make_shared<Clock::time_point>(Clock::now());
    auto dropping_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager stable_hard(clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                            warm_context(), [stable_now] { return *stable_now; });
    TimeManager dropping_hard(clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                              warm_context(), [dropping_now] { return *dropping_now; });

    const auto hard_budget = stable_hard.diagnostics().hard_budget;
    const auto d5_time = std::max(milliseconds{20}, (hard_budget * 18) / 100);
    const auto d6_time = std::max(milliseconds{36}, (hard_budget * 36) / 100);
    const auto d7_time = std::max(milliseconds{60}, (hard_budget * 60) / 100);
    // Repeated root move changes with low best-move share raise optimum_budget
    // above 70% of hard_budget while latching hard_position_; depth 7 settles
    // on one best move without a score collapse.
    for (TimeManager* tm : {&stable_hard, &dropping_hard}) {
        tm->observe_iteration({4, 50, true, false, false, 10'000, 2'000, milliseconds{150}});
        tm->observe_iteration({5, 50, true, false, false, 10'000, 2'000, d5_time});
        tm->observe_iteration({6, 50, true, false, false, 10'000, 2'000, d6_time});
    }
    stable_hard.observe_iteration({7, 45, false, false, false, 10'000, 2'000, d7_time});
    dropping_hard.observe_iteration({7, 5, false, false, false, 10'000, 2'000, d7_time});

    const auto elapsed_70 = (hard_budget * 70) / 100;
    *stable_now += elapsed_70;
    *dropping_now += elapsed_70;
    require(elapsed_70 < stable_hard.diagnostics().optimum_budget &&
                !stable_hard.should_start_next_iteration(milliseconds{1}),
            "hard position at >=65% hard_budget with >140% overrun next ply must bank clock when stable");
    require(dropping_hard.should_start_next_iteration(milliseconds{1}),
            "hard position suffering a >=40 cp score drop must still attempt the next ply inside hard_budget");
}

void test_high_latency_usable_clock_engages_emergency_pacing() {
    TimeManager laggy(clock_limits(milliseconds{3'500}, milliseconds{0}),
                      koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                      {}, false, {}, 800);
    TimeManager clean(clock_limits(milliseconds{3'500}, milliseconds{0}),
                      koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                      {}, false, {}, 30);
    require(laggy.diagnostics().usable <= milliseconds{2'000} &&
                laggy.diagnostics().emergency_pacing,
            "high network latency dropping usable clock to <=2000ms must engage emergency pacing");
    require(!clean.diagnostics().emergency_pacing &&
                laggy.diagnostics().soft_budget < clean.diagnostics().soft_budget &&
                laggy.diagnostics().hard_budget < clean.diagnostics().hard_budget,
            "low-latency 3500ms clock must stay out of emergency pacing and keep larger budgets");
}

void test_emergency_high_effort_depth5_recapture_cutoff() {
    auto emg_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg(clock_limits(milliseconds{2'000}, milliseconds{1'000}),
                    koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                    [emg_now] { return *emg_now; });
    emg.observe_iteration({4, 25, false, false, false, 10'000, 9'500, milliseconds{5}});
    emg.observe_iteration({5, 25, false, false, false, 10'000, 9'500, milliseconds{5}});
    const auto emg_opt = emg.diagnostics().optimum_budget;
    *emg_now += (emg_opt * 50) / 100;
    require(emg.diagnostics().emergency_pacing &&
                !emg.should_stop_after_iteration() &&
                emg.should_start_next_iteration(milliseconds{1}),
            "emergency high-effort depth-5 search at 50% optimum_budget must continue");
    *emg_now += (emg_opt * 12) / 100 + milliseconds{1};
    require(emg.should_stop_after_iteration() &&
                !emg.should_start_next_iteration(milliseconds{1}),
            "emergency high-effort depth-5 search past 60% optimum_budget must exit symmetrically");

    auto healthy_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager healthy(clock_limits(milliseconds{60'000}, milliseconds{1'000}),
                        koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                        [healthy_now] { return *healthy_now; });
    healthy.observe_iteration({4, 25, false, false, false, 10'000, 9'500, milliseconds{10}});
    healthy.observe_iteration({5, 25, false, false, false, 10'000, 9'500, milliseconds{10}});
    *healthy_now += (healthy.diagnostics().optimum_budget * 65) / 100;
    require(!healthy.diagnostics().emergency_pacing &&
                !healthy.should_stop_after_iteration() &&
                healthy.should_start_next_iteration(milliseconds{1}),
            "healthy clock at depth 5 and 65% optimum_budget must keep depth-6 effort gate");
}

void test_emergency_doomed_depth5_ply_guard() {
    auto emg_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg(clock_limits(milliseconds{2'000}, milliseconds{0}),
                    koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                    [emg_now] { return *emg_now; });
    // Soft budget is tight (~80 ms); complete depth 5 right past optimum_budget
    // with a sample >= 5 ms whose 1.5x predicted depth 6 pushes past hard_budget.
    emg.observe_iteration({4, 25, true, false, false, 10'000, 5'000, milliseconds{60}});
    emg.observe_iteration({5, 25, true, false, false, 10'000, 5'000, milliseconds{120}});
    const auto emg_hard = emg.diagnostics().hard_budget;
    const auto emg_opt = emg.diagnostics().optimum_budget;
    const auto emg_check = std::max(emg_opt + milliseconds{1}, emg_hard - milliseconds{40});
    *emg_now += std::min(emg_hard - milliseconds{5}, emg_check);
    require(emg.diagnostics().emergency_pacing &&
                !emg.should_start_next_iteration(milliseconds{1}),
            "emergency pacing at depth 5 past optimum_budget with doomed next ply must bank clock");

    auto drop_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg_drop(clock_limits(milliseconds{2'000}, milliseconds{0}),
                         koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                         [drop_now] { return *drop_now; });
    emg_drop.observe_iteration({4, 100, true, false, false, 10'000, 5'000, milliseconds{60}});
    emg_drop.observe_iteration({5, 30, true, false, false, 10'000, 5'000, milliseconds{120}});
    *drop_now += std::min(emg_drop.diagnostics().hard_budget - milliseconds{5},
                          std::max(emg_drop.diagnostics().optimum_budget + milliseconds{1},
                                   emg_drop.diagnostics().hard_budget - milliseconds{40}));
    require(emg_drop.should_start_next_iteration(milliseconds{1}),
            "emergency pacing with >=60 cp drop at depth 5 must still attempt depth 6 inside hard_budget");

    auto healthy_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager healthy(clock_limits(milliseconds{60'000}, milliseconds{1'000}),
                        koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                        [healthy_now] { return *healthy_now; });
    healthy.observe_iteration({4, 25, true, false, false, 10'000, 5'000, milliseconds{500}});
    healthy.observe_iteration({5, 25, true, false, false, 10'000, 5'000, milliseconds{1'800}});
    *healthy_now += healthy.diagnostics().optimum_budget + milliseconds{1};
    require(!healthy.diagnostics().emergency_pacing &&
                healthy.should_start_next_iteration(milliseconds{1}),
            "healthy clock at depth 5 past optimum_budget must retain depth>=6 doomed-ply threshold");
}

void test_emergency_hard_position_doomed_overrun_depth5_guard() {
    auto stable_now = std::make_shared<Clock::time_point>(Clock::now());
    auto dropping_now = std::make_shared<Clock::time_point>(Clock::now());
    const SearchLimits emg_clock = clock_limits(milliseconds{2'000}, milliseconds{1'000});
    TimeManager stable_emg(emg_clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                           warm_context(), [stable_now] { return *stable_now; });
    TimeManager dropping_emg(emg_clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                             warm_context(), [dropping_now] { return *dropping_now; });

    const auto emg_hard = stable_emg.diagnostics().hard_budget;
    const auto d4_time = std::max(milliseconds{10}, (emg_hard * 30) / 100);
    const auto d5_time = std::max(milliseconds{20}, (emg_hard * 52) / 100);
    for (TimeManager* tm : {&stable_emg, &dropping_emg}) {
        tm->observe_iteration({4, 50, true, false, false, 10'000, 2'000, d4_time});
    }
    stable_emg.observe_iteration({5, 45, false, false, false, 10'000, 2'000, d5_time});
    dropping_emg.observe_iteration({5, 5, false, false, false, 10'000, 2'000, d5_time});

    const auto elapsed_70 = (emg_hard * 70) / 100;
    *stable_now += elapsed_70;
    *dropping_now += elapsed_70;
    require(stable_emg.diagnostics().emergency_pacing &&
                elapsed_70 < stable_emg.diagnostics().optimum_budget &&
                !stable_emg.should_start_next_iteration(milliseconds{1}),
            "emergency hard position at depth 5 and >=65% hard_budget with >140% overrun next ply must bank clock");
    require(dropping_emg.should_start_next_iteration(milliseconds{1}),
            "emergency hard position suffering >=40 cp score drop at depth 5 must still attempt depth 6 inside hard_budget");

    auto healthy_now = std::make_shared<Clock::time_point>(Clock::now());
    const SearchLimits healthy_clock = clock_limits(milliseconds{60'000}, milliseconds{1'000});
    TimeManager healthy(healthy_clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                        warm_context(), [healthy_now] { return *healthy_now; });
    const auto healthy_hard = healthy.diagnostics().hard_budget;
    healthy.observe_iteration(
        {4, 50, true, false, false, 10'000, 2'000, (healthy_hard * 30) / 100});
    healthy.observe_iteration(
        {5, 45, false, false, false, 10'000, 2'000, (healthy_hard * 52) / 100});
    *healthy_now += (healthy_hard * 70) / 100;
    require(!healthy.diagnostics().emergency_pacing &&
                healthy.should_start_next_iteration(milliseconds{1}),
            "healthy clock hard position at depth 5 must retain depth>=7 pre-overrun guard");
}

void test_emergency_calm_depth5_70pct_cutoff() {
    auto emg_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg(clock_limits(milliseconds{2'000}, milliseconds{1'000}),
                    koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                    [emg_now] { return *emg_now; });
    emg.observe_iteration({4, 20, false, false, false, 10'000, 5'000, milliseconds{10}});
    emg.observe_iteration({5, 20, false, false, false, 10'000, 5'000, milliseconds{10}});
    const auto emg_opt = emg.diagnostics().optimum_budget;
    *emg_now += (emg_opt * 60) / 100;
    require(emg.diagnostics().emergency_pacing &&
                !emg.should_stop_after_iteration() &&
                emg.should_start_next_iteration(milliseconds{1}),
            "emergency calm depth-5 search at 60% optimum_budget must continue");
    *emg_now += (emg_opt * 12) / 100 + milliseconds{1};
    require(emg.should_stop_after_iteration() &&
                !emg.should_start_next_iteration(milliseconds{1}),
            "emergency calm depth-5 search past 70% optimum_budget must stop cleanly and symmetrically");

    auto normal_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager normal_inc(clock_limits(milliseconds{8'000}, milliseconds{1'000}),
                           koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                           [normal_now] { return *normal_now; });
    normal_inc.observe_iteration({4, 20, false, false, false, 10'000, 5'000, milliseconds{10}});
    normal_inc.observe_iteration({5, 20, false, false, false, 10'000, 5'000, milliseconds{10}});
    *normal_now += (normal_inc.diagnostics().optimum_budget * 72) / 100;
    require(!normal_inc.diagnostics().emergency_pacing &&
                !normal_inc.should_stop_after_iteration() &&
                normal_inc.should_start_next_iteration(milliseconds{1}),
            "non-emergency incremental clock at depth 5 and 72% optimum_budget must continue");

    auto asp_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager asp_emg(clock_limits(milliseconds{2'000}, milliseconds{1'000}),
                        koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                        [asp_now] { return *asp_now; });
    asp_emg.observe_iteration({4, 20, false, false, false, 10'000, 5'000, milliseconds{10}});
    asp_emg.observe_iteration({5, 50, false, false, true, 10'000, 5'000, milliseconds{10}});
    *asp_now += (asp_emg.diagnostics().optimum_budget * 72) / 100;
    require(asp_emg.should_start_next_iteration(milliseconds{1}),
            "emergency incremental search with aspiration re-search at depth 5 must not cut off at 72%");
}

void test_emergency_calm_depth4_pre_iteration_lookahead() {
    auto emg_now = std::make_shared<Clock::time_point>(Clock::now());
    auto early_now = std::make_shared<Clock::time_point>(Clock::now());
    const SearchLimits emg_clock = clock_limits(milliseconds{2'000}, milliseconds{0});
    TimeManager emg(emg_clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                    warm_context(), [emg_now] { return *emg_now; });
    TimeManager emg_early(emg_clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                          warm_context(), [early_now] { return *early_now; });
    const auto d3 = std::max(milliseconds{5}, (emg.diagnostics().soft_budget * 25) / 100);
    const auto d4 = std::max(milliseconds{10}, (emg.diagnostics().soft_budget * 50) / 100);
    for (TimeManager* tm : {&emg, &emg_early}) {
        tm->observe_iteration({3, 20, false, false, false, 10'000, 5'000, d3});
        tm->observe_iteration({4, 20, false, false, false, 10'000, 5'000, d4});
    }
    const auto emg_opt = emg.diagnostics().optimum_budget;
    *emg_now += (emg_opt * 60) / 100;
    *early_now += (emg_opt * 45) / 100;
    require(emg.diagnostics().emergency_pacing &&
                !emg.should_stop_after_iteration() &&
                !emg.should_start_next_iteration(milliseconds{1}),
            "emergency calm depth-4 lookahead at 60% optimum_budget must skip doomed depth 5");
    require(emg_early.should_start_next_iteration(milliseconds{1}),
            "emergency calm depth-4 lookahead before 55% optimum_budget must continue");

    auto healthy_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager healthy(clock_limits(milliseconds{60'000}, milliseconds{1'000}),
                        koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                        [healthy_now] { return *healthy_now; });
    const auto healthy_soft = healthy.diagnostics().soft_budget;
    healthy.observe_iteration(
        {3, 20, false, false, false, 10'000, 5'000, (healthy_soft * 25) / 100});
    healthy.observe_iteration(
        {4, 20, false, false, false, 10'000, 5'000, (healthy_soft * 50) / 100});
    *healthy_now += (healthy.diagnostics().optimum_budget * 60) / 100;
    require(!healthy.diagnostics().emergency_pacing &&
                healthy.should_start_next_iteration(milliseconds{1}),
            "healthy clock at depth 4 and 60% optimum_budget must retain depth>=5 lookahead gate");
}

void test_emergency_depth4_branching_factor_updates_lookahead() {
    auto steep_now = std::make_shared<Clock::time_point>(Clock::now());
    auto flat_now = std::make_shared<Clock::time_point>(Clock::now());
    const SearchLimits emg_clock = clock_limits(milliseconds{2'000}, milliseconds{1'000});
    TimeManager steep(emg_clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                      warm_context(), [steep_now] { return *steep_now; });
    TimeManager flat(emg_clock, koi::Color::white, 100, kMoveOverheadMs, 100,
                     warm_context(), [flat_now] { return *flat_now; });

    const auto d4 = std::max(milliseconds{12}, (steep.diagnostics().soft_budget * 30) / 100);
    const auto d3_steep = std::max(milliseconds{4}, d4 / 3);
    const auto d3_flat = d4;
    steep.observe_iteration({3, 20, false, false, false, 10'000, 5'000, d3_steep});
    steep.observe_iteration({4, 20, false, false, false, 10'000, 5'000, d4});
    flat.observe_iteration({3, 20, false, false, false, 10'000, 5'000, d3_flat});
    flat.observe_iteration({4, 20, false, false, false, 10'000, 5'000, d4});

    const auto steep_opt = steep.diagnostics().optimum_budget;
    const auto flat_opt = flat.diagnostics().optimum_budget;
    *steep_now += (steep_opt * 62) / 100;
    *flat_now += (flat_opt * 62) / 100;
    require(steep.diagnostics().emergency_pacing &&
                !steep.should_stop_after_iteration() &&
                !steep.should_start_next_iteration(milliseconds{1}),
            "emergency depth-4 branching EWMA must feed lookahead when d3->d4 grows steeply");
    require(flat.diagnostics().emergency_pacing &&
                flat.should_start_next_iteration(milliseconds{1}),
            "emergency depth-4 flat d3->d4 timing must keep branching low enough to continue");

    auto healthy_now = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager healthy(clock_limits(milliseconds{60'000}, milliseconds{1'000}),
                        koi::Color::white, 100, kMoveOverheadMs, 100, warm_context(),
                        [healthy_now] { return *healthy_now; });
    const auto healthy_d4 =
        std::max(milliseconds{30}, (healthy.diagnostics().soft_budget * 30) / 100);
    healthy.observe_iteration({3, 20, false, false, false, 10'000, 5'000, healthy_d4 / 3});
    healthy.observe_iteration({4, 20, false, false, false, 10'000, 5'000, healthy_d4});
    *healthy_now += (healthy.diagnostics().optimum_budget * 62) / 100;
    require(!healthy.diagnostics().emergency_pacing &&
                healthy.should_start_next_iteration(milliseconds{1}),
            "healthy clock at depth 4 must retain depth>=5 branching-factor update gate");
}

void test_emergency_depth5_asymmetric_conversion_speedup() {
    auto emg_opt_at = [](const int score_cp) {
        TimeManager tm(clock_limits(milliseconds{2'000}, milliseconds{1'000}),
                       koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
        tm.observe_iteration({4, score_cp, false, false, false, 10'000, 5'000, milliseconds{10}});
        tm.observe_iteration({5, score_cp, false, false, false, 10'000, 5'000, milliseconds{15}});
        require(tm.diagnostics().emergency_pacing,
                "low clock must engage emergency pacing for conversion test");
        return tm.diagnostics().optimum_budget;
    };
    const auto emg_even = emg_opt_at(20);
    const auto emg_win = emg_opt_at(700);
    const auto emg_loss = emg_opt_at(-750);
    const auto emg_defense = emg_opt_at(-550);
    require(emg_win < emg_even,
            "emergency winning position (>=+500 cp) at depth 5 must engage conversion speedup");
    require(emg_loss < emg_even,
            "emergency hopeless position (<=-700 cp) at depth 5 must engage conversion speedup");
    require(emg_defense == emg_even,
            "emergency defensive band (-500..-699 cp) at depth 5 must retain full optimum_budget");

    auto healthy_opt_at = [](const int score_cp) {
        TimeManager tm(clock_limits(milliseconds{60'000}, milliseconds{1'000}),
                       koi::Color::white, 100, kMoveOverheadMs, 100, warm_context());
        tm.observe_iteration({4, score_cp, false, false, false, 10'000, 5'000, milliseconds{10}});
        tm.observe_iteration({5, score_cp, false, false, false, 10'000, 5'000, milliseconds{15}});
        require(!tm.diagnostics().emergency_pacing,
                "healthy clock must stay out of emergency pacing");
        return tm.diagnostics().optimum_budget;
    };
    require(healthy_opt_at(700) == healthy_opt_at(20),
            "healthy clock at depth 5 must retain depth>=8 conversion speedup threshold");
}

void test_emergency_depth6_dead_drawn_endgame_speedup() {
    auto endgame_ctx = warm_context();
    endgame_ctx.game_phase = 4;
    auto mid_ctx = warm_context();
    mid_ctx.game_phase = 16;

    auto observe_depth6 = [](TimeManager& tm, const int s5, const int s6) {
        tm.observe_iteration({5, s5, false, false, false, 10'000, 5'000, milliseconds{10}});
        tm.observe_iteration({6, s6, false, false, false, 10'000, 5'000, milliseconds{15}});
    };

    const SearchLimits emg_clock = clock_limits(milliseconds{2'000}, milliseconds{1'000});
    TimeManager emg_endgame(emg_clock, koi::Color::white, 100, kMoveOverheadMs, 100, endgame_ctx);
    TimeManager emg_mid(emg_clock, koi::Color::white, 100, kMoveOverheadMs, 100, mid_ctx);
    TimeManager emg_nondrawn(emg_clock, koi::Color::white, 100, kMoveOverheadMs, 100, endgame_ctx);
    observe_depth6(emg_endgame, 5, 8);
    observe_depth6(emg_mid, 5, 8);
    observe_depth6(emg_nondrawn, 60, 60);
    require(emg_endgame.diagnostics().emergency_pacing &&
                emg_endgame.diagnostics().optimum_budget < emg_mid.diagnostics().optimum_budget,
            "emergency dead-drawn endgame at depth 6 and stability 2 must apply 22% speedup");
    require(emg_nondrawn.diagnostics().optimum_budget == emg_mid.diagnostics().optimum_budget,
            "emergency endgame outside |score|<=15 cp must not trigger dead-drawn speedup");

    const SearchLimits healthy_clock = clock_limits(milliseconds{60'000}, milliseconds{1'000});
    TimeManager healthy_endgame(healthy_clock, koi::Color::white, 100, kMoveOverheadMs, 100, endgame_ctx);
    TimeManager healthy_mid(healthy_clock, koi::Color::white, 100, kMoveOverheadMs, 100, mid_ctx);
    observe_depth6(healthy_endgame, 5, 8);
    observe_depth6(healthy_mid, 5, 8);
    require(!healthy_endgame.diagnostics().emergency_pacing &&
                healthy_endgame.diagnostics().optimum_budget == healthy_mid.diagnostics().optimum_budget,
            "healthy clock dead-drawn endgame at depth 6 must retain depth>=10/stability>=4 gate");
}

void test_emergency_depth6_ponderhit_doomed_ply_bank() {
    auto now_over = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits emg_ponder = clock_limits(milliseconds{2'000}, milliseconds{0});
    emg_ponder.ponder = true;
    const SearchLimits emg_live = clock_limits(milliseconds{2'000}, milliseconds{0});
    TimeManager emg_over(emg_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                         warm_context(), [now_over] { return *now_over; });
    emg_over.observe_iteration({5, 25, false, false, false, 10'000, 5'000, milliseconds{120}});
    emg_over.observe_iteration({6, 25, false, false, false, 10'000, 5'000, milliseconds{240}});
    emg_over.reconfigure(emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                         warm_context(), false, *now_over, 0);
    require(emg_over.diagnostics().emergency_pacing &&
                emg_over.should_stop_after_iteration() &&
                !emg_over.should_start_next_iteration(milliseconds{1}),
            "emergency depth-6 ponderhit whose next ply exceeds 2x hard_budget must bank immediately");

    auto now_drop = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg_drop(emg_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                         warm_context(), [now_drop] { return *now_drop; });
    emg_drop.observe_iteration({5, 60, false, false, false, 10'000, 5'000, milliseconds{120}});
    emg_drop.observe_iteration({6, 25, false, false, false, 10'000, 5'000, milliseconds{240}});
    emg_drop.reconfigure(emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                         warm_context(), false, *now_drop, 0);
    require(!emg_drop.should_stop_after_iteration() &&
                emg_drop.should_start_next_iteration(milliseconds{1}),
            "emergency ponderhit with >15 cp score drop must not exit early at depth 6");

    auto now_healthy = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits healthy_ponder = clock_limits(milliseconds{20'000}, milliseconds{0});
    healthy_ponder.ponder = true;
    TimeManager healthy(healthy_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                        warm_context(), [now_healthy] { return *now_healthy; });
    healthy.observe_iteration({5, 25, false, false, false, 10'000, 5'000, milliseconds{800}});
    healthy.observe_iteration({6, 25, false, false, false, 10'000, 5'000, milliseconds{3'200}});
    healthy.reconfigure(clock_limits(milliseconds{20'000}, milliseconds{0}),
                        koi::Color::white, 100, kMoveOverheadMs, 100,
                        warm_context(), false, *now_healthy, 0);
    require(!healthy.diagnostics().emergency_pacing &&
                !healthy.should_stop_after_iteration() &&
                healthy.should_start_next_iteration(milliseconds{1}),
            "healthy clock ponderhit at depth 6 must retain depth>=8/stability>=3 doomed bank threshold");
}

void test_emergency_ponderhit_discount_and_high_effort_fast_exit() {
    auto now_emg_p = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits emg_ponder = clock_limits(milliseconds{2'000}, milliseconds{0});
    emg_ponder.ponder = true;
    const SearchLimits emg_live = clock_limits(milliseconds{2'000}, milliseconds{0});

    TimeManager emg_ponderhit(emg_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                              warm_context(), [now_emg_p] { return *now_emg_p; });
    TimeManager emg_direct(emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                           warm_context());
    for (TimeManager* tm : {&emg_ponderhit, &emg_direct}) {
        tm->observe_iteration({4, 25, false, false, false, 10'000, 5'000, milliseconds{4}});
        tm->observe_iteration({5, 25, false, false, false, 10'000, 5'000, milliseconds{5}});
    }
    emg_ponderhit.reconfigure(emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                              warm_context(), false, *now_emg_p, 0);
    require(emg_ponderhit.diagnostics().emergency_pacing &&
                emg_ponderhit.diagnostics().optimum_budget < emg_direct.diagnostics().optimum_budget,
            "emergency calm ponderhit at depth 5/stability 2 must apply 20% budget discount");

    auto now_healthy_p = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits healthy_ponder = clock_limits(milliseconds{20'000}, milliseconds{0});
    healthy_ponder.ponder = true;
    const SearchLimits healthy_live = clock_limits(milliseconds{20'000}, milliseconds{0});
    TimeManager healthy_ponderhit(healthy_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                                  warm_context(), [now_healthy_p] { return *now_healthy_p; });
    TimeManager healthy_direct(healthy_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                               warm_context());
    for (TimeManager* tm : {&healthy_ponderhit, &healthy_direct}) {
        tm->observe_iteration({4, 25, false, false, false, 10'000, 5'000, milliseconds{10}});
        tm->observe_iteration({5, 25, false, false, false, 10'000, 5'000, milliseconds{20}});
    }
    healthy_ponderhit.reconfigure(healthy_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                                  warm_context(), false, *now_healthy_p, 0);
    require(!healthy_ponderhit.diagnostics().emergency_pacing &&
                healthy_ponderhit.diagnostics().optimum_budget ==
                    healthy_direct.diagnostics().optimum_budget,
            "healthy calm ponderhit at depth 5/stability 2 must retain depth>=6/stability>=3 discount gate");

    auto now_fast = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg_fast(emg_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                         warm_context(), [now_fast] { return *now_fast; });
    emg_fast.observe_iteration({5, 25, false, false, false, 10'000, 9'500, milliseconds{4}});
    emg_fast.observe_iteration({6, 25, false, false, false, 10'000, 9'500, milliseconds{5}});
    emg_fast.reconfigure(emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                         warm_context(), false, *now_fast, 0);
    require(!emg_fast.should_stop_after_iteration(),
            "fast-sample emergency ponderhit must not trigger doomed bank before 25% elapsed");
    *now_fast += (emg_fast.diagnostics().optimum_budget * 30) / 100;
    require(emg_fast.should_stop_after_iteration() &&
                !emg_fast.should_start_next_iteration(milliseconds{1}),
            "emergency high-effort ponderhit at depth 6/stability 2 must exit at 25% target");

    auto now_asp = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg_asp(emg_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                        warm_context(), [now_asp] { return *now_asp; });
    emg_asp.observe_iteration({5, 25, false, false, false, 10'000, 9'500, milliseconds{4}});
    emg_asp.observe_iteration({6, 25, false, false, true, 10'000, 9'500, milliseconds{5}});
    emg_asp.reconfigure(emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                        warm_context(), false, *now_asp, 0);
    *now_asp += (emg_asp.diagnostics().optimum_budget * 30) / 100;
    require(!emg_asp.should_stop_after_iteration() &&
                emg_asp.should_start_next_iteration(milliseconds{1}),
            "emergency high-effort ponderhit after aspiration re-search must not exit at 25%");

    auto now_healthy_eff = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager healthy_eff(healthy_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                            warm_context(), [now_healthy_eff] { return *now_healthy_eff; });
    healthy_eff.observe_iteration({5, 25, false, false, false, 10'000, 9'500, milliseconds{10}});
    healthy_eff.observe_iteration({6, 25, false, false, false, 10'000, 9'500, milliseconds{15}});
    healthy_eff.reconfigure(healthy_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                            warm_context(), false, *now_healthy_eff, 0);
    *now_healthy_eff += (healthy_eff.diagnostics().optimum_budget * 30) / 100;
    require(!healthy_eff.diagnostics().emergency_pacing &&
                !healthy_eff.should_stop_after_iteration() &&
                healthy_eff.should_start_next_iteration(milliseconds{1}),
            "healthy high-effort ponderhit at depth 6/stability 2 must retain depth>=10/stability>=4 gate");
}

void test_emergency_single_move_and_winning_mate_fast_exit() {
    auto single_ctx = warm_context();
    single_ctx.legal_move_count = 1;

    auto now_emg_p = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits emg_ponder = clock_limits(milliseconds{2'000}, milliseconds{0});
    emg_ponder.ponder = true;
    const SearchLimits emg_live = clock_limits(milliseconds{2'000}, milliseconds{0});

    TimeManager emg_single_ponder(emg_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                                  single_ctx, [now_emg_p] { return *now_emg_p; });
    emg_single_ponder.observe_iteration({2, 10, false, false, false, 200, 200, milliseconds{1}});
    emg_single_ponder.reconfigure(emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                                  single_ctx, false, *now_emg_p, 0);
    require(emg_single_ponder.diagnostics().emergency_pacing &&
                emg_single_ponder.should_stop_after_iteration() &&
                !emg_single_ponder.should_start_next_iteration(milliseconds{1}),
            "emergency single-legal-move ponderhit at depth 2 must exit immediately on ponderhit");

    auto now_emg_live = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg_single_live(emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                                single_ctx, [now_emg_live] { return *now_emg_live; });
    emg_single_live.observe_iteration({2, 10, false, false, false, 200, 200, milliseconds{1}});
    *now_emg_live += emg_single_live.diagnostics().optimum_budget;
    require(emg_single_live.should_stop_after_iteration() &&
                !emg_single_live.should_start_next_iteration(milliseconds{1}),
            "emergency single-legal-move search at depth 2 must exit once optimum_budget elapses");

    auto now_healthy_s = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits healthy_ponder = clock_limits(milliseconds{20'000}, milliseconds{0});
    healthy_ponder.ponder = true;
    const SearchLimits healthy_live = clock_limits(milliseconds{20'000}, milliseconds{0});
    TimeManager healthy_single(healthy_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                               single_ctx, [now_healthy_s] { return *now_healthy_s; });
    healthy_single.observe_iteration({2, 10, false, false, false, 200, 200, milliseconds{1}});
    healthy_single.reconfigure(healthy_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                               single_ctx, false, *now_healthy_s, 0);
    *now_healthy_s += healthy_single.diagnostics().optimum_budget;
    require(!healthy_single.diagnostics().emergency_pacing &&
                !healthy_single.should_stop_after_iteration() &&
                healthy_single.should_start_next_iteration(milliseconds{1}),
            "healthy clock single-legal-move at depth 2 must retain depth>=4 exit gate");

    auto now_mate = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits tight_emg_ponder = clock_limits(milliseconds{1'000}, milliseconds{0});
    tight_emg_ponder.ponder = true;
    const SearchLimits tight_emg_live = clock_limits(milliseconds{1'000}, milliseconds{0});

    TimeManager emg_mate(tight_emg_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                         warm_context(), [now_mate] { return *now_mate; });
    emg_mate.observe_iteration({3, 99'900, false, false, false, 1'000, 800, milliseconds{2}});
    emg_mate.observe_iteration({4, 99'900, false, false, false, 1'000, 800, milliseconds{3}});
    emg_mate.reconfigure(tight_emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                         warm_context(), false, *now_mate, 0);
    require(emg_mate.diagnostics().emergency_pacing &&
                emg_mate.diagnostics().optimum_budget < milliseconds{20} &&
                emg_mate.should_stop_after_iteration() &&
                !emg_mate.should_start_next_iteration(milliseconds{1}),
            "emergency confirmed winning mate at depth 4 must use 5 ms floor and exit immediately on ponderhit");

    auto now_mated = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager emg_mated(tight_emg_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                          warm_context(), [now_mated] { return *now_mated; });
    emg_mated.observe_iteration({3, -99'900, false, false, false, 1'000, 800, milliseconds{2}});
    emg_mated.observe_iteration({4, -99'900, false, false, false, 1'000, 800, milliseconds{3}});
    emg_mated.reconfigure(tight_emg_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                          warm_context(), false, *now_mated, 0);
    require(!emg_mated.should_stop_after_iteration() &&
                emg_mated.should_start_next_iteration(milliseconds{1}),
            "emergency getting-mated (-M) ponderhit must retain defensive search budget");

    auto now_healthy_m = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager healthy_mate(healthy_ponder, koi::Color::white, 100, kMoveOverheadMs, 100,
                             warm_context(), [now_healthy_m] { return *now_healthy_m; });
    healthy_mate.observe_iteration({3, 99'900, false, false, false, 1'000, 800, milliseconds{2}});
    healthy_mate.observe_iteration({4, 99'900, false, false, false, 1'000, 800, milliseconds{3}});
    healthy_mate.reconfigure(healthy_live, koi::Color::white, 100, kMoveOverheadMs, 100,
                             warm_context(), false, *now_healthy_m, 0);
    require(!healthy_mate.diagnostics().emergency_pacing &&
                !healthy_mate.should_stop_after_iteration() &&
                healthy_mate.should_start_next_iteration(milliseconds{1}),
            "healthy clock winning-mate ponderhit at depth 4 must retain depth>=6 instant-exit gate");
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
        {"asymmetric conversion and high-effort cutoff",
         test_asymmetric_conversion_and_high_effort_recapture_cutoff},
        {"adaptive branching factor lookahead",
         test_adaptive_branching_factor_lookahead},
        {"high-latency guard and symmetric effort stop",
         test_high_latency_hard_extension_guard_and_symmetric_effort_stop},
        {"ponderhit discount, scramble cap, and doomed hard-ply guard",
         test_ponderhit_discount_scramble_cap_and_doomed_hard_ply_guard},
        {"repeating movestogo and sudden-death ramp",
         test_repeating_movestogo_end_of_control_and_sudden_death_ramp},
        {"sub-second sudden-death bullet scramble protection",
         test_subsecond_sudden_death_bullet_scramble_protection},
        {"ponderhit instant exit and repeating emergency horizon",
         test_ponderhit_instant_exit_and_repeating_emergency_horizon},
        {"overwhelming-win noise immunity and emergency bullet lookahead",
         test_overwhelming_win_noise_immunity_and_emergency_bullet_lookahead},
        {"high-latency net-increment credit and symmetric calm stop",
         test_high_latency_net_increment_credit_and_symmetric_calm_stop},
        {"opponent flagging pressure and aspiration calm symmetry",
         test_opponent_flagging_pressure_and_aspiration_calm_symmetry},
        {"aspiration re-search fallback guard and emergency depth-4 stop",
         test_aspiration_research_fallback_guard_and_emergency_depth4_stop},
        {"single-move zero hardness and deep ponderhit fast exit",
         test_single_move_zero_hardness_and_deep_ponderhit_fast_exit},
        {"sudden-death scramble calm depth-5 cutoff",
         test_sudden_death_scramble_calm_depth5_cutoff},
        {"deep ponderhit doomed over-budget ply bank",
         test_deep_ponderhit_doomed_over_budget_ply_bank},
        {"defensive mate preserves budget and unified stop/start symmetry",
         test_defensive_mate_preserves_budget_and_unified_stop_start_symmetry},
        {"dead drawn endgame speedup banks clock",
         test_dead_drawn_endgame_speedup_banks_clock},
        {"hard position doomed overrun ply guard",
         test_hard_position_doomed_overrun_ply_guard},
        {"high latency usable clock emergency pacing",
         test_high_latency_usable_clock_engages_emergency_pacing},
        {"emergency high effort depth 5 recapture cutoff",
         test_emergency_high_effort_depth5_recapture_cutoff},
        {"emergency doomed depth 5 ply guard",
         test_emergency_doomed_depth5_ply_guard},
        {"emergency hard position depth 5 overrun guard",
         test_emergency_hard_position_doomed_overrun_depth5_guard},
        {"emergency calm depth 5 70pct cutoff",
         test_emergency_calm_depth5_70pct_cutoff},
        {"emergency calm depth 4 lookahead",
         test_emergency_calm_depth4_pre_iteration_lookahead},
        {"emergency depth 4 branching lookahead",
         test_emergency_depth4_branching_factor_updates_lookahead},
        {"emergency depth 5 conversion speedup",
         test_emergency_depth5_asymmetric_conversion_speedup},
        {"emergency depth 6 dead drawn endgame speedup",
         test_emergency_depth6_dead_drawn_endgame_speedup},
        {"emergency depth 6 ponderhit doomed ply bank",
         test_emergency_depth6_ponderhit_doomed_ply_bank},
        {"emergency ponderhit discount and high effort exit",
         test_emergency_ponderhit_discount_and_high_effort_fast_exit},
        {"emergency single move and winning mate exit",
         test_emergency_single_move_and_winning_mate_fast_exit},
    };
    return koi::test::run_tests(tests, argc, argv);
}
