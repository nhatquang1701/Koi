#include "koi/classical_evaluator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "koi/detail/attack_tables.hpp"
#include "koi/detail/search_constants.hpp"

namespace koi {
namespace {

// Tapered evaluation unit: midgame and endgame components in centipawns.
struct Score {
    int mg = 0;
    int eg = 0;

    constexpr Score& operator+=(const Score& other) noexcept {
        mg += other.mg;
        eg += other.eg;
        return *this;
    }
};

[[nodiscard]] constexpr Score operator+(Score left, const Score right) noexcept {
    left += right;
    return left;
}

[[nodiscard]] constexpr Score scaled(const Score score, const int mg, const int eg) noexcept {
    return Score{score.mg * mg, score.eg * eg};
}

// Internal development/tuning seam: term-group scales in percent.  The
// production defaults are 100 and are used unless KOI_HCE_* is set, so normal
// engine behavior is unaffected; tuners can screen variants by launching the
// same binary with different environments.
struct HceTuning {
    int passed = 100;
    int mobility = 100;
    int king_safety = 100;
    int pawn_structure = 100;
    int tempo = 100;
    int evaluation = 100;
    int threats = 100;
    int piece_terms = 100;
    int eg_component = 100;
    int passed_rank = 100;
    int mobility_knight = 100;
    int mobility_bishop = 100;
    int mobility_rook = 100;
    int mobility_queen = 100;
};

[[nodiscard]] int tuning_percent(const char* name, int fallback) noexcept {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    const std::string_view text{value};
    int parsed = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || end != text.data() + text.size() || parsed < 0 || parsed > 400) {
        return fallback;
    }
    return parsed;
}

[[nodiscard]] const HceTuning& hce_tuning() noexcept {
    static const HceTuning tuning{
        tuning_percent("KOI_HCE_PASSED_SCALE", 100),
        tuning_percent("KOI_HCE_MOBILITY_SCALE", 100),
        tuning_percent("KOI_HCE_KING_SAFETY_SCALE", 100),
        tuning_percent("KOI_HCE_PAWN_SCALE", 100),
        tuning_percent("KOI_HCE_TEMPO_SCALE", 100),
        tuning_percent("KOI_HCE_EVAL_SCALE", 100),
        tuning_percent("KOI_HCE_THREAT_SCALE", 100),
        tuning_percent("KOI_HCE_PIECE_TERM_SCALE", 100),
        tuning_percent("KOI_HCE_EG_SCALE", 100),
        tuning_percent("KOI_HCE_PASSED_RANK_SCALE", 100),
        tuning_percent("KOI_HCE_MOB_N", 100),
        tuning_percent("KOI_HCE_MOB_B", 100),
        tuning_percent("KOI_HCE_MOB_R", 100),
        tuning_percent("KOI_HCE_MOB_Q", 100)};
    return tuning;
}

[[nodiscard]] constexpr Score percent_of(Score score, int percent) noexcept {
    return Score{score.mg * percent / 100, score.eg * percent / 100};
}

constexpr int kPhaseMaximum = 24;
constexpr std::size_t kWhite = 0;
constexpr std::size_t kBlack = 1;
constexpr std::uint64_t kCenterMask =
    (std::uint64_t{1} << 27) | (std::uint64_t{1} << 28) |
    (std::uint64_t{1} << 35) | (std::uint64_t{1} << 36);  // d4/e4/d5/e5

[[nodiscard]] constexpr std::size_t type_index(const PieceType type) noexcept {
    return static_cast<std::size_t>(type);
}

[[nodiscard]] constexpr int sign_of(const std::size_t index) noexcept {
    return index == kWhite ? 1 : -1;
}

[[nodiscard]] constexpr int file_of(const int square) noexcept { return square & 7; }
[[nodiscard]] constexpr int rank_of(const int square) noexcept { return square >> 3; }
[[nodiscard]] constexpr std::uint64_t bit_of(const int square) noexcept {
    return std::uint64_t{1} << static_cast<unsigned>(square);
}

[[nodiscard]] constexpr int square_distance(const int first, const int second) noexcept {
    const int df = file_of(first) - file_of(second);
    const int dr = rank_of(first) - rank_of(second);
    return std::max(df < 0 ? -df : df, dr < 0 ? -dr : dr);
}

// Texel-tuned tapered material, piece-square, safe-mobility, and feature tables
// (rank 1 first, White perspective; Black mirrored via square ^ 56).
inline constexpr std::array<Score, 7> kMaterialWeights{{
    {0, 0}, {27, 182}, {414, 420}, {563, 475}, {517, 836}, {1525, 1407}, {0, 0}
}};

inline constexpr std::array<Score, 64> kPawnPst{{
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {-36, 2}, {-20, -3}, {-25, -7}, {-33, -16}, {-29, 9}, {-5, -5}, {20, -13}, {-24, -18},
    {-36, -11}, {-40, -15}, {-20, -27}, {-22, -29}, {-9, -26}, {-18, -26}, {4, -30}, {-23, -28},
    {-28, -6}, {-26, -10}, {-13, -30}, {8, -36}, {11, -38}, {-4, -30}, {-7, -17}, {-17, -26},
    {-12, 3}, {-4, -3}, {2, -17}, {6, -33}, {29, -34}, {12, -19}, {12, -10}, {6, -12},
    {-2, 22}, {19, 22}, {28, 22}, {49, 9}, {48, 27}, {71, 29}, {38, 44}, {23, 20},
    {6, 13}, {25, 43}, {22, 76}, {47, 86}, {40, 74}, {26, 79}, {-32, 62}, {-32, 58},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}
}};

inline constexpr std::array<Score, 64> kKnightPst{{
    {-26, -12}, {-24, -36}, {-27, 15}, {-13, 9}, {0, 5}, {-12, -7}, {-24, -23}, {-24, -17},
    {-49, 3}, {-20, -6}, {-10, 8}, {-6, 8}, {4, 5}, {1, -1}, {-7, 0}, {-11, 2},
    {-43, -3}, {-8, 4}, {-2, 14}, {14, 20}, {34, 16}, {9, 4}, {24, 2}, {-28, 2},
    {-12, -3}, {-11, 5}, {17, 22}, {13, 35}, {22, 27}, {20, 11}, {7, 10}, {4, -4},
    {11, 5}, {4, 9}, {17, 30}, {57, 22}, {20, 23}, {55, 19}, {5, 14}, {23, -2},
    {-5, -15}, {23, -4}, {4, 32}, {49, 10}, {83, -7}, {34, 8}, {35, -13}, {4, -15},
    {-12, -22}, {-28, -2}, {31, 10}, {21, 23}, {12, 4}, {66, -5}, {-2, -25}, {22, -26},
    {-137, -74}, {-9, -9}, {-15, 0}, {-23, -1}, {-20, -13}, {-31, -9}, {-11, -19}, {-97, -63}
}};

