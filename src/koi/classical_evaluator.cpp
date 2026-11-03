#include "koi/classical_evaluator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "koi/detail/attack_tables.hpp"
#include "koi/detail/hce_correction.hpp"
#include "koi/detail/search_constants.hpp"

namespace koi {
namespace {

[[nodiscard]] constexpr int abs_int(int value) noexcept { return value < 0 ? -value : value; }

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

[[nodiscard]] constexpr Score scaled(Score score, int mg, int eg) noexcept {
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
};

[[nodiscard]] int tuning_percent(const char* name, int fallback) noexcept {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 400) {
        return fallback;
    }
    return static_cast<int>(parsed);
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
        tuning_percent("KOI_HCE_PIECE_TERM_SCALE", 100)};
    return tuning;
}

[[nodiscard]] constexpr Score percent_of(Score score, int percent) noexcept {
    return Score{score.mg * percent / 100, score.eg * percent / 100};
}

constexpr int kPhaseMaximum = 24;
constexpr std::size_t kWhite = 0;
constexpr std::size_t kBlack = 1;

[[nodiscard]] constexpr std::size_t index_of(Color color) noexcept {
    return color == Color::white ? kWhite : kBlack;
}

[[nodiscard]] constexpr Color color_of(std::size_t index) noexcept {
    return index == kWhite ? Color::white : Color::black;
}

[[nodiscard]] constexpr int sign_of(std::size_t index) noexcept {
    return index == kWhite ? 1 : -1;
}

[[nodiscard]] constexpr int file_of(int square) noexcept { return square & 7; }
[[nodiscard]] constexpr int rank_of(int square) noexcept { return square >> 3; }
[[nodiscard]] constexpr std::uint64_t bit_of(int square) noexcept {
    return std::uint64_t{1} << static_cast<unsigned>(square);
}
[[nodiscard]] constexpr int distance_to_center(int square) noexcept {
    const int file = file_of(square);
    const int rank = rank_of(square);
    return abs_int(file - 3) + abs_int(file - 4) + abs_int(rank - 3) + abs_int(rank - 4);
}
[[nodiscard]] constexpr int file_center(int file) noexcept {
    constexpr std::array<int, 8> kFileCenter{0, 1, 2, 3, 3, 2, 1, 0};
    return kFileCenter[static_cast<std::size_t>(file)];
}

[[nodiscard]] constexpr int square_distance(int first, int second) noexcept {
    return std::max(abs_int(file_of(first) - file_of(second)),
                    abs_int(rank_of(first) - rank_of(second)));
}

// Tapered king tables (rank 1 first, White perspective; Black is mirrored).
// The old formula mixed castling safety and endgame centralisation in one
// expression; explicit tables make the two regimes independently tunable.
inline constexpr std::array<int, 64> kKingMg{
    20, 30, 10, 0, 0, 10, 30, 20,
    -10, -20, -20, -20, -20, -20, -20, -10,
    -20, -30, -30, -40, -40, -30, -30, -20,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
};
inline constexpr std::array<int, 64> kKingEg{
    -50, -30, -30, -30, -30, -30, -30, -50,
    -30, -30, 0, 0, 0, 0, -30, -30,
    -30, -10, 20, 30, 30, 20, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30,
    -30, -10, 30, 40, 40, 30, -10, -30,
    -30, -10, 20, 30, 30, 20, -10, -30,
    -30, -20, -10, 0, 0, -10, -20, -30,
    -50, -40, -30, -20, -20, -30, -40, -50,
};

