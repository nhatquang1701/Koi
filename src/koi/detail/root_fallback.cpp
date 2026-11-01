#include "koi/detail/root_fallback.hpp"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "koi/detail/search_context_support.hpp"
#include "koi/evaluator.hpp"
#include "koi/time_manager.hpp"

namespace koi::detail {

namespace {

constexpr int kEmergencyKingEscapeSafetyMargin = 650;
constexpr int kEmergencyKingZoneAttackWeight = 400;
constexpr int kEmergencyKingCaptureRisk = 160;
constexpr int kEmergencyQuietForcingMargin = 200;
// A narrow check can look artificially dominant in the bounded fallback
// because its small evasion set lets the one-ply oracle see the same tactical
// resource repeatedly.  Allow a sound, non-hanging major-piece check with a
// larger evasion set to displace that narrow check when the raw scores are
// still close.  This remains local to an interrupted root; completed search
// keeps the normal score ordering.
constexpr int kEmergencyBroadCheckReplacementMargin = 400;
constexpr int kEmergencySafeCaptureTieMargin = 32;
constexpr int kEmergencySafeExchangeTieMargin = 80;
// In an interrupted root, an irreversible quiet pawn push that leaves an
// immediate checking resource is more dangerous than a similarly scored
// piece move. Keep this discount local to the unsafe fallback ranking; normal
// completed search and ordinary pawn evaluation are unchanged.
constexpr int kEmergencyUnsafePawnQuietPenalty = 75;
constexpr std::size_t kMaximumEmergencyForcingCheckEvasions = 3;
constexpr std::size_t kMaximumEmergencyQuietForcingResearches = 16;
constexpr std::size_t kMaximumEmergencyRootEvasionReplies = 6;

} // namespace

bool FallbackControl::interrupted() const noexcept {
    if (stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed)) {
        return true;
    }
    return time_manager != nullptr && time_manager->should_stop(0);
}

int apply_claimable_draw_option(const GameState& state, const Color root_color,
                                const int score) noexcept {
    // Preserve the interruption sentinel: it is control flow, not a chess
    // score that can be replaced by the draw option.
    if (score <= -kInfinity || !state.is_claimable_draw()) {
        return score;
    }
    return state.side_to_move() == root_color ?
        std::max(0, score) : std::min(0, score);
}

int short_fallback_evaluate(const Evaluator& evaluator, const GameState& state,
                            const Color root_color, std::mutex* evaluator_mutex) {
    int score = 0;
    if (evaluator_mutex != nullptr) {
        std::lock_guard lock(*evaluator_mutex);
        score = evaluator.evaluate(state, root_color);
    } else {
        score = evaluator.evaluate(state, root_color);
    }
    return apply_claimable_draw_option(state, root_color, score);
}

// Emergency scanners evaluate a position from the original root's
// perspective, while the side to move changes after every copied move.  Keep
// terminal ordering in one place: a no-legal-move mate must win over a rule
// draw, and a stalemate must remain neutral.  Returning nullopt means the
// position is live and not currently draw-terminated.
std::optional<int> fallback_terminal_or_draw_score(
    const GameState& state, const Color root_color, const int mate_distance) {
    const std::vector<Move> legal_moves = state.legal_moves();
    if (legal_moves.empty()) {
        if (!state.in_check()) {
            return 0;
        }
        return state.side_to_move() == root_color ?
            -kMateScore + mate_distance : kMateScore - mate_distance;
    }
    // Claimable draws are not terminal: the emergency scanner must continue
    // through legal moves and let the side to move choose the zero option.
    if (state.is_forced_draw()) {
        return 0;
    }
    return std::nullopt;
}

std::optional<int> emergency_king_escape_risk(
    const GameState& root, const MoveMetadata& metadata,
    const GameState& after_move) noexcept {
    if (!root.in_check() || metadata.moving_piece != PieceType::king) {
        return std::nullopt;
    }

    const std::size_t side = root.side_to_move() == Color::white ? 0U : 1U;
    const auto features = after_move.position_features();
    return static_cast<int>(features.king_zone_attacks[side]) *
            kEmergencyKingZoneAttackWeight +
        (metadata.is_capture() ? kEmergencyKingCaptureRisk : 0);
}

int quiet_forcing_fallback_score(
    const GameState& after_move, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_move, root_color, 1); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList opponent_moves;
    after_move.legal_moves_with_metadata(
        opponent_moves, true, false, CheckFlagMode::all_moves);
    if (opponent_moves.empty()) {
        // after_move is the position immediately after the root candidate,
        // so no legal reply while in check means the root side delivered
        // mate, not that it was mated.
        return after_move.in_check() ? kMateScore - 1 : 0;
    }

    int worst_score = kInfinity;
    bool found_reply = false;
    for (const MoveMetadata& opponent_move : opponent_moves) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_opponent = after_move;
        if (!after_opponent.make_search_move(opponent_move)) {
            continue;
        }

        if (const auto terminal = fallback_terminal_or_draw_score(
                after_opponent, root_color, 2); terminal.has_value()) {
            worst_score = std::min(worst_score, *terminal);
            found_reply = true;
            continue;
        }

        MoveMetadataList responses;
        after_opponent.legal_moves_with_metadata(
            responses, true, false, CheckFlagMode::all_moves);
        if (responses.empty()) {
            worst_score = std::min(
                worst_score, after_opponent.in_check() ? -kMateScore + 2 : 0);
            found_reply = true;
            continue;
        }

        // The root side may already be able to claim a draw after the
        // opponent's reply.  That claim is a real zero-valued response and
        // must compete with the legal continuations below; otherwise a
        // short fallback can incorrectly treat an all-losing response set as
        // an interruption sentinel and let the opponent's minimization hide
        // the draw option.
        int best_response_score = after_opponent.is_claimable_draw() ? 0 : -kInfinity;
        for (const MoveMetadata& response : responses) {
            if (control != nullptr && control->interrupted()) {
                return -kInfinity;
            }
            GameState after_response = after_opponent;
            if (!after_response.make_search_move(response)) {
                continue;
            }
            if (const auto terminal = fallback_terminal_or_draw_score(
                    after_response, root_color, 2); terminal.has_value()) {
                best_response_score = std::max(best_response_score, *terminal);
                continue;
            }
            best_response_score = std::max(
                best_response_score,
                short_fallback_evaluate(evaluator, after_response,
                                        root_color, evaluator_mutex));
        }
        if (best_response_score > -kInfinity) {
            worst_score = std::min(worst_score, best_response_score);
            found_reply = true;
        }
    }
    return apply_claimable_draw_option(
        after_move, root_color, found_reply ? worst_score : -kInfinity);
}

int checked_root_fallback_score(
    const GameState& after_check, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_check, root_color, 1); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, true, CheckFlagMode::all_moves);
    if (evasions.empty()) {
        return after_check.in_check() ? kMateScore - 1 : 0;
    }

    int worst_evasion_score = kInfinity;
    bool found_evasion = false;
    for (const MoveMetadata& evasion : evasions) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_evasion = after_check;
        if (!after_evasion.make_search_move(evasion)) {
            continue;
        }
        if (const auto terminal = fallback_terminal_or_draw_score(
                after_evasion, root_color, 2); terminal.has_value()) {
            worst_evasion_score = std::min(worst_evasion_score, *terminal);
            found_evasion = true;
            continue;
        }
        // A static score immediately after the evasion rewards a checking
        // move even when the checked side's reply leaves a quiet, strong
        // continuation for the original side.  Give the original side one
        // bounded reply after every legal evasion before comparing checking
        // root candidates.  This remains confined to the emergency short
        // fallback; the normal search and qsearch retain their usual depth.
        MoveMetadataList replies;
        after_evasion.legal_moves_with_metadata(
            replies, true, false, CheckFlagMode::all_moves);
        if (replies.empty()) {
            worst_evasion_score = std::min(
                worst_evasion_score,
                after_evasion.in_check() ? -kMateScore + 2 : 0);
            found_evasion = true;
            continue;
        }

        int best_reply_score = short_fallback_evaluate(
            evaluator, after_evasion, root_color, evaluator_mutex);
        for (const MoveMetadata& reply : replies) {
            if (control != nullptr && control->interrupted()) {
                return -kInfinity;
            }
            GameState after_reply = after_evasion;
            if (!after_reply.make_search_move(reply)) {
                continue;
            }
            if (const auto terminal = fallback_terminal_or_draw_score(
                    after_reply, root_color, 2); terminal.has_value()) {
                best_reply_score = std::max(best_reply_score, *terminal);
                continue;
            }
            int reply_score = short_fallback_evaluate(
                evaluator, after_reply, root_color, evaluator_mutex);
            if (after_reply.in_check() && after_reply.legal_moves().empty()) {
                reply_score = kMateScore - 2;
            }
            best_reply_score = std::max(best_reply_score, reply_score);
        }
        worst_evasion_score = std::min(worst_evasion_score, best_reply_score);
        found_evasion = true;
    }
    return apply_claimable_draw_option(
        after_check, root_color, found_evasion ? worst_evasion_score : -kInfinity);
}

