#include "koi/time_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "koi/detail/search_constants.hpp"

namespace koi {
namespace {

std::chrono::milliseconds with_safety_margin(std::chrono::milliseconds duration) noexcept {
    if (duration <= std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds::zero();
    }

    const auto base_margin = std::min(std::chrono::milliseconds{50},
                                      std::max(std::chrono::milliseconds{1}, duration / 20));
    // Longer explicit requests (such as 1000 ms GUI moves) reserve up to an
    // extra 50 ms for thread-pool teardown, validation, and pipe round-trip
    // jitter without altering short <= 500 ms allocations.
    const auto extra_teardown_margin = duration > std::chrono::milliseconds{500}
        ? std::min(std::chrono::milliseconds{50},
                   (duration - std::chrono::milliseconds{500}) / 10)
        : std::chrono::milliseconds::zero();
    const auto margin = base_margin + extra_teardown_margin;
    return duration > margin ? duration - margin : std::chrono::milliseconds{1};
}

std::chrono::milliseconds scale_duration(std::chrono::milliseconds duration,
                                         std::uint32_t percent,
                                         std::uint32_t denominator = 100) noexcept {
    if (duration <= std::chrono::milliseconds::zero()) {
        return duration;
    }

    const std::uint64_t scale = std::max<std::uint32_t>(1, percent);
    using Rep = std::chrono::milliseconds::rep;
    const std::uint64_t count = static_cast<std::uint64_t>(duration.count());
    const std::uint64_t quotient = count / denominator;
    const std::uint64_t remainder = count % denominator;
    const std::uint64_t maximum = static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
    if (quotient > maximum / scale) {
        return std::chrono::milliseconds{std::numeric_limits<Rep>::max()};
    }
    const std::uint64_t whole = quotient * scale;
    const std::uint64_t fractional = (remainder * scale) / denominator;
    const std::uint64_t scaled = whole > maximum - fractional ? maximum : whole + fractional;
    return std::chrono::milliseconds{static_cast<Rep>(scaled)};
}

std::chrono::milliseconds scale_fraction(std::chrono::milliseconds duration,
                                         std::uint64_t numerator,
                                         std::uint64_t denominator) noexcept {
    if (duration <= std::chrono::milliseconds::zero() || numerator == 0) {
        return std::chrono::milliseconds::zero();
    }
    using Rep = std::chrono::milliseconds::rep;
    const std::uint64_t count = static_cast<std::uint64_t>(duration.count());
    const std::uint64_t maximum = static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
    if (count > maximum / numerator) {
        return std::chrono::milliseconds{std::numeric_limits<Rep>::max()};
    }
    return std::chrono::milliseconds{static_cast<Rep>((count * numerator) / denominator)};
}

std::chrono::milliseconds three_quarters(std::chrono::milliseconds duration) noexcept {
    return scale_fraction(duration, 3, 4);
}

std::chrono::milliseconds saturating_add(std::chrono::milliseconds left,
                                         std::chrono::milliseconds right) noexcept {
    using Rep = std::chrono::milliseconds::rep;
    const Rep left_count = left.count();
    const Rep right_count = right.count();
    const Rep maximum = std::numeric_limits<Rep>::max();
    const Rep minimum = std::numeric_limits<Rep>::lowest();

    if (right_count > 0 && left_count > maximum - right_count) {
        return std::chrono::milliseconds{maximum};
    }
    if (right_count < 0 && left_count < minimum - right_count) {
        return std::chrono::milliseconds{minimum};
    }
    return std::chrono::milliseconds{left_count + right_count};
}

std::chrono::milliseconds nonnegative_difference(std::chrono::milliseconds left,
                                                 std::chrono::milliseconds right) noexcept {
    return left > right ? left - right : std::chrono::milliseconds::zero();
}

std::uint32_t continuous_horizon(const bool has_increment, const int game_phase,
                                   const std::uint16_t fullmove) noexcept {
    if (game_phase < 0 || fullmove == 0) {
        return has_increment ? 28U : 50U;
    }
    const int phase = std::clamp(game_phase, 0, 24);
    if (has_increment) {
        const std::uint32_t mid_horizon = static_cast<std::uint32_t>(
            28 + ((12 - std::min(phase, 12)) * 5 + 6) / 12);
        const std::uint32_t end_horizon = phase <= 6 ? 34U : 32U;
        if (fullmove <= 8) {
            return 36U;
        }
        if (fullmove < 14) {
            return 36U - ((36U - mid_horizon) * (fullmove - 8U) + 3U) / 6U;
        }
        if (fullmove <= 30) {
            return mid_horizon;
        }
        if (fullmove < 42) {
            const std::uint32_t delta = end_horizon >= mid_horizon
                ? end_horizon - mid_horizon : 0U;
            return mid_horizon + (delta * (fullmove - 30U) + 6U) / 12U;
        }
        return end_horizon;
    }

    const std::uint32_t mid_horizon = static_cast<std::uint32_t>(
        46 + ((12 - std::clamp(phase, 6, 12)) * 4 + 3) / 6);
    const std::uint32_t end_horizon = phase <= 6 ? 42U : 48U;
    if (fullmove <= 8) {
        return 56U;
    }
    if (fullmove < 14) {
        return 56U - ((56U - mid_horizon) * (fullmove - 8U) + 3U) / 6U;
    }
    if (fullmove <= 32) {
        return mid_horizon;
    }
    if (fullmove < 44) {
        if (end_horizon >= mid_horizon) {
            return mid_horizon + ((end_horizon - mid_horizon) * (fullmove - 32U) + 6U) / 12U;
        }
        return mid_horizon - ((mid_horizon - end_horizon) * (fullmove - 32U) + 6U) / 12U;
    }
    return end_horizon;
}

} // namespace

TimeManager::TimeManager(SearchLimits limits, Color side_to_move, std::uint8_t speed_percent,
                         std::uint32_t move_overhead_ms, std::uint32_t slow_mover_percent,
                         bool dynamic_move_overhead)
    : TimeManager(std::move(limits), side_to_move, speed_percent, move_overhead_ms,
                  slow_mover_percent, {}, {}, dynamic_move_overhead) {}

TimeManager::TimeManager(SearchLimits limits, Color side_to_move, std::uint8_t speed_percent,
                         std::uint32_t move_overhead_ms, std::uint32_t slow_mover_percent,
                         RootTimingContext root_context,
                         TimePointProvider now, bool dynamic_move_overhead,
                         std::chrono::steady_clock::time_point search_started,
                         std::uint32_t observed_overhead_ms)
    : limits_(std::move(limits)), root_context_(root_context), now_(std::move(now)) {
    initialize(side_to_move, speed_percent, move_overhead_ms, slow_mover_percent,
               dynamic_move_overhead, observed_overhead_ms, resolve_start_ns(search_started));
}

void TimeManager::reconfigure(const SearchLimits& limits, Color side_to_move,
                              std::uint8_t speed_percent, std::uint32_t move_overhead_ms,
                              std::uint32_t slow_mover_percent,
                              const RootTimingContext& root_context,
                              bool dynamic_move_overhead,
                              std::chrono::steady_clock::time_point search_started,
                              std::uint32_t observed_overhead_ms) {
    const bool preserve_ponder_observations =
        limits_.ponder && !limits.ponder && !limits.infinite;
    limits_ = limits;
    root_context_ = root_context;
    initialize(side_to_move, speed_percent, move_overhead_ms, slow_mover_percent,
               dynamic_move_overhead, observed_overhead_ms, resolve_start_ns(search_started),
               preserve_ponder_observations);
}

std::int64_t TimeManager::resolve_start_ns(
    const std::chrono::steady_clock::time_point search_started) const noexcept {
    std::int64_t start_ns = now_nanoseconds();
    if (!now_ && search_started != std::chrono::steady_clock::time_point{}) {
        const std::int64_t supplied_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                search_started.time_since_epoch())
                .count();
        start_ns = std::min(start_ns, supplied_ns);
    }
    return start_ns;
}