// Explicit piece tables (rank 1 first, White perspective; Black mirrored).
// Values are the classic simplified-evaluation layout, kept separate for mg/eg
// so future tuning can move each regime independently.
inline constexpr std::array<int, 64> kPawnMg{
    0, 0, 0, 0, 0, 0, 0, 0,
    5, 10, 10, -20, -20, 10, 10, 5,
    5, -5, -10, 0, 0, -10, -5, 5,
    0, 0, 0, 20, 20, 0, 0, 0,
    5, 5, 10, 25, 25, 10, 5, 5,
    10, 10, 20, 30, 30, 20, 10, 10,
    50, 50, 50, 50, 50, 50, 50, 50,
    0, 0, 0, 0, 0, 0, 0, 0,
};
inline constexpr std::array<int, 64> kPawnEg{
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    5, 5, 5, 10, 10, 5, 5, 5,
    10, 10, 15, 20, 20, 15, 10, 10,
    25, 25, 30, 40, 40, 30, 25, 25,
    60, 60, 60, 60, 60, 60, 60, 60,
    0, 0, 0, 0, 0, 0, 0, 0,
};
inline constexpr std::array<int, 64> kKnightPst{
    -50, -40, -30, -30, -30, -30, -40, -50,
    -40, -20, 0, 0, 0, 0, -20, -40,
    -30, 0, 10, 15, 15, 10, 0, -30,
    -30, 5, 15, 20, 20, 15, 5, -30,
    -30, 0, 15, 20, 20, 15, 0, -30,
    -30, 5, 10, 15, 15, 10, 5, -30,
    -40, -20, 0, 5, 5, 0, -20, -40,
    -50, -40, -30, -30, -30, -30, -40, -50,
};
inline constexpr std::array<int, 64> kBishopPst{
    -20, -10, -10, -10, -10, -10, -10, -20,
    -10, 0, 0, 0, 0, 0, 0, -10,
    -10, 0, 5, 10, 10, 5, 0, -10,
    -10, 5, 5, 10, 10, 5, 5, -10,
    -10, 0, 10, 10, 10, 10, 0, -10,
    -10, 10, 10, 10, 10, 10, 10, -10,
    -10, 5, 0, 0, 0, 0, 5, -10,
    -20, -10, -10, -10, -10, -10, -10, -20,
};
inline constexpr std::array<int, 64> kRookPst{
    0, 0, 0, 5, 5, 0, 0, 0,
    -5, 0, 0, 0, 0, 0, 0, -5,
    -5, 0, 0, 0, 0, 0, 0, -5,
    -5, 0, 0, 0, 0, 0, 0, -5,
    -5, 0, 0, 0, 0, 0, 0, -5,
    -5, 0, 0, 0, 0, 0, 0, -5,
    5, 10, 10, 10, 10, 10, 10, 5,
    0, 0, 0, 0, 0, 0, 0, 0,
};
inline constexpr std::array<int, 64> kQueenPst{
    -20, -10, -10, -5, -5, -10, -10, -20,
    -10, 0, 0, 0, 0, 0, 0, -10,
    -10, 0, 5, 5, 5, 5, 0, -10,
    -5, 0, 5, 5, 5, 5, 0, -5,
    0, 0, 5, 5, 5, 5, 0, -5,
    -10, 5, 5, 5, 5, 5, 0, -10,
    -10, 0, 5, 0, 0, 0, 0, -10,
    -20, -10, -10, -5, -5, -10, -10, -20,
};

// Piece-square tables are formulas for v1; they are replaced by tuned tables
// once the term set and strength harness are stable.
[[nodiscard]] constexpr Score piece_square(PieceType type, int square, Color color) noexcept {
    // Evaluate black from White's mirrored board, so one formula serves both
    // colours and all positional symmetry is exact.
    const int normalized = color == Color::white ? square : (square ^ 56);
    const int file = file_of(normalized);
    const int rank = rank_of(normalized);
    const int center = file_center(file);
    const int distance = distance_to_center(normalized);
    const std::size_t index = static_cast<std::size_t>(rank * 8 + file);
    Score base{};
    switch (type) {
    case PieceType::pawn: base = {kPawnMg[index], kPawnEg[index]}; break;
    case PieceType::knight: base = {kKnightPst[index], kKnightPst[index]}; break;
    case PieceType::bishop: base = {kBishopPst[index], kBishopPst[index]}; break;
    case PieceType::rook: base = {kRookPst[index], kRookPst[index]}; break;
    case PieceType::queen: base = {kQueenPst[index], kQueenPst[index]}; break;
    case PieceType::king: base = {kKingMg[index], kKingEg[index]}; break;
    case PieceType::none: return {};
    }
    const std::size_t type_index_correction = static_cast<std::size_t>(static_cast<int>(type) - 1);
    base.mg += detail::hce_correction::kPstMg[type_index_correction][index];
    base.eg += detail::hce_correction::kPstEg[type_index_correction][index];
    return base;
    return {};
}