bool hanging_major_piece_check(const GameState& after_check,
                               const MoveMetadata& checking_move) noexcept {
    if (!after_check.in_check() ||
        (checking_move.moving_piece != PieceType::queen &&
         checking_move.moving_piece != PieceType::rook)) {
        return false;
    }
    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, false, CheckFlagMode::all_moves);
    return std::any_of(evasions.begin(), evasions.end(),
                       [&checking_move](const MoveMetadata& evasion) {
                           return evasion.is_capture() &&
                               evasion.move.to() == checking_move.move.to();
                       });
}

int shallow_checked_root_fallback_score(
    const GameState& after_check, const MoveMetadata& checking_move,
    const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_check, root_color, 1); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, true, CheckFlagMode::all_moves);
    if (evasions.empty()) {
        return after_check.in_check() ? kMateScore - 1 : 0;
    }

    // The checking move has just been made by the root side, so every
    // evasion belongs to the opponent.  Select the worst root-perspective
    // result, not the best one; using max here lets one attractive evasion
    // hide a stronger defensive answer and makes a shallow check look
    // artificially decisive.
    int worst_evasion_score = kInfinity;
    bool found_evasion = false;
    for (const MoveMetadata& evasion : evasions) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_evasion = after_check;
        if (!after_evasion.make_search_move(evasion)) {
            continue;
        }
        if (const auto terminal = fallback_terminal_or_draw_score(
                after_evasion, root_color, 2); terminal.has_value()) {
            worst_evasion_score = std::min(worst_evasion_score, *terminal);
            found_evasion = true;
            continue;
        }
        int evasion_score = short_fallback_evaluate(
            evaluator, after_evasion, root_color, evaluator_mutex);
        const bool captures_checking_piece = evasion.is_capture() &&
            evasion.move.to() == checking_move.move.to();
        const bool valuable_hanging_checker = captures_checking_piece &&
            (checking_move.moving_piece == PieceType::queen ||
             checking_move.moving_piece == PieceType::rook);
        if (valuable_hanging_checker) {
            // A checking queen/rook that can be taken safely is not a useful
            // emergency candidate. Keep a genuine mate-in-one sacrifice
            // available, but make a plainly hanging checker lose its tie
            // against quiet defensive moves.
            evasion_score = std::min(evasion_score, -3'000);
        }
        if (after_evasion.in_check() && after_evasion.legal_moves().empty()) {
            // The root side is to move after the opponent's evasion.  If
            // that evasion also checks the root and leaves no legal move,
            // the original checking candidate has been refuted by mate.
            evasion_score = -kMateScore + 2;
        }
        worst_evasion_score = std::min(worst_evasion_score, evasion_score);
        found_evasion = true;
    }
    return apply_claimable_draw_option(
        after_check, root_color, found_evasion ? worst_evasion_score : -kInfinity);
}

int forcing_reply_fallback_score(const GameState& after_forcing_reply,
                                 Color root_color, const Evaluator& evaluator,
                                 std::mutex* evaluator_mutex,
                                 PieceType captured_piece,
                                 const FallbackControl* control);

int king_evasion_fallback_score(
    const GameState& after_evasion, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const bool is_capture,
    const FallbackControl* control = nullptr) {
    // This helper is evaluated from Koi's perspective after the opponent has
    // checked. Do not reuse shallow_checked_root_fallback_score here: that
    // helper intentionally penalizes a capturable *root* checker, whereas a
    // capturable opponent checker is favorable to Koi.
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_evasion, root_color, 2); terminal.has_value()) {
        return *terminal;
    }

    const PositionFeatures evasion_features = after_evasion.position_features();
    const std::size_t root_side = root_color == Color::white ? 0U : 1U;
    const int king_exposure_penalty =
        static_cast<int>(evasion_features.king_zone_attacks[root_side]) *
            kEmergencyKingZoneAttackWeight +
        (is_capture ? kEmergencyKingCaptureRisk : 0);
    const int static_score = short_fallback_evaluate(
        evaluator, after_evasion, root_color, evaluator_mutex) - king_exposure_penalty;
    int worst_score = kInfinity;
    bool found_check = false;
    MoveMetadataList opponent_moves;
    after_evasion.legal_moves_with_metadata(
        opponent_moves, true, false, CheckFlagMode::all_moves);
    for (const MoveMetadata& opponent_move : opponent_moves) {
        const bool forcing_reply = opponent_move.gives_check ||
            opponent_move.is_capture() ||
            opponent_move.move.promotion() != Promotion::none;
        if (!forcing_reply) {
            continue;
        }
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_check = after_evasion;
        if (!after_check.make_search_move(opponent_move)) {
            continue;
        }
        if (const auto terminal = fallback_terminal_or_draw_score(
                after_check, root_color, 1); terminal.has_value()) {
            found_check = true;
            worst_score = std::min(worst_score, *terminal);
            continue;
        }
        if (!opponent_move.gives_check) {
            const int forcing_score = forcing_reply_fallback_score(
                after_check, root_color, evaluator, evaluator_mutex,
                opponent_move.captured_piece, control);
            if (forcing_score <= -kInfinity) {
                return -kInfinity;
            }
            found_check = true;
            worst_score = std::min(worst_score, forcing_score);
            continue;
        }
        found_check = true;
        MoveMetadataList evasions;
        after_check.legal_moves_with_metadata(
            evasions, true, true, CheckFlagMode::all_moves);
        if (evasions.empty()) {
            const int check_score = after_check.in_check() ? -kMateScore + 1 : 0;
            worst_score = std::min(worst_score, check_score);
            continue;
        }
        int best_evasion_score = -kInfinity;
        for (const MoveMetadata& evasion : evasions) {
            if (control != nullptr && control->interrupted()) {
                return -kInfinity;
            }
            GameState after_check_evasion = after_check;
            if (!after_check_evasion.make_search_move(evasion)) {
                continue;
            }
            int evasion_score = 0;
            if (const auto terminal = fallback_terminal_or_draw_score(
                    after_check_evasion, root_color, 2); terminal.has_value()) {
                evasion_score = *terminal;
            } else {
                evasion_score = short_fallback_evaluate(
                    evaluator, after_check_evasion, root_color, evaluator_mutex);
            }
            best_evasion_score = std::max(best_evasion_score, evasion_score);
        }
        // A king move that leaves the opponent a check with only one or two
        // legal answers is usually a confinement/continuation trap.  The
        // emergency scanner cannot afford a full mate search here, so charge
        // a bounded pressure penalty for a very narrow evasion set.  Normal
        // completed search is unaffected and still evaluates the position at
        // its ordinary depth.
        const int confinement_penalty = evasions.size() == 1 ? 900 :
            (evasions.size() == 2 ? 250 : 0);
        const int check_score = best_evasion_score - confinement_penalty;
        if (check_score <= -kInfinity) {
            return -kInfinity;
        }
        worst_score = std::min(worst_score, check_score);
    }
    return apply_claimable_draw_option(
        after_evasion, root_color,
        found_check ? worst_score - king_exposure_penalty : static_score);
}