inline constexpr std::array<Score, 64> kBishopPst{{
    {-22, -16}, {30, -10}, {-8, -9}, {-5, -1}, {-11, -3}, {-31, 5}, {1, 3}, {-31, -24},
    {12, -4}, {5, -26}, {27, 0}, {-5, 9}, {2, 5}, {23, -8}, {22, -24}, {22, -31},
    {-2, -4}, {26, 15}, {-5, 10}, {3, 21}, {13, 25}, {0, -3}, {31, -2}, {1, 0},
    {-12, -4}, {3, 8}, {-15, 30}, {32, 16}, {18, 17}, {1, 21}, {-8, 2}, {31, -30},
    {-8, -4}, {8, 14}, {18, 13}, {21, 19}, {34, 18}, {20, 3}, {14, 7}, {-22, -4},
    {-20, 3}, {9, 1}, {-14, -4}, {31, -2}, {23, 9}, {19, 8}, {31, 5}, {19, -5},
    {-13, -9}, {-1, -11}, {-4, 17}, {-18, -5}, {-23, 11}, {-18, -1}, {-3, -13}, {-22, -16},
    {-46, -15}, {-18, 7}, {-22, 1}, {-24, -7}, {-17, -6}, {-33, -3}, {-19, -10}, {-48, -8}
}};

inline constexpr std::array<Score, 64> kRookPst{{
    {-26, -5}, {-22, -8}, {-23, -2}, {-23, -4}, {0, -21}, {-12, -17}, {4, -20}, {-13, -14},
    {-36, -6}, {-30, -6}, {-29, 1}, {-25, -6}, {-15, -12}, {-14, -23}, {-8, -31}, {-29, -25},
    {-32, 2}, {-11, -7}, {-8, -4}, {-28, -6}, {-21, -12}, {-13, -11}, {24, -32}, {3, -29},
    {-21, 11}, {-16, 8}, {-17, 9}, {-12, 1}, {-13, -7}, {-16, -9}, {9, -16}, {-16, -12},
    {-8, 23}, {-14, 18}, {-5, 26}, {6, 8}, {12, -2}, {10, 4}, {15, -1}, {25, -7},
    {0, 21}, {7, 21}, {16, 18}, {17, 8}, {35, -3}, {35, 5}, {33, 10}, {29, 5},
    {4, -1}, {-24, 14}, {0, 17}, {23, 7}, {22, 1}, {45, -1}, {38, 1}, {33, 3},
    {9, 11}, {7, 19}, {6, 21}, {7, 19}, {43, 6}, {21, 10}, {14, 14}, {25, 19}
}};

inline constexpr std::array<Score, 64> kQueenPst{{
    {-36, -38}, {-31, -43}, {-12, -45}, {10, -69}, {6, -53}, {-42, -44}, {-25, -35}, {-15, -22},
    {-16, -29}, {-15, -30}, {15, -55}, {18, -58}, {21, -40}, {12, -52}, {13, -77}, {5, -33},
    {-15, -10}, {-3, -4}, {-18, 5}, {-11, 8}, {-6, 10}, {-9, 15}, {4, -16}, {-10, -15},
    {14, -11}, {-30, 16}, {-27, 30}, {-29, 40}, {-17, 24}, {-18, 33}, {-14, 11}, {4, 1},
    {-11, 1}, {-16, 32}, {-5, 27}, {-14, 38}, {-28, 63}, {4, 46}, {-4, 42}, {7, 28},
    {-7, -6}, {-1, 7}, {-2, 18}, {30, 23}, {42, 32}, {63, 49}, {55, 21}, {52, 18},
    {-15, -7}, {-40, 14}, {-9, 27}, {1, 25}, {2, 26}, {50, 45}, {10, 25}, {29, 28},
    {-31, -19}, {-11, -24}, {9, -1}, {34, 9}, {38, 20}, {36, 20}, {10, -26}, {1, -17}
}};

inline constexpr std::array<Score, 64> kKingPst{{
    {-1, -60}, {93, -77}, {75, -53}, {-13, -31}, {50, -56}, {-12, -36}, {81, -77}, {73, -104},
    {16, -45}, {36, -42}, {4, -14}, {-9, -10}, {-19, -4}, {-1, -17}, {46, -40}, {50, -67},
    {-19, -42}, {3, -33}, {-21, -4}, {-44, 9}, {-31, 8}, {-31, -3}, {-9, -26}, {-46, -44},
    {-22, -32}, {-29, -8}, {-26, 12}, {-29, 31}, {-51, 30}, {-35, 15}, {-46, -8}, {-43, -33},
    {-1, -13}, {-3, 15}, {-8, 49}, {-18, 54}, {-26, 52}, {-29, 51}, {-15, 20}, {-21, -6},
    {3, 3}, {10, 39}, {13, 61}, {7, 67}, {9, 68}, {8, 60}, {9, 41}, {-4, 9},
    {0, -5}, {14, 41}, {8, 59}, {10, 50}, {12, 58}, {-1, 53}, {-3, 53}, {3, 12},
    {4, -17}, {4, -9}, {7, 2}, {0, 11}, {6, 3}, {3, -6}, {3, 13}, {5, -25}
}};

inline constexpr std::array<Score, 9> kKnightMobility{{
    {-32, -7}, {-32, -17}, {-20, -8}, {-10, -5}, {1, 2}, {13, 11}, {26, 11},
    {34, 12}, {20, 0}
}};

inline constexpr std::array<Score, 14> kBishopMobility{{
    {-53, -16}, {-37, -16}, {-14, -18}, {-5, -5}, {5, 2}, {13, 7}, {17, 11},
    {20, 13}, {22, 11}, {18, 9}, {10, 6}, {4, 1}, {0, 0}, {0, -2}
}};

inline constexpr std::array<Score, 15> kRookMobility{{
    {-10, -5}, {-35, -22}, {-16, -18}, {-11, -11}, {-14, -4}, {-9, -3}, {-5, 1},
    {0, 2}, {8, 6}, {15, 9}, {17, 9}, {17, 12}, {19, 13}, {14, 13},
    {10, -3}
}};

inline constexpr std::array<Score, 28> kQueenMobility{{
    {0, 0}, {-3, 0}, {-8, -1}, {-9, -2}, {-9, -4}, {-11, -5}, {-9, -8},
    {-7, -7}, {-5, -4}, {-2, -3}, {1, 2}, {3, 0}, {3, 7}, {4, 5},
    {7, 5}, {7, 9}, {9, 5}, {6, 6}, {6, 4}, {8, 3}, {5, 1},
    {3, -3}, {2, -2}, {1, -2}, {0, -2}, {0, -2}, {0, 0}, {0, -1}
}};

