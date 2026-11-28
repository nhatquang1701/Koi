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
    {0, 0}, {24, 182}, {422, 445}, {564, 494}, {513, 864}, {1531, 1486}, {0, 0}
}};

inline constexpr std::array<Score, 64> kPawnPst{{
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {-33, 5}, {-16, -1}, {-24, -2}, {-31, -14}, {-19, 10}, {-2, -2}, {22, -10}, {-24, -17},
    {-31, -9}, {-39, -10}, {-14, -25}, {-35, -20}, {-7, -22}, {-36, -17}, {1, -25}, {-32, -24},
    {-26, -4}, {-26, -7}, {-15, -28}, {8, -36}, {12, -39}, {-1, -32}, {-9, -17}, {-17, -27},
    {-9, 5}, {-5, 0}, {4, -16}, {5, -33}, {31, -35}, {20, -23}, {15, -11}, {9, -13},
    {2, 23}, {20, 23}, {26, 21}, {53, 4}, {40, 23}, {75, 21}, {28, 41}, {31, 16},
    {8, 11}, {24, 38}, {21, 71}, {49, 79}, {40, 64}, {21, 67}, {-43, 53}, {-34, 50},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}
}};

inline constexpr std::array<Score, 64> kKnightPst{{
    {-30, -17}, {-28, -42}, {-33, 11}, {-15, 1}, {-4, 0}, {-14, -9}, {-27, -33}, {-26, -21},
    {-52, -4}, {-23, -9}, {-15, 2}, {-6, 0}, {-1, 2}, {0, -3}, {-15, -3}, {-17, -2},
    {-49, -9}, {-13, -1}, {-1, 7}, {8, 21}, {31, 19}, {5, 3}, {23, 2}, {-33, 0},
    {-17, -6}, {-15, 1}, {22, 20}, {18, 36}, {39, 32}, {34, 16}, {24, 13}, {12, -5},
    {11, 1}, {11, 7}, {18, 32}, {70, 33}, {21, 40}, {75, 31}, {9, 21}, {39, 0},
    {-7, -20}, {21, -6}, {-1, 35}, {52, 20}, {82, 6}, {26, 19}, {28, -5}, {-3, -12},
    {-12, -28}, {-28, -5}, {26, 9}, {22, 29}, {8, 10}, {67, -1}, {-4, -24}, {24, -27},
    {-140, -75}, {-10, -10}, {-15, 0}, {-22, 3}, {-18, -9}, {-31, -9}, {-11, -20}, {-91, -65}
}};

inline constexpr std::array<Score, 64> kBishopPst{{
    {-19, -19}, {36, -9}, {-5, -11}, {-6, -3}, {-12, -6}, {-31, 3}, {1, 3}, {-30, -27},
    {15, -4}, {6, -29}, {27, -1}, {-3, 7}, {-1, 4}, {23, -9}, {16, -25}, {24, -33},
    {-6, -4}, {28, 15}, {-2, 10}, {2, 25}, {15, 27}, {-3, -3}, {35, -4}, {-1, 0},
    {-13, -6}, {0, 10}, {-10, 32}, {39, 21}, {24, 22}, {6, 22}, {-8, 3}, {42, -34},
    {-7, -8}, {3, 13}, {14, 18}, {24, 25}, {40, 25}, {23, 7}, {16, 9}, {-25, 0},
    {-22, -2}, {10, -2}, {-20, -7}, {29, 2}, {19, 14}, {20, 11}, {29, 7}, {21, -4},
    {-11, -13}, {-7, -17}, {-6, 15}, {-21, -6}, {-25, 13}, {-32, 1}, {-7, -16}, {-20, -16},
    {-47, -19}, {-20, 5}, {-26, 1}, {-26, -7}, {-17, -5}, {-34, -1}, {-17, -9}, {-47, -8}
}};

inline constexpr std::array<Score, 64> kRookPst{{
    {-22, -15}, {-18, -17}, {-19, -8}, {-22, -9}, {2, -26}, {-10, -16}, {17, -24}, {-1, -29},
    {-31, -15}, {-28, -13}, {-27, -5}, {-26, -10}, {-12, -16}, {-9, -24}, {0, -33}, {-20, -31},
    {-31, -6}, {-7, -14}, {-6, -8}, {-33, -7}, {-23, -13}, {-12, -6}, {35, -31}, {11, -31},
    {-20, 4}, {-15, 2}, {-16, 5}, {-16, -1}, {-17, -6}, {-13, -5}, {13, -14}, {-10, -13},
    {-7, 19}, {-15, 15}, {-5, 23}, {0, 9}, {12, 0}, {9, 10}, {18, 3}, {33, -6},
    {-2, 24}, {7, 23}, {12, 22}, {12, 13}, {33, 0}, {33, 12}, {36, 14}, {36, 9},
    {0, 3}, {-30, 19}, {-8, 23}, {13, 12}, {15, 5}, {28, 5}, {31, 3}, {28, 8},
    {3, 16}, {5, 24}, {1, 29}, {-2, 25}, {33, 12}, {20, 17}, {15, 19}, {22, 23}
}};