[[nodiscard]] constexpr Score material(PieceType type) noexcept {
    const int material_index = static_cast<int>(type) - 1;
    Score base{};
    switch (type) {
    case PieceType::pawn: base = {100, 120}; break;
    case PieceType::knight: base = {320, 305}; break;
    case PieceType::bishop: base = {335, 325}; break;
    case PieceType::rook: base = {500, 540}; break;
    case PieceType::queen: base = {900, 930}; break;
    case PieceType::king: return {0, 0};
    case PieceType::none: return {};
    }
    if (material_index >= 0 && material_index < 5) {
        base.mg += detail::hce_correction::kMaterialMg[static_cast<std::size_t>(material_index)];
        base.eg += detail::hce_correction::kMaterialEg[static_cast<std::size_t>(material_index)];
    }
    return base;
    return {};
}

// Per-square mobility weights by piece type (indexed by PieceType value).
[[nodiscard]] constexpr Score mobility_weight(PieceType type) noexcept {
    switch (type) {
    case PieceType::knight: return {4, 4};
    case PieceType::bishop: return {3, 4};
    case PieceType::rook: return {2, 3};
    case PieceType::queen: return {1, 2};
    case PieceType::pawn:
    case PieceType::king:
    case PieceType::none: return {};
    }
    return {};
}

struct EvalBoard {
    std::array<Piece, 64> board{};
    std::array<std::uint64_t, 2> colors{};
    std::array<std::array<std::uint64_t, 7>, 2> pieces{};
    std::uint64_t occupied = 0;
    std::array<int, 2> kings{-1, -1};
    std::array<std::uint64_t, 2> pawn_attacks{};
    std::uint8_t phase = 0;
    Color side_to_move = Color::white;
    std::uint8_t development[2]{};
    std::uint8_t center_control[2]{};
    std::uint8_t king_zone_attacks[2]{};
    std::array<std::uint8_t, 64> piece_mobility{};
    std::array<std::uint64_t, 2> attacked_squares{};
};

[[nodiscard]] std::size_t type_index(PieceType type) noexcept {
    return static_cast<std::size_t>(type);
}

[[nodiscard]] EvalBoard make_eval_board(const GameState& state) {
    const PositionFeatures features = state.position_features();
    EvalBoard eval;
    eval.board = features.board;
    eval.colors = features.colors;
    eval.occupied = features.occupied;
    eval.phase = features.game_phase;
    eval.side_to_move = features.side_to_move;
    eval.development[0] = features.development[0];
    eval.development[1] = features.development[1];
    eval.center_control[0] = features.center_control[0];
    eval.center_control[1] = features.center_control[1];
    eval.king_zone_attacks[0] = features.king_zone_attacks[0];
    eval.king_zone_attacks[1] = features.king_zone_attacks[1];
    eval.piece_mobility = features.piece_mobility;
    eval.attacked_squares = features.attacked_squares;

    for (std::size_t color = 0; color < 2; ++color) {
        if (features.king_squares[color].index() < Square::kInvalid) {
            eval.kings[color] = features.king_squares[color].index();
        }
    }
    // Rebuild exact bitboards from the board snapshot.  The cached feature
    // bitboards are used when present, but the mailbox is authoritative and
    // also covers synthetic feature snapshots.
    eval.colors = {};
    for (auto& type_board : eval.pieces) {
        type_board = {};
    }
    eval.occupied = 0;
    for (int square = 0; square < 64; ++square) {
        const Piece piece = eval.board[static_cast<std::size_t>(square)];
        if (piece.empty()) {
            continue;
        }
        const std::size_t color = index_of(piece.color);
        const std::uint64_t square_bit = bit_of(square);
        eval.colors[color] |= square_bit;
        eval.pieces[color][type_index(piece.type)] |= square_bit;
        eval.occupied |= square_bit;
        if (piece.type == PieceType::king) {
            eval.kings[color] = square;
        }
    }
    for (std::size_t color = 0; color < 2; ++color) {
        std::uint64_t pawns = eval.pieces[color][type_index(PieceType::pawn)];
        while (pawns != 0) {
            const int square = std::countr_zero(pawns);
            pawns &= pawns - 1;
            eval.pawn_attacks[color] |=
                detail::pawn_attacks(square, color == kWhite);
        }
    }
    return eval;
}