inline constexpr Score kDoubledPawnPenalty{-8, -32};
inline constexpr Score kIsolatedOpenPenalty{-22, -12};
inline constexpr Score kIsolatedClosedPenalty{-11, -3};
inline constexpr Score kBackwardOpenPenalty{-18, -14};
inline constexpr Score kBackwardClosedPenalty{-1, 0};
inline constexpr std::array<Score, 6> kPhalanxPawnRankBonus{{
    {4, 0}, {14, 4}, {17, 9}, {23, 26}, {6, 23}, {2, 6}
}};
inline constexpr std::array<Score, 6> kSupportedPawnRankBonus{{
    {0, 0}, {20, 11}, {16, 8}, {33, 18}, {26, 30}, {4, 14}
}};
inline constexpr std::array<Score, 6> kCandidatePasserRankBonus{{
    {6, -10}, {9, 0}, {9, 16}, {2, 24}, {1, 5}, {0, 0}
}};
inline constexpr std::array<Score, 8> kPassedPawnRankBonus{{
    {0, 0}, {-1, -17}, {-1, -3}, {-9, 13}, {-2, 22}, {1, 28}, {5, 25}, {0, 0}
}};
inline constexpr Score kPassedOwnKingDist{5, -8};
inline constexpr Score kPassedEnemyKingDist{0, -2};
inline constexpr Score kPassedOwnKingRankDist{2, -3};
inline constexpr Score kPassedEnemyKingRankDist{-2, 10};
inline constexpr Score kPassedBlockedBonus{-5, 3};
inline constexpr Score kPassedSafeAdvanceBonus{-3, 37};
inline constexpr Score kPassedUnsafeAdvanceBonus{1, 29};
inline constexpr Score kPassedFreeSafePathBonus{-33, 53};
inline constexpr Score kConnectedPasserBonus{7, -4};
inline constexpr Score kRookBehindOwnPasser{5, 14};
inline constexpr Score kRookBehindEnemyPasser{-12, -51};
inline constexpr Score kUnstoppablePasserBonus{-1, 92};
inline constexpr int kKingDangerBase = 90;
inline constexpr std::array<int, 4> kKingDangerRingWeight{{68, 43, 49, 64}};
inline constexpr std::array<int, 4> kKingDangerSafeCheckWeight{{108, 56, 115, 108}};
inline constexpr int kKingDangerWeakRingWeight = 43;
inline constexpr int kKingDangerNoQueenWeight = -26;
inline constexpr Score kKingOpenFilePenalty{-27, -32};
inline constexpr Score kKingSemiOpenFilePenalty{6, -18};
inline constexpr std::array<Score, 5> kKingShelterOwnFile{{
    {-40, 25}, {0, 0}, {30, -6}, {19, -6}, {-9, -12}
}};
inline constexpr std::array<Score, 5> kKingShelterAdjFile{{
    {-33, 26}, {11, 5}, {18, -8}, {6, -8}, {-2, -16}
}};
inline constexpr std::array<Score, 5> kKingStormBlocked{{
    {0, 0}, {-11, -4}, {-6, -14}, {15, -19}, {13, -13}
}};
inline constexpr std::array<Score, 5> kKingStormUnblocked{{
    {10, 14}, {0, 19}, {-22, 6}, {-6, -4}, {9, -11}
}};
inline constexpr std::array<Score, 4> kHangingBonus{{
    {42, 32}, {51, 33}, {40, 30}, {38, -3}
}};
inline constexpr Score kPawnAttackDefendedBonus{73, 24};
inline constexpr Score kBishopPairBonus{49, 40};
inline constexpr Score kKnightOutpostSupported{44, 15};
inline constexpr Score kKnightOutpostUnsupported{29, 1};
inline constexpr Score kBishopOutpostSupported{27, 4};
inline constexpr Score kBishopOutpostUnsupported{23, 2};
inline constexpr Score kMinorBehindPawn{11, 3};
inline constexpr Score kBishopSameColorPawn{-1, -9};
inline constexpr Score kBishopLongDiagonal{30, 12};
inline constexpr Score kRookOpenFileBonus{65, -1};
inline constexpr Score kRookSemiOpenFileBonus{27, 10};
inline constexpr Score kRookOnQueenFileBonus{18, 5};
inline constexpr Score kRookSeventhBonus{12, 27};
inline constexpr Score kRookConnectedBonus{2, 0};
inline constexpr Score kTrappedRookPenalty{-16, -12};
inline constexpr Score kKnightPerOwnPawn{17, 5};
inline constexpr Score kRookPerOwnPawn{32, -2};
inline constexpr Score kCenterControlBonus{4, 2};
inline constexpr Score kDevelopmentBonus{7, 4};
inline constexpr Score kTempoBonus{56, 33};

[[nodiscard]] constexpr const std::array<Score, 64>& piece_square_table(
    const PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn: return kPawnPst;
    case PieceType::knight: return kKnightPst;
    case PieceType::bishop: return kBishopPst;
    case PieceType::rook: return kRookPst;
    case PieceType::queen: return kQueenPst;
    case PieceType::king:
    case PieceType::none: return kKingPst;
    }
    return kKingPst;
}

using PawnMaskTable = std::array<std::array<std::uint64_t, 64>, 2>;

[[nodiscard]] constexpr PawnMaskTable make_passed_pawn_masks() noexcept {
    PawnMaskTable masks{};
    for (std::size_t color = 0; color < 2; ++color) {
        const bool white = color == kWhite;
        for (int square = 0; square < 64; ++square) {
            const int file = file_of(square);
            const int rank = rank_of(square);
            std::uint64_t mask = 0;
            for (int ff = std::max(0, file - 1); ff <= std::min(7, file + 1); ++ff) {
                for (int fr = white ? rank + 1 : rank - 1; fr >= 0 && fr < 8;
                     fr += white ? 1 : -1) {
                    mask |= bit_of(fr * 8 + ff);
                }
            }
            masks[color][static_cast<std::size_t>(square)] = mask;
        }
    }
    return masks;
}

[[nodiscard]] constexpr PawnMaskTable make_forward_file_masks() noexcept {
    PawnMaskTable masks{};
    for (std::size_t color = 0; color < 2; ++color) {
        const bool white = color == kWhite;
        for (int square = 0; square < 64; ++square) {
            const int file = file_of(square);
            const int rank = rank_of(square);
            std::uint64_t mask = 0;
            for (int fr = white ? rank + 1 : rank - 1; fr >= 0 && fr < 8;
                 fr += white ? 1 : -1) {
                mask |= bit_of(fr * 8 + file);
            }
            masks[color][static_cast<std::size_t>(square)] = mask;
        }
    }
    return masks;
}

[[nodiscard]] constexpr PawnMaskTable make_connected_pawn_masks() noexcept {
    PawnMaskTable masks{};
    for (std::size_t color = 0; color < 2; ++color) {
        const bool white = color == kWhite;
        for (int square = 0; square < 64; ++square) {
            const int file = file_of(square);
            const int rank = rank_of(square);
            std::uint64_t mask = 0;
            for (int neighbor_file : {file - 1, file + 1}) {
                if (neighbor_file < 0 || neighbor_file > 7) {
                    continue;
                }
                for (int neighbor_rank : {rank, white ? rank - 1 : rank + 1}) {
                    if (neighbor_rank < 0 || neighbor_rank > 7) {
                        continue;
                    }
                    mask |= bit_of(neighbor_rank * 8 + neighbor_file);
                }
            }
            masks[color][static_cast<std::size_t>(square)] = mask;
        }
    }
    return masks;
}

[[nodiscard]] constexpr std::array<std::uint64_t, 8> make_adjacent_file_masks() noexcept {
    std::array<std::uint64_t, 8> masks{};
    for (int file = 0; file < 8; ++file) {
        std::uint64_t mask = 0;
        if (file > 0) {
            mask |= 0x0101010101010101ULL << static_cast<unsigned>(file - 1);
        }
        if (file < 7) {
            mask |= 0x0101010101010101ULL << static_cast<unsigned>(file + 1);
        }
        masks[static_cast<std::size_t>(file)] = mask;
    }
    return masks;
}