inline constexpr std::array<Score, 64> kQueenPst{{
    {-37, -44}, {-32, -45}, {-12, -48}, {7, -84}, {5, -60}, {-42, -46}, {-24, -37}, {-16, -25},
    {-16, -32}, {-14, -35}, {14, -54}, {22, -58}, {20, -45}, {16, -58}, {17, -80}, {7, -38},
    {-14, -15}, {2, -13}, {-12, 2}, {-8, 8}, {-2, 13}, {3, 13}, {14, -14}, {3, -20},
    {14, -19}, {-26, 13}, {-17, 25}, {-12, 40}, {-5, 36}, {-8, 36}, {1, 8}, {14, 1},
    {-13, -9}, {-19, 23}, {-3, 29}, {-13, 56}, {-23, 85}, {8, 59}, {-1, 47}, {21, 20},
    {-11, -15}, {-1, 1}, {-4, 26}, {12, 37}, {27, 51}, {55, 66}, {47, 32}, {28, 26},
    {-11, -20}, {-43, 20}, {-18, 37}, {-1, 35}, {-6, 34}, {36, 55}, {3, 26}, {36, 24},
    {-39, -30}, {-13, -28}, {0, 0}, {26, 7}, {25, 17}, {34, 22}, {5, -31}, {-9, -28}
}};

inline constexpr std::array<Score, 64> kKingPst{{
    {-15, -59}, {96, -76}, {75, -56}, {-30, -43}, {39, -70}, {-17, -44}, {87, -79}, {59, -102},
    {17, -39}, {57, -46}, {11, -22}, {-17, -19}, {-11, -17}, {1, -24}, {76, -45}, {47, -56},
    {-24, -39}, {15, -38}, {-18, -17}, {-44, -7}, {-31, -6}, {-28, -13}, {13, -30}, {-60, -35},
    {-32, -29}, {-26, -11}, {-21, -1}, {-37, 15}, {-59, 16}, {-25, 4}, {-30, -10}, {-50, -25},
    {-6, -6}, {-4, 18}, {-15, 40}, {-26, 43}, {-28, 39}, {-27, 44}, {-6, 23}, {-25, 7},
    {1, 17}, {9, 48}, {8, 62}, {1, 66}, {5, 67}, {9, 63}, {9, 52}, {-4, 26},
    {-1, 9}, {15, 53}, {6, 64}, {9, 54}, {12, 63}, {1, 65}, {-2, 68}, {3, 28},
    {5, -13}, {5, -3}, {8, 6}, {1, 15}, {5, 4}, {4, 1}, {4, 19}, {5, -20}
}};

inline constexpr std::array<Score, 9> kKnightMobility{{
    {-31, -7}, {-31, -17}, {-18, -9}, {-12, -5}, {1, 1}, {13, 11}, {25, 11},
    {34, 14}, {20, 2}
}};

inline constexpr std::array<Score, 14> kBishopMobility{{
    {-56, -18}, {-43, -16}, {-15, -19}, {-6, -7}, {6, 0}, {14, 5}, {19, 11},
    {22, 13}, {24, 12}, {20, 11}, {11, 8}, {5, 3}, {0, 1}, {0, -2}
}};

inline constexpr std::array<Score, 15> kRookMobility{{
    {-10, -5}, {-38, -23}, {-17, -23}, {-9, -15}, {-15, -6}, {-9, -5}, {-4, 0},
    {1, 0}, {9, 5}, {15, 10}, {17, 11}, {17, 16}, {19, 17}, {14, 17},
    {9, 2}
}};

inline constexpr std::array<Score, 28> kQueenMobility{{
    {0, 0}, {-4, 0}, {-12, -1}, {-16, -2}, {-16, -4}, {-15, -5}, {-13, -9},
    {-9, -9}, {-6, -7}, {-2, -6}, {2, -2}, {4, -3}, {5, 4}, {5, 3},
    {9, 3}, {9, 8}, {11, 6}, {9, 8}, {8, 6}, {11, 7}, {8, 5},
    {5, 0}, {4, 0}, {2, 0}, {1, -1}, {0, -1}, {0, 0}, {0, -1}
}};

