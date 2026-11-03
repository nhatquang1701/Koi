#pragma once

// Old tuned king PSTs (value-only reuse, same repository licence) as a focused
// candidate; the rewrite's own tables remain in place for revert.

#include <array>

namespace koi::detail::hce_legacy_king {

inline constexpr std::array<int, 64> kKingMiddleGame{
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -20, -30, -30, -40, -40, -30, -30, -20,
    -10, -20, -20, -20, -20, -20, -20, -10,
    20, 20, 0, 0, 0, 0, 20, 20,
    20, 30, 10, 0, 0, 10, 30, 20,
};

inline constexpr std::array<int, 64> kKingEndGame{
    -50, -40, -30, -20, -20, -30, -40, -50,
    -30, -20, -10, 0, 0, -10, -20, -30,
    -20, -10, 10, 20, 20, 10, -10, -20,
    -10, 0, 20, 30, 30, 20, 0, -10,
    -10, 0, 20, 30, 30, 20, 0, -10,
    -20, -10, 10, 20, 20, 10, -10, -20,
    -30, -20, -10, 0, 0, -10, -20, -30,
    -50, -40, -30, -20, -20, -30, -40, -50,
};

} // namespace koi::detail::hce_legacy_king
