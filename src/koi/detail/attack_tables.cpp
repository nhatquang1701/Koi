#include "koi/detail/attack_tables.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <memory>

#if defined(KOI_PEXT_ATTACKS) && KOI_PEXT_ATTACKS
#include <immintrin.h>
#endif

namespace koi::detail {
namespace {

using U64 = std::uint64_t;

// Direction order: N, NE, E, SE, S, SW, W, NW.
constexpr std::array<int, kAttackDirections> kSteps = {8, 9, 1, -7, -8, -9, -1, 7};
constexpr std::array<int, kAttackDirections> kFileDeltas = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr std::array<int, kAttackDirections> kRankDeltas = {1, 1, 0, -1, -1, -1, 0, 1};

[[nodiscard]] constexpr int file_of(int square) noexcept {
    return square & 7;
}

[[nodiscard]] constexpr int rank_of(int square) noexcept {
    return square >> 3;
}

struct Tables {
    std::array<U64, 64> knight{};
    std::array<U64, 64> king{};
    std::array<std::array<U64, 64>, 2> pawn{};
    std::array<std::array<U64, 64>, 2> pawn_attackers{};
    std::array<std::array<U64, 64>, kAttackDirections> rays{};

    constexpr Tables() noexcept {
        for (int square = 0; square < 64; ++square) {
            const int file = file_of(square);
            const int rank = rank_of(square);

            // Knights: eight fixed offsets with a two-file / one-rank or
            // one-file / two-rank shape.
            constexpr std::array<std::array<int, 2>, 8> knight_deltas = {{
                {1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}}};
            for (const auto& delta : knight_deltas) {
                const int target_file = file + delta[0];
                const int target_rank = rank + delta[1];
                if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                    knight[square] |= U64{1} << (target_rank * 8 + target_file);
                }
            }

            // Kings: all eight neighbours.
            for (int df = -1; df <= 1; ++df) {
                for (int dr = -1; dr <= 1; ++dr) {
                    if (df == 0 && dr == 0) continue;
                    const int target_file = file + df;
                    const int target_rank = rank + dr;
                    if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                        king[square] |= U64{1} << (target_rank * 8 + target_file);
                    }
                }
            }

            // Pawn pushes: white attacks towards larger ranks, black towards
            // smaller ones.  Both colours capture at file deltas +/-1.
            for (const int df : {-1, 1}) {
                const int white_file = file + df;
                const int white_rank = rank + 1;
                if (white_file >= 0 && white_file < 8 && white_rank < 8) {
                    pawn[0][square] |= U64{1} << (white_rank * 8 + white_file);
                }
                const int black_file = file + df;
                const int black_rank = rank - 1;
                if (black_file >= 0 && black_file < 8 && black_rank >= 0) {
                    pawn[1][square] |= U64{1} << (black_rank * 8 + black_file);
                }
            }

            // Reverse tables: which squares can a pawn of a colour attack
            // `square` from?  A white pawn attacks upwards, so its sources are
            // one rank below with mirrored file deltas.
            for (const int df : {-1, 1}) {
                const int white_file = file + df;
                const int white_rank = rank - 1;
                if (white_file >= 0 && white_file < 8 && white_rank >= 0) {
                    pawn_attackers[0][square] |= U64{1} << (white_rank * 8 + white_file);
                }
                const int black_file = file + df;
                const int black_rank = rank + 1;
                if (black_file >= 0 && black_file < 8 && black_rank < 8) {
                    pawn_attackers[1][square] |= U64{1} << (black_rank * 8 + black_file);
                }
            }

            // Rays: walk each direction until the edge of the board.
            for (int direction = 0; direction < kAttackDirections; ++direction) {
                const std::size_t index = static_cast<std::size_t>(direction);
                const int file_delta = kFileDeltas[index];
                const int rank_delta = kRankDeltas[index];
                int target_file = file + file_delta;
                int target_rank = rank + rank_delta;
                U64 mask = 0;
                while (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                    mask |= U64{1} << (target_rank * 8 + target_file);
                    target_file += file_delta;
                    target_rank += rank_delta;
                }
                rays[index][static_cast<std::size_t>(square)] = mask;
            }
        }
    }
};