inline constexpr Score kDoubledPawnPenalty{-2, -28};
inline constexpr Score kIsolatedOpenPenalty{-23, -12};
inline constexpr Score kIsolatedClosedPenalty{-12, -3};
inline constexpr Score kBackwardOpenPenalty{-18, -15};
inline constexpr Score kBackwardClosedPenalty{1, -1};
inline constexpr std::array<Score, 6> kPhalanxPawnRankBonus{{
    {4, 0}, {13, 4}, {17, 10}, {23, 25}, {6, 22}, {2, 6}
}};
inline constexpr std::array<Score, 6> kSupportedPawnRankBonus{{
    {0, 0}, {18, 12}, {17, 8}, {33, 19}, {26, 30}, {4, 14}
}};
inline constexpr std::array<Score, 6> kCandidatePasserRankBonus{{
    {5, -11}, {9, 0}, {8, 16}, {5, 21}, {0, 4}, {0, 0}
}};
inline constexpr std::array<Score, 8> kPassedPawnRankBonus{{
    {0, 0}, {1, -13}, {1, 1}, {-10, 16}, {-3, 21}, {-1, 24}, {4, 22}, {0, 0}
}};
inline constexpr Score kPassedOwnKingDist{6, -9};
inline constexpr Score kPassedEnemyKingDist{-1, -3};
inline constexpr Score kPassedOwnKingRankDist{2, -3};
inline constexpr Score kPassedEnemyKingRankDist{-2, 10};
inline constexpr Score kPassedBlockedBonus{-7, 6};
inline constexpr Score kPassedSafeAdvanceBonus{-1, 37};
inline constexpr Score kPassedUnsafeAdvanceBonus{1, 28};
inline constexpr Score kPassedFreeSafePathBonus{-32, 54};
inline constexpr Score kConnectedPasserBonus{7, -4};
inline constexpr Score kRookBehindOwnPasser{6, 13};
inline constexpr Score kRookBehindEnemyPasser{-12, -53};
inline constexpr Score kUnstoppablePasserBonus{-1, 91};
inline constexpr Score kKingAttackerWeight{2, 13};
inline constexpr Score kKingAttackerSqWeight{-10, 1};
inline constexpr Score kKingAttackerGe3Weight{-17, -8};
inline constexpr Score kKingMissingShieldPenalty{-23, 16};
inline constexpr Score kKingOpenFilePenalty{-34, -15};
inline constexpr std::array<Score, 4> kHangingBonus{{
    {42, 29}, {51, 32}, {44, 27}, {39, -4}
}};
inline constexpr Score kPawnAttackDefendedBonus{73, 24};
inline constexpr Score kBishopPairBonus{49, 41};
inline constexpr Score kKnightOutpostSupported{42, 17};
inline constexpr Score kKnightOutpostUnsupported{29, 1};
inline constexpr Score kBishopOutpostSupported{26, 4};
inline constexpr Score kBishopOutpostUnsupported{20, 3};
inline constexpr Score kMinorBehindPawn{10, 2};
inline constexpr Score kBishopSameColorPawn{-2, -10};
inline constexpr Score kBishopLongDiagonal{31, 13};
inline constexpr Score kRookOpenFileBonus{64, 1};
inline constexpr Score kRookSemiOpenFileBonus{29, 10};
inline constexpr Score kRookOnQueenFileBonus{20, 6};
inline constexpr Score kRookSeventhBonus{11, 34};
inline constexpr Score kRookConnectedBonus{1, -3};
inline constexpr Score kTrappedRookPenalty{-13, -13};
inline constexpr Score kKnightPerOwnPawn{17, 3};
inline constexpr Score kRookPerOwnPawn{32, -3};
inline constexpr Score kCenterControlBonus{5, 3};
inline constexpr Score kDevelopmentBonus{8, 6};
inline constexpr Score kTempoBonus{55, 34};

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
    std::uint8_t king_zone_attacks[2]{};
    std::array<std::uint64_t, 2> attacked_squares{};
};

