#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "koi/detail/attack_tables.hpp"
#include "koi/game_state.hpp"
#include "koi/piece_values.hpp"
#include "koi/position.hpp"

namespace koi::detail {

// Returns the conservative material gain of a legal capture sequence on the
// destination square, from the moving side's perspective. Non-captures return
// zero.
[[nodiscard]] int static_exchange_gain(const GameState&, Move) noexcept;
[[nodiscard]] int static_exchange_gain(const GameState&, const MoveMetadata&) noexcept;

constexpr int exchange_piece_value(PieceType type) noexcept {
    return piece_material_value(type);
}

constexpr PieceType promotion_piece_type(Promotion promotion) noexcept {
    switch (promotion) {
    case Promotion::knight:
        return PieceType::knight;
    case Promotion::bishop:
        return PieceType::bishop;
    case Promotion::rook:
        return PieceType::rook;
    case Promotion::queen:
        return PieceType::queen;
    case Promotion::none:
        return PieceType::pawn;
    }
    return PieceType::pawn;
}

constexpr int promotion_exchange_gain(Promotion promotion) noexcept {
    return exchange_piece_value(promotion_piece_type(promotion)) - exchange_piece_value(PieceType::pawn);
}

constexpr bool exchange_square_valid(int square) noexcept {
    return square >= 0 && square < 64;
}

constexpr std::size_t exchange_color_index(Color color) noexcept {
    return color == Color::white ? 0U : 1U;
}

constexpr std::uint64_t exchange_square_bit(int square) noexcept {
    return std::uint64_t{1} << square;
}

// Lightweight board view for static exchange evaluation.  Exchange evaluation
// needs the mailbox layout, the maintained piece bitboards, combined
// occupancy, the king squares, and the side to move.  Building the full
// evaluation feature set (attack maps, pawn structure, phase, and so on) for
// every node that contains a capture was wasted work.
//
// `piece_bitboards` follows the native layout: one mask per color and piece
// type, indexed by static_cast<std::size_t>(PieceType), with slot 0
// (PieceType::none) unused.  The snapshot is built once from the native
// position and then maintained in lockstep with the mailbox through every
// trial mutation, so attacker enumeration and the recapture legality test are
// table lookups instead of 64-square rescans.  The emitted attacker set and
// the legality predicate are exactly the mailbox versions, so the integer
// result is bit-identical.
struct ExchangeContext {
    std::array<Piece, 64> board{};
    std::array<std::array<std::uint64_t, 7>, 2> piece_bitboards{};
    std::uint64_t occupied = 0;
    std::array<int, 2> king_squares{-1, -1};
    Color side_to_move = Color::white;
};

// Mirrors one mailbox store on `square` onto the maintained masks: any
// previous occupant is cleared from its piece bitboard and from occupancy,
// then `piece` (which may be empty) is installed in both views.  Every board
// mutation in the exchange loop goes through here, so the mailbox and the
// bitboards can never diverge.
inline void exchange_store_piece(ExchangeContext& context, int square, Piece piece) noexcept {
    const std::size_t index = static_cast<std::size_t>(square);
    const std::uint64_t square_bit = exchange_square_bit(square);
    const Piece previous = context.board[index];
    if (!previous.empty()) {
        context.piece_bitboards[exchange_color_index(previous.color)]
                               [static_cast<std::size_t>(previous.type)] &= ~square_bit;
        context.occupied &= ~square_bit;
    }
    context.board[index] = piece;
    if (!piece.empty()) {
        context.piece_bitboards[exchange_color_index(piece.color)]
                               [static_cast<std::size_t>(piece.type)] |= square_bit;
        context.occupied |= square_bit;
    }
}

// Builds the exchange baseline directly from the native position: the mailbox
// through copy_board_to, the piece masks through piece_bitboard, and combined
// occupancy as the union of the twelve masks.  No FeatureState or evaluation
// feature snapshot is consulted.
inline ExchangeContext exchange_context_from(const Position& position) noexcept {
    ExchangeContext context;
    position.copy_board_to(context.board);
    for (const Color color : {Color::white, Color::black}) {
        const std::size_t color_index = exchange_color_index(color);
        std::uint64_t color_pieces = 0;
        for (const PieceType type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                                     PieceType::rook, PieceType::queen, PieceType::king}) {
            const std::uint64_t pieces = position.piece_bitboard(type, color);
            context.piece_bitboards[color_index][static_cast<std::size_t>(type)] = pieces;
            color_pieces |= pieces;
        }
        context.occupied |= color_pieces;
        const std::uint64_t king =
            context.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
        if (king != 0) {
            context.king_squares[color_index] = static_cast<int>(std::countr_zero(king));
        }
    }
    context.side_to_move = position.side_to_move();
    return context;
}

