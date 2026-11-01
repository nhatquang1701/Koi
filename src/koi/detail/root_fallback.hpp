#pragma once

// Bounded root-fallback heuristics.
//
// The serial and root-parallel drivers share these emergency scanners: when a
// short search cannot complete an authoritative iteration, a legal root move is
// chosen by a bounded static/one-ply safety pass.  They are kept in their own
// unit so the driver (`search_runner`) owns flow and this unit owns the
// correction policy.  Behavior is preserved verbatim from the original
// anonymous-namespace cluster.

#include <atomic>
#include <mutex>
#include <optional>

#include "koi/detail/search_constants.hpp"
#include "koi/game_state.hpp"
#include "koi/search_types.hpp"

namespace koi {

class Evaluator;
class TimeManager;

namespace detail {

struct FallbackControl {
    const TimeManager* time_manager = nullptr;
    const std::atomic_bool* stop_requested = nullptr;

    [[nodiscard]] bool interrupted() const noexcept;
};

struct ShortFallbackChoice {
    Move move = Move::no_move();
    int score = -kInfinity;
};

[[nodiscard]] bool root_move_is_broad_quiet_check(const GameState& root,
                                                 const MoveMetadata& metadata) noexcept;
[[nodiscard]] std::optional<Move> first_safe_short_search_move(
    const GameState& root, const MoveMetadataList& legal_moves) noexcept;
[[nodiscard]] bool short_quiet_needs_fallback(const GameState& root,
                                              const MoveMetadataList& legal_moves,
                                              const std::optional<Move>& candidate) noexcept;
[[nodiscard]] std::optional<ShortFallbackChoice> scored_safe_checking_capture(
    const GameState& root, const MoveMetadataList& legal_moves, const Evaluator& evaluator,
    std::mutex* evaluator_mutex, bool allow_deep_fallback,
    const FallbackControl* control = nullptr) noexcept;
[[nodiscard]] std::optional<ShortFallbackChoice> short_search_fallback_move(
    const GameState& root, const MoveMetadataList& legal_moves, const Evaluator& evaluator,
    std::mutex* evaluator_mutex, SearchStats* stats = nullptr,
    bool allow_deep_checked_fallback = true, const FallbackControl* control = nullptr,
    bool permit_overdue_shallow = false, bool reject_hanging_major_checks = false) noexcept;
[[nodiscard]] bool short_fallback_should_replace_completed_root(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& current_move, Move fallback_move) noexcept;
[[nodiscard]] bool short_capture_needs_fallback(const MoveMetadataList& legal_moves,
                                                const std::optional<Move>& candidate) noexcept;
[[nodiscard]] bool short_check_needs_fallback(const MoveMetadataList& legal_moves,
                                              const std::optional<Move>& candidate) noexcept;
[[nodiscard]] bool short_quiet_exposes_immediate_check(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept;
[[nodiscard]] bool short_checked_root_king_evasion_needs_fallback(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept;

} // namespace detail
} // namespace koi