[[nodiscard]] constexpr std::array<std::uint64_t, 64> make_phalanx_masks() noexcept {
    std::array<std::uint64_t, 64> masks{};
    for (int square = 0; square < 64; ++square) {
        const int file = file_of(square);
        const int rank = rank_of(square);
        std::uint64_t mask = 0;
        if (file > 0) {
            mask |= bit_of(rank * 8 + (file - 1));
        }
        if (file < 7) {
            mask |= bit_of(rank * 8 + (file + 1));
        }
        masks[static_cast<std::size_t>(square)] = mask;
    }
    return masks;
}

[[nodiscard]] constexpr PawnMaskTable make_behind_adjacent_masks() noexcept {
    PawnMaskTable masks{};
    for (std::size_t color = 0; color < 2; ++color) {
        const bool white = color == kWhite;
        for (int square = 0; square < 64; ++square) {
            const int file = file_of(square);
            const int rank = rank_of(square);
            std::uint64_t mask = 0;
            for (const int neighbor_file : {file - 1, file + 1}) {
                if (neighbor_file < 0 || neighbor_file > 7) {
                    continue;
                }
                for (int fr = rank; white ? (fr >= 0) : (fr < 8); fr += white ? -1 : 1) {
                    mask |= bit_of(fr * 8 + neighbor_file);
                }
            }
            masks[color][static_cast<std::size_t>(square)] = mask;
        }
    }
    return masks;
}

inline constexpr PawnMaskTable kPassedPawnMask = make_passed_pawn_masks();
inline constexpr PawnMaskTable kForwardFileMask = make_forward_file_masks();
inline constexpr PawnMaskTable kConnectedPawnMask = make_connected_pawn_masks();
inline constexpr std::array<std::uint64_t, 8> kAdjacentFileMask = make_adjacent_file_masks();
inline constexpr std::array<std::uint64_t, 64> kPhalanxMask = make_phalanx_masks();
inline constexpr PawnMaskTable kBehindAdjacentMask = make_behind_adjacent_masks();

struct EvalBoard {
    std::array<std::uint64_t, 2> colors{};
    std::array<std::array<std::uint64_t, 7>, 2> pieces{};
    std::uint64_t occupied = 0;
    std::array<int, 2> kings{-1, -1};
    std::array<std::uint64_t, 2> pawn_attacks{};
    std::array<std::array<Score, 4>, 2> mobility_scores{};
    std::uint8_t phase = 0;
    Color side_to_move = Color::white;
    std::uint8_t development[2]{};
    std::uint8_t center_control[2]{};
    std::array<std::uint64_t, 2> king_ring{};
    std::array<std::uint64_t, 2> attacked_squares{};
    std::array<std::uint64_t, 2> attacked_by2{};
    std::array<std::array<std::uint64_t, 7>, 2> attacks_by{};
    std::array<std::array<std::uint8_t, 4>, 2> king_ring_attackers{};
};

[[nodiscard]] EvalBoard make_eval_board(const GameState& state) noexcept {
    EvalBoard eval;
    const ExchangeSnapshot snap = state.exchange_snapshot();
    eval.pieces = *snap.piece_bitboards;
    eval.occupied = snap.occupied;
    eval.side_to_move = snap.side_to_move;

    std::array<std::uint64_t, 2> ext_ring{};
    for (std::size_t color = 0; color < 2; ++color) {
        std::uint64_t occ_color = 0;
        for (std::size_t pt = 1; pt <= 6; ++pt) {
            occ_color |= eval.pieces[color][pt];
        }
        eval.colors[color] = occ_color;
        const std::uint64_t king_bb = eval.pieces[color][type_index(PieceType::king)];
        eval.kings[color] = king_bb != 0 ? std::countr_zero(king_bb) : -1;
        if (eval.kings[color] >= 0) {
            const std::uint64_t kr =
                detail::king_attacks(eval.kings[color]) | bit_of(eval.kings[color]);
            eval.king_ring[color] = kr;
            ext_ring[color] = kr | (color == kWhite ? (kr << 8) : (kr >> 8));
        }
    }

    const int phase =
        std::popcount(eval.pieces[kWhite][type_index(PieceType::knight)] |
                      eval.pieces[kBlack][type_index(PieceType::knight)] |
                      eval.pieces[kWhite][type_index(PieceType::bishop)] |
                      eval.pieces[kBlack][type_index(PieceType::bishop)]) +
        2 * std::popcount(eval.pieces[kWhite][type_index(PieceType::rook)] |
                          eval.pieces[kBlack][type_index(PieceType::rook)]) +
        4 * std::popcount(eval.pieces[kWhite][type_index(PieceType::queen)] |
                          eval.pieces[kBlack][type_index(PieceType::queen)]);
    eval.phase = static_cast<std::uint8_t>(std::min(phase, kPhaseMaximum));

    for (std::size_t color = 0; color < 2; ++color) {
        constexpr std::array<std::uint64_t, 2> kHomeMinors{
            0x0000000000000066ULL,   // b1/c1/f1/g1
            0x6600000000000000ULL};  // b8/c8/f8/g8
        const std::uint64_t minors =
            eval.pieces[color][type_index(PieceType::knight)] |
            eval.pieces[color][type_index(PieceType::bishop)];
        eval.development[color] = static_cast<std::uint8_t>(
            std::popcount(minors & ~kHomeMinors[color]));
    }

    constexpr std::uint64_t kNotFileA = 0xFEFEFEFEFEFEFEFEULL;
    constexpr std::uint64_t kNotFileH = 0x7F7F7F7F7F7F7F7FULL;
    const std::uint64_t white_pawns = eval.pieces[kWhite][type_index(PieceType::pawn)];
    const std::uint64_t black_pawns = eval.pieces[kBlack][type_index(PieceType::pawn)];
    eval.pawn_attacks[kWhite] =
        ((white_pawns & kNotFileA) << 7) | ((white_pawns & kNotFileH) << 9);
    eval.pawn_attacks[kBlack] =
        ((black_pawns & kNotFileA) >> 9) | ((black_pawns & kNotFileH) >> 7);

    for (std::size_t color = 0; color < 2; ++color) {
        const bool white = color == kWhite;
        const std::size_t enemy = 1 - color;
        const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];
        const std::uint64_t low_ranks = white ? 0x0000000000FFFF00ULL : 0x00FFFF0000000000ULL;
        const std::uint64_t blocked_pawns =
            white ? (own_pawns & (eval.occupied >> 8)) : (own_pawns & (eval.occupied << 8));
        const std::uint64_t excluded_pawns = own_pawns & (low_ranks | blocked_pawns);
        const std::uint64_t mob_area = ~(
            eval.pieces[color][type_index(PieceType::king)] |
            eval.pieces[color][type_index(PieceType::queen)] |
            excluded_pawns |
            eval.pawn_attacks[enemy]);

        const std::uint64_t double_pawn_att = white
            ? (((own_pawns & kNotFileA) << 7) & ((own_pawns & kNotFileH) << 9))
            : (((own_pawns & kNotFileA) >> 9) & ((own_pawns & kNotFileH) >> 7));
        std::uint64_t attacks = eval.pawn_attacks[color];
        std::uint64_t by2 = double_pawn_att;
        eval.attacks_by[color][type_index(PieceType::pawn)] = eval.pawn_attacks[color];

        for (std::uint64_t bb = eval.pieces[color][type_index(PieceType::knight)]; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            const std::uint64_t pa = detail::knight_attacks(sq);
            by2 |= attacks & pa;
            attacks |= pa;
            eval.attacks_by[color][type_index(PieceType::knight)] |= pa;
            if ((pa & ext_ring[enemy]) != 0) {
                ++eval.king_ring_attackers[enemy][0];
            }
            const int mob = std::min(8, std::popcount(pa & mob_area));
            eval.mobility_scores[color][0] += kKnightMobility[static_cast<std::size_t>(mob)];
        }

        const std::uint64_t bishop_xray_occ =
            eval.occupied ^ eval.pieces[color][type_index(PieceType::queen)];
        for (std::uint64_t bb = eval.pieces[color][type_index(PieceType::bishop)]; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            const std::uint64_t pa = detail::bishop_attacks(sq, eval.occupied);
            by2 |= attacks & pa;
            attacks |= pa;
            eval.attacks_by[color][type_index(PieceType::bishop)] |= pa;
            if ((pa & ext_ring[enemy]) != 0) {
                ++eval.king_ring_attackers[enemy][1];
            }
            const std::uint64_t mob_bb = detail::bishop_attacks(sq, bishop_xray_occ);
            const int mob = std::min(13, std::popcount(mob_bb & mob_area));
            eval.mobility_scores[color][1] += kBishopMobility[static_cast<std::size_t>(mob)];
        }

        const std::uint64_t rook_xray_occ =
            bishop_xray_occ ^ eval.pieces[color][type_index(PieceType::rook)];
        for (std::uint64_t bb = eval.pieces[color][type_index(PieceType::rook)]; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            const std::uint64_t pa = detail::rook_attacks(sq, eval.occupied);
            by2 |= attacks & pa;
            attacks |= pa;
            eval.attacks_by[color][type_index(PieceType::rook)] |= pa;
            if ((pa & ext_ring[enemy]) != 0) {
                ++eval.king_ring_attackers[enemy][2];
            }
            const std::uint64_t mob_bb = detail::rook_attacks(sq, rook_xray_occ);
            const int mob = std::min(14, std::popcount(mob_bb & mob_area));
            eval.mobility_scores[color][2] += kRookMobility[static_cast<std::size_t>(mob)];
        }

        for (std::uint64_t bb = eval.pieces[color][type_index(PieceType::queen)]; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            const std::uint64_t pa = detail::queen_attacks(sq, eval.occupied);
            by2 |= attacks & pa;
            attacks |= pa;
            eval.attacks_by[color][type_index(PieceType::queen)] |= pa;
            if ((pa & ext_ring[enemy]) != 0) {
                ++eval.king_ring_attackers[enemy][3];
            }
            const int mob = std::min(27, std::popcount(pa & mob_area));
            eval.mobility_scores[color][3] += kQueenMobility[static_cast<std::size_t>(mob)];
        }

        if (eval.kings[color] >= 0) {
            const std::uint64_t ka = detail::king_attacks(eval.kings[color]);
            by2 |= attacks & ka;
            attacks |= ka;
            eval.attacks_by[color][type_index(PieceType::king)] = ka;
        }
        eval.attacked_squares[color] = attacks;
        eval.attacked_by2[color] = by2;
        eval.center_control[color] =
            static_cast<std::uint8_t>(std::popcount(attacks & kCenterMask));
    }
    return eval;
}

