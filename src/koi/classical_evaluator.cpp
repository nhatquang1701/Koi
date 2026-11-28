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

// Texel-tuned tapered material and piece-square tables (rank 1 first,
// White perspective; Black mirrored via square ^ 56).
inline constexpr std::array<Score, 7> kMaterialWeights{{
    {0, 0}, {93, 189}, {476, 396}, {476, 421}, {644, 811}, {1348, 1299}, {0, 0}
}};

inline constexpr std::array<Score, 64> kPawnPst{{
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {-58, -14}, {-38, -18}, {-33, -22}, {-48, -13}, {-34, 0}, {-12, -19}, {-2, -25}, {-45, -31},
    {-36, -30}, {-36, -21}, {-5, -38}, {-20, -31}, {4, -34}, {-26, -26}, {4, -32}, {-27, -37},
    {-37, -21}, {-21, -16}, {1, -34}, {30, -45}, {30, -41}, {8, -37}, {-3, -22}, {-27, -36},
    {-11, -5}, {-1, -3}, {21, -22}, {37, -41}, {65, -42}, {38, -23}, {20, -7}, {7, -18},
    {15, 65}, {14, 60}, {41, 46}, {73, 5}, {43, 19}, {70, 32}, {29, 56}, {7, 50},
    {57, 168}, {71, 171}, {52, 145}, {81, 141}, {39, 114}, {32, 154}, {-41, 172}, {-25, 177},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}
}};

inline constexpr std::array<Score, 64> kKnightPst{{
    {-48, -4}, {-16, -24}, {-29, 19}, {-41, 21}, {-22, 0}, {7, 4}, {-12, -8}, {-40, -16},
    {-64, 0}, {-29, -5}, {-18, 1}, {1, 1}, {4, 8}, {8, -3}, {-19, -2}, {-14, -8},
    {-64, -12}, {-20, 1}, {-1, -8}, {21, 8}, {33, 5}, {9, -8}, {19, -3}, {-50, -1},
    {-29, 4}, {-29, 18}, {24, 14}, {18, 25}, {41, 26}, {32, 18}, {8, 16}, {-9, -3},
    {10, 5}, {11, 16}, {30, 26}, {82, 22}, {20, 40}, {86, 19}, {9, 26}, {44, 4},
    {-13, -9}, {47, -9}, {46, 10}, {60, 16}, {106, -3}, {37, 9}, {50, -9}, {14, -2},
    {-35, -24}, {-22, 2}, {29, -10}, {33, 17}, {-9, 2}, {68, -20}, {-9, -20}, {13, -24},
    {-143, -77}, {-18, -3}, {-18, 6}, {-38, -6}, {-21, -13}, {-40, -4}, {-11, -12}, {-91, -60}
}};

inline constexpr std::array<Score, 64> kBishopPst{{
    {-24, -1}, {43, 13}, {19, 2}, {-7, 3}, {-22, 10}, {2, 15}, {-1, 18}, {-30, -12},
    {11, 4}, {28, -12}, {21, -6}, {-3, -2}, {4, -9}, {26, -28}, {46, -16}, {15, -28},
    {-8, -4}, {10, 10}, {15, 3}, {-3, 8}, {7, 6}, {15, -5}, {19, 2}, {-4, 7},
    {-28, 7}, {-11, 6}, {-25, 24}, {38, -3}, {12, -3}, {-7, 3}, {-1, -2}, {11, -8},
    {3, 3}, {-5, 7}, {14, 3}, {20, 6}, {26, 4}, {16, 0}, {-1, 6}, {-23, -2},
    {-4, -3}, {34, 1}, {15, -10}, {34, -3}, {15, 4}, {32, 13}, {27, 3}, {25, 8},
    {-20, -1}, {11, -16}, {-16, 3}, {-24, 12}, {-38, 2}, {-47, 4}, {-3, -18}, {-27, -13},
    {-36, -9}, {-11, 10}, {-18, 1}, {-36, -9}, {-29, -4}, {-48, -5}, {-22, 6}, {-33, -4}
}};

inline constexpr std::array<Score, 64> kRookPst{{
    {-20, -8}, {-11, -11}, {-2, -9}, {4, -14}, {25, -27}, {12, -13}, {23, -21}, {5, -29},
    {-36, -19}, {-23, -7}, {-23, -8}, {-22, -1}, {-8, -13}, {-7, -14}, {-2, -35}, {-36, -26},
    {-29, -6}, {-24, -11}, {-23, 1}, {-19, -3}, {-17, -14}, {-5, -10}, {35, -29}, {14, -25},
    {-30, 4}, {-30, 3}, {-23, 7}, {-21, 3}, {-10, -6}, {-24, -1}, {-2, -3}, {-18, -25},
    {-15, 9}, {-15, 19}, {-9, 19}, {-15, 4}, {-13, 3}, {7, -2}, {13, -5}, {20, -6},
    {-11, 15}, {19, 11}, {3, 18}, {7, 12}, {33, -6}, {40, 9}, {40, 9}, {10, 1},
    {-11, 12}, {-24, 35}, {7, 30}, {44, 25}, {36, 15}, {47, 10}, {26, 8}, {50, -3},
    {11, 2}, {-4, 22}, {14, 14}, {-12, 28}, {18, 10}, {23, 19}, {1, 20}, {8, 14}
}};