// Same baseline through the public GameState surface, for search code that
// only holds a GameState.  GameState forwards copy_board_to, piece_bitboard,
// and side_to_move to the native Position, so this produces exactly the
// snapshot the Position overload above produces.
inline ExchangeContext exchange_context_from(const GameState& state) noexcept {
    ExchangeContext context;
    state.copy_board_to(context.board);
    for (const Color color : {Color::white, Color::black}) {
        const std::size_t color_index = exchange_color_index(color);
        std::uint64_t color_pieces = 0;
        for (const PieceType type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                                     PieceType::rook, PieceType::queen, PieceType::king}) {
            const std::uint64_t pieces = state.piece_bitboard(type, color);
            context.piece_bitboards[color_index][static_cast<std::size_t>(type)] = pieces;
            color_pieces |= pieces;
        }
        context.occupied |= color_pieces;
        const std::uint64_t king =
            context.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
        if (king != 0) {
            context.king_squares[color_index] = static_cast<int>(std::countr_zero(king));
        }
    }
    context.side_to_move = state.side_to_move();
    return context;
}

// True when `attacker` attacks `target` in the trial state.
inline bool exchange_square_attacked_by_masked(
    const std::array<std::uint64_t, 7>& enemy_pieces,
    std::uint64_t occupied,
    int target,
    Color attacker,
    std::uint64_t alive_mask) noexcept {
    if (!exchange_square_valid(target)) {
        return false;
    }
    const std::uint64_t pawns =
        enemy_pieces[static_cast<std::size_t>(PieceType::pawn)] & alive_mask;
    if ((pawn_attackers_of(target, attacker == Color::white) & pawns) != 0) {
        return true;
    }
    if ((knight_attacks(target) &
         (enemy_pieces[static_cast<std::size_t>(PieceType::knight)] & alive_mask)) != 0) {
        return true;
    }
    if ((king_attacks(target) &
         (enemy_pieces[static_cast<std::size_t>(PieceType::king)] & alive_mask)) != 0) {
        return true;
    }
    const std::uint64_t queens =
        enemy_pieces[static_cast<std::size_t>(PieceType::queen)] & alive_mask;
    const std::uint64_t bishops =
        (enemy_pieces[static_cast<std::size_t>(PieceType::bishop)] & alive_mask) | queens;
    if (bishops != 0 && (bishop_attacks(target, occupied) & bishops) != 0) {
        return true;
    }
    const std::uint64_t rooks =
        (enemy_pieces[static_cast<std::size_t>(PieceType::rook)] & alive_mask) | queens;
    if (rooks != 0 && (rook_attacks(target, occupied) & rooks) != 0) {
        return true;
    }
    return false;
}

inline bool exchange_square_attacked_by(const ExchangeContext& context, int target,
                                        Color attacker) noexcept {
    const std::size_t color_index = exchange_color_index(attacker);
    return exchange_square_attacked_by_masked(
        context.piece_bitboards[color_index], context.occupied, target, attacker,
        ~std::uint64_t{0});
}