void TimeManager::initialize(Color side_to_move, std::uint8_t speed_percent,
                             std::uint32_t move_overhead_ms, std::uint32_t slow_mover_percent,
                             bool dynamic_move_overhead,
                             std::uint32_t observed_overhead_ms, std::int64_t start_ns,
                             const bool preserve_observations) {
    const auto publish_controls = [this, start_ns](const std::int64_t next_budget_ms) noexcept {
        // Publish computed limits in release order before flipping unbounded_
        // off so concurrent SMP helper checks never see a transient -1 budget
        // or stale start timestamp during ponderhit reconfiguration.
        if (!limits_.nodes.has_value()) {
            has_node_limit_.store(false, std::memory_order_release);
        }
        node_limit_.store(limits_.nodes.value_or(0), std::memory_order_relaxed);
        has_node_limit_.store(limits_.nodes.has_value(), std::memory_order_release);
        hard_deadline_reached_.store(false, std::memory_order_relaxed);
        started_ns_.store(start_ns, std::memory_order_relaxed);
        budget_ms_.store(next_budget_ms, std::memory_order_release);
        unbounded_.store(limits_.infinite || limits_.ponder, std::memory_order_release);
    };

    dynamic_move_overhead_ = dynamic_move_overhead;
    timing_ = TimeManagementStats{};
    if (!preserve_observations) {
        previous_score_.reset();
        average_score_.reset();
        stable_observations_ = 0;
        best_move_stability_ = 0;
        instability_permille_ = 0;
        mate_stable_observations_ = 0;
        last_mate_sign_ = 0;
        last_observed_depth_ = 0;
        score_drop_cp_ = 0;
        score_gain_cp_ = 0;
        best_move_effort_permille_ = 0;
        last_iteration_elapsed_ = std::chrono::milliseconds::zero();
        has_effort_sample_ = false;
        last_aspiration_researched_ = false;
        hard_position_ = false;
    }
    remaining_clock_ = std::chrono::milliseconds::zero();
    increment_clock_ = std::chrono::milliseconds::zero();
    clock_mode_ = false;
    emergency_pacing_ = false;

    timing_.initial_hardness = initial_hardness(root_context_);
    timing_.observed_hardness = timing_.initial_hardness;
    if (!preserve_observations || last_observed_depth_ == 0) {
        hard_position_ = timing_.initial_hardness >= 35;
    } else if (hard_position_) {
        timing_.observed_hardness = std::max(timing_.observed_hardness, 100);
    }
    timing_.extended_for_hard_position = hard_position_;
    timing_.horizon = std::max<std::uint32_t>(1, limits_.moves_to_go.value_or(20));

    // Explicit depth and node limits are strict upper bounds, but they must not
    // disable the clock guard: a GUI may send `go depth N wtime ...` (or mix a
    // node budget with clocks) and still expect the engine to answer inside the
    // remaining clock. Only an unbounded search has no deadline. A bare
    // `go depth N` / `go nodes N` remains untimed because it carries neither a
    // movetime nor a clock and returns at the clock check below.
    if (limits_.infinite || limits_.ponder) {
        publish_controls(-1);
        return;
    }

    if (limits_.movetime.has_value()) {
        // Slow Mover tunes how much of a game clock the engine spends per move;
        // an explicit movetime is a direct request from the GUI, so only Speed
        // scales it and the final budget never exceeds the request.  Move
        // Overhead exists to keep a late move from flagging a game clock; a
        // movetime search has no clock to protect, so subtracting it only
        // wastes the requested analysis time.  Keep the safety margin (it
        // covers teardown/transmission), not the GUI overhead.
        const auto scaled = scale_duration(*limits_.movetime, speed_percent);
        const auto movetime_budget = std::min(*limits_.movetime,
                                              with_safety_margin(scaled));
        timing_.usable = movetime_budget;
        timing_.soft_budget = movetime_budget;
        timing_.optimum_budget = movetime_budget;
        timing_.hard_budget = movetime_budget;
        timing_.overhead = std::chrono::milliseconds{
            static_cast<std::chrono::milliseconds::rep>(move_overhead_ms)};
        publish_controls(movetime_budget.count());
        return;
    }

    const std::optional<ClockLimit>& clock = side_to_move == Color::white ?
        limits_.white_clock : limits_.black_clock;
    if (!clock.has_value()) {
        publish_controls(-1);
        return;
    }

    clock_mode_ = true;
    const auto remaining = std::max(clock->remaining, std::chrono::milliseconds::zero());
    remaining_clock_ = remaining;
    increment_clock_ = clock->increment;
    std::uint32_t effective_overhead_ms = std::max(move_overhead_ms, observed_overhead_ms);
    if (dynamic_move_overhead_) {
        // Scale proportionally on bullet clocks (2% of the remaining clock,
        // 10..100 ms), while never undercutting observed GUI/network latency
        // or an explicit Move Overhead configured above the 30 ms default.
        const std::int64_t derived =
            std::clamp<std::int64_t>(remaining.count() / 50, 10, 100);
        const std::uint32_t floor_ms = std::max<std::uint32_t>(
            observed_overhead_ms, move_overhead_ms > 30U ? move_overhead_ms : 0U);
        effective_overhead_ms = std::max(static_cast<std::uint32_t>(derived), floor_ms);
    }
    const auto overhead = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(std::min<std::uint64_t>(
            effective_overhead_ms, static_cast<std::uint64_t>(
                std::numeric_limits<std::chrono::milliseconds::rep>::max())))};
    timing_.overhead = overhead;
    // Horizon: how many moves this allocation plans for.  A GUI may send
    // `movestogo`, but lichess-bot and most GUIs do not, so the fallback decides
    // the pacing.  A piecewise-linear ply/phase curve avoids sudden-cliff drops
    // while pacing the opening calmly, opening up in complex middlegames, and
    // reacting smoothly to large opponent-clock asymmetries.
    const bool has_increment = clock->increment > std::chrono::milliseconds::zero();
    std::uint32_t default_horizon = continuous_horizon(
        has_increment, root_context_.game_phase, root_context_.fullmove_number);
    if (!limits_.moves_to_go.has_value()) {
        const std::optional<ClockLimit>& opponent_clock = side_to_move == Color::white ?
            limits_.black_clock : limits_.white_clock;
        if (opponent_clock.has_value()) {
            const auto opp_remaining = std::max(
                opponent_clock->remaining, std::chrono::milliseconds::zero());
            if (remaining < opp_remaining / 2 &&
                remaining < std::chrono::milliseconds{60'000} &&
                opp_remaining >= std::chrono::milliseconds{10'000} &&
                clock->increment <= std::chrono::milliseconds{2'000}) {
                default_horizon = (default_horizon * 115U + 50U) / 100U;
            } else if (opp_remaining > std::chrono::milliseconds::zero() &&
                       remaining > saturating_add(opp_remaining, opp_remaining) &&
                       remaining >= std::chrono::milliseconds{30'000}) {
                const std::uint32_t floor_h = has_increment ? 22U : 38U;
                default_horizon = std::max(
                    floor_h, (default_horizon * 92U + 50U) / 100U);
            }
        }
    }
    timing_.horizon = std::max<std::uint32_t>(
        1, limits_.moves_to_go.value_or(default_horizon));
    // Always protect at least two effective move overheads (unbounded by the
    // 750 ms proportional reserve cap) so high-latency network links keep a real
    // buffer before flagging.
    const auto two_overheads = saturating_add(overhead, overhead);
    const auto proportional_reserve = std::min(
        std::chrono::milliseconds{750},
        std::max(std::chrono::milliseconds{50}, scale_duration(remaining, 3)));
    timing_.reserve = std::min(remaining, std::max(proportional_reserve, two_overheads));
    timing_.usable = nonnegative_difference(remaining, timing_.reserve);
    emergency_pacing_ = remaining <= std::chrono::milliseconds{3000};
    timing_.emergency_pacing = emergency_pacing_;

    const auto base_allocation = timing_.usable / timing_.horizon;
    // The increment credit only spends net post-overhead increment when low on
    // time (<= 15 increments remaining) and is capped at a quarter of usable
    // time, preventing fast incremental endgames from bleeding past zero.
    const auto net_increment = nonnegative_difference(clock->increment, overhead);
    const auto low_inc_threshold = scale_fraction(clock->increment, 15, 1);
    const auto raw_inc_credit =
        (!limits_.moves_to_go.has_value() && has_increment && remaining <= low_inc_threshold)
            ? three_quarters(net_increment)
            : three_quarters(clock->increment);
    const auto increment_credit = std::min(raw_inc_credit,
                                           scale_fraction(timing_.usable, 1, 4));
    const auto raw_soft = saturating_add(base_allocation, increment_credit);
    // A single unstable move may only take two base units.  The old increment
    // case allowed three, which let a 10+2 online game burn 70+ seconds twice in
    // a row (two-thirds of the move's clock) before the GUI round trip was even
    // paid; the increment is refunded over subsequent moves, not inside this
    // one.  Emergency pacing still tightens the low-clock case separately.
    const auto hard_units = remaining <= std::chrono::milliseconds{3000} ? 3 : 2;
    const auto raw_hard = saturating_add(
        scale_fraction(base_allocation, hard_units, 1),
        scale_fraction(increment_credit, 3, 2));
    const auto scale_budget = [slow_mover_percent, speed_percent, this](
                                  std::chrono::milliseconds duration) {
        const auto slow = scale_duration(duration, slow_mover_percent);
        return std::min(timing_.usable, scale_duration(slow, speed_percent));
    };
    timing_.soft_budget = scale_budget(std::min(raw_soft, timing_.usable));
    timing_.hard_budget = scale_budget(std::min(raw_hard, timing_.usable));
    timing_.soft_budget = std::min(timing_.soft_budget, timing_.hard_budget);

    // A three-move extension is useful with a normal clock, but it is unsafe
    // when the entire remaining clock is only a few seconds. Pace low-clock
    // searches tightly while giving verified hard positions a bounded extension.
    if (emergency_pacing_) {
        const std::uint32_t emergency_horizon = std::max<std::uint32_t>(
            10, timing_.horizon / 2);
        const auto emergency_base = timing_.usable / emergency_horizon;
        const auto emergency_soft = std::min(
            timing_.usable, saturating_add(emergency_base, increment_credit));
        const auto emergency_extension = hard_position_
            ? std::max(std::chrono::milliseconds{15}, emergency_base * 2)
            : std::max(std::chrono::milliseconds{5}, emergency_base / 2);
        const auto emergency_hard = std::min(
            timing_.usable, saturating_add(emergency_soft, emergency_extension));
        timing_.soft_budget = std::min(timing_.soft_budget, emergency_soft);
        timing_.hard_budget = std::min(timing_.hard_budget, emergency_hard);
        timing_.hard_budget = std::max(timing_.hard_budget, timing_.soft_budget);
    }

    // Flag-proof ceiling. Every heuristic above (increment credit, position
    // hardness, a skipped emergency reduction) can raise the hard budget up to
    // the whole usable clock, which would leave only the reserve between the
    // engine and a flag once the teardown and the GUI round trip are paid for.
    // These caps are applied last so nothing can bypass them: a single move may
    // only ever spend a quarter of the usable clock, and it must always leave a
    // safety margin that covers the move overhead and search teardown.
    const auto hard_safety = std::max(
        saturating_add(overhead, saturating_add(overhead, std::chrono::milliseconds{15})),
        std::min(std::chrono::milliseconds{50}, remaining / 20));
    const auto usable_ceiling = nonnegative_difference(timing_.usable, hard_safety);
    const auto fraction_ceiling = std::max(std::chrono::milliseconds{5},
                                           scale_fraction(timing_.usable, 1, 4));
    const auto capped_hard = std::min(std::min(timing_.hard_budget, fraction_ceiling),
                                      usable_ceiling);
    // A one-millisecond floor keeps the deadline comparable and still fires on
    // the first stop check rather than never.
    timing_.hard_budget = std::max(std::chrono::milliseconds{1}, capped_hard);
    timing_.soft_budget = std::min(timing_.soft_budget, timing_.hard_budget);
    timing_.optimum_budget = timing_.soft_budget;
    if (root_context_.legal_move_count == 1) {
        timing_.optimum_budget = std::min(
            timing_.soft_budget,
            std::max(std::chrono::milliseconds{2}, timing_.soft_budget / 8));
    }
    if (preserve_observations && last_observed_depth_ > 0) {
        update_optimum_budget();
    }
    publish_controls(timing_.hard_budget.count());
}

std::chrono::steady_clock::time_point TimeManager::now() const noexcept {
    if (!now_) {
        return std::chrono::steady_clock::now();
    }
    try {
        return now_();
    } catch (...) {
        return std::chrono::steady_clock::now();
    }
}

std::int64_t TimeManager::now_nanoseconds() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now().time_since_epoch()).count();
}

int TimeManager::initial_hardness(const RootTimingContext& context) noexcept {
    int hardness = 0;
    // A cold TT means "new position", not "hard position".  The old weights
    // summed to 80 for a plain root TT miss, which latched every normal move
    // into the hard class and made the soft budget unreachable.
    if (!context.table_available || !context.tt_hit) {
        hardness += 5;
    }
    if (!context.tt_exact) {
        hardness += 5;
    }
    if (context.tt_depth < 6) {
        hardness += 5;
    }
    if (context.tt_generation_age > 2) {
        hardness += 5;
    }
    if (!context.tt_has_best_move) {
        hardness += 5;
    }
    if (context.in_check) {
        hardness += 15;
    }
    if (context.forcing_move_count > 0 &&
        context.forcing_move_count * 2 >= context.legal_move_count) {
        hardness += 10;
    }
    if (context.legal_move_count >= 30) {
        hardness += 5;
    }
    if (context.tt_hit && context.tt_exact && context.tt_depth >= 8 &&
        context.tt_generation_age <= 1) {
        hardness -= 25;
    }
    return std::clamp(hardness, 0, 100);
}

std::optional<std::chrono::milliseconds> TimeManager::time_budget() const noexcept {
    const std::int64_t budget_ms = budget_ms_.load(std::memory_order_relaxed);
    if (budget_ms < 0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{budget_ms};
}

std::optional<std::uint64_t> TimeManager::node_limit() const noexcept {
    if (!has_node_limit_.load(std::memory_order_relaxed)) {
        return std::nullopt;
    }
    return node_limit_.load(std::memory_order_relaxed);
}

bool TimeManager::should_stop(std::uint64_t nodes) const noexcept {
    if (has_node_limit_.load(std::memory_order_acquire) &&
        nodes >= node_limit_.load(std::memory_order_relaxed)) {
        return true;
    }
    if (unbounded_.load(std::memory_order_acquire)) {
        return false;
    }
    const std::int64_t budget_ms = budget_ms_.load(std::memory_order_acquire);
    if (budget_ms < 0) {
        return false;
    }
    const std::int64_t elapsed_ms =
        (now_nanoseconds() - started_ns_.load(std::memory_order_relaxed)) / 1'000'000;
    if (elapsed_ms >= budget_ms) {
        hard_deadline_reached_.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void TimeManager::observe_iteration(const SearchIterationObservation& observation) noexcept {
    last_observed_depth_ = std::max(last_observed_depth_, observation.depth);
    last_aspiration_researched_ = observation.aspiration_researched;
    if (observation.iteration_elapsed > std::chrono::milliseconds::zero()) {
        last_iteration_elapsed_ = observation.iteration_elapsed;
    }

    if (observation.depth >= 4) {
        instability_permille_ /= 2U;
        if (observation.best_move_changed) {
            instability_permille_ = std::min<std::uint32_t>(4000U, instability_permille_ + 1000U);
        }
    }

    if (std::abs(observation.score_cp) >= detail::kMateThreshold) {
        const int sign = observation.score_cp > 0 ? 1 : -1;
        if (sign == last_mate_sign_ && !observation.best_move_changed) {
            mate_stable_observations_ = std::min<std::uint32_t>(8U, mate_stable_observations_ + 1U);
        } else {
            last_mate_sign_ = sign;
            mate_stable_observations_ = 1U;
        }
    } else {
        last_mate_sign_ = 0;
        mate_stable_observations_ = 0U;
    }

    bool score_swing = false;
    long long delta = 0;
    if (previous_score_.has_value()) {
        delta = static_cast<long long>(observation.score_cp) -
            static_cast<long long>(*previous_score_);
        score_swing = std::llabs(delta) >= 75;
    }

    if (observation.best_move_changed) {
        best_move_stability_ = 0;
    } else {
        best_move_stability_ = std::min<std::uint32_t>(8, best_move_stability_ + 1);
    }

    const long long drop_from_prev_move =
        (observation.depth >= 4 && root_context_.previous_search_score.has_value())
            ? std::max<long long>(
                  0, static_cast<long long>(*root_context_.previous_search_score) -
                         static_cast<long long>(observation.score_cp)) / 2
            : 0;

    if (!average_score_.has_value()) {
        average_score_ = observation.score_cp;
        score_drop_cp_ = static_cast<int>(std::clamp<long long>(drop_from_prev_move, 0, 400));
        score_gain_cp_ = 0;
    } else {
        const long long drop_from_prev = previous_score_.has_value() ?
            std::max<long long>(0, static_cast<long long>(*previous_score_) -
                                    static_cast<long long>(observation.score_cp)) : 0;
        const long long drop_from_avg = std::max<long long>(
            0, static_cast<long long>(*average_score_) -
               static_cast<long long>(observation.score_cp));
        score_drop_cp_ = static_cast<int>(std::clamp<long long>(
            std::max({drop_from_prev, drop_from_avg, drop_from_prev_move}), 0, 400));
        score_gain_cp_ = static_cast<int>(std::clamp<long long>(
            std::max<long long>(0, delta), 0, 400));
        average_score_ = static_cast<int>(
            (static_cast<long long>(*average_score_) * 2 +
             static_cast<long long>(observation.score_cp)) / 3);
    }

    if (observation.depth >= 5 && observation.nodes > 0 &&
        observation.best_move_nodes > 0 &&
        observation.best_move_nodes <= observation.nodes) {
        const std::uint32_t permille = static_cast<std::uint32_t>(
            std::clamp<std::uint64_t>(
                (observation.best_move_nodes * 1000ULL) / observation.nodes, 0ULL, 1000ULL));
        if (!has_effort_sample_) {
            best_move_effort_permille_ = permille;
            has_effort_sample_ = true;
        } else {
            best_move_effort_permille_ = (best_move_effort_permille_ + permille * 2U) / 3U;
        }
    }

    // PV growth is expected on every deeper iteration; treating the full-vector
    // inequality as instability made `stable_observations_` unable to reach 2.
    const bool hard_evidence = observation.best_move_changed || score_swing ||
        observation.aspiration_researched;
    if (hard_evidence) {
        // The stop deadline is fixed when the search starts and the safety
        // ceilings were applied there. Hard evidence must therefore not raise
        // the hard budget here; it only feeds diagnostics and iteration pacing.
        stable_observations_ = 0;
        timing_.extended_for_hard_position = true;
        timing_.observed_hardness = std::max(timing_.observed_hardness, 100);
        // Shallow depths (<= 3) routinely swap best moves and re-search narrow
        // windows while move ordering warms up. Only treat deep instability or
        // a genuine score drop as latching hard-position evidence; shallow
        // turbulence is absorbed by the continuous optimum_budget scaling.
        if (observation.depth >= 4 &&
            (observation.best_move_changed || score_drop_cp_ >= 50 ||
             (observation.aspiration_researched && score_drop_cp_ >= 25))) {
            hard_position_ = true;
        }
    } else if (previous_score_.has_value()) {
        if (!observation.best_move_changed && std::llabs(delta) <= 25 &&
            !observation.aspiration_researched) {
            ++stable_observations_;
            if (stable_observations_ >= 2) {
                // Recent iterations are stable, so the position is no longer
                // treated as hard and the dynamic soft budget may end the search.
                hard_position_ = false;
            }
        } else {
            stable_observations_ = 0;
        }
    } else {
        stable_observations_ = 1;
    }
    previous_score_ = observation.score_cp;
    update_optimum_budget();
}

void TimeManager::update_optimum_budget() noexcept {
    if (!clock_mode_) {
        timing_.optimum_budget = timing_.soft_budget;
        return;
    }
    if (root_context_.legal_move_count == 1) {
        timing_.optimum_budget = std::min(
            timing_.soft_budget,
            std::max(std::chrono::milliseconds{2}, timing_.soft_budget / 8));
        return;
    }
    if (mate_stable_observations_ >= 2 && last_observed_depth_ >= 4) {
        timing_.optimum_budget = std::min(
            timing_.soft_budget,
            std::max(std::chrono::milliseconds{20}, timing_.soft_budget / 6));
        return;
    }

    // Dynamic soft-budget scaling (in permille, 1000 = 1.00x soft_budget).
    // Preserves the synthetic unit-test contract when no best-move node effort
    // is supplied, while scaling real searches continuously by best-move
    // stability, decayed cumulative instability, node-effort concentration,
    // winning-endgame conversion speedup, and falling-vs-rising evaluation
    // asymmetry.
    std::uint64_t scale_permille = 1000;
    if (last_observed_depth_ >= 4) {
        static constexpr std::uint32_t kStabilityPermille[6] = {
            1200, 1040, 960, 880, 800, 720
        };
        const std::size_t stability_idx =
            std::min<std::size_t>(best_move_stability_, 5);
        scale_permille = (scale_permille * kStabilityPermille[stability_idx]) / 1000;
        if (instability_permille_ > 0) {
            const std::uint64_t instability_bonus = 1000ULL + (instability_permille_ * 200ULL) / 1000ULL;
            scale_permille = (scale_permille * instability_bonus) / 1000;
        }
    }

    if (has_effort_sample_ && last_observed_depth_ >= 5) {
        // High effort on the best move (e.g., 900 permille) means all sibling
        // root moves failed low quickly -> scale down strongly. Low effort on
        // the best move (e.g., 250 permille) means heavy competition among
        // alternatives -> scale up.
        const std::uint64_t effort_factor = static_cast<std::uint64_t>(
            std::clamp<int>(1480 - static_cast<int>(best_move_effort_permille_ * 82 / 100),
                            660, 1380));
        scale_permille = (scale_permille * effort_factor) / 1000;
    } else if (stable_observations_ >= 2 && score_drop_cp_ <= 15 && !last_aspiration_researched_) {
        // Without a node-effort sample, cap stable quiet iterations at 1.00x
        // soft_budget so synthetic unit tests stop cleanly at soft_budget.
        scale_permille = std::min<std::uint64_t>(scale_permille, 1000);
    }

    if (score_drop_cp_ > 15) {
        // Falling evaluation demands extra defensive verification time, whereas
        // a rising score only needs a modest check if the jump was tactical.
        const std::uint64_t drop_bonus = static_cast<std::uint64_t>(
            std::min(550, (score_drop_cp_ - 15) * 4));
        scale_permille = (scale_permille * (1000 + drop_bonus)) / 1000;
    } else if (score_gain_cp_ >= 80) {
        scale_permille = (scale_permille * 1060) / 1000;
    }

    if (last_aspiration_researched_) {
        scale_permille = (scale_permille * 1120) / 1000;
    }

    // Winning-position conversion speedup: when clearly ahead (>= 500 cp) with
    // a stable deep best move and no score collapse, move faster — especially
    // once the clock is below 20 seconds — so decided games never flag.
    if (previous_score_.has_value() && std::abs(*previous_score_) >= 500 &&
        last_observed_depth_ >= 8 && best_move_stability_ >= 2 &&
        score_drop_cp_ <= 25 && !last_aspiration_researched_) {
        const std::uint64_t conversion_scale =
            remaining_clock_ <= std::chrono::milliseconds{20'000} ? 580ULL : 760ULL;
        scale_permille = (scale_permille * conversion_scale) / 1000;
    }

    scale_permille = std::clamp<std::uint64_t>(scale_permille, 380, 1850);
    const auto scaled = scale_fraction(timing_.soft_budget, scale_permille, 1000);
    const auto floor_budget = std::max(
        std::chrono::milliseconds{1}, timing_.soft_budget / 3);
    timing_.optimum_budget = std::clamp(scaled, floor_budget, timing_.hard_budget);
}

std::chrono::milliseconds TimeManager::effective_soft_budget() const noexcept {
    return timing_.optimum_budget > std::chrono::milliseconds::zero() ?
        timing_.optimum_budget : timing_.soft_budget;
}

bool TimeManager::hard_extension_affordable() const noexcept {
    const auto elapsed = std::chrono::milliseconds{
        (now_nanoseconds() - started_ns_.load(std::memory_order_relaxed)) / 1'000'000};
    const auto remaining = nonnegative_difference(timing_.usable, elapsed);
    // Compare the *current* clock against two soft budgets plus the teardown
    // overhead: the earlier `remaining >= 4*hard` form was scale-invariant
    // because both sides shrank with the same construction-time `usable`
    // snapshot, so it stayed true for the whole move and the taper never fired.
    const auto required = saturating_add(scale_fraction(timing_.soft_budget, 2, 1),
                                         timing_.overhead);
    return remaining >= required;
}

bool TimeManager::should_stop_after_iteration() const noexcept {
    const std::int64_t budget_ms = budget_ms_.load(std::memory_order_relaxed);
    if (unbounded_.load(std::memory_order_relaxed) || budget_ms < 0) {
        return false;
    }
    const auto elapsed = std::chrono::milliseconds{
        (now_nanoseconds() - started_ns_.load(std::memory_order_relaxed)) / 1'000'000};
    if (elapsed >= std::chrono::milliseconds{budget_ms}) {
        hard_deadline_reached_.store(true, std::memory_order_relaxed);
        return true;
    }
    const auto soft_target = effective_soft_budget();
    if (clock_mode_ &&
        ((root_context_.legal_move_count == 1 && last_observed_depth_ >= 4) ||
         (mate_stable_observations_ >= 2 && last_observed_depth_ >= 4)) &&
        elapsed >= soft_target) {
        return true;
    }
    // At a low clock the cold-TT "hard" classification is intentionally not
    // trusted; stop at soft only when the clock can no longer afford the
    // extension (a root forcing extension / worker startup needs the hard
    // budget even in a quiet-looking position).
    if (emergency_pacing_ && !hard_extension_affordable() &&
        elapsed >= soft_target) {
        return true;
    }
    if (!clock_mode_) {
        return false;
    }
    if (stable_observations_ >= 2 && (!hard_position_ || !hard_extension_affordable()) &&
        elapsed >= soft_target) {
        return true;
    }
    // Even if shallow iterations had a best-move change or score jump, do not
    // let a non-collapsing position burn all the way to the hard deadline once
    // it has completed a reasonable depth and passed its scaled optimum target.
    if (last_observed_depth_ >= 5 && best_move_stability_ >= 1 &&
        score_drop_cp_ < 40 && elapsed >= soft_target) {
        return true;
    }
    return false;
}

bool TimeManager::should_start_next_iteration(std::chrono::milliseconds estimated_next_iteration) const noexcept {
    const std::int64_t budget_ms = budget_ms_.load(std::memory_order_relaxed);
    if (unbounded_.load(std::memory_order_relaxed) || budget_ms < 0) {
        return true;
    }
    const auto elapsed = std::chrono::milliseconds{
        (now_nanoseconds() - started_ns_.load(std::memory_order_relaxed)) / 1'000'000};
    const auto soft_target = effective_soft_budget();
    if (emergency_pacing_ && !hard_extension_affordable() &&
        elapsed >= soft_target) {
        return false;
    }
    if (clock_mode_ &&
        ((root_context_.legal_move_count == 1 && last_observed_depth_ >= 4) ||
         (mate_stable_observations_ >= 2 && last_observed_depth_ >= 4)) &&
        elapsed >= soft_target) {
        return false;
    }
    // Pre-iteration lookahead for stable clock searches: when we have already
    // spent >= 75% of the dynamic soft target (or when the previous iteration
    // duration indicates a 1.5x next ply would blow past 1.2x soft_target),
    // skip starting a doomed partial iteration.
    if (clock_mode_ && !hard_position_) {
        if (last_observed_depth_ >= 6 && stable_observations_ >= 2 &&
            best_move_stability_ >= 3 && score_drop_cp_ <= 15 &&
            elapsed >= scale_fraction(soft_target, 75, 100)) {
            return false;
        }
        if (last_observed_depth_ >= 5 && best_move_stability_ >= 2 &&
            score_drop_cp_ <= 20 && !last_aspiration_researched_ &&
            last_iteration_elapsed_ > std::chrono::milliseconds::zero()) {
            const auto predicted_next = saturating_add(
                last_iteration_elapsed_, last_iteration_elapsed_ / 2);
            if (elapsed >= scale_fraction(soft_target, 55, 100) &&
                saturating_add(elapsed, predicted_next) >
                    scale_fraction(soft_target, 120, 100)) {
                return false;
            }
        }
    }
    const auto budget = std::chrono::milliseconds{budget_ms};
    const auto needed = std::max(estimated_next_iteration, std::chrono::milliseconds{1});
    return elapsed < budget && saturating_add(elapsed, needed) < budget;
}

TimeManagementStats TimeManager::diagnostics() const noexcept {
    TimeManagementStats result = timing_;
    result.hard_deadline_reached = hard_deadline_reached_.load(std::memory_order_relaxed);
    return result;
}

} // namespace koi
