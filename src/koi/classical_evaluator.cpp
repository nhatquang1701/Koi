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
    {0, 0}, {-1, 220}, {409, 460}, {551, 505}, {463, 892}, {1534, 1476}, {0, 0}
}};

inline constexpr std::array<Score, 64> kPawnPst{{
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {-50, -30}, {-41, -31}, {-37, -30}, {-42, -34}, {-34, -12}, {-16, -29}, {-1, -39}, {-35, -49},
    {-35, -42}, {-42, -38}, {-10, -50}, {-28, -46}, {-3, -44}, {-30, -42}, {3, -47}, {-29, -52},
    {-38, -37}, {-38, -28}, {-14, -47}, {14, -54}, {14, -55}, {-1, -51}, {-14, -36}, {-24, -51},
    {-11, -20}, {-13, -14}, {13, -34}, {16, -49}, {41, -52}, {23, -35}, {19, -27}, {8, -33},
    {9, 48}, {10, 47}, {36, 36}, {66, 4}, {35, 13}, {49, 27}, {29, 48}, {16, 35},
    {60, 184}, {42, 178}, {54, 156}, {74, 138}, {68, 120}, {9, 146}, {-17, 161}, {-27, 169},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}
}};

inline constexpr std::array<Score, 64> kKnightPst{{
    {-26, -23}, {-29, -38}, {-37, 7}, {-9, 2}, {-3, -8}, {-12, -9}, {-32, -20}, {-36, -16},
    {-58, -12}, {-19, 1}, {-19, 4}, {-7, -2}, {-6, 5}, {6, 0}, {-1, -13}, {-17, -9},
    {-46, -5}, {-14, 2}, {-2, 5}, {6, 26}, {23, 26}, {2, 5}, {23, -3}, {-33, -1},
    {-16, -4}, {4, 1}, {19, 25}, {18, 33}, {40, 36}, {32, 16}, {28, 4}, {10, -6},
    {-2, -4}, {11, 10}, {18, 31}, {71, 27}, {16, 46}, {79, 29}, {-1, 24}, {56, -6},
    {-12, -14}, {20, -5}, {13, 24}, {33, 24}, {73, 14}, {29, 10}, {25, 5}, {-5, -17},
    {-18, -29}, {-27, -2}, {25, -10}, {25, 10}, {18, 11}, {54, -6}, {-11, -3}, {22, -26},
    {-156, -79}, {-5, -14}, {-11, 8}, {-6, 6}, {3, -5}, {-19, -12}, {-2, -10}, {-104, -67}
}};

inline constexpr std::array<Score, 64> kBishopPst{{
    {-22, -12}, {27, -7}, {-4, -9}, {-11, 6}, {-18, 1}, {-28, 7}, {-7, -9}, {-25, -31},
    {14, -3}, {5, -28}, {32, -5}, {-4, 5}, {-1, 3}, {23, -6}, {10, -31}, {18, -28},
    {-4, -8}, {30, 15}, {-4, 8}, {1, 31}, {16, 29}, {-7, -5}, {39, -6}, {6, -9},
    {-6, -16}, {-13, 14}, {-10, 40}, {35, 22}, {26, 21}, {7, 13}, {-11, 10}, {45, -32},
    {-19, -3}, {4, 16}, {20, 14}, {21, 31}, {32, 28}, {15, 13}, {15, 11}, {-19, -4},
    {-20, -13}, {21, 0}, {-15, -8}, {30, 3}, {6, 16}, {16, 1}, {33, -2}, {27, -2},
    {-29, -19}, {-11, -16}, {2, 6}, {-23, 8}, {4, -1}, {-18, -10}, {1, -22}, {-33, -12},
    {-44, -11}, {-12, -2}, {-16, 2}, {-26, -11}, {-24, 5}, {-38, 1}, {-17, 13}, {-41, -15}
}};

inline constexpr std::array<Score, 64> kRookPst{{
    {-28, -4}, {-19, -18}, {-22, -4}, {-26, -4}, {1, -22}, {-9, -9}, {15, -25}, {-6, -20},
    {-30, -11}, {-23, -10}, {-15, -5}, {-26, -8}, {-21, -8}, {3, -20}, {-3, -20}, {-25, -26},
    {-26, -8}, {-18, -9}, {-16, -6}, {-35, -8}, {-23, -9}, {-12, -11}, {24, -22}, {9, -24},
    {-29, 4}, {-31, 3}, {-9, 12}, {-20, 1}, {-13, -5}, {-17, -1}, {7, -13}, {-10, -18},
    {-5, 13}, {-17, 12}, {-4, 22}, {-9, 8}, {-3, 5}, {26, -8}, {27, -5}, {17, -13},
    {-4, 17}, {4, 20}, {23, 19}, {25, 15}, {19, 3}, {42, 10}, {52, 1}, {40, 0},
    {-3, -2}, {-29, 18}, {14, 19}, {21, 10}, {33, 1}, {44, 2}, {22, 6}, {15, -1},
    {15, 6}, {0, 21}, {11, 31}, {-2, 26}, {25, 16}, {25, 18}, {30, 16}, {5, 21}
}};