[[nodiscard]] int pawn_attack_distance(int from, int to, bool white) noexcept {
    const int forward = white ? 1 : -1;
    return rank_of(to) - rank_of(from) == forward &&
        abs_int(file_of(to) - file_of(from)) == 1;
}

[[nodiscard]] Score pawn_structure(const EvalBoard& eval, std::size_t color) {
    Score score{};
    const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];
    const std::uint64_t enemy_pawns =
        eval.pieces[1 - color][type_index(PieceType::pawn)];
    const bool white = color == kWhite;

    // File counts for doubled/isolated detection.
    std::array<int, 8> own_file_count{};
    std::array<int, 8> enemy_file_count{};
    for (std::uint64_t pawns = own_pawns; pawns != 0;) {
        const int square = std::countr_zero(pawns);
        pawns &= pawns - 1;
        ++own_file_count[static_cast<std::size_t>(file_of(square))];
    }
    for (std::uint64_t pawns = enemy_pawns; pawns != 0;) {
        const int square = std::countr_zero(pawns);
        pawns &= pawns - 1;
        ++enemy_file_count[static_cast<std::size_t>(file_of(square))];
    }

    for (int file = 0; file < 8; ++file) {
        const bool isolated = (file == 0 || own_file_count[static_cast<std::size_t>(file - 1)] == 0) &&
            (file == 7 || own_file_count[static_cast<std::size_t>(file + 1)] == 0);
        if (own_file_count[static_cast<std::size_t>(file)] > 1) {
            const int extra = own_file_count[static_cast<std::size_t>(file)] - 1;
            score += Score{-8 * extra, -12 * extra};
        }
        if (isolated && own_file_count[static_cast<std::size_t>(file)] != 0) {
            score += Score{-14, -10};
        }
    }

    constexpr std::array<int, 8> kPassedMg{0, 6, 12, 22, 38, 65, 105, 0};
    constexpr std::array<int, 8> kPassedEg{0, 10, 20, 36, 62, 105, 165, 0};

    for (std::uint64_t pawns = own_pawns; pawns != 0;) {
        const int square = std::countr_zero(pawns);
        pawns &= pawns - 1;
        const int file = file_of(square);
        const int rank = rank_of(square);
        const int own_rank = white ? rank : 7 - rank;

        // Passed pawn: no enemy pawn on the same or adjacent file ahead.
        std::uint64_t forward_mask = 0;
        for (int forward_file = std::max(0, file - 1); forward_file <= std::min(7, file + 1);
             ++forward_file) {
            for (int forward_rank = white ? rank + 1 : rank - 1;
                 forward_rank >= 0 && forward_rank < 8;
                 forward_rank += white ? 1 : -1) {
                forward_mask |= bit_of(forward_rank * 8 + forward_file);
            }
        }
        const bool passed = (enemy_pawns & forward_mask) == 0;
        if (passed) {
            Score bonus{kPassedMg[static_cast<std::size_t>(own_rank)],
                        kPassedEg[static_cast<std::size_t>(own_rank)]};
            // The attacking king escorts a passer while the defending king is
            // pushed away; this is mostly an endgame term.
            if (eval.kings[color] >= 0 && eval.kings[1 - color] >= 0) {
                const int own_king_distance = square_distance(eval.kings[color], square);
                const int enemy_king_distance =
                    square_distance(eval.kings[1 - color], square);
                const int proximity = enemy_king_distance - own_king_distance;
                bonus += Score{proximity * 2, proximity * 5};
            }
            // A piece directly in front of the passer reduces its value.
            const int ahead_rank = white ? rank + 1 : rank - 1;
            if (ahead_rank >= 0 && ahead_rank < 8 &&
                ((eval.occupied >> (ahead_rank * 8 + file)) & 1U) != 0) {
                bonus += Score{-10, -16};
            }
            score += percent_of(bonus, hce_tuning().passed);
            continue;
        }

        // Connected/phalanx: a friendly pawn on an adjacent file at the same
        // rank or one rank behind supports this pawn.
        bool connected = false;
        for (int neighbor_file : {file - 1, file + 1}) {
            if (neighbor_file < 0 || neighbor_file > 7) {
                continue;
            }
            for (int neighbor_rank : {rank, white ? rank - 1 : rank + 1}) {
                if (neighbor_rank < 0 || neighbor_rank > 7) {
                    continue;
                }
                const int neighbor = neighbor_rank * 8 + neighbor_file;
                if (((own_pawns >> neighbor) & 1U) != 0) {
                    connected = true;
                }
            }
        }
        if (connected) {
            score += Score{6, 8};
        }

        // Candidate: no enemy pawn ahead on this file and a friendly pawn
        // behind/level on an adjacent file.
        bool enemy_ahead_on_file = false;
        for (int ahead = white ? rank + 1 : rank - 1; ahead >= 0 && ahead < 8;
             ahead += white ? 1 : -1) {
            if (((enemy_pawns >> (ahead * 8 + file)) & 1U) != 0) {
                enemy_ahead_on_file = true;
            }
        }
        if (!enemy_ahead_on_file && connected) {
            score += Score{5, 8};
        }
    }
    return score;
}