constexpr Tables kTables{};

[[nodiscard]] std::uint64_t sliding_attacks(int square, std::uint64_t occupied,
                                            std::initializer_list<int> directions) noexcept {
    if (square < 0 || square >= 64) return 0;
    U64 attacks = 0;
    for (const int direction : directions) {
        const std::size_t index = static_cast<std::size_t>(direction);
        const U64 ray = kTables.rays[index][static_cast<std::size_t>(square)];
        const U64 blockers = ray & occupied;
        if (blockers == 0) {
            attacks |= ray;
            continue;
        }
        // The classical trick: the blocker's own ray is a subset of this ray,
        // so XOR removes everything beyond the first occupied square.
        const int step = kSteps[index];
        const int first = step > 0 ? std::countr_zero(blockers)
                                   : 63 - std::countl_zero(blockers);
        attacks |= ray ^ kTables.rays[index][static_cast<std::size_t>(first)];
    }
    return attacks;
}

// Relevant-occupancy mask keeps only the interior squares a blocker can
// actually influence; edge squares never hide anything past themselves, so the
// rook table holds 102,400 entries and the bishop table 5,248.
[[nodiscard]] U64 relevant_mask(int square, std::initializer_list<int> directions) noexcept {
    U64 mask = 0;
    for (const int direction : directions) {
        const std::size_t index = static_cast<std::size_t>(direction);
        const U64 ray = kTables.rays[index][static_cast<std::size_t>(square)];
        if (ray == 0) continue;
        // The final square of the ray is the board edge; a blocker there does
        // not hide anything, so it stays out of the mask.
        const U64 edge = kSteps[index] > 0
            ? (U64{1} << (63 - std::countl_zero(ray)))
            : (U64{1} << std::countr_zero(ray));
        mask |= ray & ~edge;
    }
    return mask;
}

#if defined(KOI_PEXT_ATTACKS) && KOI_PEXT_ATTACKS

struct PextSquare {
    const U64* attacks = nullptr;
    U64 mask = 0;

    [[nodiscard]] U64 lookup(U64 occupied) const noexcept {
        return attacks[static_cast<std::uint32_t>(_pext_u64(occupied, mask))];
    }
};

using SquareEntry = PextSquare;

[[nodiscard]] std::size_t slider_lookup_index(const SquareEntry& entry, U64 subset) noexcept {
    return static_cast<std::size_t>(_pext_u64(subset, entry.mask));
}

#else

constexpr std::array<U64, 64> kRookMagics = {
    0x8a80104000800020ULL, 0x140002000100040ULL,  0x2801880a0017001ULL,  0x100081001000420ULL,
    0x200020010080420ULL,  0x3001c0002010008ULL,  0x8480008002000100ULL, 0x2080088004402900ULL,
    0x800098204000ULL,     0x2024401000200040ULL, 0x100802000801000ULL,  0x120800800801000ULL,
    0x208808088000400ULL,  0x2802200800400ULL,    0x2200800100020080ULL, 0x801000060821100ULL,
    0x80044006422000ULL,   0x100808020004000ULL,  0x12108a0010204200ULL, 0x140848010000802ULL,
    0x481828014002800ULL,  0x8094004002004100ULL, 0x4010040010010802ULL, 0x20008806104ULL,
    0x100400080208000ULL,  0x2040002120081000ULL, 0x21200680100081ULL,   0x20100080080080ULL,
    0x2000a00200410ULL,    0x20080800400ULL,      0x80088400100102ULL,   0x80004600042881ULL,
    0x4040008040800020ULL, 0x440003000200801ULL,  0x4200011004500ULL,    0x188020010100100ULL,
    0x14800401802800ULL,   0x2080040080800200ULL, 0x124080204001001ULL,  0x200046502000484ULL,
    0x480400080088020ULL,  0x1000422010034000ULL, 0x30200100110040ULL,   0x100021010009ULL,
    0x2002080100110004ULL, 0x202008004008002ULL,  0x20020004010100ULL,   0x2048440040820001ULL,
    0x101002200408200ULL,  0x40802000401080ULL,   0x4008142004410100ULL, 0x2060820c0120200ULL,
    0x1001004080100ULL,    0x20c020080040080ULL,  0x2935610830022400ULL, 0x44440041009200ULL,
    0x280001040802101ULL,  0x2100190040002085ULL, 0x80c0084100102001ULL, 0x4024081001000421ULL,
    0x20030a0244872ULL,    0x12001008414402ULL,   0x2006104900a0804ULL,  0x1004081002402ULL,
};