inline constexpr std::array<Score, 64> kQueenPst{{
    {-36, -46}, {-24, -52}, {-10, -42}, {9, -88}, {10, -63}, {-35, -56}, {-27, -32}, {-21, -21},
    {-22, -38}, {-12, -41}, {17, -66}, {22, -55}, {22, -53}, {15, -58}, {15, -81}, {-1, -44},
    {1, -14}, {2, -17}, {-13, 13}, {-8, 2}, {-2, 11}, {1, 23}, {16, -11}, {12, -11},
    {15, -24}, {-24, 16}, {-19, 28}, {-11, 50}, {-4, 41}, {-1, 21}, {7, 15}, {12, 0},
    {0, -14}, {-22, 15}, {0, 19}, {-14, 54}, {-19, 75}, {7, 60}, {-2, 46}, {19, 10},
    {-5, -5}, {15, 11}, {-11, 40}, {16, 53}, {36, 64}, {52, 59}, {53, 39}, {27, 31},
    {-2, -24}, {-50, 15}, {-37, 44}, {-10, 42}, {8, 40}, {23, 44}, {-8, 33}, {43, 16},
    {-37, -35}, {-16, -25}, {3, -7}, {6, 2}, {2, 4}, {24, 14}, {-4, -20}, {-2, -10}
}};

inline constexpr std::array<Score, 64> kKingPst{{
    {-12, -22}, {94, -56}, {66, -39}, {-27, -24}, {34, -47}, {-21, -29}, {75, -58}, {42, -73},
    {15, -24}, {53, -25}, {5, -11}, {-33, 1}, {-20, -3}, {-2, -11}, {69, -32}, {35, -35},
    {-12, -33}, {12, -25}, {-25, -9}, {-40, -1}, {-35, -1}, {-23, -11}, {7, -23}, {-70, -26},
    {-19, -21}, {-11, -11}, {-20, -3}, {-36, 9}, {-44, 10}, {-31, -3}, {-40, -7}, {-54, -27},
    {-1, -14}, {10, 12}, {-15, 24}, {-34, 37}, {-24, 33}, {-11, 27}, {-8, 17}, {-14, -10},
    {19, 3}, {15, 42}, {6, 57}, {0, 55}, {-2, 45}, {13, 61}, {12, 51}, {2, 9},
    {8, 4}, {15, 46}, {11, 50}, {7, 30}, {6, 44}, {13, 42}, {9, 47}, {-2, 7},
    {3, -40}, {4, -10}, {8, 0}, {5, -2}, {2, 3}, {3, 14}, {7, 4}, {1, -17}
}};

inline constexpr std::array<Score, 9> kKnightMobility{{
    {-29, -6}, {-31, -20}, {-19, -11}, {-12, -3}, {0, 1}, {12, 11}, {25, 13},
    {36, 12}, {19, 4}
}};

inline constexpr std::array<Score, 14> kBishopMobility{{
    {-56, -20}, {-45, -17}, {-16, -17}, {-6, -11}, {5, 1}, {14, 3}, {19, 11},
    {23, 12}, {25, 11}, {18, 11}, {11, 7}, {6, 6}, {1, 3}, {1, 1}
}};

inline constexpr std::array<Score, 15> kRookMobility{{
    {-10, -5}, {-37, -21}, {-17, -27}, {-11, -16}, {-15, -12}, {-9, -5}, {-5, 0},
    {1, 2}, {10, 6}, {14, 12}, {15, 15}, {21, 17}, {18, 17}, {14, 17},
    {9, 2}
}};

inline constexpr std::array<Score, 28> kQueenMobility{{
    {0, 0}, {-4, 0}, {-11, -1}, {-14, -2}, {-16, -4}, {-14, -5}, {-13, -7},
    {-9, -8}, {-4, -8}, {-3, -8}, {1, -5}, {3, -3}, {6, 0}, {7, 6},
    {8, 5}, {10, 8}, {8, 7}, {8, 8}, {10, 8}, {10, 7}, {9, 5},
    {3, 0}, {3, 2}, {1, -2}, {1, 0}, {0, -1}, {0, -1}, {0, -1}
}};