[[nodiscard]] Score pawn_structure(const EvalBoard& eval, const std::size_t color,
                                   const HceTuning& tuning) noexcept {
    Score score{};
    const std::size_t enemy = 1 - color;
    const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];
    const std::uint64_t enemy_pawns = eval.pieces[enemy][type_index(PieceType::pawn)];
    const std::uint64_t own_pawn_att = eval.pawn_attacks[color];
    const std::uint64_t enemy_pawn_att = eval.pawn_attacks[enemy];
    const bool white = color == kWhite;

    std::array<int, 8> own_file_count{};
    std::uint64_t passed_bb = 0;
    for (std::uint64_t pawns = own_pawns; pawns != 0;) {
        const int square = std::countr_zero(pawns);
        pawns &= pawns - 1;
        ++own_file_count[static_cast<std::size_t>(file_of(square))];
        if ((enemy_pawns & kPassedPawnMask[color][static_cast<std::size_t>(square)]) == 0) {
            passed_bb |= bit_of(square);
        }
    }

    for (int file = 0; file < 8; ++file) {
        if (own_file_count[static_cast<std::size_t>(file)] > 1) {
            const int extra = own_file_count[static_cast<std::size_t>(file)] - 1;
            score += scaled(kDoubledPawnPenalty, extra, extra);
        }
    }

    const bool enemy_no_pieces =
        (eval.colors[enemy] & ~(eval.pieces[enemy][type_index(PieceType::king)] | enemy_pawns)) ==
        0;

    for (std::uint64_t pawns = own_pawns; pawns != 0;) {
        const int square = std::countr_zero(pawns);
        pawns &= pawns - 1;
        const int file = file_of(square);
        const int rank = rank_of(square);
        const int own_rank = std::clamp(white ? rank : 7 - rank, 1, 6);
        const std::uint64_t square_bit = bit_of(square);
        const std::uint64_t fwd = kForwardFileMask[color][static_cast<std::size_t>(square)];
        const bool opposed = (enemy_pawns & fwd) != 0;
        const bool isolated =
            (own_pawns & kAdjacentFileMask[static_cast<std::size_t>(file)]) == 0;
        const bool phalanx = (own_pawns & kPhalanxMask[static_cast<std::size_t>(square)]) != 0;
        const bool supported = (own_pawn_att & square_bit) != 0;
        const bool passed = (passed_bb & square_bit) != 0;
        const int ahead_square = white ? (square + 8) : (square - 8);
        const std::uint64_t ahead_bit = bit_of(ahead_square);

        if (isolated) {
            score += opposed ? kIsolatedClosedPenalty : kIsolatedOpenPenalty;
        } else if (!passed &&
                   (own_pawns & kBehindAdjacentMask[color][static_cast<std::size_t>(square)]) ==
                       0 &&
                   (((enemy_pawns | enemy_pawn_att) & ahead_bit) != 0)) {
            score += opposed ? kBackwardClosedPenalty : kBackwardOpenPenalty;
        }
        if (phalanx) {
            score += kPhalanxPawnRankBonus[static_cast<std::size_t>(own_rank - 1)];
        }
        if (supported) {
            score += kSupportedPawnRankBonus[static_cast<std::size_t>(own_rank - 1)];
        }

        if (passed) {
            static constexpr std::array<int, 8> kPassedRankFactor{{0, 0, 0, 1, 3, 6, 10, 0}};
            const int rank_factor = kPassedRankFactor[static_cast<std::size_t>(own_rank)];
            Score bonus = percent_of(
                kPassedPawnRankBonus[static_cast<std::size_t>(own_rank)],
                tuning.passed_rank);
            if (eval.kings[color] >= 0 && eval.kings[enemy] >= 0) {
                const int own_king_distance = square_distance(eval.kings[color], square);
                const int enemy_king_distance = square_distance(eval.kings[enemy], square);
                bonus += scaled(kPassedOwnKingDist, own_king_distance, own_king_distance);
                bonus += scaled(kPassedEnemyKingDist, enemy_king_distance, enemy_king_distance);
                if (rank_factor > 0) {
                    const int own_rank_dist = rank_factor * own_king_distance;
                    const int enemy_rank_dist = rank_factor * enemy_king_distance;
                    bonus += scaled(kPassedOwnKingRankDist, own_rank_dist, own_rank_dist);
                    bonus += scaled(kPassedEnemyKingRankDist, enemy_rank_dist, enemy_rank_dist);
                }
            }
            if ((eval.occupied & ahead_bit) != 0) {
                bonus += kPassedBlockedBonus;
            } else {
                if ((eval.attacked_squares[enemy] & ahead_bit) != 0) {
                    bonus += kPassedUnsafeAdvanceBonus;
                } else {
                    bonus += kPassedSafeAdvanceBonus;
                }
                if ((eval.occupied & fwd) == 0 && (eval.attacked_squares[enemy] & fwd) == 0) {
                    bonus += kPassedFreeSafePathBonus;
                }
            }
            if ((passed_bb & kConnectedPawnMask[color][static_cast<std::size_t>(square)]) != 0) {
                bonus += kConnectedPasserBonus;
            }
            const std::uint64_t behind_file =
                kForwardFileMask[enemy][static_cast<std::size_t>(square)];
            if ((eval.pieces[color][type_index(PieceType::rook)] & behind_file) != 0) {
                bonus += kRookBehindOwnPasser;
            }
            if ((eval.pieces[enemy][type_index(PieceType::rook)] & behind_file) != 0) {
                bonus += kRookBehindEnemyPasser;
            }
            if (enemy_no_pieces && (eval.colors[color] & fwd) == 0 && eval.kings[enemy] >= 0) {
                const int promo_square = (white ? 56 : 0) + file;
                const int steps = 7 - own_rank - (own_rank == 1 ? 1 : 0);
                const int tempo_adj =
                    eval.side_to_move == (white ? Color::white : Color::black) ? 0 : 1;
                if (steps + tempo_adj < square_distance(eval.kings[enemy], promo_square)) {
                    bonus += kUnstoppablePasserBonus;
                }
            }
            score += percent_of(bonus, tuning.passed);
        } else if (!opposed) {
            if (phalanx || supported ||
                std::popcount(own_pawns &
                              kBehindAdjacentMask[color][static_cast<std::size_t>(square)]) >=
                    std::popcount(enemy_pawns &
                                  kPassedPawnMask[color][static_cast<std::size_t>(square)])) {
                score += kCandidatePasserRankBonus[static_cast<std::size_t>(own_rank - 1)];
            }
        }
    }
    return score;
}