// Finds the least-valuable legal recapture square for `side` on `target` (with
// lowest square index breaking any equal-type ties), stopping as soon as the
// cheapest piece bucket yields a legal move. Piece material values are strictly
// ordered: pawn < knight < bishop < rook < queen < king.
[[nodiscard]] inline int find_least_valuable_legal_attacker(
    const ExchangeContext& context,
    int target,
    Color side,
    Piece& recapturing_piece,
    int& promotion_gain) noexcept {
    static_assert(piece_material_value(PieceType::pawn) < piece_material_value(PieceType::knight) &&
                  piece_material_value(PieceType::knight) < piece_material_value(PieceType::bishop) &&
                  piece_material_value(PieceType::bishop) < piece_material_value(PieceType::rook) &&
                  piece_material_value(PieceType::rook) < piece_material_value(PieceType::queen) &&
                  piece_material_value(PieceType::queen) < piece_material_value(PieceType::king));
    if (!exchange_square_valid(target)) {
        return -1;
    }
    const std::size_t side_index = exchange_color_index(side);
    const std::size_t opp_index = 1U - side_index;
    const auto& own_pieces = context.piece_bitboards[side_index];
    const auto& enemy_pieces = context.piece_bitboards[opp_index];
    const Color enemy = opposite(side);
    const std::uint64_t target_bit = exchange_square_bit(target);
    const std::uint64_t alive_enemy_mask = ~target_bit;
    const int own_king_sq = context.king_squares[side_index];
    const bool pawn_promotes = (target >> 3 == 0 || target >> 3 == 7);

    const auto is_legal_non_king = [&](int source) noexcept {
        const std::uint64_t occ_after =
            (context.occupied & ~exchange_square_bit(source)) | target_bit;
        return !exchange_square_attacked_by_masked(
            enemy_pieces, occ_after, own_king_sq, enemy, alive_enemy_mask);
    };

    const auto try_bucket = [&](std::uint64_t candidates, PieceType type,
                                bool promotes) noexcept -> int {
        while (candidates != 0) {
            const int source = static_cast<int>(std::countr_zero(candidates));
            candidates &= candidates - 1;
            if (type == PieceType::king) {
                const std::uint64_t occ_after =
                    (context.occupied & ~exchange_square_bit(source)) | target_bit;
                if (!exchange_square_attacked_by_masked(
                        enemy_pieces, occ_after, target, enemy, alive_enemy_mask)) {
                    recapturing_piece = Piece{PieceType::king, side};
                    promotion_gain = 0;
                    return source;
                }
            } else if (is_legal_non_king(source)) {
                recapturing_piece = Piece{promotes ? PieceType::queen : type, side};
                promotion_gain = promotes
                    ? (exchange_piece_value(PieceType::queen) -
                       exchange_piece_value(PieceType::pawn))
                    : 0;
                return source;
            }
        }
        return -1;
    };

    if (const std::uint64_t pawns =
            pawn_attackers_of(target, side == Color::white) &
            own_pieces[static_cast<std::size_t>(PieceType::pawn)];
        pawns != 0) {
        if (const int sq = try_bucket(pawns, PieceType::pawn, pawn_promotes); sq >= 0) {
            return sq;
        }
    }
    if (const std::uint64_t knights =
            knight_attacks(target) & own_pieces[static_cast<std::size_t>(PieceType::knight)];
        knights != 0) {
        if (const int sq = try_bucket(knights, PieceType::knight, false); sq >= 0) {
            return sq;
        }
    }

    const std::uint64_t own_bishops = own_pieces[static_cast<std::size_t>(PieceType::bishop)];
    const std::uint64_t own_rooks = own_pieces[static_cast<std::size_t>(PieceType::rook)];
    const std::uint64_t own_queens = own_pieces[static_cast<std::size_t>(PieceType::queen)];
    std::uint64_t diag_attacks = 0;
    if ((own_bishops | own_queens) != 0) {
        diag_attacks = bishop_attacks(target, context.occupied);
        if (const std::uint64_t bishops = diag_attacks & own_bishops; bishops != 0) {
            if (const int sq = try_bucket(bishops, PieceType::bishop, false); sq >= 0) {
                return sq;
            }
        }
    }
    std::uint64_t orth_attacks = 0;
    if ((own_rooks | own_queens) != 0) {
        orth_attacks = rook_attacks(target, context.occupied);
        if (const std::uint64_t rooks = orth_attacks & own_rooks; rooks != 0) {
            if (const int sq = try_bucket(rooks, PieceType::rook, false); sq >= 0) {
                return sq;
            }
        }
    }
    if (const std::uint64_t queens = (diag_attacks | orth_attacks) & own_queens; queens != 0) {
        if (const int sq = try_bucket(queens, PieceType::queen, false); sq >= 0) {
            return sq;
        }
    }
    if (const std::uint64_t kings =
            king_attacks(target) & own_pieces[static_cast<std::size_t>(PieceType::king)];
        kings != 0) {
        if (const int sq = try_bucket(kings, PieceType::king, false); sq >= 0) {
            return sq;
        }
    }
    return -1;
}