int checked_reply_fallback_score(const GameState& after_check, const Color root_color,
                                 const Evaluator& evaluator,
                                 std::mutex* evaluator_mutex,
                                 const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_check, root_color, 2); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, false, CheckFlagMode::all_moves);
    if (evasions.empty()) {
        return after_check.in_check() ? -kMateScore + 2 : 0;
    }

    int best_evasion_score = -kInfinity;
    for (const MoveMetadata& evasion : evasions) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_evasion = after_check;
        if (!after_evasion.make_search_move(evasion)) {
            continue;
        }

        const auto evasion_terminal = fallback_terminal_or_draw_score(
            after_evasion, root_color, 2);
        if (evasion_terminal.has_value()) {
            best_evasion_score = std::max(best_evasion_score, *evasion_terminal);
            continue;
        }
        int evasion_score = short_fallback_evaluate(
            evaluator, after_evasion, root_color, evaluator_mutex);
        if (after_evasion.in_check()) {
            MoveMetadataList replies;
            after_evasion.legal_moves_with_metadata(
                replies, true, false, CheckFlagMode::all_moves);
            if (replies.empty()) {
                evasion_score = kMateScore - 2;
            }
        } else {
            MoveMetadataList opponent_moves;
            after_evasion.legal_moves_with_metadata(
                opponent_moves, true, false, CheckFlagMode::all_moves);
            for (const MoveMetadata& opponent_move : opponent_moves) {
                if (control != nullptr && control->interrupted()) {
                    return -kInfinity;
                }
                const bool forcing_reply = opponent_move.gives_check ||
                    opponent_move.is_capture() ||
                    opponent_move.move.promotion() != Promotion::none;
                if (!forcing_reply) {
                    continue;
                }
                GameState after_forcing_reply = after_evasion;
                if (!after_forcing_reply.make_search_move(opponent_move)) {
                    continue;
                }
                const auto forcing_terminal = fallback_terminal_or_draw_score(
                    after_forcing_reply, root_color, 2);
                const int forcing_score = forcing_terminal.has_value() ? *forcing_terminal :
                    forcing_reply_fallback_score(
                        after_forcing_reply, root_color, evaluator, evaluator_mutex,
                        opponent_move.captured_piece, control);
                evasion_score = std::min(evasion_score, forcing_score);
            }
        }
        best_evasion_score = std::max(best_evasion_score, evasion_score);
    }
    return apply_claimable_draw_option(after_check, root_color, best_evasion_score);
}

int root_evasion_fallback_score(
    const GameState& after_evasion, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const FallbackControl* control = nullptr) {
    // This helper is entered after the root side has answered an initial
    // check, so the opponent is to move.  `checked_reply_fallback_score()`
    // has the opposite contract: it expects the side to move to be in check.
    // Keep the minimax direction explicit here so a non-king evasion is not
    // accidentally evaluated as though it were itself a checked position.
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_evasion, root_color, 2); terminal.has_value()) {
        return *terminal;
    }

    MoveMetadataList opponent_moves;
    after_evasion.legal_moves_with_metadata(
        opponent_moves, true, false, CheckFlagMode::all_moves);
    if (opponent_moves.empty()) {
        return after_evasion.in_check() ? kMateScore - 2 : 0;
    }

    int score = short_fallback_evaluate(
        evaluator, after_evasion, root_color, evaluator_mutex);
    std::size_t examined_forcing_replies = 0;
    for (const MoveMetadata& opponent_move : opponent_moves) {
        const bool forcing = opponent_move.gives_check ||
            opponent_move.is_capture() ||
            opponent_move.move.promotion() != Promotion::none;
        if (!forcing) {
            continue;
        }
        if (control == nullptr && examined_forcing_replies++ >=
                kMaximumEmergencyRootEvasionReplies) {
            // The no-control form is used after a very short deadline, where
            // completion matters more than exhaustive emergency scanning.
            // Keep that path bounded even if the opponent has a wide forcing
            // move set.
            break;
        }
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_forcing = after_evasion;
        if (!after_forcing.make_search_move(opponent_move)) {
            continue;
        }
        const auto forcing_terminal = fallback_terminal_or_draw_score(
            after_forcing, root_color, 2);
        const int forcing_score = forcing_terminal.has_value() ? *forcing_terminal :
            (opponent_move.gives_check ?
                checked_reply_fallback_score(
                    after_forcing, root_color, evaluator, evaluator_mutex, control) :
                forcing_reply_fallback_score(
                    after_forcing, root_color, evaluator, evaluator_mutex,
                    opponent_move.captured_piece, control));
        if (forcing_score <= -kInfinity) {
            return -kInfinity;
        }
        score = std::min(score, forcing_score);
    }
    return apply_claimable_draw_option(after_evasion, root_color, score);
}

int overdue_check_reply_fallback_score(
    const GameState& after_check, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex) {
    constexpr std::size_t kMaximumEvasions = 2;
    constexpr std::size_t kMaximumForcingReplies = 1;

    if (const auto terminal = fallback_terminal_or_draw_score(
            after_check, root_color, 2); terminal.has_value()) {
        return *terminal;
    }

    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, true, CheckFlagMode::all_moves);
    if (evasions.empty()) {
        return after_check.in_check() ? -kMateScore + 2 : 0;
    }

    // Examine king moves and captures before quiet interpositions so the
    // bounded continuation is more likely to see the critical queen escape
    // or recapture without scanning every evasion at full depth.
    const auto evasion_priority = [](const MoveMetadata& move) {
        if (move.is_capture()) {
            return 0;
        }
        if (move.moving_piece == PieceType::king) {
            return 1;
        }
        if (move.moving_piece == PieceType::queen ||
            move.moving_piece == PieceType::rook) {
            return 2;
        }
        return 3;
    };
    std::stable_sort(evasions.begin(), evasions.end(),
                     [&](const MoveMetadata& left, const MoveMetadata& right) {
                         return evasion_priority(left) < evasion_priority(right);
                     });

    int best_evasion_score = -kInfinity;
    std::size_t evasion_count = 0;
    for (const MoveMetadata& evasion : evasions) {
        if (evasion_count++ >= kMaximumEvasions) {
            break;
        }
        GameState after_evasion = after_check;
        if (!after_evasion.make_search_move(evasion)) {
            continue;
        }
        if (const auto terminal = fallback_terminal_or_draw_score(
                after_evasion, root_color, 2); terminal.has_value()) {
            best_evasion_score = std::max(best_evasion_score, *terminal);
            continue;
        }

        int evasion_score = short_fallback_evaluate(
            evaluator, after_evasion, root_color, evaluator_mutex);
        if (after_evasion.in_check()) {
            if (after_evasion.legal_moves().empty()) {
                // The evasion belongs to the root side.  A checking evasion
                // that mates the opponent is a win for the root, not a loss.
                evasion_score = kMateScore - 2;
            }
            best_evasion_score = std::max(best_evasion_score, evasion_score);
            continue;
        }

        MoveMetadataList replies;
        after_evasion.legal_moves_with_metadata(
            replies, true, true, CheckFlagMode::all_moves);
        std::array<const MoveMetadata*, kMaximumForcingReplies> selected_replies{
            nullptr};
        std::size_t selected_count = 0;
        const auto append_reply = [&](const auto& predicate) {
            if (selected_count >= selected_replies.size()) {
                return;
            }
            const auto selected = std::find_if(
                replies.begin(), replies.end(),
                [&](const MoveMetadata& move) {
                    if (std::find(selected_replies.begin(),
                                  selected_replies.begin() + selected_count,
                                  &move) != selected_replies.begin() + selected_count) {
                        return false;
                    }
                    return predicate(move);
                });
            if (selected != replies.end()) {
                selected_replies[selected_count++] = &*selected;
            }
        };
        append_reply([](const MoveMetadata& move) {
            return move.is_capture() &&
                (move.captured_piece == PieceType::queen ||
                 move.captured_piece == PieceType::rook);
        });

        for (std::size_t reply_index = 0; reply_index < selected_count; ++reply_index) {
            const MoveMetadata& reply = *selected_replies[reply_index];
            GameState after_reply = after_evasion;
            if (!after_reply.make_search_move(reply)) {
                continue;
            }
            const auto reply_terminal = fallback_terminal_or_draw_score(
                after_reply, root_color, 2);
            int reply_score = reply_terminal.has_value() ? *reply_terminal :
                forcing_reply_fallback_score(
                    after_reply, root_color, evaluator, evaluator_mutex,
                    reply.captured_piece, nullptr);
            if (!reply_terminal.has_value() && reply.is_capture() &&
                       (reply.captured_piece == PieceType::queen ||
                        reply.captured_piece == PieceType::rook)) {
                // The overdue continuation is an emergency safety oracle,
                // not a complete search.  Do not let a shallow evaluation
                // after a major-piece capture erase the material loss that
                // the continuation just established.
                reply_score = std::min(
                    reply_score, -piece_value(reply.captured_piece));
            }
            // After the root side has answered the check, it is the
            // opponent's turn.  A forcing reply therefore minimizes the
            // root-perspective score; taking max(static, reply) would let a
            // shallow static evaluation hide a decisive rook/queen capture.
            evasion_score = std::min(evasion_score, reply_score);
        }
        best_evasion_score = std::max(best_evasion_score, evasion_score);
    }
    return apply_claimable_draw_option(after_check, root_color, best_evasion_score);
}