[[nodiscard]] Score king_safety(const EvalBoard& eval, const std::size_t color) noexcept {
    const int king = eval.kings[color];
    if (king < 0) {
        return {};
    }
    Score score{};
    const int king_file = file_of(king);
    const int king_rank = rank_of(king);
    const bool white = color == kWhite;
    const std::size_t enemy = 1 - color;
    const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];
    const std::uint64_t enemy_pawns = eval.pieces[enemy][type_index(PieceType::pawn)];

    const bool enemy_has_pieces =
        (eval.pieces[enemy][type_index(PieceType::knight)] |
         eval.pieces[enemy][type_index(PieceType::bishop)] |
         eval.pieces[enemy][type_index(PieceType::rook)] |
         eval.pieces[enemy][type_index(PieceType::queen)]) != 0;
    if (enemy_has_pieces) {
        const std::uint64_t def_non_king =
            eval.attacks_by[color][type_index(PieceType::pawn)] |
            eval.attacks_by[color][type_index(PieceType::knight)] |
            eval.attacks_by[color][type_index(PieceType::bishop)] |
            eval.attacks_by[color][type_index(PieceType::rook)] |
            eval.attacks_by[color][type_index(PieceType::queen)];
        const std::uint64_t weak =
            eval.king_ring[color] & eval.attacked_squares[enemy] & ~def_non_king;
        const std::uint64_t king_only_def =
            eval.attacks_by[color][type_index(PieceType::king)] & ~def_non_king;
        const std::uint64_t safe_sq = ~eval.colors[enemy] &
            (~eval.attacked_squares[color] | (king_only_def & eval.attacked_by2[enemy]));
        const std::uint64_t b_chk = detail::bishop_attacks(king, eval.occupied);
        const std::uint64_t r_chk = detail::rook_attacks(king, eval.occupied);
        const std::uint64_t n_chk = detail::knight_attacks(king);
        const std::uint64_t q_chk = b_chk | r_chk;

        int danger = kKingDangerBase;
        for (std::size_t idx = 0; idx < 4; ++idx) {
            danger += kKingDangerRingWeight[idx] * eval.king_ring_attackers[color][idx];
        }
        danger += kKingDangerSafeCheckWeight[0] *
            std::popcount(n_chk & eval.attacks_by[enemy][type_index(PieceType::knight)] & safe_sq);
        danger += kKingDangerSafeCheckWeight[1] *
            std::popcount(b_chk & eval.attacks_by[enemy][type_index(PieceType::bishop)] & safe_sq);
        danger += kKingDangerSafeCheckWeight[2] *
            std::popcount(r_chk & eval.attacks_by[enemy][type_index(PieceType::rook)] & safe_sq);
        danger += kKingDangerSafeCheckWeight[3] *
            std::popcount(q_chk & eval.attacks_by[enemy][type_index(PieceType::queen)] & safe_sq);
        danger += kKingDangerWeakRingWeight * std::popcount(weak);
        if (eval.pieces[enemy][type_index(PieceType::queen)] == 0) {
            danger += kKingDangerNoQueenWeight;
        }
        if (danger > 0) {
            score.mg -= (danger * danger) / 1024;
            score.eg -= danger / 16;
        }
    }

    const int king_own_rank = white ? king_rank : 7 - king_rank;
    const int center_file = std::clamp(king_file, 1, 6);
    for (int file = center_file - 1; file <= center_file + 1; ++file) {
        const std::uint64_t file_mask = 0x0101010101010101ULL << static_cast<unsigned>(file);
        const std::uint64_t own_on = own_pawns & file_mask;
        const std::uint64_t enemy_on = enemy_pawns & file_mask;
        if (own_on == 0) {
            score += enemy_on == 0 ? kKingOpenFilePenalty : kKingSemiOpenFilePenalty;
        }

        int best_delta = 99;
        for (std::uint64_t bb = own_on; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            const int pr = white ? rank_of(sq) : 7 - rank_of(sq);
            if (pr >= king_own_rank && pr - king_own_rank < best_delta) {
                best_delta = pr - king_own_rank;
            }
        }
        const int own_d = best_delta < 99 ? (std::min(3, best_delta) + 1) : 0;
        const auto& shelter_table =
            file == king_file ? kKingShelterOwnFile : kKingShelterAdjFile;
        score += shelter_table[static_cast<std::size_t>(own_d)];

        int best_er = 99;
        int best_esq = -1;
        for (std::uint64_t bb = enemy_on; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            const int pr = white ? rank_of(sq) : 7 - rank_of(sq);
            if (pr > king_own_rank && pr <= 5 && pr < best_er) {
                best_er = pr;
                best_esq = sq;
            }
        }
        if (best_er <= 5) {
            const int front_sq = best_esq + (white ? -8 : 8);
            const bool blocked =
                front_sq >= 0 && front_sq < 64 && ((own_pawns & bit_of(front_sq)) != 0);
            const auto& storm_table = blocked ? kKingStormBlocked : kKingStormUnblocked;
            score += storm_table[static_cast<std::size_t>(best_er - 1)];
        }
    }
    return score;
}

