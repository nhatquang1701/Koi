#pragma once

// Position-invariant inputs of the per-move check probe.  A legal move list is
// probed against one unchanged board, so the enemy king bitboard, the combined
// occupancy, and the mover's slider bitboards are read once per list instead of
// once per move.

#include <cstdint>

#include "koi/move.hpp"

namespace koi::detail {

// Position-invariant inputs of the per-move check probe.  A legal move list is
// probed against one unchanged board, so the enemy king bitboard, the combined
// occupancy, and the mover's slider bitboards are read once per list instead of
// once per move.
struct NativeCheckProbe {
    std::uint64_t enemy_king = 0;
    std::uint64_t occupied = 0;
    std::uint64_t friendly_rook_queen = 0;
    std::uint64_t friendly_bishop_queen = 0;
    // Open-board (empty-occupancy) union of the enemy king's rook and bishop
    // rays: the conservative superset of every square from which a sliding
    // attack could reach the king and of every square a king slider line can
    // pass through.  Zero means the rays were not computed (a default probe on
    // a non-probing path); the per-move probe then runs the full computation
    // instead of trusting the prefilter.
    std::uint64_t king_slider_rays = 0;
    Color moving_color = Color::white;
};

} // namespace koi::detail
