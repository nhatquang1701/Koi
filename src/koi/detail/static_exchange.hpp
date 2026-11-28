#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

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

// Lightweight bitboard view for static exchange evaluation.  Exchange evaluation
// only needs the maintained piece bitboards, combined occupancy, the king
// squares, and the side to move — avoiding both the 128-byte mailbox copy
// and full evaluation feature extraction.
//
// `piece_bitboards` follows the native layout: one mask per color and piece
// type, indexed by static_cast<std::size_t>(PieceType), with slot 0
// (PieceType::none) unused.
struct ExchangeContext {
    const std::array<std::array<std::uint64_t, 7>, 2>* piece_bitboards = nullptr;
    std::uint64_t occupied = 0;
    std::array<int, 2> king_squares{-1, -1};
    Color side_to_move = Color::white;
};

template <typename Snapshot>
[[nodiscard]] inline ExchangeContext exchange_context_from_snapshot(const Snapshot& snap) noexcept {
    ExchangeContext context;
    context.piece_bitboards = snap.piece_bitboards;
    context.occupied = snap.occupied;
    context.side_to_move = snap.side_to_move;
    if (context.piece_bitboards != nullptr) {
        const auto& bitboards = *context.piece_bitboards;
        for (std::size_t color_index = 0; color_index < 2; ++color_index) {
            const std::uint64_t king =
                bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
            if (king != 0) {
                context.king_squares[color_index] = static_cast<int>(std::countr_zero(king));
            }
        }
    }
    return context;
}

// Builds the exchange baseline directly from the native position bitboards.
inline ExchangeContext exchange_context_from(const Position& position) noexcept {
    return exchange_context_from_snapshot(position.exchange_snapshot());
}

// Same baseline through the public GameState surface, for search code that
// only holds a GameState.
inline ExchangeContext exchange_context_from(const GameState& state) noexcept {
    return exchange_context_from_snapshot(state.exchange_snapshot());
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
    if (bishops != 0 && (bishop_pseudo_attacks(target) & bishops) != 0 &&
        (bishop_attacks(target, occupied) & bishops) != 0) {
        return true;
    }
    const std::uint64_t rooks =
        (enemy_pieces[static_cast<std::size_t>(PieceType::rook)] & alive_mask) | queens;
    if (rooks != 0 && (rook_pseudo_attacks(target) & rooks) != 0 &&
        (rook_attacks(target, occupied) & rooks) != 0) {
        return true;
    }
    return false;
}

#if defined(__clang__) || defined(__GNUC__)
#define KOI_SEE_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define KOI_SEE_ALWAYS_INLINE __forceinline
#else
#define KOI_SEE_ALWAYS_INLINE inline
#endif

struct ExchangeLegalityState {
    const std::array<std::uint64_t, 7>& enemy_pieces;
    std::uint64_t occupied;
    std::uint64_t target_bit;
    std::uint64_t alive_enemy_mask;
    int own_king_sq;
    Color enemy;
    bool ready = false;
    bool king_valid = false;
    bool always_in_check = false;
    std::uint64_t diag_candidates = 0;
    std::uint64_t orth_candidates = 0;
    std::uint64_t slider_candidates = 0;