[[nodiscard]] Score threats(const EvalBoard& eval, const std::size_t color) noexcept {
    Score score{};
    const std::size_t enemy = 1 - color;
    const std::uint64_t own_attacks = eval.attacked_squares[color];
    const std::uint64_t enemy_defended = eval.attacked_squares[enemy];

    for (std::size_t pt = type_index(PieceType::knight);
         pt <= type_index(PieceType::queen); ++pt) {
        std::uint64_t attacked = eval.pieces[enemy][pt] & own_attacks;
        while (attacked != 0) {
            const int square = std::countr_zero(attacked);
            attacked &= attacked - 1;
            const std::uint64_t square_bit = bit_of(square);
            if ((enemy_defended & square_bit) == 0) {
                score += kHangingBonus[pt - type_index(PieceType::knight)];
            } else if ((eval.pawn_attacks[color] & square_bit) != 0) {
                score += kPawnAttackDefendedBonus;
            }
        }
    }
    return score;
}

[[nodiscard]] Score piece_features(const EvalBoard& eval, const std::size_t color) noexcept {
    constexpr std::uint64_t kDarkSquares = 0xAA55AA55AA55AA55ULL;
    constexpr std::uint64_t kLightSquares = ~kDarkSquares;
    constexpr std::array<std::uint64_t, 2> kOutpostRanks{
        0x0000FFFFFF000000ULL,   // ranks 4..6 for White
        0x000000FFFFFF0000ULL};  // ranks 3..5 for Black

    Score score{};
    const bool white = color == kWhite;
    const std::size_t enemy = 1 - color;
    const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];
    const std::uint64_t enemy_pawns = eval.pieces[enemy][type_index(PieceType::pawn)];
    const std::uint64_t all_pawns = own_pawns | enemy_pawns;
    const std::uint64_t own_pawn_attacks = eval.pawn_attacks[color];
    const int own_pawn_count = std::popcount(own_pawns);

    if (std::popcount(eval.pieces[color][type_index(PieceType::bishop)]) >= 2) {
        score += kBishopPairBonus;
    }

    const std::uint64_t knights = eval.pieces[color][type_index(PieceType::knight)];
    for (std::uint64_t bb = knights; bb != 0;) {
        const int square = std::countr_zero(bb);
        bb &= bb - 1;
        const std::uint64_t square_bit = bit_of(square);
        const std::uint64_t ahead_adj =
            kPassedPawnMask[color][static_cast<std::size_t>(square)] &
            ~kForwardFileMask[color][static_cast<std::size_t>(square)];
        if ((kOutpostRanks[color] & square_bit) != 0 && (enemy_pawns & ahead_adj) == 0) {
            score += (own_pawn_attacks & square_bit) != 0
                ? kKnightOutpostSupported
                : kKnightOutpostUnsupported;
        }
    }

    const std::uint64_t bishops = eval.pieces[color][type_index(PieceType::bishop)];
    for (std::uint64_t bb = bishops; bb != 0;) {
        const int square = std::countr_zero(bb);
        bb &= bb - 1;
        const std::uint64_t square_bit = bit_of(square);
        const std::uint64_t ahead_adj =
            kPassedPawnMask[color][static_cast<std::size_t>(square)] &
            ~kForwardFileMask[color][static_cast<std::size_t>(square)];
        if ((kOutpostRanks[color] & square_bit) != 0 && (enemy_pawns & ahead_adj) == 0) {
            score += (own_pawn_attacks & square_bit) != 0
                ? kBishopOutpostSupported
                : kBishopOutpostUnsupported;
        }
        const std::uint64_t color_mask =
            (square_bit & kDarkSquares) != 0 ? kDarkSquares : kLightSquares;
        const int same_color_pawns = std::popcount(own_pawns & color_mask);
        score += scaled(kBishopSameColorPawn, same_color_pawns, same_color_pawns);
        if (std::popcount(detail::bishop_attacks(square, all_pawns) & kCenterMask) >= 2) {
            score += kBishopLongDiagonal;
        }
    }

    const std::uint64_t minors = knights | bishops;
    const std::uint64_t behind_mask = white ? (own_pawns >> 8) : (own_pawns << 8);
    const int minor_behind_count = std::popcount(behind_mask & minors);
    score += scaled(kMinorBehindPawn, minor_behind_count, minor_behind_count);

    const std::uint64_t seventh_enemy_mask =
        white ? 0x00FF000000000000ULL : 0x000000000000FF00ULL;
    const bool enemy_king_back_rank = eval.kings[enemy] >= 0 &&
        (white ? rank_of(eval.kings[enemy]) == 7 : rank_of(eval.kings[enemy]) == 0);
    const bool seventh_valid = enemy_king_back_rank || ((enemy_pawns & seventh_enemy_mask) != 0);

    const int k_norm = eval.kings[color] >= 0
        ? (white ? eval.kings[color] : (eval.kings[color] ^ 56))
        : -1;
    const int king_rank_norm = k_norm >= 0 ? rank_of(k_norm) : -1;
    const int king_file_norm = k_norm >= 0 ? file_of(k_norm) : -1;

    const std::uint64_t own_rooks = eval.pieces[color][type_index(PieceType::rook)];
    for (std::uint64_t bb = own_rooks; bb != 0;) {
        const int square = std::countr_zero(bb);
        bb &= bb - 1;
        const int file = file_of(square);
        const std::uint64_t file_mask = 0x0101010101010101ULL << static_cast<unsigned>(file);
        const bool own_pawn_on_file = (own_pawns & file_mask) != 0;
        const bool any_pawn_on_file = (all_pawns & file_mask) != 0;
        if (!any_pawn_on_file) {
            score += kRookOpenFileBonus;
        } else if (!own_pawn_on_file) {
            score += kRookSemiOpenFileBonus;
        }
        if ((eval.pieces[enemy][type_index(PieceType::queen)] & file_mask) != 0) {
            score += kRookOnQueenFileBonus;
        }
        const int own_rank = white ? rank_of(square) : 7 - rank_of(square);
        if (own_rank == 6 && seventh_valid) {
            score += kRookSeventhBonus;
        }
        if ((detail::rook_attacks(square, eval.occupied) & (own_rooks ^ bit_of(square))) != 0) {
            score += kRookConnectedBonus;
        }
        if (king_rank_norm == 0) {
            const int r_norm = white ? square : (square ^ 56);
            const int rr = rank_of(r_norm);
            const int rf = file_of(r_norm);
            if (rr <= 1 &&
                (((king_file_norm == 5 || king_file_norm == 6) && rf >= 6 &&
                  rf > king_file_norm) ||
                 ((king_file_norm >= 1 && king_file_norm <= 3) && rf <= 1 &&
                  rf < king_file_norm))) {
                score += kTrappedRookPenalty;
            }
        }
    }

    const int knight_pawn_term = std::popcount(knights) * own_pawn_count;
    const int rook_pawn_term = std::popcount(own_rooks) * own_pawn_count;
    score += scaled(kKnightPerOwnPawn, knight_pawn_term, knight_pawn_term);
    score += scaled(kRookPerOwnPawn, rook_pawn_term, rook_pawn_term);
    return score;
}