inline constexpr std::array<Score, 64> kQueenPst{{
    {-38, -22}, {-16, -39}, {3, -14}, {15, -31}, {7, -30}, {-29, -28}, {-42, -26}, {-22, -27},
    {-12, -10}, {-9, -21}, {24, -43}, {26, -34}, {25, -22}, {14, -44}, {25, -53}, {30, -31},
    {-12, -15}, {6, -9}, {-9, 3}, {1, -15}, {7, -14}, {1, 4}, {15, 4}, {0, 9},
    {0, 15}, {-23, -20}, {-17, -10}, {0, -7}, {-11, 18}, {-18, 17}, {7, -6}, {-1, 26},
    {-1, -2}, {-22, 11}, {-7, -14}, {-15, 2}, {-24, 23}, {-15, 32}, {-15, 56}, {15, 38},
    {-10, 7}, {11, 5}, {-24, 9}, {-4, 10}, {-6, 31}, {47, 41}, {69, 45}, {29, 37},
    {6, 3}, {-38, 8}, {-30, 31}, {-1, 26}, {-18, 35}, {36, 49}, {15, 23}, {43, 40},
    {-21, -28}, {-13, -25}, {-10, -8}, {11, -5}, {-1, 14}, {14, 13}, {22, -10}, {11, -19}
}};

inline constexpr std::array<Score, 64> kKingPst{{
    {1, -27}, {88, -61}, {60, -39}, {-58, -26}, {32, -54}, {-49, -21}, {75, -58}, {50, -82},
    {28, -32}, {41, -27}, {-4, -10}, {-30, 2}, {-26, -1}, {-9, -8}, {65, -32}, {26, -34},
    {-12, -36}, {2, -15}, {-19, -12}, {-50, -1}, {-18, -3}, {-46, -2}, {6, -23}, {-51, -26},
    {-16, -27}, {-3, -10}, {-26, 2}, {-26, 6}, {-45, 14}, {-41, 1}, {-28, -14}, {-51, -25},
    {4, -20}, {5, 22}, {-1, 27}, {-19, 41}, {-20, 33}, {-33, 34}, {-6, 22}, {-27, -3},
    {11, 8}, {15, 39}, {13, 52}, {4, 37}, {1, 48}, {2, 61}, {16, 59}, {-8, 14},
    {6, 12}, {24, 48}, {19, 36}, {9, 32}, {18, 36}, {15, 40}, {10, 49}, {1, 24},
    {4, -23}, {12, -24}, {14, -19}, {11, 4}, {14, 2}, {9, 4}, {5, -3}, {6, -15}
}};

inline constexpr std::array<Score, 4> kMobilityWeights{{{3, 10}, {8, 10}, {5, 4}, {1, 16}}};
inline constexpr Score kDoubledPawnPenalty{6, -38};
inline constexpr Score kIsolatedPawnPenalty{-23, -5};
inline constexpr std::array<Score, 8> kPassedPawnRankBonus{{
    {0, 0}, {2, -59}, {-23, -27}, {-16, 19}, {3, 67}, {19, 89}, {12, 74}, {0, 0}
}};
inline constexpr Score kPassedOwnKingDist{7, -11};
inline constexpr Score kPassedEnemyKingDist{-2, 19};
inline constexpr Score kPassedBlockedBonus{-3, -37};
inline constexpr Score kConnectedPawnBonus{11, 0};
inline constexpr Score kCandidatePasserBonus{22, 9};
inline constexpr Score kKingAttackerWeight{4, 17};
inline constexpr Score kKingAttackerSqWeight{-11, 1};
inline constexpr Score kKingAttackerGe3Weight{-18, -7};
inline constexpr Score kKingMissingShieldPenalty{-22, 18};
inline constexpr Score kKingOpenFilePenalty{-33, -15};
inline constexpr std::array<Score, 4> kHangingBonus{{
    {41, 28}, {52, 33}, {52, 27}, {44, -3}
}};
inline constexpr Score kPawnAttackDefendedBonus{67, 24};
inline constexpr Score kBishopPairBonus{44, 39};
inline constexpr Score kKnightOutpostBonus{7, 8};
inline constexpr Score kRookOpenFileBonus{57, -3};
inline constexpr Score kRookSemiOpenFileBonus{21, 19};
inline constexpr Score kRookSeventhBonus{9, 11};
inline constexpr Score kCenterControlBonus{3, 4};
inline constexpr Score kDevelopmentBonus{35, 15};
inline constexpr Score kTempoBonus{54, 32};

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