    [[nodiscard]] KOI_SEE_ALWAYS_INLINE bool is_legal_non_king(int source) noexcept {
        if (!ready) {
            ready = true;
            king_valid = exchange_square_valid(own_king_sq);
            if (!king_valid) {
                return true;
            }
            const std::uint64_t enemy_pawns =
                enemy_pieces[static_cast<std::size_t>(PieceType::pawn)] & alive_enemy_mask;
            if ((pawn_attackers_of(own_king_sq, enemy == Color::white) & enemy_pawns) != 0) {
                always_in_check = true;
                return false;
            }
            const std::uint64_t enemy_knights =
                enemy_pieces[static_cast<std::size_t>(PieceType::knight)] & alive_enemy_mask;
            if ((knight_attacks(own_king_sq) & enemy_knights) != 0) {
                always_in_check = true;
                return false;
            }
            const std::uint64_t enemy_kings =
                enemy_pieces[static_cast<std::size_t>(PieceType::king)] & alive_enemy_mask;
            if ((king_attacks(own_king_sq) & enemy_kings) != 0) {
                always_in_check = true;
                return false;
            }
            const std::uint64_t enemy_queens =
                enemy_pieces[static_cast<std::size_t>(PieceType::queen)];
            const std::uint64_t enemy_bq =
                (enemy_pieces[static_cast<std::size_t>(PieceType::bishop)] | enemy_queens) &
                alive_enemy_mask;
            const std::uint64_t enemy_rq =
                (enemy_pieces[static_cast<std::size_t>(PieceType::rook)] | enemy_queens) &
                alive_enemy_mask;
            diag_candidates =
                enemy_bq != 0 ? (bishop_pseudo_attacks(own_king_sq) & enemy_bq) : 0;
            orth_candidates =
                enemy_rq != 0 ? (rook_pseudo_attacks(own_king_sq) & enemy_rq) : 0;
            slider_candidates = diag_candidates | orth_candidates;
            if (slider_candidates != 0) {
                const std::uint64_t base_occ = occupied | target_bit;
                if ((diag_candidates != 0 &&
                     (bishop_attacks(own_king_sq, base_occ) & diag_candidates) != 0) ||
                    (orth_candidates != 0 &&
                     (rook_attacks(own_king_sq, base_occ) & orth_candidates) != 0)) {
                    always_in_check = true;
                    return false;
                }
            }
        }
        if (!king_valid) {
            return true;
        }
        if (always_in_check) {
            return false;
        }
        const std::uint64_t line = detail::line_mask(own_king_sq, source);
        if ((line & slider_candidates) == 0) {
            return true;
        }
        const std::uint64_t occ_after = (occupied & ~exchange_square_bit(source)) | target_bit;
        if (const std::uint64_t aligned_diag = diag_candidates & line;
            aligned_diag != 0 && (bishop_attacks(own_king_sq, occ_after) & aligned_diag) != 0) {
            return false;
        }
        if (const std::uint64_t aligned_orth = orth_candidates & line;
            aligned_orth != 0 && (rook_attacks(own_king_sq, occ_after) & aligned_orth) != 0) {
            return false;
        }
        return true;
    }
};

template <PieceType Type, bool Promotes>
[[nodiscard]] KOI_SEE_ALWAYS_INLINE int try_exchange_bucket(
    std::uint64_t candidates,
    Color side,
    ExchangeLegalityState& legality,
    Piece& recapturing_piece,
    int& promotion_gain) noexcept {
    while (candidates != 0) {
        const int source = static_cast<int>(std::countr_zero(candidates));
        candidates &= candidates - 1;
        if (legality.is_legal_non_king(source)) {
            recapturing_piece = Piece{Promotes ? PieceType::queen : Type, side};
            promotion_gain = Promotes
                ? (exchange_piece_value(PieceType::queen) -
                   exchange_piece_value(PieceType::pawn))
                : 0;
            return source;
        }
    }
    return -1;
}

#undef KOI_SEE_ALWAYS_INLINE

