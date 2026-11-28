#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Precomputed attack targets for the native board.  The native position keeps
// mailbox squares and bitboards in sync, so every attack query can be answered
// with table lookups instead of scanning the board.  Sliding attacks use Fancy
// Magic Bitboards (and PEXT lookups on x86-64 AVX2/AVX-512 variants when
// KOI_PEXT_ATTACKS is enabled), keeping slider probes O(1) while retaining
// ray/blocker tables for pin and alignment masks.

namespace koi::detail {

// Direction order used by the ray tables.  Steps are square-index deltas.
// Positive steps move towards larger square indices (N, NE, E, NW); negative
// steps move towards smaller ones (SE, S, SW, W).
inline constexpr int kAttackDirections = 8;

struct FixedAttackTables {
    std::array<std::uint64_t, 64> knight{};
    std::array<std::uint64_t, 64> king{};
    std::array<std::array<std::uint64_t, 64>, 2> pawn{};
    std::array<std::array<std::uint64_t, 64>, 2> pawn_attackers{};
    std::array<std::array<std::uint64_t, 64>, kAttackDirections> rays{};
    std::array<std::uint64_t, 64> bishop_pseudo{};
    std::array<std::uint64_t, 64> rook_pseudo{};
    std::array<std::array<std::uint64_t, 64>, 64> line{};
    std::array<std::array<std::uint64_t, 64>, 64> between{};
};

extern const FixedAttackTables kFixedAttackTables;

// Squares a knight on `square` attacks.
[[nodiscard]] inline std::uint64_t knight_attacks(int square) noexcept {
    if (static_cast<unsigned>(square) >= 64U) return 0;
    return kFixedAttackTables.knight[static_cast<std::size_t>(square)];
}

// Squares a king on `square` attacks.
[[nodiscard]] inline std::uint64_t king_attacks(int square) noexcept {
    if (static_cast<unsigned>(square) >= 64U) return 0;
    return kFixedAttackTables.king[static_cast<std::size_t>(square)];
}

// Squares a pawn of the given colour standing on `square` attacks.
[[nodiscard]] inline std::uint64_t pawn_attacks(int square, bool white) noexcept {
    if (static_cast<unsigned>(square) >= 64U) return 0;
    return kFixedAttackTables.pawn[white ? 0 : 1][static_cast<std::size_t>(square)];
}

// Squares from which a pawn of the given colour would attack `target`.
[[nodiscard]] inline std::uint64_t pawn_attackers_of(int target, bool white_pawns) noexcept {
    if (static_cast<unsigned>(target) >= 64U) return 0;
    return kFixedAttackTables.pawn_attackers[white_pawns ? 0 : 1][static_cast<std::size_t>(target)];
}

// Squares a sliding piece on `square` attacks with the given occupancy.
[[nodiscard]] std::uint64_t bishop_attacks(int square, std::uint64_t occupied) noexcept;
[[nodiscard]] std::uint64_t rook_attacks(int square, std::uint64_t occupied) noexcept;
[[nodiscard]] std::uint64_t queen_attacks(int square, std::uint64_t occupied) noexcept;

[[nodiscard]] inline std::uint64_t bishop_pseudo_attacks(int square) noexcept {
    if (static_cast<unsigned>(square) >= 64U) return 0;
    return kFixedAttackTables.bishop_pseudo[static_cast<std::size_t>(square)];
}

[[nodiscard]] inline std::uint64_t rook_pseudo_attacks(int square) noexcept {
    if (static_cast<unsigned>(square) >= 64U) return 0;
    return kFixedAttackTables.rook_pseudo[static_cast<std::size_t>(square)];
}

// Empty-board ray from `square` in `direction`, or 0 when off the board.
[[nodiscard]] inline std::uint64_t ray_attacks(int square, int direction) noexcept {
    if (static_cast<unsigned>(square) >= 64U ||
        static_cast<unsigned>(direction) >= static_cast<unsigned>(kAttackDirections)) {
        return 0;
    }
    return kFixedAttackTables.rays[static_cast<std::size_t>(direction)]
                                   [static_cast<std::size_t>(square)];
}

// Nearest occupied square on the ray from `square` in `direction`, or -1 when
// the ray is empty or leaves the board.
[[nodiscard]] int nearest_blocker(int square, int direction, std::uint64_t occupied) noexcept;

// Full board-to-board line (rank, file, or diagonal) passing through both
// `sq1` and `sq2` when `sq1 != sq2` are aligned, or 0 otherwise.
[[nodiscard]] inline std::uint64_t line_mask(int sq1, int sq2) noexcept {
    if (static_cast<unsigned>(sq1) >= 64U || static_cast<unsigned>(sq2) >= 64U) return 0;
    return kFixedAttackTables.line[static_cast<std::size_t>(sq1)]
                                   [static_cast<std::size_t>(sq2)];
}

// Squares strictly between `sq1` and `sq2` on a shared rank, file, or
// diagonal, or 0 when `sq1` and `sq2` are not aligned or are adjacent/equal.
[[nodiscard]] inline std::uint64_t between_mask(int sq1, int sq2) noexcept {
    if (static_cast<unsigned>(sq1) >= 64U || static_cast<unsigned>(sq2) >= 64U) return 0;
    return kFixedAttackTables.between[static_cast<std::size_t>(sq1)]
                                      [static_cast<std::size_t>(sq2)];
}

} // namespace koi::detail