[[nodiscard]] Score king_safety(const EvalBoard& eval, std::size_t color) {
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
    score += Score{-attacker_count * attacker_count * 3, -attacker_count * 2};
    if (attacker_count >= 3) {
        score += Score{-18, -8};
    }

    // Pawn shelter and open files around the king.
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
            score += Score{-12, -5};
        }
        const std::uint64_t file_mask = 0x0101010101010101ULL << static_cast<unsigned>(file);
        if ((own_pawns & file_mask) == 0) {
            score += Score{-6, -2};
        }
    }
    return score;
}

[[nodiscard]] Score threats(const EvalBoard& eval, std::size_t color) {
    Score score{};
    const std::size_t enemy = 1 - color;
    const std::uint64_t own_attacks = eval.attacked_squares[color];
    const std::uint64_t enemy_defended = eval.attacked_squares[enemy];
    const std::uint64_t enemy_pieces = eval.colors[enemy] &
        ~eval.pieces[enemy][type_index(PieceType::king)] &
        ~eval.pieces[enemy][type_index(PieceType::pawn)];

    constexpr std::array<Score, 7> kHangingBonus{
        Score{}, Score{12, 18}, Score{40, 35}, Score{40, 35},
        Score{80, 60}, Score{150, 120}, Score{}};

    for (std::uint64_t remaining = enemy_pieces; remaining != 0;) {
        const int square = std::countr_zero(remaining);
        remaining &= remaining - 1;
        const std::uint64_t square_bit = bit_of(square);
        if ((own_attacks & square_bit) == 0) {
            continue;
        }
        const PieceType type = eval.board[static_cast<std::size_t>(square)].type;
        if ((enemy_defended & square_bit) == 0) {
            score += kHangingBonus[type_index(type)];
        } else if ((eval.pawn_attacks[color] & square_bit) != 0) {
            // A pawn attacking a defended piece still costs the defender
            // tempo/coordination; small bonus only.
            score += Score{8, 6};
        }
    }
    return score;
}