[[nodiscard]] EvalBoard make_eval_board(const GameState& state) noexcept {
    EvalBoard eval;
    const ExchangeSnapshot snap = state.exchange_snapshot();
    eval.pieces = *snap.piece_bitboards;
    eval.occupied = snap.occupied;
    eval.side_to_move = snap.side_to_move;

    for (std::size_t color = 0; color < 2; ++color) {
        std::uint64_t occ_color = 0;
        for (std::size_t pt = 1; pt <= 6; ++pt) {
            occ_color |= eval.pieces[color][pt];
        }
        eval.colors[color] = occ_color;
        const std::uint64_t king_bb = eval.pieces[color][type_index(PieceType::king)];
        eval.kings[color] = king_bb != 0 ? std::countr_zero(king_bb) : -1;
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
        const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];
        const std::uint64_t low_ranks = white ? 0x0000000000FFFF00ULL : 0x00FFFF0000000000ULL;
        const std::uint64_t blocked_pawns =
            white ? (own_pawns & (eval.occupied >> 8)) : (own_pawns & (eval.occupied << 8));
        const std::uint64_t excluded_pawns = own_pawns & (low_ranks | blocked_pawns);
        const std::uint64_t mob_area = ~(
            eval.pieces[color][type_index(PieceType::king)] |
            eval.pieces[color][type_index(PieceType::queen)] |
            excluded_pawns |
            eval.pawn_attacks[1 - color]);

        std::uint64_t attacks = eval.pawn_attacks[color];

        for (std::uint64_t bb = eval.pieces[color][type_index(PieceType::knight)]; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            const std::uint64_t pa = detail::knight_attacks(sq);
            attacks |= pa;
            const int mob = std::min(8, std::popcount(pa & mob_area));
            eval.mobility_scores[color][0] += kKnightMobility[static_cast<std::size_t>(mob)];
        }

        const std::uint64_t bishop_xray_occ =
            eval.occupied ^ eval.pieces[color][type_index(PieceType::queen)];
        for (std::uint64_t bb = eval.pieces[color][type_index(PieceType::bishop)]; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            attacks |= detail::bishop_attacks(sq, eval.occupied);
            const std::uint64_t mob_bb = detail::bishop_attacks(sq, bishop_xray_occ);
            const int mob = std::min(13, std::popcount(mob_bb & mob_area));
            eval.mobility_scores[color][1] += kBishopMobility[static_cast<std::size_t>(mob)];
        }

        const std::uint64_t rook_xray_occ =
            bishop_xray_occ ^ eval.pieces[color][type_index(PieceType::rook)];
        for (std::uint64_t bb = eval.pieces[color][type_index(PieceType::rook)]; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            attacks |= detail::rook_attacks(sq, eval.occupied);
            const std::uint64_t mob_bb = detail::rook_attacks(sq, rook_xray_occ);
            const int mob = std::min(14, std::popcount(mob_bb & mob_area));
            eval.mobility_scores[color][2] += kRookMobility[static_cast<std::size_t>(mob)];
        }

        for (std::uint64_t bb = eval.pieces[color][type_index(PieceType::queen)]; bb != 0;) {
            const int sq = std::countr_zero(bb);
            bb &= bb - 1;
            const std::uint64_t pa = detail::queen_attacks(sq, eval.occupied);
            attacks |= pa;
            const int mob = std::min(27, std::popcount(pa & mob_area));
            eval.mobility_scores[color][3] += kQueenMobility[static_cast<std::size_t>(mob)];
        }

        if (eval.kings[color] >= 0) {
            attacks |= detail::king_attacks(eval.kings[color]);
        }
        eval.attacked_squares[color] = attacks;
        eval.center_control[color] =
            static_cast<std::uint8_t>(std::popcount(attacks & kCenterMask));
    }

    for (std::size_t color = 0; color < 2; ++color) {
        if (eval.kings[color] < 0) {
            eval.king_zone_attacks[color] = 0;
            continue;
        }
        const std::uint64_t zone =
            detail::king_attacks(eval.kings[color]) | bit_of(eval.kings[color]);
        eval.king_zone_attacks[color] = static_cast<std::uint8_t>(
            std::popcount(eval.attacked_squares[1 - color] & zone));
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
    const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];

    const int attacker_count = eval.king_zone_attacks[color];
    score += scaled(kKingAttackerWeight, attacker_count, attacker_count);
    const int attacker_sq = attacker_count * attacker_count;
    score += scaled(kKingAttackerSqWeight, attacker_sq, attacker_sq);
    if (attacker_count >= 3) {
        score += kKingAttackerGe3Weight;
    }

    for (int file = std::max(0, king_file - 1); file <= std::min(7, king_file + 1); ++file) {
        bool shield_pawn = false;
        for (int delta = 1; delta <= 2; ++delta) {
            const int rank = king_rank + (white ? delta : -delta);
            if (rank < 0 || rank > 7) {
                continue;
            }
            if (((own_pawns >> (rank * 8 + file)) & 1U) != 0) {
                shield_pawn = true;
            }
        }
        if (!shield_pawn) {
            score += kKingMissingShieldPenalty;
        }
        const std::uint64_t file_mask = 0x0101010101010101ULL << static_cast<unsigned>(file);
        if ((own_pawns & file_mask) == 0) {
            score += kKingOpenFilePenalty;
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