int forcing_reply_fallback_score(const GameState& after_forcing_reply,
                                 const Color root_color, const Evaluator& evaluator,
                                 std::mutex* evaluator_mutex,
                                 const PieceType captured_piece,
                                 const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_forcing_reply, root_color, 2); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList responses;
    after_forcing_reply.legal_moves_with_metadata(
        responses, true, false, CheckFlagMode::all_moves);
    if (responses.empty()) {
        return after_forcing_reply.in_check() ? -kMateScore + 2 : 0;
    }

    int best_score = short_fallback_evaluate(
        evaluator, after_forcing_reply, root_color, evaluator_mutex);
    // A shallow static evaluation can miss the significance of a forcing
    // move that has just taken a major root piece, especially when the root
    // still has checking or material counterplay. Keep this emergency
    // comparison conservative unless a forcing reply or mate compensates for
    // the loss.
    const bool has_forcing_loss_floor = captured_piece == PieceType::queen ||
        captured_piece == PieceType::rook || captured_piece == PieceType::bishop ||
        captured_piece == PieceType::knight;
    const int forcing_loss_floor = captured_piece == PieceType::queen ? -1'200 :
        captured_piece == PieceType::rook ? -650 : -425;
    if (has_forcing_loss_floor) {
        // A pawn capture (and a non-capturing promotion, represented by
        // `none`) still has a real static continuation when the defender's
        // replies are quiet.  Do not turn that ordinary baseline into the
        // interruption sentinel merely because no major/minor material floor
        // applies.
        best_score = std::min(best_score, forcing_loss_floor);
    }
    for (const MoveMetadata& response : responses) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        if (!response.is_capture() && !response.gives_check &&
            response.move.promotion() == Promotion::none) {
            continue;
        }
        GameState after_response = after_forcing_reply;
        if (!after_response.make_search_move(response)) {
            continue;
        }
        const auto response_terminal = fallback_terminal_or_draw_score(
            after_response, root_color, 2);
        int response_score = response_terminal.has_value() ? *response_terminal :
            short_fallback_evaluate(evaluator, after_response, root_color, evaluator_mutex);
        if (!response_terminal.has_value() && response.is_capture() &&
            captured_piece == PieceType::none &&
            (response.captured_piece == PieceType::rook ||
             response.captured_piece == PieceType::queen)) {
            // A checking sacrifice is often answered by capturing the checker,
            // but that capture can itself be refuted by an immediate recapture.
            // The interrupted-root fallback must not treat the material-winning
            // middle position as stable before seeing that one tactical reply.
            MoveMetadataList counter_responses;
            after_response.legal_moves_with_metadata(
                counter_responses, true, false, CheckFlagMode::all_moves);
            for (const MoveMetadata& counter_response : counter_responses) {
                if (control != nullptr && control->interrupted()) {
                    return -kInfinity;
                }
                if (!counter_response.is_capture() ||
                    counter_response.move.to() != response.move.to()) {
                    continue;
                }
                GameState after_counter = after_response;
                if (!after_counter.make_search_move(counter_response)) {
                    continue;
                }
                const auto counter_terminal = fallback_terminal_or_draw_score(
                    after_counter, root_color, 3);
                const int counter_score = counter_terminal.has_value() ? *counter_terminal :
                    short_fallback_evaluate(
                        evaluator, after_counter, root_color, evaluator_mutex);
                response_score = std::min(
                    response_score,
                    counter_score);
            }
        }
        best_score = std::max(best_score, response_score);
    }
    return apply_claimable_draw_option(after_forcing_reply, root_color, best_score);
}

bool root_move_is_broad_quiet_check(
    const GameState& root, const MoveMetadata& metadata) noexcept {
    if (!metadata.gives_check || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none) {
        return false;
    }
    try {
        GameState after_check = root;
        return after_check.make_search_move(metadata) && after_check.in_check() &&
            after_check.legal_moves().size() > kMaximumEmergencyForcingCheckEvasions;
    } catch (...) {
        return false;
    }
}