constexpr std::array<U64, 64> kBishopMagics = {
    0x40040844404084ULL,   0x2004208a004208ULL,   0x10190041080202ULL,   0x108060845042010ULL,
    0x581104180800210ULL,  0x2112080446200010ULL, 0x1080820820060210ULL, 0x3c0808410220200ULL,
    0x4050404440404ULL,    0x21001420088ULL,      0x24d0080801082102ULL, 0x1020a0a020400ULL,
    0x40308200402ULL,      0x4011002100800ULL,    0x401484104104005ULL,  0x801010402020200ULL,
    0x400210c3880100ULL,   0x404022024108200ULL,  0x810018200204102ULL,  0x4002801a02003ULL,
    0x85040820080400ULL,   0x810102c808880400ULL, 0xe900410884800ULL,    0x8002020480840102ULL,
    0x220200865090201ULL,  0x2010100a02021202ULL, 0x152048408022401ULL,  0x20080002081110ULL,
    0x4001001021004000ULL, 0x800040400a011002ULL, 0xe4004081011002ULL,   0x1c004001012080ULL,
    0x8004200962a00220ULL, 0x8422100208500202ULL, 0x2000402200300c08ULL, 0x8646020080080080ULL,
    0x80020a0200100808ULL, 0x2010004880111000ULL, 0x623000a080011400ULL, 0x42008c0340209202ULL,
    0x209188240001000ULL,  0x400408a884001800ULL, 0x110400a6080400ULL,   0x1840060a44020800ULL,
    0x90080104000041ULL,   0x201011000808101ULL,  0x1a2208080504f080ULL, 0x8012020600211212ULL,
    0x500861011240000ULL,  0x180806108200800ULL,  0x4000020e01040044ULL, 0x300000261044000aULL,
    0x802241102020002ULL,  0x20906061210001ULL,   0x5a84841004010310ULL, 0x4010801011c04ULL,
    0xa010109502200ULL,    0x4a02012000ULL,       0x500201010098b028ULL, 0x8040002811040900ULL,
    0x28000010020204ULL,   0x6000020202d0240ULL,  0x8918844842082200ULL, 0x4010011029020020ULL,
};

struct MagicSquare {
    const U64* attacks = nullptr;
    U64 mask = 0;
    U64 magic = 0;
    int shift = 64;

    [[nodiscard]] U64 lookup(U64 occupied) const noexcept {
        return attacks[((occupied & mask) * magic) >> shift];
    }
};

using SquareEntry = MagicSquare;

[[nodiscard]] std::size_t slider_lookup_index(const SquareEntry& entry, U64 subset) noexcept {
    return static_cast<std::size_t>((subset * entry.magic) >> entry.shift);
}

#endif // KOI_PEXT_ATTACKS