// Finds the least-valuable legal recapture square for `side` on `target` (with
// lowest square index breaking any equal-type ties), stopping as soon as the
// cheapest piece bucket yields a legal move. Piece material values are strictly
// ordered: pawn < knight < bishop < rook < queen < king.
[[nodiscard]] inline int find_least_valuable_legal_attacker(
    const std::array<std::array<std::uint64_t, 7>, 2>& piece_bitboards,
    const std::uint64_t occupied,
    const std::array<int, 2>& king_squares,
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
    const auto& own_pieces = piece_bitboards[side_index];
    const auto& enemy_pieces = piece_bitboards[opp_index];
    const Color enemy = opposite(side);
    const std::uint64_t target_bit = exchange_square_bit(target);
    const std::uint64_t alive_mask = occupied & ~target_bit;
    const int own_king_sq = king_squares[side_index];
    const bool pawn_promotes = (target >> 3 == 0 || target >> 3 == 7);

    // Lazily precompute aligned enemy sliders and non-slider checks on `own_king_sq`
    // once per attacker search: since `occ_after = (occupied | target_bit) & ~src_bit`
    // only clears `src_bit`, non-aligned attackers cannot expose a slider pin, and
    // non-slider checks depend only on `own_king_sq` and `alive_mask`.
    ExchangeLegalityState legality{
        enemy_pieces, occupied, target_bit, alive_mask, own_king_sq, enemy};

    if (const std::uint64_t pawns =
            pawn_attackers_of(target, side == Color::white) &
            (own_pieces[static_cast<std::size_t>(PieceType::pawn)] & alive_mask);
        pawns != 0) {
        const int sq = pawn_promotes
            ? try_exchange_bucket<PieceType::pawn, true>(
                  pawns, side, legality, recapturing_piece, promotion_gain)
            : try_exchange_bucket<PieceType::pawn, false>(
                  pawns, side, legality, recapturing_piece, promotion_gain);
        if (sq >= 0) {
            return sq;
        }
    }
    if (!legality.always_in_check) {
        if (const std::uint64_t knights =
                knight_attacks(target) &
                (own_pieces[static_cast<std::size_t>(PieceType::knight)] & alive_mask);
            knights != 0) {
            if (const int sq = try_exchange_bucket<PieceType::knight, false>(
                    knights, side, legality, recapturing_piece, promotion_gain);
                sq >= 0) {
                return sq;
            }
        }

        const std::uint64_t own_bishops =
            own_pieces[static_cast<std::size_t>(PieceType::bishop)] & alive_mask;
        const std::uint64_t own_rooks =
            own_pieces[static_cast<std::size_t>(PieceType::rook)] & alive_mask;
        const std::uint64_t own_queens =
            own_pieces[static_cast<std::size_t>(PieceType::queen)] & alive_mask;
        const std::uint64_t own_bq = own_bishops | own_queens;
        const std::uint64_t own_rq = own_rooks | own_queens;
        std::uint64_t diag_attacks = 0;
        if (own_bq != 0 && !legality.always_in_check &&
            (bishop_pseudo_attacks(target) & own_bq) != 0) {
            diag_attacks = bishop_attacks(target, occupied);
            if (const std::uint64_t bishops = diag_attacks & own_bishops; bishops != 0) {
                if (const int sq = try_exchange_bucket<PieceType::bishop, false>(
                        bishops, side, legality, recapturing_piece, promotion_gain);
                    sq >= 0) {
                    return sq;
                }
            }
        }
        std::uint64_t orth_attacks = 0;
        if (own_rq != 0 && !legality.always_in_check &&
            (rook_pseudo_attacks(target) & own_rq) != 0) {
            orth_attacks = rook_attacks(target, occupied);
            if (const std::uint64_t rooks = orth_attacks & own_rooks; rooks != 0) {
                if (const int sq = try_exchange_bucket<PieceType::rook, false>(
                        rooks, side, legality, recapturing_piece, promotion_gain);
                    sq >= 0) {
                    return sq;
                }
            }
        }
        if (const std::uint64_t queens = (diag_attacks | orth_attacks) & own_queens;
            queens != 0 && !legality.always_in_check) {
            if (const int sq = try_exchange_bucket<PieceType::queen, false>(
                    queens, side, legality, recapturing_piece, promotion_gain);
                sq >= 0) {
                return sq;
            }
        }
    }
    if (std::uint64_t kings =
            king_attacks(target) &
            (own_pieces[static_cast<std::size_t>(PieceType::king)] & alive_mask);
        kings != 0) {
        while (kings != 0) {
            const int source = static_cast<int>(std::countr_zero(kings));
            kings &= kings - 1;
            const std::uint64_t occ_after =
                (occupied & ~exchange_square_bit(source)) | target_bit;
            if (!exchange_square_attacked_by_masked(
                    enemy_pieces, occ_after, target, enemy, alive_mask)) {
                recapturing_piece = Piece{PieceType::king, side};
                promotion_gain = 0;
                return source;
            }
        }
    }
    return -1;
}

