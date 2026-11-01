#include "koi/game_state.hpp"

#include "koi/detail/game_state_impl.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace koi::detail {
namespace {

std::vector<std::string> sorted_native_move_strings(const Position& position) {
    std::vector<std::string> moves;
    for (const Move& move : position.legal_moves()) {
        moves.push_back(move.uci());
    }
    std::sort(moves.begin(), moves.end());
    return moves;
}

} // namespace

PositionConsistencySnapshot build_consistency_snapshot(const GameState& state) {
    PositionConsistencySnapshot snapshot;
    snapshot.native_fen = state.impl_->native_position.fen();
    snapshot.shadow_fen =
        state.impl_->compatibility_mirror.fen_for_comparison(state.impl_->native_position);
    snapshot.native_legal_moves = sorted_native_move_strings(state.impl_->native_position);
    snapshot.shadow_legal_moves = state.impl_->compatibility_mirror.legal_move_strings();
    snapshot.native_position_key = state.position_key();
    snapshot.shadow_position_key = state.impl_->compatibility_mirror.position_key();
    snapshot.native_castling_rights = state.castling_rights();
    snapshot.shadow_castling_rights = state.impl_->compatibility_mirror.castling_rights();
    snapshot.native_en_passant_square = state.en_passant_square();
    snapshot.shadow_en_passant_square = state.impl_->compatibility_mirror.en_passant_square_for_comparison(
        state.impl_->native_position);
    snapshot.native_halfmove_clock = state.halfmove_clock();
    snapshot.shadow_halfmove_clock = state.impl_->compatibility_mirror.halfmove_clock();
    snapshot.native_fullmove_number = state.fullmove_number();
    snapshot.shadow_fullmove_number = state.impl_->compatibility_mirror.fullmove_number();
    snapshot.native_repetition_count = state.repetition_count();
    snapshot.shadow_repetition_count = state.impl_->compatibility_mirror.repetition_count();
    snapshot.native_repetition_sensitive = state.is_repetition_sensitive();
    snapshot.shadow_repetition_sensitive = snapshot.shadow_repetition_count >= 2;
    snapshot.native_can_claim_threefold_repetition = state.can_claim_threefold_repetition();
    snapshot.native_is_automatic_fivefold_repetition = state.is_automatic_fivefold_repetition();
    snapshot.native_in_check = state.in_check();
    snapshot.shadow_in_check = state.impl_->compatibility_mirror.in_check();
    snapshot.native_side_to_move = state.side_to_move();
    snapshot.shadow_side_to_move = state.impl_->compatibility_mirror.side_to_move();
    const bool shadow_checkmate = snapshot.shadow_in_check && snapshot.shadow_legal_moves.empty();
    snapshot.shadow_can_claim_threefold_repetition =
        snapshot.shadow_repetition_count >= 3 && !shadow_checkmate;
    snapshot.shadow_is_automatic_fivefold_repetition =
        snapshot.shadow_repetition_count >= 5 && !shadow_checkmate;
    return snapshot;
}

} // namespace koi::detail