void init_slider_squares(std::array<SquareEntry, 64>& entries,
                         std::unique_ptr<U64[]>& table,
                         std::initializer_list<int> directions,
                         const std::array<U64, 64>* magics) noexcept {
    std::array<std::size_t, 64> offsets{};
    std::size_t total = 0;
    for (int square = 0; square < 64; ++square) {
        const std::size_t index = static_cast<std::size_t>(square);
        const U64 mask = relevant_mask(square, directions);
        const int bits = std::popcount(mask);
        entries[index].mask = mask;
#if !(defined(KOI_PEXT_ATTACKS) && KOI_PEXT_ATTACKS)
        entries[index].magic = (*magics)[index];
        entries[index].shift = 64 - bits;
#else
        (void)magics;
#endif
        offsets[index] = total;
        total += std::size_t{1} << bits;
    }
    auto storage = std::make_unique<U64[]>(total);
    U64* const base = storage.get();
    for (int square = 0; square < 64; ++square) {
        const std::size_t index = static_cast<std::size_t>(square);
        U64* const sq_attacks = base + offsets[index];
        entries[index].attacks = sq_attacks;
        const U64 mask = entries[index].mask;
        U64 subset = 0;
        do {
            sq_attacks[slider_lookup_index(entries[index], subset)] =
                sliding_attacks(square, subset, directions);
            subset = (subset - mask) & mask;
        } while (subset != 0);
    }
    table = std::move(storage);
}

struct SliderTables {
    std::array<SquareEntry, 64> rook{};
    std::array<SquareEntry, 64> bishop{};
    std::unique_ptr<U64[]> rook_table;
    std::unique_ptr<U64[]> bishop_table;

    SliderTables() noexcept {
#if defined(KOI_PEXT_ATTACKS) && KOI_PEXT_ATTACKS
        const std::array<U64, 64>* rook_magics = nullptr;
        const std::array<U64, 64>* bishop_magics = nullptr;
#else
        const std::array<U64, 64>* rook_magics = &kRookMagics;
        const std::array<U64, 64>* bishop_magics = &kBishopMagics;
#endif
        init_slider_squares(rook, rook_table, {0, 2, 4, 6}, rook_magics);
        init_slider_squares(bishop, bishop_table, {1, 3, 5, 7}, bishop_magics);
    }
};

const SliderTables kSliderTables{};

} // namespace

std::uint64_t knight_attacks(int square) noexcept {
    if (square < 0 || square >= 64) return 0;
    return kTables.knight[static_cast<std::size_t>(square)];
}

std::uint64_t king_attacks(int square) noexcept {
    if (square < 0 || square >= 64) return 0;
    return kTables.king[static_cast<std::size_t>(square)];
}

std::uint64_t pawn_attacks(int square, bool white) noexcept {
    if (square < 0 || square >= 64) return 0;
    return kTables.pawn[white ? 0 : 1][static_cast<std::size_t>(square)];
}

std::uint64_t pawn_attackers_of(int target, bool white_pawns) noexcept {
    if (target < 0 || target >= 64) return 0;
    return kTables.pawn_attackers[white_pawns ? 0 : 1][static_cast<std::size_t>(target)];
}

std::uint64_t bishop_attacks(int square, std::uint64_t occupied) noexcept {
    if (square < 0 || square >= 64) return 0;
    return kSliderTables.bishop[static_cast<std::size_t>(square)].lookup(occupied);
}

std::uint64_t rook_attacks(int square, std::uint64_t occupied) noexcept {
    if (square < 0 || square >= 64) return 0;
    return kSliderTables.rook[static_cast<std::size_t>(square)].lookup(occupied);
}

std::uint64_t queen_attacks(int square, std::uint64_t occupied) noexcept {
    if (square < 0 || square >= 64) return 0;
    const std::size_t index = static_cast<std::size_t>(square);
    return kSliderTables.bishop[index].lookup(occupied) |
        kSliderTables.rook[index].lookup(occupied);
}

std::uint64_t ray_attacks(int square, int direction) noexcept {
    if (square < 0 || square >= 64 || direction < 0 || direction >= kAttackDirections) return 0;
    return kTables.rays[static_cast<std::size_t>(direction)][static_cast<std::size_t>(square)];
}

int nearest_blocker(int square, int direction, std::uint64_t occupied) noexcept {
    const U64 ray = ray_attacks(square, direction);
    if (ray == 0) return -1;
    const U64 blockers = ray & occupied;
    if (blockers == 0) return -1;
    const int step = kSteps[static_cast<std::size_t>(direction)];
    return step > 0 ? std::countr_zero(blockers) : 63 - std::countl_zero(blockers);
}

} // namespace koi::detail