inline constexpr Score kDoubledPawnPenalty{5, -37};
inline constexpr Score kIsolatedPawnPenalty{-24, -6};
inline constexpr std::array<Score, 8> kPassedPawnRankBonus{{
    {0, 0}, {-3, -60}, {-7, -35}, {-12, 14}, {0, 64}, {16, 89}, {13, 64}, {0, 0}
}};
inline constexpr Score kPassedOwnKingDist{9, -12};
inline constexpr Score kPassedEnemyKingDist{-2, 21};
inline constexpr Score kPassedBlockedBonus{-4, -42};
inline constexpr Score kConnectedPawnBonus{9, 0};
inline constexpr Score kCandidatePasserBonus{17, 12};
inline constexpr Score kKingAttackerWeight{3, 16};
inline constexpr Score kKingAttackerSqWeight{-11, 1};
inline constexpr Score kKingAttackerGe3Weight{-15, -6};
inline constexpr Score kKingMissingShieldPenalty{-25, 18};
inline constexpr Score kKingOpenFilePenalty{-34, -14};
inline constexpr std::array<Score, 4> kHangingBonus{{
    {43, 22}, {49, 30}, {43, 27}, {35, -4}
}};
inline constexpr Score kPawnAttackDefendedBonus{74, 20};
inline constexpr Score kBishopPairBonus{52, 41};
inline constexpr Score kKnightOutpostSupported{43, 16};
inline constexpr Score kKnightOutpostUnsupported{28, -1};
inline constexpr Score kBishopOutpostSupported{24, 1};
inline constexpr Score kBishopOutpostUnsupported{18, 1};
inline constexpr Score kMinorBehindPawn{11, 2};
inline constexpr Score kBishopSameColorPawn{0, -13};
inline constexpr Score kBishopLongDiagonal{34, 16};
inline constexpr Score kRookOpenFileBonus{66, -1};
inline constexpr Score kRookSemiOpenFileBonus{24, 18};
inline constexpr Score kRookOnQueenFileBonus{18, 5};
inline constexpr Score kRookSeventhBonus{12, 34};
inline constexpr Score kRookConnectedBonus{1, -4};
inline constexpr Score kTrappedRookPenalty{-11, -12};
inline constexpr Score kKnightPerOwnPawn{18, 0};
inline constexpr Score kRookPerOwnPawn{42, -9};
inline constexpr Score kCenterControlBonus{6, 4};
inline constexpr Score kDevelopmentBonus{9, 7};
inline constexpr Score kTempoBonus{56, 32};

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

inline constexpr PawnMaskTable kPassedPawnMask = make_passed_pawn_masks();
inline constexpr PawnMaskTable kForwardFileMask = make_forward_file_masks();
inline constexpr PawnMaskTable kConnectedPawnMask = make_connected_pawn_masks();

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
    const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];
    const std::uint64_t enemy_pawns =
        eval.pieces[1 - color][type_index(PieceType::pawn)];
    const bool white = color == kWhite;

    std::array<int, 8> own_file_count{};
    for (std::uint64_t pawns = own_pawns; pawns != 0;) {
        const int square = std::countr_zero(pawns);
        pawns &= pawns - 1;
        ++own_file_count[static_cast<std::size_t>(file_of(square))];
    }

    for (int file = 0; file < 8; ++file) {
        const bool isolated =
            (file == 0 || own_file_count[static_cast<std::size_t>(file - 1)] == 0) &&
            (file == 7 || own_file_count[static_cast<std::size_t>(file + 1)] == 0);
        if (own_file_count[static_cast<std::size_t>(file)] > 1) {
            const int extra = own_file_count[static_cast<std::size_t>(file)] - 1;
            score += scaled(kDoubledPawnPenalty, extra, extra);
        }
        if (isolated && own_file_count[static_cast<std::size_t>(file)] != 0) {
            score += kIsolatedPawnPenalty;
        }
    }

    for (std::uint64_t pawns = own_pawns; pawns != 0;) {
        const int square = std::countr_zero(pawns);
        pawns &= pawns - 1;
        const int file = file_of(square);
        const int rank = rank_of(square);
        const int own_rank = white ? rank : 7 - rank;

        const bool passed =
            (enemy_pawns & kPassedPawnMask[color][static_cast<std::size_t>(square)]) == 0;
        if (passed) {
            Score bonus = percent_of(
                kPassedPawnRankBonus[static_cast<std::size_t>(own_rank)],
                tuning.passed_rank);
            if (eval.kings[color] >= 0 && eval.kings[1 - color] >= 0) {
                const int own_king_distance = square_distance(eval.kings[color], square);
                const int enemy_king_distance =
                    square_distance(eval.kings[1 - color], square);
                bonus += scaled(kPassedOwnKingDist, own_king_distance, own_king_distance);
                bonus += scaled(kPassedEnemyKingDist, enemy_king_distance, enemy_king_distance);
            }
            const int ahead_rank = white ? rank + 1 : rank - 1;
            if (ahead_rank >= 0 && ahead_rank < 8 &&
                ((eval.occupied >> (ahead_rank * 8 + file)) & 1U) != 0) {
                bonus += kPassedBlockedBonus;
            }
            score += percent_of(bonus, tuning.passed);
            continue;
        }

        const bool connected =
            (own_pawns & kConnectedPawnMask[color][static_cast<std::size_t>(square)]) != 0;
        if (connected) {
            score += kConnectedPawnBonus;
            if ((enemy_pawns & kForwardFileMask[color][static_cast<std::size_t>(square)]) == 0) {
                score += kCandidatePasserBonus;
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