inline int static_exchange_gain_from_features(const ExchangeContext& context,
                                              const MoveMetadata& initial) noexcept {
    const Move move = initial.move;
    if (context.piece_bitboards == nullptr || move.is_no_move() ||
        move.from().index() == Square::kInvalid ||
        move.to().index() == Square::kInvalid ||
        (!initial.is_capture() && initial.captured_piece == PieceType::none)) {
        return 0;
    }

    const auto& piece_bitboards = *context.piece_bitboards;
    std::uint64_t occupied = context.occupied;
    std::array<int, 2> king_squares = context.king_squares;
    const int source = move.from().index();
    const int target = move.to().index();
    if (!exchange_square_valid(source) || !exchange_square_valid(target)) {
        return 0;
    }

    const Color moving_color = context.side_to_move;
    const std::size_t moving_color_index = exchange_color_index(moving_color);
    const std::size_t moving_type_index = static_cast<std::size_t>(initial.moving_piece);
    const std::uint64_t source_bit = exchange_square_bit(source);
    if (moving_type_index == 0 ||
        (piece_bitboards[moving_color_index][moving_type_index] & source_bit) == 0 ||
        king_squares[0] < 0 || king_squares[1] < 0) {
        return 0;
    }

    int captured_square = target;
    if (initial.kind == MoveKind::en_passant) {
        captured_square += moving_color == Color::white ? -8 : 8;
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
    const std::uint64_t target_bit = exchange_square_bit(target);
    const std::uint64_t captured_bit = exchange_square_bit(captured_square);
    const std::size_t opp_color_index = 1U - moving_color_index;
    PieceType captured_type = PieceType::none;
    const std::size_t hint_captured_index = static_cast<std::size_t>(initial.captured_piece);
    if (hint_captured_index != 0 &&
        (piece_bitboards[opp_color_index][hint_captured_index] & captured_bit) != 0) {
        captured_type = initial.captured_piece;
        occupied &= ~captured_bit;
    } else {
        for (std::size_t c = 0; c < 2 && captured_type == PieceType::none; ++c) {
            for (std::size_t t = 1; t <= 6; ++t) {
                if ((piece_bitboards[c][t] & captured_bit) != 0) {
                    captured_type = static_cast<PieceType>(t);
                    occupied &= ~captured_bit;
                    break;
                }
            }
        }
    }
    gains[0] = exchange_piece_value(captured_type) + promotion_exchange_gain(move.promotion());

    PieceType placed_type = initial.moving_piece;
    if (move.promotion() != Promotion::none) {
        placed_type = promotion_piece_type(move.promotion());
    }
    occupied = (occupied & ~source_bit) | target_bit;
    if (initial.moving_piece == PieceType::king) {
        king_squares[moving_color_index] = target;
    }

    Color side = opposite(moving_color);
    PieceType target_type = placed_type;
    std::size_t depth = 0;
    for (;;) {
        Piece recapturing_piece{};
        int recapture_promotion_gain = 0;
        const int attacker_square = find_least_valuable_legal_attacker(
            piece_bitboards, occupied, king_squares, target, side,
            recapturing_piece, recapture_promotion_gain);

        if (attacker_square < 0 || depth + 1 >= kMaximumExchangeDepth) {
            break;
        }

        ++depth;
        gains[depth] = exchange_piece_value(target_type) - gains[depth - 1] +
            recapture_promotion_gain;

        const std::size_t cur_index = exchange_color_index(side);
        const PieceType departing_type =
            recapture_promotion_gain != 0 ? PieceType::pawn : recapturing_piece.type;
        const std::uint64_t attacker_bit = exchange_square_bit(attacker_square);
        occupied &= ~attacker_bit;
        if (departing_type == PieceType::king) {
            king_squares[cur_index] = target;
        }
        target_type = recapturing_piece.type;
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