[[nodiscard]] bool insufficient_material(const EvalBoard& eval) noexcept {
    for (std::size_t color = 0; color < 2; ++color) {
        const std::uint64_t non_king = eval.colors[color] &
            ~eval.pieces[color][type_index(PieceType::king)];
        const std::uint64_t pawns = eval.pieces[color][type_index(PieceType::pawn)];
        const std::uint64_t rooks = eval.pieces[color][type_index(PieceType::rook)];
        const std::uint64_t queens = eval.pieces[color][type_index(PieceType::queen)];
        if (pawns != 0 || rooks != 0 || queens != 0) {
            return false;
        }
        const int minors = std::popcount(non_king);
        if (minors > 1) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool opposite_bishop_draw(const EvalBoard& eval) noexcept {
    for (std::size_t color = 0; color < 2; ++color) {
        const std::uint64_t non_king = eval.colors[color] &
            ~eval.pieces[color][type_index(PieceType::king)];
        const int minors = std::popcount(non_king);
        if (minors > 1) {
            return false;
        }
    }
    int bishop_square[2] = {-1, -1};
    for (std::size_t color = 0; color < 2; ++color) {
        const std::uint64_t bishops = eval.pieces[color][type_index(PieceType::bishop)];
        const std::uint64_t others = eval.colors[color] &
            ~eval.pieces[color][type_index(PieceType::king)] & ~bishops;
        if (others != 0) {
            return false;
        }
        if (bishops != 0) {
            if (std::popcount(bishops) != 1) {
                return false;
            }
            bishop_square[color] = std::countr_zero(bishops);
        }
    }
    if (bishop_square[0] < 0 || bishop_square[1] < 0) {
        return false;
    }
    const bool white_square_bishop_0 =
        (file_of(bishop_square[0]) + rank_of(bishop_square[0])) % 2 == 0;
    const bool white_square_bishop_1 =
        (file_of(bishop_square[1]) + rank_of(bishop_square[1])) % 2 == 0;
    return white_square_bishop_0 != white_square_bishop_1;
}

[[nodiscard]] int tapered(const Score score, int phase) noexcept {
    phase = std::clamp(phase, 0, kPhaseMaximum);
    return (score.mg * phase + score.eg * (kPhaseMaximum - phase)) / kPhaseMaximum;
}

constexpr void add_trace_scaled(ClassicalScore& dst, const Score value, const int sign) noexcept {
    dst.mg += value.mg * sign;
    dst.eg += value.eg * sign;
}

template <bool CollectTrace>
auto evaluate_classical_impl(const GameState& state, const Color perspective) {
    const EvalBoard eval = make_eval_board(state);
    [[maybe_unused]] ClassicalEvalTrace tr{};
    if constexpr (CollectTrace) {
        tr.phase = eval.phase;
    }
    if (insufficient_material(eval)) {
        if constexpr (CollectTrace) {
            tr.insufficient_material = true;
            return tr;
        } else {
            return 0;
        }
    }

    const HceTuning& tuning = hce_tuning();
    Score total{};

    for (std::size_t color = 0; color < 2; ++color) {
        const int sign = sign_of(color);
        Score mat_side{};
        Score pst_side{};

        for (std::size_t pt = type_index(PieceType::pawn);
             pt <= type_index(PieceType::king); ++pt) {
            std::uint64_t bb = eval.pieces[color][pt];
            if (bb == 0) {
                continue;
            }
            const int count = std::popcount(bb);
            mat_side += scaled(kMaterialWeights[pt], count, count);
            const auto& table = piece_square_table(static_cast<PieceType>(pt));
            while (bb != 0) {
                const int square = std::countr_zero(bb);
                bb &= bb - 1;
                const int normalized = color == kWhite ? square : (square ^ 56);
                pst_side += table[static_cast<std::size_t>(normalized)];
            }
        }

        if constexpr (CollectTrace) {
            add_trace_scaled(tr.material, mat_side, sign);
            add_trace_scaled(tr.pst, pst_side, sign);
        }
        total += scaled(mat_side + pst_side, sign, sign);

        Score side{};
        const std::array<int, 4> mob_piece_scales{{
            tuning.mobility_knight,
            tuning.mobility_bishop,
            tuning.mobility_rook,
            tuning.mobility_queen,
        }};
        for (std::size_t mob_idx = 0; mob_idx < 4; ++mob_idx) {
            const Score mob_raw = eval.mobility_scores[color][mob_idx];
            if (mob_raw.mg == 0 && mob_raw.eg == 0) {
                continue;
            }
            const Score mob_term = percent_of(
                mob_raw, tuning.mobility * mob_piece_scales[mob_idx] / 100);
            side += mob_term;
            if constexpr (CollectTrace) {
                add_trace_scaled(tr.mobility, mob_term, sign);
            }
        }

        const Score pawns_term =
            percent_of(pawn_structure(eval, color, tuning), tuning.pawn_structure);
        const Score king_term = percent_of(king_safety(eval, color), tuning.king_safety);
        const Score threats_term = percent_of(threats(eval, color), tuning.threats);
        const Score piece_term = percent_of(piece_features(eval, color), tuning.piece_terms);
        const Score center_term =
            scaled(kCenterControlBonus, eval.center_control[color], eval.center_control[color]);
        const Score dev_term =
            scaled(kDevelopmentBonus, eval.development[color], eval.development[color]);

        side += pawns_term;
        side += king_term;
        side += threats_term;
        side += piece_term;
        side += center_term;
        side += dev_term;

        if constexpr (CollectTrace) {
            add_trace_scaled(tr.pawns, pawns_term, sign);
            add_trace_scaled(tr.king_safety, king_term, sign);
            add_trace_scaled(tr.threats, threats_term, sign);
            add_trace_scaled(tr.piece_terms, piece_term, sign);
            add_trace_scaled(tr.space_dev, center_term + dev_term, sign);
        }

        total += scaled(side, sign, sign);
    }

    const Score tempo = eval.side_to_move == Color::white
        ? kTempoBonus
        : Score{-kTempoBonus.mg, -kTempoBonus.eg};
    const Score tempo_term = percent_of(tempo, tuning.tempo);
    total += tempo_term;

    total.eg = total.eg * tuning.eg_component / 100;
    int score = tapered(total, eval.phase);
    // Opposite bishops with no other material is drawish, but not a strict
    // rule draw; damp rather than zero it so small residual imbalances survive.
    if (opposite_bishop_draw(eval)) {
        if constexpr (CollectTrace) {
            tr.opposite_bishop_draw = true;
        }
        score = score / 4;
    }
    score = score * tuning.evaluation / 100;
    score = std::clamp(score, -detail::kMateThreshold + 1, detail::kMateThreshold - 1);

    if constexpr (CollectTrace) {
        tr.tempo = {tempo_term.mg, tempo_term.eg};
        tr.total = {total.mg, total.eg};
        tr.total_white_cp = score;
        return tr;
    } else {
        return perspective == Color::white ? score : -score;
    }
}

} // namespace

int ClassicalEvaluator::evaluate(const GameState& state, Color perspective) const {
    return evaluate_classical_impl<false>(state, perspective);
}

ClassicalEvalTrace ClassicalEvaluator::trace(const GameState& state) const {
    return evaluate_classical_impl<true>(state, Color::white);
}

} // namespace koi