struct EvalBoard {
    std::array<std::uint64_t, 2> colors{};
    std::array<std::array<std::uint64_t, 7>, 2> pieces{};
    std::uint64_t occupied = 0;
    std::array<int, 2> kings{-1, -1};
    std::array<std::uint64_t, 2> pawn_attacks{};
    std::array<std::array<std::uint16_t, 4>, 2> mobility{};
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

    // Home-square development count: knights off b1/g1 or bishops off c1/f1
    // for White, mirrored for Black.
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

    constexpr std::uint64_t kCenterMask =
        (std::uint64_t{1} << 27) | (std::uint64_t{1} << 28) |
        (std::uint64_t{1} << 35) | (std::uint64_t{1} << 36);  // d4/e4/d5/e5

    for (std::size_t color = 0; color < 2; ++color) {
        const std::uint64_t own = eval.colors[color];
        const std::uint64_t pawns = eval.pieces[color][type_index(PieceType::pawn)];
        constexpr std::uint64_t kNotFileA = 0xFEFEFEFEFEFEFEFEULL;
        constexpr std::uint64_t kNotFileH = 0x7F7F7F7F7F7F7F7FULL;
        eval.pawn_attacks[color] = color == kWhite
            ? (((pawns & kNotFileA) << 7) | ((pawns & kNotFileH) << 9))
            : (((pawns & kNotFileA) >> 9) | ((pawns & kNotFileH) >> 7));

        std::uint64_t attacks = eval.pawn_attacks[color];

        auto accumulate_piece_attacks = [&](const PieceType pt, auto&& attack_fn) noexcept {
            const std::size_t mob_index = type_index(pt) - type_index(PieceType::knight);
            int mob_sum = 0;
            for (std::uint64_t bb = eval.pieces[color][type_index(pt)]; bb != 0;) {
                const int square = std::countr_zero(bb);
                bb &= bb - 1;
                const std::uint64_t piece_attacks = attack_fn(square);
                attacks |= piece_attacks;
                mob_sum += std::popcount(piece_attacks & ~own);
            }
            eval.mobility[color][mob_index] = static_cast<std::uint16_t>(mob_sum);
        };

        accumulate_piece_attacks(
            PieceType::knight, [](int sq) noexcept { return detail::knight_attacks(sq); });
        accumulate_piece_attacks(
            PieceType::bishop,
            [&eval](int sq) noexcept { return detail::bishop_attacks(sq, eval.occupied); });
        accumulate_piece_attacks(
            PieceType::rook,
            [&eval](int sq) noexcept { return detail::rook_attacks(sq, eval.occupied); });
        accumulate_piece_attacks(
            PieceType::queen,
            [&eval](int sq) noexcept { return detail::queen_attacks(sq, eval.occupied); });

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
    Score score{};
    const bool white = color == kWhite;
    const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];
    const std::uint64_t enemy_pawns = eval.pieces[1 - color][type_index(PieceType::pawn)];
    const std::uint64_t enemy_pawn_attacks = eval.pawn_attacks[1 - color];

    if (std::popcount(eval.pieces[color][type_index(PieceType::bishop)]) >= 2) {
        score += kBishopPairBonus;
    }

    for (std::uint64_t knights = eval.pieces[color][type_index(PieceType::knight)];
         knights != 0;) {
        const int square = std::countr_zero(knights);
        knights &= knights - 1;
        const std::uint64_t square_bit = bit_of(square);
        if ((eval.pawn_attacks[color] & square_bit) != 0 &&
            (enemy_pawn_attacks & square_bit) == 0) {
            score += kKnightOutpostBonus;
        }
    }

    for (std::uint64_t rooks = eval.pieces[color][type_index(PieceType::rook)];
         rooks != 0;) {
        const int square = std::countr_zero(rooks);
        rooks &= rooks - 1;
        const int file = file_of(square);
        const std::uint64_t file_mask = 0x0101010101010101ULL << static_cast<unsigned>(file);
        const bool own_pawn_on_file = (own_pawns & file_mask) != 0;
        const bool any_pawn_on_file = ((own_pawns | enemy_pawns) & file_mask) != 0;
        if (!any_pawn_on_file) {
            score += kRookOpenFileBonus;
        } else if (!own_pawn_on_file) {
            score += kRookSemiOpenFileBonus;
        }
        const int own_rank = white ? rank_of(square) : 7 - rank_of(square);
        if (own_rank == 6) {
            score += kRookSeventhBonus;
        }
    }
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
    // King + bishop (or bare king) vs king + bishop with no pawns is a draw
    // when the bishops are on opposite colours (or one side is bare).
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
            const int mob_count = static_cast<int>(eval.mobility[color][mob_idx]);
            if (mob_count == 0) {
                continue;
            }
            const Score mob_term = percent_of(
                scaled(kMobilityWeights[mob_idx], mob_count, mob_count),
                tuning.mobility * mob_piece_scales[mob_idx] / 100);
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