[[nodiscard]] Score piece_features(const EvalBoard& eval, std::size_t color) {
    Score score{};
    const bool white = color == kWhite;
    const std::uint64_t own_pawns = eval.pieces[color][type_index(PieceType::pawn)];
    const std::uint64_t enemy_pawns = eval.pieces[1 - color][type_index(PieceType::pawn)];
    const std::uint64_t enemy_pawn_attacks = eval.pawn_attacks[1 - color];

    if (std::popcount(eval.pieces[color][type_index(PieceType::bishop)]) >= 2) {
        score += Score{35, 50};
    }

    for (std::uint64_t knights = eval.pieces[color][type_index(PieceType::knight)];
         knights != 0;) {
        const int square = std::countr_zero(knights);
        knights &= knights - 1;
        const std::uint64_t square_bit = bit_of(square);
        if ((eval.pawn_attacks[color] & square_bit) != 0 &&
            (enemy_pawn_attacks & square_bit) == 0) {
            score += Score{25, 15};
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
            score += Score{20, 12};
        } else if (!own_pawn_on_file) {
            score += Score{10, 6};
        }
        const int own_rank = white ? rank_of(square) : 7 - rank_of(square);
        if (own_rank == 6) {
            score += Score{20, 25};
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

} // namespace

int ClassicalEvaluator::evaluate(const GameState& state, Color perspective) const {
    const EvalBoard eval = make_eval_board(state);
    if (insufficient_material(eval) || opposite_bishop_draw(eval)) {
        return 0;
    }

    Score total{};
    for (int square = 0; square < 64; ++square) {
        const Piece piece = eval.board[static_cast<std::size_t>(square)];
        if (piece.empty()) {
            continue;
        }
        const std::size_t color = index_of(piece.color);
        const Score value = material(piece.type) + piece_square(piece.type, square, piece.color);
        total += scaled(value, sign_of(color), sign_of(color));
    }

    for (std::size_t color = 0; color < 2; ++color) {
        const int sign = sign_of(color);
        Score side{};

        // Mobility by piece type.
        for (int square = 0; square < 64; ++square) {
            const Piece piece = eval.board[static_cast<std::size_t>(square)];
            if (piece.empty() || index_of(piece.color) != color) {
                continue;
            }
            const Score weight = mobility_weight(piece.type);
            const int mobility = eval.piece_mobility[static_cast<std::size_t>(square)];
            side += percent_of(Score{weight.mg * mobility, weight.eg * mobility},
                               hce_tuning().mobility);
        }

        side += percent_of(pawn_structure(eval, color), hce_tuning().pawn_structure);
        side += percent_of(king_safety(eval, color), hce_tuning().king_safety);
        side += percent_of(threats(eval, color), hce_tuning().threats);
        side += percent_of(piece_features(eval, color), hce_tuning().piece_terms);

        // Space/development are opening components.
        side += Score{eval.center_control[color] * 3, 0};
        side += Score{static_cast<int>(eval.development[color]) * 4, 0};

        total += scaled(side, sign, sign);
    }

    // Side-to-move tempo.  The score is White-positive, so Black's tempo
    // subtracts.
    const Score tempo = eval.side_to_move == Color::white ? Score{10, 10} : Score{-10, -10};
    total += percent_of(tempo, hce_tuning().tempo);

    int score = tapered(total, eval.phase);
    // Opposite bishops with no other material is drawish, but not a strict
    // rule draw; damp rather than zero it so small residual imbalances survive.
    if (opposite_bishop_draw(eval)) {
        score = score / 4;
    }
    score = score * hce_tuning().evaluation / 100;
    score = std::clamp(score, -detail::kMateThreshold + 1, detail::kMateThreshold - 1);
    return perspective == Color::white ? score : -score;
}

} // namespace koi