std::optional<Move> first_safe_short_search_move(
    const GameState& root, const MoveMetadataList& legal_moves) noexcept {
    try {
        for (const MoveMetadata& metadata : legal_moves) {
            if (root_move_is_broad_quiet_check(root, metadata)) {
                continue;
            }
            if (!root_move_exposes_immediate_check(root, metadata)) {
                return metadata.move;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

bool is_safe_equal_non_pawn_exchange(const MoveMetadata& metadata) noexcept;
bool short_quiet_needs_fallback(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept;
bool quiet_move_leaves_safe_capture(
    const MoveMetadata& candidate, const MoveMetadataList& opponent_moves) noexcept;

std::optional<ShortFallbackChoice> scored_safe_checking_capture(
    const GameState& root, const MoveMetadataList& legal_moves,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const bool allow_deep_fallback, const FallbackControl* control) noexcept {
    try {
        for (const MoveMetadata& metadata : legal_moves) {
            if (control != nullptr && control->interrupted()) {
                return std::nullopt;
            }
            if (!metadata.is_capture() || !metadata.see_computed || metadata.see_score < 0 ||
                (!metadata.gives_check && !root.move_gives_check(metadata.move))) {
                continue;
            }

            GameState after_check = root;
            if (!after_check.make_search_move(metadata)) {
                continue;
            }
            // The move itself may end the game.  Preserve an immediate mate,
            // but do not nominate a stalemate or rule-draw move as a tactical
            // replacement for a completed root line.
            if (const auto terminal = fallback_terminal_or_draw_score(
                    after_check, root.side_to_move(), 1); terminal.has_value()) {
                if (*terminal >= kMateThreshold) {
                    return ShortFallbackChoice{metadata.move, *terminal};
                }
                continue;
            }

            const std::vector<Move> evasions = after_check.legal_moves();
            if (evasions.empty()) {
                return ShortFallbackChoice{
                    metadata.move, after_check.in_check() ? kMateScore - 1 : 0};
            }

            const bool deadline_seen = control != nullptr && control->interrupted();
            const bool use_deep_fallback = allow_deep_fallback && !deadline_seen;
            const FallbackControl* score_control = use_deep_fallback ? control : nullptr;
            const int score = use_deep_fallback ?
                checked_root_fallback_score(
                    after_check, root.side_to_move(), evaluator, evaluator_mutex, score_control) :
                shallow_checked_root_fallback_score(
                    after_check, metadata, root.side_to_move(), evaluator, evaluator_mutex,
                    score_control);
            if (score <= -kInfinity && control != nullptr && control->interrupted()) {
                return std::nullopt;
            }
            if (score > -kInfinity) {
                // A move-only safety helper is allowed to nominate this
                // candidate, but it must publish the score obtained from the
                // candidate's own checked continuation.
                return ShortFallbackChoice{metadata.move, score};
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<ShortFallbackChoice> short_search_fallback_move(
    const GameState& root, const MoveMetadataList& legal_moves,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    SearchStats* stats,
    bool allow_deep_checked_fallback,
    const FallbackControl* control,
    bool permit_overdue_shallow,
    bool reject_hanging_major_checks) noexcept {
    if (stats != nullptr) {
        ++stats->short_fallback_invocations;
    }
    const PositionFeatures root_features = root.position_features();
    MoveMetadataList fallback_moves = legal_moves;
    if (root.in_check()) {
        // Checked roots must compare all king evasions before spending the
        // bounded fallback budget on unrelated interpositions. Otherwise a
        // slow first candidate can leave a later, safer king escape unseen.
        std::stable_partition(
            fallback_moves.begin(), fallback_moves.end(),
            [](const MoveMetadata& metadata) { return metadata.moving_piece == PieceType::king; });
    }
    std::optional<Move> best_move;
    int best_score = -kInfinity;
    int best_king_escape_risk = kInfinity;
    bool best_is_king_evasion = false;
    bool best_king_evasion_is_capture = false;
    bool best_is_safe_capture = false;
    bool best_is_safe_forcing_capture = false;
    bool best_is_safe_equal_non_pawn_exchange = false;
    bool best_is_forcing_check = false;
    bool best_is_hanging_major_check = false;
    std::optional<Move> best_quiet_forcing_move;
    int best_quiet_forcing_score = -kInfinity;
    std::optional<Move> broad_check_fallback_move;
    int broad_check_fallback_score = -kInfinity;
    bool broad_check_fallback_is_major = false;
    std::size_t quiet_forcing_researches = 0;
    std::optional<Move> unsafe_best_move;
    int unsafe_best_score = -kInfinity;
    bool unsafe_best_is_quiet = false;
    std::optional<Move> first_unsafe_quiet_move;
    int first_unsafe_quiet_score = -kInfinity;
    std::optional<Move> best_safe_pawn_capture;
    int best_safe_pawn_capture_score = -kInfinity;
    try {
        for (const MoveMetadata& metadata : fallback_moves) {
            const bool deadline_seen = control != nullptr && control->interrupted();
            if (deadline_seen && !permit_overdue_shallow) {
                break;
            }
            if (stats != nullptr) {
                ++stats->short_fallback_candidates;
                if (deadline_seen) {
                    ++stats->short_fallback_overdue_candidates;
                }
            }
            const bool perform_deep_fallback = allow_deep_checked_fallback && !deadline_seen;
            const bool bounded_shallow_fallback = permit_overdue_shallow &&
                !allow_deep_checked_fallback;
            GameState after_move = root;
            if (!after_move.make_search_move(metadata)) {
                continue;
            }

            int score = 0;
            const auto move_terminal = fallback_terminal_or_draw_score(
                after_move, root.side_to_move(), 1);
            const bool move_is_terminal = move_terminal.has_value();
            bool hanging_major_check = false;
            if (move_is_terminal) {
                // A fallback move that reaches a terminal position or a
                // forced rule draw must not be replaced by a static
                // evaluation. Claimable draws remain live and are handled as
                // an optional zero score by the continuation helpers. The
                // helper checks mate before draw so an immediate checkmate
                // remains decisive.
                score = *move_terminal;
            } else if (after_move.in_check()) {
                const std::vector<Move> evasions = after_move.legal_moves();
                if (evasions.empty()) {
                    score = kMateScore - 1;
                } else if (perform_deep_fallback) {
                    score = checked_root_fallback_score(
                        after_move, root.side_to_move(), evaluator, evaluator_mutex, control);
                } else {
                    // A post-deadline shallow pass is intentionally bounded to
                    // the legal evasions of this checking move.  It is still
                // needed to distinguish a useful checking candidate from
                // a hanging checker, so do not make the already-approved
                // overdue shallow pass return -infinity immediately.
                    const FallbackControl* shallow_control =
                        permit_overdue_shallow &&
                            (deadline_seen || bounded_shallow_fallback) ? nullptr : control;
                    score = shallow_checked_root_fallback_score(
                        after_move, metadata, root.side_to_move(), evaluator, evaluator_mutex,
                        shallow_control);
                }
                hanging_major_check = hanging_major_piece_check(after_move, metadata);
                if (reject_hanging_major_checks && hanging_major_check && !metadata.is_capture()) {
                    // The emergency fallback must not rank a capturable
                    // queen/rook check above a quiet move.  The completion
                    // boundary rejects this candidate later, which would
                    // otherwise leave the original shallow move in place.
                    continue;
                }
            } else if (root.in_check()) {
                // A non-checking evasion must be judged against the opponent's
                // immediate forcing replies. Static material alone can prefer
                // a legal-looking king/rook move that leaves a decisive check.
                if (permit_overdue_shallow &&
                    (deadline_seen || bounded_shallow_fallback)) {
                    // Even in the ultra-short checked-root path, use the
                    // bounded reply scanners. Static evaluation alone can
                    // rank a king move into a checking net above the safe
                    // escape; these helpers inspect only forcing replies and
                    // remain independent of the expired time token.
                    score = metadata.moving_piece == PieceType::king ?
                        king_evasion_fallback_score(
                            after_move, root.side_to_move(), evaluator, evaluator_mutex,
                            metadata.is_capture(), nullptr) :
                        root_evasion_fallback_score(
                            after_move, root.side_to_move(), evaluator, evaluator_mutex, nullptr);
                } else if (metadata.moving_piece == PieceType::king) {
                    score = king_evasion_fallback_score(
                        after_move, root.side_to_move(), evaluator, evaluator_mutex,
                        metadata.is_capture(), control);
                } else {
                    score = root_evasion_fallback_score(
                        after_move, root.side_to_move(), evaluator, evaluator_mutex, control);
                }
            } else {
                score = short_fallback_evaluate(
                    evaluator, after_move, root.side_to_move(), evaluator_mutex);
            }

            // A bounded reply scanner can observe the deadline after this
            // root candidate has already been made. Its -kInfinity result is
            // an interruption sentinel, not a real evaluation; never let it
            // become the authoritative score or feed the fallback tie-breaks.
            if (score <= -kInfinity) {
                if (control != nullptr && control->interrupted()) {
                    break;
                }
                continue;
            }

            // A static root score can overvalue a material-winning move that
            // opens an immediate checking reply.  This matters only when the
            // first search iteration is interrupted and this emergency
            // fallback is authoritative, so spend the bounded extra work on
            // checking replies rather than allowing a one-ply horizon trap.
            bool exposes_immediate_check = false;
            std::size_t forcing_reply_count = 0;
            std::size_t check_evasion_count = 0;
            bool consequence_scan_incomplete = false;
            if (!move_is_terminal && !after_move.in_check() && perform_deep_fallback &&
                !(root.in_check() && metadata.moving_piece == PieceType::king)) {
                MoveMetadataList opponent_moves;
                after_move.legal_moves_with_metadata(
                    opponent_moves, true, true, CheckFlagMode::all_moves);
                if (quiet_move_leaves_safe_capture(metadata, opponent_moves)) {
                    // Do not let the one-ply score of a quiet move with a
                    // safe forcing capture survive the emergency scan.
                    continue;
                }
                for (const MoveMetadata& opponent_move : opponent_moves) {
                    if (control != nullptr && control->interrupted()) {
                        consequence_scan_incomplete = true;
                        break;
                    }
                    const bool forcing_reply = opponent_move.gives_check ||
                        opponent_move.is_capture() ||
                        opponent_move.move.promotion() != Promotion::none;
                    if (!forcing_reply) {
                        continue;
                    }
                    ++forcing_reply_count;
                    if (opponent_move.gives_check) {
                        exposes_immediate_check = true;
                    }
                    GameState after_reply = after_move;
                    if (!after_reply.make_search_move(opponent_move)) {
                        continue;
                    }
                    const int forcing_score = opponent_move.gives_check ?
                        checked_reply_fallback_score(
                            after_reply, root.side_to_move(), evaluator, evaluator_mutex, control) :
                        forcing_reply_fallback_score(
                            after_reply, root.side_to_move(), evaluator, evaluator_mutex,
                            opponent_move.captured_piece, control);
                    if (forcing_score <= -kInfinity) {
                        consequence_scan_incomplete = true;
                        break;
                    }
                    score = std::min(score, forcing_score);
                }
                if (consequence_scan_incomplete) {
                    // A deadline can fire after one or more opponent replies
                    // have been scored. Those partial minima are not a
                    // reliable comparison between root moves; keep only the
                    // static score when the consequence scan did not finish.
                    score = short_fallback_evaluate(
                        evaluator, after_move, root.side_to_move(), evaluator_mutex);
                }
            } else if (!move_is_terminal && !after_move.in_check() && permit_overdue_shallow &&
                       (deadline_seen || !allow_deep_checked_fallback)) {
                // Once the deadline has fired, inspect one opponent forcing
                // reply and only our forcing continuations. This catches a
                // poisoned capture or immediate check without starting the
                // recursive reply tree used by the normal emergency scan. A
                // caller that explicitly disables deep fallback uses the same
                // bounded scan even if the deadline has not flipped yet, so
                // timing between the root iteration and correction cannot
                // change the tactical safety decision.
                MoveMetadataList opponent_moves;
                after_move.legal_moves_with_metadata(
                    opponent_moves, true, true, CheckFlagMode::all_moves);
                if (!(root.in_check() && metadata.moving_piece == PieceType::king) &&
                    quiet_move_leaves_safe_capture(metadata, opponent_moves)) {
                    // The overdue path is allowed to be conservative: a
                    // quiet move that immediately leaves a safe forcing
                    // capture cannot be its authoritative choice.
                    continue;
                }
                const auto is_forcing_reply = [](const MoveMetadata& move) {
                    return move.gives_check || move.is_capture() ||
                        move.move.promotion() != Promotion::none;
                };
                // Keep the overdue probe bounded, but do not let an early
                // material capture consume the only non-check slot.  A
                // poisoned move can leave both a valuable recapture and a
                // less relevant major capture in the reply list; inspect two
                // valuable captures/promotions so the emergency comparison
                // can see both tactical consequences.
                std::array<const MoveMetadata*, 6> selected_replies{
                    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
                std::size_t selected_reply_count = 0;
                const auto append_reply = [&](const auto& predicate,
                                              const bool prefer_distinct_source = false) {
                    if (selected_reply_count >= selected_replies.size()) {
                        return;
                    }
                    auto selected = std::find_if(
                        opponent_moves.begin(), opponent_moves.end(),
                        [&](const MoveMetadata& move) {
                            if (std::find(selected_replies.begin(),
                                          selected_replies.begin() + selected_reply_count,
                                          &move) != selected_replies.begin() + selected_reply_count) {
                                return false;
                            }
                            if (prefer_distinct_source && std::any_of(
                                    selected_replies.begin(),
                                    selected_replies.begin() + selected_reply_count,
                                    [&](const MoveMetadata* prior) {
                                        return prior != nullptr &&
                                            prior->gives_check &&
                                            prior->move.from() == move.move.from();
                                    })) {
                                return false;
                            }
                            return predicate(move);
                        });
                    if (selected == opponent_moves.end() && prefer_distinct_source) {
                        // If all checks come from one source, retain the
                        // bounded second probe rather than dropping it.
                        selected = std::find_if(
                            opponent_moves.begin(), opponent_moves.end(),
                            [&](const MoveMetadata& move) {
                                if (std::find(
                                        selected_replies.begin(),
                                        selected_replies.begin() + selected_reply_count,
                                        &move) != selected_replies.begin() + selected_reply_count) {
                                    return false;
                                }
                                return predicate(move);
                            });
                    }
                    if (selected != opponent_moves.end()) {
                        selected_replies[selected_reply_count++] = &*selected;
                    }
                };
                // A material capture can be poisoned by an immediate
                // recapture on the destination square. Probe that concrete
                // consequence before the broader checking/capture probes;
                // otherwise the bounded overdue scan can spend its entire
                // budget on unrelated forcing moves and accept a horizon
                // capture that the opponent simply takes back.
                append_reply([&metadata](const MoveMetadata& move) {
                    return move.is_capture() && move.move.to() == metadata.move.to();
                });
                append_reply([](const MoveMetadata& move) { return move.gives_check; });
                append_reply([](const MoveMetadata& move) { return move.gives_check; }, true);
                append_reply([](const MoveMetadata& move) { return move.gives_check; }, true);
                append_reply([](const MoveMetadata& move) {
                    return (move.is_capture() && move.captured_piece != PieceType::pawn) ||
                        move.move.promotion() != Promotion::none;
                });
                append_reply([](const MoveMetadata& move) {
                    return (move.is_capture() && move.captured_piece != PieceType::pawn) ||
                        move.move.promotion() != Promotion::none;
                });
                if (selected_reply_count == 0) {
                    append_reply(is_forcing_reply);
                }
                for (std::size_t reply_index = 0;
                     reply_index < selected_reply_count; ++reply_index) {
                    const MoveMetadata& opponent_move = *selected_replies[reply_index];
                    ++forcing_reply_count;
                    exposes_immediate_check = exposes_immediate_check ||
                        opponent_move.gives_check;
                    GameState after_reply = after_move;
                    if (!after_reply.make_search_move(opponent_move)) {
                        continue;
                    }
                    const int forcing_score = opponent_move.gives_check ?
                        overdue_check_reply_fallback_score(
                            after_reply, root.side_to_move(), evaluator, evaluator_mutex) :
                        forcing_reply_fallback_score(
                            after_reply, root.side_to_move(), evaluator, evaluator_mutex,
                            opponent_move.captured_piece);
                    score = std::min(score, forcing_score);
                }
            }

            // When no root iteration completes, a raw static score can make a
            // losing capture look attractive because it collects material
            // before the recapture is seen.  Keep the emergency fallback
            // conservative: only an immediate checking capture may override
            // SEE's losing-capture warning at this boundary.  Normal completed
            // search iterations still evaluate these moves through qsearch.
            if (metadata.is_capture() && metadata.see_computed &&
                metadata.see_score < 0 && !metadata.gives_check) {
                continue;
            }

            const bool unsafe_pawn_capture = metadata.is_capture() &&
                metadata.captured_piece == PieceType::pawn &&
                (forcing_reply_count > 1 || exposes_immediate_check) &&
                !metadata.gives_check;
            if (unsafe_pawn_capture) {
                if (metadata.see_computed && metadata.see_score >= 0 &&
                    score > best_safe_pawn_capture_score) {
                    // Keep a positive-SEE pawn capture available as a last
                    // resort. It remains excluded from ordinary emergency
                    // ranking, but if every quiet alternative exposes a
                    // forcing reply, choosing the strongest non-losing
                    // capture is safer than returning the least-bad unsafe
                    // quiet move.
                    best_safe_pawn_capture = metadata.move;
                    best_safe_pawn_capture_score = score;
                }
                // A positive one-ply SEE is not enough to make a pawn grab
                // authoritative in an interrupted root.  If the capture
                // leaves multiple forcing replies, or an immediate check that
                // was discovered after the deadline, let a quieter candidate
                // survive the emergency comparison. Narrow pawn captures
                // with no checking consequence remain eligible.
                continue;
            }

            if (after_move.in_check()) {
                check_evasion_count = after_move.legal_moves().size();
            }
            const std::optional<int> king_escape_risk =
                emergency_king_escape_risk(root, metadata, after_move);
            const bool preserve_king_evasion_tie = root.in_check() &&
                best_is_king_evasion && !king_escape_risk.has_value() &&
                score <= best_score + kEmergencyKingEscapeSafetyMargin;
            const bool prefer_quiet_king_evasion = root.in_check() &&
                best_is_king_evasion && best_king_evasion_is_capture &&
                king_escape_risk.has_value() && !metadata.is_capture() &&
                score >= best_score - kEmergencyQuietForcingMargin;
            const bool preserve_quiet_king_evasion = root.in_check() &&
                best_is_king_evasion && !best_king_evasion_is_capture &&
                !best_is_safe_capture && metadata.is_capture() &&
                score <= best_score + kEmergencyQuietForcingMargin;
            const bool safe_capture = metadata.is_capture() && metadata.see_computed &&
                metadata.see_score >= 0;
            // A non-negative SEE on a pawn capture is not enough evidence to
            // protect it from a quiet alternative in the interrupted-root
            // fallback.  Poisoned pawn captures can still expose a forcing
            // recapture or a tactical counterattack that the shallow SEE
            // exchange does not model.  Reserve the safety tie-break for
            // captures of a non-pawn piece, or pawn captures with a narrow
            // forcing-reply set; the normal search and qsearch continue to
            // evaluate every capture normally.
            const bool safe_material_capture = safe_capture &&
                (metadata.captured_piece != PieceType::pawn || forcing_reply_count <= 1);
            const bool safe_forcing_capture = safe_capture && after_move.in_check();
            // A broad check can still be a horizon trap when the checked side
            // has several useful evasions.  Only narrow checks retain the
            // emergency near-tie priority; ordinary search ordering remains
            // unchanged.
            const bool forcing_check = after_move.in_check() &&
                check_evasion_count <= kMaximumEmergencyForcingCheckEvasions;
            const bool broad_quiet_check = after_move.in_check() &&
                !forcing_check && !metadata.is_capture();
            const bool safer_king_evasion = king_escape_risk.has_value() &&
                best_is_king_evasion && score >= best_score - kEmergencyKingEscapeSafetyMargin &&
                *king_escape_risk < best_king_escape_risk;
            const int quiet_safety_tie_margin = best_is_safe_capture ?
                kEmergencySafeExchangeTieMargin : kEmergencySafeCaptureTieMargin;
            const bool quiet_move_tie = !metadata.is_capture() && !safe_capture &&
                best_is_safe_capture &&
                score <= best_score + quiet_safety_tie_margin;
            const bool quiet_forcing_check_tie = !metadata.is_capture() &&
                !forcing_check && best_is_forcing_check &&
                score <= best_score + kEmergencyQuietForcingMargin;
            const bool safe_capture_tie = safe_forcing_capture &&
                !best_is_safe_capture &&
                score >= best_score - kEmergencySafeCaptureTieMargin;
            const bool safe_capture_safety_tie = safe_capture &&
                safe_material_capture && !safe_forcing_capture && !best_is_safe_capture &&
                score >= best_score - kEmergencySafeExchangeTieMargin;
            const bool forcing_check_tie = forcing_check && !best_is_forcing_check &&
                score >= best_score - kEmergencyQuietForcingMargin;
            const bool preserve_safe_forcing_capture = best_is_safe_forcing_capture &&
                !metadata.is_capture() && metadata.gives_check &&
                score <= best_score + kEmergencyQuietForcingMargin;

            // An equal-value, non-pawn exchange with non-negative SEE has
            // already passed a material safety check and its forcing replies
            // were included above. Do not discard that tactical exchange only
            // because the opponent also has a checking reply; keep the broad
            // immediate-check guard for quiet moves and pawn captures.
            const bool safe_equal_non_pawn_exchange =
                is_safe_equal_non_pawn_exchange(metadata);
            const bool safe_non_pawn_capture = safe_capture &&
                (metadata.captured_piece == PieceType::queen ||
                 (metadata.moving_piece == PieceType::pawn &&
                  (metadata.captured_piece == PieceType::bishop ||
                   metadata.captured_piece == PieceType::knight)));
            const bool safe_equal_exchange_tie = safe_equal_non_pawn_exchange &&
                !best_is_safe_capture && !best_is_safe_forcing_capture &&
                !best_is_safe_equal_non_pawn_exchange &&
                score >= best_score - kEmergencySafeExchangeTieMargin;

            if (broad_quiet_check) {
                // Keep a broad, non-hanging major-piece check as a deferred
                // forcing candidate.  Minor-piece and pawn checks with a
                // broad evasion set are too easy to overvalue at this shallow
                // horizon, while a queen/rook check can still be the critical
                // forcing resource an interrupted root needs.
                const bool major_check = metadata.moving_piece == PieceType::queen ||
                    metadata.moving_piece == PieceType::rook;
                if (major_check && !hanging_major_check &&
                    (!broad_check_fallback_move.has_value() ||
                    score > broad_check_fallback_score)) {
                    broad_check_fallback_move = metadata.move;
                    broad_check_fallback_score = score;
                    broad_check_fallback_is_major = true;
                }
                continue;
            }
            if (exposes_immediate_check && !root.in_check() &&
                !safe_equal_non_pawn_exchange && !safe_non_pawn_capture) {
                const int unsafe_score = score -
                    (metadata.moving_piece == PieceType::pawn && !metadata.is_capture() ?
                         kEmergencyUnsafePawnQuietPenalty : 0);
                if (!first_unsafe_quiet_move.has_value() && !metadata.is_capture() &&
                    metadata.move.promotion() == Promotion::none) {
                    first_unsafe_quiet_move = metadata.move;
                    first_unsafe_quiet_score = unsafe_score;
                }
                if (!unsafe_best_move.has_value() || unsafe_score > unsafe_best_score) {
                    unsafe_best_score = unsafe_score;
                    unsafe_best_move = metadata.move;
                    unsafe_best_is_quiet = !metadata.is_capture() &&
                        metadata.move.promotion() == Promotion::none;
                }
                continue;
            }

            const bool quiet_forcing = perform_deep_fallback && !root.in_check() &&
                !metadata.is_capture() &&
                !metadata.gives_check && metadata.move.promotion() == Promotion::none &&
                quiet_move_is_forcing(root_features, after_move.position_features(), metadata);
            if (quiet_forcing &&
                quiet_forcing_researches < kMaximumEmergencyQuietForcingResearches) {
                ++quiet_forcing_researches;
                if (stats != nullptr) {
                    // The emergency path performs the same bounded quiet-forcing
                    // consequence extension as the normal root search, but may
                    // run before a complete negamax iteration fits in the clock.
                    ++stats->quiet_forcing_extensions;
                }
                const int consequence_score = quiet_forcing_fallback_score(
                    after_move, root.side_to_move(), evaluator, evaluator_mutex, control);
                if (consequence_score > -kInfinity) {
                    score = consequence_score;
                }
            }
            if (quiet_forcing &&
                (!best_quiet_forcing_move.has_value() || score > best_quiet_forcing_score)) {
                best_quiet_forcing_score = score;
                best_quiet_forcing_move = metadata.move;
            }
            if ((!best_move.has_value() || score > best_score || safer_king_evasion ||
                 safe_capture_tie || safe_capture_safety_tie || safe_equal_exchange_tie ||
                 forcing_check_tie || prefer_quiet_king_evasion) &&
                (!quiet_move_tie || prefer_quiet_king_evasion) &&
                !quiet_forcing_check_tie &&
                !preserve_safe_forcing_capture &&
                !preserve_king_evasion_tie &&
                !preserve_quiet_king_evasion) {
                best_score = score;
                best_move = metadata.move;
                best_is_king_evasion = king_escape_risk.has_value();
                best_king_evasion_is_capture = best_is_king_evasion && metadata.is_capture();
                best_king_escape_risk = king_escape_risk.value_or(kInfinity);
                best_is_safe_capture = safe_material_capture;
                best_is_safe_forcing_capture = safe_forcing_capture;
                best_is_safe_equal_non_pawn_exchange = safe_equal_non_pawn_exchange;
                best_is_forcing_check = forcing_check;
                best_is_hanging_major_check = hanging_major_check;
            }
        }
    } catch (...) {
        return std::nullopt;
    }
    if (best_quiet_forcing_move.has_value() &&
        !best_is_safe_capture &&
        (!best_is_safe_equal_non_pawn_exchange ||
         best_quiet_forcing_score > best_score + kEmergencySafeExchangeTieMargin) &&
        (!best_is_forcing_check ||
         best_quiet_forcing_score > best_score + kEmergencyQuietForcingMargin) &&
         (!best_move.has_value() ||
          best_quiet_forcing_score >= best_score - kEmergencyQuietForcingMargin)) {
        return ShortFallbackChoice{*best_quiet_forcing_move, best_quiet_forcing_score};
    }
    const bool broad_check_can_replace_competitive_move =
        broad_check_fallback_move.has_value() && broad_check_fallback_is_major &&
        best_move.has_value() &&
        !best_is_safe_capture && !best_is_safe_forcing_capture &&
        !best_is_safe_equal_non_pawn_exchange &&
        broad_check_fallback_score >= best_score - kEmergencyBroadCheckReplacementMargin;
    if (broad_check_can_replace_competitive_move ||
        (broad_check_fallback_move.has_value() && broad_check_fallback_is_major &&
         !best_move.has_value() &&
         (!unsafe_best_move.has_value() ||
         broad_check_fallback_score >= unsafe_best_score -
              kEmergencyBroadCheckReplacementMargin))) {
        return ShortFallbackChoice{*broad_check_fallback_move, broad_check_fallback_score};
    }
    if (!best_move.has_value() && best_safe_pawn_capture.has_value() &&
        first_unsafe_quiet_move.has_value()) {
        // If every positive-SEE pawn capture was quarantined and all useful
        // quiet alternatives expose immediate checks, retain stable move
        // ordering instead of promoting the least-bad unsafe quiet score.
        return ShortFallbackChoice{*first_unsafe_quiet_move, first_unsafe_quiet_score};
    }
    if (unsafe_best_move.has_value() &&
        (unsafe_best_is_quiet || best_is_hanging_major_check) &&
        (!best_move.has_value() ||
         unsafe_best_score >= best_score + kEmergencyQuietForcingMargin)) {
        return ShortFallbackChoice{*unsafe_best_move, unsafe_best_score};
    }
    if (broad_check_fallback_move.has_value() && broad_check_fallback_is_major &&
        !best_move.has_value()) {
        return ShortFallbackChoice{*broad_check_fallback_move, broad_check_fallback_score};
    }
    if (best_move.has_value()) {
        return ShortFallbackChoice{*best_move, best_score};
    }
    if (unsafe_best_move.has_value()) {
        return ShortFallbackChoice{*unsafe_best_move, unsafe_best_score};
    }
    return std::nullopt;
}

bool short_fallback_should_replace_completed_root(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& current_move, const Move fallback_move) noexcept {
    try {
        const auto metadata_for = [&legal_moves](const Move move) {
            return std::find_if(
                legal_moves.begin(), legal_moves.end(),
                [move](const MoveMetadata& metadata) { return metadata.move == move; });
        };
        const auto fallback_metadata = metadata_for(fallback_move);
        if (fallback_metadata == legal_moves.end()) {
            return false;
        }
        if (fallback_metadata->gives_check && !fallback_metadata->is_capture() &&
            (fallback_metadata->moving_piece == PieceType::queen ||
             fallback_metadata->moving_piece == PieceType::rook)) {
            GameState after_fallback = root;
            if (after_fallback.make_search_move(*fallback_metadata) &&
                hanging_major_piece_check(after_fallback, *fallback_metadata)) {
                // Do not let a shallow emergency correction overturn a
                // completed root iteration with a capturable major-piece
                // check. The ordinary search/qsearch must establish such a
                // sacrifice as sound before it becomes authoritative.
                return false;
            }
        }
        const bool fallback_forcing = fallback_metadata->is_capture() ||
            fallback_metadata->gives_check ||
            fallback_metadata->move.promotion() != Promotion::none;
        if (!current_move.has_value()) {
            return true;
        }

        const auto current_metadata = metadata_for(*current_move);
        if (current_metadata == legal_moves.end()) {
            return false;
        }
        if (fallback_forcing) {
            if (current_metadata->gives_check && !current_metadata->is_capture() &&
                current_metadata->move.promotion() == Promotion::none) {
                GameState after_current = root;
                if (after_current.make_search_move(*current_metadata)) {
                    const std::size_t evasion_count = after_current.legal_moves().size();
                    if (evasion_count == 0) {
                        // Do not replace a completed mate with any shallow
                        // alternative, even when that alternative captures.
                        return false;
                    }
                    if (fallback_metadata->is_capture()) {
                        // A completed non-mating checking move that can be
                        // superseded by a checking capture has a concrete
                        // material/tactical alternative worth keeping in the
                        // bounded correction.
                        return true;
                    }
                    if (!hanging_major_piece_check(after_current, *current_metadata)) {
                        // A completed non-hanging check is already a forcing
                        // result; another shallow non-capturing check alone is
                        // not enough evidence to replace it.
                        return false;
                    }
                }
            }
            return true;
        }
        if (root.in_check() &&
            current_metadata->moving_piece == PieceType::king &&
            fallback_metadata->moving_piece == PieceType::king) {
            // A depth-one checked-root result can choose one legal king
            // escape before the bounded forcing-reply scanner sees the
            // difference between two quiet evasions.  Let the scanner's
            // complete king-evasion comparison replace that result; unlike
            // ordinary quiet moves, both candidates are legal responses to
            // the same check and the fallback has already inspected their
            // immediate forcing consequences.
            return fallback_move != *current_move;
        }
        if (current_metadata->is_capture()) {
            const bool current_exposes = root_move_exposes_immediate_check(
                root, *current_metadata);
            if (current_metadata->captured_piece == PieceType::pawn && current_exposes) {
                // A completed depth-one capture is normally protected from a
                // quiet emergency replacement.  A pawn grab that leaves an
                // immediate checking reply is the exception: the bounded
                // fallback has already scored those replies, so permit only
                // a quiet, non-promoting alternative.  Several moves in a
                // tactical root can expose the same checking resource; a
                // binary exposure test must not reject every alternative.
                return !fallback_metadata->is_capture() &&
                    !fallback_metadata->gives_check &&
                    fallback_metadata->move.promotion() == Promotion::none;
            }
            return true;
        }
        const bool fallback_is_king_escape = root.in_check() &&
            fallback_metadata->moving_piece == PieceType::king;
        return fallback_is_king_escape &&
            root_move_exposes_immediate_check(root, *current_metadata);
    } catch (...) {
        return false;
    }
}

bool short_capture_needs_fallback(const MoveMetadataList& legal_moves,
                                  const std::optional<Move>& candidate) noexcept {
    if (!candidate.has_value()) {
        return false;
    }
    const auto metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    return metadata != legal_moves.end() && metadata->is_capture() &&
        !metadata->gives_check;
}

bool short_check_needs_fallback(const MoveMetadataList& legal_moves,
                                const std::optional<Move>& candidate) noexcept {
    if (!candidate.has_value()) {
        return false;
    }
    const auto metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    if (metadata == legal_moves.end() || metadata->is_capture() ||
        !metadata->gives_check || metadata->move.promotion() != Promotion::none) {
        return false;
    }
    // A non-capturing queen/rook check can be attractive at depth one while
    // the checked side still has a quiet escape and the checking piece has no
    // immediate material consequence. Reuse the bounded short fallback for
    // this narrow class; minor-piece and pawn checks remain on the ordinary
    // completed-iteration path.
    return metadata->moving_piece == PieceType::queen ||
        metadata->moving_piece == PieceType::rook;
}

bool is_safe_short_capture(const MoveMetadata& metadata) noexcept {
    return metadata.is_capture() && metadata.see_computed && metadata.see_score >= 0;
}

bool is_safe_material_capture(const MoveMetadata& metadata) noexcept {
    return is_safe_short_capture(metadata) &&
        metadata.captured_piece != PieceType::pawn;
}

bool is_safe_equal_non_pawn_exchange(const MoveMetadata& metadata) noexcept {
    return is_safe_short_capture(metadata) && metadata.moving_piece != PieceType::pawn &&
        metadata.captured_piece == metadata.moving_piece &&
        metadata.captured_piece != PieceType::none;
}

bool short_quiet_needs_fallback(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept {
    if (!candidate.has_value()) {
        return false;
    }

    const auto candidate_metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    if (candidate_metadata == legal_moves.end() || candidate_metadata->is_capture() ||
        candidate_metadata->gives_check ||
        candidate_metadata->move.promotion() != Promotion::none) {
        return false;
    }

    return std::any_of(legal_moves.begin(), legal_moves.end(),
                       [candidate, &root](const MoveMetadata& move) {
                           return move.move != *candidate &&
                               (is_safe_material_capture(move) ||
                                (move.is_capture() &&
                                 (move.gives_check || root.move_gives_check(move.move))));
                       });
}

bool short_quiet_exposes_immediate_check(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept {
    if (!candidate.has_value()) {
        return false;
    }
    const auto metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    if (metadata == legal_moves.end() || metadata->is_capture() ||
        metadata->gives_check || metadata->move.promotion() != Promotion::none) {
        return false;
    }
    return root_move_exposes_immediate_check(root, *metadata);
}

bool short_checked_root_king_evasion_needs_fallback(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept {
    if (!root.in_check() || !candidate.has_value()) {
        return false;
    }
    const auto metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    return metadata != legal_moves.end() && metadata->moving_piece == PieceType::king;
}

bool quiet_move_leaves_safe_capture(
    const MoveMetadata& candidate, const MoveMetadataList& opponent_moves) noexcept {
    if (candidate.is_capture() || candidate.gives_check ||
        candidate.move.promotion() != Promotion::none) {
        return false;
    }
    return std::any_of(opponent_moves.begin(), opponent_moves.end(),
                       [&candidate](const MoveMetadata& opponent_move) {
                           if (!opponent_move.is_capture() || !opponent_move.see_computed ||
                               opponent_move.see_score < 0) {
                               return false;
                           }
                           return opponent_move.move.to() == candidate.move.to() ||
                               opponent_move.captured_piece != PieceType::pawn;
                       });
}

// Root workers consume a shared atomic index.  At a short time limit the
// order of that queue is therefore part of playing strength: moves that were
// strong at the preceding completed iteration should be handed to workers
// before previously unseen tail moves.  Keep the identity separate from the
// current vector position so reordering does not change deterministic ties.

} // namespace koi::detail