inline int static_exchange_gain_from_features(const ExchangeContext& context,
                                              const MoveMetadata& initial) noexcept {
    const Move move = initial.move;
    if (move.is_no_move() || move.from().index() == Square::kInvalid ||
        move.to().index() == Square::kInvalid ||
        (!initial.is_capture() && initial.captured_piece == PieceType::none)) {
        return 0;
    }

    ExchangeContext trial = context;
    const int source = move.from().index();
    const int target = move.to().index();
    if (!exchange_square_valid(source) || !exchange_square_valid(target)) {
        return 0;
    }

    const Piece moving_piece = trial.board[static_cast<std::size_t>(source)];
    if (moving_piece.empty() || moving_piece.color != trial.side_to_move ||
        moving_piece.type != initial.moving_piece || trial.king_squares[0] < 0 ||
        trial.king_squares[1] < 0) {
        return 0;
    }

    int captured_square = target;
    if (initial.kind == MoveKind::en_passant) {
        captured_square += moving_piece.color == Color::white ? -8 : 8;
    }
    if (!exchange_square_valid(captured_square)) {
        return 0;
    }

    constexpr std::size_t kMaximumExchangeDepth = 32;
    // Deliberately not zero-initialized: every read below touches only entries
    // 0..depth, and each entry is written before its first read (gains[0]
    // just below, gains[depth] right after ++depth), so clearing 32 slots per
    // capture was dead work.
    std::array<int, kMaximumExchangeDepth> gains;
    const Piece captured_piece = trial.board[static_cast<std::size_t>(captured_square)];
    gains[0] = exchange_piece_value(captured_piece.type) + promotion_exchange_gain(move.promotion());

    Piece placed_piece = moving_piece;
    if (move.promotion() != Promotion::none) {
        placed_piece.type = promotion_piece_type(move.promotion());
    }
    exchange_store_piece(trial, source, Piece{});
    exchange_store_piece(trial, captured_square, Piece{});
    exchange_store_piece(trial, target, placed_piece);
    if (moving_piece.type == PieceType::king) {
        trial.king_squares[exchange_color_index(moving_piece.color)] = target;
    }

    Color side = opposite(moving_piece.color);
    std::size_t depth = 0;
    for (;;) {
        Piece recapturing_piece{};
        int recapture_promotion_gain = 0;
        const int attacker_square = find_least_valuable_legal_attacker(
            trial, target, side, recapturing_piece, recapture_promotion_gain);

        if (attacker_square < 0 || depth + 1 >= kMaximumExchangeDepth) {
            break;
        }

        const Piece target_piece = trial.board[static_cast<std::size_t>(target)];
        ++depth;
        gains[depth] = exchange_piece_value(target_piece.type) - gains[depth - 1] +
            recapture_promotion_gain;

        const Piece departing_piece = trial.board[static_cast<std::size_t>(attacker_square)];
        exchange_store_piece(trial, attacker_square, Piece{});
        exchange_store_piece(trial, target, recapturing_piece);
        if (departing_piece.type == PieceType::king) {
            trial.king_squares[exchange_color_index(side)] = target;
        }
        side = opposite(side);
    }

    while (depth > 0) {
        gains[depth - 1] = -std::max(-gains[depth - 1], gains[depth]);
        --depth;
    }
    return gains[0];
}

// Evaluates one move against an already-built position baseline.  Callers that
// evaluate several moves of the same position (GameState::finalize_metadata,
// SearchMoveOrdering::order, SearchMovePicker) build the context once instead
// of rebuilding the mailbox and piece masks per capture; the score itself is
// identical to static_exchange_gain(GameState, MoveMetadata).
[[nodiscard]] inline int static_exchange_gain(const ExchangeContext& context,
                                              const MoveMetadata& initial) noexcept {
    return static_exchange_gain_from_features(context, initial);
}

} // namespace koi::detail
